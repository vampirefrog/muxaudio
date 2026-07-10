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
	if(th_decode(MUX_CODEC_MP3, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		th_buf_free(&muxed);
		return -1;
	}
	printf(
		"Encoded %zu -> %zu bytes; decoded %zu samples/ch\n",
		input_bytes,
		muxed.len,
		out.audio.len / sizeof(int16_t) / NUM_CHANNELS
	);

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
	int16_t *sig = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	int16_t *tmp = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	int failed = 0;

	if(!sig || !tmp) {
		fprintf(stderr, "alloc failed\n");
		free(sig);
		free(tmp);
		return 1;
	}

	printf("=== muxaudio Waveform Tests ===\n");

	generate_sine(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if(test_waveform("440 Hz Sine", sig, 10.0f, 1024) != 0)
		failed++;

	generate_square(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if(test_waveform("440 Hz Square", sig, -10.0f, 1024) != 0)
		failed++;

	generate_sawtooth(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if(test_waveform("440 Hz Sawtooth", sig, -10.0f, 1024) != 0)
		failed++;

	generate_chirp(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 100.0f, 8000.0f, 0.4f);
	if(test_waveform("Chirp 100Hz-8kHz", sig, -5.0f, 1024) != 0)
		failed++;

	/* C major chord (multi-tone) */
	generate_silence(sig, NUM_SAMPLES, NUM_CHANNELS);
	const float chord[] = {261.63f, 329.63f, 392.00f};
	for(int c = 0; c < 3; c++) {
		generate_sine(tmp, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, chord[c], 0.25f);
		for(size_t i = 0; i < (size_t)NUM_SAMPLES * NUM_CHANNELS; i++)
			sig[i] += tmp[i];
	}
	if(test_waveform("C Major Chord", sig, 0.0f, 1024) != 0)
		failed++;

	free(sig);
	free(tmp);

	printf("\n========================================\n");
	if(failed == 0) {
		printf("\xe2\x9c\x93 All tests passed!\n");
		return 0;
	}
	printf("\xe2\x9c\x97 %d test(s) failed\n", failed);
	return 1;
}
