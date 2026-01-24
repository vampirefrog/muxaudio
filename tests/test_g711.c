/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test G.711 ALAW and MULAW codecs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mux.h"

#define SAMPLE_RATE 8000
#define NUM_CHANNELS 1
#define NUM_SAMPLES 1000
#define AMPLITUDE 16000

static void generate_sine_wave(int16_t *samples, int num_samples,
			       int sample_rate, float frequency)
{
	int i;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		samples[i] = (int16_t)(AMPLITUDE * sin(2.0 * M_PI * frequency * t));
	}
}

static int test_codec(enum mux_codec_type codec_type, const char *codec_name)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	int16_t *input_samples;
	int16_t *output_samples;
	uint8_t *encoded;
	size_t input_consumed, output_written;
	size_t encoded_size = 0;
	size_t decoded_size = 0;
	int stream_type;
	int ret;
	int i;
	double mse = 0.0;
	double snr;

	printf("Testing %s codec...\n", codec_name);

	/* Allocate buffers */
	input_samples = malloc(NUM_SAMPLES * sizeof(int16_t));
	output_samples = malloc(NUM_SAMPLES * sizeof(int16_t));
	encoded = malloc(NUM_SAMPLES * 2 + 100);  /* Plenty of room */

	if (!input_samples || !output_samples || !encoded) {
		fprintf(stderr, "  FAIL: Memory allocation failed\n");
		return -1;
	}

	/* Generate test signal */
	generate_sine_wave(input_samples, NUM_SAMPLES, SAMPLE_RATE, 440.0f);

	/* Create encoder */
	enc = mux_encoder_new(codec_type, SAMPLE_RATE, NUM_CHANNELS, 1, NULL, 0);
	if (!enc) {
		fprintf(stderr, "  FAIL: Failed to create encoder\n");
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Encode */
	ret = mux_encoder_encode(enc, input_samples,
				 NUM_SAMPLES * sizeof(int16_t),
				 &input_consumed, MUX_STREAM_AUDIO);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Encode failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Read encoded output */
	ret = mux_encoder_read(enc, encoded, NUM_SAMPLES * 2 + 100, &output_written);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Encoder read failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}
	encoded_size = output_written;

	mux_encoder_destroy(enc);

	printf("  Encoded %zu bytes -> %zu bytes (%.1f%% compression)\n",
	       NUM_SAMPLES * sizeof(int16_t), encoded_size,
	       100.0 * (1.0 - (double)encoded_size / (NUM_SAMPLES * sizeof(int16_t))));

	/* Create decoder */
	dec = mux_decoder_new(codec_type, 1, NULL, 0);
	if (!dec) {
		fprintf(stderr, "  FAIL: Failed to create decoder\n");
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Decode */
	ret = mux_decoder_decode(dec, encoded, encoded_size, &input_consumed);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Decode failed with error %d\n", ret);
		mux_decoder_destroy(dec);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Read decoded output */
	ret = mux_decoder_read(dec, output_samples,
			       NUM_SAMPLES * sizeof(int16_t),
			       &output_written, &stream_type);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Decoder read failed with error %d\n", ret);
		mux_decoder_destroy(dec);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}
	decoded_size = output_written;

	mux_decoder_destroy(dec);

	printf("  Decoded %zu bytes\n", decoded_size);

	if (decoded_size != NUM_SAMPLES * sizeof(int16_t)) {
		fprintf(stderr, "  FAIL: Output size mismatch: expected %zu, got %zu\n",
			NUM_SAMPLES * sizeof(int16_t), decoded_size);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Calculate MSE and SNR */
	for (i = 0; i < NUM_SAMPLES; i++) {
		double diff = (double)(input_samples[i] - output_samples[i]);
		mse += diff * diff;
	}
	mse /= NUM_SAMPLES;

	/* Calculate signal power for SNR */
	double signal_power = 0.0;
	for (i = 0; i < NUM_SAMPLES; i++) {
		signal_power += (double)input_samples[i] * input_samples[i];
	}
	signal_power /= NUM_SAMPLES;

	if (mse > 0) {
		snr = 10.0 * log10(signal_power / mse);
	} else {
		snr = 100.0;  /* Effectively infinite */
	}

	printf("  MSE: %.2f, SNR: %.1f dB\n", mse, snr);

	/* G.711 codecs typically have SNR around 38 dB for a sine wave */
	if (snr < 30.0) {
		fprintf(stderr, "  FAIL: SNR too low (expected >= 30 dB)\n");
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	printf("  PASS\n\n");

	free(input_samples);
	free(output_samples);
	free(encoded);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("G.711 Codec Tests\n");
	printf("=================\n\n");

	/* Test ALAW */
	if (test_codec(MUX_CODEC_ALAW, "A-law") != 0)
		failures++;

	/* Test MULAW */
	if (test_codec(MUX_CODEC_MULAW, "mu-law") != 0)
		failures++;

	if (failures == 0) {
		printf("All tests passed!\n");
		return 0;
	} else {
		printf("%d test(s) failed.\n", failures);
		return 1;
	}
}
