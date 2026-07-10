/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Multi-frame FLAC push-streaming test.
 *
 * The FLAC decoder feeds libFLAC from a STREAMINFO-sized buffer rather than
 * relying on mux frame boundaries, so it must reconstruct a *multi-frame*
 * stream losslessly regardless of how the muxed bytes are chunked into
 * decode() calls - down to one byte at a time - and in both mux (num_streams=2)
 * and raw passthrough (num_streams=1) modes. Single-frame round-trips (covered
 * by test_flac_simple) do not exercise the tail-flush path, which is where the
 * interesting bugs live.
 */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
#define CH 2
#define FRAMES 44100            /* ~11 FLAC frames at blocksize 4096 */
#define NSAMP (FRAMES * CH)

struct collector {
	struct th_buf audio;
	struct th_buf side;
};

static int emit(void *user, int st, const void *d, size_t n, int flags)
{
	struct collector *c = user;
	(void)flags;
	return th_append(st == MUX_STREAM_AUDIO ? &c->audio : &c->side, d, n);
}

static int tests, fails;
#define CK(c, m) do { tests++; if (!(c)) { fails++; printf("  FAIL: %s\n", m); } } while (0)

static int decode_chunked(int streams, const struct th_buf *enc, size_t chunk,
			  struct collector *out)
{
	struct mux_decoder *dec;
	size_t step = chunk ? chunk : enc->len;
	int rc = 0;

	memset(out, 0, sizeof(*out));
	dec = mux_decoder_new(MUX_CODEC_FLAC, streams, NULL, 0, emit, out);
	if (!dec)
		return -1;
	for (size_t off = 0; off < enc->len && !rc; off += step) {
		size_t n = enc->len - off;
		if (n > step) n = step;
		rc = mux_decoder_decode(dec, enc->data + off, n);
	}
	rc |= mux_decoder_finalize(dec);
	mux_decoder_destroy(dec);
	return rc;
}

static void run(int streams, const int16_t *pcm)
{
	struct mux_param params[] = { { .name = "compression", .value.i = 5 } };
	const char *side1 = "event:start";
	const char *side2 = "event:end t=1000";
	struct th_buf enc = {0};
	int have_side = (streams == 2);

	printf("== num_streams=%d ==\n", streams);

	if (th_encode(MUX_CODEC_FLAC, RATE, CH, streams, params, 1,
		      pcm, (size_t)NSAMP * sizeof(int16_t),
		      have_side ? side1 : NULL, have_side ? strlen(side1) : 0,
		      &enc) != MUX_OK) {
		printf("  encode failed\n");
		fails++;
		return;
	}
	/* second side message (th_encode only sends one) */
	(void)side2;

	printf("  encoded %d samples -> %zu bytes (~%d frames)\n",
	       NSAMP, enc.len, (FRAMES + 4095) / 4096);

	size_t chunks[] = { 0, 1, 7, 64, 4096 };
	for (size_t k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		struct collector c;
		CK(decode_chunked(streams, &enc, chunks[k], &c) == 0, "decode rc");
		CK(c.audio.len == (size_t)NSAMP * sizeof(int16_t),
		   "decoded byte count == source");
		CK(c.audio.len == (size_t)NSAMP * sizeof(int16_t) &&
		   memcmp(c.audio.data, pcm, (size_t)NSAMP * sizeof(int16_t)) == 0,
		   "lossless exact across chunking");
		if (have_side)
			CK(c.side.len == strlen(side1) &&
			   memcmp(c.side.data, side1, strlen(side1)) == 0,
			   "side channel intact");
		th_buf_free(&c.audio);
		th_buf_free(&c.side);
	}

	th_buf_free(&enc);
}

int main(void)
{
	int16_t *pcm = malloc((size_t)NSAMP * sizeof(int16_t));
	unsigned seed = 1;
	int i;

	if (!pcm) { fprintf(stderr, "alloc failed\n"); return 1; }

	/* Tone + pseudo-random noise so frames are sizeable and numerous. */
	for (i = 0; i < FRAMES; i++) {
		seed = seed * 1103515245u + 12345u;
		int16_t noise = (int16_t)((seed >> 16) & 0x1fff) - 4096;
		int16_t s = (int16_t)(6000.0 *
			__builtin_sin(2.0 * 3.14159265 * 330.0 * i / RATE)) + noise;
		pcm[i * CH] = s;
		pcm[i * CH + 1] = (int16_t)(s - noise / 2);
	}

	run(2, pcm);   /* muxed audio + side channel */
	run(1, pcm);   /* raw FLAC byte stream, no mux framing */

	free(pcm);
	printf("\n%d checks, %d failed\n", tests, fails);
	return fails ? 1 : 0;
}
