/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void)
{
	struct mux_encoder *enc;
	int16_t pcm_input[4096];  /* Small test */
	uint8_t muxed_buffer[16384];
	size_t input_consumed, output_written;
	int ret;

	printf("=== Simple MP3 Encoder Test ===\n\n");

	/* Fill with simple data */
	for (int i = 0; i < 4096; i++)
		pcm_input[i] = (int16_t)(i * 100);

	/* Create encoder */
	printf("Creating MP3 encoder...\n");
	enc = mux_encoder_new(MUX_CODEC_MP3, 44100, 2, NULL, 0);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		return 1;
	}
	printf("Encoder created\n\n");

	/* Encode */
	printf("Encoding %zu bytes...\n", sizeof(pcm_input));
	ret = mux_encoder_encode(enc, pcm_input, sizeof(pcm_input),
				 &input_consumed, MUX_STREAM_AUDIO);
	printf("Encode returned: %d, consumed: %zu\n", ret, input_consumed);

	if (ret != MUX_OK) {
		fprintf(stderr, "Encode failed\n");
		return 1;
	}

	/* Read output */
	printf("Reading output...\n");
	ret = mux_encoder_read(enc, muxed_buffer, sizeof(muxed_buffer),
			       &output_written);
	printf("Read returned: %d, written: %zu\n", ret, output_written);

	/* Cleanup */
	mux_encoder_destroy(enc);

	printf("\n=== Test passed ===\n");
	return 0;
}
