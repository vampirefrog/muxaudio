/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void print_hex(const char *label, const uint8_t *data, size_t len)
{
	printf("%s (%zu bytes): ", label, len);
	for (size_t i = 0; i < len && i < 32; i++)
		printf("%02x ", data[i]);
	if (len > 32)
		printf("...");
	printf("\n");
}

int main(void)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	uint8_t audio_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	uint8_t side_data[] = {0xAA, 0xBB, 0xCC, 0xDD};
	uint8_t muxed_buffer[1024];
	uint8_t output_buffer[1024];
	size_t input_consumed, output_written;
	int stream_type;
	int ret;

	printf("=== muxaudio PCM Codec Test ===\n\n");

	/* List available codecs */
	const struct mux_codec_info *codecs;
	int codec_count;
	ret = mux_list_codecs(&codecs, &codec_count);
	if (ret == MUX_OK) {
		printf("Available codecs:\n");
		for (int i = 0; i < codec_count; i++) {
			printf("  %d: %s - %s\n", codecs[i].type,
			       codecs[i].name, codecs[i].description);
		}
		printf("\n");
	}

	/* Create encoder */
	printf("Creating PCM encoder...\n");
	enc = mux_encoder_new(MUX_CODEC_PCM, 44100, 2, 2, NULL, 0);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		return 1;
	}
	printf("Encoder created successfully\n\n");

	/* Encode audio data */
	printf("Encoding audio data...\n");
	print_hex("Audio input", audio_data, sizeof(audio_data));
	ret = mux_encoder_encode(enc, audio_data, sizeof(audio_data),
				 &input_consumed, MUX_STREAM_AUDIO);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to encode audio: %d\n", ret);
		return 1;
	}
	printf("Encoded %zu bytes of audio\n\n", input_consumed);

	/* Encode side channel data */
	printf("Encoding side channel data...\n");
	print_hex("Side channel input", side_data, sizeof(side_data));
	ret = mux_encoder_encode(enc, side_data, sizeof(side_data),
				 &input_consumed, MUX_STREAM_SIDE_CHANNEL);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to encode side channel: %d\n", ret);
		return 1;
	}
	printf("Encoded %zu bytes of side channel\n\n", input_consumed);

	/* Read muxed output */
	printf("Reading muxed output...\n");
	ret = mux_encoder_read(enc, muxed_buffer, sizeof(muxed_buffer),
			       &output_written);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to read encoder output: %d\n", ret);
		return 1;
	}
	print_hex("Muxed output", muxed_buffer, output_written);
	printf("\n");

	/* Create decoder */
	printf("Creating PCM decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_PCM, 2, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		return 1;
	}
	printf("Decoder created successfully\n\n");

	/* Decode muxed data */
	printf("Decoding muxed data...\n");
	ret = mux_decoder_decode(dec, muxed_buffer, output_written,
				 &input_consumed);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to decode: %d\n", ret);
		return 1;
	}
	printf("Decoded %zu bytes of muxed data\n\n", input_consumed);

	/* Read audio output */
	printf("Reading decoded audio...\n");
	ret = mux_decoder_read(dec, output_buffer, sizeof(output_buffer),
			       &output_written, &stream_type);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to read audio: %d\n", ret);
		return 1;
	}
	printf("Stream type: %s\n",
	       stream_type == MUX_STREAM_AUDIO ? "AUDIO" : "SIDE_CHANNEL");
	print_hex("Audio output", output_buffer, output_written);

	if (stream_type == MUX_STREAM_AUDIO &&
	    output_written == sizeof(audio_data) &&
	    memcmp(output_buffer, audio_data, sizeof(audio_data)) == 0) {
		printf("✓ Audio data matches!\n\n");
	} else {
		fprintf(stderr, "✗ Audio data mismatch!\n\n");
		return 1;
	}

	/* Read side channel output */
	printf("Reading decoded side channel...\n");
	ret = mux_decoder_read(dec, output_buffer, sizeof(output_buffer),
			       &output_written, &stream_type);
	if (ret != MUX_OK) {
		fprintf(stderr, "Failed to read side channel: %d\n", ret);
		return 1;
	}
	printf("Stream type: %s\n",
	       stream_type == MUX_STREAM_AUDIO ? "AUDIO" : "SIDE_CHANNEL");
	print_hex("Side channel output", output_buffer, output_written);

	if (stream_type == MUX_STREAM_SIDE_CHANNEL &&
	    output_written == sizeof(side_data) &&
	    memcmp(output_buffer, side_data, sizeof(side_data)) == 0) {
		printf("✓ Side channel data matches!\n\n");
	} else {
		fprintf(stderr, "✗ Side channel data mismatch!\n\n");
		return 1;
	}

	/* Cleanup */
	mux_encoder_destroy(enc);
	mux_decoder_destroy(dec);

	printf("=== All tests passed! ===\n");
	return 0;
}
