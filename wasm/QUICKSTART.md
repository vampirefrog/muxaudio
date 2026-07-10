# MuxAudio WASM - Quick Start

## 1-Minute Setup

### Install Emscripten (one-time)

```bash
# Download
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Install and activate
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### Build (automatic dependencies!)

```bash
cd /path/to/muxaudio/wasm
mkdir build && cd build
emcmake cmake ..
make
```

Output: `muxaudio.js` and `muxaudio.wasm` (~200KB total)

**CMake automatically downloads and compiles Vorbis from GitHub!** First build takes 1-2 minutes, subsequent builds are instant (cached).

### Test

```bash
# In wasm/build/
cp ../test.html ../muxaudio.wrapper.js .
python3 -m http.server 8000
# Open http://localhost:8000/test.html
```

## Build Options

### Minimal (PCM only, ~50KB)

```bash
emcmake cmake .. -DWASM_CODEC_VORBIS=OFF
make
```

### Default (PCM + Vorbis, ~200KB)

```bash
emcmake cmake ..
make
```

### With additional codecs (automatic!)

All codecs except MP3 work automatically via CMake FetchContent:

```bash
# PCM only (~50KB)
emcmake cmake .. -DWASM_CODEC_VORBIS=OFF
make

# PCM + Vorbis (~200KB) - DEFAULT
emcmake cmake ..
make

# PCM + Vorbis + Opus (~400KB) - Great for speech/VoIP
emcmake cmake .. -DWASM_CODEC_OPUS=ON
make

# PCM + Vorbis + FLAC (~400KB) - Lossless compression
emcmake cmake .. -DWASM_CODEC_FLAC=ON
make

# All codecs (~800KB) - RECOMMENDED
emcmake cmake .. -DWASM_BUILD_ALL=ON
make

# MP3 decode support
emcmake cmake .. -DWASM_CODEC_MP3=ON
make
```

**CMake downloads and compiles everything automatically!**

## Usage

### JavaScript

```javascript
import { createMuxAudio } from './muxaudio.wrapper.js';

const mux = await createMuxAudio();

// Encode
const encoder = mux.Encoder('vorbis', 44100, 2, { bitrate: 128 });
encoder.encode(pcmSamples);  // Int16Array
const muxed = encoder.finalize();  // Uint8Array
encoder.destroy();

// Decode
const decoder = mux.Decoder('vorbis');
decoder.decode(muxed);
const outputs = decoder.finalize();  // Array of {data, streamType}
decoder.destroy();
```

### With Side Channel

```javascript
const encoder = mux.Encoder('opus', 48000, 2);

// Audio
encoder.encode(audioSamples, mux.STREAM_AUDIO);

// Metadata
const meta = new TextEncoder().encode(JSON.stringify({ts: Date.now()}));
encoder.encode(meta, mux.STREAM_SIDE_CHANNEL);

const muxed = encoder.finalize();
```

## Files

```
wasm/
├── Makefile              ← Main build system
├── README.md             ← Full documentation
├── QUICKSTART.md         ← This file
├── muxaudio.wrapper.js   ← JavaScript API
├── test.html             ← Browser test
└── build/                ← Output (created by make)
    ├── muxaudio.js
    └── muxaudio.wasm
```

## Codec Availability

| Codec | Status | Default | Size Impact |
|-------|--------|---------|-------------|
| PCM | ✅ Built-in | ✓ | +30KB |
| Vorbis | ✅ Auto-download | ✓ | +150KB |
| Opus | ✅ Auto-download | ✗ | +150KB |
| FLAC | ✅ Auto-download | ✗ | +200KB |
| MP3 | ✅ Auto-download (decode only) | ✗ | +200KB |

**CMake automatically downloads and builds all codecs!** No manual setup needed.

**Note:** MP3 decoding works via mpg123. Encoding requires LAME (not available).

## Troubleshooting

**"emcc not found"**
```bash
source /path/to/emsdk/emsdk_env.sh
```

**Build fails with download errors**
```bash
# Clear cache and retry
rm -rf build/_deps
emcmake cmake ..
make
```

**Build too slow**
```bash
make -j8  # Parallel build (8 jobs)
```

**WASM too large**
```bash
emcmake cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel
make
```

## Performance

Typical encode speed (Chrome, M1 Mac):
- PCM: 500x real-time
- Vorbis: 20x real-time
- Opus: 30x real-time
- FLAC: 10x real-time

Memory per instance:
- ~100KB (PCM)
- ~500KB (Vorbis)
- ~300KB (Opus)
- ~1MB (FLAC)

## Next Steps

- Read [README.md](README.md) for full documentation
- Check [WASM_STRATEGY.md](WASM_STRATEGY.md) for architecture details
- See `test.html` for code examples
