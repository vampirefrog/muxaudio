/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * demux - Decode multiplexed stream to audio and side channel data
 *
 * Reads a multiplexed stream from stdin, writes raw PCM audio (int16,
 * interleaved) to stdout and side channel data to fd 3.
 */

#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define INPUT_BUFFER_SIZE 16384

struct decoder_config {
	enum mux_codec_type codec;
	int num_streams;
	int verbose;
};

struct demux_stats {
	size_t total_audio;
	size_t total_side;
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
	fprintf(stderr, "  -s, --streams NUM      Number of streams: 1=passthrough, 2=mux (default: 2)\n");
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

/* Emit: audio -> stdout, side channel -> fd 3. Returns 0 on success. */
static int emit_cb(void *user, int stream_type, const void *data, size_t size)
{
	struct demux_stats *stats = user;

	if (size == 0)
		return 0;

	if (stream_type == MUX_STREAM_AUDIO) {
		if (write(STDOUT_FILENO, data, size) != (ssize_t)size) {
			perror("write(stdout)");
			return 1;
		}
		stats->total_audio += size;
	} else {
		ssize_t w = write(3, data, size);   /* fd 3 optional */
		if (w > 0)
			stats->total_side += w;
	}
	return 0;
}

static int decode_stream(const struct decoder_config *config)
{
	struct mux_decoder *dec;
	struct demux_stats stats = { 0, 0 };
	uint8_t input_buffer[INPUT_BUFFER_SIZE];
	ssize_t input_read;
	int ret;

	dec = mux_decoder_new(config->codec, config->num_streams, NULL, 0,
			      emit_cb, &stats);
	if (!dec) {
		fprintf(stderr, "Error: Failed to create decoder\n");
		return 1;
	}

	for (;;) {
		input_read = read(STDIN_FILENO, input_buffer, sizeof(input_buffer));
		if (input_read < 0) {
			perror("read(stdin)");
			mux_decoder_destroy(dec);
			return 1;
		}
		if (input_read == 0)
			break;

		ret = mux_decoder_decode(dec, input_buffer, input_read);
		if (ret != MUX_OK) {
			const struct mux_error_info *err = mux_decoder_get_error(dec);
			fprintf(stderr, "Error: Decode failed: %s\n", err->message);
			mux_decoder_destroy(dec);
			return 1;
		}
	}

	ret = mux_decoder_finalize(dec);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_decoder_get_error(dec);
		fprintf(stderr, "Error: Finalize failed: %s\n", err->message);
		mux_decoder_destroy(dec);
		return 1;
	}

	if (config->verbose)
		fprintf(stderr, "Decoded: %zu bytes audio, %zu bytes side channel\n",
			stats.total_audio, stats.total_side);

	mux_decoder_destroy(dec);
	return 0;
}

int main(int argc, char **argv)
{
	struct decoder_config config = {
		.codec = MUX_CODEC_FLAC,
		.num_streams = 2,
		.verbose = 0
	};

	static struct option long_options[] = {
		{"codec",     required_argument, 0, 'c'},
		{"streams",   required_argument, 0, 's'},
		{"verbose",   no_argument,       0, 'v'},
		{"help",      no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:s:vh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			if (mux_codec_from_name(optarg, &config.codec) != MUX_OK) {
				fprintf(stderr, "Error: Unknown codec '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 's':
			config.num_streams = atoi(optarg);
			if (config.num_streams != 1 && config.num_streams != 2) {
				fprintf(stderr, "Error: Invalid stream count (must be 1 or 2)\n");
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
