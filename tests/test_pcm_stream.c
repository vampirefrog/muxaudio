/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * test_pcm_stream - exercises the push/streaming (callback) API on the
 * buffer-free codecs (PCM, A-law, mu-law): output via sink, demux via emit,
 * no library-owned buffering.
 *
 * Core property tested: decode is chunking-invariant. Feeding the muxed stream
 * one byte at a time (or in odd-sized chunks) must reconstruct byte-identical
 * audio and reassemble the same discrete side-channel messages as a single
 * decode() call. For PCM (lossless) the audio also equals the source exactly.
 */
#include "mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- tiny growable byte buffer ------------------------------------------ */
struct buf {
	uint8_t *data;
	size_t len, cap;
};

static void buf_append(struct buf *b, const void *data, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 64;
		while (cap < b->len + n) cap *= 2;
		b->data = realloc(b->data, cap);
		b->cap = cap;
	}
	if (n) memcpy(b->data + b->len, data, n);
	b->len += n;
}

static int buf_eq(const struct buf *a, const struct buf *b)
{
	return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

static void buf_free(struct buf *b) { free(b->data); memset(b, 0, sizeof(*b)); }

/* ---- encoder sink -------------------------------------------------------- */
static int sink_cb(void *user, const void *data, size_t size)
{
	buf_append((struct buf *)user, data, size);
	return 0;
}

/* ---- decoder emit collector --------------------------------------------- */
struct collector {
	struct buf audio;   /* concatenated audio bytes */
	struct buf side;    /* concatenated side-channel bytes (self-framing) */
};

static int emit_cb(void *user, int stream_type, const void *data, size_t size)
{
	struct collector *c = user;
	buf_append(stream_type == MUX_STREAM_AUDIO ? &c->audio : &c->side,
		   data, size);
	return 0;
}

static void collector_free(struct collector *c)
{
	buf_free(&c->audio);
	buf_free(&c->side);
}

/* ---- test fixtures ------------------------------------------------------- */
static int16_t audio1[512];
static int16_t audio2[300];
static const char *side1 = "event:phoneme=AA start=0";
static const char *side2 = "viseme:12 t=340ms";

static int tests_run, tests_failed;
#define CHECK(cond, msg) do { \
	tests_run++; \
	if (!(cond)) { tests_failed++; printf("  FAIL: %s\n", msg); } \
} while (0)

/* Encode the fixture stream with the given codec into 'out'. */
static int encode_fixture(enum mux_codec_type codec, int rate, struct buf *out)
{
	struct mux_encoder *enc =
		mux_encoder_new(codec, rate, 1, 2, NULL, 0, sink_cb, out);
	if (!enc) { printf("  encoder_new failed\n"); return -1; }

	int r = 0;
	r |= mux_encoder_encode(enc, audio1, sizeof(audio1), MUX_STREAM_AUDIO);
	r |= mux_encoder_encode(enc, side1, strlen(side1), MUX_STREAM_SIDE_CHANNEL);
	r |= mux_encoder_encode(enc, audio2, sizeof(audio2), MUX_STREAM_AUDIO);
	r |= mux_encoder_encode(enc, side2, strlen(side2), MUX_STREAM_SIDE_CHANNEL);
	r |= mux_encoder_finalize(enc);
	mux_encoder_destroy(enc);
	return r;
}

/* Decode 'enc' with the given codec, feeding 'chunk' bytes at a time
 * (0 = all at once), collecting demuxed output into 'c'. */
static int decode_chunked(enum mux_codec_type codec, struct buf *enc,
			  size_t chunk, struct collector *c)
{
	memset(c, 0, sizeof(*c));

	struct mux_decoder *dec =
		mux_decoder_new(codec, 2, NULL, 0, emit_cb, c);
	if (!dec) { printf("  decoder_new failed\n"); return -1; }

	size_t step = chunk ? chunk : enc->len;
	int rc = 0;
	for (size_t off = 0; off < enc->len && !rc; off += step) {
		size_t n = enc->len - off;
		if (n > step) n = step;
		rc = mux_decoder_decode(dec, enc->data + off, n);
	}
	rc |= mux_decoder_finalize(dec);
	mux_decoder_destroy(dec);
	return rc;
}

/* Compare two collectors for equality (audio bytes + side bytes). */
static void compare(struct collector *ref, struct collector *got,
		    const char *label)
{
	CHECK(buf_eq(&ref->audio, &got->audio), "audio matches reference");
	CHECK(buf_eq(&ref->side, &got->side), "side channel matches reference");
	(void)label;
}

static void run_codec(enum mux_codec_type codec, const char *name, int lossless,
		      int rate)
{
	printf("== %s ==\n", name);

	struct buf enc = {0};
	if (encode_fixture(codec, rate, &enc) != 0) {
		printf("  encode failed\n");
		tests_failed++;
		return;
	}
	printf("  encoded %zu bytes\n", enc.len);

	/* Reference decode: whole blob at once. */
	struct collector ref;
	CHECK(decode_chunked(codec, &enc, 0, &ref) == 0, "reference decode rc");

	/* For lossless PCM the audio must equal the source exactly. */
	if (lossless) {
		struct buf src = {0};
		buf_append(&src, audio1, sizeof(audio1));
		buf_append(&src, audio2, sizeof(audio2));
		CHECK(buf_eq(&src, &ref.audio), "lossless audio equals source");
		buf_free(&src);
	}
	/* Side channel is a plain ordered byte stream: the two messages come
	 * back concatenated in encode order. */
	{
		struct buf exp = {0};
		buf_append(&exp, side1, strlen(side1));
		buf_append(&exp, side2, strlen(side2));
		CHECK(buf_eq(&exp, &ref.side), "side channel == concatenated messages");
		buf_free(&exp);
	}

	/* Every chunking must reproduce the reference exactly. */
	size_t chunks[] = { 1, 3, 7, 250 };
	for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		struct collector got;
		CHECK(decode_chunked(codec, &enc, chunks[k], &got) == 0,
		      "chunked decode rc");
		compare(&ref, &got, name);
		collector_free(&got);
	}

	collector_free(&ref);
	buf_free(&enc);
}

int main(void)
{
	for (int i = 0; i < 512; i++) audio1[i] = (int16_t)(i * 37 - 9000);
	for (int i = 0; i < 300; i++) audio2[i] = (int16_t)(1000 - i * 11);

	run_codec(MUX_CODEC_PCM,   "pcm",   1, 16000);
	run_codec(MUX_CODEC_ALAW,  "alaw",  0, 16000);
	run_codec(MUX_CODEC_MULAW, "mulaw", 0, 16000);
#ifdef HAVE_FLAC
	run_codec(MUX_CODEC_FLAC,  "flac",  1, 16000);  /* lossless: decoded == source */
#endif
#ifdef HAVE_OPUS
	run_codec(MUX_CODEC_OPUS,  "opus",  0, 16000);  /* lossy: chunk-invariance only */
#endif
#ifdef HAVE_VORBIS
	run_codec(MUX_CODEC_VORBIS, "vorbis", 0, 16000);
#endif
#ifdef HAVE_MP3
	run_codec(MUX_CODEC_MP3,   "mp3",   0, 16000);
#endif
#ifdef HAVE_AAC
	run_codec(MUX_CODEC_AAC,   "aac",   0, 16000);
#endif
#ifdef HAVE_AMR
	run_codec(MUX_CODEC_AMR,   "amr",   0, 8000);  /* AMR-NB: 8 kHz mono */
#endif

	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
