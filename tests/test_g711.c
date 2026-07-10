/* SPDX-License-Identifier: GPL-3.0-or-later */
/* G.711 companding has no delay, so decoded audio aligns with the source and
 * a direct SNR check is meaningful (~38 dB for a sine). */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>

#define NUM_SAMPLES 1000

static int test_codec(enum mux_codec_type codec, const char *name) {
	int16_t input[NUM_SAMPLES];
	struct th_buf muxed;
	struct th_out out;
	double snr;

	printf("Testing %s codec...\n", name);
	th_sine(input, NUM_SAMPLES, 1, 8000, 440.0, 0.49);

	/* passthrough (num_streams=1): audio-only companded byte stream */
	if(th_encode(codec, 8000, 1, 1, NULL, 0, input, sizeof(input), NULL, 0, &muxed) != MUX_OK) {
		fprintf(stderr, "  FAIL: encode\n");
		return -1;
	}
	printf("  Encoded %zu bytes -> %zu bytes\n", sizeof(input), muxed.len);

	if(th_decode(codec, 1, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "  FAIL: decode\n");
		th_buf_free(&muxed);
		return -1;
	}

	if(out.audio.len != sizeof(input)) {
		fprintf(stderr, "  FAIL: size mismatch %zu != %zu\n", out.audio.len, sizeof(input));
		th_buf_free(&muxed);
		th_out_free(&out);
		return -1;
	}

	snr = th_snr(input, (const int16_t *)out.audio.data, NUM_SAMPLES);
	printf("  SNR: %.1f dB\n", snr);
	th_buf_free(&muxed);
	th_out_free(&out);

	if(snr < 30.0) {
		fprintf(stderr, "  FAIL: SNR too low (expected >= 30 dB)\n");
		return -1;
	}
	printf("  PASS\n\n");
	return 0;
}

int main(void) {
	int failures = 0;

	printf("G.711 Codec Tests\n=================\n\n");
	if(test_codec(MUX_CODEC_ALAW, "A-law") != 0)
		failures++;
	if(test_codec(MUX_CODEC_MULAW, "mu-law") != 0)
		failures++;

	if(failures == 0) {
		printf("All tests passed!\n");
		return 0;
	}
	printf("%d test(s) failed.\n", failures);
	return 1;
}
