/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 2
#define DURATION_SEC 2
#define NUM_SAMPLES (SAMPLE_RATE * DURATION_SEC)

/*
 * Test configuration structure
 */
struct test_config {
	int sample_rate;
	int num_channels;
	int compression_level;
	const char *description;
};

/*
 * Validate lossless audio - every sample must match exactly
 */
static int validate_lossless(const int16_t *original, size_t original_samples,
			     const int16_t *decoded, size_t decoded_samples,
			     int num_channels, int max_offset)
{
	size_t i;
	int differences = 0;
	size_t samples_to_check;

	/* Check sample count (allow some padding from decoder) */
	if (decoded_samples < original_samples) {
		printf("  ERROR: Decoded %zu samples, expected at least %zu\n",
		       decoded_samples, original_samples);
		return -1;
	}

	if (decoded_samples > original_samples + (size_t)max_offset) {
		printf("  WARNING: Decoded %zu samples, expected %zu (offset: %zu)\n",
		       decoded_samples, original_samples,
		       decoded_samples - original_samples);
	}

	samples_to_check = original_samples;
	if (decoded_samples < samples_to_check)
		samples_to_check = decoded_samples;

	/* Check every sample for exact match */
	for (i = 0; i < samples_to_check * num_channels; i++) {
		if (original[i] != decoded[i]) {
			if (differences < 10) {
				printf("  MISMATCH at sample %zu (channel %zu): "
				       "expected %d, got %d\n",
				       i / num_channels, i % num_channels,
				       original[i], decoded[i]);
			}
			differences++;
		}
	}

	if (differences > 0) {
		printf("  FAILED: %d samples differ (%.4f%%)\n",
		       differences,
		       100.0 * differences / (samples_to_check * num_channels));
		return -1;
	}

	printf("  PERFECT: All %zu samples match exactly (lossless)\n",
	       samples_to_check);
	return 0;
}

/*
 * Test a waveform with given configuration
 */
static int test_waveform_config(const char *name, int16_t *test_signal,
				const struct test_config *config)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	uint8_t *muxed_buffer;
	int16_t *decoded_audio;
	size_t muxed_capacity = 512 * 1024;
	size_t input_bytes = NUM_SAMPLES * config->num_channels * sizeof(int16_t);
	size_t decoded_capacity = NUM_SAMPLES * config->num_channels * 2;
	size_t input_consumed, output_written, total_muxed = 0;
	size_t total_decoded = 0;
	int stream_type;
	int ret;

	printf("\n=== %s ===\n", name);
	printf("  Config: %s\n", config->description);
	fflush(stdout);

	/* Allocate buffers */
	muxed_buffer = malloc(muxed_capacity);
	decoded_audio = malloc(decoded_capacity * sizeof(int16_t));

	if (!muxed_buffer || !decoded_audio) {
		fprintf(stderr, "Failed to allocate buffers\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Create encoder with compression level */
	struct mux_param params[] = {
		{ .name = "compression", .value.i = config->compression_level }
	};

	enc = mux_encoder_new(MUX_CODEC_FLAC, config->sample_rate,
			      config->num_channels, 2, params, 1);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Encode in chunks */
	size_t chunk_size = 8192;
	size_t offset = 0;

	while (offset < input_bytes) {
		size_t to_encode = input_bytes - offset;
		if (to_encode > chunk_size)
			to_encode = chunk_size;

		ret = mux_encoder_encode(enc,
					 (uint8_t *)test_signal + offset,
					 to_encode,
					 &input_consumed,
					 MUX_STREAM_AUDIO);
		if (ret != MUX_OK) {
			const struct mux_error_info *err = mux_encoder_get_error(enc);
			fprintf(stderr, "Encode failed: %s\n", err->message);
			mux_encoder_destroy(enc);
			free(muxed_buffer);
			free(decoded_audio);
			return -1;
		}
		offset += input_consumed;
	}

	/* Finalize encoder */
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Finalize failed: %s\n", err->message);
		mux_encoder_destroy(enc);
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Read muxed output */
	while (total_muxed < muxed_capacity) {
		ret = mux_encoder_read(enc,
				       muxed_buffer + total_muxed,
				       muxed_capacity - total_muxed,
				       &output_written);
		if (ret == MUX_ERROR_AGAIN || output_written == 0)
			break;
		if (ret != MUX_OK)
			break;
		total_muxed += output_written;
	}

	printf("  Encoded: %zu → %zu bytes (%.1f%% compression)\n",
	       input_bytes, total_muxed, total_muxed * 100.0 / input_bytes);
	fflush(stdout);

	mux_encoder_destroy(enc);

	/* Create decoder */
	dec = mux_decoder_new(MUX_CODEC_FLAC, 2, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Decode in chunks */
	offset = 0;
	chunk_size = 2048;

	while (offset < total_muxed) {
		size_t to_decode = total_muxed - offset;
		if (to_decode > chunk_size)
			to_decode = chunk_size;

		ret = mux_decoder_decode(dec,
					 muxed_buffer + offset,
					 to_decode,
					 &input_consumed);
		if (ret != MUX_OK) {
			const struct mux_error_info *err = mux_decoder_get_error(dec);
			fprintf(stderr, "Decode error: %s\n", err->message);
		}
		offset += input_consumed;
	}

	/* Finalize decoder */
	ret = mux_decoder_finalize(dec);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Finalize error: %s\n", err->message);
	}

	/* Read decoded output */
	while (total_decoded < decoded_capacity) {
		ret = mux_decoder_read(dec,
				       decoded_audio + total_decoded,
				       (decoded_capacity - total_decoded) * sizeof(int16_t),
				       &output_written,
				       &stream_type);
		if (ret == MUX_ERROR_AGAIN || output_written == 0)
			break;
		if (ret != MUX_OK)
			break;
		if (stream_type == MUX_STREAM_AUDIO)
			total_decoded += output_written / sizeof(int16_t);
	}

	printf("  Decoded: %zu samples\n", total_decoded / config->num_channels);

	mux_decoder_destroy(dec);

	/* Validate lossless compression */
	ret = validate_lossless(test_signal, NUM_SAMPLES,
				decoded_audio, total_decoded / config->num_channels,
				config->num_channels, 256);

	free(muxed_buffer);
	free(decoded_audio);

	return ret;
}

/*
 * Test a waveform with default configuration
 */
static int test_waveform(const char *name, int16_t *test_signal)
{
	struct test_config config = {
		.sample_rate = SAMPLE_RATE,
		.num_channels = NUM_CHANNELS,
		.compression_level = 5,
		.description = "44100 Hz, stereo, compression level 5"
	};
	return test_waveform_config(name, test_signal, &config);
}

/*
 * Test compression levels
 */
static int test_compression_levels(int16_t *test_signal)
{
	int level;
	int failed = 0;

	printf("\n========================================\n");
	printf("Testing Compression Levels\n");
	printf("========================================\n");

	for (level = 0; level <= 8; level++) {
		struct test_config config = {
			.sample_rate = SAMPLE_RATE,
			.num_channels = NUM_CHANNELS,
			.compression_level = level,
			.description = ""
		};
		char desc[128];
		snprintf(desc, sizeof(desc),
			 "44100 Hz, stereo, compression level %d", level);
		config.description = desc;

		if (test_waveform_config("Sine Wave", test_signal, &config) != 0)
			failed++;
	}

	return failed;
}

/*
 * Test different sample rates
 */
static int test_sample_rates(void)
{
	int failed = 0;
	int rates[] = { 8000, 16000, 22050, 44100, 48000, 96000 };
	int i;

	printf("\n========================================\n");
	printf("Testing Sample Rates\n");
	printf("========================================\n");

	for (i = 0; i < 6; i++) {
		int rate = rates[i];
		int num_samples = rate * 1;  /* 1 second */
		int16_t *signal = malloc(num_samples * 2 * sizeof(int16_t));

		if (!signal) {
			fprintf(stderr, "Failed to allocate signal buffer\n");
			failed++;
			continue;
		}

		generate_sine(signal, num_samples, 2, rate, 440.0f, 0.5f);

		/* Use the signal we just created */
		struct mux_encoder *enc;
		struct mux_decoder *dec;
		uint8_t *muxed_buffer;
		int16_t *decoded_audio;
		size_t muxed_capacity = 256 * 1024;
		size_t input_bytes = num_samples * 2 * sizeof(int16_t);
		size_t decoded_capacity = num_samples * 2 * 2;
		size_t input_consumed, output_written, total_muxed = 0;
		size_t total_decoded = 0;
		int stream_type;
		int ret;

		printf("\n=== Testing %d Hz ===\n", rate);
		fflush(stdout);

		muxed_buffer = malloc(muxed_capacity);
		decoded_audio = malloc(decoded_capacity * sizeof(int16_t));

		if (!muxed_buffer || !decoded_audio) {
			fprintf(stderr, "Failed to allocate buffers\n");
			free(signal);
			free(muxed_buffer);
			free(decoded_audio);
			failed++;
			continue;
		}

		struct mux_param params[] = {
			{ .name = "compression", .value.i = 5 }
		};

		enc = mux_encoder_new(MUX_CODEC_FLAC, rate, 2, 2, params, 1);
		if (!enc) {
			fprintf(stderr, "Failed to create encoder\n");
			free(signal);
			free(muxed_buffer);
			free(decoded_audio);
			failed++;
			continue;
		}

		ret = mux_encoder_encode(enc, signal, input_bytes,
					 &input_consumed, MUX_STREAM_AUDIO);
		if (ret != MUX_OK) {
			const struct mux_error_info *err = mux_encoder_get_error(enc);
			fprintf(stderr, "Encode failed: %s\n", err->message);
			mux_encoder_destroy(enc);
			free(signal);
			free(muxed_buffer);
			free(decoded_audio);
			failed++;
			continue;
		}

		mux_encoder_finalize(enc);

		while (total_muxed < muxed_capacity) {
			ret = mux_encoder_read(enc, muxed_buffer + total_muxed,
					       muxed_capacity - total_muxed,
					       &output_written);
			if (ret == MUX_ERROR_AGAIN || output_written == 0)
				break;
			total_muxed += output_written;
		}

		printf("  Encoded: %zu → %zu bytes (%.1f%%)\n",
		       input_bytes, total_muxed, total_muxed * 100.0 / input_bytes);

		mux_encoder_destroy(enc);

		dec = mux_decoder_new(MUX_CODEC_FLAC, 2, NULL, 0);
		if (!dec) {
			fprintf(stderr, "Failed to create decoder\n");
			free(signal);
			free(muxed_buffer);
			free(decoded_audio);
			failed++;
			continue;
		}

		mux_decoder_decode(dec, muxed_buffer, total_muxed, &input_consumed);
		mux_decoder_finalize(dec);

		while (total_decoded < decoded_capacity) {
			ret = mux_decoder_read(dec, decoded_audio + total_decoded,
					       (decoded_capacity - total_decoded) * sizeof(int16_t),
					       &output_written, &stream_type);
			if (ret == MUX_ERROR_AGAIN || output_written == 0)
				break;
			if (stream_type == MUX_STREAM_AUDIO)
				total_decoded += output_written / sizeof(int16_t);
		}

		printf("  Decoded: %zu samples\n", total_decoded / 2);

		ret = validate_lossless(signal, num_samples, decoded_audio,
					total_decoded / 2, 2, 256);
		if (ret != 0)
			failed++;

		mux_decoder_destroy(dec);
		free(signal);
		free(muxed_buffer);
		free(decoded_audio);
	}

	return failed;
}

/*
 * Test different channel configurations
 */
static int test_channels(void)
{
	int failed = 0;
	int channels_list[] = { 1, 2 };
	int i;

	printf("\n========================================\n");
	printf("Testing Channel Configurations\n");
	printf("========================================\n");

	for (i = 0; i < 2; i++) {
		int channels = channels_list[i];
		int num_samples = 44100;  /* 1 second */
		int16_t *signal = malloc(num_samples * channels * sizeof(int16_t));

		if (!signal) {
			fprintf(stderr, "Failed to allocate signal buffer\n");
			failed++;
			continue;
		}

		generate_sine(signal, num_samples, channels, 44100, 440.0f, 0.5f);

		printf("\n=== Testing %s ===\n", channels == 1 ? "Mono" : "Stereo");

		/* Simplified test */
		struct mux_encoder *enc;
		struct mux_decoder *dec;
		uint8_t *muxed = malloc(256 * 1024);
		size_t decoded_capacity = (size_t)num_samples * channels * 2;
		int16_t *decoded = malloc(decoded_capacity * sizeof(int16_t));
		size_t muxed_size = 0, decoded_size = 0;
		size_t consumed, written;
		int stream_type;

		struct mux_param params[] = {{ .name = "compression", .value.i = 5 }};
		enc = mux_encoder_new(MUX_CODEC_FLAC, 44100, channels, 2, params, 1);

		if (enc) {
			mux_encoder_encode(enc, signal, num_samples * channels * sizeof(int16_t),
					   &consumed, MUX_STREAM_AUDIO);
			mux_encoder_finalize(enc);

			while (muxed_size < 256 * 1024) {
				if (mux_encoder_read(enc, muxed + muxed_size,
						     256 * 1024 - muxed_size, &written) != MUX_OK ||
				    written == 0)
					break;
				muxed_size += written;
			}

			printf("  Encoded: %zu bytes (%.1f%%)\n", muxed_size,
			       muxed_size * 100.0 / (num_samples * channels * sizeof(int16_t)));

			mux_encoder_destroy(enc);
		}

		dec = mux_decoder_new(MUX_CODEC_FLAC, 2, NULL, 0);
		if (dec) {
			mux_decoder_decode(dec, muxed, muxed_size, &consumed);
			mux_decoder_finalize(dec);

			while (decoded_size < decoded_capacity) {
				if (mux_decoder_read(dec, decoded + decoded_size,
						     (decoded_capacity - decoded_size) * sizeof(int16_t),
						     &written, &stream_type) != MUX_OK || written == 0)
					break;
				if (stream_type == MUX_STREAM_AUDIO)
					decoded_size += written / sizeof(int16_t);
			}

			printf("  Decoded: %zu samples\n", decoded_size / channels);

			if (validate_lossless(signal, num_samples, decoded,
					      decoded_size / channels, channels, 256) != 0)
				failed++;

			mux_decoder_destroy(dec);
		}

		free(signal);
		free(muxed);
		free(decoded);
	}

	return failed;
}

int main(void)
{
	int16_t *test_signal;
	int failed = 0;

	printf("=== muxaudio FLAC Codec Validation Test ===\n");
	printf("Sample rate: %d Hz\n", SAMPLE_RATE);
	printf("Channels: %d\n", NUM_CHANNELS);
	printf("Duration: %d seconds\n", DURATION_SEC);
	printf("Total samples: %d\n\n", NUM_SAMPLES);

	/* Allocate test signal buffer */
	test_signal = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	if (!test_signal) {
		fprintf(stderr, "Failed to allocate test signal buffer\n");
		return 1;
	}

	printf("========================================\n");
	printf("Testing Waveforms (Lossless Compression)\n");
	printf("========================================\n");

	/* Test 1: Sine wave */
	generate_sine(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Sine Wave", test_signal) != 0)
		failed++;

	/* Test 2: Sinc pulse */
	generate_sinc(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 4000.0f, 0.5f);
	if (test_waveform("Sinc Pulse (4kHz cutoff)", test_signal) != 0)
		failed++;

	/* Test 3: Triangle wave */
	generate_triangle(test_signal, NUM_SAMPLES, NUM_CHANNELS,
			  SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Triangle Wave", test_signal) != 0)
		failed++;

	/* Test 4: Chirp */
	generate_chirp(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		       SAMPLE_RATE, 100.0f, 8000.0f, 0.4f);
	if (test_waveform("Chirp 100Hz-8kHz", test_signal) != 0)
		failed++;

	/* Test 5: Multi-tone (C Major Chord) */
	generate_silence(test_signal, NUM_SAMPLES, NUM_CHANNELS);
	int16_t *temp = malloc(NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t));
	if (temp) {
		generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
			      SAMPLE_RATE, 261.63f, 0.25f);  /* C4 */
		for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
			test_signal[i] += temp[i];

		generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
			      SAMPLE_RATE, 329.63f, 0.25f);  /* E4 */
		for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
			test_signal[i] += temp[i];

		generate_sine(temp, NUM_SAMPLES, NUM_CHANNELS,
			      SAMPLE_RATE, 392.00f, 0.25f);  /* G4 */
		for (size_t i = 0; i < NUM_SAMPLES * NUM_CHANNELS; i++)
			test_signal[i] += temp[i];

		free(temp);

		if (test_waveform("C Major Chord (C-E-G)", test_signal) != 0)
			failed++;
	}

	/* Test compression levels */
	generate_sine(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 440.0f, 0.5f);
	failed += test_compression_levels(test_signal);

	free(test_signal);

	/* Test different sample rates */
	failed += test_sample_rates();

	/* Test different channel configurations */
	failed += test_channels();

	printf("\n========================================\n");
	if (failed == 0) {
		printf("All tests passed!\n");
		printf("FLAC codec validated: perfect lossless compression\n");
		return 0;
	} else {
		printf("%d test(s) failed\n", failed);
		return 1;
	}
}
