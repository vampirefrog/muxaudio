/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Vorbis is lossy: validate reconstruction quality (SNR, with time-alignment)
 * across several waveforms. */
#include "mux.h"
#include "mux_testhelp.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 2
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

static int test_waveform(const char *name, int16_t *test_signal,
			 float min_snr_db, int max_offset)
{
	struct mux_param params[] = { { .name = "quality", .value.f = 0.6f } };
	struct th_buf muxed;
	struct th_out out;
	size_t input_bytes = (size_t)NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t);
	int ret;

	printf("\n=== Testing %s ===\n", name);

	if (th_encode(MUX_CODEC_VORBIS, SAMPLE_RATE, NUM_CHANNELS, 2, params, 1,
		      test_signal, input_bytes, NULL, 0, &muxed) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return -1;
	}
	if (th_decode(MUX_CODEC_VORBIS, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		th_buf_free(&muxed);
		return -1;
	}
	printf("Encoded %zu -> %zu bytes; decoded %zu samples/ch\n",
	       input_bytes, muxed.len,
	       out.audio.len / sizeof(int16_t) / NUM_CHANNELS);

	ret = validate_lossy_audio(test_signal, NUM_SAMPLES,
				   (const int16_t *)out.audio.data,
				   out.audio.len / sizeof(int16_t) / NUM_CHANNELS,
				   NUM_CHANNELS, min_snr_db, max_offset);
	th_buf_free(&muxed);
	th_out_free(&out);
	return ret;
}

int main(void)
{
	int16_t *sig = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	int failed = 0;

	if (!sig) { fprintf(stderr, "alloc failed\n"); return 1; }
	printf("=== muxaudio Vorbis Validation ===\n");

	generate_sine(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Sine", sig, 0.0f, 4096) != 0) failed++;

	generate_triangle(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Triangle", sig, -10.0f, 4096) != 0) failed++;

	generate_chirp(sig, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 100.0f, 8000.0f, 0.4f);
	if (test_waveform("Chirp 100Hz-8kHz", sig, -10.0f, 4096) != 0) failed++;

	free(sig);
	printf("\n========================================\n");
	if (failed == 0) { printf("\xe2\x9c\x93 All tests passed!\n"); return 0; }
	printf("\xe2\x9c\x97 %d test(s) failed\n", failed);
	return 1;
}
