[![Native Build](https://github.com/vampirefrog/muxaudio/actions/workflows/build-native.yml/badge.svg)](https://github.com/vampirefrog/muxaudio/actions/workflows/build-native.yml)
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

```c
#include <mux.h>

// Encode audio with FLAC
struct mux_encoder *enc = mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, NULL, 0);
mux_encoder_encode(enc, pcm_data, pcm_size, &consumed, MUX_STREAM_AUDIO);
mux_encoder_finalize(enc);
mux_encoder_read(enc, output, sizeof(output), &written);
mux_encoder_destroy(enc);

// Decode
struct mux_decoder *dec = mux_decoder_new(MUX_CODEC_FLAC, NULL, 0);
mux_decoder_decode(dec, input, input_size, &consumed);
mux_decoder_finalize(dec);
mux_decoder_read(dec, pcm_out, sizeof(pcm_out), &written, &stream_type);
mux_decoder_destroy(dec);
```

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
#define MUX_ERROR_AGAIN   8   // Try again (need more data)
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
                                    const struct mux_param *params,
                                    int num_params);
```

**Returns**: Encoder instance or `NULL` on error
**Parameters**:
- `codec_type`: Codec to use
- `sample_rate`: Sample rate in Hz (e.g., 44100)
- `num_channels`: Number of channels (1=mono, 2=stereo)
- `params`: Optional parameters array (can be `NULL`)
- `num_params`: Number of parameters (0 if `params` is `NULL`)

**Example**:
```c
// FLAC with compression level 8
struct mux_param params[] = {
    { .name = "compression", .value.i = 8 }
};
struct mux_encoder *enc = mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, params, 1);
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
                     const struct mux_param *params,
                     int num_params);
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
                       size_t *input_consumed,
                       int stream_type);
```

**Returns**: `MUX_OK` on success, error code on failure
**Parameters**:
- `enc`: Encoder instance
- `input`: Input data (PCM audio as int16_t for audio, any data for side channel)
- `input_size`: Size of input in bytes
- `input_consumed`: Pointer to receive bytes consumed
- `stream_type`: `MUX_STREAM_AUDIO` or `MUX_STREAM_SIDE_CHANNEL`

**Example**:
```c
int16_t audio[8192];
size_t consumed;
int ret = mux_encoder_encode(enc, audio, sizeof(audio), &consumed, MUX_STREAM_AUDIO);
```

#### `mux_encoder_read`
Read encoded/multiplexed output.

```c
int mux_encoder_read(struct mux_encoder *enc,
                     void *output,
                     size_t output_size,
                     size_t *output_written);
```

**Returns**: `MUX_OK` on success, `MUX_ERROR_AGAIN` if no data available
**Example**:
```c
uint8_t buffer[4096];
size_t written;
while (mux_encoder_read(enc, buffer, sizeof(buffer), &written) == MUX_OK) {
    // Write buffer to file/stream
}
```

#### `mux_encoder_finalize`
Flush any buffered data.

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
                                    const struct mux_param *params,
                                    int num_params);
```

**Returns**: Decoder instance or `NULL` on error
**Example**:
```c
struct mux_decoder *dec = mux_decoder_new(MUX_CODEC_OPUS, NULL, 0);
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
                     const struct mux_param *params,
                     int num_params);
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
                       size_t input_size,
                       size_t *input_consumed);
```

**Returns**: `MUX_OK` on success
**Parameters**:
- `dec`: Decoder instance
- `input`: Encoded/multiplexed input data
- `input_size`: Size of input in bytes
- `input_consumed`: Pointer to receive bytes consumed

**Example**:
```c
uint8_t encoded[4096];
size_t consumed;
mux_decoder_decode(dec, encoded, sizeof(encoded), &consumed);
```

#### `mux_decoder_read`
Read decoded audio or side channel data.

```c
int mux_decoder_read(struct mux_decoder *dec,
                     void *output,
                     size_t output_size,
                     size_t *output_written,
                     int *stream_type);
```

**Returns**: `MUX_OK` on success, `MUX_ERROR_AGAIN` if no data available
**Parameters**:
- `stream_type`: Receives `MUX_STREAM_AUDIO` or `MUX_STREAM_SIDE_CHANNEL`

**Example**:
```c
int16_t pcm[8192];
size_t written;
int stream_type;
while (mux_decoder_read(dec, pcm, sizeof(pcm), &written, &stream_type) == MUX_OK) {
    if (stream_type == MUX_STREAM_AUDIO) {
        // Process audio
    } else {
        // Process side channel data
    }
}
```

#### `mux_decoder_finalize`
Flush any buffered decoded data.

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

int main(void) {
    // Allocate test audio
    int16_t audio[88200];  // 1 second stereo at 44100 Hz

    // Generate 440 Hz sine wave
    for (int i = 0; i < 44100; i++) {
        int16_t sample = 10000 * sin(2 * M_PI * 440 * i / 44100);
        audio[i * 2] = sample;     // Left
        audio[i * 2 + 1] = sample; // Right
    }

    // Create FLAC encoder with max compression
    struct mux_param params[] = {
        { .name = "compression", .value.i = 8 }
    };
    struct mux_encoder *enc = mux_encoder_new(MUX_CODEC_FLAC, 44100, 2, params, 1);

    // Encode
    size_t consumed;
    mux_encoder_encode(enc, audio, sizeof(audio), &consumed, MUX_STREAM_AUDIO);
    mux_encoder_finalize(enc);

    // Read output
    uint8_t output[65536];
    size_t total = 0;
    size_t written;
    while (mux_encoder_read(enc, output + total, sizeof(output) - total, &written) == MUX_OK) {
        total += written;
    }

    printf("Compressed %zu bytes to %zu bytes (%.1f%%)\n",
           sizeof(audio), total, total * 100.0 / sizeof(audio));

    mux_encoder_destroy(enc);

    // Decode back
    struct mux_decoder *dec = mux_decoder_new(MUX_CODEC_FLAC, NULL, 0);
    mux_decoder_decode(dec, output, total, &consumed);
    mux_decoder_finalize(dec);

    int16_t decoded[88200];
    int stream_type;
    size_t decoded_size = 0;
    while (mux_decoder_read(dec, decoded + decoded_size / sizeof(int16_t),
                           sizeof(decoded) - decoded_size,
                           &written, &stream_type) == MUX_OK) {
        decoded_size += written;
    }

    // Verify lossless
    if (memcmp(audio, decoded, sizeof(audio)) == 0) {
        printf("Perfect lossless compression verified!\n");
    }

    mux_decoder_destroy(dec);
    return 0;
}
```

### Example 2: Opus with Side Channel Metadata

```c
#include <mux.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    // Create Opus encoder (48 kHz required for Opus)
    struct mux_param params[] = {
        { .name = "bitrate", .value.i = 64 }  // 64 kbps
    };
    struct mux_encoder *enc = mux_encoder_new(MUX_CODEC_OPUS, 48000, 2, params, 1);

    // Encode audio chunks with timestamp metadata
    for (int chunk = 0; chunk < 10; chunk++) {
        // Generate 20ms of audio (960 samples at 48kHz)
        int16_t audio[960 * 2];
        // ... fill with audio data ...

        size_t consumed;
        mux_encoder_encode(enc, audio, sizeof(audio), &consumed, MUX_STREAM_AUDIO);

        // Add timestamp metadata for this chunk
        char metadata[64];
        snprintf(metadata, sizeof(metadata), "timestamp=%d", chunk * 20);
        mux_encoder_encode(enc, metadata, strlen(metadata) + 1,
                          &consumed, MUX_STREAM_SIDE_CHANNEL);
    }

    mux_encoder_finalize(enc);

    // Read multiplexed output
    uint8_t muxed[65536];
    size_t total = 0;
    size_t written;
    while (mux_encoder_read(enc, muxed + total, sizeof(muxed) - total, &written) == MUX_OK) {
        total += written;
    }

    mux_encoder_destroy(enc);

    // Decode - audio and metadata come out interleaved
    struct mux_decoder *dec = mux_decoder_new(MUX_CODEC_OPUS, NULL, 0);
    mux_decoder_decode(dec, muxed, total, &consumed);
    mux_decoder_finalize(dec);

    uint8_t buffer[8192];
    int stream_type;
    while (mux_decoder_read(dec, buffer, sizeof(buffer), &written, &stream_type) == MUX_OK) {
        if (stream_type == MUX_STREAM_AUDIO) {
            printf("Audio: %zu bytes\n", written);
        } else {
            printf("Metadata: %s\n", (char *)buffer);
        }
    }

    mux_decoder_destroy(dec);
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
