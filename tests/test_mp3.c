/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MP3 round-trip with a side channel: audio decodes non-empty and the
 * side-channel message survives byte-exact. */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>
#include <string.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define NUM_SAMPLES SAMPLE_RATE /* 1 second */

int main(void) {
	static int16_t pcm[NUM_SAMPLES * NUM_CHANNELS];
	uint8_t side_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
	struct mux_param params[] = {
		{.name = "bitrate", .value.i = 128},
		{.name = "quality", .value.i = 5},
		{.name = "vbr", .value.b = 0}
	};
	struct th_buf muxed;
	struct th_out out;
	int rc = 0;

	printf("=== muxaudio MP3 Codec Test ===\n\n");
	th_sine(pcm, NUM_SAMPLES, NUM_CHANNELS, SAMPLE_RATE, 440.0, 0.5);

	if(th_encode(
		   MUX_CODEC_MP3,
		   SAMPLE_RATE,
		   NUM_CHANNELS,
		   2,
		   params,
		   3,
		   pcm,
		   sizeof(pcm),
		   side_data,
		   sizeof(side_data),
		   &muxed
	   ) != MUX_OK) {
		fprintf(stderr, "encode failed\n");
		return 1;
	}
	printf("Muxed %zu bytes (%.1f%% of original)\n", muxed.len, muxed.len * 100.0 / sizeof(pcm));

	if(th_decode(MUX_CODEC_MP3, 2, muxed.data, muxed.len, &out) != MUX_OK) {
		fprintf(stderr, "decode failed\n");
		return 1;
	}
	printf("Decoded audio %zu bytes, side %zu bytes\n", out.audio.len, out.side.len);

	if(out.audio.len > 0)
		printf("\xe2\x9c\x93 Successfully decoded audio\n");
	else {
		fprintf(stderr, "\xe2\x9c\x97 No audio decoded\n");
		rc = 1;
	}

	if(out.side.len == sizeof(side_data) &&
	   memcmp(out.side.data, side_data, sizeof(side_data)) == 0)
		printf("\xe2\x9c\x93 Side channel data matches\n");
	else {
		fprintf(stderr, "\xe2\x9c\x97 Side channel mismatch\n");
		rc = 1;
	}

	th_buf_free(&muxed);
	th_out_free(&out);
	printf("\n=== %s ===\n", rc ? "FAILED" : "All tests passed!");
	return rc;
}
