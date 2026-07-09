/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>

/*
 * FLAC codec (push / streaming, LEB128-framed)
 *
 * Encoder: libFLAC's write callback fires once per FLAC frame (and once for the
 * stream header). Each such write becomes exactly one LEB128 audio frame emitted
 * straight to the sink - no output buffer.
 *
 * Decoder: libFLAC is pull-based (read callback), which we bridge to the push
 * API by exploiting the framing above. The LEB128 parser tells us, via
 * MUX_EMIT_FRAME_END, where each FLAC frame ends; we reassemble one frame into a
 * fixed buffer and let libFLAC pull that single complete frame, then flush to
 * clear its end-of-stream flag before the next frame. No growable buffering.
 */

#define FLAC_ENC_BLOCK   4096            /* samples/channel per process() call */
#define FLAC_FRAME_CAP   (1u << 18)      /* max reassembled FLAC frame (256 KiB) */

struct flac_encoder_data {
	FLAC__StreamEncoder *enc;
	struct mux_encoder *owner;       /* for sink access inside callbacks */
	FLAC__int32 *planar[8];          /* per-channel scratch, allocated once */
	int sink_ret;                    /* sticky non-zero sink return */
	int sample_rate;
	int num_channels;
};

struct flac_decoder_data {
	FLAC__StreamDecoder *dec;
	struct mux_decoder *owner;       /* for emit access inside callbacks */
	struct mux_leb128_parser parser;

	uint8_t frame[FLAC_FRAME_CAP];   /* one reassembled FLAC frame/header */
	size_t frame_len;                /* bytes accumulated                 */
	size_t frame_read;               /* bytes handed to libFLAC read cb   */

	int emit_ret;                    /* sticky non-zero emit return */
	int overflow;                    /* frame exceeded FLAC_FRAME_CAP */
	int sample_rate;
	int num_channels;
};

static const struct mux_param_desc flac_encoder_params[] = {
	{
		.name = "compression",
		.description = "Compression level (0=fast, 8=best)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 0, .max = 8, .def = 5 }
	}
};

static const int flac_sample_rates[] = { 1000, 655350 };  /* Min/max range */

static const struct mux_param *find_param(const struct mux_param *params,
					  int num_params, const char *name)
{
	int i;
	for (i = 0; i < num_params; i++)
		if (strcmp(params[i].name, name) == 0)
			return &params[i];
	return NULL;
}

/* ---- encoder ------------------------------------------------------------ */

/* Called once per FLAC frame (and for header blocks): wrap as one audio frame. */
static FLAC__StreamEncoderWriteStatus flac_write_callback(
	const FLAC__StreamEncoder *encoder,
	const FLAC__byte buffer[], size_t bytes,
	unsigned samples, unsigned current_frame, void *client_data)
{
	struct flac_encoder_data *data = client_data;
	struct mux_encoder *enc = data->owner;
	int r;

	(void)encoder;
	(void)samples;
	(void)current_frame;

	if (bytes > 0) {
		r = mux_leb128_emit_frame(buffer, bytes, MUX_STREAM_AUDIO,
					  enc->num_streams,
					  enc->sink, enc->sink_user);
		if (r) {
			data->sink_ret = r;
			return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
		}
	}
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static int mux_flac_encoder_init(struct mux_encoder *enc, int sample_rate,
				 int num_channels, const struct mux_param *params,
				 int num_params)
{
	struct flac_encoder_data *data;
	const struct mux_param *param;
	int compression = 5;
	int ch;
	FLAC__StreamEncoderInitStatus init_status;

	if (num_channels < 1 || num_channels > 8) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "FLAC supports 1-8 channels", NULL, 0, NULL);
		return MUX_ERROR_INVAL;
	}

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->owner = enc;
	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	for (ch = 0; ch < num_channels; ch++) {
		data->planar[ch] = malloc(FLAC_ENC_BLOCK * sizeof(FLAC__int32));
		if (!data->planar[ch]) {
			while (ch-- > 0) free(data->planar[ch]);
			free(data);
			mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
					      "Failed to allocate FLAC scratch",
					      NULL, 0, NULL);
			return MUX_ERROR_NOMEM;
		}
	}

	param = find_param(params, num_params, "compression");
	if (param)
		compression = param->value.i;

	data->enc = FLAC__stream_encoder_new();
	if (!data->enc) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to create FLAC encoder",
				      "libFLAC", 0, NULL);
		goto fail;
	}

	FLAC__stream_encoder_set_channels(data->enc, num_channels);
	FLAC__stream_encoder_set_bits_per_sample(data->enc, 16);
	FLAC__stream_encoder_set_sample_rate(data->enc, sample_rate);
	FLAC__stream_encoder_set_compression_level(data->enc, compression);

	init_status = FLAC__stream_encoder_init_stream(
		data->enc, flac_write_callback, NULL, NULL, NULL, data);
	if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize FLAC encoder",
				      "libFLAC", init_status,
				      FLAC__StreamEncoderInitStatusString[init_status]);
		FLAC__stream_encoder_delete(data->enc);
		goto fail;
	}

	enc->codec_data = data;
	return MUX_OK;

fail:
	for (ch = 0; ch < num_channels; ch++) free(data->planar[ch]);
	free(data);
	return MUX_ERROR_INIT;
}

static void mux_flac_encoder_deinit(struct mux_encoder *enc)
{
	struct flac_encoder_data *data;
	int ch;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;
	if (data->enc)
		FLAC__stream_encoder_delete(data->enc);
	for (ch = 0; ch < data->num_channels; ch++)
		free(data->planar[ch]);
	free(data);
	enc->codec_data = NULL;
}

static int mux_flac_encoder_encode(struct mux_encoder *enc, const void *input,
				   size_t input_size, int stream_type)
{
	struct flac_encoder_data *data;
	const int16_t *pcm;
	size_t total, off;
	int nch;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0)
		return MUX_OK;

	/* Side channel passes through uncompressed. */
	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_leb128_emit_frame(input, input_size, stream_type,
					     enc->num_streams,
					     enc->sink, enc->sink_user);

	pcm = input;
	nch = data->num_channels;
	total = input_size / sizeof(int16_t) / nch;

	for (off = 0; off < total; ) {
		size_t blk = total - off;
		size_t i;
		int ch;

		if (blk > FLAC_ENC_BLOCK)
			blk = FLAC_ENC_BLOCK;

		for (i = 0; i < blk; i++)
			for (ch = 0; ch < nch; ch++)
				data->planar[ch][i] =
					pcm[(off + i) * nch + ch];

		if (!FLAC__stream_encoder_process(
			    data->enc,
			    (const FLAC__int32 *const *)data->planar, blk)) {
			FLAC__StreamEncoderState st =
				FLAC__stream_encoder_get_state(data->enc);
			mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
					      "FLAC encoding failed", "libFLAC",
					      st, FLAC__StreamEncoderStateString[st]);
			return MUX_ERROR_ENCODE;
		}
		if (data->sink_ret)
			return data->sink_ret;

		off += blk;
	}

	return MUX_OK;
}

static int mux_flac_encoder_finalize(struct mux_encoder *enc)
{
	struct flac_encoder_data *data;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Flushes remaining frames through the write callback -> sink. */
	FLAC__stream_encoder_finish(data->enc);
	return data->sink_ret;
}

/* ---- decoder ------------------------------------------------------------ */

static FLAC__StreamDecoderReadStatus flac_read_callback(
	const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
	size_t *bytes, void *client_data)
{
	struct flac_decoder_data *data = client_data;
	size_t avail = data->frame_len - data->frame_read;

	(void)decoder;

	if (avail == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if (*bytes > avail)
		*bytes = avail;
	memcpy(buffer, data->frame + data->frame_read, *bytes);
	data->frame_read += *bytes;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus flac_dec_write_callback(
	const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
	const FLAC__int32 *const buffer[], void *client_data)
{
	struct flac_decoder_data *data = client_data;
	struct mux_decoder *dec = data->owner;
	unsigned bs = frame->header.blocksize;
	int nch = frame->header.channels;
	unsigned i = 0;

	(void)decoder;

	while (i < bs) {
		int16_t tmp[2048];
		unsigned cap = (unsigned)(sizeof(tmp) / sizeof(tmp[0])) / nch;
		unsigned m = bs - i;
		unsigned j;
		int ch, r;

		if (m > cap)
			m = cap;

		for (j = 0; j < m; j++)
			for (ch = 0; ch < nch; ch++)
				tmp[j * nch + ch] = (int16_t)buffer[ch][i + j];

		r = mux_decoder_emit(dec, MUX_STREAM_AUDIO, tmp,
				     (size_t)m * nch * sizeof(int16_t), 0);
		if (r) {
			data->emit_ret = r;
			return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
		}
		i += m;
	}
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void flac_dec_metadata_callback(const FLAC__StreamDecoder *decoder,
				       const FLAC__StreamMetadata *metadata,
				       void *client_data)
{
	struct flac_decoder_data *data = client_data;
	(void)decoder;
	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		data->sample_rate = metadata->data.stream_info.sample_rate;
		data->num_channels = metadata->data.stream_info.channels;
	}
}

static void flac_dec_error_callback(const FLAC__StreamDecoder *decoder,
				    FLAC__StreamDecoderErrorStatus status,
				    void *client_data)
{
	(void)decoder;
	(void)status;
	(void)client_data;
}

static int mux_flac_decoder_init(struct mux_decoder *dec,
				 const struct mux_param *params, int num_params)
{
	struct flac_decoder_data *data;
	FLAC__StreamDecoderInitStatus init_status;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC decoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->owner = dec;
	mux_leb128_parser_init(&data->parser);

	data->dec = FLAC__stream_decoder_new();
	if (!data->dec) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to create FLAC decoder",
				      "libFLAC", 0, NULL);
		free(data);
		return MUX_ERROR_INIT;
	}

	init_status = FLAC__stream_decoder_init_stream(
		data->dec, flac_read_callback, NULL, NULL, NULL, NULL,
		flac_dec_write_callback, flac_dec_metadata_callback,
		flac_dec_error_callback, data);
	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "FLAC decoder init failed", "libFLAC",
				      init_status,
				      FLAC__StreamDecoderInitStatusString[init_status]);
		FLAC__stream_decoder_delete(data->dec);
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

static void mux_flac_decoder_deinit(struct mux_decoder *dec)
{
	struct flac_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;
	if (data->dec) {
		FLAC__stream_decoder_finish(data->dec);
		FLAC__stream_decoder_delete(data->dec);
	}
	free(data);
	dec->codec_data = NULL;
}

/* Drive libFLAC over one reassembled, complete FLAC frame (or header block). */
static int flac_decode_unit(struct flac_decoder_data *data)
{
	data->frame_read = 0;
	data->emit_ret = 0;

	/* One process_single consumes the whole unit; the read callback returns
	 * END_OF_STREAM at the unit boundary, which is how libFLAC knows the
	 * frame ended. Loop in case a header unit yields a metadata block first. */
	while (data->frame_read < data->frame_len) {
		if (!FLAC__stream_decoder_process_single(data->dec))
			break;
		if (data->emit_ret)
			break;
	}

	/* Clear the end-of-stream flag so the next unit can be read. Safe here
	 * because a complete frame has already been emitted. */
	FLAC__stream_decoder_flush(data->dec);

	data->frame_len = 0;
	data->frame_read = 0;
	return data->emit_ret;
}

/* Parser emit shim: reassemble audio into whole FLAC frames; pass side through. */
static int flac_parser_emit(void *user, int stream_type, const void *chunk,
			    size_t size, int flags)
{
	struct mux_decoder *dec = user;
	struct flac_decoder_data *data = dec->codec_data;

	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_decoder_emit(dec, stream_type, chunk, size, flags);

	if (data->frame_len + size > FLAC_FRAME_CAP) {
		data->overflow = 1;
		mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
				      "FLAC frame exceeds reassembly buffer",
				      NULL, 0, NULL);
		return MUX_ERROR_FORMAT;
	}
	if (size) {
		memcpy(data->frame + data->frame_len, chunk, size);
		data->frame_len += size;
	}

	if (flags & MUX_EMIT_FRAME_END)
		return flac_decode_unit(data);

	return MUX_OK;
}

static int mux_flac_decoder_decode(struct mux_decoder *dec, const void *input,
				   size_t input_size)
{
	struct flac_decoder_data *data;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(&data->parser, input, input_size,
				      dec->num_streams, flac_parser_emit, dec);
}

static int mux_flac_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;  /* frames are decoded as they complete; nothing buffered */
	return MUX_OK;
}

const struct mux_codec_ops mux_codec_flac_ops = {
	.encoder_init = mux_flac_encoder_init,
	.encoder_deinit = mux_flac_encoder_deinit,
	.encoder_encode = mux_flac_encoder_encode,
	.encoder_finalize = mux_flac_encoder_finalize,

	.decoder_init = mux_flac_decoder_init,
	.decoder_deinit = mux_flac_decoder_deinit,
	.decoder_decode = mux_flac_decoder_decode,
	.decoder_finalize = mux_flac_decoder_finalize,

	.encoder_params = flac_encoder_params,
	.encoder_param_count = sizeof(flac_encoder_params) / sizeof(flac_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = flac_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
