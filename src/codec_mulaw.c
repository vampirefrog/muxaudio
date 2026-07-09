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
 * Mu-law decoder state: just the streaming demux parser (no buffers).
 */
struct mulaw_decoder_data {
	struct mux_leb128_parser parser;
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
				int stream_type)
{
	const int16_t *pcm_in;
	size_t num_samples;
	size_t off;
	int ret;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	if (input_size == 0)
		return MUX_OK;

	/* Side channel: pass through unchanged */
	if (stream_type != MUX_STREAM_AUDIO) {
		return mux_leb128_emit_frame(input, input_size, stream_type,
					     enc->num_streams,
					     enc->sink, enc->sink_user);
	}

	/* Audio: one mu-law byte per 16-bit sample. Payload size is known up
	 * front, so emit the frame header, then convert and emit in chunks. */
	num_samples = input_size / sizeof(int16_t);
	if (num_samples == 0)
		return MUX_OK;

	ret = mux_leb128_emit_header(num_samples, MUX_STREAM_AUDIO,
				     enc->num_streams, enc->sink, enc->sink_user);
	if (ret)
		return ret;

	pcm_in = input;
	for (off = 0; off < num_samples; ) {
		uint8_t mulaw[4096];
		size_t take = num_samples - off;
		size_t i;

		if (take > sizeof(mulaw))
			take = sizeof(mulaw);

		for (i = 0; i < take; i++)
			mulaw[i] = mulaw_encode_sample(pcm_in[off + i]);

		ret = enc->sink(enc->sink_user, mulaw, take);
		if (ret)
			return ret;

		off += take;
	}

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
	struct mulaw_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	mux_leb128_parser_init(&data->parser);
	dec->codec_data = data;
	return MUX_OK;
}

/*
 * Mu-law decoder deinitialization
 */
static void mulaw_decoder_deinit(struct mux_decoder *dec)
{
	if (!dec)
		return;
	free(dec->codec_data);
	dec->codec_data = NULL;
}

/*
 * Parser emit shim: audio payload is mu-law bytes -> convert to 16-bit PCM
 * through a fixed stack buffer; side channel passes through unchanged.
 */
static int mulaw_parser_emit(void *user, int stream_type, const void *data,
			     size_t size, int flags)
{
	struct mux_decoder *dec = user;
	const uint8_t *in = data;
	size_t off;
	int ret;

	if (stream_type != MUX_STREAM_AUDIO)
		return mux_decoder_emit(dec, stream_type, data, size, flags);

	if (size == 0)
		return mux_decoder_emit(dec, MUX_STREAM_AUDIO, data, 0, flags);

	for (off = 0; off < size; ) {
		int16_t pcm[2048];
		size_t take = size - off;
		size_t i;
		int last;

		if (take > 2048)
			take = 2048;

		for (i = 0; i < take; i++)
			pcm[i] = mulaw_decode_sample(in[off + i]);

		last = (off + take == size);
		ret = mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm,
				       take * sizeof(int16_t),
				       last ? flags : 0);
		if (ret)
			return ret;

		off += take;
	}

	return MUX_OK;
}

/*
 * Mu-law decoder decode
 */
static int mulaw_decoder_decode(struct mux_decoder *dec,
				const void *input,
				size_t input_size)
{
	struct mulaw_decoder_data *data;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(&data->parser, input, input_size,
				      dec->num_streams, mulaw_parser_emit, dec);
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
	.encoder_finalize = mulaw_encoder_finalize,

	.decoder_init = mulaw_decoder_init,
	.decoder_deinit = mulaw_decoder_deinit,
	.decoder_decode = mulaw_decoder_decode,
	.decoder_finalize = mulaw_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = mulaw_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
