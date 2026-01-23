/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fdk-aac/aacenc_lib.h>
#include <fdk-aac/aacdecoder_lib.h>

/*
 * AAC encoder state
 * Uses LEB128 framing (like MP3/PCM/FLAC)
 */
struct aac_encoder_data {
	/* FDK-AAC encoder handle */
	HANDLE_AACENCODER enc;

	/* Encoder configuration */
	int sample_rate;
	int num_channels;
	int bitrate;

	/* Input/output buffers for FDK-AAC */
	int16_t *input_buf;
	uint8_t *output_buf;
	int input_buf_size;
	int output_buf_size;
};

/*
 * AAC decoder state
 */
struct aac_decoder_data {
	/* FDK-AAC decoder handle */
	HANDLE_AACDECODER dec;

	/* Input buffer for LEB128 demuxing */
	struct mux_buffer input_buf;

	/* Decoder configuration */
	int sample_rate;
	int num_channels;
};

/*
 * AAC encoder parameters
 */
static const struct mux_param_desc aac_encoder_params[] = {
	{
		.name = "bitrate",
		.description = "Bitrate in kbps",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 8, .max = 512, .def = 128 }
	},
	{
		.name = "profile",
		.description = "AAC profile (2=LC, 5=HE, 29=HEv2)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 2, .max = 29, .def = 2 }
	}
};

/*
 * AAC sample rate constraints (discrete list)
 */
static const int aac_sample_rates[] = {
	8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000
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
 * AAC encoder initialization
 */
static int mux_aac_encoder_init(struct mux_encoder *enc,
				 int sample_rate,
				 int num_channels,
				 const struct mux_param *params,
				 int num_params)
{
	struct aac_encoder_data *data;
	const struct mux_param *param;
	int bitrate = 128000;  /* bits per second */
	int profile = 2;       /* AAC-LC */
	AACENC_ERROR err;
	CHANNEL_MODE channel_mode;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	/* Get parameters */
	param = find_param(params, num_params, "bitrate");
	if (param)
		bitrate = param->value.i * 1000;  /* Convert kbps to bps */

	param = find_param(params, num_params, "profile");
	if (param)
		profile = param->value.i;

	data->bitrate = bitrate;

	/* Determine channel mode */
	switch (num_channels) {
	case 1:
		channel_mode = MODE_1;  /* Mono */
		break;
	case 2:
		channel_mode = MODE_2;  /* Stereo */
		break;
	default:
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "Unsupported channel count for AAC",
				      NULL, 0, NULL);
		free(data);
		return MUX_ERROR_INVAL;
	}

	/* Open encoder */
	err = aacEncOpen(&data->enc, 0, num_channels);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to open AAC encoder",
				      "libfdk-aac", err, NULL);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Configure encoder */
	aacEncoder_SetParam(data->enc, AACENC_AOT, profile);
	aacEncoder_SetParam(data->enc, AACENC_SAMPLERATE, sample_rate);
	aacEncoder_SetParam(data->enc, AACENC_CHANNELMODE, channel_mode);
	aacEncoder_SetParam(data->enc, AACENC_BITRATE, bitrate);
	aacEncoder_SetParam(data->enc, AACENC_TRANSMUX, TT_MP4_RAW);  /* Raw AAC frames */

	/* Initialize encoder */
	err = aacEncEncode(data->enc, NULL, NULL, NULL, NULL);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AAC encoder",
				      "libfdk-aac", err, NULL);
		aacEncClose(&data->enc);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Get encoder info for buffer sizes */
	AACENC_InfoStruct info;
	err = aacEncInfo(data->enc, &info);
	if (err != AACENC_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to get AAC encoder info",
				      "libfdk-aac", err, NULL);
		aacEncClose(&data->enc);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Allocate buffers */
	data->input_buf_size = info.frameLength * num_channels;
	data->output_buf_size = info.maxOutBufBytes;
	data->input_buf = malloc(data->input_buf_size * sizeof(int16_t));
	data->output_buf = malloc(data->output_buf_size);

	if (!data->input_buf || !data->output_buf) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC buffers",
				      NULL, 0, NULL);
		aacEncClose(&data->enc);
		free(data->input_buf);
		free(data->output_buf);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	enc->codec_data = data;
	return MUX_OK;
}

/*
 * AAC encoder deinitialization
 */
static void mux_aac_encoder_deinit(struct mux_encoder *enc)
{
	struct aac_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	if (data->enc)
		aacEncClose(&data->enc);

	free(data->input_buf);
	free(data->output_buf);
	free(data);
	enc->codec_data = NULL;
}

/*
 * AAC encoder encode
 * For audio: encode with AAC and write LEB128 frames
 * For side channel: write raw LEB128 frames
 */
static int mux_aac_encoder_encode(struct mux_encoder *enc,
				   const void *input,
				   size_t input_size,
				   size_t *input_consumed,
				   int stream_type)
{
	struct aac_encoder_data *data;
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

	/* Side channel data passes through uncompressed */
	if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
		ret = mux_leb128_write_frame(&enc->output, input, input_size,
					     stream_type, enc->num_streams);
		if (ret != MUX_OK)
			return ret;
		*input_consumed = input_size;
		return MUX_OK;
	}

	/* Audio data: encode with AAC */
	const int16_t *pcm = input;
	size_t samples_available = input_size / sizeof(int16_t);
	size_t samples_per_frame = data->input_buf_size;

	/* Process complete frames */
	*input_consumed = 0;

	while (samples_available >= samples_per_frame) {
		AACENC_BufDesc in_buf = { 0 };
		AACENC_BufDesc out_buf = { 0 };
		AACENC_InArgs in_args = { 0 };
		AACENC_OutArgs out_args = { 0 };
		int in_identifier = IN_AUDIO_DATA;
		int in_size, in_elem_size;
		int out_identifier = OUT_BITSTREAM_DATA;
		int out_size, out_elem_size;
		void *in_ptr, *out_ptr;

		/* Setup input buffer */
		in_size = samples_per_frame * sizeof(int16_t);
		in_elem_size = sizeof(int16_t);
		in_ptr = (void *)pcm;

		in_buf.numBufs = 1;
		in_buf.bufs = &in_ptr;
		in_buf.bufferIdentifiers = &in_identifier;
		in_buf.bufSizes = &in_size;
		in_buf.bufElSizes = &in_elem_size;

		/* Setup output buffer */
		out_size = data->output_buf_size;
		out_elem_size = 1;
		out_ptr = data->output_buf;

		out_buf.numBufs = 1;
		out_buf.bufs = &out_ptr;
		out_buf.bufferIdentifiers = &out_identifier;
		out_buf.bufSizes = &out_size;
		out_buf.bufElSizes = &out_elem_size;

		/* Input arguments */
		in_args.numInSamples = samples_per_frame;

		/* Encode */
		AACENC_ERROR err = aacEncEncode(data->enc, &in_buf, &out_buf,
						&in_args, &out_args);
		if (err != AACENC_OK) {
			mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
					      "AAC encoding failed",
					      "libfdk-aac", err, NULL);
			return MUX_ERROR_ENCODE;
		}

		/* Write encoded frame as LEB128 */
		if (out_args.numOutBytes > 0) {
			ret = mux_leb128_write_frame(&enc->output,
						     data->output_buf,
						     out_args.numOutBytes,
						     MUX_STREAM_AUDIO, enc->num_streams);
			if (ret != MUX_OK)
				return ret;
		}

		pcm += samples_per_frame;
		*input_consumed += samples_per_frame * sizeof(int16_t);
		samples_available -= samples_per_frame;
	}

	return MUX_OK;
}

/*
 * AAC encoder read
 * Reads multiplexed output data
 */
static int mux_aac_encoder_read(struct mux_encoder *enc,
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
 * AAC encoder finalize
 * Flushes any buffered data
 */
static int mux_aac_encoder_finalize(struct mux_encoder *enc)
{
	struct aac_encoder_data *data;
	AACENC_BufDesc in_buf = { 0 };
	AACENC_BufDesc out_buf = { 0 };
	AACENC_InArgs in_args = { 0 };
	AACENC_OutArgs out_args = { 0 };
	int out_identifier = OUT_BITSTREAM_DATA;
	int out_size, out_elem_size;
	void *out_ptr;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Flush encoder by passing NULL input */
	out_size = data->output_buf_size;
	out_elem_size = 1;
	out_ptr = data->output_buf;

	out_buf.numBufs = 1;
	out_buf.bufs = &out_ptr;
	out_buf.bufferIdentifiers = &out_identifier;
	out_buf.bufSizes = &out_size;
	out_buf.bufElSizes = &out_elem_size;

	in_args.numInSamples = -1;  /* Signal EOF */

	AACENC_ERROR err = aacEncEncode(data->enc, &in_buf, &out_buf,
					&in_args, &out_args);
	if (err != AACENC_OK && err != AACENC_ENCODE_EOF) {
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
				      "AAC finalize failed",
				      "libfdk-aac", err, NULL);
		return MUX_ERROR_ENCODE;
	}

	/* Write final frame if any */
	if (out_args.numOutBytes > 0) {
		ret = mux_leb128_write_frame(&enc->output,
					     data->output_buf,
					     out_args.numOutBytes,
					     MUX_STREAM_AUDIO, enc->num_streams);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * AAC decoder initialization
 */
static int mux_aac_decoder_init(struct mux_decoder *dec,
				 const struct mux_param *params,
				 int num_params)
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

	/* Initialize input buffer for LEB128 demuxing */
	if (mux_buffer_init(&data->input_buf, 8192) != MUX_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate AAC input buffer",
				      NULL, 0, NULL);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	/* Create AAC decoder */
	data->dec = aacDecoder_Open(TT_MP4_RAW, 1);  /* 1 layer */
	if (!data->dec) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to create AAC decoder",
				      "libfdk-aac", 0, NULL);
		mux_buffer_deinit(&data->input_buf);
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * AAC decoder deinitialization
 */
static void mux_aac_decoder_deinit(struct mux_decoder *dec)
{
	struct aac_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->dec)
		aacDecoder_Close(data->dec);

	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * AAC decoder decode
 * Reads LEB128 frames and decodes AAC audio
 */
static int mux_aac_decoder_decode(struct mux_decoder *dec,
				   const void *input,
				   size_t input_size,
				   size_t *input_consumed)
{
	struct aac_decoder_data *data;
	uint8_t frame_buf[8192];
	size_t frame_size;
	int stream_type;
	int ret;
	size_t consumed = 0;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Add input to buffer */
	ret = mux_buffer_write(&data->input_buf, input, input_size);
	if (ret != MUX_OK)
		return ret;

	consumed = input_size;

	/* Try to read frames from input buffer */
	while (1) {
		ret = mux_leb128_read_frame(&data->input_buf,
					    frame_buf, sizeof(frame_buf),
					    &frame_size, &stream_type, dec->num_streams);
		if (ret == MUX_ERROR_AGAIN) {
			/* Need more data */
			break;
		}
		if (ret != MUX_OK) {
			mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
					      "Failed to read LEB128 frame",
					      NULL, 0, NULL);
			return ret;
		}

		/* Side channel passes through */
		if (stream_type == MUX_STREAM_SIDE_CHANNEL) {
			ret = mux_buffer_write(&dec->side_output,
					       frame_buf, frame_size);
			if (ret != MUX_OK)
				return ret;
			continue;
		}

		/* Audio data: decode with AAC */
		uint8_t *input_ptr = frame_buf;
		UINT buffer_size = (UINT)frame_size;
		UINT bytes_valid = buffer_size;

		/* Fill decoder with compressed data */
		AAC_DECODER_ERROR err = aacDecoder_Fill(data->dec, &input_ptr,
							&buffer_size, &bytes_valid);
		if (err != AAC_DEC_OK) {
			mux_decoder_set_error(dec, MUX_ERROR_FORMAT,
					      "AAC decoder fill failed",
					      "libfdk-aac", err, NULL);
			continue;  /* Try to continue */
		}

		/* Decode frame */
		int16_t pcm_buf[8192];  /* Large enough for any AAC frame */
		err = aacDecoder_DecodeFrame(data->dec, pcm_buf,
					     sizeof(pcm_buf) / sizeof(int16_t),
					     0);
		if (err != AAC_DEC_OK) {
			/* Some errors are recoverable */
			if (err != AAC_DEC_NOT_ENOUGH_BITS)
				mux_decoder_set_error(dec, MUX_ERROR_DECODE,
						      "AAC decode failed",
						      "libfdk-aac", err, NULL);
			continue;
		}

		/* Get stream info */
		CStreamInfo *info = aacDecoder_GetStreamInfo(data->dec);
		if (info && info->numChannels > 0) {
			data->sample_rate = info->sampleRate;
			data->num_channels = info->numChannels;

			/* Write decoded PCM to output */
			size_t pcm_bytes = info->frameSize * info->numChannels * sizeof(int16_t);
			ret = mux_buffer_write(&dec->audio_output, pcm_buf, pcm_bytes);
			if (ret != MUX_OK)
				return ret;
		}
	}

	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * AAC decoder read
 * Reads decoded audio or side channel data
 */
static int mux_aac_decoder_read(struct mux_decoder *dec,
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
 * AAC decoder finalize
 * Flushes any remaining data
 */
static int mux_aac_decoder_finalize(struct mux_decoder *dec)
{
	/* AAC decoder doesn't require finalization */
	(void)dec;
	return MUX_OK;
}

/*
 * AAC codec operations
 */
const struct mux_codec_ops mux_codec_aac_ops = {
	.encoder_init = mux_aac_encoder_init,
	.encoder_deinit = mux_aac_encoder_deinit,
	.encoder_encode = mux_aac_encoder_encode,
	.encoder_read = mux_aac_encoder_read,
	.encoder_finalize = mux_aac_encoder_finalize,

	.decoder_init = mux_aac_decoder_init,
	.decoder_deinit = mux_aac_decoder_deinit,
	.decoder_decode = mux_aac_decoder_decode,
	.decoder_read = mux_aac_decoder_read,
	.decoder_finalize = mux_aac_decoder_finalize,

	.encoder_params = aac_encoder_params,
	.encoder_param_count = sizeof(aac_encoder_params) / sizeof(aac_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = aac_sample_rates,
	.sample_rate_count = sizeof(aac_sample_rates) / sizeof(aac_sample_rates[0]),
	.sample_rate_is_range = 0
};
