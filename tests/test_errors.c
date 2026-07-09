/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_testhelp.h"
#include <stdio.h>
#include <string.h>

static void print_error(const char *context, const struct mux_error_info *err)
{
	if (!err || err->code == MUX_OK) {
		printf("%s: No error\n", context);
		return;
	}
	printf("\n=== %s ===\n", context);
	printf("Error code: %d (%s)\n", err->code, mux_error_string(err->code));
	printf("Message: %s\n", err->message);
	if (err->library_name) {
		printf("Library: %s (code %d)\n", err->library_name, err->library_code);
		if (err->library_msg)
			printf("Library message: %s\n", err->library_msg);
	}
	printf("\n");
}

/* Emit callback that always aborts, to exercise error propagation. */
static int reject_emit(void *user, int st, const void *d, size_t n, int flags)
{
	(void)user; (void)st; (void)d; (void)n; (void)flags;
	return 1;
}

int main(void)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	const struct mux_error_info *err;
	struct th_buf sink = {0};

	printf("=== muxaudio Error Reporting Test ===\n\n");

	/* Test 1: invalid codec type -> NULL */
	printf("Test 1: encoder with invalid codec type...\n");
	enc = mux_encoder_new(999, 44100, 2, 2, NULL, 0, th_sink, &sink);
	if (enc) {
		printf("\xe2\x9c\x97 should have failed\n");
		mux_encoder_destroy(enc);
		return 1;
	}
	printf("\xe2\x9c\x93 failed as expected (NULL)\n");

	/* Test 2: NULL sink rejected */
	printf("\nTest 2: encoder with NULL sink...\n");
	enc = mux_encoder_new(MUX_CODEC_PCM, 44100, 2, 2, NULL, 0, NULL, NULL);
	if (enc) {
		printf("\xe2\x9c\x97 should have failed\n");
		mux_encoder_destroy(enc);
		return 1;
	}
	printf("\xe2\x9c\x93 NULL sink rejected\n");

	/* Test 3: valid PCM encoder - no error */
	printf("\nTest 3: valid PCM encoder...\n");
	enc = mux_encoder_new(MUX_CODEC_PCM, 44100, 2, 2, NULL, 0, th_sink, &sink);
	if (!enc) { printf("\xe2\x9c\x97 create failed\n"); return 1; }
	err = mux_encoder_get_error(enc);
	print_error("After successful encoder creation", err);
	if (err->code != MUX_OK) { printf("\xe2\x9c\x97 unexpected error\n"); return 1; }
	printf("\xe2\x9c\x93 no error after creation\n");
	mux_encoder_destroy(enc);

	/* Test 4: emit callback abort propagates out of decode */
	printf("\nTest 4: emit callback abort propagates...\n");
	dec = mux_decoder_new(MUX_CODEC_PCM, 2, NULL, 0, reject_emit, NULL);
	if (!dec) { printf("\xe2\x9c\x97 create failed\n"); return 1; }
	{
		/* one framed side-channel byte so the parser calls emit */
		uint8_t muxed[] = { (1 << 1) | 1, 0x42 };
		int r = mux_decoder_decode(dec, muxed, sizeof(muxed));
		if (r != 0)
			printf("\xe2\x9c\x93 abort propagated (rc=%d)\n", r);
		else {
			printf("\xe2\x9c\x97 abort not propagated\n");
			mux_decoder_destroy(dec);
			return 1;
		}
	}
	mux_decoder_destroy(dec);

	/* Test 5: error strings */
	printf("\nTest 5: error code to string mapping...\n");
	printf("MUX_OK: %s\n", mux_error_string(MUX_OK));
	printf("MUX_ERROR: %s\n", mux_error_string(MUX_ERROR));
	printf("MUX_ERROR_NOMEM: %s\n", mux_error_string(MUX_ERROR_NOMEM));
	printf("MUX_ERROR_ENCODE: %s\n", mux_error_string(MUX_ERROR_ENCODE));
	printf("MUX_ERROR_DECODE: %s\n", mux_error_string(MUX_ERROR_DECODE));
	printf("MUX_ERROR_INIT: %s\n", mux_error_string(MUX_ERROR_INIT));
	printf("Invalid code (99): %s\n", mux_error_string(99));

	th_buf_free(&sink);
	printf("\n=== All error reporting tests passed! ===\n");
	return 0;
}
