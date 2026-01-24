/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MUX_INTERNAL_H
#define MUX_INTERNAL_H

#include "mux.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Forward declarations
 */
struct mux_encoder;
struct mux_decoder;
struct mux_codec_ops;

/*
 * Internal buffer for holding encoded/decoded data
 */
struct mux_buffer {
	uint8_t *data;
	size_t size;
	size_t capacity;
	size_t read_pos;
};

/*
 * Codec operations vtable (pseudo-class virtual methods)
 */
struct mux_codec_ops {
	/* Encoder operations */
	int (*encoder_init)(struct mux_encoder *enc,
			    int sample_rate,
			    int num_channels,
			    const struct mux_param *params,
			    int num_params);

	void (*encoder_deinit)(struct mux_encoder *enc);

	int (*encoder_encode)(struct mux_encoder *enc,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed,
			      int stream_type);

	int (*encoder_read)(struct mux_encoder *enc,
			    void *output,
			    size_t output_size,
			    size_t *output_written);

	int (*encoder_finalize)(struct mux_encoder *enc);

	/* Decoder operations */
	int (*decoder_init)(struct mux_decoder *dec,
			    const struct mux_param *params,
			    int num_params);

	void (*decoder_deinit)(struct mux_decoder *dec);

	int (*decoder_decode)(struct mux_decoder *dec,
			      const void *input,
			      size_t input_size,
			      size_t *input_consumed);

	int (*decoder_read)(struct mux_decoder *dec,
			    void *output,
			    size_t output_size,
			    size_t *output_written,
			    int *stream_type);

	int (*decoder_finalize)(struct mux_decoder *dec);

	/* Parameter info */
	const struct mux_param_desc *encoder_params;
	int encoder_param_count;
	const struct mux_param_desc *decoder_params;
	int decoder_param_count;

	/* Sample rate constraints */
	const int *supported_sample_rates;
	int sample_rate_count;
	int sample_rate_is_range;  /* If true, first two values are min/max */
};

/*
 * Encoder base structure
 */
struct mux_encoder {
	enum mux_codec_type codec_type;
	const struct mux_codec_ops *ops;
	int sample_rate;
	int num_channels;
	int num_streams;  /* 1 = passthrough, 2 = muxed audio + side channel */

	/* Output buffer for multiplexed data */
	struct mux_buffer output;

	/* Error information */
	struct mux_error_info error;

	/* Codec-specific data */
	void *codec_data;
};

/*
 * Decoder base structure
 */
struct mux_decoder {
	enum mux_codec_type codec_type;
	const struct mux_codec_ops *ops;
	int num_streams;  /* 1 = passthrough, 2 = muxed audio + side channel */

	/* Output buffers for demultiplexed audio and side channel */
	struct mux_buffer audio_output;
	struct mux_buffer side_output;

	/* Error information */
	struct mux_error_info error;

	/* Codec-specific data */
	void *codec_data;
};

/*
 * Buffer management utilities
 */
int mux_buffer_init(struct mux_buffer *buf, size_t initial_capacity);
void mux_buffer_deinit(struct mux_buffer *buf);
int mux_buffer_write(struct mux_buffer *buf, const void *data, size_t size);
int mux_buffer_read(struct mux_buffer *buf, void *data, size_t size,
		    size_t *bytes_read);
int mux_buffer_available(const struct mux_buffer *buf);
void mux_buffer_clear(struct mux_buffer *buf);

/*
 * Codec registry
 */
const struct mux_codec_ops *mux_get_codec_ops(enum mux_codec_type type);

/*
 * Error handling helpers
 */
void mux_encoder_set_error(struct mux_encoder *enc, int code,
			   const char *message,
			   const char *library_name,
			   int library_code,
			   const char *library_msg);

void mux_decoder_set_error(struct mux_decoder *dec, int code,
			   const char *message,
			   const char *library_name,
			   int library_code,
			   const char *library_msg);

/*
 * LEB128 encoding/decoding utilities
 */
int mux_leb128_encode(uint64_t value, uint8_t *output, size_t output_size);
int mux_leb128_decode(const uint8_t *input, size_t input_size,
		      uint64_t *value, size_t *bytes_read);

/*
 * LEB128 frame format helpers
 */
int mux_leb128_write_frame(struct mux_buffer *output,
			   const void *payload, size_t payload_size,
			   int stream_type, int num_streams);
int mux_leb128_read_frame(struct mux_buffer *input,
			  void *payload, size_t payload_capacity,
			  size_t *payload_size, int *stream_type, int num_streams);

/*
 * Codec-specific operations (implemented by each codec)
 */
extern const struct mux_codec_ops mux_codec_pcm_ops;
extern const struct mux_codec_ops mux_codec_opus_ops;
extern const struct mux_codec_ops mux_codec_vorbis_ops;
extern const struct mux_codec_ops mux_codec_flac_ops;
extern const struct mux_codec_ops mux_codec_mp3_ops;
extern const struct mux_codec_ops mux_codec_aac_ops;
extern const struct mux_codec_ops mux_codec_alaw_ops;
extern const struct mux_codec_ops mux_codec_mulaw_ops;
extern const struct mux_codec_ops mux_codec_amr_ops;
extern const struct mux_codec_ops mux_codec_amr_wb_ops;

#endif /* MUX_INTERNAL_H */
