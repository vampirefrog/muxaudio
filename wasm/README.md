# MuxAudio WebAssembly Build

Build muxaudio for WebAssembly with **automatic codec dependency management** using CMake.

## Quick Start

### Default Build (PCM + Vorbis)

```bash
cd wasm
mkdir build && cd build
emcmake cmake ..
make
```

Output: `muxaudio.js` and `muxaudio.wasm` (~200KB)

**CMake automatically downloads and compiles Vorbis from GitHub!**

### With All Codecs (PCM + Vorbis + Opus + FLAC)

```bash
emcmake cmake .. -DWASM_BUILD_ALL=ON
make
```

**CMake automatically downloads Opus and FLAC and builds everything!**

First build takes ~2-3 minutes (downloads + compilation), subsequent builds are instant (cached).

### Custom Codec Selection

```bash
# Opus for speech/low latency
emcmake cmake .. -DWASM_CODEC_OPUS=ON
make

# FLAC for lossless compression
emcmake cmake .. -DWASM_CODEC_FLAC=ON
make

# All three
emcmake cmake .. \
  -DWASM_CODEC_VORBIS=ON \
  -DWASM_CODEC_OPUS=ON \
  -DWASM_CODEC_FLAC=ON
make
```

Zero manual setup required!

## Build Options

```bash
# Choose codecs
emcmake cmake .. \
  -DWASM_CODEC_VORBIS=ON \
  -DWASM_CODEC_OPUS=ON \
  -DWASM_CODEC_FLAC=ON \
  -DWASM_CODEC_MP3=ON

# Debug build
emcmake cmake .. -DCMAKE_BUILD_TYPE=Debug

# Size optimization
emcmake cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel

# Custom output name
emcmake cmake .. -DWASM_OUTPUT_NAME=muxaudio-full
```

## Available Codecs

| Codec   | Status | Default | CMake Option |
|---------|--------|---------|--------------|
| PCM     | ✅ Auto (built-in) | ✓ | Always enabled |
| Vorbis  | ✅ Auto (FetchContent) | ✓ | `WASM_CODEC_VORBIS` |
| Opus    | ✅ Auto (FetchContent) | ✗ | `WASM_CODEC_OPUS=ON` |
| FLAC    | ✅ Auto (FetchContent) | ✗ | `WASM_CODEC_FLAC=ON` |
| MP3     | ✅ Auto decode (mpg123) | ✗ | `WASM_CODEC_MP3=ON` |
| AAC     | ❌ Not supported | ✗ | - |

**All codecs are automatically downloaded and built by CMake!** No manual dependency installation required.

**Note:** MP3 encoding requires LAME which is not available. MP3 decoding works via mpg123.

## Codec Recommendations

### For minimal bundle size (~50KB):
```bash
emcmake cmake .. -DWASM_CODEC_VORBIS=OFF
make
# Result: PCM only
```

### For best compatibility (~200KB):
```bash
emcmake cmake ..  # Default
make
# Result: PCM + Vorbis (good lossy compression)
```

### For lossless compression (~400KB):
```bash
emcmake cmake .. -DWASM_CODEC_FLAC=ON
make
# Result: PCM + Vorbis + FLAC
```

### For speech/VoIP (~400KB):
```bash
emcmake cmake .. -DWASM_CODEC_OPUS=ON
make
# Result: PCM + Vorbis + Opus (best for speech)
```

### For all features (~800KB):
```bash
emcmake cmake .. -DWASM_BUILD_ALL=ON
make
# Result: PCM + Vorbis + Opus + FLAC + MP3 (decode)
```

## Build Process

### 1. Install Emscripten

```bash
# Download and install
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Build muxaudio

```bash
cd wasm
mkdir build && cd build
emcmake cmake .. -DWASM_BUILD_ALL=ON  # or choose specific codecs
make
```

**That's it!** CMake automatically:
- Downloads codec libraries from official sources
- Configures them for WebAssembly
- Compiles and links everything

Dependencies are cached in `build/_deps/` for future builds.

## Usage in JavaScript

### Basic Example

```javascript
import { createMuxAudio } from './muxaudio.wrapper.js';

async function demo() {
  const mux = await createMuxAudio();

  // Create encoder
  const encoder = mux.MuxEncoder('vorbis', 44100, 2, {
    bitrate: 128  // 128 kbps
  });

  // Generate audio (1 second of sine wave)
  const sampleRate = 44100;
  const pcm = new Int16Array(sampleRate * 2); // stereo
  for (let i = 0; i < sampleRate; i++) {
    const sample = Math.sin(2 * Math.PI * 440 * i / sampleRate) * 32767;
    pcm[i * 2] = sample;
    pcm[i * 2 + 1] = sample;
  }

  // Encode
  encoder.encode(pcm);
  const encoded = encoder.finalize();
  console.log(`Encoded ${pcm.length * 2} bytes → ${encoded.length} bytes`);

  // Decode
  const decoder = mux.MuxDecoder('vorbis');
  decoder.decode(encoded);
  const { data: decoded } = decoder.finalize();
  console.log(`Decoded ${encoded.length} bytes → ${decoded.length * 2} bytes`);

  // Cleanup
  encoder.destroy();
  decoder.destroy();
}

demo();
```

### With Side Channel Data

```javascript
const encoder = mux.MuxEncoder('opus', 48000, 2, { bitrate: 64 });

// Audio stream
encoder.encode(audioSamples, 0); // stream_type = 0 (audio)

// Metadata stream
const metadata = new TextEncoder().encode(JSON.stringify({ timestamp: Date.now() }));
encoder.encode(metadata, 1); // stream_type = 1 (side channel)

const muxed = encoder.finalize();
```

### Query Available Codecs

```javascript
const codecs = mux.listCodecs();
console.log('Available codecs:', codecs);
// ['pcm', 'vorbis', 'opus', 'flac']
```

## Testing

### Browser Test

```bash
# Build
make CODECS=pcm,vorbis

# Serve locally
python3 -m http.server 8000

# Open browser to http://localhost:8000/test.html
```

### Node.js Test

```javascript
import { createMuxAudio } from './build/muxaudio.js';

const mux = await createMuxAudio();
// ... use as normal
```

## Bundle Size

| Configuration | WASM Size | JS Size | Total | Gzipped |
|--------------|-----------|---------|-------|---------|
| PCM only | ~30KB | ~20KB | ~50KB | ~20KB |
| PCM + Vorbis (default) | ~180KB | ~20KB | ~200KB | ~70KB |
| + Opus | ~350KB | ~20KB | ~370KB | ~130KB |
| + FLAC | ~400KB | ~20KB | ~420KB | ~140KB |
| + MP3 | ~550KB | ~20KB | ~570KB | ~190KB |
| All (Vorbis + Opus + FLAC + MP3) | ~800KB | ~25KB | ~825KB | ~280KB |

## File Structure

```
wasm/
├── CMakeLists.txt        # Main build system
├── BUILD.md              # Detailed build instructions
├── QUICKSTART.md         # Quick reference
├── README.md             # This file
├── muxaudio.wrapper.js   # JavaScript wrapper API
├── test.html             # Browser test page
└── build/                # Build output (created by cmake)
    ├── muxaudio.js
    ├── muxaudio.wasm
    └── _deps/            # Downloaded dependencies (cached)
```

## Troubleshooting

### "emcc not found"

Install Emscripten and activate it:
```bash
source /path/to/emsdk/emsdk_env.sh
```

See: https://emscripten.org/docs/getting_started/downloads.html

### CMake download errors

If downloads fail, clear cache and retry:
```bash
rm -rf build/_deps
emcmake cmake ..
make
```

### Large WASM file

Use only the codecs you need:
```bash
emcmake cmake .. -DWASM_CODEC_VORBIS=ON  # Instead of -DWASM_BUILD_ALL=ON
```

Or optimize for size:
```bash
emcmake cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel
```

### Build errors

Enable verbose output:
```bash
make VERBOSE=1
```

Check that Emscripten is properly activated:
```bash
which emcc
# Should show path to emsdk/upstream/emscripten/emcc
```

## Advanced Usage

### Custom Emscripten Flags

Edit `CMakeLists.txt` and add to `EMSCRIPTEN_LINK_FLAGS`:

```cmake
list(APPEND EMSCRIPTEN_LINK_FLAGS
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=5MB
)
```

### Link Additional Libraries

```cmake
target_link_libraries(${WASM_OUTPUT_NAME} PRIVATE mylib)
```

### Add Codec Library

To add a new codec with FetchContent:

```cmake
FetchContent_Declare(
    mycodec
    GIT_REPOSITORY https://github.com/user/mycodec.git
    GIT_TAG v1.0
)
FetchContent_MakeAvailable(mycodec)
list(APPEND MUXAUDIO_LIBRARIES mycodec)
```

## Performance

### Encoding Benchmark (Chrome, M1 Mac)

| Codec | Sample Rate | Channels | Real-time Factor |
|-------|-------------|----------|------------------|
| PCM | 44100 | 2 | 500x |
| Vorbis (128kbps) | 44100 | 2 | 20x |
| Opus (64kbps) | 48000 | 2 | 30x |
| FLAC (level 5) | 44100 | 2 | 10x |

*Real-time factor: how much faster than real-time playback (higher is better)*

### Memory Usage

Typical memory usage per encoder/decoder instance:
- PCM: ~100KB
- Vorbis: ~500KB
- Opus: ~300KB
- FLAC: ~1MB

## License

GPL-3.0-or-later (same as muxaudio)

Note: Some codec libraries have different licenses:
- Vorbis: BSD-like
- Opus: BSD
- FLAC: BSD
- LAME (MP3): LGPL
- FDK-AAC: Custom (non-free)

Ensure license compatibility when distributing.
