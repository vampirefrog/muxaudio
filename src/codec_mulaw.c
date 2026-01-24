/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Mu-law codec (G.711 mu-law)
 *
 * Compresses 16-bit linear PCM to 8-bit mu-law companded samples.
 * Standard for North American and Japanese telephony systems.
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

/* Mu-law compression constant */
#define MULAW_BIAS 0x84
#define MULAW_CLIP 32635

/*
 * Mu-law codec state
 */
struct mulaw_codec_data {
	struct mux_buffer input_buf;
};

/*
 * Mu-law encoding: 16-bit linear PCM -> 8-bit mu-law
 * Format: seeemmmm where s=sign, eee=exponent, mmmm=mantissa
 */
static uint8_t mulaw_encode_sample(int16_t pcm)
{
	int sign;
	int exponent;
	int mantissa;
	int sample;

	/* Get sign and make sample positive */
	sign = (pcm >> 8) & 0x80;
	if (sign)
		pcm = -pcm;

	/* Clip to maximum */
	if (pcm > MULAW_CLIP)
		pcm = MULAW_CLIP;

	/* Add bias for compression */
	sample = pcm + MULAW_BIAS;

	/* Find exponent (position of highest bit) */
	exponent = 7;
	for (exponent = 7; exponent > 0; exponent--) {
		if (sample & (1 << (exponent + 7)))
			break;
	}

	/* Extract mantissa */
	mantissa = (sample >> (exponent + 3)) & 0x0F;

	/* Combine and invert (mu-law inverts all bits) */
	return ~(sign | (exponent << 4) | mantissa);
}

/*
 * Mu-law decoding: 8-bit mu-law -> 16-bit linear PCM
 */
static int16_t mulaw_decode_sample(uint8_t mulaw)
{
	int sign;
	int exponent;
	int mantissa;
	int sample;

	/* Invert bits (mu-law stores inverted) */
	mulaw = ~mulaw;

	/* Extract fields */
	sign = mulaw & 0x80;
	exponent = (mulaw >> 4) & 0x07;
	mantissa = mulaw & 0x0F;

	/* Reconstruct sample */
	sample = ((mantissa << 3) + MULAW_BIAS) << exponent;
	sample -= MULAW_BIAS;

	return sign ? -sample : sample;
}

/*
 * Mu-law encoder initialization
 */
static int mulaw_encoder_init(struct mux_encoder *enc,
			      int sample_rate,
			      int num_channels,
			      const struct mux_param *params,
			      int num_params)
{
	(void)sample_rate;
	(void)num_channels;
	(void)params;
	(void)num_params;

	enc->codec_data = NULL;
	return MUX_OK;
}

/*
 * Mu-law encoder deinitialization
 */
static void mulaw_encoder_deinit(struct mux_encoder *enc)
{
	(void)enc;
}

/*
 * Mu-law encoder encode
 * Converts 16-bit PCM samples to 8-bit mu-law
 */
static int mulaw_encoder_encode(struct mux_encoder *enc,
				const void *input,
				size_t input_size,
				size_t *input_consumed,
				int stream_type)
{
	const int16_t *pcm_in;
	uint8_t *mulaw_out;
	size_t num_samples;
	size_t i;
	int ret;

	if (!enc || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	if (input_size == 0) {
		*input_consumed = 0;
		return MUX_OK;
	}

	/* For audio stream: convert PCM to mu-law */
	if (stream_type == MUX_STREAM_AUDIO) {
		/* Input is 16-bit samples */
		num_samples = input_size / sizeof(int16_t);
		if (num_samples == 0) {
			*input_consumed = 0;
			return MUX_OK;
		}

		mulaw_out = malloc(num_samples);
		if (!mulaw_out)
			return MUX_ERROR_NOMEM;

		pcm_in = (const int16_t *)input;
		for (i = 0; i < num_samples; i++) {
			mulaw_out[i] = mulaw_encode_sample(pcm_in[i]);
		}

		/* Write LEB128-framed packet */
		ret = mux_leb128_write_frame(&enc->output, mulaw_out, num_samples,
					     stream_type, enc->num_streams);
		free(mulaw_out);

		if (ret != MUX_OK)
			return ret;

		*input_consumed = num_samples * sizeof(int16_t);
	} else {
		/* Side channel: pass through unchanged */
		ret = mux_leb128_write_frame(&enc->output, input, input_size,
					     stream_type, enc->num_streams);
		if (ret != MUX_OK)
			return ret;

		*input_consumed = input_size;
	}

	return MUX_OK;
}

/*
 * Mu-law encoder read
 */
static int mulaw_encoder_read(struct mux_encoder *enc,
			      void *output,
			      size_t output_size,
			      size_t *output_written)
{
	size_t bytes_read;
	int ret;

	if (!enc || !output || !output_written)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&enc->output, output, output_size, &bytes_read);
	if (ret != MUX_OK) {
		*output_written = 0;
		return ret;
	}

	*output_written = bytes_read;
	return MUX_OK;
}

/*
 * Mu-law encoder finalize
 */
static int mulaw_encoder_finalize(struct mux_encoder *enc)
{
	(void)enc;
	return MUX_OK;
}

/*
 * Mu-law decoder initialization
 */
static int mulaw_decoder_init(struct mux_decoder *dec,
			      const struct mux_param *params,
			      int num_params)
{
	struct mulaw_codec_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	if (mux_buffer_init(&data->input_buf, 4096) != MUX_OK) {
		free(data);
		return MUX_ERROR_NOMEM;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * Mu-law decoder deinitialization
 */
static void mulaw_decoder_deinit(struct mux_decoder *dec)
{
	struct mulaw_codec_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;
	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * Mu-law decoder decode
 */
static int mulaw_decoder_decode(struct mux_decoder *dec,
				const void *input,
				size_t input_size,
				size_t *input_consumed)
{
	struct mulaw_codec_data *data;
	uint8_t *frame_buf = NULL;
	int16_t *pcm_out = NULL;
	size_t frame_buf_capacity = 8192;
	size_t frame_size;
	int stream_type;
	int ret;
	size_t consumed = 0;
	size_t i;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	frame_buf = malloc(frame_buf_capacity);
	if (!frame_buf)
		return MUX_ERROR_NOMEM;

	ret = mux_buffer_write(&data->input_buf, input, input_size);
	if (ret != MUX_OK) {
		free(frame_buf);
		return ret;
	}

	consumed = input_size;

	while (1) {
		ret = mux_leb128_read_frame(&data->input_buf,
					    frame_buf, frame_buf_capacity,
					    &frame_size, &stream_type, dec->num_streams);
		if (ret == MUX_ERROR_AGAIN)
			break;

		if (ret == MUX_ERROR_INVAL && frame_size > frame_buf_capacity) {
			uint8_t *new_buf = realloc(frame_buf, frame_size);
			if (!new_buf) {
				free(frame_buf);
				return MUX_ERROR_NOMEM;
			}
			frame_buf = new_buf;
			frame_buf_capacity = frame_size;

			ret = mux_leb128_read_frame(&data->input_buf,
						    frame_buf, frame_buf_capacity,
						    &frame_size, &stream_type, dec->num_streams);
		}

		if (ret != MUX_OK) {
			free(frame_buf);
			return ret;
		}

		if (stream_type == MUX_STREAM_AUDIO) {
			/* Convert mu-law to 16-bit PCM */
			pcm_out = malloc(frame_size * sizeof(int16_t));
			if (!pcm_out) {
				free(frame_buf);
				return MUX_ERROR_NOMEM;
			}

			for (i = 0; i < frame_size; i++) {
				pcm_out[i] = mulaw_decode_sample(frame_buf[i]);
			}

			ret = mux_buffer_write(&dec->audio_output,
					       pcm_out, frame_size * sizeof(int16_t));
			free(pcm_out);
		} else {
			ret = mux_buffer_write(&dec->side_output,
					       frame_buf, frame_size);
		}

		if (ret != MUX_OK) {
			free(frame_buf);
			return ret;
		}
	}

	free(frame_buf);
	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * Mu-law decoder read
 */
static int mulaw_decoder_read(struct mux_decoder *dec,
			      void *output,
			      size_t output_size,
			      size_t *output_written,
			      int *stream_type)
{
	size_t bytes_read;
	int ret;

	if (!dec || !output || !output_written || !stream_type)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&dec->audio_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_AUDIO;
		return MUX_OK;
	}

	ret = mux_buffer_read(&dec->side_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_SIDE_CHANNEL;
		return MUX_OK;
	}

	*output_written = 0;
	return MUX_ERROR_AGAIN;
}

/*
 * Mu-law decoder finalize
 */
static int mulaw_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * Mu-law supports telephony standard rates, but can handle any rate
 */
static const int mulaw_sample_rates[] = { 8000, 48000 };

/*
 * Mu-law codec operations
 */
const struct mux_codec_ops mux_codec_mulaw_ops = {
	.encoder_init = mulaw_encoder_init,
	.encoder_deinit = mulaw_encoder_deinit,
	.encoder_encode = mulaw_encoder_encode,
	.encoder_read = mulaw_encoder_read,
	.encoder_finalize = mulaw_encoder_finalize,

	.decoder_init = mulaw_decoder_init,
	.decoder_deinit = mulaw_decoder_deinit,
	.decoder_decode = mulaw_decoder_decode,
	.decoder_read = mulaw_decoder_read,
	.decoder_finalize = mulaw_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = mulaw_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
