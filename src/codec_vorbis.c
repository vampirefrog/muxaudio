/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

/*
 * Vorbis codec (push / streaming, OGG-contained)
 *
 * Like Opus: two OGG logical streams (audio serial 1, side channel serial 2).
 * OGG pages are emitted straight to the sink; decoded PCM and side packets go
 * straight to the emit callback. Vorbis accepts arbitrary sample counts per
 * analysis call, so there is no sub-frame carry.
 */

struct vorbis_encoder_data {
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;

	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_side_stream;
	int headers_written;

	int sample_rate;
	int num_channels;
};

struct vorbis_decoder_data {
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;

	ogg_sync_state oy;
	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_audio_stream;
	int have_side_stream;

	int decoder_inited;
};

static const struct mux_param_desc vorbis_encoder_params[] = {
	{ .name = "quality", .description = "Quality (-0.1=low, 1.0=high)",
	  .type = MUX_PARAM_TYPE_FLOAT, .range.f = { .min = -0.1f, .max = 1.0f, .def = 0.4f } },
	{ .name = "bitrate", .description = "Target bitrate in kbps (overrides quality)",
	  .type = MUX_PARAM_TYPE_INT, .range.i = { .min = 32, .max = 500, .def = 0 } }
};

static const struct mux_param *find_param(const struct mux_param *params,
					  int num_params, const char *name)
{
	int i;
	for (i = 0; i < num_params; i++)
		if (strcmp(params[i].name, name) == 0)
			return &params[i];
	return NULL;
}

/* ---- OGG page emission -------------------------------------------------- */

static int emit_ogg_page(struct mux_encoder *enc, ogg_page *og)
{
	int ret = mux_encoder_emit(enc, og->header, og->header_len);
	if (ret)
		return ret;
	return mux_encoder_emit(enc, og->body, og->body_len);
}

static int flush_ogg_stream(struct mux_encoder *enc, ogg_stream_state *os)
{
	ogg_page og;
	int ret;
	while (ogg_stream_flush(os, &og)) {
		ret = emit_ogg_page(enc, &og);
		if (ret)
			return ret;
	}
	return MUX_OK;
}

static int pageout_ogg_stream(struct mux_encoder *enc, ogg_stream_state *os)
{
	ogg_page og;
	int ret;
	while (ogg_stream_pageout(os, &og)) {
		ret = emit_ogg_page(enc, &og);
		if (ret)
			return ret;
	}
	return MUX_OK;
}

/* ---- encoder ------------------------------------------------------------ */

static int vorbis_encoder_init(struct mux_encoder *enc, int sample_rate,
			       int num_channels, const struct mux_param *params,
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

	param = find_param(params, num_params, "quality");
	if (param)
		quality = param->value.f;
	param = find_param(params, num_params, "bitrate");
	if (param)
		bitrate = param->value.i;

	vorbis_info_init(&data->vi);

	if (bitrate > 0)
		ret = vorbis_encode_init(&data->vi, num_channels, sample_rate,
					 -1, bitrate * 1000, -1);
	else
		ret = vorbis_encode_init_vbr(&data->vi, num_channels,
					     sample_rate, quality);

	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize Vorbis encoder",
				      "libvorbis", ret, "vorbis_encode_init failed");
		vorbis_info_clear(&data->vi);
		free(data);
		return MUX_ERROR_INIT;
	}

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
	ogg_packet header, header_comm, header_code;
	vorbis_analysis_headerout(&data->vd, &data->vc,
				  &header, &header_comm, &header_code);
	ogg_stream_packetin(&data->os_audio, &header);
	ogg_stream_packetin(&data->os_audio, &header_comm);
	ogg_stream_packetin(&data->os_audio, &header_code);

	ret = flush_ogg_stream(enc, &data->os_audio);
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

static int vorbis_encoder_encode(struct mux_encoder *enc, const void *input,
				 size_t input_size, int stream_type)
{
	struct vorbis_encoder_data *data;
	int ret;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0)
		return MUX_OK;

	/* Side channel data: separate OGG stream, one packet per message. */
	if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
		ogg_packet op;

		if (!data->have_side_stream) {
			ret = ogg_stream_init(&data->os_side, 2);
			if (ret != 0) {
				mux_encoder_set_error(enc, MUX_ERROR_INIT,
						      "Failed to init side channel stream",
						      "libogg", ret, NULL);
				return MUX_ERROR_INIT;
			}
			data->have_side_stream = 1;

			memset(&op, 0, sizeof(op));
			op.packet = (unsigned char *)"SIDE";
			op.bytes = 4;
			op.b_o_s = 1;
			ogg_stream_packetin(&data->os_side, &op);
			ret = flush_ogg_stream(enc, &data->os_side);
			if (ret != MUX_OK)
				return ret;
		}

		memset(&op, 0, sizeof(op));
		op.packet = (unsigned char *)input;
		op.bytes = input_size;
		ogg_stream_packetin(&data->os_side, &op);
		return pageout_ogg_stream(enc, &data->os_side);
	}

	/* Audio data: encode with Vorbis */
	const int16_t *pcm = input;
	size_t num_samples = input_size / sizeof(int16_t) / data->num_channels;

	float **buffer = vorbis_analysis_buffer(&data->vd, num_samples);
	for (size_t i = 0; i < num_samples; i++)
		for (int ch = 0; ch < data->num_channels; ch++)
			buffer[ch][i] = pcm[i * data->num_channels + ch] / 32768.0f;
	vorbis_analysis_wrote(&data->vd, num_samples);

	while (vorbis_analysis_blockout(&data->vd, &data->vb) == 1) {
		ogg_packet op;
		vorbis_analysis(&data->vb, NULL);
		vorbis_bitrate_addblock(&data->vb);

		while (vorbis_bitrate_flushpacket(&data->vd, &op)) {
			ogg_stream_packetin(&data->os_audio, &op);
			ret = pageout_ogg_stream(enc, &data->os_audio);
			if (ret != MUX_OK)
				return ret;
		}
	}

	return MUX_OK;
}

static int vorbis_encoder_finalize(struct mux_encoder *enc)
{
	struct vorbis_encoder_data *data;
	ogg_packet op;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Signal end of audio and flush remaining blocks. */
	vorbis_analysis_wrote(&data->vd, 0);
	while (vorbis_analysis_blockout(&data->vd, &data->vb) == 1) {
		vorbis_analysis(&data->vb, NULL);
		vorbis_bitrate_addblock(&data->vb);
		while (vorbis_bitrate_flushpacket(&data->vd, &op))
			ogg_stream_packetin(&data->os_audio, &op);
	}

	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
	ogg_stream_packetin(&data->os_audio, &op);
	ret = flush_ogg_stream(enc, &data->os_audio);
	if (ret != MUX_OK)
		return ret;

	if (data->have_side_stream) {
		memset(&op, 0, sizeof(op));
		op.e_o_s = 1;
		ogg_stream_packetin(&data->os_side, &op);
		ret = flush_ogg_stream(enc, &data->os_side);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/* ---- decoder ------------------------------------------------------------ */

static int vorbis_decoder_init(struct mux_decoder *dec,
			       const struct mux_param *params, int num_params)
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

	ret = ogg_sync_init(&data->oy);
	if (ret != 0) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize OGG sync", "libogg",
				      ret, "ogg_sync_init failed");
		free(data);
		return MUX_ERROR_INIT;
	}

	vorbis_info_init(&data->vi);
	vorbis_comment_init(&data->vc);

	dec->codec_data = data;
	return MUX_OK;
}

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

/* Emit one pcmout run of planar float as interleaved int16 (fixed stack buf). */
static int vorbis_emit_pcm(struct mux_decoder *dec, float **pcm, int samples,
			   int channels)
{
	int done = 0;

	while (done < samples) {
		int16_t tmp[2048];
		int cap = (int)(sizeof(tmp) / sizeof(tmp[0])) / channels;
		int m = samples - done;
		int i, ch, ret;

		if (m > cap)
			m = cap;

		for (i = 0; i < m; i++) {
			for (ch = 0; ch < channels; ch++) {
				float val = pcm[ch][done + i] * 32768.0f;
				if (val > 32767.0f)
					val = 32767.0f;
				if (val < -32768.0f)
					val = -32768.0f;
				tmp[i * channels + ch] = (int16_t)val;
			}
		}

		ret = mux_decoder_emit(dec, MUX_STREAM_AUDIO, tmp,
				       (size_t)m * channels * sizeof(int16_t));
		if (ret)
			return ret;

		done += m;
	}
	return MUX_OK;
}

static int vorbis_decoder_decode(struct mux_decoder *dec, const void *input,
				 size_t input_size)
{
	struct vorbis_decoder_data *data;
	char *buffer;
	ogg_page og;
	ogg_packet op;
	int ret;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0)
		return MUX_OK;

	buffer = ogg_sync_buffer(&data->oy, input_size);
	if (!buffer) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM, "ogg_sync_buffer failed",
				      "libogg", 0, NULL);
		return MUX_ERROR_NOMEM;
	}
	memcpy(buffer, input, input_size);
	ogg_sync_wrote(&data->oy, input_size);

	while (ogg_sync_pageout(&data->oy, &og) == 1) {
		int serial = ogg_page_serialno(&og);

		if (!data->have_audio_stream && serial == 1) {
			ogg_stream_init(&data->os_audio, serial);
			data->have_audio_stream = 1;
		} else if (!data->have_side_stream && serial == 2) {
			ogg_stream_init(&data->os_side, serial);
			data->have_side_stream = 1;
		}

		if (data->have_audio_stream && serial == 1)
			ogg_stream_pagein(&data->os_audio, &og);
		else if (data->have_side_stream && serial == 2)
			ogg_stream_pagein(&data->os_side, &og);
	}

	if (data->have_audio_stream) {
		while (ogg_stream_packetout(&data->os_audio, &op) == 1) {
			if (!data->decoder_inited) {
				ret = vorbis_synthesis_headerin(&data->vi, &data->vc, &op);
				if (ret == 0) {
					continue;   /* header packet, need more */
				} else if (ret < 0) {
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

			if (data->decoder_inited) {
				if (vorbis_synthesis(&data->vb, &op) == 0) {
					float **pcm;
					int samples;

					vorbis_synthesis_blockin(&data->vd, &data->vb);
					while ((samples = vorbis_synthesis_pcmout(&data->vd, &pcm)) > 0) {
						ret = vorbis_emit_pcm(dec, pcm, samples,
								      data->vi.channels);
						if (ret)
							return ret;
						vorbis_synthesis_read(&data->vd, samples);
					}
				}
			}
		}
	}

	if (data->have_side_stream) {
		while (ogg_stream_packetout(&data->os_side, &op) == 1) {
			if (op.b_o_s)
				continue;   /* skip "SIDE" header packet */
			if (op.e_o_s && op.bytes == 0)
				continue;
			ret = mux_decoder_emit(dec, MUX_STREAM_SIDE_CHANNEL,
					       op.packet, op.bytes);
			if (ret)
				return ret;
		}
	}

	return MUX_OK;
}

static int vorbis_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

static const int vorbis_sample_rates[] = { 1000, 384000 };  /* Min/max range */

const struct mux_codec_ops mux_codec_vorbis_ops = {
	.encoder_init = vorbis_encoder_init,
	.encoder_deinit = vorbis_encoder_deinit,
	.encoder_encode = vorbis_encoder_encode,
	.encoder_finalize = vorbis_encoder_finalize,

	.decoder_init = vorbis_decoder_init,
	.decoder_deinit = vorbis_decoder_deinit,
	.decoder_decode = vorbis_decoder_decode,
	.decoder_finalize = vorbis_decoder_finalize,

	.encoder_params = vorbis_encoder_params,
	.encoder_param_count = sizeof(vorbis_encoder_params) / sizeof(vorbis_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = vorbis_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
