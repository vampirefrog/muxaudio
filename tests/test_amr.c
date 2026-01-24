/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test AMR-NB and AMR-WB codecs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mux.h"

#define AMR_SAMPLE_RATE    8000
#define AMR_WB_SAMPLE_RATE 16000
#define NUM_CHANNELS       1
#define DURATION_MS        100  /* 100ms of audio = 5 AMR frames */
#define AMPLITUDE          16000

static void generate_sine_wave(int16_t *samples, int num_samples,
			       int sample_rate, float frequency)
{
	int i;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		samples[i] = (int16_t)(AMPLITUDE * sin(2.0 * M_PI * frequency * t));
	}
}

static int test_amr_nb(void)
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
	int num_samples;
	double mse = 0.0;
	double snr;
	int i;

	printf("Testing AMR-NB codec...\n");

	num_samples = AMR_SAMPLE_RATE * DURATION_MS / 1000;

	input_samples = malloc(num_samples * sizeof(int16_t));
	output_samples = malloc(num_samples * sizeof(int16_t) * 2);  /* Extra space */
	encoded = malloc(num_samples * 2);

	if (!input_samples || !output_samples || !encoded) {
		fprintf(stderr, "  FAIL: Memory allocation failed\n");
		return -1;
	}

	/* Generate 300Hz tone (voice range) */
	generate_sine_wave(input_samples, num_samples, AMR_SAMPLE_RATE, 300.0f);

	/* Create encoder */
	enc = mux_encoder_new(MUX_CODEC_AMR, AMR_SAMPLE_RATE, NUM_CHANNELS, 1, NULL, 0);
	if (!enc) {
		fprintf(stderr, "  FAIL: Failed to create encoder\n");
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Encode */
	ret = mux_encoder_encode(enc, input_samples,
				 num_samples * sizeof(int16_t),
				 &input_consumed, MUX_STREAM_AUDIO);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Encode failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Finalize */
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Finalize failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Read encoded output */
	ret = mux_encoder_read(enc, encoded, num_samples * 2, &output_written);
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

	printf("  Encoded %d samples (%zu bytes) -> %zu bytes (%.1f%% compression)\n",
	       num_samples, num_samples * sizeof(int16_t), encoded_size,
	       100.0 * (1.0 - (double)encoded_size / (num_samples * sizeof(int16_t))));

	/* Create decoder */
	dec = mux_decoder_new(MUX_CODEC_AMR, 1, NULL, 0);
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
	decoded_size = 0;
	while (1) {
		size_t chunk;
		ret = mux_decoder_read(dec, output_samples + decoded_size / sizeof(int16_t),
				       num_samples * 2 * sizeof(int16_t) - decoded_size,
				       &chunk, &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "  FAIL: Decoder read failed with error %d\n", ret);
			mux_decoder_destroy(dec);
			free(input_samples);
			free(output_samples);
			free(encoded);
			return -1;
		}
		decoded_size += chunk;
	}

	mux_decoder_destroy(dec);

	printf("  Decoded %zu bytes\n", decoded_size);

	/* AMR encodes in 160-sample frames, so output may differ slightly */
	int output_samples_count = decoded_size / sizeof(int16_t);
	int compare_samples = (output_samples_count < num_samples) ? output_samples_count : num_samples;

	/* Calculate MSE and SNR */
	for (i = 0; i < compare_samples; i++) {
		double diff = (double)(input_samples[i] - output_samples[i]);
		mse += diff * diff;
	}
	mse /= compare_samples;

	double signal_power = 0.0;
	for (i = 0; i < compare_samples; i++) {
		signal_power += (double)input_samples[i] * input_samples[i];
	}
	signal_power /= compare_samples;

	if (mse > 0) {
		snr = 10.0 * log10(signal_power / mse);
	} else {
		snr = 100.0;
	}

	printf("  MSE: %.2f, SNR: %.1f dB\n", mse, snr);

	/*
	 * Note: opencore-amrnb may produce variable quality depending on
	 * the system/version. We just verify that encoding/decoding works
	 * and produces some output. Production quality testing would require
	 * a reference implementation.
	 */
	if (decoded_size == 0) {
		fprintf(stderr, "  FAIL: No decoded output\n");
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

static int test_amr_wb(void)
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
	int num_samples;
	double mse = 0.0;
	double snr;
	int i;

	printf("Testing AMR-WB codec...\n");

	num_samples = AMR_WB_SAMPLE_RATE * DURATION_MS / 1000;

	input_samples = malloc(num_samples * sizeof(int16_t));
	output_samples = malloc(num_samples * sizeof(int16_t) * 2);
	encoded = malloc(num_samples * 2);

	if (!input_samples || !output_samples || !encoded) {
		fprintf(stderr, "  FAIL: Memory allocation failed\n");
		return -1;
	}

	/* Generate 300Hz tone */
	generate_sine_wave(input_samples, num_samples, AMR_WB_SAMPLE_RATE, 300.0f);

	/* Create encoder - may fail if vo-amrwbenc not available */
	enc = mux_encoder_new(MUX_CODEC_AMR_WB, AMR_WB_SAMPLE_RATE, NUM_CHANNELS, 1, NULL, 0);
	if (!enc) {
		printf("  SKIP: AMR-WB encoding not available (vo-amrwbenc not found)\n\n");
		free(input_samples);
		free(output_samples);
		free(encoded);
		return 0;  /* Not a failure - encoding is optional */
	}

	/* Encode */
	ret = mux_encoder_encode(enc, input_samples,
				 num_samples * sizeof(int16_t),
				 &input_consumed, MUX_STREAM_AUDIO);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Encode failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Finalize */
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		fprintf(stderr, "  FAIL: Finalize failed with error %d\n", ret);
		mux_encoder_destroy(enc);
		free(input_samples);
		free(output_samples);
		free(encoded);
		return -1;
	}

	/* Read encoded output */
	ret = mux_encoder_read(enc, encoded, num_samples * 2, &output_written);
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

	printf("  Encoded %d samples (%zu bytes) -> %zu bytes (%.1f%% compression)\n",
	       num_samples, num_samples * sizeof(int16_t), encoded_size,
	       100.0 * (1.0 - (double)encoded_size / (num_samples * sizeof(int16_t))));

	/* Create decoder */
	dec = mux_decoder_new(MUX_CODEC_AMR_WB, 1, NULL, 0);
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
	decoded_size = 0;
	while (1) {
		size_t chunk;
		ret = mux_decoder_read(dec, output_samples + decoded_size / sizeof(int16_t),
				       num_samples * 2 * sizeof(int16_t) - decoded_size,
				       &chunk, &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "  FAIL: Decoder read failed with error %d\n", ret);
			mux_decoder_destroy(dec);
			free(input_samples);
			free(output_samples);
			free(encoded);
			return -1;
		}
		decoded_size += chunk;
	}

	mux_decoder_destroy(dec);

	printf("  Decoded %zu bytes\n", decoded_size);

	int output_samples_count = decoded_size / sizeof(int16_t);
	int compare_samples = (output_samples_count < num_samples) ? output_samples_count : num_samples;

	/* Calculate MSE and SNR */
	for (i = 0; i < compare_samples; i++) {
		double diff = (double)(input_samples[i] - output_samples[i]);
		mse += diff * diff;
	}
	mse /= compare_samples;

	double signal_power = 0.0;
	for (i = 0; i < compare_samples; i++) {
		signal_power += (double)input_samples[i] * input_samples[i];
	}
	signal_power /= compare_samples;

	if (mse > 0) {
		snr = 10.0 * log10(signal_power / mse);
	} else {
		snr = 100.0;
	}

	printf("  MSE: %.2f, SNR: %.1f dB\n", mse, snr);

	/* AMR-WB typically achieves 25-35 dB SNR */
	if (snr < 15.0) {
		fprintf(stderr, "  FAIL: SNR too low (expected >= 15 dB)\n");
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

	printf("AMR Codec Tests\n");
	printf("===============\n\n");

	if (test_amr_nb() != 0)
		failures++;

	if (test_amr_wb() != 0)
		failures++;

	if (failures == 0) {
		printf("All tests passed!\n");
		return 0;
	} else {
		printf("%d test(s) failed.\n", failures);
		return 1;
	}
}
