# MuxAudio Build Variants

MuxAudio supports multiple build configurations to suit different deployment scenarios.

## Build Targets

### 1. Shared Library (Dynamic Linking)
- **Output**: `libmuxaudio.so` (Linux), `muxaudio.dll` (Windows)
- **Codec Dependencies**: External shared libraries/DLLs required at runtime
- **Use Case**: Applications that can distribute codec libraries separately
- **CMake Options**: `-DBUILD_SHARED=ON -DBUILD_STATIC_FULL=OFF`

**Advantages:**
- Smaller library file size
- Codec libraries can be updated independently
- Multiple applications can share codec libraries

**Disadvantages:**
- Requires distributing codec DLLs/SOs alongside your application
- Runtime dependencies on codec libraries

### 2. Fully Static Library
- **Output**: `libmuxaudio-static.a` (Linux), `muxaudio-static.lib` (Windows)
- **Codec Dependencies**: All codec code embedded (no runtime dependencies)
- **Use Case**: Applications requiring self-contained deployment
- **CMake Options**: `-DBUILD_SHARED=OFF -DBUILD_STATIC_FULL=ON`

**Advantages:**
- No runtime codec dependencies
- Self-contained deployment
- Easier redistribution

**Disadvantages:**
- Larger executable size
- Must link all codec static libraries when building your application
- Codec updates require rebuilding your application

### 3. WASM (WebAssembly)
- **Output**: `muxaudio.js`, `muxaudio.wasm`
- **Codec Dependencies**: All codecs bundled and compiled from source
- **Use Case**: Browser-based audio processing
- **CMake Options**: Use `emcmake` (automatically sets fully static mode)

**Characteristics:**
- Decoder-only (LAME not available for WASM)
- Fully static with all codecs embedded
- Codecs: PCM, Vorbis, Opus, FLAC, MP3 (decode only)

## Building

### Linux - Shared Library
```bash
cmake -B build -S . \
  -DBUILD_ALL_CODECS=ON \
  -DBUILD_SHARED=ON \
  -DBUILD_STATIC_FULL=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux - Static Library
```bash
cmake -B build -S . \
  -DBUILD_ALL_CODECS=ON \
  -DBUILD_SHARED=OFF \
  -DBUILD_STATIC_FULL=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Windows - Shared Library (vcpkg)
```powershell
cmake -B build -S . `
  -DBUILD_ALL_CODECS=ON `
  -DBUILD_SHARED=ON `
  -DBUILD_STATIC_FULL=OFF `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

### Windows - Static Library (vcpkg)
```powershell
cmake -B build -S . `
  -DBUILD_ALL_CODECS=ON `
  -DBUILD_SHARED=OFF `
  -DBUILD_STATIC_FULL=ON `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

### WASM
```bash
emcmake cmake -B build -S . \
  -DBUILD_ALL_CODECS=ON \
  -DBUILD_ENCODERS=OFF \
  -DBUILD_DECODERS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Linking Against MuxAudio

### Shared Library

**Linux:**
```bash
gcc myapp.c -o myapp -lmuxaudio
```

**Windows:**
```cmd
cl myapp.c muxaudio.lib
```

Make sure muxaudio.dll and codec DLLs are in your PATH or application directory.

### Static Library

**Linux:**
```bash
gcc myapp.c -o myapp \
  -L. -lmuxaudio-static \
  -lvorbisenc -lvorbis -lopus -lFLAC -lmp3lame -lmpg123 -lfdk-aac -logg \
  -lm
```

**CMake:**
```cmake
target_link_libraries(myapp
  muxaudio-static
  vorbisenc vorbis opus FLAC mp3lame mpg123 fdk-aac ogg m
)
```

**Windows:**
```cmd
cl myapp.c muxaudio-static.lib vorbisenc.lib vorbis.lib opus.lib FLAC.lib ^
  mpg123.lib fdk-aac.lib ogg.lib
```

## Release Packages

The release workflow creates the following packages:

1. **muxaudio-linux-x64-shared.tar.gz**
   - Shared library + codec .so files + CLI tools

2. **muxaudio-linux-x64-static.tar.gz**
   - Static library + codec .a files + README

3. **muxaudio-windows-x64-shared.zip**
   - DLL + import library + codec DLLs

4. **muxaudio-windows-x64-static.zip**
   - Static library + codec .lib files + README

5. **muxaudio-wasm-decoder.tar.gz**
   - WASM module (decoder-only) with embedded codecs

## Notes

- Static libraries on Linux/Windows do not actually "embed" codec code in the .a/.lib file
- They still require linking against codec static libraries when building your application
- The advantage is no runtime DLL/SO dependencies - everything is compiled into your executable
- For WASM, codecs are truly embedded into the .wasm module (fully self-contained)
