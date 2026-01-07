/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <lame/lame.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
	lame_global_flags *gfp;
	hip_t hip;
	int16_t pcm_in[2304];  /* 1152 samples * 2 channels */
	uint8_t mp3_buf[8192];
	int16_t pcm_l[1152], pcm_r[1152];
	int mp3_bytes, samples;

	printf("Testing LAME directly...\n\n");

	/* Create encoder */
	gfp = lame_init();
	lame_set_num_channels(gfp, 2);
	lame_set_in_samplerate(gfp, 44100);
	lame_set_brate(gfp, 128);
	lame_init_params(gfp);

	/* Fill with test data */
	for (int i = 0; i < 2304; i++)
		pcm_in[i] = (int16_t)(i * 100);

	/* Encode */
	printf("Encoding %zu bytes of PCM...\n", sizeof(pcm_in));
	mp3_bytes = lame_encode_buffer_interleaved(gfp, pcm_in, 1152,
						    mp3_buf, sizeof(mp3_buf));
	printf("Encoded to %d bytes of MP3\n", mp3_bytes);

	if (mp3_bytes < 0) {
		printf("Encode error!\n");
		return 1;
	}

	/* Print first few bytes */
	printf("MP3 data: ");
	for (int i = 0; i < (mp3_bytes < 16 ? mp3_bytes : 16); i++)
		printf("%02x ", mp3_buf[i]);
	printf("\n\n");

	/* Try to decode */
	printf("Decoding...\n");
	hip = hip_decode_init();
	samples = hip_decode(hip, mp3_buf, mp3_bytes, pcm_l, pcm_r);
	printf("Decoded %d samples\n", samples);

	if (samples < 0) {
		printf("Decode error! Return code: %d\n", samples);
	} else if (samples == 0) {
		printf("No samples decoded (need more data?)\n");
	} else {
		printf("Successfully decoded %d samples\n", samples);
	}

	hip_decode_exit(hip);
	lame_close(gfp);

	return 0;
}
