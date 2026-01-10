/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <opus.h>
#include <ogg/ogg.h>

/*
 * Opus encoder state
 * Uses OGG container with two streams: audio and side channel
 */
struct opus_encoder_data {
	/* Opus encoder state */
	OpusEncoder *enc;

	/* OGG stream states */
	ogg_stream_state os_audio;     /* Audio stream */
	ogg_stream_state os_side;      /* Side channel stream */
	int have_side_stream;          /* Flag if side stream is initialized */

	/* Track if headers have been written */
	int headers_written;

	/* Opus packet number (for granule position) */
	int64_t packet_count;
	int64_t granule_pos;

	/* Sample rate and channels */
	int sample_rate;
	int num_channels;

	/* Frame size in samples */
	int frame_size;
};

/*
 * Opus decoder state
 */
struct opus_decoder_data {
	/* Opus decoder state */
	OpusDecoder *dec;

	/* OGG sync state for parsing input */
	ogg_sync_state oy;

	/* OGG stream states (discovered from input) */
	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_audio_stream;
	int have_side_stream;

	/* Track decoder initialization */
	int decoder_inited;

	/* Sample rate info */
	int sample_rate;
	int num_channels;
};

/*
 * Opus encoder parameters
 */
static const struct mux_param_desc opus_encoder_params[] = {
	{
		.name = "bitrate",
		.description = "Target bitrate in kbps",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 6, .max = 510, .def = 64 }
	},
	{
		.name = "complexity",
		.description = "Encoding complexity (0-10)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 0, .max = 10, .def = 10 }
	},
	{
		.name = "vbr",
		.description = "Enable VBR mode",
		.type = MUX_PARAM_TYPE_BOOL,
		.range.b = { .def = 1 }
	}
};

/*
 * Opus supported sample rates (discrete list)
 */
static const int opus_sample_rates[] = {
	8000, 12000, 16000, 24000, 48000
};

/*
 * Helper to find parameter value by name
 */
static const struct mux_param *find_param(const struct mux_param *params,
					  int num_params,
					  const char *name)
{
	int i;

	for (i = 0; i < num_params; i++) {
		if (strcmp(params[i].name, name) == 0)
			return &params[i];
	}
	return NULL;
}

/*
 * Helper to write OGG pages to output buffer
 */
static int write_ogg_page(struct mux_buffer *output, ogg_page *og)
{
	int ret;

	ret = mux_buffer_write(output, og->header, og->header_len);
	if (ret != MUX_OK)
		return ret;

	ret = mux_buffer_write(output, og->body, og->body_len);
	if (ret != MUX_OK)
		return ret;

	return MUX_OK;
}

/*
 * Helper to flush OGG stream pages to output buffer
 */
static int flush_ogg_stream(struct mux_buffer *output, ogg_stream_state *os)
{
	ogg_page og;
	int ret;

	while (ogg_stream_flush(os, &og)) {
		ret = write_ogg_page(output, &og);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * Helper to write out OGG stream pages to output buffer
 */
static int pageout_ogg_stream(struct mux_buffer *output, ogg_stream_state *os)
{
	ogg_page og;
	int ret;

	while (ogg_stream_pageout(os, &og)) {
		ret = write_ogg_page(output, &og);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * Write OpusHead header packet
 */
static int write_opus_header(ogg_stream_state *os, int sample_rate, int channels)
{
	unsigned char header[19];
	ogg_packet op;

	/* OpusHead structure */
	memcpy(header, "OpusHead", 8);
	header[8] = 1;                    /* Version */
	header[9] = channels;             /* Channel count */
	header[10] = 0; header[11] = 0;   /* Pre-skip (little-endian) */
	/* Input sample rate (little-endian) */
	header[12] = (sample_rate >> 0) & 0xFF;
	header[13] = (sample_rate >> 8) & 0xFF;
	header[14] = (sample_rate >> 16) & 0xFF;
	header[15] = (sample_rate >> 24) & 0xFF;
	header[16] = 0; header[17] = 0;   /* Output gain */
	header[18] = 0;                   /* Channel mapping family */

	memset(&op, 0, sizeof(op));
	op.packet = header;
	op.bytes = 19;
	op.b_o_s = 1;
	op.e_o_s = 0;
	op.granulepos = 0;
	op.packetno = 0;

	return ogg_stream_packetin(os, &op);
}

/*
 * Write OpusTags header packet
 */
static int write_opus_tags(ogg_stream_state *os)
{
	unsigned char header[256];
	ogg_packet op;
	int pos = 0;
	const char *vendor = "muxaudio";
	int vendor_len = strlen(vendor);

	/* OpusTags structure */
	memcpy(header, "OpusTags", 8);
	pos = 8;

	/* Vendor string length (little-endian) */
	header[pos++] = (vendor_len >> 0) & 0xFF;
	header[pos++] = (vendor_len >> 8) & 0xFF;
	header[pos++] = (vendor_len >> 16) & 0xFF;
	header[pos++] = (vendor_len >> 24) & 0xFF;

	/* Vendor string */
	memcpy(header + pos, vendor, vendor_len);
	pos += vendor_len;

	/* User comment list count (0) */
	header[pos++] = 0;
	header[pos++] = 0;
	header[pos++] = 0;
	header[pos++] = 0;

	memset(&op, 0, sizeof(op));
	op.packet = header;
	op.bytes = pos;
	op.b_o_s = 0;
	op.e_o_s = 0;
	op.granulepos = 0;
	op.packetno = 1;

	return ogg_stream_packetin(os, &op);
}

/*
 * Opus encoder initialization
 */
static int mux_opus_encoder_init(struct mux_encoder *enc,
			      int sample_rate,
			      int num_channels,
			      const struct mux_param *params,
			      int num_params)
{
	struct opus_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 64000;
	int complexity = 10;
	int vbr = 1;
	int opus_error;
	int ret;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate Opus encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	/* Opus frame size: 20ms at given sample rate */
	data->frame_size = sample_rate / 50;

	/* Get parameters */
	param = find_param(params, num_params, "bitrate");
	if (param)
		bitrate = param->value.i * 1000;

	param = find_param(params, num_params, "complexity");
	if (param)
		complexity = param->value.i;

	param = find_param(params, num_params, "vbr");
	if (param)
		vbr = param->value.b;

	/* Create Opus encoder */
	data->enc = opus_encoder_create(sample_rate, num_channels,
					OPUS_APPLICATION_AUDIO, &opus_error);
	if (!data->enc || opus_error != OPUS_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to create Opus encoder",
				      "libopus", opus_error, opus_strerror(opus_error));
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Configure encoder */
	opus_encoder_ctl(data->enc, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(data->enc, OPUS_SET_COMPLEXITY(complexity));
	opus_encoder_ctl(data->enc, OPUS_SET_VBR(vbr));

	/* Initialize OGG stream for audio (serial number 1) */
	ret = ogg_stream_init(&data->os_audio, 1);
	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize OGG audio stream",
				      "libogg", ret, "ogg_stream_init failed");
		opus_encoder_destroy(data->enc);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Write Opus headers */
	write_opus_header(&data->os_audio, sample_rate, num_channels);
	write_opus_tags(&data->os_audio);

	/* Flush headers to output */
	ret = flush_ogg_stream(&enc->output, &data->os_audio);
	if (ret != MUX_OK) {
		ogg_stream_clear(&data->os_audio);
		opus_encoder_destroy(data->enc);
		free(data);
		return ret;
	}

	data->headers_written = 1;
	data->packet_count = 0;
	data->granule_pos = 0;

	enc->codec_data = data;
	return MUX_OK;
}

/*
 * Opus encoder deinitialization
 */
static void mux_opus_encoder_deinit(struct mux_encoder *enc)
{
	struct opus_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	ogg_stream_clear(&data->os_audio);
	if (data->have_side_stream)
		ogg_stream_clear(&data->os_side);

	if (data->enc)
		opus_encoder_destroy(data->enc);

	free(data);
	enc->codec_data = NULL;
}

/*
 * Opus encoder encode
 * For audio: compress with Opus and write to OGG audio stream
 * For side channel: write to separate OGG stream
 */
static int mux_opus_encoder_encode(struct mux_encoder *enc,
				const void *input,
				size_t input_size,
				size_t *input_consumed,
				int stream_type)
{
	struct opus_encoder_data *data;
	int ret;

	if (!enc || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0) {
		*input_consumed = 0;
		return MUX_OK;
	}

	/* Side channel data: write to separate OGG stream */
	if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
		/* Initialize side stream on first use */
		if (!data->have_side_stream) {
			ret = ogg_stream_init(&data->os_side, 2);
			if (ret != 0) {
				mux_encoder_set_error(enc, MUX_ERROR_INIT,
						      "Failed to init side channel stream",
						      "libogg", ret, NULL);
				return MUX_ERROR_INIT;
			}
			data->have_side_stream = 1;

			/* Write a header packet for side channel stream */
			ogg_packet op;
			memset(&op, 0, sizeof(op));
			op.packet = (unsigned char *)"SIDE";
			op.bytes = 4;
			op.b_o_s = 1;
			op.packetno = 0;
			ogg_stream_packetin(&data->os_side, &op);
			flush_ogg_stream(&enc->output, &data->os_side);
		}

		/* Create OGG packet from side channel data */
		ogg_packet op;
		memset(&op, 0, sizeof(op));
		op.packet = (unsigned char *)input;
		op.bytes = input_size;
		op.b_o_s = 0;
		op.e_o_s = 0;

		ogg_stream_packetin(&data->os_side, &op);

		/* Write out any completed pages */
		ret = pageout_ogg_stream(&enc->output, &data->os_side);
		if (ret != MUX_OK)
			return ret;

		*input_consumed = input_size;
		return MUX_OK;
	}

	/* Audio data: encode with Opus */
	const int16_t *pcm = input;
	size_t num_samples = input_size / sizeof(int16_t) / data->num_channels;

	/* Process in frame_size chunks */
	size_t samples_consumed = 0;
	while (samples_consumed + data->frame_size <= num_samples) {
		unsigned char opus_packet[4000];
		int opus_len;

		opus_len = opus_encode(data->enc,
				       pcm + (samples_consumed * data->num_channels),
				       data->frame_size,
				       opus_packet,
				       sizeof(opus_packet));

		if (opus_len < 0) {
			mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
					      "Opus encoding failed",
					      "libopus", opus_len, opus_strerror(opus_len));
			return MUX_ERROR_ENCODE;
		}

		if (opus_len > 0) {
			/* Create OGG packet */
			ogg_packet op;
			memset(&op, 0, sizeof(op));
			op.packet = opus_packet;
			op.bytes = opus_len;
			op.b_o_s = 0;
			op.e_o_s = 0;
			data->granule_pos += data->frame_size;
			op.granulepos = data->granule_pos;
			op.packetno = data->packet_count++;

			ogg_stream_packetin(&data->os_audio, &op);

			/* Write out any completed pages */
			ret = pageout_ogg_stream(&enc->output, &data->os_audio);
			if (ret != MUX_OK)
				return ret;
		}

		samples_consumed += data->frame_size;
	}

	*input_consumed = samples_consumed * data->num_channels * sizeof(int16_t);
	return MUX_OK;
}

/*
 * Opus encoder read
 * Reads multiplexed OGG output data
 */
static int mux_opus_encoder_read(struct mux_encoder *enc,
			      void *output,
			      size_t output_size,
			      size_t *output_written)
{
	size_t bytes_read;
	int ret;

	if (!enc || !output || !output_written)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&enc->output, output, output_size,
			      &bytes_read);
	if (ret != MUX_OK) {
		*output_written = 0;
		return ret;
	}

	*output_written = bytes_read;
	return MUX_OK;
}

/*
 * Opus encoder finalize
 * Flushes OGG streams
 */
static int mux_opus_encoder_finalize(struct mux_encoder *enc)
{
	struct opus_encoder_data *data;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Mark audio stream as ended */
	ogg_packet op;
	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
	op.granulepos = data->granule_pos;
	op.packetno = data->packet_count;
	ogg_stream_packetin(&data->os_audio, &op);

	/* Flush audio stream */
	ret = flush_ogg_stream(&enc->output, &data->os_audio);
	if (ret != MUX_OK)
		return ret;

	/* Flush side stream if present */
	if (data->have_side_stream) {
		memset(&op, 0, sizeof(op));
		op.e_o_s = 1;
		ogg_stream_packetin(&data->os_side, &op);

		ret = flush_ogg_stream(&enc->output, &data->os_side);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * Opus decoder initialization
 */
static int mux_opus_decoder_init(struct mux_decoder *dec,
			      const struct mux_param *params,
			      int num_params)
{
	struct opus_decoder_data *data;
	int ret;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate Opus decoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	/* Initialize OGG sync state */
	ret = ogg_sync_init(&data->oy);
	if (ret != 0) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize OGG sync",
				      "libogg", ret, "ogg_sync_init failed");
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * Opus decoder deinitialization
 */
static void mux_opus_decoder_deinit(struct mux_decoder *dec)
{
	struct opus_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->have_audio_stream)
		ogg_stream_clear(&data->os_audio);
	if (data->have_side_stream)
		ogg_stream_clear(&data->os_side);

	if (data->decoder_inited)
		opus_decoder_destroy(data->dec);

	ogg_sync_clear(&data->oy);

	free(data);
	dec->codec_data = NULL;
}

/*
 * Parse OpusHead header to get sample rate and channels
 */
static int parse_opus_header(const unsigned char *packet, int bytes,
			      int *sample_rate, int *channels)
{
	if (bytes < 19)
		return -1;

	if (memcmp(packet, "OpusHead", 8) != 0)
		return -1;

	*channels = packet[9];
	*sample_rate = packet[12] | (packet[13] << 8) |
		       (packet[14] << 16) | (packet[15] << 24);

	return 0;
}

/*
 * Opus decoder decode
 * Reads OGG pages and decodes Opus audio
 */
static int mux_opus_decoder_decode(struct mux_decoder *dec,
				const void *input,
				size_t input_size,
				size_t *input_consumed)
{
	struct opus_decoder_data *data;
	char *buffer;
	ogg_page og;
	ogg_packet op;
	int ret;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Submit input data to OGG sync */
	buffer = ogg_sync_buffer(&data->oy, input_size);
	if (!buffer) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "ogg_sync_buffer failed",
				      "libogg", 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	memcpy(buffer, input, input_size);
	ogg_sync_wrote(&data->oy, input_size);

	/* Process OGG pages */
	while (ogg_sync_pageout(&data->oy, &og) == 1) {
		int serial = ogg_page_serialno(&og);

		/* Identify stream by serial number */
		if (!data->have_audio_stream && serial == 1) {
			/* Audio stream */
			ogg_stream_init(&data->os_audio, serial);
			data->have_audio_stream = 1;
		} else if (!data->have_side_stream && serial == 2) {
			/* Side channel stream */
			ogg_stream_init(&data->os_side, serial);
			data->have_side_stream = 1;
		}

		/* Submit page to appropriate stream */
		if (data->have_audio_stream && serial == 1) {
			ogg_stream_pagein(&data->os_audio, &og);
		} else if (data->have_side_stream && serial == 2) {
			ogg_stream_pagein(&data->os_side, &og);
		}
	}

	/* Process audio stream packets */
	if (data->have_audio_stream) {
		while (ogg_stream_packetout(&data->os_audio, &op) == 1) {
			/* Initialize decoder on first header packet */
			if (!data->decoder_inited) {
				/* Check if this is OpusHead */
				if (op.bytes >= 19 && memcmp(op.packet, "OpusHead", 8) == 0) {
					ret = parse_opus_header(op.packet, op.bytes,
							       &data->sample_rate,
							       &data->num_channels);
					if (ret < 0) {
						mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
								      "Invalid OpusHead",
								      NULL, 0, NULL);
						return MUX_ERROR_FORMAT;
					}
					continue;
				}

				/* Skip OpusTags */
				if (op.bytes >= 8 && memcmp(op.packet, "OpusTags", 8) == 0) {
					continue;
				}

				/* Now initialize decoder */
				if (data->sample_rate > 0 && data->num_channels > 0) {
					int opus_error;
					data->dec = opus_decoder_create(data->sample_rate,
									data->num_channels,
									&opus_error);
					if (!data->dec || opus_error != OPUS_OK) {
						mux_decoder_set_error(dec, MUX_ERROR_INIT,
								      "opus_decoder_create failed",
								      "libopus", opus_error,
								      opus_strerror(opus_error));
						return MUX_ERROR_INIT;
					}
					data->decoder_inited = 1;
				}
			}

			/* Decode audio packet */
			if (data->decoder_inited) {
				int16_t pcm_buf[5760 * 2];  /* Max Opus frame size * stereo */
				int samples;

				samples = opus_decode(data->dec, op.packet, op.bytes,
						      pcm_buf, 5760, 0);

				if (samples < 0) {
					mux_decoder_set_error(dec, MUX_ERROR_DECODE,
							      "opus_decode failed",
							      "libopus", samples,
							      opus_strerror(samples));
					return MUX_ERROR_DECODE;
				}

				if (samples > 0) {
					size_t pcm_size = samples * data->num_channels * sizeof(int16_t);
					ret = mux_buffer_write(&dec->audio_output,
							       pcm_buf, pcm_size);
					if (ret != MUX_OK)
						return ret;
				}
			}
		}
	}

	/* Process side channel packets */
	if (data->have_side_stream) {
		while (ogg_stream_packetout(&data->os_side, &op) == 1) {
			/* Skip header packet */
			if (op.b_o_s)
				continue;

			/* Write side channel data to output */
			ret = mux_buffer_write(&dec->side_output,
					       op.packet, op.bytes);
			if (ret != MUX_OK)
				return ret;
		}
	}

	*input_consumed = input_size;
	return MUX_OK;
}

/*
 * Opus decoder read
 * Reads decoded audio or side channel data
 */
static int mux_opus_decoder_read(struct mux_decoder *dec,
			      void *output,
			      size_t output_size,
			      size_t *output_written,
			      int *stream_type)
{
	size_t bytes_read;
	int ret;

	if (!dec || !output || !output_written || !stream_type)
		return MUX_ERROR_INVAL;

	/* Try audio buffer first */
	ret = mux_buffer_read(&dec->audio_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_AUDIO;
		return MUX_OK;
	}

	/* Try side channel buffer */
	ret = mux_buffer_read(&dec->side_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_SIDE_CHANNEL;
		return MUX_OK;
	}

	/* No data available */
	*output_written = 0;
	return MUX_ERROR_AGAIN;
}

/*
 * Opus decoder finalize
 * Flushes any remaining data
 */
static int mux_opus_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * Opus codec operations
 */
const struct mux_codec_ops mux_codec_opus_ops = {
	.encoder_init = mux_opus_encoder_init,
	.encoder_deinit = mux_opus_encoder_deinit,
	.encoder_encode = mux_opus_encoder_encode,
	.encoder_read = mux_opus_encoder_read,
	.encoder_finalize = mux_opus_encoder_finalize,

	.decoder_init = mux_opus_decoder_init,
	.decoder_deinit = mux_opus_decoder_deinit,
	.decoder_decode = mux_opus_decoder_decode,
	.decoder_read = mux_opus_decoder_read,
	.decoder_finalize = mux_opus_decoder_finalize,

	.encoder_params = opus_encoder_params,
	.encoder_param_count = sizeof(opus_encoder_params) / sizeof(opus_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = opus_sample_rates,
	.sample_rate_count = sizeof(opus_sample_rates) / sizeof(opus_sample_rates[0]),
	.sample_rate_is_range = 0
};
