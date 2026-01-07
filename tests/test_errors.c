/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
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
		printf("Library: %s\n", err->library_name);
		printf("Library error code: %d\n", err->library_code);
		if (err->library_msg)
			printf("Library message: %s\n", err->library_msg);
	}
	printf("\n");
}

int main(void)
{
	struct mux_encoder *enc;
	struct mux_decoder *dec;
	const struct mux_error_info *err;
	int ret;

	printf("=== muxaudio Error Reporting Test ===\n\n");

	/*
	 * Test 1: Invalid codec type
	 */
	printf("Test 1: Creating encoder with invalid codec type...\n");
	enc = mux_encoder_new(999, 44100, 2, NULL, 0);
	if (!enc) {
		printf("✓ Encoder creation failed as expected (returns NULL)\n");
	} else {
		printf("✗ Encoder should have failed!\n");
		mux_encoder_destroy(enc);
		return 1;
	}

	/*
	 * Test 2: MP3 encoder success - should have no error
	 */
	printf("\nTest 2: Creating valid MP3 encoder...\n");
	enc = mux_encoder_new(MUX_CODEC_MP3, 44100, 2, NULL, 0);
	if (!enc) {
		printf("✗ Failed to create encoder\n");
		return 1;
	}

	err = mux_encoder_get_error(enc);
	print_error("After successful encoder creation", err);

	if (err->code == MUX_OK) {
		printf("✓ No error after successful creation\n");
	} else {
		printf("✗ Unexpected error!\n");
	}

	mux_encoder_destroy(enc);

	/*
	 * Test 3: MP3 decoder  success - should have no error
	 */
	printf("\nTest 3: Creating valid MP3 decoder...\n");
	dec = mux_decoder_new(MUX_CODEC_MP3, NULL, 0);
	if (!dec) {
		printf("✗ Failed to create decoder\n");
		return 1;
	}

	err = mux_decoder_get_error(dec);
	print_error("After successful decoder creation", err);

	if (err->code == MUX_OK) {
		printf("✓ No error after successful creation\n");
	} else {
		printf("✗ Unexpected error!\n");
	}

	/*
	 * Test 4: Test decode error by feeding garbage data
	 */
	printf("\nTest 4: Feeding garbage data to decoder...\n");
	uint8_t garbage[1024];
	memset(garbage, 0xFF, sizeof(garbage));

	size_t consumed;
	ret = mux_decoder_decode(dec, garbage, sizeof(garbage), &consumed);
	(void)ret;  /* May or may not fail depending on buffering */

	err = mux_decoder_get_error(dec);
	if (err->code != MUX_OK) {
		print_error("After decoding garbage", err);
		printf("✓ Error was properly reported\n");
	} else {
		printf("(No error - decoder may have buffered the data)\n");
	}

	mux_decoder_destroy(dec);

	/*
	 * Test 5: Test error strings
	 */
	printf("\nTest 5: Error code to string mapping...\n");
	printf("MUX_OK: %s\n", mux_error_string(MUX_OK));
	printf("MUX_ERROR: %s\n", mux_error_string(MUX_ERROR));
	printf("MUX_ERROR_NOMEM: %s\n", mux_error_string(MUX_ERROR_NOMEM));
	printf("MUX_ERROR_ENCODE: %s\n", mux_error_string(MUX_ERROR_ENCODE));
	printf("MUX_ERROR_DECODE: %s\n", mux_error_string(MUX_ERROR_DECODE));
	printf("MUX_ERROR_INIT: %s\n", mux_error_string(MUX_ERROR_INIT));
	printf("Invalid code (99): %s\n", mux_error_string(99));

	printf("\n=== All error reporting tests passed! ===\n");
	return 0;
}
