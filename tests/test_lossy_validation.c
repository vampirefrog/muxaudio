/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include "test_utils.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 2
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

static int test_waveform(const char *name, int16_t *test_signal, float min_snr_db, int max_offset) {
	struct mux_param params[] = {
		{.name = "bitrate", .value.i = 128},
		{.name = "quality", .value.i = 5}
	};
	struct th_buf muxed;
	struct th_out out;
	size_t input_bytes = (size_t)NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t);
	int ret;

	printf("\n=== Testing %s ===\n", name);

	if(th_encode(
		   MUX_CODEC_MP3,
		   SAMPLE_RATE,
		   NUM_CHANNELS,
		   2,
		   params,
		   2,
		   test_signal,
		   input_bytes,
		   NULL,
		   0,
		   &muxed
	   ) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return -1;
	}
	printf(
		"Encoded: %zu -> %zu bytes (%.1f%%)\n",
		input_bytes,
		muxed.len,
		muxed.len * 100.0 / input_bytes
	);

	if(th_decode(MUX_CODEC_MP3, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		th_buf_free(&muxed);
		return -1;
	}
	printf("Decoded: %zu samples/ch\n", out.audio.len / sizeof(int16_t) / NUM_CHANNELS);

	printf("\nValidation:\n");
	ret = validate_lossy_audio(
		test_signal,
		NUM_SAMPLES,
		(const int16_t *)out.audio.data,
		out.audio.len / sizeof(int16_t) / NUM_CHANNELS,
		NUM_CHANNELS,
		min_snr_db,
		max_offset
	);

	th_buf_free(&muxed);
	th_out_free(&out);
	return ret;
}

int main(void) {
	int16_t *test_signal;
	int failed = 0;

	printf("=== muxaudio Lossy Codec Validation Test ===\n");
	printf(
		"Sample rate: %d Hz, channels: %d, %d seconds\n\n",
		SAMPLE_RATE,
		NUM_CHANNELS,
		DURATION_SEC
	);

	test_signal = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	if(!test_signal) {
		fprintf(stderr, "alloc failed\n");
		return 1;
	}

	generate_sine(test_signal, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if(test_waveform("440 Hz Sine Wave", test_signal, 10.0f, 1024) != 0)
		failed++;

	generate_sinc(test_signal, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 4000.0f, 0.5f);
	if(test_waveform("Sinc Pulse (4kHz cutoff)", test_signal, -5.0f, 1024) != 0)
		failed++;

	generate_triangle(test_signal, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if(test_waveform("440 Hz Triangle Wave", test_signal, -10.0f, 1024) != 0)
		failed++;

	generate_chirp(test_signal, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 100.0f, 8000.0f, 0.4f);
	if(test_waveform("Chirp 100Hz-8kHz", test_signal, -5.0f, 1024) != 0)
		failed++;

	free(test_signal);

	printf("\n========================================\n");
	if(failed == 0) {
		printf("\xe2\x9c\x93 All tests passed!\n");
		return 0;
	}
	printf("\xe2\x9c\x97 %d test(s) failed\n", failed);
	return 1;
}
