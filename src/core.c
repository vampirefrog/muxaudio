/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

/*
 * Codec information table
 */
static const struct mux_codec_info codec_info_table[] = {
	{
		.type = MUX_CODEC_PCM,
		.name = "pcm",
		.description = "Raw PCM audio"
	},
	{
		.type = MUX_CODEC_OPUS,
		.name = "opus",
		.description = "Opus audio codec"
	},
	{
		.type = MUX_CODEC_VORBIS,
		.name = "vorbis",
		.description = "Vorbis audio codec"
	},
	{
		.type = MUX_CODEC_FLAC,
		.name = "flac",
		.description = "FLAC lossless audio codec"
	},
	{
		.type = MUX_CODEC_MP3,
		.name = "mp3",
		.description = "MP3 audio codec"
	},
	{
		.type = MUX_CODEC_AAC,
		.name = "aac",
		.description = "AAC audio codec"
	},
	{
		.type = MUX_CODEC_ALAW,
		.name = "alaw",
		.description = "G.711 A-law codec"
	},
	{
		.type = MUX_CODEC_MULAW,
		.name = "mulaw",
		.description = "G.711 mu-law codec"
	},
	{
		.type = MUX_CODEC_AMR,
		.name = "amr",
		.description = "AMR-NB (Adaptive Multi-Rate Narrowband) codec"
	},
	{
		.type = MUX_CODEC_AMR_WB,
		.name = "amr-wb",
		.description = "AMR-WB (Adaptive Multi-Rate Wideband) codec"
	}
};

/*
 * Codec operations table
 */
static const struct mux_codec_ops *codec_ops_table[MUX_CODEC_MAX] = {
	[MUX_CODEC_PCM] = &mux_codec_pcm_ops,
#ifdef HAVE_MP3
	[MUX_CODEC_MP3] = &mux_codec_mp3_ops,
#else
	[MUX_CODEC_MP3] = NULL,
#endif
#ifdef HAVE_VORBIS
	[MUX_CODEC_VORBIS] = &mux_codec_vorbis_ops,
#else
	[MUX_CODEC_VORBIS] = NULL,
#endif
#ifdef HAVE_OPUS
	[MUX_CODEC_OPUS] = &mux_codec_opus_ops,
#else
	[MUX_CODEC_OPUS] = NULL,
#endif
#ifdef HAVE_FLAC
	[MUX_CODEC_FLAC] = &mux_codec_flac_ops,
#else
	[MUX_CODEC_FLAC] = NULL,
#endif
#ifdef HAVE_AAC
	[MUX_CODEC_AAC] = &mux_codec_aac_ops,
#else
	[MUX_CODEC_AAC] = NULL,
#endif
	/* G.711 codecs - always available (no external dependencies) */
	[MUX_CODEC_ALAW] = &mux_codec_alaw_ops,
	[MUX_CODEC_MULAW] = &mux_codec_mulaw_ops,
#ifdef HAVE_AMR
	[MUX_CODEC_AMR] = &mux_codec_amr_ops,
#else
	[MUX_CODEC_AMR] = NULL,
#endif
#ifdef HAVE_AMR_WB
	[MUX_CODEC_AMR_WB] = &mux_codec_amr_wb_ops
#else
	[MUX_CODEC_AMR_WB] = NULL
#endif
};

int mux_list_codecs(const struct mux_codec_info **codecs, int *count)
{
	if (!codecs || !count)
		return MUX_ERROR_INVAL;

	*codecs = codec_info_table;
	*count = MUX_CODEC_MAX;

	return MUX_OK;
}

int mux_codec_from_name(const char *name, enum mux_codec_type *codec)
{
	int i;

	if (!name || !codec)
		return MUX_ERROR_INVAL;

	for (i = 0; i < MUX_CODEC_MAX; i++) {
		if (strcmp(codec_info_table[i].name, name) == 0) {
			*codec = codec_info_table[i].type;
			return MUX_OK;
		}
	}

	return MUX_ERROR_INVAL;
}

const char *mux_codec_to_name(enum mux_codec_type codec)
{
	if (codec < 0 || codec >= MUX_CODEC_MAX)
		return NULL;

	return codec_info_table[codec].name;
}

const struct mux_codec_ops *mux_get_codec_ops(enum mux_codec_type type)
{
	if (type < 0 || type >= MUX_CODEC_MAX)
		return NULL;

	return codec_ops_table[type];
}

int mux_get_encoder_params(enum mux_codec_type codec_type,
			   const struct mux_param_desc **params,
			   int *count)
{
	const struct mux_codec_ops *ops;

	if (!params || !count)
		return MUX_ERROR_INVAL;

	ops = mux_get_codec_ops(codec_type);
	if (!ops)
		return MUX_ERROR_NOCODEC;

	*params = ops->encoder_params;
	*count = ops->encoder_param_count;

	return MUX_OK;
}

int mux_get_decoder_params(enum mux_codec_type codec_type,
			   const struct mux_param_desc **params,
			   int *count)
{
	const struct mux_codec_ops *ops;

	if (!params || !count)
		return MUX_ERROR_INVAL;

	ops = mux_get_codec_ops(codec_type);
	if (!ops)
		return MUX_ERROR_NOCODEC;

	*params = ops->decoder_params;
	*count = ops->decoder_param_count;

	return MUX_OK;
}

int mux_get_supported_sample_rates(enum mux_codec_type codec_type,
				    struct mux_sample_rate_list *list)
{
	const struct mux_codec_ops *ops;

	if (!list)
		return MUX_ERROR_INVAL;

	ops = mux_get_codec_ops(codec_type);
	if (!ops)
		return MUX_ERROR_NOCODEC;

	list->rates = ops->supported_sample_rates;
	list->count = ops->sample_rate_count;
	list->is_range = ops->sample_rate_is_range;

	return MUX_OK;
}

/*
 * Encoder - static allocation
 */
int mux_encoder_init(struct mux_encoder *enc,
		     enum mux_codec_type codec_type,
		     int sample_rate,
		     int num_channels,
		     int num_streams,
		     const struct mux_param *params,
		     int num_params)
{
	const struct mux_codec_ops *ops;
	int ret;

	if (!enc)
		return MUX_ERROR_INVAL;

	if (num_streams != 1 && num_streams != 2)
		return MUX_ERROR_INVAL;

	memset(enc, 0, sizeof(*enc));

	ops = mux_get_codec_ops(codec_type);
	if (!ops || !ops->encoder_init)
		return MUX_ERROR_NOCODEC;

	enc->codec_type = codec_type;
	enc->ops = ops;
	enc->sample_rate = sample_rate;
	enc->num_channels = num_channels;
	enc->num_streams = num_streams;

	ret = mux_buffer_init(&enc->output, 4096);
	if (ret != MUX_OK)
		return ret;

	ret = ops->encoder_init(enc, sample_rate, num_channels,
				params, num_params);
	if (ret != MUX_OK) {
		mux_buffer_deinit(&enc->output);
		return ret;
	}

	return MUX_OK;
}

void mux_encoder_deinit(struct mux_encoder *enc)
{
	if (!enc)
		return;

	if (enc->ops && enc->ops->encoder_deinit)
		enc->ops->encoder_deinit(enc);

	mux_buffer_deinit(&enc->output);
	memset(enc, 0, sizeof(*enc));
}

/*
 * Encoder - dynamic allocation
 */
struct mux_encoder *mux_encoder_new(enum mux_codec_type codec_type,
				    int sample_rate,
				    int num_channels,
				    int num_streams,
				    const struct mux_param *params,
				    int num_params)
{
	struct mux_encoder *enc;
	int ret;

	enc = calloc(1, sizeof(*enc));
	if (!enc)
		return NULL;

	ret = mux_encoder_init(enc, codec_type, sample_rate, num_channels,
			       num_streams, params, num_params);
	if (ret != MUX_OK) {
		free(enc);
		return NULL;
	}

	return enc;
}

void mux_encoder_destroy(struct mux_encoder *enc)
{
	if (!enc)
		return;

	mux_encoder_deinit(enc);
	free(enc);
}

/*
 * Decoder - static allocation
 */
int mux_decoder_init(struct mux_decoder *dec,
		     enum mux_codec_type codec_type,
		     int num_streams,
		     const struct mux_param *params,
		     int num_params)
{
	const struct mux_codec_ops *ops;
	int ret;

	if (!dec)
		return MUX_ERROR_INVAL;

	if (num_streams != 1 && num_streams != 2)
		return MUX_ERROR_INVAL;

	memset(dec, 0, sizeof(*dec));

	ops = mux_get_codec_ops(codec_type);
	if (!ops || !ops->decoder_init)
		return MUX_ERROR_NOCODEC;

	dec->codec_type = codec_type;
	dec->ops = ops;
	dec->num_streams = num_streams;

	ret = mux_buffer_init(&dec->audio_output, 4096);
	if (ret != MUX_OK)
		return ret;

	ret = mux_buffer_init(&dec->side_output, 1024);
	if (ret != MUX_OK) {
		mux_buffer_deinit(&dec->audio_output);
		return ret;
	}

	ret = ops->decoder_init(dec, params, num_params);
	if (ret != MUX_OK) {
		mux_buffer_deinit(&dec->audio_output);
		mux_buffer_deinit(&dec->side_output);
		return ret;
	}

	return MUX_OK;
}

void mux_decoder_deinit(struct mux_decoder *dec)
{
	if (!dec)
		return;

	if (dec->ops && dec->ops->decoder_deinit)
		dec->ops->decoder_deinit(dec);

	mux_buffer_deinit(&dec->audio_output);
	mux_buffer_deinit(&dec->side_output);
	memset(dec, 0, sizeof(*dec));
}

/*
 * Decoder - dynamic allocation
 */
struct mux_decoder *mux_decoder_new(enum mux_codec_type codec_type,
				    int num_streams,
				    const struct mux_param *params,
				    int num_params)
{
	struct mux_decoder *dec;
	int ret;

	dec = calloc(1, sizeof(*dec));
	if (!dec)
		return NULL;

	ret = mux_decoder_init(dec, codec_type, num_streams, params, num_params);
	if (ret != MUX_OK) {
		free(dec);
		return NULL;
	}

	return dec;
}

void mux_decoder_destroy(struct mux_decoder *dec)
{
	if (!dec)
		return;

	mux_decoder_deinit(dec);
	free(dec);
}

/*
 * Encoding operations
 */
int mux_encoder_encode(struct mux_encoder *enc,
		       const void *input,
		       size_t input_size,
		       size_t *input_consumed,
		       int stream_type)
{
	if (!enc || !enc->ops || !enc->ops->encoder_encode)
		return MUX_ERROR_INVAL;

	return enc->ops->encoder_encode(enc, input, input_size,
					input_consumed, stream_type);
}

int mux_encoder_read(struct mux_encoder *enc,
		     void *output,
		     size_t output_size,
		     size_t *output_written)
{
	if (!enc || !enc->ops || !enc->ops->encoder_read)
		return MUX_ERROR_INVAL;

	return enc->ops->encoder_read(enc, output, output_size,
				      output_written);
}

int mux_encoder_finalize(struct mux_encoder *enc)
{
	if (!enc || !enc->ops)
		return MUX_ERROR_INVAL;

	/* encoder_finalize is optional - some codecs don't need it */
	if (!enc->ops->encoder_finalize)
		return MUX_OK;

	return enc->ops->encoder_finalize(enc);
}

/*
 * Decoding operations
 */
int mux_decoder_decode(struct mux_decoder *dec,
		       const void *input,
		       size_t input_size,
		       size_t *input_consumed)
{
	if (!dec || !dec->ops || !dec->ops->decoder_decode)
		return MUX_ERROR_INVAL;

	return dec->ops->decoder_decode(dec, input, input_size,
					input_consumed);
}

int mux_decoder_read(struct mux_decoder *dec,
		     void *output,
		     size_t output_size,
		     size_t *output_written,
		     int *stream_type)
{
	if (!dec || !dec->ops || !dec->ops->decoder_read)
		return MUX_ERROR_INVAL;

	return dec->ops->decoder_read(dec, output, output_size,
				      output_written, stream_type);
}

int mux_decoder_finalize(struct mux_decoder *dec)
{
	if (!dec || !dec->ops)
		return MUX_ERROR_INVAL;

	/* decoder_finalize is optional - some codecs don't need it */
	if (!dec->ops->decoder_finalize)
		return MUX_OK;

	return dec->ops->decoder_finalize(dec);
}
