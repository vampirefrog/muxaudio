/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * muxaudio-decode - Decode multiplexed stream to audio and side channel data
 *
 * Usage: muxaudio-decode [options]
 *
 * Reads multiplexed stream from stdin, writes raw PCM audio to stdout,
 * and side channel data to fd 3.
 */

#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define INPUT_BUFFER_SIZE 16384
#define OUTPUT_BUFFER_SIZE 8192

struct decoder_config {
	enum mux_codec_type codec;
	int verbose;
};

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Decode multiplexed stream to audio and side channel data\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c, --codec CODEC      Codec to use (pcm, mp3, vorbis, opus, flac, aac)\n");
	fprintf(stderr, "                         Default: flac\n");
	fprintf(stderr, "  -v, --verbose          Print stream information to stderr\n");
	fprintf(stderr, "  -h, --help             Show this help\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Input:\n");
	fprintf(stderr, "  stdin:  Multiplexed stream\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Output:\n");
	fprintf(stderr, "  stdout: Raw PCM audio (int16, interleaved)\n");
	fprintf(stderr, "  fd 3:   Side channel data (if present in stream)\n");
}


static int decode_stream(const struct decoder_config *config)
{
	struct mux_decoder *dec;
	uint8_t input_buffer[INPUT_BUFFER_SIZE];
	uint8_t output_buffer[OUTPUT_BUFFER_SIZE];
	ssize_t input_read;
	size_t consumed, written;
	int stream_type;
	int ret;
	int input_eof = 0;
	size_t total_audio = 0, total_side = 0;

	/* Create decoder */
	dec = mux_decoder_new(config->codec, NULL, 0);
	if (!dec) {
		fprintf(stderr, "Error: Failed to create decoder\n");
		return 1;
	}

	/* Main decoding loop */
	while (!input_eof) {
		/* Read input from stdin */
		input_read = read(STDIN_FILENO, input_buffer, sizeof(input_buffer));
		if (input_read < 0) {
			perror("read(stdin)");
			mux_decoder_destroy(dec);
			return 1;
		}
		if (input_read == 0) {
			input_eof = 1;
			/* Finalize decoder */
			ret = mux_decoder_finalize(dec);
			if (ret != MUX_OK) {
				const struct mux_error_info *err = mux_decoder_get_error(dec);
				fprintf(stderr, "Error: Finalize failed: %s\n", err->message);
				mux_decoder_destroy(dec);
				return 1;
			}
		} else {
			/* Decode input */
			ret = mux_decoder_decode(dec, input_buffer, input_read, &consumed);
			if (ret != MUX_OK) {
				const struct mux_error_info *err = mux_decoder_get_error(dec);
				fprintf(stderr, "Error: Decode failed: %s\n", err->message);
				mux_decoder_destroy(dec);
				return 1;
			}
		}

		/* Read and write all available output */
		while (1) {
			ret = mux_decoder_read(dec, output_buffer, sizeof(output_buffer),
					       &written, &stream_type);
			if (ret == MUX_ERROR_AGAIN || written == 0)
				break;
			if (ret != MUX_OK) {
				fprintf(stderr, "Error: Failed to read decoder output\n");
				mux_decoder_destroy(dec);
				return 1;
			}

			/* Write to appropriate output */
			if (stream_type == MUX_STREAM_AUDIO) {
				if (write(STDOUT_FILENO, output_buffer, written) != (ssize_t)written) {
					perror("write(stdout)");
					mux_decoder_destroy(dec);
					return 1;
				}
				total_audio += written;
			} else if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
				/* Try to write to fd 3, but don't fail if it's not available */
				ssize_t w = write(3, output_buffer, written);
				if (w > 0)
					total_side += w;
			}
		}
	}

	if (config->verbose) {
		fprintf(stderr, "Decoded: %zu bytes audio, %zu bytes side channel\n",
			total_audio, total_side);
	}

	mux_decoder_destroy(dec);
	return 0;
}

int main(int argc, char **argv)
{
	struct decoder_config config = {
		.codec = MUX_CODEC_FLAC,
		.verbose = 0
	};

	static struct option long_options[] = {
		{"codec",     required_argument, 0, 'c'},
		{"verbose",   no_argument,       0, 'v'},
		{"help",      no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:vh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (mux_codec_from_name(optarg, &config.codec) != MUX_OK) {
				fprintf(stderr, "Error: Unknown codec '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 'v':
			config.verbose = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return decode_stream(&config);
}
