# WebAssembly Strategy - Minimal Glue Approach

## Overview

Compile muxaudio to WebAssembly and expose a clean JavaScript API with minimal glue code.

## Core Strategy: Direct C API Exposure

The muxaudio C API is already clean and well-designed. Instead of writing wrapper layers, we'll:

1. **Export C functions directly** using Emscripten's EXPORTED_FUNCTIONS
2. **Use JS TypedArrays** to share memory between JS and WASM
3. **Create thin JS wrapper class** that handles memory allocation/copying only
4. **Start with PCM codec only** (zero dependencies, smallest WASM size)

## Build Configuration

### CMake for WASM

Create `wasm/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.10)
project(muxaudio-wasm)

set(CMAKE_C_COMPILER "emcc")
set(CMAKE_C_STANDARD 99)

# Build PCM-only version (no external dependencies)
add_library(muxaudio-wasm
    ../src/core.c
    ../src/buffer.c
    ../src/error.c
    ../src/mux_leb128.c
    ../src/codec_pcm.c
)

target_include_directories(muxaudio-wasm PRIVATE
    ../include
    ../src
)

# Emscripten flags
set(EMCC_FLAGS
    -s WASM=1
    -s MODULARIZE=1
    -s EXPORT_NAME=createMuxAudio
    -s ALLOW_MEMORY_GROWTH=1
    -s EXPORTED_RUNTIME_METHODS=['cwrap','ccall','setValue','getValue','_malloc','_free']
    -s EXPORTED_FUNCTIONS=['_malloc','_free','_mux_encoder_new','_mux_encoder_destroy','_mux_encoder_encode','_mux_encoder_read','_mux_encoder_finalize','_mux_decoder_new','_mux_decoder_destroy','_mux_decoder_decode','_mux_decoder_read','_mux_decoder_finalize','_mux_codec_from_name']
    -O3
)

set_target_properties(muxaudio-wasm PROPERTIES
    SUFFIX ".js"
    COMPILE_FLAGS "${EMCC_FLAGS}"
    LINK_FLAGS "${EMCC_FLAGS}"
)
```

### Alternative: Direct emcc command

```bash
emcc \
  src/core.c src/buffer.c src/error.c src/mux_leb128.c src/codec_pcm.c \
  -I include -I src \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createMuxAudio \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_RUNTIME_METHODS=['cwrap','ccall','_malloc','_free'] \
  -s EXPORTED_FUNCTIONS=['_malloc','_free','_mux_encoder_new','_mux_encoder_destroy','_mux_encoder_encode','_mux_encoder_read','_mux_encoder_finalize','_mux_decoder_new','_mux_decoder_destroy','_mux_decoder_decode','_mux_decoder_read','_mux_decoder_finalize','_mux_codec_from_name'] \
  -O3 \
  -o wasm/muxaudio.js
```

## JavaScript Wrapper (Minimal)

Create `wasm/muxaudio.wrapper.js`:

```javascript
export class MuxEncoder {
  constructor(module, codec, sampleRate, channels) {
    this.module = module;
    this.codecType = 0; // MUX_CODEC_PCM

    // C functions bound via cwrap
    this._new = module.cwrap('mux_encoder_new', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._destroy = module.cwrap('mux_encoder_destroy', null, ['number']);
    this._encode = module.cwrap('mux_encoder_encode', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._read = module.cwrap('mux_encoder_read', 'number',
      ['number', 'number', 'number', 'number']);
    this._finalize = module.cwrap('mux_encoder_finalize', 'number', ['number']);

    // Create encoder instance
    this.encoder = this._new(this.codecType, sampleRate, channels, 0, 0);
    if (!this.encoder) {
      throw new Error('Failed to create encoder');
    }
  }

  encode(audioData, streamType = 0) {
    const { _malloc, _free, HEAP16 } = this.module;

    // Allocate input buffer in WASM memory
    const numBytes = audioData.length * 2; // int16
    const inputPtr = _malloc(numBytes);
    HEAP16.set(new Int16Array(audioData.buffer), inputPtr / 2);

    // Allocate consumed counter
    const consumedPtr = _malloc(8);

    // Call encode
    const result = this._encode(
      this.encoder,
      inputPtr,
      numBytes,
      consumedPtr,
      streamType
    );

    _free(inputPtr);
    _free(consumedPtr);

    if (result !== 0) {
      throw new Error(`Encode failed: ${result}`);
    }

    return this.read();
  }

  read() {
    const { _malloc, _free, HEAPU8 } = this.module;
    const outputSize = 1024 * 1024; // 1MB buffer
    const outputPtr = _malloc(outputSize);
    const writtenPtr = _malloc(8);

    const result = this._read(this.encoder, outputPtr, outputSize, writtenPtr);

    if (result === 0) {
      const written = this.module.getValue(writtenPtr, 'i64');
      const output = HEAPU8.slice(outputPtr, outputPtr + written);
      _free(outputPtr);
      _free(writtenPtr);
      return output;
    }

    _free(outputPtr);
    _free(writtenPtr);
    return new Uint8Array(0);
  }

  finalize() {
    this._finalize(this.encoder);
    return this.read();
  }

  destroy() {
    if (this.encoder) {
      this._destroy(this.encoder);
      this.encoder = null;
    }
  }
}

export class MuxDecoder {
  constructor(module, codec) {
    this.module = module;
    this.codecType = 0; // MUX_CODEC_PCM

    this._new = module.cwrap('mux_decoder_new', 'number',
      ['number', 'number', 'number']);
    this._destroy = module.cwrap('mux_decoder_destroy', null, ['number']);
    this._decode = module.cwrap('mux_decoder_decode', 'number',
      ['number', 'number', 'number', 'number']);
    this._read = module.cwrap('mux_decoder_read', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._finalize = module.cwrap('mux_decoder_finalize', 'number', ['number']);

    this.decoder = this._new(this.codecType, 0, 0);
    if (!this.decoder) {
      throw new Error('Failed to create decoder');
    }
  }

  decode(muxData) {
    const { _malloc, _free, HEAPU8 } = this.module;

    const inputPtr = _malloc(muxData.length);
    HEAPU8.set(muxData, inputPtr);

    const consumedPtr = _malloc(8);

    const result = this._decode(
      this.decoder,
      inputPtr,
      muxData.length,
      consumedPtr
    );

    _free(inputPtr);
    _free(consumedPtr);

    if (result !== 0 && result !== -6) { // MUX_ERROR_EOF is ok
      throw new Error(`Decode failed: ${result}`);
    }

    return this.read();
  }

  read() {
    const { _malloc, _free, HEAP16 } = this.module;
    const outputSize = 1024 * 1024;
    const outputPtr = _malloc(outputSize);
    const writtenPtr = _malloc(8);
    const streamTypePtr = _malloc(4);

    const result = this._read(
      this.decoder,
      outputPtr,
      outputSize,
      writtenPtr,
      streamTypePtr
    );

    if (result === 0) {
      const written = this.module.getValue(writtenPtr, 'i64');
      const streamType = this.module.getValue(streamTypePtr, 'i32');
      const output = new Int16Array(
        HEAP16.buffer.slice(outputPtr, outputPtr + written)
      );

      _free(outputPtr);
      _free(writtenPtr);
      _free(streamTypePtr);

      return { data: output, streamType };
    }

    _free(outputPtr);
    _free(writtenPtr);
    _free(streamTypePtr);

    return { data: new Int16Array(0), streamType: 0 };
  }

  finalize() {
    this._finalize(this.decoder);
    return this.read();
  }

  destroy() {
    if (this.decoder) {
      this._destroy(this.decoder);
      this.decoder = null;
    }
  }
}

export async function createMuxAudio() {
  const Module = await createMuxAudioWasm();
  return {
    Module,
    MuxEncoder: (codec, sr, ch) => new MuxEncoder(Module, codec, sr, ch),
    MuxDecoder: (codec) => new MuxDecoder(Module, codec)
  };
}
```

## Usage Example

```javascript
import { createMuxAudio } from './muxaudio.wrapper.js';

async function demo() {
  const mux = await createMuxAudio();

  // Create encoder
  const encoder = mux.MuxEncoder('pcm', 44100, 2);

  // Generate 1 second of sine wave
  const sampleRate = 44100;
  const pcm = new Int16Array(sampleRate * 2); // stereo
  for (let i = 0; i < sampleRate; i++) {
    const sample = Math.sin(2 * Math.PI * 440 * i / sampleRate) * 32767;
    pcm[i * 2] = sample;     // left
    pcm[i * 2 + 1] = sample; // right
  }

  // Encode
  encoder.encode(pcm);
  const muxed = encoder.finalize();
  console.log(`Encoded ${pcm.length * 2} bytes → ${muxed.length} bytes`);

  // Decode
  const decoder = mux.MuxDecoder('pcm');
  decoder.decode(muxed);
  const { data: decoded } = decoder.finalize();
  console.log(`Decoded ${muxed.length} bytes → ${decoded.length * 2} bytes`);

  // Cleanup
  encoder.destroy();
  decoder.destroy();
}
```

## Build Steps

```bash
# Install emscripten
# See: https://emscripten.org/docs/getting_started/downloads.html

# Build
cd wasm
emcc \
  ../src/core.c ../src/buffer.c ../src/error.c ../src/mux_leb128.c ../src/codec_pcm.c \
  -I ../include -I ../src \
  -s WASM=1 \
  -s MODULARIZE=1 \
  -s EXPORT_NAME=createMuxAudioWasm \
  -s ALLOW_MEMORY_GROWTH=1 \
  -s EXPORTED_RUNTIME_METHODS=['cwrap','setValue','getValue','_malloc','_free'] \
  -s EXPORTED_FUNCTIONS=['_malloc','_free','_mux_encoder_new','_mux_encoder_destroy','_mux_encoder_encode','_mux_encoder_read','_mux_encoder_finalize','_mux_decoder_new','_mux_decoder_destroy','_mux_decoder_decode','_mux_decoder_read','_mux_decoder_finalize'] \
  -O3 \
  -o muxaudio.js

# Output: muxaudio.js and muxaudio.wasm
```

## Why This is Minimal

1. **No wrapper functions in C** - Export existing API directly
2. **No complex type marshalling** - Just pointers and buffers
3. **Simple memory management** - Only malloc/free for buffers
4. **Single codec (PCM)** - No external dependencies
5. **~30 lines per class** - Thin JS wrappers only

## Adding More Codecs Later

For codecs with no external dependencies (like a pure C Opus implementation):
- Add source files to emcc command
- Define HAVE_OPUS or HAVE_CODEC in compile flags
- No JS changes needed - same API

For external libraries (MP3, Vorbis):
- Compile library to WASM separately
- Link .bc files together
- Increases bundle size significantly

## Size Estimates

- PCM only: ~50KB WASM
- PCM + Opus (if pure C): ~200KB WASM
- PCM + Opus + Vorbis: ~500KB+ WASM

## Testing

Create `wasm/test.html`:

```html
<!DOCTYPE html>
<html>
<head><title>MuxAudio WASM Test</title></head>
<body>
  <h1>MuxAudio WASM Test</h1>
  <pre id="output"></pre>

  <script type="module">
    import { createMuxAudio } from './muxaudio.wrapper.js';

    const out = document.getElementById('output');

    async function test() {
      out.textContent = 'Loading WASM...\n';
      const mux = await createMuxAudio();
      out.textContent += 'Loaded!\n';

      const encoder = mux.MuxEncoder('pcm', 44100, 2);
      const pcm = new Int16Array(1024);
      for (let i = 0; i < 1024; i++) {
        pcm[i] = i % 256;
      }

      encoder.encode(pcm);
      const muxed = encoder.finalize();
      out.textContent += `Encoded: ${muxed.length} bytes\n`;

      const decoder = mux.MuxDecoder('pcm');
      decoder.decode(muxed);
      const { data } = decoder.finalize();
      out.textContent += `Decoded: ${data.length} samples\n`;

      encoder.destroy();
      decoder.destroy();

      out.textContent += 'Test passed!';
    }

    test().catch(e => out.textContent += `Error: ${e}`);
  </script>
</body>
</html>
```

## Summary

This approach minimizes glue by:
- Exposing C API directly (no wrapper layer)
- Using Emscripten's cwrap for automatic binding
- Keeping JS wrapper thin (memory management only)
- Starting with dependency-free PCM codec
- Clean separation: C does audio processing, JS does memory copies

Total code: ~200 lines JS wrapper + build script vs thousands of lines for alternatives.
