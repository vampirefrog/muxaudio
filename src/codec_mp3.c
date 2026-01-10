/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef HAVE_MP3_ENCODE
#include <lame/lame.h>
#endif

#ifdef HAVE_MP3_DECODE
#include <mpg123.h>
#endif

#ifdef HAVE_MP3_ENCODE
/*
 * LAME error code translation
 */
static const char *lame_encode_error_string(int lame_ret)
{
	switch (lame_ret) {
	case -1:
		return "mp3buf was too small";
	case -2:
		return "malloc problem";
	case -3:
		return "lame_init_params was not called";
	case -4:
		return "psycho acoustic problems";
	default:
		if (lame_ret < 0)
			return "unknown LAME error";
		return NULL;
	}
}
#endif

#ifdef HAVE_MP3_DECODE
static const char *mpg123_error_string(mpg123_handle *mh)
{
	const char *err = mpg123_plain_strerror(mpg123_errcode(mh));
	return err ? err : "unknown mpg123 error";
}
#endif

#ifdef HAVE_MP3_ENCODE
/*
 * MP3 encoder state
 */
struct mp3_encoder_data {
	lame_global_flags *gfp;
	uint8_t mp3_buffer[8192];  /* Buffer for encoded MP3 frames */
};
#endif

#ifdef HAVE_MP3_DECODE
/*
 * MP3 decoder state
 */
struct mp3_decoder_data {
	mpg123_handle *mh;  /* mpg123 decoder handle */
	struct mux_buffer input_buf;  /* Buffer for muxed input */
};
#endif

#ifdef HAVE_MP3_ENCODE
/*
 * MP3 encoder parameters
 */
static const struct mux_param_desc mp3_encoder_params[] = {
	{
		.name = "bitrate",
		.description = "Bitrate in kbps (CBR mode)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 32, .max = 320, .def = 128 }
	},
	{
		.name = "quality",
		.description = "Quality (0=best, 9=worst)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 0, .max = 9, .def = 5 }
	},
	{
		.name = "vbr",
		.description = "Enable VBR mode",
		.type = MUX_PARAM_TYPE_BOOL,
		.range.b = { .def = 0 }
	}
};
#endif

/*
 * Helper to find parameter value by name
 */
static const struct mux_param *find_param(const struct mux_param *params,
					  int num_params,
					  const char *name)
{
	int i;

	for (i = 0; i < num_params; i++) {
		if (strcmp(params[i].name, name) == 0)
			return &params[i];
	}
	return NULL;
}

#ifdef HAVE_MP3_ENCODE
/*
 * MP3 encoder initialization
 */
static int mp3_encoder_init(struct mux_encoder *enc,
			    int sample_rate,
			    int num_channels,
			    const struct mux_param *params,
			    int num_params)
{
	struct mp3_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 128;
	int quality = 5;
	int vbr = 0;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate MP3 encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	/* Initialize LAME */
	data->gfp = lame_init();
	if (!data->gfp) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize LAME encoder",
				      "libmp3lame", 0, "lame_init() failed");
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Get parameters */
	param = find_param(params, num_params, "bitrate");
	if (param)
		bitrate = param->value.i;

	param = find_param(params, num_params, "quality");
	if (param)
		quality = param->value.i;

	param = find_param(params, num_params, "vbr");
	if (param)
		vbr = param->value.b;

	/* Configure LAME */
	lame_set_num_channels(data->gfp, num_channels);
	lame_set_in_samplerate(data->gfp, sample_rate);
	lame_set_quality(data->gfp, quality);

	if (vbr) {
		lame_set_VBR(data->gfp, vbr_default);
	} else {
		lame_set_brate(data->gfp, bitrate);
	}

	if (lame_init_params(data->gfp) < 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize LAME parameters",
				      "libmp3lame", 0, "lame_init_params() failed");
		lame_close(data->gfp);
		free(data);
		return MUX_ERROR_INIT;
	}

	enc->codec_data = data;
	return MUX_OK;
}

/*
 * MP3 encoder deinitialization
 */
static void mp3_encoder_deinit(struct mux_encoder *enc)
{
	struct mp3_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;
	if (data->gfp)
		lame_close(data->gfp);
	free(data);
	enc->codec_data = NULL;
}

/*
 * MP3 encoder encode
 * For audio: compress with LAME and write LEB128 frames
 * For side channel: write raw LEB128 frames
 */
static int mp3_encoder_encode(struct mux_encoder *enc,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed,
			      int stream_type)
{
	struct mp3_encoder_data *data;
	int ret;

	if (!enc || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0) {
		*input_consumed = 0;
		return MUX_OK;
	}

	/* Side channel data passes through uncompressed */
	if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
		ret = mux_leb128_write_frame(&enc->output, input, input_size,
					     stream_type);
		if (ret != MUX_OK)
			return ret;
		*input_consumed = input_size;
		return MUX_OK;
	}

	/* Audio data: compress with LAME */
	/* Assume input is interleaved 16-bit PCM */
	const int16_t *pcm = input;
	size_t num_samples = input_size / sizeof(int16_t) / enc->num_channels;
	int mp3_bytes;

	if (enc->num_channels == 1) {
		mp3_bytes = lame_encode_buffer(data->gfp,
					       pcm, NULL,
					       num_samples,
					       data->mp3_buffer,
					       sizeof(data->mp3_buffer));
	} else {
		mp3_bytes = lame_encode_buffer_interleaved(data->gfp,
							   (short *)pcm,
							   num_samples,
							   data->mp3_buffer,
							   sizeof(data->mp3_buffer));
	}

	if (mp3_bytes < 0) {
		const char *err_msg = lame_encode_error_string(mp3_bytes);
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
				      "LAME encoding failed",
				      "libmp3lame", mp3_bytes, err_msg);
		return MUX_ERROR_ENCODE;
	}

	/* Write compressed data as LEB128 frame if we got output */
	if (mp3_bytes > 0) {
		ret = mux_leb128_write_frame(&enc->output,
					     data->mp3_buffer, mp3_bytes,
					     MUX_STREAM_AUDIO);
		if (ret != MUX_OK)
			return ret;
	}

	*input_consumed = input_size;
	return MUX_OK;
}

/*
 * MP3 encoder read
 */
static int mp3_encoder_read(struct mux_encoder *enc,
			    void *output,
			    size_t output_size,
			    size_t *output_written)
{
	size_t bytes_read;
	int ret;

	if (!enc || !output || !output_written)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&enc->output, output, output_size,
			      &bytes_read);
	if (ret != MUX_OK) {
		*output_written = 0;
		return ret;
	}

	*output_written = bytes_read;
	return MUX_OK;
}

/*
 * MP3 encoder finalize
 * Flushes any buffered PCM data and generates final MP3 frames
 */
static int mp3_encoder_finalize(struct mux_encoder *enc)
{
	struct mp3_encoder_data *data;
	int mp3_bytes;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Flush LAME encoder */
	mp3_bytes = lame_encode_flush(data->gfp,
				      data->mp3_buffer,
				      sizeof(data->mp3_buffer));

	if (mp3_bytes < 0) {
		const char *err_msg = lame_encode_error_string(mp3_bytes);
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
				      "LAME flush failed",
				      "libmp3lame", mp3_bytes, err_msg);
		return MUX_ERROR_ENCODE;
	}

	/* Write flushed data as LEB128 frame if we got output */
	if (mp3_bytes > 0) {
		ret = mux_leb128_write_frame(&enc->output,
					     data->mp3_buffer, mp3_bytes,
					     MUX_STREAM_AUDIO);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}
#endif /* HAVE_MP3_ENCODE */

#ifdef HAVE_MP3_DECODE
/*
 * MP3 decoder initialization
 */
static int mp3_decoder_init(struct mux_decoder *dec,
			    const struct mux_param *params,
			    int num_params)
{
	struct mp3_decoder_data *data;
	int err;

	(void)params;
	(void)num_params;

	/* Initialize mpg123 library (once per process) */
	static int mpg123_inited = 0;
	if (!mpg123_inited) {
		if (mpg123_init() != MPG123_OK) {
			mux_decoder_set_error(dec, MUX_ERROR_INIT,
					      "Failed to initialize mpg123 library",
					      "mpg123", 0, "mpg123_init() failed");
			return MUX_ERROR_INIT;
		}
		mpg123_inited = 1;
	}

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate MP3 decoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	/* Create mpg123 decoder handle */
	data->mh = mpg123_new(NULL, &err);
	if (!data->mh) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to create mpg123 handle",
				      "mpg123", err, mpg123_plain_strerror(err));
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Open in feed mode (streaming) */
	if (mpg123_open_feed(data->mh) != MPG123_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to open mpg123 feed mode",
				      "mpg123", 0, mpg123_error_string(data->mh));
		mpg123_delete(data->mh);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Allocate input buffer for demuxing */
	if (mux_buffer_init(&data->input_buf, 4096) != MUX_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate input buffer",
				      NULL, 0, NULL);
		mpg123_delete(data->mh);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * MP3 decoder deinitialization
 */
static void mp3_decoder_deinit(struct mux_decoder *dec)
{
	struct mp3_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;
	if (data->mh)
		mpg123_delete(data->mh);
	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * MP3 decoder decode
 * Reads LEB128 frames and decompresses MP3 audio using mpg123
 */
static int mp3_decoder_decode(struct mux_decoder *dec,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed)
{
	struct mp3_decoder_data *data;
	uint8_t frame_buf[8192];
	size_t frame_size;
	int stream_type;
	int ret;
	size_t consumed = 0;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Add input to buffer */
	ret = mux_buffer_write(&data->input_buf, input, input_size);
	if (ret != MUX_OK)
		return ret;

	consumed = input_size;

	/* Try to read frames from input buffer */
	while (1) {
		ret = mux_leb128_read_frame(&data->input_buf,
					    frame_buf, sizeof(frame_buf),
					    &frame_size, &stream_type);
		if (ret == MUX_ERROR_AGAIN) {
			/* Need more data */
			break;
		}
		if (ret != MUX_OK) {
			mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
					      "Failed to read LEB128 frame",
					      NULL, 0, NULL);
			return ret;
		}

		/* Side channel passes through */
		if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
			ret = mux_buffer_write(&dec->side_output,
					       frame_buf, frame_size);
			if (ret != MUX_OK)
				return ret;
			continue;
		}

		/* Feed MP3 data to mpg123 */
		ret = mpg123_feed(data->mh, frame_buf, frame_size);
		if (ret != MPG123_OK && ret != MPG123_NEED_MORE) {
			mux_decoder_set_error(dec, MUX_ERROR_DECODE,
					      "mpg123_feed failed",
					      "mpg123", ret, mpg123_error_string(data->mh));
			return MUX_ERROR_DECODE;
		}

		/* Read all available decoded data after this feed */
		while (1) {
			unsigned char pcm_buf[16384];
			size_t pcm_bytes;

			ret = mpg123_read(data->mh, pcm_buf, sizeof(pcm_buf), &pcm_bytes);

			/* Write decoded data if we got any */
			if (pcm_bytes > 0) {
				int write_ret = mux_buffer_write(&dec->audio_output,
							       pcm_buf, pcm_bytes);
				if (write_ret != MUX_OK)
					return write_ret;
			}

			/* Check return code */
			if (ret == MPG123_OK) {
				/* Got data, continue reading */
				continue;
			} else if (ret == MPG123_DONE) {
				/* End of stream */
				break;
			} else if (ret == MPG123_NEED_MORE) {
				/* Need more input - break inner loop to feed more */
				break;
			} else if (ret == MPG123_NEW_FORMAT) {
				/* Format changed - get new format and continue */
				long rate;
				int channels, encoding;
				mpg123_getformat(data->mh, &rate, &channels, &encoding);
				continue;
			} else {
				/* Error */
				mux_decoder_set_error(dec, MUX_ERROR_DECODE,
						      "mpg123_read failed",
						      "mpg123", ret, mpg123_error_string(data->mh));
				return MUX_ERROR_DECODE;
			}
		}
	}

	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * MP3 decoder read
 */
static int mp3_decoder_read(struct mux_decoder *dec,
			    void *output,
			    size_t output_size,
			    size_t *output_written,
			    int *stream_type)
{
	size_t bytes_read;
	int ret;

	if (!dec || !output || !output_written || !stream_type)
		return MUX_ERROR_INVAL;

	/* Try audio buffer first */
	ret = mux_buffer_read(&dec->audio_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_AUDIO;
		return MUX_OK;
	}

	/* Try side channel buffer */
	ret = mux_buffer_read(&dec->side_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_SIDE_CHANNEL;
		return MUX_OK;
	}

	/* No data available */
	*output_written = 0;
	return MUX_ERROR_AGAIN;
}

/*
 * MP3 decoder finalize
 * Flushes mpg123 internal buffers by feeding NULL
 */
static int mp3_decoder_finalize(struct mux_decoder *dec)
{
	struct mp3_decoder_data *data;
	int ret;

	if (!dec)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data || !data->mh)
		return MUX_ERROR_INVAL;

	/* Feed NULL to signal end of stream */
	ret = mpg123_feed(data->mh, NULL, 0);
	if (ret != MPG123_OK && ret != MPG123_NEED_MORE) {
		mux_decoder_set_error(dec, MUX_ERROR_DECODE,
				      "mpg123_feed(NULL) failed",
				      "mpg123", ret, mpg123_error_string(data->mh));
		return MUX_ERROR_DECODE;
	}

	/* Read out remaining decoded data */
	while (1) {
		unsigned char pcm_buf[16384];
		size_t pcm_bytes;

		ret = mpg123_read(data->mh, pcm_buf, sizeof(pcm_buf), &pcm_bytes);

		/* Write decoded data if we got any */
		if (pcm_bytes > 0) {
			int write_ret = mux_buffer_write(&dec->audio_output,
						       pcm_buf, pcm_bytes);
			if (write_ret != MUX_OK)
				return write_ret;
		}

		/* Check return code */
		if (ret == MPG123_OK) {
			/* Got data, continue reading */
			continue;
		} else if (ret == MPG123_DONE) {
			/* End of stream reached */
			break;
		} else if (ret == MPG123_NEED_MORE) {
			/* No more data */
			break;
		} else if (ret == MPG123_NEW_FORMAT) {
			/* Format changed - get new format and continue */
			long rate;
			int channels, encoding;
			mpg123_getformat(data->mh, &rate, &channels, &encoding);
			continue;
		} else {
			/* Error */
			mux_decoder_set_error(dec, MUX_ERROR_DECODE,
					      "mpg123_read failed during finalize",
					      "mpg123", ret, mpg123_error_string(data->mh));
			return MUX_ERROR_DECODE;
		}
	}

	return MUX_OK;
}
#endif /* HAVE_MP3_DECODE */

/*
 * MP3 sample rate constraints (MPEG-1 and MPEG-2 supported rates)
 */
static const int mp3_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000
};

/*
 * MP3 codec operations
 */
const struct mux_codec_ops mux_codec_mp3_ops = {
#ifdef HAVE_MP3_ENCODE
	.encoder_init = mp3_encoder_init,
	.encoder_deinit = mp3_encoder_deinit,
	.encoder_encode = mp3_encoder_encode,
	.encoder_read = mp3_encoder_read,
	.encoder_finalize = mp3_encoder_finalize,
	.encoder_params = mp3_encoder_params,
	.encoder_param_count = sizeof(mp3_encoder_params) / sizeof(mp3_encoder_params[0]),
#else
	.encoder_init = NULL,
	.encoder_deinit = NULL,
	.encoder_encode = NULL,
	.encoder_read = NULL,
	.encoder_finalize = NULL,
	.encoder_params = NULL,
	.encoder_param_count = 0,
#endif

#ifdef HAVE_MP3_DECODE
	.decoder_init = mp3_decoder_init,
	.decoder_deinit = mp3_decoder_deinit,
	.decoder_decode = mp3_decoder_decode,
	.decoder_read = mp3_decoder_read,
	.decoder_finalize = mp3_decoder_finalize,
#else
	.decoder_init = NULL,
	.decoder_deinit = NULL,
	.decoder_decode = NULL,
	.decoder_read = NULL,
	.decoder_finalize = NULL,
#endif

	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = mp3_sample_rates,
	.sample_rate_count = sizeof(mp3_sample_rates) / sizeof(mp3_sample_rates[0]),
	.sample_rate_is_range = 0
};
