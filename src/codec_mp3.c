/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_MP3_ENCODE
#include <lame/lame.h>
#endif

#ifdef HAVE_MP3_USE_MPG123
#include <mpg123.h>
#endif

/*
 * MP3 codec (push / streaming, LEB128-framed)
 *
 * Encoder: LAME output is emitted as LEB128 audio frames straight to the sink.
 * PCM is processed in fixed-size blocks into a fixed scratch buffer - no
 * growable/output buffering.
 *
 * Decoder: mpg123's feed mode buffers input internally and yields PCM as frames
 * complete, so the LEB128 parser can feed it audio bytes in any chunking (down
 * to one byte) with no reassembly. Without mpg123 the decoder is a passthrough
 * demux that forwards raw MP3 frame bytes on the audio stream.
 */

#ifdef HAVE_MP3_ENCODE
/* One LAME call per block; worst-case output is ceil(1.25*n)+7200 (lame.h). */
#define MP3_ENC_BLOCK 8192
#define MP3_ENC_BUFCAP (((MP3_ENC_BLOCK * 5 + 3) / 4) + 7200 + 1024)

static const char *lame_encode_error_string(int lame_ret) {
	switch(lame_ret) {
	case -1:
		return "mp3buf was too small";
	case -2:
		return "malloc problem";
	case -3:
		return "lame_init_params was not called";
	case -4:
		return "psycho acoustic problems";
	default:
		return lame_ret < 0 ? "unknown LAME error" : NULL;
	}
}
#endif

#ifdef HAVE_MP3_USE_MPG123
static const char *mpg123_error_string(mpg123_handle *mh) {
	const char *err = mpg123_plain_strerror(mpg123_errcode(mh));
	return err ? err : "unknown mpg123 error";
}
#endif

#ifdef HAVE_MP3_ENCODE
struct mp3_encoder_data {
	lame_global_flags *gfp;
	uint8_t mp3_buffer[MP3_ENC_BUFCAP];
};

static const struct mux_param_desc mp3_encoder_params[] = {
	{.name = "bitrate",
	 .description = "Bitrate in kbps (CBR mode)",
	 .type = MUX_PARAM_TYPE_INT,
	 .range.i = {.min = 32, .max = 320, .def = 128}},
	{.name = "quality",
	 .description = "Quality (0=best, 9=worst)",
	 .type = MUX_PARAM_TYPE_INT,
	 .range.i = {.min = 0, .max = 9, .def = 5}},
	{.name = "vbr",
	 .description = "Enable VBR mode",
	 .type = MUX_PARAM_TYPE_BOOL,
	 .range.b = {.def = 0}}
};

static const struct mux_param *
find_param(const struct mux_param *params, int num_params, const char *name) {
	int i;
	for(i = 0; i < num_params; i++)
		if(strcmp(params[i].name, name) == 0)
			return &params[i];
	return NULL;
}

static int mp3_encoder_init(
	struct mux_encoder *enc,
	int sample_rate,
	int num_channels,
	const struct mux_param *params,
	int num_params
) {
	struct mp3_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 128, quality = 5, vbr = 0;

	data = calloc(1, sizeof(*data));
	if(!data) {
		mux_encoder_set_error(
			enc,
			MUX_ERROR_NOMEM,
			"Failed to allocate MP3 encoder data",
			NULL,
			0,
			NULL
		);
		return MUX_ERROR_NOMEM;
	}

	data->gfp = lame_init();
	if(!data->gfp) {
		mux_encoder_set_error(
			enc,
			MUX_ERROR_INIT,
			"Failed to initialize LAME encoder",
			"libmp3lame",
			0,
			"lame_init() failed"
		);
		free(data);
		return MUX_ERROR_INIT;
	}

	param = find_param(params, num_params, "bitrate");
	if(param)
		bitrate = param->value.i;
	param = find_param(params, num_params, "quality");
	if(param)
		quality = param->value.i;
	param = find_param(params, num_params, "vbr");
	if(param)
		vbr = param->value.b;

	lame_set_num_channels(data->gfp, num_channels);
	lame_set_in_samplerate(data->gfp, sample_rate);
	lame_set_quality(data->gfp, quality);
	if(vbr)
		lame_set_VBR(data->gfp, vbr_default);
	else
		lame_set_brate(data->gfp, bitrate);

	if(lame_init_params(data->gfp) < 0) {
		mux_encoder_set_error(
			enc,
			MUX_ERROR_INIT,
			"Failed to initialize LAME parameters",
			"libmp3lame",
			0,
			"lame_init_params() failed"
		);
		lame_close(data->gfp);
		free(data);
		return MUX_ERROR_INIT;
	}

	enc->codec_data = data;
	return MUX_OK;
}

static void mp3_encoder_deinit(struct mux_encoder *enc) {
	struct mp3_encoder_data *data;

	if(!enc || !enc->codec_data)
		return;
	data = enc->codec_data;
	if(data->gfp)
		lame_close(data->gfp);
	free(data);
	enc->codec_data = NULL;
}

static int mp3_emit_lame(struct mux_encoder *enc, int mp3_bytes) {
	struct mp3_encoder_data *data = enc->codec_data;

	if(mp3_bytes < 0) {
		mux_encoder_set_error(
			enc,
			MUX_ERROR_ENCODE,
			"LAME encoding failed",
			"libmp3lame",
			mp3_bytes,
			lame_encode_error_string(mp3_bytes)
		);
		return MUX_ERROR_ENCODE;
	}
	if(mp3_bytes == 0)
		return MUX_OK;
	return mux_leb128_emit_frame(
		data->mp3_buffer,
		mp3_bytes,
		MUX_STREAM_AUDIO,
		enc->num_streams,
		enc->sink,
		enc->sink_user
	);
}

static int
mp3_encoder_encode(struct mux_encoder *enc, const void *input, size_t input_size, int stream_type) {
	struct mp3_encoder_data *data;
	const int16_t *pcm;
	size_t total, off;
	int nch, ret;

	if(!enc || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if(!data)
		return MUX_ERROR_INVAL;
	if(input_size == 0)
		return MUX_OK;

	if(stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_leb128_emit_frame(
			input,
			input_size,
			stream_type,
			enc->num_streams,
			enc->sink,
			enc->sink_user
		);

	pcm = input;
	nch = enc->num_channels;
	total = input_size / sizeof(int16_t) / nch;

	for(off = 0; off < total;) {
		size_t blk = total - off;
		int mp3_bytes;

		if(blk > MP3_ENC_BLOCK)
			blk = MP3_ENC_BLOCK;

		if(nch == 1)
			mp3_bytes = lame_encode_buffer(
				data->gfp,
				pcm + off,
				NULL,
				blk,
				data->mp3_buffer,
				sizeof(data->mp3_buffer)
			);
		else
			mp3_bytes = lame_encode_buffer_interleaved(
				data->gfp,
				(short *)(pcm + off * nch),
				blk,
				data->mp3_buffer,
				sizeof(data->mp3_buffer)
			);

		ret = mp3_emit_lame(enc, mp3_bytes);
		if(ret)
			return ret;

		off += blk;
	}

	return MUX_OK;
}

static int mp3_encoder_finalize(struct mux_encoder *enc) {
	struct mp3_encoder_data *data;
	int mp3_bytes;

	if(!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if(!data)
		return MUX_ERROR_INVAL;

	mp3_bytes = lame_encode_flush(data->gfp, data->mp3_buffer, sizeof(data->mp3_buffer));
	return mp3_emit_lame(enc, mp3_bytes);
}
#endif /* HAVE_MP3_ENCODE */

#ifdef HAVE_MP3_DECODE
struct mp3_decoder_data {
#ifdef HAVE_MP3_USE_MPG123
	mpg123_handle *mh;
#endif
	struct mux_leb128_parser parser;
};

#ifdef HAVE_MP3_USE_MPG123
/* Drain all currently-decodable PCM from mpg123 to the emit callback. */
static int mp3_drain(struct mux_decoder *dec, mpg123_handle *mh) {
	for(;;) {
		unsigned char pcm_buf[16384];
		size_t pcm_bytes = 0;
		int ret = mpg123_read(mh, pcm_buf, sizeof(pcm_buf), &pcm_bytes);

		if(pcm_bytes > 0) {
			int r = mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm_buf, pcm_bytes);
			if(r)
				return r;
		}

		if(ret == MPG123_OK)
			continue;
		if(ret == MPG123_NEW_FORMAT) {
			long rate;
			int channels, encoding;
			mpg123_getformat(mh, &rate, &channels, &encoding);
			continue;
		}
		if(ret == MPG123_NEED_MORE || ret == MPG123_DONE)
			return MUX_OK;

		mux_decoder_set_error(
			dec,
			MUX_ERROR_DECODE,
			"mpg123_read failed",
			"mpg123",
			ret,
			mpg123_error_string(mh)
		);
		return MUX_ERROR_DECODE;
	}
}
#endif

/* Parser emit shim: audio -> mpg123 (or passthrough); side -> forward. */
static int mp3_parser_emit(void *user, int stream_type, const void *chunk, size_t size) {
	struct mux_decoder *dec = user;
	struct mp3_decoder_data *data = dec->codec_data;

	if(stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_decoder_emit(dec, stream_type, chunk, size);

#ifdef HAVE_MP3_USE_MPG123
	if(size > 0) {
		int ret = mpg123_feed(data->mh, chunk, size);
		if(ret != MPG123_OK && ret != MPG123_NEED_MORE) {
			mux_decoder_set_error(
				dec,
				MUX_ERROR_DECODE,
				"mpg123_feed failed",
				"mpg123",
				ret,
				mpg123_error_string(data->mh)
			);
			return MUX_ERROR_DECODE;
		}
	}
	return mp3_drain(dec, data->mh);
#else
	/* Passthrough: forward raw MP3 frame bytes on the audio stream. */
	(void)data;
	return mux_decoder_emit(dec, MUX_STREAM_AUDIO, chunk, size);
#endif
}

static int
mp3_decoder_init(struct mux_decoder *dec, const struct mux_param *params, int num_params) {
	struct mp3_decoder_data *data;
#ifdef HAVE_MP3_USE_MPG123
	int err;
	static int mpg123_inited = 0;
	if(!mpg123_inited) {
		if(mpg123_init() != MPG123_OK) {
			mux_decoder_set_error(
				dec,
				MUX_ERROR_INIT,
				"Failed to initialize mpg123 library",
				"mpg123",
				0,
				"mpg123_init() failed"
			);
			return MUX_ERROR_INIT;
		}
		mpg123_inited = 1;
	}
#endif
	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if(!data) {
		mux_decoder_set_error(
			dec,
			MUX_ERROR_NOMEM,
			"Failed to allocate MP3 decoder data",
			NULL,
			0,
			NULL
		);
		return MUX_ERROR_NOMEM;
	}
	mux_leb128_parser_init(&data->parser);

#ifdef HAVE_MP3_USE_MPG123
	data->mh = mpg123_new(NULL, &err);
	if(!data->mh) {
		mux_decoder_set_error(
			dec,
			MUX_ERROR_INIT,
			"Failed to create mpg123 handle",
			"mpg123",
			err,
			mpg123_plain_strerror(err)
		);
		free(data);
		return MUX_ERROR_INIT;
	}
	if(mpg123_open_feed(data->mh) != MPG123_OK) {
		mux_decoder_set_error(
			dec,
			MUX_ERROR_INIT,
			"Failed to open mpg123 feed mode",
			"mpg123",
			0,
			mpg123_error_string(data->mh)
		);
		mpg123_delete(data->mh);
		free(data);
		return MUX_ERROR_INIT;
	}
#endif

	dec->codec_data = data;
	return MUX_OK;
}

static void mp3_decoder_deinit(struct mux_decoder *dec) {
	struct mp3_decoder_data *data;

	if(!dec || !dec->codec_data)
		return;
	data = dec->codec_data;
#ifdef HAVE_MP3_USE_MPG123
	if(data->mh)
		mpg123_delete(data->mh);
#endif
	free(data);
	dec->codec_data = NULL;
}

static int mp3_decoder_decode(struct mux_decoder *dec, const void *input, size_t input_size) {
	struct mp3_decoder_data *data;

	if(!dec || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = dec->codec_data;
	if(!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(
		&data->parser,
		input,
		input_size,
		dec->num_streams,
		mp3_parser_emit,
		dec
	);
}

static int mp3_decoder_finalize(struct mux_decoder *dec) {
#ifdef HAVE_MP3_USE_MPG123
	struct mp3_decoder_data *data;
	int ret;

	if(!dec)
		return MUX_ERROR_INVAL;
	data = dec->codec_data;
	if(!data || !data->mh)
		return MUX_ERROR_INVAL;

	ret = mpg123_feed(data->mh, NULL, 0);
	if(ret != MPG123_OK && ret != MPG123_NEED_MORE) {
		mux_decoder_set_error(
			dec,
			MUX_ERROR_DECODE,
			"mpg123_feed(NULL) failed",
			"mpg123",
			ret,
			mpg123_error_string(data->mh)
		);
		return MUX_ERROR_DECODE;
	}
	return mp3_drain(dec, data->mh);
#else
	(void)dec;
	return MUX_OK;
#endif
}
#endif /* HAVE_MP3_DECODE */

static const int mp3_sample_rates[] =
	{8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

const struct mux_codec_ops mux_codec_mp3_ops = {
#ifdef HAVE_MP3_ENCODE
	.encoder_init = mp3_encoder_init,
	.encoder_deinit = mp3_encoder_deinit,
	.encoder_encode = mp3_encoder_encode,
	.encoder_finalize = mp3_encoder_finalize,
	.encoder_params = mp3_encoder_params,
	.encoder_param_count = sizeof(mp3_encoder_params) / sizeof(mp3_encoder_params[0]),
#else
	.encoder_init = NULL,
	.encoder_deinit = NULL,
	.encoder_encode = NULL,
	.encoder_finalize = NULL,
	.encoder_params = NULL,
	.encoder_param_count = 0,
#endif

#ifdef HAVE_MP3_DECODE
	.decoder_init = mp3_decoder_init,
	.decoder_deinit = mp3_decoder_deinit,
	.decoder_decode = mp3_decoder_decode,
	.decoder_finalize = mp3_decoder_finalize,
#else
	.decoder_init = NULL,
	.decoder_deinit = NULL,
	.decoder_decode = NULL,
	.decoder_finalize = NULL,
#endif

	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = mp3_sample_rates,
	.sample_rate_count = sizeof(mp3_sample_rates) / sizeof(mp3_sample_rates[0]),
	.sample_rate_is_range = 0
};
