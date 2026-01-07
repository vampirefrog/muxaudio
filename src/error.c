/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mux.h"
#include "mux_internal.h"
#include <string.h>

/*
 * Error code to string mapping
 */
static const char *error_strings[] = {
	[0] = "Success",
	[-MUX_ERROR] = "Generic error",
	[-MUX_ERROR_NOMEM] = "Out of memory",
	[-MUX_ERROR_INVAL] = "Invalid argument",
	[-MUX_ERROR_AGAIN] = "Need more data",
	[-MUX_ERROR_NOCODEC] = "Codec not available",
	[-MUX_ERROR_EOF] = "End of file",
	[-MUX_ERROR_ENCODE] = "Encoding error",
	[-MUX_ERROR_DECODE] = "Decoding error",
	[-MUX_ERROR_FORMAT] = "Format/container error",
	[-MUX_ERROR_INIT] = "Initialization error"
};

const char *mux_error_string(int error_code)
{
	int index;

	if (error_code == 0)
		return error_strings[0];

	index = -error_code;
	if (index < 0 || index >= (int)(sizeof(error_strings) / sizeof(error_strings[0])))
		return "Unknown error";

	if (!error_strings[index])
		return "Unknown error";

	return error_strings[index];
}

void mux_encoder_set_error(struct mux_encoder *enc, int code,
			   const char *message,
			   const char *library_name,
			   int library_code,
			   const char *library_msg)
{
	if (!enc)
		return;

	enc->error.code = code;
	enc->error.message = message ? message : mux_error_string(code);
	enc->error.library_name = library_name;
	enc->error.library_code = library_code;
	enc->error.library_msg = library_msg;
}

void mux_decoder_set_error(struct mux_decoder *dec, int code,
			   const char *message,
			   const char *library_name,
			   int library_code,
			   const char *library_msg)
{
	if (!dec)
		return;

	dec->error.code = code;
	dec->error.message = message ? message : mux_error_string(code);
	dec->error.library_name = library_name;
	dec->error.library_code = library_code;
	dec->error.library_msg = library_msg;
}

const struct mux_error_info *mux_encoder_get_error(struct mux_encoder *enc)
{
	if (!enc)
		return NULL;

	return &enc->error;
}

const struct mux_error_info *mux_decoder_get_error(struct mux_decoder *dec)
{
	if (!dec)
		return NULL;

	return &dec->error;
}

void mux_encoder_clear_error(struct mux_encoder *enc)
{
	if (!enc)
		return;

	memset(&enc->error, 0, sizeof(enc->error));
}

void mux_decoder_clear_error(struct mux_decoder *dec)
{
	if (!dec)
		return;

	memset(&dec->error, 0, sizeof(dec->error));
}
