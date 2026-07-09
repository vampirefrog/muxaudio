/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>

/*
 * AAC codec (push / streaming, LEB128-framed, raw AAC frames)
 *
 * Encoder: fdk-aac consumes whole frames (frameLength * channels samples), so a
 * fixed one-frame carry buffer holds the sub-frame tail between encode() calls
 * (Layer-3 carry). Each encoded frame is emitted as one LEB128 audio frame.
 *
 * Decoder: raw AAC frames have no in-band length, so we rely on the LEB128
 * framing: reassemble one AAC frame (marked by MUX_EMIT_FRAME_END) into a fixed
 * buffer, then Fill + DecodeFrame it and emit the PCM. Side channel forwards.
 */

#define AAC_FRAME_CAP  16384   /* max reassembled raw AAC frame */

struct aac_encoder_data {
	HANDLE_AACENCODER enc;
	struct mux_encoder *owner;

	int sample_rate;
	int num_channels;
	int bitrate;

	int16_t *frame_buf;        /* one-frame carry, input_buf_size samples */
	int frame_samples;         /* input_buf_size (interleaved samples/frame) */
	int pending;               /* interleaved samples currently carried */

	uint8_t *output_buf;
	int output_buf_size;
};

struct aac_decoder_data {
	HANDLE_AACDECODER dec;
	struct mux_leb128_parser parser;

	uint8_t frame[AAC_FRAME_CAP];   /* one reassembled raw AAC frame */
	size_t frame_len;
	int overflow;

	int sample_rate;
	int num_channels;
};

static const struct mux_param_desc aac_encoder_params[] = {
	{ .name = "bitrate", .description = "Bitrate in kbps",
	  .type = MUX_PARAM_TYPE_INT, .range.i = { .min = 8, .max = 512, .def = 128 } },
	{ .name = "profile", .description = "AAC profile (2=LC, 5=HE, 29=HEv2)",
	  .type = MUX_PARAM_TYPE_INT, .range.i = { .min = 2, .max = 29, .def = 2 } }
};

static const int aac_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000
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

/* ---- encoder ------------------------------------------------------------ */

static int mux_aac_encoder_init(struct mux_encoder *enc, int sample_rate,
				int num_channels, const struct mux_param *params,
				int num_params)
{
	struct aac_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 128000, profile = 2;
	AACENC_ERROR err;
	CHANNEL_MODE channel_mode;
	AACENC_InfoStruct info;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->owner = enc;
	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	param = find_param(params, num_params, "bitrate");
	if (param) bitrate = param->value.i * 1000;
	param = find_param(params, num_params, "profile");
	if (param) profile = param->value.i;
	data->bitrate = bitrate;

	switch (num_channels) {
	case 1: channel_mode = MODE_1; break;
	case 2: channel_mode = MODE_2; break;
	default:
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "Unsupported channel count for AAC",
				      NULL, 0, NULL);
		free(data);
		return MUX_ERROR_INVAL;
	}

	err = aacEncOpen(&data->enc, 0, num_channels);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT, "Failed to open AAC encoder",
				      "libfdk-aac", err, NULL);
		free(data);
		return MUX_ERROR_INIT;
	}

	aacEncoder_SetParam(data->enc, AACENC_AOT, profile);
	aacEncoder_SetParam(data->enc, AACENC_SAMPLERATE, sample_rate);
	aacEncoder_SetParam(data->enc, AACENC_CHANNELMODE, channel_mode);
	aacEncoder_SetParam(data->enc, AACENC_BITRATE, bitrate);
	/* ADTS: each frame is self-delimiting and carries its own config, so the
	 * decoder needs no out-of-band AudioSpecificConfig - the right choice for
	 * a streaming/muxed format. */
	aacEncoder_SetParam(data->enc, AACENC_TRANSMUX, TT_MP4_ADTS);

	err = aacEncEncode(data->enc, NULL, NULL, NULL, NULL);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AAC encoder",
				      "libfdk-aac", err, NULL);
		aacEncClose(&data->enc);
		free(data);
		return MUX_ERROR_INIT;
	}

	err = aacEncInfo(data->enc, &info);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to get AAC encoder info",
				      "libfdk-aac", err, NULL);
		aacEncClose(&data->enc);
		free(data);
		return MUX_ERROR_INIT;
	}

	data->frame_samples = info.frameLength * num_channels;
	data->output_buf_size = info.maxOutBufBytes;
	data->frame_buf = malloc(data->frame_samples * sizeof(int16_t));
	data->output_buf = malloc(data->output_buf_size);
	if (!data->frame_buf || !data->output_buf) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC buffers", NULL, 0, NULL);
		aacEncClose(&data->enc);
		free(data->frame_buf);
		free(data->output_buf);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	enc->codec_data = data;
	return MUX_OK;
}

static void mux_aac_encoder_deinit(struct mux_encoder *enc)
{
	struct aac_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;
	data = enc->codec_data;
	if (data->enc)
		aacEncClose(&data->enc);
	free(data->frame_buf);
	free(data->output_buf);
	free(data);
	enc->codec_data = NULL;
}

/* Encode exactly 'nsamples' interleaved samples from 'pcm' and emit the frame.
 * nsamples is normally frame_samples; -1 signals EOF flush. */
static int aac_encode_frame(struct mux_encoder *enc, struct aac_encoder_data *data,
			    const int16_t *pcm, int nsamples)
{
	AACENC_BufDesc in_buf = { 0 }, out_buf = { 0 };
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };
	int in_identifier = IN_AUDIO_DATA, out_identifier = OUT_BITSTREAM_DATA;
	int in_size, in_elem_size = sizeof(int16_t);
	int out_size, out_elem_size = 1;
	void *in_ptr = (void *)pcm, *out_ptr = data->output_buf;
	AACENC_ERROR err;

	if (nsamples > 0) {
		in_size = nsamples * sizeof(int16_t);
		in_buf.numBufs = 1;
		in_buf.bufs = &in_ptr;
		in_buf.bufferIdentifiers = &in_identifier;
		in_buf.bufSizes = &in_size;
		in_buf.bufElSizes = &in_elem_size;
		in_args.numInSamples = nsamples;
	} else {
		in_args.numInSamples = -1;   /* EOF */
	}

	out_size = data->output_buf_size;
	out_buf.numBufs = 1;
	out_buf.bufs = &out_ptr;
	out_buf.bufferIdentifiers = &out_identifier;
	out_buf.bufSizes = &out_size;
	out_buf.bufElSizes = &out_elem_size;

	err = aacEncEncode(data->enc, &in_buf, &out_buf, &in_args, &out_args);
	if (err != AACENC_OK && err != AACENC_ENCODE_EOF) {
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE, "AAC encoding failed",
				      "libfdk-aac", err, NULL);
		return MUX_ERROR_ENCODE;
	}

	if (out_args.numOutBytes > 0)
		return mux_leb128_emit_frame(data->output_buf, out_args.numOutBytes,
					     MUX_STREAM_AUDIO, enc->num_streams,
					     enc->sink, enc->sink_user);
	return MUX_OK;
}

static int mux_aac_encoder_encode(struct mux_encoder *enc, const void *input,
				  size_t input_size, int stream_type)
{
	struct aac_encoder_data *data;
	const int16_t *pcm;
	int avail, idx, frame, rem, ret;

	if (!enc || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;
	if (input_size == 0)
		return MUX_OK;

	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_leb128_emit_frame(input, input_size, stream_type,
					     enc->num_streams,
					     enc->sink, enc->sink_user);

	pcm = input;
	frame = data->frame_samples;
	avail = (int)(input_size / sizeof(int16_t));   /* interleaved samples */
	idx = 0;

	if (data->pending > 0) {
		int need = frame - data->pending;
		int take = avail < need ? avail : need;

		memcpy(data->frame_buf + data->pending, pcm, take * sizeof(int16_t));
		data->pending += take;
		idx += take;

		if (data->pending < frame)
			return MUX_OK;

		ret = aac_encode_frame(enc, data, data->frame_buf, frame);
		if (ret)
			return ret;
		data->pending = 0;
	}

	while (avail - idx >= frame) {
		ret = aac_encode_frame(enc, data, pcm + idx, frame);
		if (ret)
			return ret;
		idx += frame;
	}

	rem = avail - idx;
	if (rem > 0) {
		memcpy(data->frame_buf, pcm + idx, rem * sizeof(int16_t));
		data->pending = rem;
	}

	return MUX_OK;
}

static int mux_aac_encoder_finalize(struct mux_encoder *enc)
{
	struct aac_encoder_data *data;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;
	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Zero-pad and encode the final sub-frame tail, then flush EOF. */
	if (data->pending > 0) {
		memset(data->frame_buf + data->pending, 0,
		       (data->frame_samples - data->pending) * sizeof(int16_t));
		ret = aac_encode_frame(enc, data, data->frame_buf, data->frame_samples);
		if (ret)
			return ret;
		data->pending = 0;
	}

	return aac_encode_frame(enc, data, NULL, -1);
}

/* ---- decoder ------------------------------------------------------------ */

static int mux_aac_decoder_init(struct mux_decoder *dec,
				const struct mux_param *params, int num_params)
{
	struct aac_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC decoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}
	mux_leb128_parser_init(&data->parser);

	data->dec = aacDecoder_Open(TT_MP4_ADTS, 1);
	if (!data->dec) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to create AAC decoder",
				      "libfdk-aac", 0, NULL);
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

static void mux_aac_decoder_deinit(struct mux_decoder *dec)
{
	struct aac_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;
	data = dec->codec_data;
	if (data->dec)
		aacDecoder_Close(data->dec);
	free(data);
	dec->codec_data = NULL;
}

/* Decode one complete reassembled raw AAC frame and emit the PCM. */
static int aac_decode_frame(struct mux_decoder *dec, struct aac_decoder_data *data)
{
	uint8_t *in_ptr = data->frame;
	UINT buffer_size = (UINT)data->frame_len;
	UINT bytes_valid = buffer_size;
	AAC_DECODER_ERROR err;

	err = aacDecoder_Fill(data->dec, &in_ptr, &buffer_size, &bytes_valid);
	if (err != AAC_DEC_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_FORMAT, "AAC decoder fill failed",
				      "libfdk-aac", err, NULL);
		return MUX_OK;   /* skip this frame, keep going */
	}

	for (;;) {
		int16_t pcm_buf[8192];
		CStreamInfo *info;

		err = aacDecoder_DecodeFrame(data->dec, pcm_buf,
					     sizeof(pcm_buf) / sizeof(int16_t), 0);
		if (err == AAC_DEC_NOT_ENOUGH_BITS)
			break;
		if (err != AAC_DEC_OK) {
			mux_decoder_set_error(dec, MUX_ERROR_DECODE, "AAC decode failed",
					      "libfdk-aac", err, NULL);
			break;
		}

		info = aacDecoder_GetStreamInfo(data->dec);
		if (info && info->numChannels > 0) {
			size_t sz = (size_t)info->frameSize * info->numChannels *
				    sizeof(int16_t);
			int r;

			data->sample_rate = info->sampleRate;
			data->num_channels = info->numChannels;
			r = mux_decoder_emit(dec, MUX_STREAM_AUDIO, pcm_buf, sz, 0);
			if (r)
				return r;
		}
	}

	return MUX_OK;
}

/* Parser emit shim: reassemble one raw AAC frame; pass side through. */
static int aac_parser_emit(void *user, int stream_type, const void *chunk,
			   size_t size, int flags)
{
	struct mux_decoder *dec = user;
	struct aac_decoder_data *data = dec->codec_data;
	int ret;

	if (stream_type == MUX_STREAM_SIDE_CHANNEL)
		return mux_decoder_emit(dec, stream_type, chunk, size, flags);

	if (data->frame_len + size > AAC_FRAME_CAP) {
		data->overflow = 1;
		mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
				      "AAC frame exceeds reassembly buffer",
				      NULL, 0, NULL);
		return MUX_ERROR_FORMAT;
	}
	if (size) {
		memcpy(data->frame + data->frame_len, chunk, size);
		data->frame_len += size;
	}

	if (flags & MUX_EMIT_FRAME_END) {
		ret = aac_decode_frame(dec, data);
		data->frame_len = 0;
		return ret;
	}
	return MUX_OK;
}

static int mux_aac_decoder_decode(struct mux_decoder *dec, const void *input,
				  size_t input_size)
{
	struct aac_decoder_data *data;

	if (!dec || (!input && input_size))
		return MUX_ERROR_INVAL;
	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	return mux_leb128_parser_feed(&data->parser, input, input_size,
				      dec->num_streams, aac_parser_emit, dec);
}

static int mux_aac_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

const struct mux_codec_ops mux_codec_aac_ops = {
	.encoder_init = mux_aac_encoder_init,
	.encoder_deinit = mux_aac_encoder_deinit,
	.encoder_encode = mux_aac_encoder_encode,
	.encoder_finalize = mux_aac_encoder_finalize,

	.decoder_init = mux_aac_decoder_init,
	.decoder_deinit = mux_aac_decoder_deinit,
	.decoder_decode = mux_aac_decoder_decode,
	.decoder_finalize = mux_aac_decoder_finalize,

	.encoder_params = aac_encoder_params,
	.encoder_param_count = sizeof(aac_encoder_params) / sizeof(aac_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = aac_sample_rates,
	.sample_rate_count = sizeof(aac_sample_rates) / sizeof(aac_sample_rates[0]),
	.sample_rate_is_range = 0
};
