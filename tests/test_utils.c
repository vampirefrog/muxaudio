/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_utils.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Waveform generators
 */

void generate_sine(int16_t *buffer, size_t num_samples, int channels,
		   int sample_rate, float frequency, float amplitude)
{
	size_t i, ch;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		int16_t sample = (int16_t)(sin(2.0 * M_PI * frequency * t) *
					   32767.0 * amplitude);
		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_sinc(int16_t *buffer, size_t num_samples, int channels,
		   int sample_rate, float cutoff_freq, float amplitude)
{
	size_t i, ch;
	int center = num_samples / 2;
	float fc = cutoff_freq / sample_rate;  /* Normalized frequency */

	for (i = 0; i < num_samples; i++) {
		float x = (float)((int)i - center);
		float sinc_val;

		if (x == 0.0f) {
			sinc_val = 2.0f * fc;
		} else {
			float arg = 2.0f * M_PI * fc * x;
			sinc_val = sin(arg) / (M_PI * x);
		}

		/* Apply Hamming window to reduce ringing */
		float window = 0.54f - 0.46f * cos(2.0f * M_PI * i / num_samples);
		int16_t sample = (int16_t)(sinc_val * window * 32767.0 * amplitude);

		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_square(int16_t *buffer, size_t num_samples, int channels,
		     int sample_rate, float frequency, float amplitude)
{
	size_t i, ch;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		float phase = fmod(t * frequency, 1.0f);
		int16_t sample = (int16_t)((phase < 0.5f ? 1.0f : -1.0f) *
					   32767.0 * amplitude);
		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_triangle(int16_t *buffer, size_t num_samples, int channels,
		       int sample_rate, float frequency, float amplitude)
{
	size_t i, ch;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		float phase = fmod(t * frequency, 1.0f);
		float tri = (phase < 0.5f) ? (4.0f * phase - 1.0f) :
					     (3.0f - 4.0f * phase);
		int16_t sample = (int16_t)(tri * 32767.0 * amplitude);
		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_sawtooth(int16_t *buffer, size_t num_samples, int channels,
		       int sample_rate, float frequency, float amplitude)
{
	size_t i, ch;
	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		float phase = fmod(t * frequency, 1.0f);
		float saw = 2.0f * phase - 1.0f;
		int16_t sample = (int16_t)(saw * 32767.0 * amplitude);
		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_noise(int16_t *buffer, size_t num_samples, int channels,
		    float amplitude)
{
	size_t i, ch;
	for (i = 0; i < num_samples; i++) {
		for (ch = 0; ch < (size_t)channels; ch++) {
			float r = (float)rand() / RAND_MAX * 2.0f - 1.0f;
			buffer[i * channels + ch] = (int16_t)(r * 32767.0 * amplitude);
		}
	}
}

void generate_chirp(int16_t *buffer, size_t num_samples, int channels,
		    int sample_rate, float start_freq, float end_freq,
		    float amplitude)
{
	size_t i, ch;
	float duration = (float)num_samples / sample_rate;

	for (i = 0; i < num_samples; i++) {
		float t = (float)i / sample_rate;
		/* Instantaneous phase for linear chirp */
		float phase = 2.0f * M_PI * (start_freq * t +
					     0.5f * (end_freq - start_freq) * t * t / duration);
		int16_t sample = (int16_t)(sin(phase) * 32767.0 * amplitude);
		for (ch = 0; ch < (size_t)channels; ch++)
			buffer[i * channels + ch] = sample;
	}
}

void generate_silence(int16_t *buffer, size_t num_samples, int channels)
{
	memset(buffer, 0, num_samples * channels * sizeof(int16_t));
}

/*
 * Audio comparison and validation
 */

float calculate_snr(const int16_t *original, const int16_t *decoded,
		    size_t num_samples)
{
	double signal_power = 0.0;
	double noise_power = 0.0;
	size_t i;

	if (num_samples == 0)
		return 0.0f;

	for (i = 0; i < num_samples; i++) {
		double sig = original[i];
		double noise = original[i] - decoded[i];
		signal_power += sig * sig;
		noise_power += noise * noise;
	}

	if (noise_power < 1e-10)
		return 100.0f;  /* Essentially perfect */

	return 10.0f * log10(signal_power / noise_power);
}

float calculate_rmse(const int16_t *original, const int16_t *decoded,
		     size_t num_samples)
{
	double sum_sq_error = 0.0;
	size_t i;

	if (num_samples == 0)
		return 0.0f;

	for (i = 0; i < num_samples; i++) {
		double error = original[i] - decoded[i];
		sum_sq_error += error * error;
	}

	return sqrt(sum_sq_error / num_samples);
}

int find_time_offset(const int16_t *original, size_t original_samples,
		     const int16_t *decoded, size_t decoded_samples,
		     int max_offset)
{
	int best_offset = 0;
	double best_correlation = -1e30;
	int offset;

	/* Limit search range */
	size_t search_len = original_samples;
	if (search_len > 4096)
		search_len = 4096;  /* Limit for performance */

	for (offset = -max_offset; offset <= max_offset; offset++) {
		double correlation = 0.0;
		size_t count = 0;
		size_t i;

		for (i = 0; i < search_len; i++) {
			int orig_idx = (int)i;
			int dec_idx = (int)i + offset;

			if (orig_idx >= 0 && orig_idx < (int)original_samples &&
			    dec_idx >= 0 && dec_idx < (int)decoded_samples) {
				correlation += (double)original[orig_idx] *
					       (double)decoded[dec_idx];
				count++;
			}
		}

		if (count > 0) {
			correlation /= count;
			if (correlation > best_correlation) {
				best_correlation = correlation;
				best_offset = offset;
			}
		}
	}

	return best_offset;
}

int validate_lossy_audio(const int16_t *original, size_t original_samples,
			 const int16_t *decoded, size_t decoded_samples,
			 int channels,
			 float min_snr_db,
			 int max_time_offset)
{
	int offset;
	float snr;
	size_t compare_samples;
	const int16_t *orig_ptr;
	const int16_t *dec_ptr;

	/* Convert to element counts for interleaved data */
	size_t original_elements = original_samples * channels;
	size_t decoded_elements = decoded_samples * channels;

	/* Fast path: check if buffers are identical (lossless case) */
	if (original_elements == decoded_elements) {
		if (memcmp(original, decoded, original_elements * sizeof(int16_t)) == 0) {
			/* Perfect match - skip offset detection */
			printf("  Time offset: 0 samples (0.00 ms @ 44.1kHz)\n");
			printf("  SNR: 100.00 dB (minimum: %.2f dB)\n", min_snr_db);
			printf("  PASS\n");
			return 0;
		}
	}

	/* Find time alignment (works on elements) */
	offset = find_time_offset(original, original_elements,
				  decoded, decoded_elements,
				  max_time_offset * channels);

	/* Convert offset back to samples for display */
	int offset_samples = offset / channels;
	printf("  Time offset: %d samples (%.2f ms @ 44.1kHz)\n",
	       offset_samples, offset_samples * 1000.0f / 44100.0f);

	/* Align signals for comparison */
	if (offset >= 0) {
		orig_ptr = original;
		dec_ptr = decoded + offset;
		compare_samples = (original_elements < decoded_elements - offset) ?
				  original_elements : (decoded_elements - offset);
	} else {
		orig_ptr = original - offset;
		dec_ptr = decoded;
		compare_samples = (original_elements + offset < decoded_elements) ?
				  (original_elements + offset) : decoded_elements;
	}

	/* compare_samples is already in elements */

	if (compare_samples < 100) {
		printf("  Error: Not enough samples to compare\n");
		return -1;
	}

	/* Calculate SNR */
	snr = calculate_snr(orig_ptr, dec_ptr, compare_samples);
	printf("  SNR: %.2f dB (minimum: %.2f dB)\n", snr, min_snr_db);

	/* Validate */
	if (abs(offset_samples) > max_time_offset) {
		printf("  FAIL: Time offset exceeds maximum\n");
		return -1;
	}

	if (snr < min_snr_db) {
		printf("  FAIL: SNR below minimum\n");
		return -1;
	}

	printf("  PASS\n");
	return 0;
}

void print_audio_stats(const int16_t *original, size_t original_samples,
		       const int16_t *decoded, size_t decoded_samples,
		       int channels)
{
	float snr, rmse;
	int offset;
	size_t compare_samples;
	const int16_t *orig_ptr;
	const int16_t *dec_ptr;

	printf("\n=== Audio Comparison Statistics ===\n");
	printf("Original samples: %zu\n", original_samples);
	printf("Decoded samples: %zu\n", decoded_samples);
	printf("Channels: %d\n", channels);

	/* Find offset */
	offset = find_time_offset(original, original_samples,
				  decoded, decoded_samples, 2048);
	printf("Time offset: %d samples (%.2f ms @ 44.1kHz)\n",
	       offset, offset * 1000.0f / 44100.0f);

	/* Align for comparison */
	if (offset >= 0) {
		orig_ptr = original;
		dec_ptr = decoded + offset * channels;
		compare_samples = (original_samples < decoded_samples - offset) ?
				  original_samples : (decoded_samples - offset);
	} else {
		orig_ptr = original - offset * channels;
		dec_ptr = decoded;
		compare_samples = (original_samples + offset < decoded_samples) ?
				  (original_samples + offset) : decoded_samples;
	}

	compare_samples *= channels;

	if (compare_samples > 0) {
		snr = calculate_snr(orig_ptr, dec_ptr, compare_samples);
		rmse = calculate_rmse(orig_ptr, dec_ptr, compare_samples);

		printf("SNR: %.2f dB\n", snr);
		printf("RMSE: %.2f\n", rmse);
	}
	printf("\n");
}
