/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FLAC is lossless: decoded audio must equal the source exactly. */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
	int16_t pcm[8192];
	struct mux_param params[] = { { .name = "compression", .value.i = 5 } };
	struct th_buf muxed;
	struct th_out out;
	int rc = 0;

	printf("=== Simple FLAC Encoder/Decoder Test ===\n\n");
	th_sine(pcm, 4096, 2, 44100, 440.0, 0.31);

	if (th_encode(MUX_CODEC_FLAC, 44100, 2, 2, params, 1,
		      pcm, sizeof(pcm), NULL, 0, &muxed) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	printf("Encoded %zu bytes -> %zu bytes (%.1f%%)\n",
	       sizeof(pcm), muxed.len, muxed.len * 100.0 / sizeof(pcm));

	if (th_decode(MUX_CODEC_FLAC, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}
	printf("Decoded %zu bytes\n", out.audio.len);

	if (out.audio.len == sizeof(pcm) &&
	    memcmp(out.audio.data, pcm, sizeof(pcm)) == 0) {
		printf("PERFECT! All samples match exactly (lossless)\n");
	} else {
		printf("FAILED: lossless mismatch (%zu vs %zu bytes)\n",
		       out.audio.len, sizeof(pcm));
		rc = 1;
	}

	th_buf_free(&muxed);
	th_out_free(&out);
	return rc;
}
