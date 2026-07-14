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
 * A-law decoder state: just the streaming demux parser (no buffers).
 */
struct alaw_decoder_data {
	struct mux_leb128_parser parser;
};

/*
 * A-law encoding table: 16-bit linear PCM -> 8-bit A-law
 * Segment encoding: seeemmmm where s=sign, eee=exponent, mmmm=mantissa
 */
static uint8_t alaw_encode_sample(int16_t pcm) {
	int sign;
	int exponent;
	int mantissa;
	uint8_t alaw;
	int sample;

	/* Get sign bit */
	sign = (pcm >> 8) & 0x80;
	if(sign)
		pcm = -pcm;

	/* Clamp to positive range */
	if(pcm > 32635)
		pcm = 32635;

	sample = pcm;

	/* Find exponent and mantissa */
	if(sample < 256) {
		exponent = 0;
		mantissa = sample >> 4;
	} else {
		/* Find the position of the highest bit */
		for(exponent = 1; exponent < 8; exponent++) {
			if(sample < (256 << exponent))
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
static int16_t alaw_decode_sample(uint8_t alaw) {
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
	if(exponent == 0) {
		sample = (mantissa << 4) + 8;
	} else {
		sample = ((mantissa << 4) + 264) << (exponent - 1);
	}

	return sign ? -sample : sample;
}

/*
 * A-law encoder initialization
 */
static int alaw_encoder_init(
	struct mux_encoder *enc,
	int sample_rate,
	int num_channels,
	const struct mux_param *params,
	int num_params
) {
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
static void alaw_encoder_deinit(struct mux_encoder *enc) { (void)enc; }

/*
 * A-law encoder encode
 * Converts 16-bit PCM samples to 8-bit A-law, streamed through a fixed stack
 * buffer with no heap allocation.
 */
static int alaw_encoder_encode(
	struct mux_encoder *enc,
	const void *input,
	size_t input_size,
	int stream_type
) {
	const int16_t *pcm_in;
	size_t num_samples;
	size_t off;
	int ret;

	if(!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	if(input_size == 0)
		return MUX_OK;

	/* Side channel: pass through unchanged */
	if(stream_type != MUX_STREAM_AUDIO) {
		return mux_leb128_emit_frame(
			input,
			input_size,
			stream_type,
			enc->num_streams,
			enc->sink,
			enc->sink_user
		);
	}

	/* Audio: one A-law byte per 16-bit sample. Payload size is known up
	 * front, so emit the frame header, then convert and emit in chunks. */
	num_samples = input_size / sizeof(int16_t);
	if(num_samples == 0)
		return MUX_OK;

	ret = mux_leb128_emit_header(
		num_samples,
		MUX_STREAM_AUDIO,
		enc->num_streams,
		enc->sink,
		enc->sink_user
	);
	if(ret)
		return ret;

	pcm_in = input;
	for(off = 0; off < num_samples;) {
		uint8_t alaw[4096];
		size_t take = num_samples - off;
		size_t i;

		if(take > sizeof(alaw))
			take = sizeof(alaw);

		for(i = 0; i < take; i++)
			alaw[i] = alaw_encode_sample(pcm_in[off + i]);

		ret = enc->sink(enc->sink_user, alaw, take);
		if(ret)
			return ret;

		off += take;
	}

	return MUX_OK;
}

/*
 * A-law encoder finalize
 */
static int alaw_encoder_finalize(struct mux_encoder *enc) {
	(void)enc;
	return MUX_OK;
}

/*
 * A-law decoder initialization
 */
static int
alaw_decoder_init(struct mux_decoder *dec, const struct mux_param *params, int num_params) {
	struct alaw_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if(!data)
		return MUX_ERROR_NOMEM;

	mux_leb128_parser_init(&data->parser);
	dec->codec_data = data;
	return MUX_OK;
}

/*
 * A-law decoder deinitialization
 */
static void alaw_decoder_deinit(struct mux_decoder *dec) {
	if(!dec)
		return;
	free(dec->codec_data);
	dec->codec_data = NULL;
}

/*
 * Parser emit shim: audio payload is A-law bytes -> convert to 16-bit PCM
 * through a fixed stack buffer; side channel passes through unchanged.
 */
static int alaw_parser_emit(void *user, int stream_type, const void *data, size_t size) {
	struct mux_decoder *dec = user;
	const uint8_t *in = data;
	size_t off;
	int ret;

	if(stream_type != MUX_STREAM_AUDIO)
		return mux_decoder_emit(dec, stream_type, data, size);

	for(off = 0; off < size;) {
		int16_t pcm[2048];
		size_t take = size - off;
		size_t i;

		if(take > 2048)
			take = 2048;

		for(i = 0; i < take; i++)
			pcm[i] = alaw_decode_sample(in[off + i]);

		ret = mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm, take * sizeof(int16_t));
		if(ret)
			return ret;

		off += take;
	}

	return MUX_OK;
}

/*
 * A-law decoder decode
 */
static int alaw_decoder_decode(struct mux_decoder *dec, const void *input, size_t input_size) {
	struct alaw_decoder_data *data;

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
		alaw_parser_emit,
		dec
	);
}

/*
 * A-law decoder finalize
 */
static int alaw_decoder_finalize(struct mux_decoder *dec) {
	(void)dec;
	return MUX_OK;
}

/*
 * A-law supports telephony standard rates, but can handle any rate
 */
static const int alaw_sample_rates[] = {8000, 48000};

/*
 * A-law codec operations
 */
static const char *const alaw_file_extensions[] = { "al", "alaw" };

const struct mux_codec_ops mux_codec_alaw_ops = {
	.encoder_init = alaw_encoder_init,
	.encoder_deinit = alaw_encoder_deinit,
	.encoder_encode = alaw_encoder_encode,
	.encoder_finalize = alaw_encoder_finalize,

	.decoder_init = alaw_decoder_init,
	.decoder_deinit = alaw_decoder_deinit,
	.decoder_decode = alaw_decoder_decode,
	.decoder_finalize = alaw_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = alaw_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1,
	.file_extensions = alaw_file_extensions,
	.file_extension_count = sizeof(alaw_file_extensions) / sizeof(alaw_file_extensions[0])
};
