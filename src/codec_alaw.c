/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A-law codec (G.711 A-law)
 *
 * Compresses 16-bit linear PCM to 8-bit A-law companded samples.
 * Standard for European telephony systems.
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * A-law codec state
 */
struct alaw_codec_data {
	struct mux_buffer input_buf;
};

/*
 * A-law encoding table: 16-bit linear PCM -> 8-bit A-law
 * Segment encoding: seeemmmm where s=sign, eee=exponent, mmmm=mantissa
 */
static uint8_t alaw_encode_sample(int16_t pcm)
{
	int sign;
	int exponent;
	int mantissa;
	uint8_t alaw;
	int sample;

	/* Get sign bit */
	sign = (pcm >> 8) & 0x80;
	if (sign)
		pcm = -pcm;

	/* Clamp to positive range */
	if (pcm > 32635)
		pcm = 32635;

	sample = pcm;

	/* Find exponent and mantissa */
	if (sample < 256) {
		exponent = 0;
		mantissa = sample >> 4;
	} else {
		/* Find the position of the highest bit */
		for (exponent = 1; exponent < 8; exponent++) {
			if (sample < (256 << exponent))
				break;
		}
		mantissa = (sample >> (exponent + 3)) & 0x0F;
	}

	/* Combine into A-law byte */
	alaw = sign | (exponent << 4) | mantissa;

	/* XOR with 0x55 (alternating pattern) for transmission optimization */
	return alaw ^ 0x55;
}

/*
 * A-law decoding: 8-bit A-law -> 16-bit linear PCM
 */
static int16_t alaw_decode_sample(uint8_t alaw)
{
	int sign;
	int exponent;
	int mantissa;
	int sample;

	/* Remove XOR mask */
	alaw ^= 0x55;

	/* Extract fields */
	sign = alaw & 0x80;
	exponent = (alaw >> 4) & 0x07;
	mantissa = alaw & 0x0F;

	/* Reconstruct sample */
	if (exponent == 0) {
		sample = (mantissa << 4) + 8;
	} else {
		sample = ((mantissa << 4) + 264) << (exponent - 1);
	}

	return sign ? -sample : sample;
}

/*
 * A-law encoder initialization
 */
static int alaw_encoder_init(struct mux_encoder *enc,
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
 * A-law encoder deinitialization
 */
static void alaw_encoder_deinit(struct mux_encoder *enc)
{
	(void)enc;
}

/*
 * A-law encoder encode
 * Converts 16-bit PCM samples to 8-bit A-law
 */
static int alaw_encoder_encode(struct mux_encoder *enc,
			       const void *input,
			       size_t input_size,
			       size_t *input_consumed,
			       int stream_type)
{
	const int16_t *pcm_in;
	uint8_t *alaw_out;
	size_t num_samples;
	size_t i;
	int ret;

	if (!enc || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	if (input_size == 0) {
		*input_consumed = 0;
		return MUX_OK;
	}

	/* For audio stream: convert PCM to A-law */
	if (stream_type == MUX_STREAM_AUDIO) {
		/* Input is 16-bit samples */
		num_samples = input_size / sizeof(int16_t);
		if (num_samples == 0) {
			*input_consumed = 0;
			return MUX_OK;
		}

		alaw_out = malloc(num_samples);
		if (!alaw_out)
			return MUX_ERROR_NOMEM;

		pcm_in = (const int16_t *)input;
		for (i = 0; i < num_samples; i++) {
			alaw_out[i] = alaw_encode_sample(pcm_in[i]);
		}

		/* Write LEB128-framed packet */
		ret = mux_leb128_write_frame(&enc->output, alaw_out, num_samples,
					     stream_type, enc->num_streams);
		free(alaw_out);

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
 * A-law encoder read
 */
static int alaw_encoder_read(struct mux_encoder *enc,
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
 * A-law encoder finalize
 */
static int alaw_encoder_finalize(struct mux_encoder *enc)
{
	(void)enc;
	return MUX_OK;
}

/*
 * A-law decoder initialization
 */
static int alaw_decoder_init(struct mux_decoder *dec,
			     const struct mux_param *params,
			     int num_params)
{
	struct alaw_codec_data *data;

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
 * A-law decoder deinitialization
 */
static void alaw_decoder_deinit(struct mux_decoder *dec)
{
	struct alaw_codec_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;
	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * A-law decoder decode
 */
static int alaw_decoder_decode(struct mux_decoder *dec,
			       const void *input,
			       size_t input_size,
			       size_t *input_consumed)
{
	struct alaw_codec_data *data;
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
			/* Convert A-law to 16-bit PCM */
			pcm_out = malloc(frame_size * sizeof(int16_t));
			if (!pcm_out) {
				free(frame_buf);
				return MUX_ERROR_NOMEM;
			}

			for (i = 0; i < frame_size; i++) {
				pcm_out[i] = alaw_decode_sample(frame_buf[i]);
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
 * A-law decoder read
 */
static int alaw_decoder_read(struct mux_decoder *dec,
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
 * A-law decoder finalize
 */
static int alaw_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * A-law supports telephony standard rates, but can handle any rate
 */
static const int alaw_sample_rates[] = { 8000, 48000 };

/*
 * A-law codec operations
 */
const struct mux_codec_ops mux_codec_alaw_ops = {
	.encoder_init = alaw_encoder_init,
	.encoder_deinit = alaw_encoder_deinit,
	.encoder_encode = alaw_encoder_encode,
	.encoder_read = alaw_encoder_read,
	.encoder_finalize = alaw_encoder_finalize,

	.decoder_init = alaw_decoder_init,
	.decoder_deinit = alaw_decoder_deinit,
	.decoder_decode = alaw_decoder_decode,
	.decoder_read = alaw_decoder_read,
	.decoder_finalize = alaw_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = alaw_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
