/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * mux_testhelp.h - whole-buffer encode/decode helpers over the push/callback
 * API, for tests that just want "PCM in -> muxed blob -> PCM (+side) out".
 * Header-only; include after mux.h is available.
 */
#ifndef MUX_TESTHELP_H
#define MUX_TESTHELP_H

#include "mux.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Growable byte buffer. */
struct th_buf {
	uint8_t *data;
	size_t len, cap;
};

static inline int th_append(struct th_buf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		size_t c = b->cap ? b->cap : 256;
		while (c < b->len + n) c *= 2;
		b->data = realloc(b->data, c);
		if (!b->data) return -1;
		b->cap = c;
	}
	if (n) memcpy(b->data + b->len, d, n);
	b->len += n;
	return 0;
}

static inline void th_buf_free(struct th_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static inline int th_sink(void *user, const void *data, size_t size)
{
	return th_append((struct th_buf *)user, data, size);
}

/* Encode one audio buffer and (optionally) one side-channel message into a
 * muxed blob. Returns MUX_OK; fills 'out' (caller th_buf_free's it). */
static inline int th_encode(enum mux_codec_type codec, int rate, int channels,
			    int num_streams, const struct mux_param *params,
			    int num_params, const int16_t *pcm, size_t pcm_bytes,
			    const void *side, size_t side_len, struct th_buf *out)
{
	struct mux_encoder *e;
	int r;

	memset(out, 0, sizeof(*out));
	e = mux_encoder_new(codec, rate, channels, num_streams, params,
			    num_params, th_sink, out);
	if (!e)
		return MUX_ERROR;

	r = MUX_OK;
	if (pcm_bytes)
		r = mux_encoder_encode(e, pcm, pcm_bytes, MUX_STREAM_AUDIO);
	if (r == MUX_OK && side && side_len)
		r = mux_encoder_encode(e, side, side_len, MUX_STREAM_SIDE_CHANNEL);
	if (r == MUX_OK)
		r = mux_encoder_finalize(e);

	mux_encoder_destroy(e);
	if (r != MUX_OK)
		th_buf_free(out);
	return r;
}

/* Decoded output: audio bytes and (concatenated) side-channel bytes. */
struct th_out {
	struct th_buf audio;
	struct th_buf side;
};

static inline int th_emit(void *user, int stream_type, const void *data,
			  size_t size, int flags)
{
	struct th_out *o = user;
	(void)flags;
	return th_append(stream_type == MUX_STREAM_AUDIO ? &o->audio : &o->side,
			 data, size);
}

/* Decode a whole muxed blob, collecting audio and side output. */
static inline int th_decode(enum mux_codec_type codec, int num_streams,
			    const uint8_t *muxed, size_t muxed_len,
			    struct th_out *out)
{
	struct mux_decoder *d;
	int r;

	memset(out, 0, sizeof(*out));
	d = mux_decoder_new(codec, num_streams, NULL, 0, th_emit, out);
	if (!d)
		return MUX_ERROR;

	r = mux_decoder_decode(d, muxed, muxed_len);
	if (r == MUX_OK)
		r = mux_decoder_finalize(d);

	mux_decoder_destroy(d);
	return r;
}

static inline void th_out_free(struct th_out *o)
{
	th_buf_free(&o->audio);
	th_buf_free(&o->side);
}

/* Interleaved sine generator (all channels identical). */
static inline void th_sine(int16_t *buf, int nframes, int channels,
			   int rate, double freq, double amp)
{
	int i, ch;
	for (i = 0; i < nframes; i++) {
		double v = amp * 32767.0 *
			   __builtin_sin(2.0 * 3.14159265358979 * freq * i / rate);
		for (ch = 0; ch < channels; ch++)
			buf[i * channels + ch] = (int16_t)v;
	}
}

/* SNR in dB comparing the first n interleaved samples (>=100 => ~lossless). */
static inline double th_snr(const int16_t *orig, const int16_t *dec, size_t n)
{
	double sig = 0.0, err = 0.0;
	size_t i;
	for (i = 0; i < n; i++) {
		double d = (double)orig[i] - (double)dec[i];
		sig += (double)orig[i] * orig[i];
		err += d * d;
	}
	if (err == 0.0)
		return 100.0;
	if (sig == 0.0)
		return 0.0;
	return 10.0 * __builtin_log10(sig / err);
}

#endif /* MUX_TESTHELP_H */
