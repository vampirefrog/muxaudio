/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * muxaudio-encode - Encode audio from stdin with side channel data from fd 3
 *
 * Usage: muxaudio-encode [options]
 *
 * Reads raw PCM audio (int16 stereo) from stdin, side channel data from fd 3,
 * and writes the multiplexed stream to stdout.
 */

#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define AUDIO_BUFFER_SIZE 8192
#define SIDE_BUFFER_SIZE 4096
#define OUTPUT_BUFFER_SIZE 16384

struct encoder_config {
	enum mux_codec_type codec;
	int sample_rate;
	int num_channels;
	int bitrate;
	int compression;
};

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [options]\n", prog);
	fprintf(stderr, "\n");
	fprintf(stderr, "Encode audio from stdin with optional side channel data from fd 3\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -c, --codec CODEC      Codec to use (pcm, mp3, vorbis, opus, flac)\n");
	fprintf(stderr, "                         Default: flac\n");
	fprintf(stderr, "  -r, --rate RATE        Sample rate in Hz (default: 44100)\n");
	fprintf(stderr, "  -n, --channels NUM     Number of channels (default: 2)\n");
	fprintf(stderr, "  -b, --bitrate KBPS     Bitrate in kbps for lossy codecs (default: 128)\n");
	fprintf(stderr, "  -l, --level LEVEL      Compression level 0-8 for FLAC (default: 5)\n");
	fprintf(stderr, "  -h, --help             Show this help\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Input:\n");
	fprintf(stderr, "  stdin:  Raw PCM audio (int16, interleaved)\n");
	fprintf(stderr, "  fd 3:   Side channel data (optional)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Output:\n");
	fprintf(stderr, "  stdout: Multiplexed stream\n");
}

static enum mux_codec_type parse_codec(const char *name)
{
	if (strcmp(name, "pcm") == 0)
		return MUX_CODEC_PCM;
	if (strcmp(name, "mp3") == 0)
		return MUX_CODEC_MP3;
	if (strcmp(name, "vorbis") == 0)
		return MUX_CODEC_VORBIS;
	if (strcmp(name, "opus") == 0)
		return MUX_CODEC_OPUS;
	if (strcmp(name, "flac") == 0)
		return MUX_CODEC_FLAC;
	return -1;
}

static int encode_stream(const struct encoder_config *config)
{
	struct mux_encoder *enc;
	struct mux_param params[2];
	int num_params = 0;
	uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
	uint8_t side_buffer[SIDE_BUFFER_SIZE];
	uint8_t output_buffer[OUTPUT_BUFFER_SIZE];
	ssize_t audio_read, side_read;
	size_t consumed, written;
	int ret;
	int audio_eof = 0, side_eof = 0;

	/* Set up codec parameters */
	if (config->codec == MUX_CODEC_MP3 || config->codec == MUX_CODEC_VORBIS ||
	    config->codec == MUX_CODEC_OPUS) {
		params[num_params].name = "bitrate";
		params[num_params].value.i = config->bitrate;
		num_params++;
	} else if (config->codec == MUX_CODEC_FLAC) {
		params[num_params].name = "compression";
		params[num_params].value.i = config->compression;
		num_params++;
	}

	/* Create encoder */
	enc = mux_encoder_new(config->codec, config->sample_rate,
			      config->num_channels, params, num_params);
	if (!enc) {
		fprintf(stderr, "Error: Failed to create encoder\n");
		return 1;
	}

	/* Main encoding loop */
	while (!audio_eof || !side_eof) {
		/* Read audio from stdin */
		if (!audio_eof) {
			audio_read = read(STDIN_FILENO, audio_buffer, sizeof(audio_buffer));
			if (audio_read < 0) {
				perror("read(stdin)");
				mux_encoder_destroy(enc);
				return 1;
			}
			if (audio_read == 0) {
				audio_eof = 1;
			} else {
				ret = mux_encoder_encode(enc, audio_buffer, audio_read,
							 &consumed, MUX_STREAM_AUDIO);
				if (ret != MUX_OK) {
					const struct mux_error_info *err = mux_encoder_get_error(enc);
					fprintf(stderr, "Error: Encode failed: %s\n", err->message);
					mux_encoder_destroy(enc);
					return 1;
				}
			}
		}

		/* Read side channel data from fd 3 */
		if (!side_eof) {
			side_read = read(3, side_buffer, sizeof(side_buffer));
			if (side_read < 0) {
				/* fd 3 not available - that's okay */
				side_eof = 1;
			} else if (side_read == 0) {
				side_eof = 1;
			} else {
				ret = mux_encoder_encode(enc, side_buffer, side_read,
							 &consumed, MUX_STREAM_SIDE_CHANNEL);
				if (ret != MUX_OK) {
					const struct mux_error_info *err = mux_encoder_get_error(enc);
					fprintf(stderr, "Error: Encode failed: %s\n", err->message);
					mux_encoder_destroy(enc);
					return 1;
				}
			}
		}

		/* Write output to stdout */
		while (1) {
			ret = mux_encoder_read(enc, output_buffer, sizeof(output_buffer),
					       &written);
			if (ret == MUX_ERROR_AGAIN || written == 0)
				break;
			if (ret != MUX_OK) {
				fprintf(stderr, "Error: Failed to read encoder output\n");
				mux_encoder_destroy(enc);
				return 1;
			}

			if (write(STDOUT_FILENO, output_buffer, written) != (ssize_t)written) {
				perror("write(stdout)");
				mux_encoder_destroy(enc);
				return 1;
			}
		}
	}

	/* Finalize encoder */
	ret = mux_encoder_finalize(enc);
	if (ret != MUX_OK) {
		const struct mux_error_info *err = mux_encoder_get_error(enc);
		fprintf(stderr, "Error: Finalize failed: %s\n", err->message);
		mux_encoder_destroy(enc);
		return 1;
	}

	/* Write remaining output */
	while (1) {
		ret = mux_encoder_read(enc, output_buffer, sizeof(output_buffer),
				       &written);
		if (ret == MUX_ERROR_AGAIN || written == 0)
			break;
		if (ret != MUX_OK) {
			fprintf(stderr, "Error: Failed to read encoder output\n");
			mux_encoder_destroy(enc);
			return 1;
		}

		if (write(STDOUT_FILENO, output_buffer, written) != (ssize_t)written) {
			perror("write(stdout)");
			mux_encoder_destroy(enc);
			return 1;
		}
	}

	mux_encoder_destroy(enc);
	return 0;
}

int main(int argc, char **argv)
{
	struct encoder_config config = {
		.codec = MUX_CODEC_FLAC,
		.sample_rate = 44100,
		.num_channels = 2,
		.bitrate = 128,
		.compression = 5
	};

	static struct option long_options[] = {
		{"codec",     required_argument, 0, 'c'},
		{"rate",      required_argument, 0, 'r'},
		{"channels",  required_argument, 0, 'n'},
		{"bitrate",   required_argument, 0, 'b'},
		{"level",     required_argument, 0, 'l'},
		{"help",      no_argument,       0, 'h'},
		{0, 0, 0, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:r:n:b:l:h", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			config.codec = parse_codec(optarg);
			if (config.codec == (enum mux_codec_type)-1) {
				fprintf(stderr, "Error: Unknown codec '%s'\n", optarg);
				usage(argv[0]);
				return 1;
			}
			break;
		case 'r':
			config.sample_rate = atoi(optarg);
			if (config.sample_rate <= 0) {
				fprintf(stderr, "Error: Invalid sample rate\n");
				return 1;
			}
			break;
		case 'n':
			config.num_channels = atoi(optarg);
			if (config.num_channels <= 0 || config.num_channels > 8) {
				fprintf(stderr, "Error: Invalid channel count\n");
				return 1;
			}
			break;
		case 'b':
			config.bitrate = atoi(optarg);
			if (config.bitrate <= 0) {
				fprintf(stderr, "Error: Invalid bitrate\n");
				return 1;
			}
			break;
		case 'l':
			config.compression = atoi(optarg);
			if (config.compression < 0 || config.compression > 8) {
				fprintf(stderr, "Error: Invalid compression level\n");
				return 1;
			}
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return encode_stream(&config);
}
