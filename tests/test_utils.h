/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TEST_UTILS_H
#define TEST_UTILS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Waveform generation
 */

/* Generate sine wave */
void generate_sine(int16_t *buffer, size_t num_samples, int channels,
		   int sample_rate, float frequency, float amplitude);

/* Generate band-limited sinc pulse */
void generate_sinc(int16_t *buffer, size_t num_samples, int channels,
		   int sample_rate, float cutoff_freq, float amplitude);

/* Generate square wave */
void generate_square(int16_t *buffer, size_t num_samples, int channels,
		     int sample_rate, float frequency, float amplitude);

/* Generate triangle wave */
void generate_triangle(int16_t *buffer, size_t num_samples, int channels,
		       int sample_rate, float frequency, float amplitude);

/* Generate sawtooth wave */
void generate_sawtooth(int16_t *buffer, size_t num_samples, int channels,
		       int sample_rate, float frequency, float amplitude);

/* Generate white noise */
void generate_noise(int16_t *buffer, size_t num_samples, int channels,
		    float amplitude);

/* Generate frequency sweep (chirp) */
void generate_chirp(int16_t *buffer, size_t num_samples, int channels,
		    int sample_rate, float start_freq, float end_freq,
		    float amplitude);

/* Generate silence */
void generate_silence(int16_t *buffer, size_t num_samples, int channels);

/*
 * Audio comparison and validation
 */

/* Calculate Signal-to-Noise Ratio in dB */
float calculate_snr(const int16_t *original, const int16_t *decoded,
		    size_t num_samples);

/* Calculate Root Mean Square Error */
float calculate_rmse(const int16_t *original, const int16_t *decoded,
		     size_t num_samples);

/* Find time offset between two signals using cross-correlation
 * Returns offset in samples (negative = decoded is ahead) */
int find_time_offset(const int16_t *original, size_t original_samples,
		     const int16_t *decoded, size_t decoded_samples,
		     int max_offset);

/* Validate lossy codec output
 * Returns 0 if validation passes, -1 otherwise */
int validate_lossy_audio(const int16_t *original, size_t original_samples,
			 const int16_t *decoded, size_t decoded_samples,
			 int channels,
			 float min_snr_db,
			 int max_time_offset);

/* Print comparison statistics */
void print_audio_stats(const int16_t *original, size_t original_samples,
		       const int16_t *decoded, size_t decoded_samples,
		       int channels);

#endif /* TEST_UTILS_H */
