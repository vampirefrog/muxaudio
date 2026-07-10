# FFmpeg/libavformat Integration Plan

## Overview

This plan outlines integration of FFmpeg's libavformat and libavcodec into muxaudio to add:
- Container format support (M4A, WebM, WAV, OGG containers)
- Additional codecs (G.722, G.726, Speex, iLBC)
- Unified codec backend option

## Recommended Approach: FFmpeg as Secondary Backend

Add FFmpeg-based implementations alongside existing native codecs rather than replacing them.

**Benefits:**
- Non-breaking: existing codecs continue to work
- Selective usage: FFmpeg only for new codecs or containers
- Smaller WASM size possible with minimal FFmpeg build
- Gradual migration path

## New Codecs via FFmpeg

| Codec | FFmpeg ID | Sample Rate | Use Case |
|-------|-----------|-------------|----------|
| G.722 | AV_CODEC_ID_ADPCM_G722 | 16000 Hz | HD telephony, VoIP |
| G.726 | AV_CODEC_ID_ADPCM_G726 | 8000 Hz | DECT, VoIP |
| GSM-FR | AV_CODEC_ID_GSM | 8000 Hz | Legacy GSM |
| Speex | AV_CODEC_ID_SPEEX | 8/16/32kHz | VoIP (legacy) |
| iLBC | AV_CODEC_ID_ILBC | 8000 Hz | WebRTC fallback |

## Container Format Support

| Container | Extension | Codecs | Notes |
|-----------|-----------|--------|-------|
| M4A/MP4 | .m4a | AAC, ALAC | Apple ecosystem, streaming |
| WebM | .webm | Opus, Vorbis | Web standard |
| OGG | .ogg | Vorbis, Opus, FLAC | Current internal use |
| WAV | .wav | PCM, ALAW, MULAW | Uncompressed |
| FLAC | .flac | FLAC | Lossless container |
| MKA | .mka | All | Universal container |

## API Design

### New Container Enum

```c
enum mux_container_type {
    MUX_CONTAINER_NONE = 0,    /* Raw codec output (current behavior) */
    MUX_CONTAINER_OGG,
    MUX_CONTAINER_M4A,
    MUX_CONTAINER_WEBM,
    MUX_CONTAINER_WAV,
    MUX_CONTAINER_MKA,
    MUX_CONTAINER_FLAC,
    MUX_CONTAINER_MAX
};
```

### Extended Creation Functions

```c
/* New extended creation with container support */
struct mux_encoder *mux_encoder_new_ex(
    enum mux_codec_type codec_type,
    enum mux_container_type container_type,
    int sample_rate,
    int num_channels,
    int num_streams,
    const struct mux_param *params,
    int num_params);

/* Existing functions remain backward-compatible (CONTAINER_NONE) */
```

## Build System Changes

### New CMake Options

```cmake
# FFmpeg options
option(CODEC_FFMPEG "Enable FFmpeg-based codecs" OFF)
option(CODEC_G722 "Enable G.722 codec (requires FFmpeg)" OFF)
option(CODEC_G726 "Enable G.726 codec (requires FFmpeg)" OFF)
option(CONTAINER_SUPPORT "Enable container format support" OFF)

# WASM-specific
option(WASM_FFMPEG_MINIMAL "Build minimal FFmpeg for WASM" OFF)
```

### FFmpeg Detection

```cmake
if(CODEC_FFMPEG OR CONTAINER_SUPPORT)
    if(IS_WASM)
        # Build minimal FFmpeg from source
        # Only enable required codecs/formats
    else()
        # Use system FFmpeg via pkg-config
        pkg_check_modules(AVFORMAT libavformat)
        pkg_check_modules(AVCODEC libavcodec)
        pkg_check_modules(AVUTIL libavutil)
    endif()
endif()
```

## WASM Considerations

### Minimal FFmpeg Build

For WASM, build FFmpeg with minimal features:

```bash
emconfigure ./configure \
    --disable-everything \
    --disable-programs \
    --disable-doc \
    --disable-network \
    --enable-small \
    --enable-decoder=adpcm_g722,adpcm_g726,pcm_s16le \
    --enable-encoder=adpcm_g722,adpcm_g726,pcm_s16le \
    --enable-demuxer=wav,ogg \
    --enable-muxer=wav,ogg \
    --cc=emcc
```

### Estimated WASM Sizes

| Configuration | Size |
|---------------|------|
| Current (no FFmpeg) | ~800KB |
| + Minimal FFmpeg (G.722) | ~1MB |
| + Container support | ~1.2MB |
| Full FFmpeg | ~5MB+ (avoid) |

### Alternative: Native-Only FFmpeg

```cmake
if(IS_WASM)
    # Skip FFmpeg for WASM (too large)
    set(CODEC_FFMPEG OFF)
else()
    # Enable FFmpeg for native builds
    option(CODEC_FFMPEG "Enable FFmpeg" ON)
endif()
```

## Implementation Phases

### Phase 1: Infrastructure (Files to create)

- `src/ffmpeg_common.h` - Shared FFmpeg utilities
- `src/ffmpeg_common.c` - Helper functions (format conversion, error handling)
- Update `CMakeLists.txt` - FFmpeg detection

### Phase 2: G.722 Codec

- `src/codec_g722.c` - G.722 implementation using FFmpeg
- `tests/test_g722.c` - Tests
- Update `include/mux.h` - Add MUX_CODEC_G722
- Update `src/core.c` - Register codec

### Phase 3: Container Support

- `src/container_ffmpeg.c` - Container muxing/demuxing
- Update `include/mux.h` - Add container API
- Update `src/mux_internal.h` - Add container_ops vtable
- Update `src/core.c` - Container integration

### Phase 4: Additional Codecs

- `src/codec_g726.c` - G.726 codec
- `src/codec_gsm.c` - GSM-FR codec (optional)
- Additional tests

### Phase 5: WASM Integration

- Configure minimal FFmpeg build for Emscripten
- Test WASM builds
- Optimize bundle size

## File Structure After Integration

```
src/
├── codec_pcm.c          # Existing
├── codec_vorbis.c       # Existing
├── codec_opus.c         # Existing
├── codec_flac.c         # Existing
├── codec_mp3.c          # Existing
├── codec_aac.c          # Existing
├── codec_alaw.c         # Existing (G.711)
├── codec_mulaw.c        # Existing (G.711)
├── codec_amr.c          # Existing
├── codec_amr_wb.c       # Existing
├── ffmpeg_common.h      # NEW: FFmpeg utilities
├── ffmpeg_common.c      # NEW: FFmpeg helpers
├── codec_g722.c         # NEW: G.722 via FFmpeg
├── codec_g726.c         # NEW: G.726 via FFmpeg
├── container_ffmpeg.c   # NEW: Container support
├── core.c
├── buffer.c
├── error.c
└── mux_leb128.c
```

## Dependencies

### Native Build
```bash
# Debian/Ubuntu
sudo apt install libavformat-dev libavcodec-dev libavutil-dev

# Or with all FFmpeg libs
sudo apt install ffmpeg libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

### vcpkg (Windows)
```
vcpkg install ffmpeg:x64-windows
```

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| WASM size bloat | Make FFmpeg optional, use minimal build |
| API breaking changes | Add `_ex` functions, keep existing API |
| FFmpeg version issues | Test with FFmpeg 4.x, 5.x, 6.x |
| License concerns | Document LGPL requirements |

## Success Criteria

1. G.722 codec encodes/decodes correctly
2. M4A container produces playable files
3. All existing tests pass
4. WASM size increase < 500KB (with minimal FFmpeg)
5. No breaking changes to existing API

## Next Steps

1. Review and approve this plan
2. Start with Phase 1: Add FFmpeg detection to CMakeLists.txt
3. Implement G.722 codec as proof of concept
4. Iterate based on results
