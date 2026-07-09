/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FLAC is lossless: decoded audio must equal the source exactly, across a
 * variety of waveforms and both mono and stereo. */
#include "mux.h"
#include "mux_testhelp.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define NUM_SAMPLES (SAMPLE_RATE / 2)   /* 0.5s */

static int roundtrip_exact(const char *name, int channels, int level,
			   const int16_t *pcm)
{
	struct mux_param params[] = { { .name = "compression", .value.i = level } };
	size_t bytes = (size_t)NUM_SAMPLES * channels * sizeof(int16_t);
	struct th_buf muxed;
	struct th_out out;
	int rc = 0;

	printf("\n=== %s (%d ch, level %d) ===\n", name, channels, level);

	if (th_encode(MUX_CODEC_FLAC, SAMPLE_RATE, channels, 2, params, 1,
		      pcm, bytes, NULL, 0, &muxed) != MUX_OK) {
		fprintf(stderr, "  encode failed\n");
		return -1;
	}
	if (th_decode(MUX_CODEC_FLAC, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "  decode failed\n");
		th_buf_free(&muxed);
		return -1;
	}

	if (out.audio.len != bytes ||
	    memcmp(out.audio.data, pcm, bytes) != 0) {
		printf("  FAIL: not lossless (%zu vs %zu bytes)\n", out.audio.len, bytes);
		rc = -1;
	} else {
		printf("  PASS: %zu -> %zu bytes, bit-exact\n", bytes, muxed.len);
	}

	th_buf_free(&muxed);
	th_out_free(&out);
	return rc;
}

int main(void)
{
	int16_t *sig = malloc((size_t)NUM_SAMPLES * 2 * sizeof(int16_t));
	int failed = 0;

	if (!sig) { fprintf(stderr, "alloc failed\n"); return 1; }
	printf("=== muxaudio FLAC Lossless Validation ===\n");

	generate_sine(sig, NUM_SAMPLES, 1, SAMPLE_RATE, 440.0f, 0.6f);
	if (roundtrip_exact("Sine mono", 1, 5, sig) != 0) failed++;

	generate_sine(sig, NUM_SAMPLES, 2, SAMPLE_RATE, 440.0f, 0.6f);
	if (roundtrip_exact("Sine stereo", 2, 8, sig) != 0) failed++;

	generate_square(sig, NUM_SAMPLES, 2, SAMPLE_RATE, 220.0f, 0.5f);
	if (roundtrip_exact("Square stereo", 2, 5, sig) != 0) failed++;

	generate_noise(sig, NUM_SAMPLES, 2, 0.8f);
	if (roundtrip_exact("Noise stereo", 2, 0, sig) != 0) failed++;

	free(sig);
	printf("\n========================================\n");
	if (failed == 0) { printf("\xe2\x9c\x93 All tests passed!\n"); return 0; }
	printf("\xe2\x9c\x97 %d test(s) failed\n", failed);
	return 1;
}
