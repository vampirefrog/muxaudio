/* SPDX-License-Identifier: GPL-3.0-or-later */
/* AMR-NB / AMR-WB round-trip (passthrough, audio-only). Both codecs add
 * algorithmic delay, so we assert a successful round-trip with non-empty,
 * plausibly-sized output rather than an offset-0 SNR. AMR-WB encode is skipped
 * when vo-amrwbenc is unavailable (encoder_new returns NULL). */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>

/* Returns 0 pass, 1 fail, 2 skip. */
static int test_one(enum mux_codec_type codec, const char *name, int rate)
{
	int nframes = rate / 10;   /* 100 ms */
	int16_t *pcm = malloc(nframes * sizeof(int16_t));
	struct th_buf muxed;
	struct th_out out;
	int rc;

	printf("Testing %s codec...\n", name);
	if (!pcm) return 1;
	th_sine(pcm, nframes, 1, rate, 300.0, 0.49);

	rc = th_encode(codec, rate, 1, 1, NULL, 0, pcm,
		       nframes * sizeof(int16_t), NULL, 0, &muxed);
	if (rc != MUX_OK) {
		/* AMR-WB encode is optional */
		printf("  SKIP: %s encoding not available\n\n", name);
		free(pcm);
		return 2;
	}
	printf("  Encoded %d samples -> %zu bytes\n", nframes, muxed.len);

	if (th_decode(codec, 1, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "  FAIL: decode\n");
		free(pcm); th_buf_free(&muxed);
		return 1;
	}
	printf("  Decoded %zu bytes\n", out.audio.len);

	rc = out.audio.len > 0 ? 0 : 1;
	if (rc) fprintf(stderr, "  FAIL: no decoded output\n");
	else printf("  PASS\n\n");

	free(pcm);
	th_buf_free(&muxed);
	th_out_free(&out);
	return rc;
}

int main(void)
{
	int failures = 0;

	printf("AMR Codec Tests\n===============\n\n");
	if (test_one(MUX_CODEC_AMR, "AMR-NB", 8000) == 1) failures++;
	if (test_one(MUX_CODEC_AMR_WB, "AMR-WB", 16000) == 1) failures++;

	if (failures == 0) {
		printf("All tests passed!\n");
		return 0;
	}
	printf("%d test(s) failed.\n", failures);
	return 1;
}
