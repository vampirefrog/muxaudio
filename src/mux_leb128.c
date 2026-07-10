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
 * Feed bytes into the streaming demux parser. Demultiplexes into two ordered
 * byte streams and delivers each stream's bytes to 'emit' as they are
 * recognised, holding only integer state between calls. It does not preserve
 * per-frame boundaries - a payload may be delivered in any number of chunks and
 * consecutive frames of the same stream simply concatenate.
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
			return emit(user, MUX_STREAM_AUDIO, in, n);
		return MUX_OK;
	}

	while (n > 0) {
		if (p->state < 64) {
			/* Accumulate one LEB128 header byte; state is its index. */
			uint8_t b = *in++;
			n--;

			if (p->state == 0)
				p->remaining = b & 0x7f;
			else
				p->remaining |= (uint64_t)(b & 0x7f) << (7 * p->state);

			if (b & 0x80) {
				if (p->state >= 9)
					return MUX_ERROR_FORMAT;  /* varint too long */
				p->state++;
			} else {
				/* Header complete: low bit is the stream, the rest is
				 * the payload length. Fold the stream into the state. */
				int stream = (int)(p->remaining & 1);
				p->remaining >>= 1;
				p->state = p->remaining ? (64 + stream) : 0;
			}
		} else {
			/* Deliver payload of the current frame (stream = state-64). */
			size_t take = (p->remaining < n) ? (size_t)p->remaining : n;

			ret = emit(user, p->state - 64, in, take);
			if (ret)
				return ret;

			in += take;
			n -= take;
			p->remaining -= take;
			if (p->remaining == 0)
				p->state = 0;
		}
	}

	return MUX_OK;
}
