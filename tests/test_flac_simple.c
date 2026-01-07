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
	int16_t pcm_input[8192];  /* 1 second at 44100 Hz stereo */
	uint8_t muxed_buffer[65536];
	int16_t pcm_output[8192];
	size_t input_consumed, output_written;
	size_t total_muxed = 0, total_decoded = 0;
	int stream_type;
	int ret;

	printf("=== Simple FLAC Encoder/Decoder Test ===\n\n");

	/* FLAC is lossless - should perfectly reconstruct the signal */
	printf("Using FLAC at 44100 Hz (lossless compression)\n\n");

	/* Fill with simple sine wave data */
	for (int i = 0; i < 8192; i++)
		pcm_input[i] = (int16_t)(10000.0 * sin(2.0 * 3.14159 * 440.0 * i / 44100.0));

	/* Create encoder with compression parameter */
	printf("Creating FLAC encoder...\n");
	struct mux_param params[] = {
		{ .name = "compression", .value.i = 5 }  /* Default compression */
	};
	enc = mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, params, 1);
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
	printf("Creating FLAC decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_FLAC, NULL, 0);
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

	/* Verify lossless compression - compare input and output */
	printf("\nVerifying lossless compression...\n");
	int differences = 0;
	size_t samples_to_check = (total_decoded < sizeof(pcm_input)) ?
		total_decoded / sizeof(int16_t) : sizeof(pcm_input) / sizeof(int16_t);

	for (size_t i = 0; i < samples_to_check; i++) {
		if (pcm_input[i] != pcm_output[i]) {
			differences++;
			if (differences <= 5) {
				printf("  Difference at sample %zu: input=%d, output=%d\n",
				       i, pcm_input[i], pcm_output[i]);
			}
		}
	}

	if (differences == 0) {
		printf("PERFECT! All %zu samples match exactly (lossless)\n", samples_to_check);
	} else {
		printf("WARNING: %d differences found (expected 0 for lossless)\n", differences);
	}

	/* Cleanup decoder */
	mux_decoder_destroy(dec);

	printf("\n=== Test %s ===\n", differences == 0 ? "passed" : "FAILED");
	return differences == 0 ? 0 : 1;
}
