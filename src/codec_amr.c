/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AMR-NB (Adaptive Multi-Rate Narrowband) codec
 *
 * Uses opencore-amr for encoding and decoding. Push / streaming, LEB128-framed.
 * - Sample rate: 8000 Hz, mono
 * - Frame: 160 samples (20ms); encoded frames are self-delimiting (mode byte)
 *
 * Encoder holds a fixed one-frame PCM carry (Layer-3). Decoder reassembles one
 * AMR frame per LEB128 audio frame (mux mode) or splits the raw bitstream by
 * mode-byte length (passthrough) - both bounded by a tiny fixed buffer.
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_AMR
#include <opencore-amrnb/interf_enc.h>
#include <opencore-amrnb/interf_dec.h>

#define AMR_SAMPLE_RATE     8000
#define AMR_FRAME_SAMPLES   160   /* 20ms at 8kHz */
#define AMR_MAX_FRAME_SIZE  32    /* Maximum encoded frame size */

#define AMR_MODE_475   0
#define AMR_MODE_515   1
#define AMR_MODE_590   2
#define AMR_MODE_670   3
#define AMR_MODE_740   4
#define AMR_MODE_795   5
#define AMR_MODE_1020  6
#define AMR_MODE_1220  7

struct amr_encoder_data {
	void *encoder;
	int mode;
	int dtx;

	int16_t input_buf[AMR_FRAME_SAMPLES];   /* fixed one-frame carry */
	int input_samples;
};

struct amr_decoder_data {
	void *decoder;
	struct mux_leb128_parser parser;

	uint8_t frame[64];   /* reassembly (mux) / accumulator (passthrough) */
	int frame_len;
	int need;            /* passthrough: expected frame size (0 = unknown) */
};

static const struct mux_param_desc amr_encoder_params[] = {
	{ .name = "bitrate",
	  .description = "Bitrate in kbps (4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2)",
	  .type = MUX_PARAM_TYPE_FLOAT, .range.f = { .min = 4.75f, .max = 12.2f, .def = 12.2f } },
	{ .name = "dtx",
	  .description = "Enable discontinuous transmission (silence compression)",
	  .type = MUX_PARAM_TYPE_BOOL, .range.b = { .def = 0 } }
};

static int bitrate_to_mode(float bitrate)
{
	if (bitrate <= 4.75f) return AMR_MODE_475;
	if (bitrate <= 5.15f) return AMR_MODE_515;
	if (bitrate <= 5.9f)  return AMR_MODE_590;
	if (bitrate <= 6.7f)  return AMR_MODE_670;
	if (bitrate <= 7.4f)  return AMR_MODE_740;
	if (bitrate <= 7.95f) return AMR_MODE_795;
	if (bitrate <= 10.2f) return AMR_MODE_1020;
	return AMR_MODE_1220;
}

/* Encoded AMR frame size (bytes, incl. mode byte) from the mode byte. */
static int amr_get_frame_size(uint8_t mode_byte)
{
	int frame_type = (mode_byte >> 3) & 0x0F;
	static const int frame_sizes[] = {
		13, 14, 16, 18, 20, 21, 27, 32,  /* speech modes 0-7 */
		6,                               /* SID */
		0, 0, 0, 0, 0, 0,                /* reserved 9-14 */
		1                                /* NO_DATA */
	};
	return frame_type > 15 ? -1 : frame_sizes[frame_type];
}

/* ---- encoder ------------------------------------------------------------ */

static int amr_encoder_init(struct mux_encoder *enc, int sample_rate,
			    int num_channels, const struct mux_param *params,
			    int num_params)
{
	struct amr_encoder_data *data;
	int i;

	if (sample_rate != AMR_SAMPLE_RATE) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-NB requires 8000 Hz sample rate",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INVAL;
	}
	if (num_channels != 1) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-NB requires mono audio",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	data->mode = AMR_MODE_1220;
	data->dtx = 0;
	for (i = 0; i < num_params; i++) {
		if (strcmp(params[i].name, "bitrate") == 0)
			data->mode = bitrate_to_mode(params[i].value.f);
		else if (strcmp(params[i].name, "dtx") == 0)
			data->dtx = params[i].value.b;
	}

	data->encoder = Encoder_Interface_init(data->dtx);
	if (!data->encoder) {
		free(data);
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AMR encoder",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	data->input_samples = 0;
	enc->codec_data = data;
	return MUX_OK;
}

static void amr_encoder_deinit(struct mux_encoder *enc)
{
	struct amr_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;
	data = enc->codec_data;
	if (data->encoder)
		Encoder_Interface_exit(data->encoder);
	free(data);
	enc->codec_data = NULL;
}

static int amr_encode_frame(struct mux_encoder *enc, struct amr_encoder_data *data)
{
	uint8_t frame_buf[AMR_MAX_FRAME_SIZE];
	int frame_size = Encoder_Interface_Encode(data->encoder, data->mode,
						  data->input_buf, frame_buf, 0);
	if (frame_size < 0) {
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE, "AMR encoding failed",
				      "opencore-amrnb", frame_size, NULL);
		return MUX_ERROR_ENCODE;
	}
	if (frame_size == 0)
		return MUX_OK;
	return mux_leb128_emit_frame(frame_buf, frame_size, MUX_STREAM_AUDIO,
				     enc->num_streams, enc->sink, enc->sink_user);
}

static int amr_encoder_encode(struct mux_encoder *enc, const void *input,
			      size_t input_size, int stream_type)
{
	struct amr_encoder_data *data;
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
		size_t need = AMR_FRAME_SAMPLES - data->input_samples;
		size_t copy = avail < need ? avail : need;

		memcpy(&data->input_buf[data->input_samples], samples,
		       copy * sizeof(int16_t));
		data->input_samples += copy;
		samples += copy;
		avail -= copy;

		if (data->input_samples == AMR_FRAME_SAMPLES) {
			ret = amr_encode_frame(enc, data);
			if (ret)
				return ret;
			data->input_samples = 0;
		}
	}

	return MUX_OK;
}

static int amr_encoder_finalize(struct mux_encoder *enc)
{
	struct amr_encoder_data *data;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (data->input_samples > 0) {
		memset(&data->input_buf[data->input_samples], 0,
		       (AMR_FRAME_SAMPLES - data->input_samples) * sizeof(int16_t));
		int ret = amr_encode_frame(enc, data);
		if (ret)
			return ret;
		data->input_samples = 0;
	}

	return MUX_OK;
}

/* ---- decoder ------------------------------------------------------------ */

static int amr_decoder_init(struct mux_decoder *dec,
			    const struct mux_param *params, int num_params)
{
	struct amr_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	mux_leb128_parser_init(&data->parser);
	data->decoder = Decoder_Interface_init();
	if (!data->decoder) {
		free(data);
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize AMR decoder",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

static void amr_decoder_deinit(struct mux_decoder *dec)
{
	struct amr_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;
	data = dec->codec_data;
	if (data->decoder)
		Decoder_Interface_exit(data->decoder);
	free(data);
	dec->codec_data = NULL;
}

static int amr_decode_one(struct mux_decoder *dec, struct amr_decoder_data *data,
			  uint8_t *amr_frame)
{
	int16_t pcm_out[AMR_FRAME_SAMPLES];
	Decoder_Interface_Decode(data->decoder, amr_frame, pcm_out, 0);
	return mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm_out,
				sizeof(pcm_out), 0);
}

/* Parser emit shim. Mux mode: reassemble one AMR frame per LEB128 audio frame.
 * Passthrough: split the raw AMR bitstream by mode-byte length. */
static int amr_parser_emit(void *user, int stream_type, const void *chunk,
			   size_t size, int flags)
{
	struct mux_decoder *dec = user;
	struct amr_decoder_data *data = dec->codec_data;
	const uint8_t *in = chunk;
	int ret;

	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_decoder_emit(dec, stream_type, chunk, size, flags);

	if (dec->num_streams == 2) {
		if (data->frame_len + (int)size > (int)sizeof(data->frame)) {
			mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
					      "AMR frame exceeds buffer", NULL, 0, NULL);
			return MUX_ERROR_FORMAT;
		}
		if (size) {
			memcpy(data->frame + data->frame_len, in, size);
			data->frame_len += size;
		}
		if (flags & MUX_EMIT_FRAME_END) {
			ret = data->frame_len > 0
			      ? amr_decode_one(dec, data, data->frame) : MUX_OK;
			data->frame_len = 0;
			return ret;
		}
		return MUX_OK;
	}

	/* Passthrough: self-delimiting frames via mode byte. */
	for (size_t i = 0; i < size; i++) {
		data->frame[data->frame_len++] = in[i];
		if (data->frame_len == 1) {
			data->need = amr_get_frame_size(data->frame[0]);
			if (data->need <= 0 || data->need > (int)sizeof(data->frame)) {
				mux_decoder_set_error(dec, MUX_ERROR_DECODE,
						      "Invalid AMR frame header",
						      NULL, 0, NULL);
				return MUX_ERROR_DECODE;
			}
		}
		if (data->frame_len == data->need) {
			ret = amr_decode_one(dec, data, data->frame);
			data->frame_len = 0;
			data->need = 0;
			if (ret)
				return ret;
		}
	}
	return MUX_OK;
}

static int amr_decoder_decode(struct mux_decoder *dec, const void *input,
			      size_t input_size)
{
	struct amr_decoder_data *data;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(&data->parser, input, input_size,
				      dec->num_streams, amr_parser_emit, dec);
}

static int amr_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

static const int amr_sample_rates[] = { 8000 };

const struct mux_codec_ops mux_codec_amr_ops = {
	.encoder_init = amr_encoder_init,
	.encoder_deinit = amr_encoder_deinit,
	.encoder_encode = amr_encoder_encode,
	.encoder_finalize = amr_encoder_finalize,

	.decoder_init = amr_decoder_init,
	.decoder_deinit = amr_decoder_deinit,
	.decoder_decode = amr_decoder_decode,
	.decoder_finalize = amr_decoder_finalize,

	.encoder_params = amr_encoder_params,
	.encoder_param_count = sizeof(amr_encoder_params) / sizeof(amr_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = amr_sample_rates,
	.sample_rate_count = 1,
	.sample_rate_is_range = 0
};

#endif /* HAVE_AMR */
