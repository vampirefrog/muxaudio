/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * PCM codec
 *
 * PCM passes raw samples through. Muxing is handled entirely by the streaming
 * LEB128 framing layer, so the encoder is stateless and the decoder holds only
 * the small integer parser state - no buffers, no per-call allocation.
 */

struct pcm_decoder_data {
	struct mux_leb128_parser parser;
};

/*
 * Encoder: stateless
 */
static int pcm_encoder_init(
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

static void pcm_encoder_deinit(struct mux_encoder *enc) { (void)enc; }

static int
pcm_encoder_encode(struct mux_encoder *enc, const void *input, size_t input_size, int stream_type) {
	if(!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	if(input_size == 0)
		return MUX_OK;

	return mux_leb128_emit_frame(
		input,
		input_size,
		stream_type,
		enc->num_streams,
		enc->sink,
		enc->sink_user
	);
}

static int pcm_encoder_finalize(struct mux_encoder *enc) {
	(void)enc; /* nothing buffered */
	return MUX_OK;
}

/*
 * Decoder: streaming demux parser
 */
static int
pcm_decoder_init(struct mux_decoder *dec, const struct mux_param *params, int num_params) {
	struct pcm_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if(!data)
		return MUX_ERROR_NOMEM;

	mux_leb128_parser_init(&data->parser);
	dec->codec_data = data;
	return MUX_OK;
}

static void pcm_decoder_deinit(struct mux_decoder *dec) {
	if(!dec)
		return;
	free(dec->codec_data);
	dec->codec_data = NULL;
}

static int pcm_decoder_decode(struct mux_decoder *dec, const void *input, size_t input_size) {
	struct pcm_decoder_data *data;

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
		dec->emit,
		dec->emit_user
	);
}

static int pcm_decoder_finalize(struct mux_decoder *dec) {
	(void)dec; /* nothing buffered */
	return MUX_OK;
}

/*
 * PCM sample rate constraints (supports any rate)
 */
static const int pcm_sample_rates[] = {1000, 384000}; /* Min/max range */

static const char *const pcm_file_extensions[] = { "pcm", "raw" };

const struct mux_codec_ops mux_codec_pcm_ops = {
	.encoder_init = pcm_encoder_init,
	.encoder_deinit = pcm_encoder_deinit,
	.encoder_encode = pcm_encoder_encode,
	.encoder_finalize = pcm_encoder_finalize,

	.decoder_init = pcm_decoder_init,
	.decoder_deinit = pcm_decoder_deinit,
	.decoder_decode = pcm_decoder_decode,
	.decoder_finalize = pcm_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = pcm_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1,
	.file_extensions = pcm_file_extensions,
	.file_extension_count = sizeof(pcm_file_extensions) / sizeof(pcm_file_extensions[0])
};
