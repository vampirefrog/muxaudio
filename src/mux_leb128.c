/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <string.h>

/*
 * Encode a 64-bit unsigned integer as unsigned LEB128.
 * Returns number of bytes written, or negative error code.
 */
int mux_leb128_encode(uint64_t value, uint8_t *output, size_t output_size)
{
	size_t count = 0;

	if (!output)
		return MUX_ERROR_INVAL;

	do {
		uint8_t byte = value & 0x7f;
		value >>= 7;

		if (value != 0)
			byte |= 0x80;  /* More bytes to come */

		if (count >= output_size)
			return MUX_ERROR_INVAL;

		output[count++] = byte;
	} while (value != 0);

	return count;
}

/*
 * Emit a single mux frame straight to the sink - no buffering.
 *
 * Passthrough mode (num_streams == 1): write the raw payload.
 * Mux mode (num_streams == 2): write [LEB128 length_and_stream][payload].
 */
int mux_leb128_emit_header(size_t payload_size, int stream_type,
			   int num_streams, mux_sink_fn sink, void *user)
{
	uint8_t header[10];  /* Max 10 bytes for a 64-bit LEB128 value */
	uint64_t length_with_stream;
	int header_len;

	if (!sink)
		return MUX_ERROR_INVAL;

	if (num_streams == 1)
		return MUX_OK;  /* passthrough: no framing */

	length_with_stream = ((uint64_t)payload_size << 1) | (stream_type & 1);

	header_len = mux_leb128_encode(length_with_stream, header, sizeof(header));
	if (header_len < 0)
		return header_len;

	return sink(user, header, header_len);
}

int mux_leb128_emit_frame(const void *payload, size_t payload_size,
			  int stream_type, int num_streams,
			  mux_sink_fn sink, void *user)
{
	int ret;

	if (!sink)
		return MUX_ERROR_INVAL;

	ret = mux_leb128_emit_header(payload_size, stream_type, num_streams,
				     sink, user);
	if (ret)
		return ret;

	if (payload && payload_size > 0)
		return sink(user, payload, payload_size);

	return MUX_OK;
}

void mux_leb128_parser_init(struct mux_leb128_parser *p)
{
	memset(p, 0, sizeof(*p));
}

/*
 * Feed bytes into the streaming demux parser. Delivers demuxed data to 'emit'
 * as it is recognised, holding only integer state between calls. A payload may
 * span any number of feed() calls and be delivered in any number of emit()
 * chunks; MUX_EMIT_FRAME_END flags the chunk that completes a frame.
 */
int mux_leb128_parser_feed(struct mux_leb128_parser *p,
			   const void *input, size_t size,
			   int num_streams,
			   mux_emit_fn emit, void *user)
{
	const uint8_t *in = input;
	size_t n = size;
	int ret;

	if (!p || !emit || (!input && size))
		return MUX_ERROR_INVAL;

	/* Passthrough mode - every byte is audio, no framing. */
	if (num_streams == 1) {
		if (n > 0)
			return emit(user, MUX_STREAM_AUDIO, in, n, 0);
		return MUX_OK;
	}

	while (n > 0) {
		if (!p->in_payload) {
			/* Accumulate one LEB128 header byte. */
			uint8_t b = *in++;
			n--;

			p->acc |= (uint64_t)(b & 0x7f) << p->shift;
			p->shift += 7;

			if (b & 0x80) {
				if (p->shift >= 64)
					return MUX_ERROR_FORMAT;  /* varint too long */
				continue;
			}

			/* Header complete. */
			p->stream_type = (int)(p->acc & 1);
			p->payload_remaining = p->acc >> 1;
			p->acc = 0;
			p->shift = 0;
			p->in_payload = 1;

			if (p->payload_remaining == 0) {
				/* Zero-length frame: one empty, terminal chunk. */
				ret = emit(user, p->stream_type, in, 0,
					   MUX_EMIT_FRAME_END);
				if (ret)
					return ret;
				p->in_payload = 0;
			}
		} else {
			size_t take = (p->payload_remaining < n)
				      ? (size_t)p->payload_remaining : n;
			int last = (take == p->payload_remaining);

			ret = emit(user, p->stream_type, in, take,
				   last ? MUX_EMIT_FRAME_END : 0);
			if (ret)
				return ret;

			in += take;
			n -= take;
			p->payload_remaining -= take;
			if (p->payload_remaining == 0)
				p->in_payload = 0;
		}
	}

	return MUX_OK;
}
