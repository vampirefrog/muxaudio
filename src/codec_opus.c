/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <opus.h>
#include <ogg/ogg.h>

/*
 * Opus codec (push / streaming, OGG-contained)
 *
 * Muxing uses two OGG logical streams (audio serial 1, side channel serial 2)
 * rather than LEB128 framing. OGG pages are emitted straight to the sink as they
 * are produced - no output buffer. On decode, libogg's ogg_sync owns input
 * reassembly (bounded to a page), and decoded PCM / side packets go straight to
 * the emit callback.
 *
 * Opus only accepts whole 20 ms frames, so a fixed one-frame carry buffer holds
 * the sub-frame tail between encode() calls (Layer-3 carry, allocated once).
 */

struct opus_encoder_data {
	OpusEncoder *enc;
	struct mux_encoder *owner;     /* for sink access */

	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_side_stream;

	int64_t packet_count;
	int64_t granule_pos;

	int sample_rate;
	int num_channels;
	int frame_size;                /* samples/channel per Opus frame */

	/* Fixed sub-frame carry: exactly one frame, allocated once at init. */
	int16_t *pending;
	size_t pending_samples;
};

struct opus_decoder_data {
	OpusDecoder *dec;
	struct mux_decoder *owner;     /* for emit access */

	ogg_sync_state oy;
	ogg_stream_state os_audio;
	ogg_stream_state os_side;
	int have_audio_stream;
	int have_side_stream;

	int decoder_inited;
	int sample_rate;
	int num_channels;
};

static const struct mux_param_desc opus_encoder_params[] = {
	{ .name = "bitrate", .description = "Target bitrate in kbps",
	  .type = MUX_PARAM_TYPE_INT, .range.i = { .min = 6, .max = 510, .def = 64 } },
	{ .name = "complexity", .description = "Encoding complexity (0-10)",
	  .type = MUX_PARAM_TYPE_INT, .range.i = { .min = 0, .max = 10, .def = 10 } },
	{ .name = "vbr", .description = "Enable VBR mode",
	  .type = MUX_PARAM_TYPE_BOOL, .range.b = { .def = 1 } }
};

static const int opus_sample_rates[] = { 8000, 12000, 16000, 24000, 48000 };

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
	int r = mux_encoder_emit(enc, og->header, og->header_len);
	if (r)
		return r;
	return mux_encoder_emit(enc, og->body, og->body_len);
}

static int flush_ogg_stream(struct mux_encoder *enc, ogg_stream_state *os)
{
	ogg_page og;
	int r;
	while (ogg_stream_flush(os, &og)) {
		r = emit_ogg_page(enc, &og);
		if (r)
			return r;
	}
	return MUX_OK;
}

static int pageout_ogg_stream(struct mux_encoder *enc, ogg_stream_state *os)
{
	ogg_page og;
	int r;
	while (ogg_stream_pageout(os, &og)) {
		r = emit_ogg_page(enc, &og);
		if (r)
			return r;
	}
	return MUX_OK;
}

static int write_opus_header(ogg_stream_state *os, int sample_rate, int channels,
			     int pre_skip)
{
	unsigned char header[19];
	ogg_packet op;

	memcpy(header, "OpusHead", 8);
	header[8] = 1;
	header[9] = channels;
	header[10] = pre_skip & 0xFF;
	header[11] = (pre_skip >> 8) & 0xFF;
	header[12] = (sample_rate >> 0) & 0xFF;
	header[13] = (sample_rate >> 8) & 0xFF;
	header[14] = (sample_rate >> 16) & 0xFF;
	header[15] = (sample_rate >> 24) & 0xFF;
	header[16] = 0; header[17] = 0;
	header[18] = 0;

	memset(&op, 0, sizeof(op));
	op.packet = header;
	op.bytes = 19;
	op.b_o_s = 1;
	return ogg_stream_packetin(os, &op);
}

static int write_opus_tags(ogg_stream_state *os)
{
	unsigned char header[256];
	ogg_packet op;
	int pos;
	const char *vendor = "muxaudio";
	int vendor_len = strlen(vendor);

	memcpy(header, "OpusTags", 8);
	pos = 8;
	header[pos++] = (vendor_len >> 0) & 0xFF;
	header[pos++] = (vendor_len >> 8) & 0xFF;
	header[pos++] = (vendor_len >> 16) & 0xFF;
	header[pos++] = (vendor_len >> 24) & 0xFF;
	memcpy(header + pos, vendor, vendor_len);
	pos += vendor_len;
	header[pos++] = 0; header[pos++] = 0; header[pos++] = 0; header[pos++] = 0;

	memset(&op, 0, sizeof(op));
	op.packet = header;
	op.bytes = pos;
	op.packetno = 1;
	return ogg_stream_packetin(os, &op);
}

/* ---- encoder ------------------------------------------------------------ */

static int mux_opus_encoder_init(struct mux_encoder *enc, int sample_rate,
				 int num_channels, const struct mux_param *params,
				 int num_params)
{
	struct opus_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 64000, complexity = 10, vbr = 1;
	int opus_error, ret;
	opus_int32 lookahead = 0;
	int pre_skip;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate Opus encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->owner = enc;
	data->sample_rate = sample_rate;
	data->num_channels = num_channels;
	data->frame_size = sample_rate / 50;   /* 20 ms */

	data->pending = malloc((size_t)data->frame_size * num_channels *
			       sizeof(int16_t));
	if (!data->pending) {
		free(data);
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate Opus carry buffer",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	param = find_param(params, num_params, "bitrate");
	if (param) bitrate = param->value.i * 1000;
	param = find_param(params, num_params, "complexity");
	if (param) complexity = param->value.i;
	param = find_param(params, num_params, "vbr");
	if (param) vbr = param->value.b;

	data->enc = opus_encoder_create(sample_rate, num_channels,
					OPUS_APPLICATION_AUDIO, &opus_error);
	if (!data->enc || opus_error != OPUS_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to create Opus encoder", "libopus",
				      opus_error, opus_strerror(opus_error));
		free(data->pending);
		free(data);
		return MUX_ERROR_INIT;
	}

	opus_encoder_ctl(data->enc, OPUS_SET_BITRATE(bitrate));
	opus_encoder_ctl(data->enc, OPUS_SET_COMPLEXITY(complexity));
	opus_encoder_ctl(data->enc, OPUS_SET_VBR(vbr));
	opus_encoder_ctl(data->enc, OPUS_GET_LOOKAHEAD(&lookahead));
	pre_skip = (int)((int64_t)lookahead * 48000 / sample_rate);

	ret = ogg_stream_init(&data->os_audio, 1);
	if (ret != 0) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize OGG audio stream",
				      "libogg", ret, "ogg_stream_init failed");
		opus_encoder_destroy(data->enc);
		free(data->pending);
		free(data);
		return MUX_ERROR_INIT;
	}

	write_opus_header(&data->os_audio, sample_rate, num_channels, pre_skip);
	write_opus_tags(&data->os_audio);

	ret = flush_ogg_stream(enc, &data->os_audio);
	if (ret != MUX_OK) {
		ogg_stream_clear(&data->os_audio);
		opus_encoder_destroy(data->enc);
		free(data->pending);
		free(data);
		return ret;
	}

	enc->codec_data = data;
	return MUX_OK;
}

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
	free(data->pending);
	free(data);
	enc->codec_data = NULL;
}

/* Encode exactly one frame of frame_size samples/channel and emit its pages. */
static int opus_encode_frame(struct mux_encoder *enc,
			     struct opus_encoder_data *data,
			     const int16_t *frame_pcm)
{
	unsigned char packet[4000];
	ogg_packet op;
	int len;

	len = opus_encode(data->enc, frame_pcm, data->frame_size,
			  packet, sizeof(packet));
	if (len < 0) {
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE, "Opus encoding failed",
				      "libopus", len, opus_strerror(len));
		return MUX_ERROR_ENCODE;
	}
	if (len == 0)
		return MUX_OK;

	memset(&op, 0, sizeof(op));
	op.packet = packet;
	op.bytes = len;
	data->granule_pos += data->frame_size;
	op.granulepos = data->granule_pos;
	op.packetno = data->packet_count++;
	ogg_stream_packetin(&data->os_audio, &op);

	return pageout_ogg_stream(enc, &data->os_audio);
}

static int mux_opus_encoder_encode(struct mux_encoder *enc, const void *input,
				   size_t input_size, int stream_type)
{
	struct opus_encoder_data *data;
	const int16_t *pcm;
	size_t in_samples, idx, rem;
	int nch, ret;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	if (input_size == 0)
		return MUX_OK;

	/* Side channel: separate OGG stream, one packet per message. */
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

	/* Audio: combine sub-frame carry with input, encode whole frames,
	 * keep the remainder (< 1 frame) in the fixed carry buffer. */
	pcm = input;
	nch = data->num_channels;
	in_samples = input_size / sizeof(int16_t) / nch;
	idx = 0;

	if (data->pending_samples > 0) {
		size_t need = (size_t)data->frame_size - data->pending_samples;
		size_t take = in_samples < need ? in_samples : need;

		memcpy(data->pending + data->pending_samples * nch, pcm,
		       take * nch * sizeof(int16_t));
		data->pending_samples += take;
		idx += take;

		if (data->pending_samples < (size_t)data->frame_size)
			return MUX_OK;   /* still short of a full frame */

		ret = opus_encode_frame(enc, data, data->pending);
		if (ret)
			return ret;
		data->pending_samples = 0;
	}

	while (in_samples - idx >= (size_t)data->frame_size) {
		ret = opus_encode_frame(enc, data, pcm + idx * nch);
		if (ret)
			return ret;
		idx += data->frame_size;
	}

	rem = in_samples - idx;
	if (rem > 0) {
		memcpy(data->pending, pcm + idx * nch,
		       rem * nch * sizeof(int16_t));
		data->pending_samples = rem;
	}

	return MUX_OK;
}

static int mux_opus_encoder_finalize(struct mux_encoder *enc)
{
	struct opus_encoder_data *data;
	ogg_packet op;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Zero-pad and encode the final sub-frame tail so no samples are lost;
	 * pre_skip / stream end let the decoder trim padding. */
	if (data->pending_samples > 0) {
		int nch = data->num_channels;
		memset(data->pending + data->pending_samples * nch, 0,
		       ((size_t)data->frame_size - data->pending_samples) * nch *
		       sizeof(int16_t));
		ret = opus_encode_frame(enc, data, data->pending);
		if (ret)
			return ret;
		data->pending_samples = 0;
	}

	memset(&op, 0, sizeof(op));
	op.e_o_s = 1;
	op.granulepos = data->granule_pos;
	op.packetno = data->packet_count;
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

static int mux_opus_decoder_init(struct mux_decoder *dec,
				 const struct mux_param *params, int num_params)
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

	data->owner = dec;

	ret = ogg_sync_init(&data->oy);
	if (ret != 0) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize OGG sync", "libogg",
				      ret, "ogg_sync_init failed");
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

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

static int parse_opus_header(const unsigned char *packet, int bytes,
			     int *sample_rate, int *channels)
{
	if (bytes < 19 || memcmp(packet, "OpusHead", 8) != 0)
		return -1;
	*channels = packet[9];
	*sample_rate = packet[12] | (packet[13] << 8) |
		       (packet[14] << 16) | (packet[15] << 24);
	return 0;
}

static int mux_opus_decoder_decode(struct mux_decoder *dec, const void *input,
				   size_t input_size)
{
	struct opus_decoder_data *data;
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
				if (op.bytes >= 19 &&
				    memcmp(op.packet, "OpusHead", 8) == 0) {
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
				if (op.bytes >= 8 &&
				    memcmp(op.packet, "OpusTags", 8) == 0)
					continue;

				if (data->sample_rate > 0 && data->num_channels > 0) {
					int opus_error;
					data->dec = opus_decoder_create(
						data->sample_rate,
						data->num_channels, &opus_error);
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

			if (data->decoder_inited && op.bytes > 0) {
				int16_t pcm_buf[5760 * 2];
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
					size_t sz = (size_t)samples *
						    data->num_channels *
						    sizeof(int16_t);
					ret = mux_decoder_emit(dec, MUX_STREAM_AUDIO,
							       pcm_buf, sz);
					if (ret)
						return ret;
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

static int mux_opus_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

const struct mux_codec_ops mux_codec_opus_ops = {
	.encoder_init = mux_opus_encoder_init,
	.encoder_deinit = mux_opus_encoder_deinit,
	.encoder_encode = mux_opus_encoder_encode,
	.encoder_finalize = mux_opus_encoder_finalize,

	.decoder_init = mux_opus_decoder_init,
	.decoder_deinit = mux_opus_decoder_deinit,
	.decoder_decode = mux_opus_decoder_decode,
	.decoder_finalize = mux_opus_decoder_finalize,

	.encoder_params = opus_encoder_params,
	.encoder_param_count = sizeof(opus_encoder_params) / sizeof(opus_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = opus_sample_rates,
	.sample_rate_count = sizeof(opus_sample_rates) / sizeof(opus_sample_rates[0]),
	.sample_rate_is_range = 0
};
