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
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

### Windows - Self-contained DLL (codecs linked in, vcpkg)
Build a shared library against the static-triplet codecs so all codec code is
linked into `muxaudio.dll` (no external codec DLLs needed):
```powershell
cmake -B build -S . `
  -DBUILD_ALL_CODECS=ON `
  -DBUILD_SHARED=ON `
  -DBUILD_STATIC_FULL=OFF `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

For **32-bit** builds, add `-A Win32` and use the `x86-windows` /
`x86-windows-static` triplets (default platform is x64).

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

The release workflow creates the following packages. Every archive includes the
public header `mux.h` and a `README.txt`.

**Linux**

1. **muxaudio-linux-x64-shared.tar.gz** - shared library + codec .so files + CLI tools
2. **muxaudio-linux-x64-static.tar.gz** - static library + codec .a files

**Windows** - three linkage variants, each in **x64** and **x86**:

3. **muxaudio-windows-{x64,x86}-shared.zip**
   - `muxaudio.dll` + import `muxaudio.lib`, with external codec DLLs alongside
4. **muxaudio-windows-{x64,x86}-shared-bundled.zip**
   - Self-contained `muxaudio.dll` (codecs statically linked in, static CRT) +
     import `muxaudio.lib`; **no external codec DLLs required**
5. **muxaudio-windows-{x64,x86}-static.zip**
   - `muxaudio-static.lib` + codec `.lib` files

**WASM**

6. **muxaudio-wasm-decoder.tar.gz** - WASM module (decoder-only) with embedded codecs

**Debian** (from the `.deb` build) already ships both linkages with external
system codec libraries: `libmuxaudio0` (shared/dynamic) and `libmuxaudio-static.a`
inside `libmuxaudio-dev` (static), alongside `muxaudio-tools`.

## Notes

- The **self-contained Windows DLL** (`shared-bundled`) links the codec static
  libraries into `muxaudio.dll` itself, so consumers ship a single DLL. It uses
  the static CRT (`x64-windows-static` / `x86-windows-static` triplets with
  `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`).
- The plain **static** libraries on Linux/Windows do not "embed" codec code in
  the .a/.lib file - they still require linking the codec static libraries when
  building your application. The advantage is no runtime DLL/SO dependencies.
- For WASM, codecs are truly embedded into the .wasm module (fully self-contained).
