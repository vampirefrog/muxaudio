/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

int main(void)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	int16_t pcm_input[9600];  /* 0.2 seconds at 48000 Hz stereo */
	uint8_t muxed_buffer[65536];
	int16_t pcm_output[9600];
	size_t input_consumed, output_written;
	size_t total_muxed = 0, total_decoded = 0;
	int stream_type;
	int ret;

	printf("=== Simple Opus Encoder/Decoder Test ===\n\n");

	/* Opus only supports specific sample rates: 8000, 12000, 16000, 24000, 48000 */
	printf("Using Opus at 48000 Hz (Opus native sample rate)\n\n");

	/* Fill with simple sine wave data */
	for (int i = 0; i < 9600; i++)
		pcm_input[i] = (int16_t)(10000.0 * sin(2.0 * 3.14159 * 440.0 * i / 48000.0));

	/* Create encoder with bitrate parameter */
	printf("Creating Opus encoder...\n");
	struct mux_param params[] = {
		{ .name = "bitrate", .value.i = 64 }
	};
	enc = mux_encoder_new(MUX_CODEC_OPUS, 48000, 2, 2, params, 1);
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
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Encode failed: %s\n", err->message);
		mux_encoder_destroy(enc);
		return 1;
	}

	/* Finalize encoder to flush data */
	printf("Finalizing encoder...\n");
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Finalize failed: %s\n", err->message);
		mux_encoder_destroy(enc);
		return 1;
	}

	/* Read output */
	printf("Reading encoded output...\n");
	while (total_muxed < sizeof(muxed_buffer)) {
		ret = mux_encoder_read(enc, muxed_buffer + total_muxed,
				       sizeof(muxed_buffer) - total_muxed,
				       &output_written);
		if (ret == MUX_ERROR_AGAIN || output_written == 0)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "Read failed\n");
			mux_encoder_destroy(enc);
			return 1;
		}
		total_muxed += output_written;
	}
	printf("Total encoded: %zu bytes (%.1f%% of original)\n\n",
	       total_muxed, total_muxed * 100.0 / sizeof(pcm_input));

	/* Cleanup encoder */
	mux_encoder_destroy(enc);

	/* Create decoder */
	printf("Creating Opus decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_OPUS, 2, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		return 1;
	}
	printf("Decoder created\n\n");

	/* Decode */
	printf("Decoding %zu bytes...\n", total_muxed);
	ret = mux_decoder_decode(dec, muxed_buffer, total_muxed,
				 &input_consumed);
	printf("Decode returned: %d, consumed: %zu\n", ret, input_consumed);

	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Decode failed: %s\n", err->message);
		mux_decoder_destroy(dec);
		return 1;
	}

	/* Finalize decoder */
	printf("Finalizing decoder...\n");
	ret = mux_decoder_finalize(dec);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Finalize failed: %s\n", err->message);
		mux_decoder_destroy(dec);
		return 1;
	}

	/* Read decoded output */
	printf("Reading decoded output...\n");
	while (total_decoded < sizeof(pcm_output)) {
		ret = mux_decoder_read(dec, pcm_output + total_decoded,
				       sizeof(pcm_output) - total_decoded,
				       &output_written, &stream_type);
		if (ret == MUX_ERROR_AGAIN || output_written == 0)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "Decoder read failed\n");
			mux_decoder_destroy(dec);
			return 1;
		}
		if (stream_type == MUX_STREAM_AUDIO)
			total_decoded += output_written;
	}
	printf("Total decoded: %zu bytes (%zu samples)\n",
	       total_decoded, total_decoded / sizeof(int16_t));

	/* Cleanup decoder */
	mux_decoder_destroy(dec);

	printf("\n=== Test passed ===\n");
	return 0;
}
