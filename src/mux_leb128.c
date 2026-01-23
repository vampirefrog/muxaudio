/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <string.h>

/*
 * Encode a 64-bit unsigned integer as unsigned LEB128
 * Returns number of bytes written, or negative error code
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
 * Decode unsigned LEB128 to a 64-bit unsigned integer
 * Returns MUX_OK on success, bytes_read contains number of bytes consumed
 */
int mux_leb128_decode(const uint8_t *input, size_t input_size,
		      uint64_t *value, size_t *bytes_read)
{
	uint64_t result = 0;
	int shift = 0;
	size_t count = 0;
	uint8_t byte;

	if (!input || !value || !bytes_read)
		return MUX_ERROR_INVAL;

	do {
		if (count >= input_size)
			return MUX_ERROR_AGAIN;  /* Need more data */

		byte = input[count++];
		result |= (uint64_t)(byte & 0x7f) << shift;
		shift += 7;

		if (shift >= 64)
			return MUX_ERROR_INVAL;  /* Overflow */

	} while (byte & 0x80);

	*value = result;
	*bytes_read = count;

	return MUX_OK;
}

/*
 * Write a frame with LEB128 header
 * Format: [length_with_stream_id: LEB128][payload]
 * LSB of length indicates stream: 0=audio, 1=side channel
 * Actual payload length = length_value >> 1
 *
 * When num_streams == 1 (passthrough mode), writes raw payload without LEB128 framing.
 * When num_streams == 2 (mux mode), writes LEB128 frame with stream type.
 */
int mux_leb128_write_frame(struct mux_buffer *output,
			   const void *payload, size_t payload_size,
			   int stream_type, int num_streams)
{
	uint8_t leb128_buf[10];  /* Max 10 bytes for 64-bit LEB128 */
	uint64_t length_with_stream;
	int leb128_len;
	int ret;

	/* Passthrough mode - write raw payload without LEB128 framing */
	if (num_streams == 1) {
		if (payload && payload_size > 0) {
			ret = mux_buffer_write(output, payload, payload_size);
			if (ret != MUX_OK)
				return ret;
		}
		return MUX_OK;
	}

	/* Mux mode - write LEB128 frame */
	/* Encode payload size in upper bits, stream type in LSB */
	length_with_stream = (payload_size << 1) | (stream_type & 1);

	leb128_len = mux_leb128_encode(length_with_stream, leb128_buf,
				       sizeof(leb128_buf));
	if (leb128_len < 0)
		return leb128_len;

	/* Write LEB128 header */
	ret = mux_buffer_write(output, leb128_buf, leb128_len);
	if (ret != MUX_OK)
		return ret;

	/* Write payload */
	if (payload && payload_size > 0) {
		ret = mux_buffer_write(output, payload, payload_size);
		if (ret != MUX_OK)
			return ret;
	}

	return MUX_OK;
}

/*
 * Read a frame with LEB128 header
 * Returns MUX_OK if frame read, MUX_ERROR_AGAIN if need more data
 *
 * When num_streams == 1 (passthrough mode), reads all available raw data as audio stream.
 * When num_streams == 2 (mux mode), reads LEB128 frame with stream type.
 */
int mux_leb128_read_frame(struct mux_buffer *input,
			  void *payload, size_t payload_capacity,
			  size_t *payload_size, int *stream_type, int num_streams)
{
	uint64_t length_with_stream;
	size_t leb128_bytes;
	size_t actual_payload_size;
	uint8_t *data_ptr;
	int ret;
	int stream;
	size_t available;

	if (!input || !payload_size || !stream_type)
		return MUX_ERROR_INVAL;

	/* Passthrough mode - read all available data as audio stream */
	if (num_streams == 1) {
		available = input->size - input->read_pos;
		if (available == 0)
			return MUX_ERROR_AGAIN;

		actual_payload_size = available < payload_capacity ? available : payload_capacity;

		if (payload && actual_payload_size > 0) {
			memcpy(payload, input->data + input->read_pos, actual_payload_size);
		}

		input->read_pos += actual_payload_size;
		*payload_size = actual_payload_size;
		*stream_type = MUX_STREAM_AUDIO;

		/* Reset buffer if fully read */
		if (input->read_pos == input->size) {
			input->read_pos = 0;
			input->size = 0;
		}

		return MUX_OK;
	}

	/* Mux mode - read LEB128 frame */
	/* Try to decode LEB128 header */
	data_ptr = input->data + input->read_pos;
	ret = mux_leb128_decode(data_ptr,
				input->size - input->read_pos,
				&length_with_stream, &leb128_bytes);
	if (ret != MUX_OK)
		return ret;

	/* Extract stream type and payload size */
	stream = length_with_stream & 1;
	actual_payload_size = length_with_stream >> 1;

	/* Check if we have the full frame */
	if (input->size - input->read_pos < leb128_bytes + actual_payload_size)
		return MUX_ERROR_AGAIN;

	/* Set payload size even if buffer is too small (caller can reallocate) */
	*payload_size = actual_payload_size;
	*stream_type = stream;

	/* Check output buffer size */
	if (payload && payload_capacity < actual_payload_size)
		return MUX_ERROR_INVAL;

	/* Advance past LEB128 header */
	input->read_pos += leb128_bytes;

	/* Read payload */
	if (payload && actual_payload_size > 0) {
		memcpy(payload, input->data + input->read_pos,
		       actual_payload_size);
	}
	input->read_pos += actual_payload_size;

	*payload_size = actual_payload_size;
	*stream_type = stream;

	/* Reset buffer if fully read */
	if (input->read_pos == input->size) {
		input->read_pos = 0;
		input->size = 0;
	}

	return MUX_OK;
}
