/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AMR-NB (Adaptive Multi-Rate Narrowband) codec
 *
 * Uses opencore-amr library for encoding and decoding.
 * Standard for GSM and 3G voice communication.
 *
 * Characteristics:
 * - Sample rate: 8000 Hz
 * - Frame size: 160 samples (20ms)
 * - Bitrates: 4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2 kbps
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_AMR
#include <opencore-amrnb/interf_enc.h>
#include <opencore-amrnb/interf_dec.h>

/* AMR-NB constants */
#define AMR_SAMPLE_RATE     8000
#define AMR_FRAME_SAMPLES   160   /* 20ms at 8kHz */
#define AMR_MAX_FRAME_SIZE  32    /* Maximum encoded frame size */

/*
 * AMR modes (bitrates) - use library's enum Mode values
 * MR475 = 0  (4.75 kbps)
 * MR515 = 1  (5.15 kbps)
 * MR59  = 2  (5.90 kbps)
 * MR67  = 3  (6.70 kbps)
 * MR74  = 4  (7.40 kbps)
 * MR795 = 5  (7.95 kbps)
 * MR102 = 6  (10.2 kbps)
 * MR122 = 7  (12.2 kbps, default)
 */
#define AMR_MODE_475   0
#define AMR_MODE_515   1
#define AMR_MODE_590   2
#define AMR_MODE_670   3
#define AMR_MODE_740   4
#define AMR_MODE_795   5
#define AMR_MODE_1020  6
#define AMR_MODE_1220  7

/*
 * AMR encoder state
 */
struct amr_encoder_data {
	void *encoder;
	int mode;  /* AMR mode (0-7) */
	int dtx;   /* Discontinuous transmission */

	/* Input buffer for accumulating samples */
	int16_t input_buf[AMR_FRAME_SAMPLES];
	int input_samples;
};

/*
 * AMR decoder state
 */
struct amr_decoder_data {
	void *decoder;
	struct mux_buffer input_buf;
};

/*
 * Parameter descriptors
 */
static const struct mux_param_desc amr_encoder_params[] = {
	{
		.name = "bitrate",
		.description = "Bitrate in kbps (4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2)",
		.type = MUX_PARAM_TYPE_FLOAT,
		.range.f = { .min = 4.75f, .max = 12.2f, .def = 12.2f }
	},
	{
		.name = "dtx",
		.description = "Enable discontinuous transmission (silence compression)",
		.type = MUX_PARAM_TYPE_BOOL,
		.range.b = { .def = 0 }
	}
};

/*
 * Convert bitrate to AMR mode
 */
static int bitrate_to_mode(float bitrate)
{
	if (bitrate <= 4.75f) return AMR_MODE_475;
	if (bitrate <= 5.15f) return AMR_MODE_515;
	if (bitrate <= 5.9f)  return AMR_MODE_590;
	if (bitrate <= 6.7f)  return AMR_MODE_670;
	if (bitrate <= 7.4f)  return AMR_MODE_740;
	if (bitrate <= 7.95f) return AMR_MODE_795;
	if (bitrate <= 10.2f) return AMR_MODE_1020;
	return AMR_MODE_1220;
}

/*
 * AMR encoder initialization
 */
static int amr_encoder_init(struct mux_encoder *enc,
			    int sample_rate,
			    int num_channels,
			    const struct mux_param *params,
			    int num_params)
{
	struct amr_encoder_data *data;
	int i;

	/* AMR only supports 8kHz mono */
	if (sample_rate != AMR_SAMPLE_RATE) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-NB requires 8000 Hz sample rate",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	if (num_channels != 1) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-NB requires mono audio",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	/* Default settings */
	data->mode = AMR_MODE_1220;
	data->dtx = 0;

	/* Parse parameters */
	for (i = 0; i < num_params; i++) {
		if (strcmp(params[i].name, "bitrate") == 0) {
			data->mode = bitrate_to_mode(params[i].value.f);
		} else if (strcmp(params[i].name, "dtx") == 0) {
			data->dtx = params[i].value.b;
		}
	}

	/* Create encoder */
	data->encoder = Encoder_Interface_init(data->dtx);
	if (!data->encoder) {
		free(data);
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AMR encoder",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	data->input_samples = 0;
	enc->codec_data = data;

	return MUX_OK;
}

/*
 * AMR encoder deinitialization
 */
static void amr_encoder_deinit(struct mux_encoder *enc)
{
	struct amr_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	if (data->encoder)
		Encoder_Interface_exit(data->encoder);

	free(data);
	enc->codec_data = NULL;
}

/*
 * AMR encoder encode
 */
static int amr_encoder_encode(struct mux_encoder *enc,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed,
			      int stream_type)
{
	struct amr_encoder_data *data;
	const int16_t *samples;
	uint8_t frame_buf[AMR_MAX_FRAME_SIZE];
	size_t samples_available;
	size_t consumed = 0;
	int frame_size;
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

	/* Side channel: pass through unchanged */
	if (stream_type != MUX_STREAM_AUDIO) {
		ret = mux_leb128_write_frame(&enc->output, input, input_size,
					     stream_type, enc->num_streams);
		if (ret != MUX_OK)
			return ret;
		*input_consumed = input_size;
		return MUX_OK;
	}

	/* Audio encoding */
	samples = (const int16_t *)input;
	samples_available = input_size / sizeof(int16_t);

	while (samples_available > 0) {
		/* Fill input buffer */
		size_t need = AMR_FRAME_SAMPLES - data->input_samples;
		size_t copy = (samples_available < need) ? samples_available : need;

		memcpy(&data->input_buf[data->input_samples], samples,
		       copy * sizeof(int16_t));
		data->input_samples += copy;
		samples += copy;
		samples_available -= copy;
		consumed += copy * sizeof(int16_t);

		/* Encode complete frame */
		if (data->input_samples == AMR_FRAME_SAMPLES) {
			frame_size = Encoder_Interface_Encode(
				data->encoder,
				data->mode,
				data->input_buf,
				frame_buf,
				0  /* force_speech */
			);

			if (frame_size < 0) {
				mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
						      "AMR encoding failed",
						      "opencore-amrnb", frame_size, NULL);
				return MUX_ERROR_ENCODE;
			}

			/* Write frame */
			ret = mux_leb128_write_frame(&enc->output, frame_buf, frame_size,
						     MUX_STREAM_AUDIO, enc->num_streams);
			if (ret != MUX_OK)
				return ret;

			data->input_samples = 0;
		}
	}

	*input_consumed = consumed;
	return MUX_OK;
}

/*
 * AMR encoder read
 */
static int amr_encoder_read(struct mux_encoder *enc,
			    void *output,
			    size_t output_size,
			    size_t *output_written)
{
	size_t bytes_read;
	int ret;

	if (!enc || !output || !output_written)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&enc->output, output, output_size, &bytes_read);
	if (ret != MUX_OK) {
		*output_written = 0;
		return ret;
	}

	*output_written = bytes_read;
	return MUX_OK;
}

/*
 * AMR encoder finalize
 */
static int amr_encoder_finalize(struct mux_encoder *enc)
{
	struct amr_encoder_data *data;
	uint8_t frame_buf[AMR_MAX_FRAME_SIZE];
	int frame_size;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Encode any remaining samples (pad with zeros) */
	if (data->input_samples > 0) {
		/* Zero-pad the remaining buffer */
		memset(&data->input_buf[data->input_samples], 0,
		       (AMR_FRAME_SAMPLES - data->input_samples) * sizeof(int16_t));

		frame_size = Encoder_Interface_Encode(
			data->encoder,
			data->mode,
			data->input_buf,
			frame_buf,
			0
		);

		if (frame_size > 0) {
			ret = mux_leb128_write_frame(&enc->output, frame_buf, frame_size,
						     MUX_STREAM_AUDIO, enc->num_streams);
			if (ret != MUX_OK)
				return ret;
		}

		data->input_samples = 0;
	}

	return MUX_OK;
}

/*
 * AMR decoder initialization
 */
static int amr_decoder_init(struct mux_decoder *dec,
			    const struct mux_param *params,
			    int num_params)
{
	struct amr_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	if (mux_buffer_init(&data->input_buf, 4096) != MUX_OK) {
		free(data);
		return MUX_ERROR_NOMEM;
	}

	data->decoder = Decoder_Interface_init();
	if (!data->decoder) {
		mux_buffer_deinit(&data->input_buf);
		free(data);
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize AMR decoder",
				      "opencore-amrnb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * AMR decoder deinitialization
 */
static void amr_decoder_deinit(struct mux_decoder *dec)
{
	struct amr_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->decoder)
		Decoder_Interface_exit(data->decoder);

	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * Get AMR frame size from mode byte
 * AMR frame sizes (packed, including mode byte):
 * Mode 0-7: speech frames, mode 8: SID, mode 15: NO_DATA
 */
static int amr_get_frame_size(uint8_t mode_byte)
{
	/* Extract frame type from bits 7-4 */
	int frame_type = (mode_byte >> 3) & 0x0F;

	/* Frame sizes for each frame type */
	static const int frame_sizes[] = {
		13,  /* Mode 0: 4.75 kbps */
		14,  /* Mode 1: 5.15 kbps */
		16,  /* Mode 2: 5.90 kbps */
		18,  /* Mode 3: 6.70 kbps */
		20,  /* Mode 4: 7.40 kbps */
		21,  /* Mode 5: 7.95 kbps */
		27,  /* Mode 6: 10.2 kbps */
		32,  /* Mode 7: 12.2 kbps */
		6,   /* Mode 8: SID (comfort noise) */
		0, 0, 0, 0, 0, 0,  /* Modes 9-14: reserved */
		1    /* Mode 15: NO_DATA */
	};

	if (frame_type > 15)
		return -1;

	return frame_sizes[frame_type];
}

/*
 * AMR decoder decode
 */
static int amr_decoder_decode(struct mux_decoder *dec,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed)
{
	struct amr_decoder_data *data;
	uint8_t *frame_buf = NULL;
	int16_t pcm_out[AMR_FRAME_SAMPLES];
	size_t frame_buf_capacity = 1024;
	size_t frame_size;
	int stream_type;
	int ret;

	if (!dec || !input || !input_consumed)
		return MUX_ERROR_INVAL;

	data = dec->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	frame_buf = malloc(frame_buf_capacity);
	if (!frame_buf)
		return MUX_ERROR_NOMEM;

	ret = mux_buffer_write(&data->input_buf, input, input_size);
	if (ret != MUX_OK) {
		free(frame_buf);
		return ret;
	}

	*input_consumed = input_size;

	/* Passthrough mode: parse AMR frames directly */
	if (dec->num_streams == 1) {
		while (data->input_buf.size - data->input_buf.read_pos > 0) {
			uint8_t *buf_ptr = data->input_buf.data + data->input_buf.read_pos;
			size_t available = data->input_buf.size - data->input_buf.read_pos;
			int amr_frame_size;

			/* Get frame size from mode byte */
			amr_frame_size = amr_get_frame_size(buf_ptr[0]);
			if (amr_frame_size <= 0) {
				free(frame_buf);
				return MUX_ERROR_DECODE;
			}

			/* Check if we have a complete frame */
			if (available < (size_t)amr_frame_size)
				break;

			/* Decode AMR frame */
			Decoder_Interface_Decode(data->decoder, buf_ptr, pcm_out, 0);

			ret = mux_buffer_write(&dec->audio_output,
					       pcm_out, sizeof(pcm_out));
			if (ret != MUX_OK) {
				free(frame_buf);
				return ret;
			}

			/* Advance read position */
			data->input_buf.read_pos += amr_frame_size;
		}

		/* Reset buffer if fully read */
		if (data->input_buf.read_pos == data->input_buf.size) {
			data->input_buf.read_pos = 0;
			data->input_buf.size = 0;
		}

		free(frame_buf);
		return MUX_OK;
	}

	/* Mux mode: use LEB128 framing */
	while (1) {
		ret = mux_leb128_read_frame(&data->input_buf,
					    frame_buf, frame_buf_capacity,
					    &frame_size, &stream_type, dec->num_streams);
		if (ret == MUX_ERROR_AGAIN)
			break;

		if (ret == MUX_ERROR_INVAL && frame_size > frame_buf_capacity) {
			uint8_t *new_buf = realloc(frame_buf, frame_size);
			if (!new_buf) {
				free(frame_buf);
				return MUX_ERROR_NOMEM;
			}
			frame_buf = new_buf;
			frame_buf_capacity = frame_size;

			ret = mux_leb128_read_frame(&data->input_buf,
						    frame_buf, frame_buf_capacity,
						    &frame_size, &stream_type, dec->num_streams);
		}

		if (ret != MUX_OK) {
			free(frame_buf);
			return ret;
		}

		if (stream_type == MUX_STREAM_AUDIO) {
			/* Decode AMR frame */
			Decoder_Interface_Decode(data->decoder, frame_buf, pcm_out, 0);

			ret = mux_buffer_write(&dec->audio_output,
					       pcm_out, sizeof(pcm_out));
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
	return MUX_OK;
}

/*
 * AMR decoder read
 */
static int amr_decoder_read(struct mux_decoder *dec,
			    void *output,
			    size_t output_size,
			    size_t *output_written,
			    int *stream_type)
{
	size_t bytes_read;
	int ret;

	if (!dec || !output || !output_written || !stream_type)
		return MUX_ERROR_INVAL;

	ret = mux_buffer_read(&dec->audio_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_AUDIO;
		return MUX_OK;
	}

	ret = mux_buffer_read(&dec->side_output, output, output_size,
			      &bytes_read);
	if (ret == MUX_OK) {
		*output_written = bytes_read;
		*stream_type = MUX_STREAM_SIDE_CHANNEL;
		return MUX_OK;
	}

	*output_written = 0;
	return MUX_ERROR_AGAIN;
}

/*
 * AMR decoder finalize
 */
static int amr_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * AMR-NB only supports 8kHz
 */
static const int amr_sample_rates[] = { 8000 };

/*
 * AMR codec operations
 */
const struct mux_codec_ops mux_codec_amr_ops = {
	.encoder_init = amr_encoder_init,
	.encoder_deinit = amr_encoder_deinit,
	.encoder_encode = amr_encoder_encode,
	.encoder_read = amr_encoder_read,
	.encoder_finalize = amr_encoder_finalize,

	.decoder_init = amr_decoder_init,
	.decoder_deinit = amr_decoder_deinit,
	.decoder_decode = amr_decoder_decode,
	.decoder_read = amr_decoder_read,
	.decoder_finalize = amr_decoder_finalize,

	.encoder_params = amr_encoder_params,
	.encoder_param_count = sizeof(amr_encoder_params) / sizeof(amr_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = amr_sample_rates,
	.sample_rate_count = 1,
	.sample_rate_is_range = 0
};

#endif /* HAVE_AMR */
