/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>

int main(void) {
	int16_t pcm[8192]; /* 4096 frames stereo */
	struct th_buf muxed;
	struct th_out out;
	int ok;

	printf("=== Simple MP3 Encoder/Decoder Test ===\n");
	th_sine(pcm, 4096, 2, 44100, 440.0, 0.3);

	if(th_encode(MUX_CODEC_MP3, 44100, 2, 2, NULL, 0, pcm, sizeof(pcm), NULL, 0, &muxed) !=
	   MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	printf("Encoded %zu -> %zu bytes\n", sizeof(pcm), muxed.len);

	if(th_decode(MUX_CODEC_MP3, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}
	printf("Decoded %zu bytes\n", out.audio.len);

	ok = muxed.len > 0 && out.audio.len > 0;
	th_buf_free(&muxed);
	th_out_free(&out);
	printf("%s\n", ok ? "=== Test passed ===" : "FAIL");
	return ok ? 0 : 1;
}
