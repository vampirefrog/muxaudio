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

static int test_waveform(const char *name, int16_t *test_signal,
			 float min_snr_db, int max_offset)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	uint8_t *muxed_buffer;
	int16_t *decoded_audio;
	size_t muxed_capacity = 256 * 1024;
	size_t decoded_capacity = NUM_SAMPLES * NUM_CHANNELS * 2;
	size_t input_consumed, output_written, total_muxed = 0;
	size_t total_decoded = 0;
	int stream_type;
	int ret;

	printf("\n=== Testing %s ===\n", name);
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

	/* Create encoder */
	struct mux_param params[] = {
		{ .name = "quality", .value.f = 0.4f }
	};

	enc = mux_encoder_new(MUX_CODEC_VORBIS, SAMPLE_RATE, NUM_CHANNELS,
			      params, 1);
	if (!enc) {
		fprintf(stderr, "Failed to create encoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Encode in chunks */
	size_t chunk_size = 4096;
	size_t offset = 0;
	size_t input_bytes = NUM_SAMPLES * NUM_CHANNELS * sizeof(int16_t);

	printf("Encoding %zu bytes...\n", input_bytes);
	fflush(stdout);

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

	/* Finalize encoder to flush any buffered data */
	printf("Finalizing encoder...\n");
	fflush(stdout);
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
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;
		total_muxed += output_written;
	}

	printf("Encoded: %zu bytes input → %zu bytes output (%.1f%% of original)\n",
	       input_bytes, total_muxed, total_muxed * 100.0 / input_bytes);
	fflush(stdout);

	mux_encoder_destroy(enc);

	/* Create decoder */
	printf("Creating decoder...\n");
	fflush(stdout);
	dec = mux_decoder_new(MUX_CODEC_VORBIS, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Failed to create decoder\n");
		free(muxed_buffer);
		free(decoded_audio);
		return -1;
	}

	/* Decode in chunks */
	printf("Decoding %zu bytes...\n", total_muxed);
	fflush(stdout);

	offset = 0;
	chunk_size = 1024;

	int decode_no_progress = 0;
	while (offset < total_muxed && decode_no_progress < 10) {
		size_t to_decode = total_muxed - offset;
		if (to_decode > chunk_size)
			to_decode = chunk_size;

		size_t offset_before = offset;
		ret = mux_decoder_decode(dec,
					 muxed_buffer + offset,
					 to_decode,
					 &input_consumed);
		if (ret != MUX_OK) {
			const struct mux_error_info *err = mux_decoder_get_error(dec);
			if (err->code != MUX_OK)
				fprintf(stderr, "Decode error: %s\n", err->message);
			/* Continue anyway, errors might be recoverable */
		}
		offset += input_consumed;

		/* Detect stuck decoder */
		if (offset == offset_before && input_consumed == 0)
			decode_no_progress++;
		else
			decode_no_progress = 0;
	}

	if (decode_no_progress >= 10) {
		printf("Warning: Decoder made no progress, %zu/%zu bytes decoded\n",
		       offset, total_muxed);
	}

	printf("Decode complete: fed %zu bytes\n", offset);
	fflush(stdout);

	/* Finalize decoder to flush any buffered data */
	printf("Finalizing decoder...\n");
	fflush(stdout);
	ret = mux_decoder_finalize(dec);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		if (err->code != MUX_OK)
			fprintf(stderr, "Finalize error: %s\n", err->message);
	}

	printf("Reading output...\n");
	fflush(stdout);

	/* Read decoded output */
	int no_progress_count = 0;
	while (total_decoded < decoded_capacity && no_progress_count < 100) {
		size_t before = total_decoded;
		size_t requested = (decoded_capacity - total_decoded) * sizeof(int16_t);

		ret = mux_decoder_read(dec,
				       decoded_audio + total_decoded,
				       requested,
				       &output_written,
				       &stream_type);
		if (ret == MUX_ERROR_AGAIN)
			break;
		if (ret != MUX_OK)
			break;
		if (output_written == 0)
			break;

		if (stream_type == MUX_STREAM_AUDIO)
			total_decoded += output_written / sizeof(int16_t);

		/* Detect infinite loops */
		if (total_decoded == before)
			no_progress_count++;
		else
			no_progress_count = 0;
	}

	printf("Decoded: %zu samples\n", total_decoded / NUM_CHANNELS);

	mux_decoder_destroy(dec);

	/* Validate */
	printf("\nValidation:\n");
	ret = validate_lossy_audio(test_signal, NUM_SAMPLES,
				   decoded_audio, total_decoded / NUM_CHANNELS,
				   NUM_CHANNELS,
				   min_snr_db, max_offset);

	free(muxed_buffer);
	free(decoded_audio);

	return ret;
}

int main(void)
{
	int16_t *test_signal;
	int failed = 0;

	printf("=== muxaudio Vorbis Codec Validation Test ===\n");
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

	/*
	 * Test 1: Sine wave (simple tone, Vorbis should handle well)
	 */
	generate_sine(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Sine Wave", test_signal, 15.0f, 1024) != 0)
		failed++;

	/*
	 * Test 2: Sinc pulse (band-limited impulse, very challenging)
	 */
	generate_sinc(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		      SAMPLE_RATE, 4000.0f, 0.5f);
	if (test_waveform("Sinc Pulse (4kHz cutoff)", test_signal, -5.0f, 1024) != 0)
		failed++;

	/*
	 * Test 3: Triangle wave (harmonics)
	 */
	generate_triangle(test_signal, NUM_SAMPLES, NUM_CHANNELS,
			  SAMPLE_RATE, 440.0f, 0.5f);
	if (test_waveform("440 Hz Triangle Wave", test_signal, -5.0f, 1024) != 0)
		failed++;

	/*
	 * Test 4: Chirp (frequency sweep)
	 */
	generate_chirp(test_signal, NUM_SAMPLES, NUM_CHANNELS,
		       SAMPLE_RATE, 100.0f, 8000.0f, 0.4f);
	if (test_waveform("Chirp 100Hz-8kHz", test_signal, 0.0f, 1024) != 0)
		failed++;

	/*
	 * Test 5: Multi-tone (several frequencies)
	 */
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

		if (test_waveform("C Major Chord (C-E-G)", test_signal, 5.0f, 1024) != 0)
			failed++;
	}

	free(test_signal);

	printf("\n========================================\n");
	if (failed == 0) {
		printf("All tests passed!\n");
		return 0;
	} else {
		printf("%d test(s) failed\n", failed);
		return 1;
	}
}
