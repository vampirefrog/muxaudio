/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AMR-WB (Adaptive Multi-Rate Wideband / G.722.2) codec
 *
 * Decode via opencore-amrwb; encode via vo-amrwbenc (optional). Push /
 * streaming, LEB128-framed, mirroring codec_amr.c.
 * - Sample rate: 16000 Hz, mono; frame: 320 samples (20ms), self-delimiting.
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_AMR_WB

#include <opencore-amrwb/dec_if.h>

#ifdef HAVE_AMR_WB_ENCODE
#include <vo-amrwbenc/enc_if.h>
#endif

#define AMR_WB_SAMPLE_RATE     16000
#define AMR_WB_FRAME_SAMPLES   320   /* 20ms at 16kHz */
#define AMR_WB_MAX_FRAME_SIZE  61

#define AMR_WB_MODE_660   0
#define AMR_WB_MODE_885   1
#define AMR_WB_MODE_1265  2
#define AMR_WB_MODE_1425  3
#define AMR_WB_MODE_1585  4
#define AMR_WB_MODE_1825  5
#define AMR_WB_MODE_1985  6
#define AMR_WB_MODE_2305  7
#define AMR_WB_MODE_2385  8

struct amr_wb_encoder_data {
#ifdef HAVE_AMR_WB_ENCODE
	void *encoder;
	int mode;
	int dtx;
	int16_t input_buf[AMR_WB_FRAME_SAMPLES];   /* fixed one-frame carry */
	int input_samples;
#else
	int dummy;
#endif
};

struct amr_wb_decoder_data {
	void *decoder;
	struct mux_leb128_parser parser;

	uint8_t frame[64];   /* reassembly (mux) / accumulator (passthrough) */
	int frame_len;
	int need;            /* passthrough: expected frame size (0 = unknown) */
};

static const struct mux_param_desc amr_wb_encoder_params[] = {
	{ .name = "bitrate",
	  .description = "Bitrate in kbps (6.6, 8.85, 12.65, 14.25, 15.85, 18.25, 19.85, 23.05, 23.85)",
	  .type = MUX_PARAM_TYPE_FLOAT, .range.f = { .min = 6.6f, .max = 23.85f, .def = 23.85f } },
	{ .name = "dtx",
	  .description = "Enable discontinuous transmission (silence compression)",
	  .type = MUX_PARAM_TYPE_BOOL, .range.b = { .def = 0 } }
};

#ifdef HAVE_AMR_WB_ENCODE
static int bitrate_to_mode(float bitrate)
{
	if (bitrate <= 6.6f)   return AMR_WB_MODE_660;
	if (bitrate <= 8.85f)  return AMR_WB_MODE_885;
	if (bitrate <= 12.65f) return AMR_WB_MODE_1265;
	if (bitrate <= 14.25f) return AMR_WB_MODE_1425;
	if (bitrate <= 15.85f) return AMR_WB_MODE_1585;
	if (bitrate <= 18.25f) return AMR_WB_MODE_1825;
	if (bitrate <= 19.85f) return AMR_WB_MODE_1985;
	if (bitrate <= 23.05f) return AMR_WB_MODE_2305;
	return AMR_WB_MODE_2385;
}
#endif

/* Encoded AMR-WB frame size (bytes, incl. mode byte) from the mode byte. */
static int amr_wb_get_frame_size(uint8_t mode_byte)
{
	int frame_type = (mode_byte >> 3) & 0x0F;
	static const int frame_sizes[] = {
		18, 24, 33, 37, 41, 47, 51, 59, 61,  /* speech modes 0-8 */
		6,                                   /* SID */
		0, 0, 0, 0,                          /* reserved 10-13 */
		1, 1                                 /* speech lost, NO_DATA */
	};
	return frame_type > 15 ? -1 : frame_sizes[frame_type];
}

/* ---- encoder ------------------------------------------------------------ */

static int amr_wb_encoder_init(struct mux_encoder *enc, int sample_rate,
			       int num_channels, const struct mux_param *params,
			       int num_params)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;
	int i;

	if (sample_rate != AMR_WB_SAMPLE_RATE) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-WB requires 16000 Hz sample rate",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INVAL;
	}
	if (num_channels != 1) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-WB requires mono audio",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	data->mode = AMR_WB_MODE_2385;
	data->dtx = 0;
	for (i = 0; i < num_params; i++) {
		if (strcmp(params[i].name, "bitrate") == 0)
			data->mode = bitrate_to_mode(params[i].value.f);
		else if (strcmp(params[i].name, "dtx") == 0)
			data->dtx = params[i].value.b;
	}

	data->encoder = E_IF_init();
	if (!data->encoder) {
		free(data);
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AMR-WB encoder",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INIT;
	}

	data->input_samples = 0;
	enc->codec_data = data;
	return MUX_OK;
#else
	(void)sample_rate;
	(void)num_channels;
	(void)params;
	(void)num_params;
	mux_encoder_set_error(enc, MUX_ERROR_NOCODEC,
			      "AMR-WB encoding not available (vo-amrwbenc not found)",
			      "vo-amrwbenc", 0, NULL);
	return MUX_ERROR_NOCODEC;
#endif
}

static void amr_wb_encoder_deinit(struct mux_encoder *enc)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;
	data = enc->codec_data;
	if (data->encoder)
		E_IF_exit(data->encoder);
	free(data);
	enc->codec_data = NULL;
#else
	(void)enc;
#endif
}

#ifdef HAVE_AMR_WB_ENCODE
static int amr_wb_encode_frame(struct mux_encoder *enc,
			       struct amr_wb_encoder_data *data)
{
	uint8_t frame_buf[AMR_WB_MAX_FRAME_SIZE];
	int frame_size = E_IF_encode(data->encoder, data->mode,
				     data->input_buf, frame_buf, data->dtx);
	if (frame_size < 0) {
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE, "AMR-WB encoding failed",
				      "vo-amrwbenc", frame_size, NULL);
		return MUX_ERROR_ENCODE;
	}
	if (frame_size == 0)
		return MUX_OK;
	return mux_leb128_emit_frame(frame_buf, frame_size, MUX_STREAM_AUDIO,
				     enc->num_streams, enc->sink, enc->sink_user);
}
#endif

static int amr_wb_encoder_encode(struct mux_encoder *enc, const void *input,
				 size_t input_size, int stream_type)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;
	const int16_t *samples;
	size_t avail;
	int ret;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;
	if (input_size == 0)
		return MUX_OK;

	if (stream_type != MUX_STREAM_AUDIO)
		return mux_leb128_emit_frame(input, input_size, stream_type,
					     enc->num_streams,
					     enc->sink, enc->sink_user);

	samples = input;
	avail = input_size / sizeof(int16_t);

	while (avail > 0) {
		size_t need = AMR_WB_FRAME_SAMPLES - data->input_samples;
		size_t copy = avail < need ? avail : need;

		memcpy(&data->input_buf[data->input_samples], samples,
		       copy * sizeof(int16_t));
		data->input_samples += copy;
		samples += copy;
		avail -= copy;

		if (data->input_samples == AMR_WB_FRAME_SAMPLES) {
			ret = amr_wb_encode_frame(enc, data);
			if (ret)
				return ret;
			data->input_samples = 0;
		}
	}

	return MUX_OK;
#else
	(void)enc;
	(void)input;
	(void)input_size;
	(void)stream_type;
	return MUX_ERROR_NOCODEC;
#endif
}

static int amr_wb_encoder_finalize(struct mux_encoder *enc)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (data->input_samples > 0) {
		memset(&data->input_buf[data->input_samples], 0,
		       (AMR_WB_FRAME_SAMPLES - data->input_samples) * sizeof(int16_t));
		int ret = amr_wb_encode_frame(enc, data);
		if (ret)
			return ret;
		data->input_samples = 0;
	}

	return MUX_OK;
#else
	(void)enc;
	return MUX_OK;
#endif
}

/* ---- decoder ------------------------------------------------------------ */

static int amr_wb_decoder_init(struct mux_decoder *dec,
			       const struct mux_param *params, int num_params)
{
	struct amr_wb_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	mux_leb128_parser_init(&data->parser);
	data->decoder = D_IF_init();
	if (!data->decoder) {
		free(data);
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize AMR-WB decoder",
				      "opencore-amrwb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

static void amr_wb_decoder_deinit(struct mux_decoder *dec)
{
	struct amr_wb_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;
	data = dec->codec_data;
	if (data->decoder)
		D_IF_exit(data->decoder);
	free(data);
	dec->codec_data = NULL;
}

static int amr_wb_decode_one(struct mux_decoder *dec,
			     struct amr_wb_decoder_data *data, uint8_t *amr_frame)
{
	int16_t pcm_out[AMR_WB_FRAME_SAMPLES];
	D_IF_decode(data->decoder, amr_frame, pcm_out, 0);
	return mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm_out,
				sizeof(pcm_out), 0);
}

/* Parser emit shim. Mux mode: reassemble one AMR-WB frame per LEB128 audio
 * frame. Passthrough: split the raw bitstream by mode-byte length. */
static int amr_wb_parser_emit(void *user, int stream_type, const void *chunk,
			      size_t size, int flags)
{
	struct mux_decoder *dec = user;
	struct amr_wb_decoder_data *data = dec->codec_data;
	const uint8_t *in = chunk;
	int ret;

	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_decoder_emit(dec, stream_type, chunk, size, flags);

	if (dec->num_streams == 2) {
		if (data->frame_len + (int)size > (int)sizeof(data->frame)) {
			mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
					      "AMR-WB frame exceeds buffer", NULL, 0, NULL);
			return MUX_ERROR_FORMAT;
		}
		if (size) {
			memcpy(data->frame + data->frame_len, in, size);
			data->frame_len += size;
		}
		if (flags & MUX_EMIT_FRAME_END) {
			ret = data->frame_len > 0
			      ? amr_wb_decode_one(dec, data, data->frame) : MUX_OK;
			data->frame_len = 0;
			return ret;
		}
		return MUX_OK;
	}

	/* Passthrough: self-delimiting frames via mode byte. */
	for (size_t i = 0; i < size; i++) {
		data->frame[data->frame_len++] = in[i];
		if (data->frame_len == 1) {
			data->need = amr_wb_get_frame_size(data->frame[0]);
			if (data->need <= 0 || data->need > (int)sizeof(data->frame)) {
				mux_decoder_set_error(dec, MUX_ERROR_DECODE,
						      "Invalid AMR-WB frame header",
						      NULL, 0, NULL);
				return MUX_ERROR_DECODE;
			}
		}
		if (data->frame_len == data->need) {
			ret = amr_wb_decode_one(dec, data, data->frame);
			data->frame_len = 0;
			data->need = 0;
			if (ret)
				return ret;
		}
	}
	return MUX_OK;
}

static int amr_wb_decoder_decode(struct mux_decoder *dec, const void *input,
				 size_t input_size)
{
	struct amr_wb_decoder_data *data;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(&data->parser, input, input_size,
				      dec->num_streams, amr_wb_parser_emit, dec);
}

static int amr_wb_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

static const int amr_wb_sample_rates[] = { 16000 };

const struct mux_codec_ops mux_codec_amr_wb_ops = {
	.encoder_init = amr_wb_encoder_init,
	.encoder_deinit = amr_wb_encoder_deinit,
	.encoder_encode = amr_wb_encoder_encode,
	.encoder_finalize = amr_wb_encoder_finalize,

	.decoder_init = amr_wb_decoder_init,
	.decoder_deinit = amr_wb_decoder_deinit,
	.decoder_decode = amr_wb_decoder_decode,
	.decoder_finalize = amr_wb_decoder_finalize,

	.encoder_params = amr_wb_encoder_params,
	.encoder_param_count = sizeof(amr_wb_encoder_params) / sizeof(amr_wb_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = amr_wb_sample_rates,
	.sample_rate_count = 1,
	.sample_rate_is_range = 0
};

#endif /* HAVE_AMR_WB */
