/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * AMR-WB (Adaptive Multi-Rate Wideband) codec
 *
 * Uses opencore-amrwb library for decoding.
 * Also known as G.722.2, standard for HD voice in 3G/4G/VoLTE.
 *
 * Note: Encoding requires vo-amrwbenc library (separate from opencore-amrwb).
 * This implementation supports decode-only with opencore-amrwb,
 * and encode+decode when vo-amrwbenc is available.
 *
 * Characteristics:
 * - Sample rate: 16000 Hz
 * - Frame size: 320 samples (20ms)
 * - Bitrates: 6.6, 8.85, 12.65, 14.25, 15.85, 18.25, 19.85, 23.05, 23.85 kbps
 */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_AMR_WB

#include <opencore-amrwb/dec_if.h>

#ifdef HAVE_AMR_WB_ENCODE
#include <vo-amrwbenc/enc_if.h>
#endif

/* AMR-WB constants */
#define AMR_WB_SAMPLE_RATE     16000
#define AMR_WB_FRAME_SAMPLES   320   /* 20ms at 16kHz */
#define AMR_WB_MAX_FRAME_SIZE  61    /* Maximum encoded frame size */

/*
 * AMR-WB modes (bitrates)
 * 0 = 6.60 kbps
 * 1 = 8.85 kbps
 * 2 = 12.65 kbps
 * 3 = 14.25 kbps
 * 4 = 15.85 kbps
 * 5 = 18.25 kbps
 * 6 = 19.85 kbps
 * 7 = 23.05 kbps
 * 8 = 23.85 kbps (default)
 */
#define AMR_WB_MODE_660   0
#define AMR_WB_MODE_885   1
#define AMR_WB_MODE_1265  2
#define AMR_WB_MODE_1425  3
#define AMR_WB_MODE_1585  4
#define AMR_WB_MODE_1825  5
#define AMR_WB_MODE_1985  6
#define AMR_WB_MODE_2305  7
#define AMR_WB_MODE_2385  8

/*
 * AMR-WB encoder state
 */
struct amr_wb_encoder_data {
#ifdef HAVE_AMR_WB_ENCODE
	void *encoder;
	int mode;  /* AMR-WB mode (0-8) */
	int dtx;

	/* Input buffer for accumulating samples */
	int16_t input_buf[AMR_WB_FRAME_SAMPLES];
	int input_samples;
#else
	int dummy;  /* Placeholder when encoding not available */
#endif
};

/*
 * AMR-WB decoder state
 */
struct amr_wb_decoder_data {
	void *decoder;
	struct mux_buffer input_buf;
};

/*
 * Parameter descriptors
 */
static const struct mux_param_desc amr_wb_encoder_params[] = {
	{
		.name = "bitrate",
		.description = "Bitrate in kbps (6.6, 8.85, 12.65, 14.25, 15.85, 18.25, 19.85, 23.05, 23.85)",
		.type = MUX_PARAM_TYPE_FLOAT,
		.range.f = { .min = 6.6f, .max = 23.85f, .def = 23.85f }
	},
	{
		.name = "dtx",
		.description = "Enable discontinuous transmission (silence compression)",
		.type = MUX_PARAM_TYPE_BOOL,
		.range.b = { .def = 0 }
	}
};

#ifdef HAVE_AMR_WB_ENCODE
/*
 * Convert bitrate to AMR-WB mode
 */
static int bitrate_to_mode(float bitrate)
{
	if (bitrate <= 6.6f)  return AMR_WB_MODE_660;
	if (bitrate <= 8.85f) return AMR_WB_MODE_885;
	if (bitrate <= 12.65f) return AMR_WB_MODE_1265;
	if (bitrate <= 14.25f) return AMR_WB_MODE_1425;
	if (bitrate <= 15.85f) return AMR_WB_MODE_1585;
	if (bitrate <= 18.25f) return AMR_WB_MODE_1825;
	if (bitrate <= 19.85f) return AMR_WB_MODE_1985;
	if (bitrate <= 23.05f) return AMR_WB_MODE_2305;
	return AMR_WB_MODE_2385;
}
#endif

/*
 * AMR-WB encoder initialization
 */
static int amr_wb_encoder_init(struct mux_encoder *enc,
			       int sample_rate,
			       int num_channels,
			       const struct mux_param *params,
			       int num_params)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;
	int i;

	/* AMR-WB only supports 16kHz mono */
	if (sample_rate != AMR_WB_SAMPLE_RATE) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-WB requires 16000 Hz sample rate",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	if (num_channels != 1) {
		mux_encoder_set_error(enc, MUX_ERROR_INVAL,
				      "AMR-WB requires mono audio",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INVAL;
	}

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	/* Default settings */
	data->mode = AMR_WB_MODE_2385;
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
	data->encoder = E_IF_init();
	if (!data->encoder) {
		free(data);
		mux_encoder_set_error(enc, MUX_ERROR_INIT,
				      "Failed to initialize AMR-WB encoder",
				      "vo-amrwbenc", 0, NULL);
		return MUX_ERROR_INIT;
	}

	data->input_samples = 0;
	enc->codec_data = data;

	return MUX_OK;
#else
	(void)sample_rate;
	(void)num_channels;
	(void)params;
	(void)num_params;

	mux_encoder_set_error(enc, MUX_ERROR_NOCODEC,
			      "AMR-WB encoding not available (vo-amrwbenc not found)",
			      "vo-amrwbenc", 0, NULL);
	return MUX_ERROR_NOCODEC;
#endif
}

/*
 * AMR-WB encoder deinitialization
 */
static void amr_wb_encoder_deinit(struct mux_encoder *enc)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;

	if (!enc || !enc->codec_data)
		return;

	data = enc->codec_data;

	if (data->encoder)
		E_IF_exit(data->encoder);

	free(data);
	enc->codec_data = NULL;
#else
	(void)enc;
#endif
}

/*
 * AMR-WB encoder encode
 */
static int amr_wb_encoder_encode(struct mux_encoder *enc,
				 const void *input,
				 size_t input_size,
				 size_t *input_consumed,
				 int stream_type)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;
	const int16_t *samples;
	uint8_t frame_buf[AMR_WB_MAX_FRAME_SIZE];
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
		size_t need = AMR_WB_FRAME_SAMPLES - data->input_samples;
		size_t copy = (samples_available < need) ? samples_available : need;

		memcpy(&data->input_buf[data->input_samples], samples,
		       copy * sizeof(int16_t));
		data->input_samples += copy;
		samples += copy;
		samples_available -= copy;
		consumed += copy * sizeof(int16_t);

		/* Encode complete frame */
		if (data->input_samples == AMR_WB_FRAME_SAMPLES) {
			frame_size = E_IF_encode(
				data->encoder,
				data->mode,
				data->input_buf,
				frame_buf,
				data->dtx
			);

			if (frame_size < 0) {
				mux_encoder_set_error(enc, MUX_ERROR_ENCODE,
						      "AMR-WB encoding failed",
						      "vo-amrwbenc", frame_size, NULL);
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
#else
	(void)enc;
	(void)input;
	(void)input_size;
	(void)input_consumed;
	(void)stream_type;
	return MUX_ERROR_NOCODEC;
#endif
}

/*
 * AMR-WB encoder read
 */
static int amr_wb_encoder_read(struct mux_encoder *enc,
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
 * AMR-WB encoder finalize
 */
static int amr_wb_encoder_finalize(struct mux_encoder *enc)
{
#ifdef HAVE_AMR_WB_ENCODE
	struct amr_wb_encoder_data *data;
	uint8_t frame_buf[AMR_WB_MAX_FRAME_SIZE];
	int frame_size;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	data = enc->codec_data;
	if (!data)
		return MUX_ERROR_INVAL;

	/* Encode any remaining samples (pad with zeros) */
	if (data->input_samples > 0) {
		memset(&data->input_buf[data->input_samples], 0,
		       (AMR_WB_FRAME_SAMPLES - data->input_samples) * sizeof(int16_t));

		frame_size = E_IF_encode(
			data->encoder,
			data->mode,
			data->input_buf,
			frame_buf,
			data->dtx
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
#else
	(void)enc;
	return MUX_OK;
#endif
}

/*
 * AMR-WB decoder initialization
 */
static int amr_wb_decoder_init(struct mux_decoder *dec,
			       const struct mux_param *params,
			       int num_params)
{
	struct amr_wb_decoder_data *data;

	(void)params;
	(void)num_params;

	data = calloc(1, sizeof(*data));
	if (!data)
		return MUX_ERROR_NOMEM;

	if (mux_buffer_init(&data->input_buf, 4096) != MUX_OK) {
		free(data);
		return MUX_ERROR_NOMEM;
	}

	data->decoder = D_IF_init();
	if (!data->decoder) {
		mux_buffer_deinit(&data->input_buf);
		free(data);
		mux_decoder_set_error(dec, MUX_ERROR_INIT,
				      "Failed to initialize AMR-WB decoder",
				      "opencore-amrwb", 0, NULL);
		return MUX_ERROR_INIT;
	}

	dec->codec_data = data;
	return MUX_OK;
}

/*
 * AMR-WB decoder deinitialization
 */
static void amr_wb_decoder_deinit(struct mux_decoder *dec)
{
	struct amr_wb_decoder_data *data;

	if (!dec || !dec->codec_data)
		return;

	data = dec->codec_data;

	if (data->decoder)
		D_IF_exit(data->decoder);

	mux_buffer_deinit(&data->input_buf);
	free(data);
	dec->codec_data = NULL;
}

/*
 * Get AMR-WB frame size from mode byte
 * AMR-WB frame sizes (packed, including mode byte):
 */
static int amr_wb_get_frame_size(uint8_t mode_byte)
{
	/* Extract frame type from bits 7-4 (shifted right by 3, masked with 0x0F) */
	int frame_type = (mode_byte >> 3) & 0x0F;

	/* Frame sizes for each frame type */
	static const int frame_sizes[] = {
		18,  /* Mode 0: 6.60 kbps */
		24,  /* Mode 1: 8.85 kbps */
		33,  /* Mode 2: 12.65 kbps */
		37,  /* Mode 3: 14.25 kbps */
		41,  /* Mode 4: 15.85 kbps */
		47,  /* Mode 5: 18.25 kbps */
		51,  /* Mode 6: 19.85 kbps */
		59,  /* Mode 7: 23.05 kbps */
		61,  /* Mode 8: 23.85 kbps */
		6,   /* Mode 9: SID (comfort noise) */
		0, 0, 0, 0,  /* Modes 10-13: reserved */
		1,   /* Mode 14: speech lost */
		1    /* Mode 15: NO_DATA */
	};

	if (frame_type > 15)
		return -1;

	return frame_sizes[frame_type];
}

/*
 * AMR-WB decoder decode
 */
static int amr_wb_decoder_decode(struct mux_decoder *dec,
				 const void *input,
				 size_t input_size,
				 size_t *input_consumed)
{
	struct amr_wb_decoder_data *data;
	uint8_t *frame_buf = NULL;
	int16_t pcm_out[AMR_WB_FRAME_SAMPLES];
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

	/* Passthrough mode: parse AMR-WB frames directly */
	if (dec->num_streams == 1) {
		while (data->input_buf.size - data->input_buf.read_pos > 0) {
			uint8_t *buf_ptr = data->input_buf.data + data->input_buf.read_pos;
			size_t available = data->input_buf.size - data->input_buf.read_pos;
			int amr_frame_size;

			/* Get frame size from mode byte */
			amr_frame_size = amr_wb_get_frame_size(buf_ptr[0]);
			if (amr_frame_size <= 0) {
				free(frame_buf);
				return MUX_ERROR_DECODE;
			}

			/* Check if we have a complete frame */
			if (available < (size_t)amr_frame_size)
				break;

			/* Decode AMR-WB frame */
			D_IF_decode(data->decoder, buf_ptr, pcm_out, 0);

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
			/* Decode AMR-WB frame */
			D_IF_decode(data->decoder, frame_buf, pcm_out, 0);

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
 * AMR-WB decoder read
 */
static int amr_wb_decoder_read(struct mux_decoder *dec,
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
 * AMR-WB decoder finalize
 */
static int amr_wb_decoder_finalize(struct mux_decoder *dec)
{
	(void)dec;
	return MUX_OK;
}

/*
 * AMR-WB only supports 16kHz
 */
static const int amr_wb_sample_rates[] = { 16000 };

/*
 * AMR-WB codec operations
 */
const struct mux_codec_ops mux_codec_amr_wb_ops = {
	.encoder_init = amr_wb_encoder_init,
	.encoder_deinit = amr_wb_encoder_deinit,
	.encoder_encode = amr_wb_encoder_encode,
	.encoder_read = amr_wb_encoder_read,
	.encoder_finalize = amr_wb_encoder_finalize,

	.decoder_init = amr_wb_decoder_init,
	.decoder_deinit = amr_wb_decoder_deinit,
	.decoder_decode = amr_wb_decoder_decode,
	.decoder_read = amr_wb_decoder_read,
	.decoder_finalize = amr_wb_decoder_finalize,

	.encoder_params = amr_wb_encoder_params,
	.encoder_param_count = sizeof(amr_wb_encoder_params) / sizeof(amr_wb_encoder_params[0]),
	.decoder_params = NULL,
	.decoder_param_count = 0,

	.supported_sample_rates = amr_wb_sample_rates,
	.sample_rate_count = 1,
	.sample_rate_is_range = 0
};

#endif /* HAVE_AMR_WB */
