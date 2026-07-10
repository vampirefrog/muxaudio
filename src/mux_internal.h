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
 * Codec operations vtable (pseudo-class virtual methods)
 *
 * The push model: encode() consumes all input and emits muxed bytes through
 * enc->sink; decode() consumes all input and emits demuxed data through
 * dec->emit. There is no library-owned output buffer and no read() step.
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
			      int stream_type);

	int (*encoder_finalize)(struct mux_encoder *enc);

	/* Decoder operations */
	int (*decoder_init)(struct mux_decoder *dec,
			    const struct mux_param *params,
			    int num_params);

	void (*decoder_deinit)(struct mux_decoder *dec);

	int (*decoder_decode)(struct mux_decoder *dec,
			      const void *input,
			      size_t input_size);

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

	/* Output sink for the muxed byte stream */
	mux_sink_fn sink;
	void *sink_user;

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

	/* Emit callback for demuxed audio + side channel */
	mux_emit_fn emit;
	void *emit_user;

	/* Error information */
	struct mux_error_info error;

	/* Codec-specific data */
	void *codec_data;
};

/*
 * Output helpers. Forward one chunk to the registered callback and propagate
 * its return value (0 = continue, non-zero = abort).
 */
int mux_encoder_emit(struct mux_encoder *enc, const void *data, size_t size);
int mux_decoder_emit(struct mux_decoder *dec, int stream_type,
		     const void *data, size_t size);

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
 * LEB128 varint encoding utility
 */
int mux_leb128_encode(uint64_t value, uint8_t *output, size_t output_size);

/*
 * Streaming LEB128 mux framing
 *
 * Frame format (num_streams == 2): [length_and_stream: LEB128][payload]
 *   where length_and_stream = (payload_size << 1) | (stream_type & 1).
 * Passthrough (num_streams == 1): raw bytes, no framing, all audio.
 *
 * Encode side: emit one frame straight to the sink (no buffering).
 * Decode side: a byte-driven state machine that holds only integer state -
 *   no payload buffer. It demultiplexes into two ordered byte streams and
 *   delivers each stream's bytes to 'emit' in whatever chunks the input
 *   arrives in; it does not preserve per-frame boundaries (neither stream
 *   needs them - side-channel data frames itself).
 */
int mux_leb128_emit_frame(const void *payload, size_t payload_size,
			  int stream_type, int num_streams,
			  mux_sink_fn sink, void *user);

/*
 * Emit only a frame's LEB128 header, declaring a payload of 'payload_size'
 * bytes. The caller then streams exactly that many payload bytes to the sink
 * itself - lets transforming codecs (e.g. G.711) convert through a fixed stack
 * buffer with no heap allocation. No-op in passthrough mode (num_streams == 1).
 */
int mux_leb128_emit_header(size_t payload_size, int stream_type,
			   int num_streams, mux_sink_fn sink, void *user);

struct mux_leb128_parser {
	uint64_t acc;              /* varint accumulator (header)      */
	int shift;                 /* current varint bit shift         */
	int in_payload;            /* 0 = reading header, 1 = payload   */
	int stream_type;           /* stream type of current frame     */
	uint64_t payload_remaining;/* payload bytes left in this frame */
};

void mux_leb128_parser_init(struct mux_leb128_parser *p);
int mux_leb128_parser_feed(struct mux_leb128_parser *p,
			   const void *input, size_t size,
			   int num_streams,
			   mux_emit_fn emit, void *user);

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
