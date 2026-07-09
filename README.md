[![Native Build](https://github.com/vampirefrog/muxaudio/actions/workflows/build-native.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/build-native.yml)
[![Windows Build](https://github.com/vampirefrog/muxaudio/actions/workflows/build-windows.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/build-windows.yml)
[![WASM Build](https://github.com/vampirefrog/muxaudio/actions/workflows/build-wasm.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/build-wasm.yml)
[![Test Matrix](https://github.com/vampirefrog/muxaudio/actions/workflows/test-matrix.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/test-matrix.yml)
[![Release](https://github.com/vampirefrog/muxaudio/actions/workflows/release.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/release.yml)

# muxaudio

A lightweight, efficient audio codec multiplexing library with support for multiple formats and side channel data.

## Features

- **6 Audio Codecs**: PCM, MP3, Vorbis, Opus, FLAC, AAC
- **Lossless & Lossy**: Choose between quality and compression
- **Side Channel Multiplexing**: Embed metadata alongside audio
- **Simple API**: Clean C99 interface
- **Command-Line Tools**: Encode/decode from stdin/stdout
- **Zero Dependencies**: Only codec libraries needed
- **Frame-Level Streaming**: Low latency, real-time capable

## Supported Codecs

| Codec  | Type     | Use Case               | Dependencies        | Status |
|--------|----------|------------------------|---------------------|--------|
| PCM    | Lossless | Uncompressed           | None                | ✅     |
| FLAC   | Lossless | Archive, mastering     | libFLAC, libogg     | ✅     |
| Opus   | Lossy    | VoIP, low latency      | libopus, libogg     | ✅     |
| Vorbis | Lossy    | Music, streaming       | libvorbis, libogg   | ✅     |
| MP3    | Lossy    | Wide compatibility     | libmp3lame, libmpg123 | ✅   |
| AAC    | Lossy    | High quality, mobile   | libfdk-aac          | ✅     |

## Quick Start

### Installation

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install libogg-dev libvorbis-dev libopus-dev \
                     libflac-dev libmp3lame-dev libmpg123-dev \
                     libfdk-aac-dev

# Build
mkdir build && cd build
cmake ..
make
sudo make install
```

### Basic Usage

muxaudio uses a **push model with callbacks**: you feed input in, and encoded/
decoded data is delivered synchronously to a callback you register. There is no
internal output buffer and no separate "read" step.

```c
#include <mux.h>

// --- Encode audio with FLAC ---
// Sink receives the muxed byte stream (write it to a file, socket, buffer...).
int on_muxed(void *user, const void *data, size_t size) {
    fwrite(data, 1, size, (FILE *)user);
    return 0;  // non-zero aborts
}

struct mux_encoder *enc =
    mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, /*num_streams=*/2, NULL, 0,
                    on_muxed, out_file);
mux_encoder_encode(enc, pcm_data, pcm_size, MUX_STREAM_AUDIO);  // calls on_muxed
mux_encoder_finalize(enc);                                      // flushes tail
mux_encoder_destroy(enc);

// --- Decode ---
// Emit receives demuxed audio and side-channel data, in arbitrary chunks.
int on_output(void *user, int stream_type, const void *data, size_t size,
              int flags) {
    if (stream_type == MUX_STREAM_AUDIO)
        fwrite(data, 1, size, pcm_out);         // decoded PCM (int16)
    else if (flags & MUX_EMIT_FRAME_END)
        handle_event(data, size);               // a complete side-channel msg
    return 0;
}

struct mux_decoder *dec =
    mux_decoder_new(MUX_CODEC_FLAC, /*num_streams=*/2, NULL, 0, on_output, NULL);
mux_decoder_decode(dec, input, input_size);     // may be called repeatedly
mux_decoder_finalize(dec);
mux_decoder_destroy(dec);
```

Input can be fed in any chunking — down to one byte per `decode()` call — and
produces identical output. `MUX_EMIT_FRAME_END` marks the chunk that completes a
logical side-channel message, so discrete events can be reassembled.

---

## API Reference

### Types

#### `enum mux_codec_type`
Supported codec types.

```c
enum mux_codec_type {
    MUX_CODEC_PCM,     // Uncompressed PCM
    MUX_CODEC_OPUS,    // Opus codec
    MUX_CODEC_VORBIS,  // Vorbis codec
    MUX_CODEC_FLAC,    // FLAC lossless
    MUX_CODEC_MP3,     // MP3 (MPEG-1 Layer III)
    MUX_CODEC_AAC,     // AAC (Advanced Audio Coding)
    MUX_CODEC_MAX
};
```

#### `struct mux_param`
Codec configuration parameter.

```c
struct mux_param {
    const char *name;          // Parameter name (e.g., "bitrate")
    union {
        int i;                 // Integer value
        float f;               // Float value
    } value;
};
```

#### `struct mux_error_info`
Detailed error information.

```c
struct mux_error_info {
    int code;                  // Error code (MUX_OK, MUX_ERROR_*, etc.)
    const char *message;       // Human-readable message
    const char *source;        // Source component (e.g., "libFLAC")
    int source_code;           // Source-specific error code
    const char *source_msg;    // Source error message
};
```

### Constants

```c
#define MUX_STREAM_AUDIO        0  // Audio stream type
#define MUX_STREAM_SIDE_CHANNEL 1  // Side channel (metadata) stream type
#define MUX_EMIT_FRAME_END      1  // emit flag: this chunk completes a frame
```

### Callback Types

```c
// Encoder output: receives the muxed byte stream. Return 0 to continue,
// non-zero to abort (propagated back out of the encode call).
typedef int (*mux_sink_fn)(void *user, const void *data, size_t size);

// Decoder output: receives demuxed audio / side-channel data in arbitrary
// chunks. flags carries MUX_EMIT_FRAME_END on the chunk completing a frame.
typedef int (*mux_emit_fn)(void *user, int stream_type,
                           const void *data, size_t size, int flags);
```

### Error Codes

```c
#define MUX_OK            0   // Success
#define MUX_ERROR_NOMEM   1   // Out of memory
#define MUX_ERROR_INVAL   2   // Invalid argument
#define MUX_ERROR_INIT    3   // Initialization failed
#define MUX_ERROR_ENCODE  4   // Encoding error
#define MUX_ERROR_DECODE  5   // Decoding error
#define MUX_ERROR_FORMAT  6   // Format error
#define MUX_ERROR_NOCODEC 7   // Codec not available
```

---

## Core API Functions

### Codec Discovery

#### `mux_list_codecs`
List all available codecs.

```c
int mux_list_codecs(const struct mux_codec_info **codecs, int *count);
```

**Returns**: `MUX_OK` on success
**Parameters**:
- `codecs`: Pointer to receive codec info array
- `count`: Pointer to receive number of codecs

**Example**:
```c
const struct mux_codec_info *codecs;
int count;
mux_list_codecs(&codecs, &count);
for (int i = 0; i < count; i++) {
    printf("%s: %s\n", codecs[i].name, codecs[i].description);
}
```

#### `mux_codec_from_name`
Convert codec name to type.

```c
int mux_codec_from_name(const char *name, enum mux_codec_type *codec);
```

**Returns**: `MUX_OK` if found, `MUX_ERROR_INVAL` if not found
**Parameters**:
- `name`: Codec name ("pcm", "mp3", "vorbis", "opus", "flac", "aac")
- `codec`: Pointer to receive codec type

**Example**:
```c
enum mux_codec_type codec;
if (mux_codec_from_name("flac", &codec) == MUX_OK) {
    // Use codec
}
```

#### `mux_codec_to_name`
Convert codec type to name.

```c
const char *mux_codec_to_name(enum mux_codec_type codec);
```

**Returns**: Codec name string or `NULL` if invalid
**Example**:
```c
const char *name = mux_codec_to_name(MUX_CODEC_FLAC);  // Returns "flac"
```

### Parameter Introspection

#### `mux_get_encoder_params`
Get available encoder parameters for a codec.

```c
int mux_get_encoder_params(enum mux_codec_type codec_type,
                           const struct mux_param_desc **params,
                           int *count);
```

**Returns**: `MUX_OK` on success
**Example**:
```c
const struct mux_param_desc *params;
int count;
mux_get_encoder_params(MUX_CODEC_OPUS, &params, &count);
for (int i = 0; i < count; i++) {
    printf("%s: %s (default: %d)\n",
           params[i].name, params[i].description, params[i].range.i.def);
}
```

#### `mux_get_decoder_params`
Get available decoder parameters for a codec.

```c
int mux_get_decoder_params(enum mux_codec_type codec_type,
                           const struct mux_param_desc **params,
                           int *count);
```

#### `mux_get_supported_sample_rates`
Query supported sample rates for a codec.

```c
int mux_get_supported_sample_rates(enum mux_codec_type codec_type,
                                   struct mux_sample_rate_list *list);
```

**Returns**: `MUX_OK` on success
**Example**:
```c
struct mux_sample_rate_list list;
mux_get_supported_sample_rates(MUX_CODEC_OPUS, &list);
if (list.is_range) {
    printf("Supports %d-%d Hz\n", list.rates[0], list.rates[1]);
} else {
    for (int i = 0; i < list.count; i++) {
        printf("Supports %d Hz\n", list.rates[i]);
    }
}
```

---

## Encoder API

### Dynamic Allocation

#### `mux_encoder_new`
Create a new encoder.

```c
struct mux_encoder *mux_encoder_new(enum mux_codec_type codec_type,
                                    int sample_rate,
                                    int num_channels,
                                    int num_streams,
                                    const struct mux_param *params,
                                    int num_params,
                                    mux_sink_fn sink,
                                    void *sink_user);
```

**Returns**: Encoder instance or `NULL` on error
**Parameters**:
- `codec_type`: Codec to use
- `sample_rate`: Sample rate in Hz (e.g., 44100)
- `num_channels`: Number of channels (1=mono, 2=stereo)
- `num_streams`: `1` = passthrough (audio only), `2` = mux audio + side channel
- `params`: Optional parameters array (can be `NULL`)
- `num_params`: Number of parameters (0 if `params` is `NULL`)
- `sink`: Callback receiving muxed output (required, non-NULL)
- `sink_user`: Opaque pointer passed to `sink`

**Example**:
```c
// FLAC with compression level 8, writing muxed output to a file
struct mux_param params[] = {
    { .name = "compression", .value.i = 8 }
};
struct mux_encoder *enc =
    mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, 2, params, 1, on_muxed, out_file);
```

#### `mux_encoder_destroy`
Destroy an encoder.

```c
void mux_encoder_destroy(struct mux_encoder *enc);
```

### Static Allocation

#### `mux_encoder_init`
Initialize a statically allocated encoder.

```c
int mux_encoder_init(struct mux_encoder *enc,
                     enum mux_codec_type codec_type,
                     int sample_rate,
                     int num_channels,
                     int num_streams,
                     const struct mux_param *params,
                     int num_params,
                     mux_sink_fn sink,
                     void *sink_user);
```

#### `mux_encoder_deinit`
Deinitialize an encoder.

```c
void mux_encoder_deinit(struct mux_encoder *enc);
```

### Encoding Operations

#### `mux_encoder_encode`
Encode audio or side channel data.

```c
int mux_encoder_encode(struct mux_encoder *enc,
                       const void *input,
                       size_t input_size,
                       int stream_type);
```

**Returns**: `MUX_OK`, a `MUX_ERROR_*` code, or the non-zero value returned by
the sink. Consumes the entire input, emitting muxed output through the sink
callback registered at construction.
**Parameters**:
- `enc`: Encoder instance
- `input`: Input data (PCM audio as int16_t for audio, any data for side channel)
- `input_size`: Size of input in bytes
- `stream_type`: `MUX_STREAM_AUDIO` or `MUX_STREAM_SIDE_CHANNEL`

**Example**:
```c
int16_t audio[8192];
int ret = mux_encoder_encode(enc, audio, sizeof(audio), MUX_STREAM_AUDIO);
// encoded bytes were delivered to the sink during this call
```

#### `mux_encoder_finalize`
Flush any codec-internal carry (e.g. a partial frame) to the sink.

```c
int mux_encoder_finalize(struct mux_encoder *enc);
```

**Note**: Call before destroying encoder or when done encoding.

#### `mux_encoder_get_error`
Get detailed error information.

```c
const struct mux_error_info *mux_encoder_get_error(struct mux_encoder *enc);
```

**Example**:
```c
if (mux_encoder_encode(enc, data, size, &consumed, MUX_STREAM_AUDIO) != MUX_OK) {
    const struct mux_error_info *err = mux_encoder_get_error(enc);
    fprintf(stderr, "Error: %s\n", err->message);
}
```

---

## Decoder API

### Dynamic Allocation

#### `mux_decoder_new`
Create a new decoder.

```c
struct mux_decoder *mux_decoder_new(enum mux_codec_type codec_type,
                                    int num_streams,
                                    const struct mux_param *params,
                                    int num_params,
                                    mux_emit_fn emit,
                                    void *emit_user);
```

**Returns**: Decoder instance or `NULL` on error
**Parameters**:
- `num_streams`: `1` = passthrough, `2` = demux audio + side channel
- `emit`: Callback receiving demuxed audio / side-channel data (required)
- `emit_user`: Opaque pointer passed to `emit`

**Example**:
```c
struct mux_decoder *dec =
    mux_decoder_new(MUX_CODEC_OPUS, 2, NULL, 0, on_output, NULL);
```

#### `mux_decoder_destroy`
Destroy a decoder.

```c
void mux_decoder_destroy(struct mux_decoder *dec);
```

### Static Allocation

#### `mux_decoder_init`
Initialize a statically allocated decoder.

```c
int mux_decoder_init(struct mux_decoder *dec,
                     enum mux_codec_type codec_type,
                     int num_streams,
                     const struct mux_param *params,
                     int num_params,
                     mux_emit_fn emit,
                     void *emit_user);
```

#### `mux_decoder_deinit`
Deinitialize a decoder.

```c
void mux_decoder_deinit(struct mux_decoder *dec);
```

### Decoding Operations

#### `mux_decoder_decode`
Decode multiplexed input.

```c
int mux_decoder_decode(struct mux_decoder *dec,
                       const void *input,
                       size_t input_size);
```

**Returns**: `MUX_OK`, a `MUX_ERROR_*` code, or the non-zero value returned by
the emit callback. Consumes the entire input, delivering demuxed data through
the emit callback registered at construction. May be called repeatedly with any
chunking (down to one byte).
**Parameters**:
- `dec`: Decoder instance
- `input`: Encoded/multiplexed input data
- `input_size`: Size of input in bytes

**Example**:
```c
uint8_t encoded[4096];
mux_decoder_decode(dec, encoded, sizeof(encoded));  // output goes to the emit cb
```

The emit callback receives `stream_type` (`MUX_STREAM_AUDIO` or
`MUX_STREAM_SIDE_CHANNEL`) per chunk; check `flags & MUX_EMIT_FRAME_END` to know
when a discrete side-channel message is complete.

#### `mux_decoder_finalize`
Flush any codec-internal carry.

```c
int mux_decoder_finalize(struct mux_decoder *dec);
```

#### `mux_decoder_get_error`
Get detailed error information.

```c
const struct mux_error_info *mux_decoder_get_error(struct mux_decoder *dec);
```

---

## Command-Line Tools

### mux

Encode raw PCM audio from stdin with optional side channel data from fd 3.

```bash
mux [options] < input.raw > output.mux
```

**Options**:
- `-c, --codec CODEC` - Codec: pcm, mp3, vorbis, opus, flac, aac (default: flac)
- `-r, --rate RATE` - Sample rate in Hz (default: 44100)
- `-n, --channels NUM` - Number of channels (default: 2)
- `-b, --bitrate KBPS` - Bitrate for lossy codecs in kbps (default: 128)
- `-l, --level LEVEL` - Compression level 0-8 for FLAC (default: 5)
- `-h, --help` - Show help

**Examples**:
```bash
# FLAC encoding
cat audio.raw | mux -c flac -r 44100 -n 2 > output.mux

# Opus with custom bitrate
mux -c opus -b 96 < input.raw > output.mux

# With side channel metadata
mux -c flac < audio.raw 3< metadata.txt > output.mux
```

### demux

Decode multiplexed stream from stdin to PCM audio and side channel data.

```bash
demux [options] < input.mux > output.raw
```

**Options**:
- `-c, --codec CODEC` - Codec: pcm, mp3, vorbis, opus, flac, aac (default: flac)
- `-v, --verbose` - Print stream information to stderr
- `-h, --help` - Show help

**Examples**:
```bash
# Decode to PCM
demux -c flac < input.mux > output.raw

# Decode with side channel extraction
demux -c opus < input.mux > audio.raw 3> metadata.txt

# Verbose output
demux -c flac -v < input.mux > output.raw
```

---

## Complete Examples

### Example 1: FLAC Lossless Compression

```c
#include <mux.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

// Simple growable byte buffer used by both callbacks.
struct buf { uint8_t *data; size_t len, cap; };
static int buf_append(void *user, const void *d, size_t n) {
    struct buf *b = user;
    if (b->len + n > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        while (b->cap < b->len + n) b->cap *= 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, d, n);
    b->len += n;
    return 0;
}
static int on_audio(void *user, int stream_type, const void *d, size_t n, int f) {
    (void)f;
    if (stream_type == MUX_STREAM_AUDIO) return buf_append(user, d, n);
    return 0;
}

int main(void) {
    int16_t audio[88200];  // 1 second stereo at 44100 Hz
    for (int i = 0; i < 44100; i++) {
        int16_t s = 10000 * sin(2 * M_PI * 440 * i / 44100);
        audio[i * 2] = audio[i * 2 + 1] = s;
    }

    // Encode: muxed bytes accumulate into 'muxed' via the sink.
    struct buf muxed = {0};
    struct mux_param params[] = { { .name = "compression", .value.i = 8 } };
    struct mux_encoder *enc =
        mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, 2, params, 1, buf_append, &muxed);
    mux_encoder_encode(enc, audio, sizeof(audio), MUX_STREAM_AUDIO);
    mux_encoder_finalize(enc);
    mux_encoder_destroy(enc);
    printf("Compressed %zu bytes to %zu bytes (%.1f%%)\n",
           sizeof(audio), muxed.len, muxed.len * 100.0 / sizeof(audio));

    // Decode: decoded PCM accumulates into 'decoded' via the emit callback.
    struct buf decoded = {0};
    struct mux_decoder *dec =
        mux_decoder_new(MUX_CODEC_FLAC, 2, NULL, 0, on_audio, &decoded);
    mux_decoder_decode(dec, muxed.data, muxed.len);
    mux_decoder_finalize(dec);
    mux_decoder_destroy(dec);

    if (decoded.len == sizeof(audio) && memcmp(audio, decoded.data, sizeof(audio)) == 0)
        printf("Perfect lossless compression verified!\n");

    free(muxed.data);
    free(decoded.data);
    return 0;
}
```

### Example 2: Opus with Side Channel Metadata

```c
#include <mux.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Collect muxed output.
struct buf { uint8_t *data; size_t len, cap; };
static int sink(void *u, const void *d, size_t n) {
    struct buf *b = u;
    if (b->len + n > b->cap) { b->cap = (b->cap ? b->cap*2 : 4096);
        while (b->cap < b->len+n) b->cap*=2; b->data = realloc(b->data,b->cap); }
    memcpy(b->data + b->len, d, n); b->len += n; return 0;
}

// On decode, audio and metadata are delivered interleaved. Reassemble each
// side-channel message using MUX_EMIT_FRAME_END.
static char msg[256]; static size_t msg_len;
static int on_out(void *u, int st, const void *d, size_t n, int flags) {
    (void)u;
    if (st == MUX_STREAM_AUDIO) {
        printf("Audio: %zu bytes\n", n);
    } else {
        memcpy(msg + msg_len, d, n); msg_len += n;
        if (flags & MUX_EMIT_FRAME_END) {
            msg[msg_len] = 0; printf("Metadata: %s\n", msg); msg_len = 0;
        }
    }
    return 0;
}

int main(void) {
    struct buf muxed = {0};
    struct mux_param params[] = { { .name = "bitrate", .value.i = 64 } };
    struct mux_encoder *enc =
        mux_encoder_new(MUX_CODEC_OPUS, 48000, 2, 2, params, 1, sink, &muxed);

    for (int chunk = 0; chunk < 10; chunk++) {
        int16_t audio[960 * 2];   // 20ms at 48kHz
        // ... fill with audio data ...
        mux_encoder_encode(enc, audio, sizeof(audio), MUX_STREAM_AUDIO);

        char meta[64];
        int len = snprintf(meta, sizeof(meta), "timestamp=%d", chunk * 20);
        mux_encoder_encode(enc, meta, len, MUX_STREAM_SIDE_CHANNEL);
    }
    mux_encoder_finalize(enc);
    mux_encoder_destroy(enc);

    struct mux_decoder *dec =
        mux_decoder_new(MUX_CODEC_OPUS, 2, NULL, 0, on_out, NULL);
    mux_decoder_decode(dec, muxed.data, muxed.len);
    mux_decoder_finalize(dec);
    mux_decoder_destroy(dec);

    free(muxed.data);
    return 0;
}
```

### Example 3: Codec Parameter Discovery

```c
#include <mux.h>
#include <stdio.h>

void print_codec_info(enum mux_codec_type codec) {
    const char *name = mux_codec_to_name(codec);
    printf("\n=== %s ===\n", name);

    // Get encoder parameters
    const struct mux_param_desc *params;
    int count;
    if (mux_get_encoder_params(codec, &params, &count) == MUX_OK) {
        printf("Encoder parameters:\n");
        for (int i = 0; i < count; i++) {
            printf("  %s: %s\n", params[i].name, params[i].description);
            if (params[i].type == MUX_PARAM_TYPE_INT) {
                printf("    Range: %d-%d (default: %d)\n",
                       params[i].range.i.min,
                       params[i].range.i.max,
                       params[i].range.i.def);
            }
        }
    }

    // Get sample rate info
    struct mux_sample_rate_list sr_list;
    if (mux_get_supported_sample_rates(codec, &sr_list) == MUX_OK) {
        printf("Sample rates: ");
        if (sr_list.is_range) {
            printf("%d-%d Hz (continuous)\n", sr_list.rates[0], sr_list.rates[1]);
        } else {
            for (int i = 0; i < sr_list.count; i++) {
                printf("%d%s", sr_list.rates[i], i < sr_list.count - 1 ? ", " : "");
            }
            printf(" Hz (discrete)\n");
        }
    }
}

int main(void) {
    // List all codecs
    const struct mux_codec_info *codecs;
    int count;
    mux_list_codecs(&codecs, &count);

    for (int i = 0; i < count; i++) {
        print_codec_info(codecs[i].type);
    }

    return 0;
}
```

---

## Codec-Specific Parameters

### FLAC
```c
struct mux_param params[] = {
    { .name = "compression", .value.i = 8 }  // 0 (fast) - 8 (best)
};
```

### MP3
```c
struct mux_param params[] = {
    { .name = "bitrate", .value.i = 192 }  // kbps: 8-320
};
```

### Vorbis
```c
struct mux_param params[] = {
    { .name = "quality", .value.f = 0.6f }  // 0.0 (low) - 1.0 (high)
};
// Or:
struct mux_param params[] = {
    { .name = "bitrate", .value.i = 128 }  // kbps
};
```

### Opus
```c
struct mux_param params[] = {
    { .name = "bitrate", .value.i = 128 },     // kbps: 6-510
    { .name = "complexity", .value.i = 10 },   // 0-10
    { .name = "vbr", .value.i = 1 }            // 0=CBR, 1=VBR
};
```

### AAC
```c
struct mux_param params[] = {
    { .name = "bitrate", .value.i = 128 },   // kbps: 8-512
    { .name = "profile", .value.i = 2 }      // 2=LC, 5=HE, 29=HEv2
};
```

---

## Building & Dependencies

### Required Dependencies
- CMake 3.10+
- C99 compiler (GCC, Clang)

### Optional Dependencies (Codecs)
- **MP3**: libmp3lame-dev, libmpg123-dev
- **Vorbis**: libogg-dev, libvorbis-dev
- **Opus**: libogg-dev, libopus-dev
- **FLAC**: libogg-dev, libflac-dev
- **AAC**: libfdk-aac-dev (note: FDK-AAC has custom license restrictions)

### Build Options
```bash
mkdir build && cd build

# Build with all available codecs
cmake ..
make

# Build without specific codec
cmake -DHAVE_MP3=OFF ..

# Install
sudo make install
```

### Checking Available Codecs
After building, check which codecs are available:

```bash
./mux --help  # Lists available codecs
```

Or programmatically:

```c
const struct mux_codec_ops *ops = mux_get_codec_ops(MUX_CODEC_AAC);
if (ops) {
    printf("AAC is available\n");
} else {
    printf("AAC is not available\n");
}
```

---

## Performance

Typical compression ratios (1 second, 44100 Hz stereo, 440 Hz sine wave):

| Codec       | Settings      | Size  | Ratio | Type     |
|-------------|---------------|-------|-------|----------|
| PCM         | -             | 176KB | 100%  | Lossless |
| FLAC        | Level 8       | 18KB  | 10%   | Lossless |
| Vorbis      | Quality 0.4   | 8KB   | 4.4%  | Lossy    |
| Opus        | 128 kbps      | 14KB  | 8.2%  | Lossy    |
| MP3         | 192 kbps      | 25KB  | 14.6% | Lossy    |
| AAC         | 128 kbps      | 15KB  | 8.5%  | Lossy    |

---

## Error Handling

All functions return an error code or NULL on failure. Get detailed error information:

```c
if (mux_encoder_encode(enc, data, size, &consumed, MUX_STREAM_AUDIO) != MUX_OK) {
    const struct mux_error_info *err = mux_encoder_get_error(enc);
    fprintf(stderr, "Error %d: %s\n", err->code, err->message);
    if (err->source) {
        fprintf(stderr, "Source: %s (code %d)\n", err->source, err->source_code);
        if (err->source_msg) {
            fprintf(stderr, "Details: %s\n", err->source_msg);
        }
    }
}
```

---

## Testing

```bash
# Run all tests
make test

# Run specific test
./test_flac_simple
./test_opus_simple
./test_vorbis_validation
./test_flac_validation
```

---

## License

GPL-3.0-or-later

**Note**: AAC support uses libfdk-aac which has its own license restrictions (not GPL).

---

## Contributing

Contributions welcome! Please ensure:
- Code follows existing style
- All tests pass
- New features include tests
- API changes are documented

---

## See Also

- Example code in `tests/` directory
