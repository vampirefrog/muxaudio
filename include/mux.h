/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MUX_H
#define MUX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stream types for multiplexed data
 */
#define MUX_STREAM_AUDIO 0
#define MUX_STREAM_SIDE_CHANNEL 1

/*
 * Codec types
 */
enum mux_codec_type {
	MUX_CODEC_PCM = 0,
	MUX_CODEC_OPUS,
	MUX_CODEC_VORBIS,
	MUX_CODEC_FLAC,
	MUX_CODEC_MP3,
	MUX_CODEC_AAC,
	MUX_CODEC_ALAW,
	MUX_CODEC_MULAW,
	MUX_CODEC_AMR,
	MUX_CODEC_AMR_WB,
	MUX_CODEC_MAX
};

/*
 * Parameter types for codec configuration
 */
enum mux_param_type {
	MUX_PARAM_TYPE_INT,
	MUX_PARAM_TYPE_FLOAT,
	MUX_PARAM_TYPE_BOOL,
	MUX_PARAM_TYPE_STRING
};

/*
 * Return codes
 */
#define MUX_OK 0
#define MUX_ERROR -1
#define MUX_ERROR_NOMEM -2
#define MUX_ERROR_INVAL -3
#define MUX_ERROR_NOCODEC -4
#define MUX_ERROR_EOF -5
#define MUX_ERROR_ENCODE -6 /* Encoding error */
#define MUX_ERROR_DECODE -7 /* Decoding error */
#define MUX_ERROR_FORMAT -8 /* Format/container error */
#define MUX_ERROR_INIT -9	/* Initialization error */

/*
 * Error information structure
 */
struct mux_error_info {
	int code;				  /* mux library error code */
	const char *message;	  /* human-readable message */
	int library_code;		  /* underlying library error code */
	const char *library_name; /* name of underlying library */
	const char *library_msg;  /* library-specific error message */
};

/*
 * Parameter descriptor - describes available codec parameters
 */
struct mux_param_desc {
	const char *name;
	const char *description;
	enum mux_param_type type;
	union {
		struct {
			int min;
			int max;
			int def;
		} i;
		struct {
			float min;
			float max;
			float def;
		} f;
		struct {
			int def;
		} b;
		struct {
			const char *def;
		} s;
	} range;
};

/*
 * Parameter value - used to configure codecs
 */
struct mux_param {
	const char *name;
	union {
		int i;
		float f;
		int b;
		const char *s;
	} value;
};

/*
 * Codec information
 */
struct mux_codec_info {
	enum mux_codec_type type;
	const char *name;
	const char *description;
};

/*
 * Opaque encoder/decoder structures
 * Internal implementation is hidden from users
 */
struct mux_encoder;
struct mux_decoder;

/*
 * Output sink: receives the muxed byte stream produced by the encoder.
 * Invoked synchronously from within mux_encoder_encode()/mux_encoder_finalize(),
 * possibly several times per call, with data in arbitrarily sized chunks.
 * Return 0 to continue; a non-zero return aborts and is propagated back out of
 * the encode call. The library never retains 'data' after the call returns.
 */
typedef int (*mux_sink_fn)(void *user, const void *data, size_t size);

/*
 * Emit callback: receives demultiplexed audio / side-channel data produced by
 * the decoder. Invoked synchronously from within mux_decoder_decode()/
 * mux_decoder_finalize(), possibly several times per call, with data delivered
 * in arbitrarily sized chunks. Both streams are plain ordered byte streams -
 * audio is interleaved int16 PCM, side channel is opaque bytes in the order
 * they were encoded (it frames itself, if it needs to). Return 0 to continue;
 * a non-zero return aborts and is propagated out of the decode call.
 */
typedef int (*mux_emit_fn)(void *user, int stream_type, const void *data, size_t size);

/*
 * Codec discovery
 */
int mux_list_codecs(const struct mux_codec_info **codecs, int *count);

/*
 * Codec name conversion
 */
int mux_codec_from_name(const char *name, enum mux_codec_type *codec);
const char *mux_codec_to_name(enum mux_codec_type codec);

/*
 * Parameter introspection
 */
int mux_get_encoder_params(
	enum mux_codec_type codec_type,
	const struct mux_param_desc **params,
	int *count
);

int mux_get_decoder_params(
	enum mux_codec_type codec_type,
	const struct mux_param_desc **params,
	int *count
);

/*
 * Sample rate constraints
 */
struct mux_sample_rate_list {
	const int *rates; /* Array of supported sample rates */
	int count;		  /* Number of rates in array */
	int is_range;	  /* If true, rates[0]=min, rates[1]=max */
};

/*
 * Query supported sample rates for a codec
 * Returns MUX_OK and fills in the sample_rate_list structure.
 * If is_range is true, any rate between rates[0] and rates[1] is supported.
 * If is_range is false, only the discrete rates in the array are supported.
 */
int mux_get_supported_sample_rates(
	enum mux_codec_type codec_type,
	struct mux_sample_rate_list *list
);

/*
 * Convenience: check whether a specific sample rate is accepted by a codec.
 * Returns MUX_OK if supported, MUX_ERROR_INVAL if not, or MUX_ERROR_NOCODEC
 * if the codec isn't compiled into this build.
 */
int mux_sample_rate_supported(enum mux_codec_type codec_type, int sample_rate);

/*
 * Encoder - static allocation
 * 'sink' receives the muxed output and is required (may not be NULL).
 */
int mux_encoder_init(
	struct mux_encoder *enc,
	enum mux_codec_type codec_type,
	int sample_rate,
	int num_channels,
	int num_streams,
	const struct mux_param *params,
	int num_params,
	mux_sink_fn sink,
	void *sink_user
);

void mux_encoder_deinit(struct mux_encoder *enc);

/*
 * Encoder - dynamic allocation
 */
struct mux_encoder *mux_encoder_new(
	enum mux_codec_type codec_type,
	int sample_rate,
	int num_channels,
	int num_streams,
	const struct mux_param *params,
	int num_params,
	mux_sink_fn sink,
	void *sink_user
);

void mux_encoder_destroy(struct mux_encoder *enc);

/*
 * Decoder - static allocation
 * 'emit' receives the demuxed output and is required (may not be NULL).
 */
int mux_decoder_init(
	struct mux_decoder *dec,
	enum mux_codec_type codec_type,
	int num_streams,
	const struct mux_param *params,
	int num_params,
	mux_emit_fn emit,
	void *emit_user
);

void mux_decoder_deinit(struct mux_decoder *dec);

/*
 * Decoder - dynamic allocation
 */
struct mux_decoder *mux_decoder_new(
	enum mux_codec_type codec_type,
	int num_streams,
	const struct mux_param *params,
	int num_params,
	mux_emit_fn emit,
	void *emit_user
);

void mux_decoder_destroy(struct mux_decoder *dec);

/*
 * Encoding: audio/side_channel -> muxed bytes.
 * Consumes the entire input, emitting muxed output through the sink callback
 * registered at construction. Returns MUX_OK, a MUX_ERROR_* code, or the
 * non-zero value returned by the sink.
 */
int mux_encoder_encode(
	struct mux_encoder *enc,
	const void *input,
	size_t input_size,
	int stream_type
);

/*
 * Finalize encoder: flush any codec-internal carry (e.g. a partial frame) to
 * the sink. Call once when done encoding.
 */
int mux_encoder_finalize(struct mux_encoder *enc);

/*
 * Decoding: muxed bytes -> audio/side_channel.
 * Consumes the entire input, delivering demuxed data through the emit callback
 * registered at construction. Input may be fed in any chunking, down to a
 * single byte per call. Returns MUX_OK, a MUX_ERROR_* code, or the non-zero
 * value returned by the emit callback.
 */
int mux_decoder_decode(struct mux_decoder *dec, const void *input, size_t input_size);

/*
 * Finalize decoder: flush any codec-internal carry. Call once when the input
 * stream ends.
 */
int mux_decoder_finalize(struct mux_decoder *dec);

/*
 * Error reporting
 */
const struct mux_error_info *mux_encoder_get_error(struct mux_encoder *enc);
const struct mux_error_info *mux_decoder_get_error(struct mux_decoder *dec);
void mux_encoder_clear_error(struct mux_encoder *enc);
void mux_decoder_clear_error(struct mux_decoder *dec);

/*
 * Get human-readable error message for error code
 */
const char *mux_error_string(int error_code);

#ifdef __cplusplus
}
#endif

#endif /* MUX_H */
