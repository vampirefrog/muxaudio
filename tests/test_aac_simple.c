/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 1
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

int main(void)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	int16_t pcm_input[NUM_SAMPLES * NUM_CHANNELS];
	uint8_t muxed_buffer[128 * 1024];
	int16_t pcm_output[NUM_SAMPLES * NUM_CHANNELS * 2];
	size_t input_consumed, output_written, total_muxed = 0;
	size_t total_decoded = 0;
	int stream_type;
	int ret;

	printf("=== Simple AAC Encoder/Decoder Test ===\n\n");
	printf("Using AAC at %d Hz (128 kbps, lossy compression)\n\n", SAMPLE_RATE);

	/* Generate test signal: 440 Hz sine wave */
	for (int i = 0; i < NUM_SAMPLES; i++) {
		int16_t sample = (int16_t)(10000.0 * sin(2.0 * M_PI * 440.0 * i / SAMPLE_RATE));
		pcm_input[i * NUM_CHANNELS + 0] = sample;  /* Left */
		pcm_input[i * NUM_CHANNELS + 1] = sample;  /* Right */
	}

	/* Create encoder */
	printf("Creating AAC encoder...\n");
	struct mux_param params[] = {
		{ .name = "bitrate", .value.i = 128 }
	};
	enc = mux_encoder_new(MUX_CODEC_AAC, SAMPLE_RATE, NUM_CHANNELS, 2, params, 1);
	if (!enc) {
		fprintf(stderr, "Error: Failed to create AAC encoder\n");
		fprintf(stderr, "Make sure libfdk-aac is installed\n");
		return 1;
	}
	printf("Encoder created\n\n");

	/* Encode */
	printf("Encoding %zu bytes...\n", sizeof(pcm_input));
	ret = mux_encoder_encode(enc, pcm_input, sizeof(pcm_input),
				 &input_consumed, MUX_STREAM_AUDIO);
	printf("Encode returned: %d, consumed: %zu\n", ret, input_consumed);

	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Encode error: %s\n", err->message);
		mux_encoder_destroy(enc);
		return 1;
	}

	/* Finalize encoder */
	printf("Finalizing encoder...\n");
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Finalize error: %s\n", err->message);
		mux_encoder_destroy(enc);
		return 1;
	}

	/* Read encoded output */
	printf("Reading encoded output...\n");
	while (total_muxed < sizeof(muxed_buffer)) {
		ret = mux_encoder_read(enc, muxed_buffer + total_muxed,
				       sizeof(muxed_buffer) - total_muxed,
				       &output_written);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;
		total_muxed += output_written;
	}

	printf("Total encoded: %zu bytes (%.1f%% of original)\n\n",
	       total_muxed, total_muxed * 100.0 / sizeof(pcm_input));

	mux_encoder_destroy(enc);

	/* Create decoder */
	printf("Creating AAC decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_AAC, 2, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Error: Failed to create AAC decoder\n");
		return 1;
	}
	printf("Decoder created\n\n");

	/* Decode */
	printf("Decoding %zu bytes...\n", total_muxed);
	ret = mux_decoder_decode(dec, muxed_buffer, total_muxed, &input_consumed);
	printf("Decode returned: %d, consumed: %zu\n", ret, input_consumed);

	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Decode error: %s\n", err->message);
		mux_decoder_destroy(dec);
		return 1;
	}

	/* Finalize decoder */
	printf("Finalizing decoder...\n");
	ret = mux_decoder_finalize(dec);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Finalize error: %s\n", err->message);
	}

	/* Read decoded output */
	printf("Reading decoded output...\n");
	while (total_decoded < sizeof(pcm_output)) {
		ret = mux_decoder_read(dec, pcm_output + total_decoded / sizeof(int16_t),
				       sizeof(pcm_output) - total_decoded,
				       &output_written, &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;
		if (stream_type == MUX_STREAM_AUDIO)
			total_decoded += output_written;
	}

	printf("Total decoded: %zu bytes (%zu samples)\n\n",
	       total_decoded, total_decoded / sizeof(int16_t) / NUM_CHANNELS);

	mux_decoder_destroy(dec);

	if (total_decoded == 0) {
		fprintf(stderr, "Error: No audio was decoded\n");
		return 1;
	}

	printf("=== Test passed ===\n");
	return 0;
}
