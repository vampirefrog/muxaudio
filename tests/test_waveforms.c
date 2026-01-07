/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 1
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

static int test_waveform_pcm(const char *name, int16_t *test_signal)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	uint8_t *muxed_buffer;
	int16_t *decoded_audio;
	size_t muxed_capacity = 512 * 1024;
	size_t decoded_capacity = NUM_SAMPLES * NUM_CHANNELS * 2;
	size_t input_consumed, output_written, total_muxed = 0;
	size_t total_decoded = 0;
	int stream_type;
	int ret;

	printf("\n=== Testing %s ===\n", name);

	/* Allocate buffers */
	muxed_buffer = malloc(muxed_capacity);
	decoded_audio = malloc(decoded_capacity * sizeof(int16_t));

	if (!muxed_buffer || !decoded_audio) {
		fprintf(stderr, "Failed to allocate buffers\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Create PCM encoder */
	enc = mux_encoder_new(MUX_CODEC_PCM, SAMPLE_RATE, NUM_CHANNELS, NULL, 0);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Encode */
	size_t input_bytes = NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t);
	ret = mux_encoder_encode(enc, test_signal, input_bytes,
				 &input_consumed, MUX_STREAM_AUDIO);
	if (ret != MUX_OK) {
		fprintf(stderr, "Encode failed\n");
		mux_encoder_destroy(enc);
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Read muxed output */
	while (total_muxed < muxed_capacity) {
		ret = mux_encoder_read(enc, muxed_buffer + total_muxed,
				       muxed_capacity - total_muxed,
				       &output_written);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;
		total_muxed += output_written;
	}

	printf("Encoded: %zu bytes → %zu bytes (with LEB128 framing)\n",
	       input_bytes, total_muxed);

	mux_encoder_destroy(enc);

	/* Create PCM decoder */
	dec = mux_decoder_new(MUX_CODEC_PCM, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Decode */
	ret = mux_decoder_decode(dec, muxed_buffer, total_muxed, &input_consumed);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Decode failed: %s (code %d)\n", err->message, ret);
		mux_decoder_destroy(dec);
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Read decoded output */
	while (total_decoded < decoded_capacity) {
		ret = mux_decoder_read(dec, decoded_audio + total_decoded,
				       (decoded_capacity - total_decoded) * sizeof(int16_t),
				       &output_written, &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;

		if (stream_type == MUX_STREAM_AUDIO)
			total_decoded += output_written / sizeof(int16_t);
	}

	printf("Decoded: %zu samples\n", total_decoded / NUM_CHANNELS);

	mux_decoder_destroy(dec);

	/* Debug: Check for exact match */
	size_t expected_elements = NUM_SAMPLES * NUM_CHANNELS;
	if (total_decoded == expected_elements) {
		int diff = memcmp(test_signal, decoded_audio,
				  expected_elements * sizeof(int16_t));
		if (diff != 0) {
			printf("\nWarning: Decoded data differs from input!\n");
			/* Find first difference */
			for (size_t i = 0; i < expected_elements; i++) {
				if (test_signal[i] != decoded_audio[i]) {
					printf("  First diff at element %zu: "
					       "orig=%d decoded=%d\n",
					       i, test_signal[i], decoded_audio[i]);
					break;
				}
			}
		} else {
			printf("\nDebug: Buffers match perfectly\n");
		}
	} else {
		printf("\nWarning: Element count mismatch: expected %zu, got %zu\n",
		       expected_elements, total_decoded);
	}

	/* Validate (PCM is lossless, so SNR should be perfect) */
	printf("\nValidation:\n");
	ret = validate_lossy_audio(test_signal, NUM_SAMPLES,
				   decoded_audio, total_decoded / NUM_CHANNELS,
				   NUM_CHANNELS,
				   90.0f,  /* Expect perfect or near-perfect SNR */
				   16);    /* Small time offset tolerance */

	free(muxed_buffer);
	free(decoded_audio);

	return ret;
}

int main(void)
{
	int16_t *test_signal;
	int16_t *temp;
	int failed = 0;

	printf("=== muxaudio Waveform Generator Test ===\n");
	printf("Sample rate: %d Hz\n", SAMPLE_RATE);
	printf("Channels: %d\n", NUM_CHANNELS);
	printf("Duration: %d second\n", DURATION_SEC);
	printf("Codec: PCM (lossless)\n\n");

	/* Allocate test signal buffer */
	test_signal = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	temp = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));

	if (!test_signal || !temp) {
		fprintf(stderr, "Failed to allocate buffers\n");
		return 1;
	}

	/*
	 * Test all waveform generators
	 */

	/* Sine wave */
	generate_sine(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 440.0f, 0.7f);
	if (test_waveform_pcm("440 Hz Sine Wave", test_signal) != 0)
		failed++;

	/* Sinc pulse */
	generate_sinc(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 4000.0f, 0.5f);
	if (test_waveform_pcm("Sinc Pulse (4kHz cutoff)", test_signal) != 0)
		failed++;

	/* Square wave */
	generate_square(test_signal, NUM_SAMPLES, NUM_CHANNELS,
			SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform_pcm("440 Hz Square Wave", test_signal) != 0)
		failed++;

	/* Triangle wave */
	generate_triangle(test_signal, NUM_SAMPLES, NUM_CHANNELS,
			  SAMPLE_RATE, 440.0f, 0.6f);
	if (test_waveform_pcm("440 Hz Triangle Wave", test_signal) != 0)
		failed++;

	/* Sawtooth wave */
	generate_sawtooth(test_signal, NUM_SAMPLES, NUM_CHANNELS,
			  SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform_pcm("440 Hz Sawtooth Wave", test_signal) != 0)
		failed++;

	/* White noise */
	generate_noise(test_signal, NUM_SAMPLES, NUM_CHANNELS, 0.3f);
	if (test_waveform_pcm("White Noise", test_signal) != 0)
		failed++;

	/* Chirp */
	generate_chirp(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		       SAMPLE_RATE, 100.0f, 8000.0f, 0.5f);
	if (test_waveform_pcm("Chirp 100Hz-8kHz", test_signal) != 0)
		failed++;

	/* Multi-tone (C Major Chord) */
	generate_silence(test_signal, NUM_SAMPLES, NUM_CHANNELS);
	generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 261.63f, 0.25f);  /* C4 */
	for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
		test_signal[i] += temp[i];

	generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 329.63f, 0.25f);  /* E4 */
	for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
		test_signal[i] += temp[i];

	generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 392.00f, 0.25f);  /* G4 */
	for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
		test_signal[i] += temp[i];

	if (test_waveform_pcm("C Major Chord (C-E-G)", test_signal) != 0)
		failed++;

	free(test_signal);
	free(temp);

	printf("\n========================================\n");
	if (failed == 0) {
		printf("✓ All waveform tests passed!\n");
		printf("\nWaveform generators validated:\n");
		printf("  - Sine wave\n");
		printf("  - Sinc pulse (band-limited)\n");
		printf("  - Square wave\n");
		printf("  - Triangle wave\n");
		printf("  - Sawtooth wave\n");
		printf("  - White noise\n");
		printf("  - Chirp (frequency sweep)\n");
		printf("  - Multi-tone combinations\n");
		return 0;
	} else {
		printf("✗ %d test(s) failed\n", failed);
		return 1;
	}
}
