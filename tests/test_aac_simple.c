/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>

int main(void) {
	int16_t pcm[44100 * 2]; /* 1s stereo @ 44100 */
	struct mux_param params[] = {{.name = "bitrate", .value.i = 128}};
	struct th_buf muxed;
	struct th_out out;
	int ok;

	printf("=== Simple AAC Encoder/Decoder Test ===\n");
	th_sine(pcm, 44100, 2, 44100, 440.0, 0.31);

	if(th_encode(MUX_CODEC_AAC, 44100, 2, 2, params, 1, pcm, sizeof(pcm), NULL, 0, &muxed) !=
	   MUX_OK) {
		fprintf(stderr, "encode failed (is libfdk-aac present?)\n");
		return 1;
	}
	if(th_decode(MUX_CODEC_AAC, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}
	printf("Encoded %zu -> %zu bytes; decoded %zu bytes\n", sizeof(pcm), muxed.len, out.audio.len);

	ok = out.audio.len > 0;
	th_buf_free(&muxed);
	th_out_free(&out);
	printf("%s\n", ok ? "=== Test passed ===" : "FAIL: no audio decoded");
	return ok ? 0 : 1;
}
