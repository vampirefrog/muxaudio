/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 1
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

/*
 * Generate a simple sine wave test signal
 */
static void generate_sine_wave(int16_t *buffer, int num_samples,
			       int channels, float frequency)
{
	int i, ch;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / SAMPLE_RATE;
		int16_t sample = (int16_t)(sin(2.0 * M_PI * frequency * t) * 32767.0 * 0.5);
		for (ch = 0; ch < channels; ch++) {
			buffer[i * channels + ch] = sample;
		}
	}
}

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
	int16_t pcm_input[NUM_SAMPLES * NUM_CHANNELS];
	int16_t pcm_output[NUM_SAMPLES * NUM_CHANNELS * 2];  /* Extra space */
	uint8_t side_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
	uint8_t muxed_buffer[256 * 1024];  /* 256KB buffer */
	uint8_t temp_buffer[8192];
	size_t input_consumed, output_written, total_muxed;
	int stream_type;
	int ret;
	size_t total_pcm_out = 0;
	int got_side_channel = 0;

	printf("=== muxaudio MP3 Codec Test ===\n\n");

	/* Generate test audio */
	printf("Generating test audio (440 Hz sine wave, %d sec)...\n",
	       DURATION_SEC);
	generate_sine_wave(pcm_input, NUM_SAMPLES, NUM_CHANNELS, 440.0);
	printf("Generated %zu bytes of PCM audio\n\n",
	       sizeof(pcm_input));

	/* Set up encoder parameters */
	struct mux_param params[] = {
		{ .name = "bitrate", .value.i = 128 },
		{ .name = "quality", .value.i = 5 },
		{ .name = "vbr", .value.b = 0 }
	};

	/* Create encoder */
	printf("Creating MP3 encoder (128 kbps CBR)...\n");
	enc = mux_encoder_new(MUX_CODEC_MP3, SAMPLE_RATE, NUM_CHANNELS, 2,
			      params, 3);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		return 1;
	}
	printf("Encoder created successfully\n\n");

	/* Encode audio in chunks */
	printf("Encoding audio...\n");
	size_t chunk_size = 4096;
	size_t offset = 0;

	while (offset < sizeof(pcm_input)) {
		size_t to_encode = sizeof(pcm_input) - offset;
		if (to_encode > chunk_size)
			to_encode = chunk_size;

		ret = mux_encoder_encode(enc,
					 (uint8_t *)pcm_input + offset,
					 to_encode,
					 &input_consumed,
					 MUX_STREAM_AUDIO);
		if (ret != MUX_OK) {
			fprintf(stderr, "Failed to encode audio: %d\n", ret);
			return 1;
		}
		offset += input_consumed;
	}
	printf("Encoded %zu bytes of PCM audio\n\n", offset);

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

	/* Read all muxed output */
	printf("Reading muxed output...\n");
	total_muxed = 0;
	while (1) {
		ret = mux_encoder_read(enc,
				       muxed_buffer + total_muxed,
				       sizeof(muxed_buffer) - total_muxed,
				       &output_written);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "Failed to read encoder output: %d\n", ret);
			return 1;
		}
		if (output_written == 0)
			break;
		total_muxed += output_written;
	}
	printf("Total muxed output: %zu bytes\n", total_muxed);
	printf("Compression ratio: %.2f%%\n\n",
	       (float)total_muxed / sizeof(pcm_input) * 100.0);

	/* Create decoder */
	printf("Creating MP3 decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_MP3, 2, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		return 1;
	}
	printf("Decoder created successfully\n\n");

	/* Decode in chunks */
	printf("Decoding muxed data...\n");
	offset = 0;
	chunk_size = 1024;

	while (offset < total_muxed) {
		size_t to_decode = total_muxed - offset;
		if (to_decode > chunk_size)
			to_decode = chunk_size;

		ret = mux_decoder_decode(dec,
					 muxed_buffer + offset,
					 to_decode,
					 &input_consumed);
		if (ret != MUX_OK) {
			fprintf(stderr, "Failed to decode: %d\n", ret);
			return 1;
		}
		offset += input_consumed;
	}
	printf("Decoded %zu bytes of muxed data\n\n", offset);

	/* Read decoded outputs */
	printf("Reading decoded outputs...\n");
	while (1) {
		ret = mux_decoder_read(dec, temp_buffer, sizeof(temp_buffer),
				       &output_written, &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "Failed to read decoded data: %d\n", ret);
			return 1;
		}

		if (stream_type == MUX_STREAM_AUDIO) {
			if (total_pcm_out + output_written <= sizeof(pcm_output)) {
				memcpy((uint8_t *)pcm_output + total_pcm_out,
				       temp_buffer, output_written);
				total_pcm_out += output_written;
			}
			printf("  Audio chunk: %zu bytes\n", output_written);
		} else {
			printf("  Side channel: ");
			print_hex("", temp_buffer, output_written);
			if (output_written == sizeof(side_data) &&
			    memcmp(temp_buffer, side_data, sizeof(side_data)) == 0) {
				printf("  ✓ Side channel data matches!\n");
				got_side_channel = 1;
			} else {
				fprintf(stderr, "  ✗ Side channel data mismatch!\n");
				return 1;
			}
		}
	}

	printf("\nTotal decoded PCM: %zu bytes\n", total_pcm_out);
	printf("Original PCM: %zu bytes\n", sizeof(pcm_input));

	/* Verify we got audio back */
	if (total_pcm_out > 0) {
		printf("✓ Successfully decoded audio\n");
	} else {
		fprintf(stderr, "✗ No audio decoded!\n");
		return 1;
	}

	if (got_side_channel) {
		printf("✓ Side channel received\n");
	} else {
		fprintf(stderr, "✗ Side channel not received!\n");
		return 1;
	}

	/* Cleanup */
	mux_encoder_destroy(enc);
	mux_decoder_destroy(dec);

	printf("\n=== All tests passed! ===\n");
	return 0;
}
