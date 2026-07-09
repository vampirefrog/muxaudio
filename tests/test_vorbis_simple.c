/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>

int main(void)
{
	int16_t pcm[8192];   /* 4096 frames stereo @ 44100 */
	struct mux_param params[] = { { .name = "quality", .value.f = 0.4f } };
	struct th_buf muxed;
	struct th_out out;
	int ok;

	printf("=== Simple Vorbis Encoder/Decoder Test ===\n");
	th_sine(pcm, 4096, 2, 44100, 440.0, 0.31);

	if (th_encode(MUX_CODEC_VORBIS, 44100, 2, 2, params, 1,
		      pcm, sizeof(pcm), NULL, 0, &muxed) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	if (th_decode(MUX_CODEC_VORBIS, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}
	printf("Encoded %zu -> %zu bytes; decoded %zu bytes\n",
	       sizeof(pcm), muxed.len, out.audio.len);

	ok = out.audio.len >= sizeof(pcm) / 2;
	th_buf_free(&muxed);
	th_out_free(&out);
	printf("%s\n", ok ? "=== Test passed ===" : "FAIL: implausible length");
	return ok ? 0 : 1;
}
