/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
	uint8_t audio_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	uint8_t side_data[]  = {0xAA, 0xBB, 0xCC, 0xDD};
	struct th_buf muxed;
	struct th_out out;
	int rc = 0;

	printf("=== muxaudio PCM Codec Test ===\n\n");

	if (th_encode(MUX_CODEC_PCM, 44100, 2, 2, NULL, 0,
		      (int16_t *)audio_data, sizeof(audio_data),
		      side_data, sizeof(side_data), &muxed) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	printf("Muxed %zu bytes\n", muxed.len);

	if (th_decode(MUX_CODEC_PCM, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}

	if (out.audio.len == sizeof(audio_data) &&
	    memcmp(out.audio.data, audio_data, sizeof(audio_data)) == 0)
		printf("\xe2\x9c\x93 Audio data matches\n");
	else {
		fprintf(stderr, "\xe2\x9c\x97 Audio mismatch (%zu bytes)\n", out.audio.len);
		rc = 1;
	}

	if (out.side.len == sizeof(side_data) &&
	    memcmp(out.side.data, side_data, sizeof(side_data)) == 0)
		printf("\xe2\x9c\x93 Side channel data matches\n");
	else {
		fprintf(stderr, "\xe2\x9c\x97 Side channel mismatch (%zu bytes)\n", out.side.len);
		rc = 1;
	}

	th_buf_free(&muxed);
	th_out_free(&out);

	printf("\n=== %s ===\n", rc ? "FAILED" : "All tests passed!");
	return rc;
}
