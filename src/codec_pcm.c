/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * PCM codec state
 * For PCM, we just pass through raw samples and use LEB128 muxing
 */
struct pcm_codec_data {
	/* Input buffer for demuxing */
	struct mux_buffer input_buf;
};

/*
 * PCM encoder initialization
 */
static int pcm_encoder_init(struct mux_encoder *enc,
			    int sample_rate,
			    int num_channels,
			    const struct mux_param *params,
			    int num_params)
{
	/* PCM has no parameters and no special initialization */
	(void)sample_rate;
	(void)num_channels;
	(void)params;
	(void)num_params;

	enc->codec_data = NULL;
	return MUX_OK;
}

/*
 * PCM encoder deinitialization
 */
static void pcm_encoder_deinit(struct mux_encoder *enc)
{
	/* Nothing to clean up */
	(void)enc;
}

/*
 * PCM encoder encode
 * Takes raw PCM or side channel data and writes LEB128-framed packets
 */
static int pcm_encoder_encode(struct mux_encoder *enc,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed,
			      int stream_type)
{
	int ret;

	if (!enc || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	if (input_size == 0) {
		*input_consumed = 0;
		return MUX_OK;
	}

	/* Write LEB128-framed packet to output buffer */
	ret = mux_leb128_write_frame(&enc->output, input, input_size,
				     stream_type);
	if (ret != MUX_OK)
		return ret;

	*input_consumed = input_size;
	return MUX_OK;
}

/*
 * PCM encoder read
 * Reads multiplexed output data
 */
static int pcm_encoder_read(struct mux_encoder *enc,
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
 * PCM encoder finalize
 * No-op for PCM since there's no buffering
 */
static int pcm_encoder_finalize(struct mux_encoder *enc)
{
	(void)enc;
	return MUX_OK;
}

/*
 * PCM decoder initialization
 */
static int pcm_decoder_init(struct mux_decoder *dec,
			    const struct mux_param *params,
			    int num_params)
{
	struct pcm_codec_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	if (mux_buffer_init(&data->input_buf, 4096) != MUX_OK) {
		free(data);
		return MUX_ERROR_NOMEM;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * PCM decoder deinitialization
 */
static void pcm_decoder_deinit(struct mux_decoder *dec)
{
	struct pcm_codec_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;
	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * PCM decoder decode
 * Reads multiplexed input data and demuxes into audio/side channel buffers
 */
static int pcm_decoder_decode(struct mux_decoder *dec,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed)
{
	struct pcm_codec_data *data;
	uint8_t *frame_buf = NULL;
	size_t frame_buf_capacity = 8192;
	size_t frame_size;
	int stream_type;
	int ret;
	size_t consumed = 0;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Allocate initial frame buffer */
	frame_buf = malloc(frame_buf_capacity);
	if (!frame_buf)
		return MUX_ERROR_NOMEM;

	/* Add input to buffer */
	ret = mux_buffer_write(&data->input_buf, input, input_size);
	if (ret != MUX_OK) {
		free(frame_buf);
		return ret;
	}

	consumed = input_size;

	/* Try to read frames from input buffer */
	while (1) {
		ret = mux_leb128_read_frame(&data->input_buf,
					    frame_buf, frame_buf_capacity,
					    &frame_size, &stream_type);
		if (ret == MUX_ERROR_AGAIN) {
			/* Need more data */
			break;
		}
		if (ret == MUX_ERROR_INVAL && frame_size > frame_buf_capacity) {
			/* Frame too large, reallocate buffer */
			uint8_t *new_buf = realloc(frame_buf, frame_size);
			if (!new_buf) {
				free(frame_buf);
				return MUX_ERROR_NOMEM;
			}
			frame_buf = new_buf;
			frame_buf_capacity = frame_size;

			/* Try again */
			ret = mux_leb128_read_frame(&data->input_buf,
						    frame_buf, frame_buf_capacity,
						    &frame_size, &stream_type);
		}

		if (ret != MUX_OK) {
			free(frame_buf);
			return ret;
		}

		/* Write to appropriate output buffer */
		if (stream_type == MUX_STREAM_AUDIO) {
			ret = mux_buffer_write(&dec->audio_output,
					       frame_buf, frame_size);
		} else {
			ret = mux_buffer_write(&dec->side_output,
					       frame_buf, frame_size);
		}

		if (ret != MUX_OK) {
			free(frame_buf);
			return ret;
		}
	}

	free(frame_buf);
	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * PCM decoder read
 * Reads demultiplexed audio or side channel data
 */
static int pcm_decoder_read(struct mux_decoder *dec,
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
 * PCM decoder finalize
 * No-op for PCM since there's no buffering
 */
static int pcm_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * PCM sample rate constraints (supports any rate)
 */
static const int pcm_sample_rates[] = { 1000, 384000 };  /* Min/max range */

/*
 * PCM codec operations
 */
const struct mux_codec_ops mux_codec_pcm_ops = {
	.encoder_init = pcm_encoder_init,
	.encoder_deinit = pcm_encoder_deinit,
	.encoder_encode = pcm_encoder_encode,
	.encoder_read = pcm_encoder_read,
	.encoder_finalize = pcm_encoder_finalize,

	.decoder_init = pcm_decoder_init,
	.decoder_deinit = pcm_decoder_deinit,
	.decoder_decode = pcm_decoder_decode,
	.decoder_read = pcm_decoder_read,
	.decoder_finalize = pcm_decoder_finalize,

	.encoder_params = NULL,
	.encoder_param_count = 0,
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = pcm_sample_rates,
	.sample_rate_count = 2,
	.sample_rate_is_range = 1
};
