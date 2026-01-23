/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>

/*
 * FLAC encoder state
 * Uses LEB128 framing (like MP3/PCM)
 */
struct flac_encoder_data {
	/* FLAC encoder state */
	FLAC__StreamEncoder *enc;

	/* Output buffer for FLAC data before LEB128 framing */
	struct mux_buffer flac_buffer;

	/* Sample rate and channels */
	int sample_rate;
	int num_channels;
};

/*
 * FLAC decoder state
 */
struct flac_decoder_data {
	/* FLAC decoder state */
	FLAC__StreamDecoder *dec;

	/* Input buffer for LEB128 demuxing */
	struct mux_buffer leb128_input_buf;

	/* Input buffer for FLAC decoder */
	struct mux_buffer flac_input_buf;

	/* Buffer for decoded audio output */
	struct mux_buffer *audio_output_buf;

	/* Sample rate info */
	int sample_rate;
	int num_channels;
};

/*
 * FLAC encoder parameters
 */
static const struct mux_param_desc flac_encoder_params[] = {
	{
		.name = "compression",
		.description = "Compression level (0=fast, 8=best)",
		.type = MUX_PARAM_TYPE_INT,
		.range.i = { .min = 0, .max = 8, .def = 5 }
	}
};

/*
 * FLAC sample rate constraints (supports wide range)
 */
static const int flac_sample_rates[] = { 1000, 655350 };  /* Min/max range */

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
 * FLAC encoder write callback - writes to internal buffer
 */
static FLAC__StreamEncoderWriteStatus flac_write_callback(
	const FLAC__StreamEncoder *encoder,
	const FLAC__byte buffer[],
	size_t bytes,
	unsigned samples,
	unsigned current_frame,
	void *client_data)
{
	struct flac_encoder_data *data = client_data;

	(void)encoder;
	(void)samples;
	(void)current_frame;

	/* Write FLAC data to internal buffer */
	mux_buffer_write(&data->flac_buffer, buffer, bytes);

	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

/*
 * FLAC encoder initialization
 */
static int mux_flac_encoder_init(struct mux_encoder *enc,
				  int sample_rate,
				  int num_channels,
				  const struct mux_param *params,
				  int num_params)
{
	struct flac_encoder_data *data;
	const struct mux_param *param;
	int compression = 5;
	FLAC__StreamEncoderInitStatus init_status;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC encoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->sample_rate = sample_rate;
	data->num_channels = num_channels;

	/* Initialize internal buffer */
	if (mux_buffer_init(&data->flac_buffer, 8192) != MUX_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC buffer",
				      NULL, 0, NULL);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	/* Get parameters */
	param = find_param(params, num_params, "compression");
	if (param)
		compression = param->value.i;

	/* Create FLAC encoder */
	data->enc = FLAC__stream_encoder_new();
	if (!data->enc) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to create FLAC encoder",
				      "libFLAC", 0, NULL);
		mux_buffer_deinit(&data->flac_buffer);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Configure encoder */
	FLAC__stream_encoder_set_channels(data->enc, num_channels);
	FLAC__stream_encoder_set_bits_per_sample(data->enc, 16);
	FLAC__stream_encoder_set_sample_rate(data->enc, sample_rate);
	FLAC__stream_encoder_set_compression_level(data->enc, compression);

	/* Initialize FLAC encoder with stream output */
	init_status = FLAC__stream_encoder_init_stream(
		data->enc,
		flac_write_callback,
		NULL,  /* seek callback (not needed) */
		NULL,  /* tell callback (not needed) */
		NULL,  /* metadata callback (not needed) */
		data   /* client data */
	);

	if (init_status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize FLAC encoder",
				      "libFLAC", init_status,
				      FLAC__StreamEncoderInitStatusString[init_status]);
		FLAC__stream_encoder_delete(data->enc);
		mux_buffer_deinit(&data->flac_buffer);
		free(data);
		return MUX_ERROR_INIT;
	}

	enc->codec_data = data;
	return MUX_OK;
}

/*
 * FLAC encoder deinitialization
 */
static void mux_flac_encoder_deinit(struct mux_encoder *enc)
{
	struct flac_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	if (data->enc)
		FLAC__stream_encoder_delete(data->enc);

	mux_buffer_deinit(&data->flac_buffer);
	free(data);
	enc->codec_data = NULL;
}

/*
 * FLAC encoder encode
 * For audio: compress with FLAC and write LEB128 frames
 * For side channel: write raw LEB128 frames
 */
static int mux_flac_encoder_encode(struct mux_encoder *enc,
				    const void *input,
				    size_t input_size,
				    size_t *input_consumed,
				    int stream_type)
{
	struct flac_encoder_data *data;
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

	/* Audio data: encode with FLAC */
	const int16_t *pcm = input;
	size_t num_samples = input_size / sizeof(int16_t) / data->num_channels;

	/* Convert interleaved int16 to FLAC__int32 planar */
	FLAC__int32 *buffer[8];  /* Max 8 channels */
	int ch;

	for (ch = 0; ch < data->num_channels; ch++) {
		buffer[ch] = malloc(num_samples * sizeof(FLAC__int32));
		if (!buffer[ch]) {
			for (int i = 0; i < ch; i++)
				free(buffer[i]);
			mux_encoder_set_error(enc, MUX_ERROR_NOMEM,
					      "Failed to allocate FLAC buffer",
					      NULL, 0, NULL);
			return MUX_ERROR_NOMEM;
		}
	}

	for (size_t i = 0; i < num_samples; i++) {
		for (ch = 0; ch < data->num_channels; ch++) {
			buffer[ch][i] = pcm[i * data->num_channels + ch];
		}
	}

	/* Encode samples */
	FLAC__bool ok = FLAC__stream_encoder_process(data->enc,
						     (const FLAC__int32 *const *)buffer,
						     num_samples);

	/* Free buffers */
	for (ch = 0; ch < data->num_channels; ch++)
		free(buffer[ch]);

	if (!ok) {
		FLAC__StreamEncoderState state = FLAC__stream_encoder_get_state(data->enc);
		mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
				      "FLAC encoding failed",
				      "libFLAC", state,
				      FLAC__StreamEncoderStateString[state]);
		return MUX_ERROR_ENCODE;
	}

	/* Read FLAC data from internal buffer and write as LEB128 frames */
	uint8_t flac_data[8192];
	size_t flac_bytes;

	while (mux_buffer_read(&data->flac_buffer, flac_data, sizeof(flac_data),
			       &flac_bytes) == MUX_OK && flac_bytes > 0) {
		ret = mux_leb128_write_frame(&enc->output, flac_data, flac_bytes,
					     MUX_STREAM_AUDIO, enc->num_streams);
		if (ret != MUX_OK)
			return ret;
	}

	*input_consumed = input_size;
	return MUX_OK;
}

/*
 * FLAC encoder read
 * Reads multiplexed output data
 */
static int mux_flac_encoder_read(struct mux_encoder *enc,
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
 * FLAC encoder finalize
 * Flushes any buffered data
 */
static int mux_flac_encoder_finalize(struct mux_encoder *enc)
{
	struct flac_encoder_data *data;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Finish FLAC encoding */
	FLAC__stream_encoder_finish(data->enc);

	/* Read any remaining FLAC data from internal buffer and write as LEB128 frames */
	uint8_t flac_data[8192];
	size_t flac_bytes;

	while (mux_buffer_read(&data->flac_buffer, flac_data, sizeof(flac_data),
			       &flac_bytes) == MUX_OK && flac_bytes > 0) {
		ret = mux_leb128_write_frame(&enc->output, flac_data, flac_bytes,
					     MUX_STREAM_AUDIO, enc->num_streams);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * FLAC decoder read callback - reads from FLAC input buffer
 */
static FLAC__StreamDecoderReadStatus flac_decoder_read_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__byte buffer[],
	size_t *bytes,
	void *client_data)
{
	struct flac_decoder_data *data = client_data;
	size_t read_bytes;

	(void)decoder;

	/* Try to read from FLAC input buffer */
	if (mux_buffer_available(&data->flac_input_buf) == 0) {
		*bytes = 0;
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	mux_buffer_read(&data->flac_input_buf, buffer, *bytes, &read_bytes);
	*bytes = read_bytes;

	return read_bytes > 0 ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE :
				FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

/*
 * FLAC decoder write callback - writes decoded audio to buffer
 */
static FLAC__StreamDecoderWriteStatus flac_decoder_write_callback(
	const FLAC__StreamDecoder *decoder,
	const FLAC__Frame *frame,
	const FLAC__int32 *const buffer[],
	void *client_data)
{
	struct flac_decoder_data *data = client_data;
	int ch;
	unsigned i;
	int16_t *pcm_buf;
	size_t pcm_size;

	(void)decoder;

	/* Convert planar FLAC__int32 to interleaved int16 */
	pcm_size = frame->header.blocksize * data->num_channels * sizeof(int16_t);
	pcm_buf = malloc(pcm_size);
	if (!pcm_buf)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	for (i = 0; i < frame->header.blocksize; i++) {
		for (ch = 0; ch < data->num_channels; ch++) {
			pcm_buf[i * data->num_channels + ch] = (int16_t)buffer[ch][i];
		}
	}

	/* Write to audio output buffer */
	mux_buffer_write(data->audio_output_buf, pcm_buf, pcm_size);

	free(pcm_buf);
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/*
 * FLAC decoder metadata callback
 */
static void flac_decoder_metadata_callback(
	const FLAC__StreamDecoder *decoder,
	const FLAC__StreamMetadata *metadata,
	void *client_data)
{
	struct flac_decoder_data *data = client_data;

	(void)decoder;

	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		data->sample_rate = metadata->data.stream_info.sample_rate;
		data->num_channels = metadata->data.stream_info.channels;
	}
}

/*
 * FLAC decoder error callback
 */
static void flac_decoder_error_callback(
	const FLAC__StreamDecoder *decoder,
	FLAC__StreamDecoderErrorStatus status,
	void *client_data)
{
	(void)decoder;
	(void)status;
	(void)client_data;
	/* Errors are handled elsewhere */
}

/*
 * FLAC decoder initialization
 */
static int mux_flac_decoder_init(struct mux_decoder *dec,
				  const struct mux_param *params,
				  int num_params)
{
	struct flac_decoder_data *data;
	FLAC__StreamDecoderInitStatus init_status;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC decoder data",
				      NULL, 0, NULL);
		return MUX_ERROR_NOMEM;
	}

	data->audio_output_buf = &dec->audio_output;

	/* Initialize input buffer for LEB128 demuxing */
	if (mux_buffer_init(&data->leb128_input_buf, 4096) != MUX_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate LEB128 input buffer",
				      NULL, 0, NULL);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	/* Initialize input buffer for FLAC decoder */
	if (mux_buffer_init(&data->flac_input_buf, 4096) != MUX_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_NOMEM,
				      "Failed to allocate FLAC input buffer",
				      NULL, 0, NULL);
		mux_buffer_deinit(&data->leb128_input_buf);
		free(data);
		return MUX_ERROR_NOMEM;
	}

	/* Create FLAC decoder */
	data->dec = FLAC__stream_decoder_new();
	if (!data->dec) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to create FLAC decoder",
				      "libFLAC", 0, NULL);
		mux_buffer_deinit(&data->flac_input_buf);
		mux_buffer_deinit(&data->leb128_input_buf);
		free(data);
		return MUX_ERROR_INIT;
	}

	/* Initialize FLAC decoder with stream callbacks */
	init_status = FLAC__stream_decoder_init_stream(
		data->dec,
		flac_decoder_read_callback,
		NULL,  /* seek callback */
		NULL,  /* tell callback */
		NULL,  /* length callback */
		NULL,  /* eof callback */
		flac_decoder_write_callback,
		flac_decoder_metadata_callback,
		flac_decoder_error_callback,
		data   /* client data */
	);

	if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "FLAC decoder init failed",
				      "libFLAC", init_status,
				      FLAC__StreamDecoderInitStatusString[init_status]);
		FLAC__stream_decoder_delete(data->dec);
		mux_buffer_deinit(&data->flac_input_buf);
		mux_buffer_deinit(&data->leb128_input_buf);
		free(data);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * FLAC decoder deinitialization
 */
static void mux_flac_decoder_deinit(struct mux_decoder *dec)
{
	struct flac_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->dec) {
		FLAC__stream_decoder_finish(data->dec);
		FLAC__stream_decoder_delete(data->dec);
	}

	mux_buffer_deinit(&data->flac_input_buf);
	mux_buffer_deinit(&data->leb128_input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * FLAC decoder decode
 * Reads LEB128 frames and decodes FLAC audio
 */
static int mux_flac_decoder_decode(struct mux_decoder *dec,
				    const void *input,
				    size_t input_size,
				    size_t *input_consumed)
{
	struct flac_decoder_data *data;
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

	/* Add input to LEB128 buffer */
	ret = mux_buffer_write(&data->leb128_input_buf, input, input_size);
	if (ret != MUX_OK)
		return ret;

	consumed = input_size;

	/* Try to read frames from LEB128 input buffer */
	while (1) {
		ret = mux_leb128_read_frame(&data->leb128_input_buf,
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

		/* Audio data: add to FLAC input buffer and process */
		ret = mux_buffer_write(&data->flac_input_buf, frame_buf, frame_size);
		if (ret != MUX_OK)
			return ret;
	}

	/* Process FLAC stream - decode whatever data is available */
	if (mux_buffer_available(&data->flac_input_buf) > 0) {
		/* Process metadata first if needed */
		if (!FLAC__stream_decoder_get_total_samples(data->dec)) {
			FLAC__stream_decoder_process_until_end_of_metadata(data->dec);
		}

		/* Process audio frames */
		while (mux_buffer_available(&data->flac_input_buf) > 0 &&
		       (FLAC__stream_decoder_get_state(data->dec) == FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC ||
			FLAC__stream_decoder_get_state(data->dec) == FLAC__STREAM_DECODER_READ_FRAME)) {
			if (!FLAC__stream_decoder_process_single(data->dec))
				break;
		}
	}

	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * FLAC decoder read
 * Reads decoded audio or side channel data
 */
static int mux_flac_decoder_read(struct mux_decoder *dec,
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
 * FLAC decoder finalize
 * Flushes any remaining data
 */
static int mux_flac_decoder_finalize(struct mux_decoder *dec)
{
	struct flac_decoder_data *data;

	if (!dec)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Process any remaining frames */
	while (FLAC__stream_decoder_get_state(data->dec) != FLAC__STREAM_DECODER_END_OF_STREAM) {
		if (!FLAC__stream_decoder_process_single(data->dec))
			break;
	}

	return MUX_OK;
}

/*
 * FLAC codec operations
 */
const struct mux_codec_ops mux_codec_flac_ops = {
	.encoder_init = mux_flac_encoder_init,
	.encoder_deinit = mux_flac_encoder_deinit,
	.encoder_encode = mux_flac_encoder_encode,
	.encoder_read = mux_flac_encoder_read,
	.encoder_finalize = mux_flac_encoder_finalize,

	.decoder_init = mux_flac_decoder_init,
	.decoder_deinit = mux_flac_decoder_deinit,
	.decoder_decode = mux_flac_decoder_decode,
	.decoder_read = mux_flac_decoder_read,
	.decoder_finalize = mux_flac_decoder_finalize,

	.encoder_params = flac_encoder_params,
	.encoder_param_count = sizeof(flac_encoder_params) / sizeof(flac_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = flac_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
