/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <stdlib.h>
#include <string.h>

int mux_buffer_init(struct mux_buffer *buf, size_t initial_capacity)
{
	if (!buf)
		return MUX_ERROR_INVAL;

	memset(buf, 0, sizeof(*buf));

	if (initial_capacity > 0) {
		buf->data = malloc(initial_capacity);
		if (!buf->data)
			return MUX_ERROR_NOMEM;
		buf->capacity = initial_capacity;
	}

	buf->size = 0;
	buf->read_pos = 0;

	return MUX_OK;
}

void mux_buffer_deinit(struct mux_buffer *buf)
{
	if (!buf)
		return;

	free(buf->data);
	memset(buf, 0, sizeof(*buf));
}

static int mux_buffer_ensure_capacity(struct mux_buffer *buf, size_t needed)
{
	size_t new_capacity;
	uint8_t *new_data;

	if (buf->capacity >= needed)
		return MUX_OK;

	/* Grow by 1.5x or to needed size, whichever is larger */
	new_capacity = buf->capacity + (buf->capacity >> 1);
	if (new_capacity < needed)
		new_capacity = needed;

	new_data = realloc(buf->data, new_capacity);
	if (!new_data)
		return MUX_ERROR_NOMEM;

	buf->data = new_data;
	buf->capacity = new_capacity;

	return MUX_OK;
}

int mux_buffer_write(struct mux_buffer *buf, const void *data, size_t size)
{
	int ret;

	if (!buf || !data)
		return MUX_ERROR_INVAL;

	if (size == 0)
		return MUX_OK;

	ret = mux_buffer_ensure_capacity(buf, buf->size + size);
	if (ret != MUX_OK)
		return ret;

	memcpy(buf->data + buf->size, data, size);
	buf->size += size;

	return MUX_OK;
}

int mux_buffer_read(struct mux_buffer *buf, void *data, size_t size,
		    size_t *bytes_read)
{
	size_t available;

	if (!buf || !bytes_read)
		return MUX_ERROR_INVAL;

	available = buf->size - buf->read_pos;
	if (available == 0) {
		*bytes_read = 0;
		return MUX_ERROR_AGAIN;
	}

	if (size > available)
		size = available;

	if (data)
		memcpy(data, buf->data + buf->read_pos, size);

	buf->read_pos += size;
	*bytes_read = size;

	/* Reset buffer if fully read */
	if (buf->read_pos == buf->size) {
		buf->read_pos = 0;
		buf->size = 0;
	}

	return MUX_OK;
}

int mux_buffer_available(const struct mux_buffer *buf)
{
	if (!buf)
		return 0;

	return buf->size - buf->read_pos;
}

void mux_buffer_clear(struct mux_buffer *buf)
{
	if (!buf)
		return;

	buf->size = 0;
	buf->read_pos = 0;
}
