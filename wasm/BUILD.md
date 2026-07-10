# Building MuxAudio for WebAssembly

Simple CMake-based build using Emscripten.

## Codec Availability

**Working automatically via CMake FetchContent:**
- ✅ PCM (built-in)
- ✅ Vorbis (auto-download from GitHub)
- ✅ Opus (auto-download from GitHub)
- ✅ FLAC (auto-download from GitHub)
- ✅ MP3 decode (auto-download mpg123 from GitHub)

**Not available:**
- ❌ MP3 encode (LAME doesn't have CMake build)

**CMake automatically downloads, configures, and builds all codec dependencies!**

## Prerequisites

### Install Emscripten

```bash
# Clone the Emscripten SDK
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk

# Install and activate latest version
./emsdk install latest
./emsdk activate latest

# Add to your shell (bash/zsh)
source ./emsdk_env.sh
```

Add to your `~/.bashrc` or `~/.zshrc`:
```bash
source /path/to/emsdk/emsdk_env.sh
```

Verify installation:
```bash
emcc --version
# Should show: emcc (Emscripten...) 3.x.x
```

## Quick Build

### Default Build (PCM + Vorbis, ~200KB)

```bash
cd wasm
mkdir build && cd build
emcmake cmake ..
make
```

Output: `muxaudio.js` and `muxaudio.wasm`

**This just works - no additional dependencies needed!**

### Minimal Build (PCM only, ~50KB)

```bash
emcmake cmake .. -DWASM_CODEC_VORBIS=OFF
make
```

### Custom Codec Selection

```bash
emcmake cmake .. \
  -DWASM_CODEC_VORBIS=ON \
  -DWASM_CODEC_OPUS=ON \
  -DWASM_CODEC_FLAC=ON
make
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `WASM_CODEC_PCM` | ON | PCM codec (always enabled) |
| `WASM_CODEC_VORBIS` | ON | Vorbis (Emscripten port) |
| `WASM_CODEC_OPUS` | OFF | Opus (auto-download) |
| `WASM_CODEC_FLAC` | OFF | FLAC (auto-download) |
| `WASM_CODEC_MP3` | OFF | MP3/LAME (auto-download, encode only) |
| `WASM_BUILD_ALL` | OFF | Enable all codecs |
| `WASM_OUTPUT_NAME` | muxaudio | Output filename |

### Examples

**Minimal build (PCM only, ~50KB):**
```bash
emcmake cmake .. -DWASM_CODEC_VORBIS=OFF
make
```

**Opus + FLAC:**
```bash
emcmake cmake .. \
  -DWASM_CODEC_OPUS=ON \
  -DWASM_CODEC_FLAC=ON
make
```

**Debug build:**
```bash
emcmake cmake .. -DCMAKE_BUILD_TYPE=Debug
make
```

**Custom output name:**
```bash
emcmake cmake .. -DWASM_OUTPUT_NAME=myaudio
make
# Creates myaudio.js and myaudio.wasm
```

## How It Works

### Automatic Dependency Management

CMake automatically:
1. Downloads codec libraries from official sources (GitHub, SourceForge)
2. Configures them for WebAssembly compilation
3. Builds them as static libraries
4. Links everything into a single WASM module

**No manual downloads or builds required!**

### Codec Sources

- **Vorbis**: Emscripten port (built-in, instant)
- **Opus**: xiph/opus (GitHub, v1.4)
- **FLAC**: xiph/flac (GitHub, v1.4.3)
- **MP3**: LAME 3.100 (SourceForge)

Downloaded sources are cached in `build/_deps/` for reuse.

## Build Process Details

### First Build (with dependencies)

```bash
emcmake cmake .. -DWASM_CODEC_OPUS=ON
make
```

Steps:
1. CMake fetches Opus source from GitHub (~3MB)
2. Configures Opus for WASM compilation
3. Compiles Opus library (~30 seconds)
4. Compiles muxaudio sources
5. Links everything into WASM module

Total time: ~1-2 minutes (one-time)

### Subsequent Builds (incremental)

```bash
# Edit source code
make
```

Only recompiles changed files (~5 seconds).

Dependencies are cached, not re-downloaded.

### Clean Builds

```bash
# Clean build artifacts (keep dependencies)
make clean

# Clean everything including downloaded dependencies
rm -rf build
```

## Testing

### Browser Test

```bash
# In wasm/build directory
python3 -m http.server 8000

# Copy wrapper
cp ../muxaudio.wrapper.js .
cp ../test.html .

# Open browser
open http://localhost:8000/test.html
```

### Node.js Test

```javascript
// test.mjs
import { createMuxAudio } from './build/muxaudio.js';

const mux = await createMuxAudio();
const encoder = mux.Encoder('vorbis', 44100, 2, { bitrate: 128 });

// Generate test audio
const samples = new Int16Array(44100 * 2);
for (let i = 0; i < 44100; i++) {
  const val = Math.sin(2 * Math.PI * 440 * i / 44100) * 16384;
  samples[i*2] = samples[i*2+1] = val;
}

encoder.encode(samples);
const encoded = encoder.finalize();
console.log(`Encoded: ${encoded.length} bytes`);

encoder.destroy();
```

Run: `node test.mjs`

## Bundle Size

| Configuration | WASM | JS | Total | Gzipped |
|--------------|------|----|----|---------|
| PCM only | 30KB | 20KB | 50KB | 20KB |
| PCM + Vorbis (default) | 180KB | 20KB | 200KB | 70KB |
| + Opus | 350KB | 20KB | 370KB | 130KB |
| + FLAC | 550KB | 20KB | 570KB | 190KB |
| + MP3 | 700KB | 20KB | 720KB | 240KB |
| All codecs | 800KB | 25KB | 825KB | 280KB |

## Optimization

### Size Optimization

```bash
emcmake cmake .. -DCMAKE_BUILD_TYPE=MinSizeRel
make
```

Creates smaller WASM (~10-20% reduction).

### Speed Optimization

```bash
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Default is already `-O3` (maximum speed).

## Troubleshooting

### "emcc: command not found"

```bash
source /path/to/emsdk/emsdk_env.sh
```

Or add to `~/.bashrc`/`~/.zshrc`.

### CMake version too old

Requires CMake 3.16+. Update:

```bash
# Ubuntu/Debian
sudo apt-add-repository ppa:ubuntu-toolchain-r/test
sudo apt update
sudo apt install cmake

# macOS
brew install cmake

# Or use Emscripten's bundled cmake
emcmake --version
```

### Git clone failures

If behind a firewall/proxy:

```bash
# Use HTTPS instead of git://
git config --global url."https://".insteadOf git://
```

### FetchContent download errors

CMake caches downloads in `build/_deps/`. If corrupted:

```bash
rm -rf build/_deps
emcmake cmake ..
make
```

### Build failures

Enable verbose output:

```bash
make VERBOSE=1
```

Check compiler errors and ensure Emscripten is activated:

```bash
which emcc
# Should show: /path/to/emsdk/upstream/emscripten/emcc
```

## Advanced Usage

### Custom Compiler Flags

```bash
emcmake cmake .. -DCMAKE_C_FLAGS="-DCUSTOM_DEFINE=1"
```

### Additional Emscripten Flags

Edit `CMakeLists.txt` and add to `EMSCRIPTEN_LINK_FLAGS`:

```cmake
list(APPEND EMSCRIPTEN_LINK_FLAGS
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=5MB
)
```

### Use System Libraries (if available)

```bash
# Not recommended for WASM, but possible:
emcmake cmake .. -DCMAKE_FIND_ROOT_PATH=/path/to/wasm/libs
```

## CI/CD Integration

### GitHub Actions

```yaml
name: Build WASM
on: [push]
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - uses: mymindstorm/setup-emsdk@v12
        with:
          version: latest
      - name: Build
        run: |
          cd wasm
          mkdir build && cd build
          emcmake cmake .. -DWASM_BUILD_ALL=ON
          make
      - uses: actions/upload-artifact@v3
        with:
          name: wasm-build
          path: wasm/build/*.{js,wasm}
```

### Docker

```dockerfile
FROM emscripten/emsdk:latest

WORKDIR /src
COPY . .

RUN cd wasm && \
    mkdir build && cd build && \
    emcmake cmake .. -DWASM_BUILD_ALL=ON && \
    make

# Output in /src/wasm/build/
```

## Next Steps

- See [README.md](README.md) for JavaScript API usage
- Check [QUICKSTART.md](QUICKSTART.md) for quick examples
- Read [WASM_STRATEGY.md](../WASM_STRATEGY.md) for architecture details
