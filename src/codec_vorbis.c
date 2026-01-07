/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

/*
 * Vorbis encoder state
 * Uses OGG container with two streams: audio and side channel
 */
struct vorbis_encoder_data {
	/* Vorbis encoder state */
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;

	/* OGG stream states */
	ogg_stream_state os_audio;     /* Audio stream */
	ogg_stream_state os_side;      /* Side channel stream */
	int have_side_stream;          /* Flag if side stream is initialized */

	/* Track if headers have been written */
	int headers_written;

	/* Sample rate and channels (needed for vorbis_analysis) */
	int sample_rate;
	int num_channels;
};

/*
 * Vorbis decoder state
 */
struct vorbis_decoder_data {
	/* Vorbis decoder state */
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;

	/* OGG sync state for parsing input */
	ogg_sync_state oy;

	/* OGG stream states (discovered from input) */
	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_audio_stream;
	int have_side_stream;

	/* Track decoder initialization */
	int decoder_inited;
};

/*
 * Vorbis encoder parameters
 */
static const struct mux_param_desc vorbis_encoder_params[] = {
	{
		.name = "quality",
		.description = "Quality (-0.1=low, 1.0=high)",
		.type = MUX_PARAM_TYPE_FLOAT,
		.range.f = { .min = -0.1f, .max = 1.0f, .def = 0.4f }
	},
	{
		.name = "bitrate",
		.description = "Target bitrate in kbps (overrides quality)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 32, .max = 500, .def = 0 }
	}
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
 * Vorbis encoder initialization
 */
static int vorbis_encoder_init(struct mux_encoder *enc,
				int sample_rate,
				int num_channels,
				const struct mux_param *params,
				int num_params)
{
	struct vorbis_encoder_data *data;
	const struct mux_param *param;
	float quality = 0.4f;
	int bitrate = 0;
	int ret;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate Vorbis encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	/* Get parameters */
	param = find_param(params, num_params, "quality");
	if (param)
		quality = param->value.f;

	param = find_param(params, num_params, "bitrate");
	if (param)
		bitrate = param->value.i;

	/* Initialize vorbis encoder */
	vorbis_info_init(&data->vi);

	if (bitrate > 0) {
		/* Use ABR mode */
		ret = vorbis_encode_init(&data->vi, num_channels,
					 sample_rate, -1, bitrate * 1000, -1);
	} else {
		/* Use VBR quality mode */
		ret = vorbis_encode_init_vbr(&data->vi, num_channels,
					     sample_rate, quality);
	}

	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize Vorbis encoder",
				      "libvorbis", ret, "vorbis_encode_init failed");
		vorbis_info_clear(&data->vi);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Initialize vorbis analysis structures */
	vorbis_comment_init(&data->vc);
	vorbis_comment_add_tag(&data->vc, "ENCODER", "muxaudio");

	ret = vorbis_analysis_init(&data->vd, &data->vi);
	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize Vorbis analysis",
				      "libvorbis", ret, "vorbis_analysis_init failed");
		vorbis_comment_clear(&data->vc);
		vorbis_info_clear(&data->vi);
		free(data);
		return MUX_ERROR_INIT;
	}

	ret = vorbis_block_init(&data->vd, &data->vb);
	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize Vorbis block",
				      "libvorbis", ret, "vorbis_block_init failed");
		vorbis_dsp_clear(&data->vd);
		vorbis_comment_clear(&data->vc);
		vorbis_info_clear(&data->vi);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Initialize OGG stream for audio (serial number 1) */
	ret = ogg_stream_init(&data->os_audio, 1);
	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize OGG audio stream",
				      "libogg", ret, "ogg_stream_init failed");
		vorbis_block_clear(&data->vb);
		vorbis_dsp_clear(&data->vd);
		vorbis_comment_clear(&data->vc);
		vorbis_info_clear(&data->vi);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Write Vorbis headers */
	ogg_packet header;
	ogg_packet header_comm;
	ogg_packet header_code;

	vorbis_analysis_headerout(&data->vd, &data->vc,
				  &header, &header_comm, &header_code);

	ogg_stream_packetin(&data->os_audio, &header);
	ogg_stream_packetin(&data->os_audio, &header_comm);
	ogg_stream_packetin(&data->os_audio, &header_code);

	/* Flush headers to output */
	ret = flush_ogg_stream(&enc->output, &data->os_audio);
	if (ret != MUX_OK) {
		ogg_stream_clear(&data->os_audio);
		vorbis_block_clear(&data->vb);
		vorbis_dsp_clear(&data->vd);
		vorbis_comment_clear(&data->vc);
		vorbis_info_clear(&data->vi);
		free(data);
		return ret;
	}

	data->headers_written = 1;

	enc->codec_data = data;
	return MUX_OK;
}

/*
 * Vorbis encoder deinitialization
 */
static void vorbis_encoder_deinit(struct mux_encoder *enc)
{
	struct vorbis_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	ogg_stream_clear(&data->os_audio);
	if (data->have_side_stream)
		ogg_stream_clear(&data->os_side);

	vorbis_block_clear(&data->vb);
	vorbis_dsp_clear(&data->vd);
	vorbis_comment_clear(&data->vc);
	vorbis_info_clear(&data->vi);

	free(data);
	enc->codec_data = NULL;
}

/*
 * Vorbis encoder encode
 * For audio: compress with Vorbis and write to OGG audio stream
 * For side channel: write to separate OGG stream
 */
static int vorbis_encoder_encode(struct mux_encoder *enc,
				 const void *input,
				 size_t input_size,
				 size_t *input_consumed,
				 int stream_type)
{
	struct vorbis_encoder_data *data;
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

	/* Audio data: encode with Vorbis */
	const int16_t *pcm = input;
	size_t num_samples = input_size / sizeof(int16_t) / data->num_channels;

	/* Get buffer for analysis */
	float **buffer = vorbis_analysis_buffer(&data->vd, num_samples);

	/* Convert interleaved int16 PCM to planar float */
	for (size_t i = 0; i < num_samples; i++) {
		for (int ch = 0; ch < data->num_channels; ch++) {
			buffer[ch][i] = pcm[i * data->num_channels + ch] / 32768.0f;
		}
	}

	/* Tell the library how many samples we wrote */
	vorbis_analysis_wrote(&data->vd, num_samples);

	/* Process blocks and write packets */
	while (vorbis_analysis_blockout(&data->vd, &data->vb) == 1) {
		vorbis_analysis(&data->vb, NULL);
		vorbis_bitrate_addblock(&data->vb);

		ogg_packet op;
		while (vorbis_bitrate_flushpacket(&data->vd, &op)) {
			ogg_stream_packetin(&data->os_audio, &op);

			/* Write out any completed pages */
			ret = pageout_ogg_stream(&enc->output, &data->os_audio);
			if (ret != MUX_OK)
				return ret;
		}
	}

	*input_consumed = input_size;
	return MUX_OK;
}

/*
 * Vorbis encoder read
 * Reads multiplexed OGG output data
 */
static int vorbis_encoder_read(struct mux_encoder *enc,
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
 * Vorbis encoder finalize
 * Flushes any buffered audio and closes OGG streams
 */
static int vorbis_encoder_finalize(struct mux_encoder *enc)
{
	struct vorbis_encoder_data *data;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Signal end of audio data */
	vorbis_analysis_wrote(&data->vd, 0);

	/* Flush remaining blocks */
	while (vorbis_analysis_blockout(&data->vd, &data->vb) == 1) {
		vorbis_analysis(&data->vb, NULL);
		vorbis_bitrate_addblock(&data->vb);

		ogg_packet op;
		while (vorbis_bitrate_flushpacket(&data->vd, &op)) {
			ogg_stream_packetin(&data->os_audio, &op);
		}
	}

	/* Mark audio stream as ended */
	ogg_packet op;
	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
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
 * Vorbis decoder initialization
 */
static int vorbis_decoder_init(struct mux_decoder *dec,
				const struct mux_param *params,
				int num_params)
{
	struct vorbis_decoder_data *data;
	int ret;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate Vorbis decoder data",
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

	/* Initialize Vorbis info and comment */
	vorbis_info_init(&data->vi);
	vorbis_comment_init(&data->vc);

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * Vorbis decoder deinitialization
 */
static void vorbis_decoder_deinit(struct mux_decoder *dec)
{
	struct vorbis_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->have_audio_stream)
		ogg_stream_clear(&data->os_audio);
	if (data->have_side_stream)
		ogg_stream_clear(&data->os_side);

	if (data->decoder_inited) {
		vorbis_block_clear(&data->vb);
		vorbis_dsp_clear(&data->vd);
	}

	vorbis_comment_clear(&data->vc);
	vorbis_info_clear(&data->vi);
	ogg_sync_clear(&data->oy);

	free(data);
	dec->codec_data = NULL;
}

/*
 * Vorbis decoder decode
 * Reads OGG pages and decodes Vorbis audio
 */
static int vorbis_decoder_decode(struct mux_decoder *dec,
				 const void *input,
				 size_t input_size,
				 size_t *input_consumed)
{
	struct vorbis_decoder_data *data;
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
			/* Initialize decoder on first packet */
			if (!data->decoder_inited) {
				ret = vorbis_synthesis_headerin(&data->vi, &data->vc, &op);
				if (ret == 0) {
					/* Got a header packet, need more */
					continue;
				} else if (ret < 0) {
					/* Not a header, initialize synthesis */
					ret = vorbis_synthesis_init(&data->vd, &data->vi);
					if (ret != 0) {
						mux_decoder_set_error(dec, MUX_ERROR_INIT,
								      "vorbis_synthesis_init failed",
								      "libvorbis", ret, NULL);
						return MUX_ERROR_INIT;
					}

					ret = vorbis_block_init(&data->vd, &data->vb);
					if (ret != 0) {
						mux_decoder_set_error(dec, MUX_ERROR_INIT,
								      "vorbis_block_init failed",
								      "libvorbis", ret, NULL);
						vorbis_dsp_clear(&data->vd);
						return MUX_ERROR_INIT;
					}

					data->decoder_inited = 1;
				}
			}

			/* Decode audio packet */
			if (data->decoder_inited) {
				ret = vorbis_synthesis(&data->vb, &op);
				if (ret == 0) {
					vorbis_synthesis_blockin(&data->vd, &data->vb);

					/* Extract decoded PCM */
					float **pcm;
					int samples;
					while ((samples = vorbis_synthesis_pcmout(&data->vd, &pcm)) > 0) {
						/* Convert planar float to interleaved int16 */
						int channels = data->vi.channels;
						size_t pcm_size = samples * channels * sizeof(int16_t);
						int16_t *pcm_buf = malloc(pcm_size);

						if (!pcm_buf) {
							mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
									      "Failed to allocate PCM buffer",
									      NULL, 0, NULL);
							return MUX_ERROR_NOMEM;
						}

						for (int i = 0; i < samples; i++) {
							for (int ch = 0; ch < channels; ch++) {
								float val = pcm[ch][i] * 32768.0f;
								if (val > 32767.0f)
									val = 32767.0f;
								if (val < -32768.0f)
									val = -32768.0f;
								pcm_buf[i * channels + ch] = (int16_t)val;
							}
						}

						/* Write to audio output buffer */
						ret = mux_buffer_write(&dec->audio_output,
								       pcm_buf, pcm_size);
						free(pcm_buf);

						if (ret != MUX_OK)
							return ret;

						vorbis_synthesis_read(&data->vd, samples);
					}
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
 * Vorbis decoder read
 * Reads decoded audio or side channel data
 */
static int vorbis_decoder_read(struct mux_decoder *dec,
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
 * Vorbis decoder finalize
 * Flushes any remaining data
 */
static int vorbis_decoder_finalize(struct mux_decoder *dec)
{
	struct vorbis_decoder_data *data;

	if (!dec)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Any remaining data should have been processed */
	return MUX_OK;
}

/*
 * Vorbis sample rate constraints (supports any rate)
 */
static const int vorbis_sample_rates[] = { 1000, 384000 };  /* Min/max range */

/*
 * Vorbis codec operations
 */
const struct mux_codec_ops mux_codec_vorbis_ops = {
	.encoder_init = vorbis_encoder_init,
	.encoder_deinit = vorbis_encoder_deinit,
	.encoder_encode = vorbis_encoder_encode,
	.encoder_read = vorbis_encoder_read,
	.encoder_finalize = vorbis_encoder_finalize,

	.decoder_init = vorbis_decoder_init,
	.decoder_deinit = vorbis_decoder_deinit,
	.decoder_decode = vorbis_decoder_decode,
	.decoder_read = vorbis_decoder_read,
	.decoder_finalize = vorbis_decoder_finalize,

	.encoder_params = vorbis_encoder_params,
	.encoder_param_count = sizeof(vorbis_encoder_params) / sizeof(vorbis_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = vorbis_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
