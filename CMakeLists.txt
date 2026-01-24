# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(muxaudio VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Detect build type
if(EMSCRIPTEN)
    set(IS_WASM TRUE)
    message(STATUS "Building for WebAssembly with Emscripten")
else()
    set(IS_WASM FALSE)
    message(STATUS "Building for native platform")
endif()

# Build options
if(NOT IS_WASM)
    option(BUILD_TOOLS "Build command-line tools" ON)
    option(BUILD_TESTS "Build tests" ON)
    option(BUILD_SHARED "Build shared library" ON)
    option(BUILD_STATIC_FULL "Build fully static library with embedded codecs" ON)
endif()

# Encoder/Decoder options
option(BUILD_ENCODERS "Build encoder functionality" ON)
option(BUILD_DECODERS "Build decoder functionality" ON)

# Codec options
option(CODEC_PCM "Include PCM codec (always on)" ON)
option(CODEC_VORBIS "Include Vorbis codec" ON)
option(CODEC_OPUS "Include Opus codec" OFF)
option(CODEC_FLAC "Include FLAC codec" OFF)
option(CODEC_MP3 "Include MP3 codec" OFF)
option(CODEC_AAC "Include AAC codec (native only)" OFF)

# MP3 sub-options (both default to ON when MP3 is enabled)
option(CODEC_MP3_DECODE "Enable MP3 decoding" ON)
option(CODEC_MP3_ENCODE "Enable MP3 encoding" ON)

# Build all codecs
option(BUILD_ALL_CODECS "Build all available codecs" OFF)
if(BUILD_ALL_CODECS)
    set(CODEC_VORBIS ON)
    set(CODEC_OPUS ON)
    set(CODEC_FLAC ON)
    set(CODEC_MP3 ON)
    if(NOT IS_WASM)
        set(CODEC_AAC ON)
    endif()
endif()

# WASM-specific options
if(IS_WASM)
    set(WASM_OUTPUT_NAME "muxaudio" CACHE STRING "WASM output filename (without extension)")
endif()

# Compiler flags
if(IS_WASM)
    add_compile_options(
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
elseif(NOT MSVC)
    add_compile_options(
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
        -Wstrict-prototypes
        -Wmissing-prototypes
    )
endif()

# Core sources (always included)
set(MUXAUDIO_SOURCES
    src/core.c
    src/buffer.c
    src/error.c
    src/mux_leb128.c
    src/codec_pcm.c
    src/codec_alaw.c
    src/codec_mulaw.c
)

set(MUXAUDIO_DEFINES "")
set(MUXAUDIO_LIBRARIES "")
set(MUXAUDIO_INCLUDES "")

# ==============================================================================
# Dependency Management
# ==============================================================================

if(IS_WASM)
    # WASM: Bundle codec libraries from source using FetchContent
    include(FetchContent)

    # Helper: Ensure Ogg is available (shared by Vorbis, Opus, FLAC)
    macro(ensure_ogg)
        if(NOT TARGET ogg)
            message(STATUS "Fetching libogg...")
            FetchContent_Declare(
                ogg
                GIT_REPOSITORY https://github.com/xiph/ogg.git
                GIT_TAG v1.3.5
                GIT_SHALLOW TRUE
            )
            set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(ogg)

            # Set variables that FindOgg.cmake looks for
            FetchContent_GetProperties(ogg SOURCE_DIR OGG_SOURCE_DIR)
            set(OGG_INCLUDE_DIR "${OGG_SOURCE_DIR}/include" CACHE PATH "")
            set(OGG_LIBRARY ogg CACHE STRING "")
            set(Ogg_FOUND TRUE CACHE BOOL "")
        endif()
    endmacro()
else()
    # Native: Use system-installed codec libraries
    # Prefer vcpkg's find_package, fallback to pkg-config

    # Determine if we need static libraries
    set(NEED_STATIC_CODECS FALSE)
    if(BUILD_STATIC_FULL AND NOT BUILD_SHARED)
        set(NEED_STATIC_CODECS TRUE)
    endif()

    # Try vcpkg-style find_package first (Windows with vcpkg)
    if(DEFINED ENV{VCPKG_ROOT} OR DEFINED VCPKG_TARGET_TRIPLET)
        message(STATUS "Using vcpkg for dependencies")

        find_package(Ogg CONFIG QUIET)
        find_package(Vorbis CONFIG QUIET)
        find_package(Opus CONFIG QUIET)
        find_package(FLAC CONFIG QUIET)
        find_package(mpg123 CONFIG QUIET)
        find_package(fdk-aac CONFIG QUIET)

        # Map vcpkg targets to our variable names
        if(TARGET Ogg::ogg)
            set(OGG_FOUND TRUE)
            set(OGG_LIBRARIES Ogg::ogg)
            set(OGG_INCLUDE_DIRS "")
        endif()

        if(TARGET Vorbis::vorbisenc AND TARGET Vorbis::vorbis)
            set(VORBIS_FOUND TRUE)
            set(VORBIS_LIBRARIES Vorbis::vorbisenc Vorbis::vorbis Ogg::ogg)
            set(VORBIS_INCLUDE_DIRS "")
        endif()

        if(TARGET Opus::opus)
            set(OPUS_FOUND TRUE)
            set(OPUS_LIBRARIES Opus::opus)
            set(OPUS_INCLUDE_DIRS "")
        endif()

        if(TARGET FLAC::FLAC)
            set(FLAC_FOUND TRUE)
            set(FLAC_LIBRARIES FLAC::FLAC)
            set(FLAC_INCLUDE_DIRS "")
        endif()

        if(TARGET MPG123::libmpg123)
            set(MPG123_FOUND TRUE)
            set(MPG123_LIBRARIES MPG123::libmpg123)
            set(MPG123_INCLUDE_DIRS "")
        endif()

        # fdk-aac target name varies
        if(TARGET fdk-aac)
            set(FDK_AAC_FOUND TRUE)
            set(FDK_AAC_LIBRARIES fdk-aac)
            set(FDK_AAC_INCLUDE_DIRS "")
        elseif(TARGET FDK-AAC::fdk-aac)
            set(FDK_AAC_FOUND TRUE)
            set(FDK_AAC_LIBRARIES FDK-AAC::fdk-aac)
            set(FDK_AAC_INCLUDE_DIRS "")
        endif()

        # LAME doesn't have vcpkg package, try find_library
        find_library(LAME_LIBRARY mp3lame)
        if(LAME_LIBRARY)
            set(LAME_FOUND TRUE)
            set(LAME_LIBRARIES ${LAME_LIBRARY})
        endif()
    else()
        # Linux/Unix: Use pkg-config
        find_package(PkgConfig)

        if(PKG_CONFIG_FOUND)
            # Prefer pkg-config for better include path handling
            pkg_check_modules(OGG ogg)
            pkg_check_modules(VORBIS vorbis vorbisenc)
            pkg_check_modules(OPUS opus)
            pkg_check_modules(FLAC flac)
            pkg_check_modules(MPG123 libmpg123)
            pkg_check_modules(FDK_AAC fdk-aac)

            # LAME doesn't always have pkg-config, fallback to find_library
            pkg_check_modules(LAME lame)
            if(NOT LAME_FOUND)
                find_library(LAME_LIBRARY mp3lame)
                if(LAME_LIBRARY)
                    set(LAME_FOUND TRUE)
                    set(LAME_LIBRARIES ${LAME_LIBRARY})
                endif()
            endif()
        else()
            # Fallback to find_library if pkg-config not available
            find_library(OGG_LIBRARY ogg)
            find_library(VORBIS_LIBRARY vorbis)
            find_library(VORBISENC_LIBRARY vorbisenc)
            find_library(OPUS_LIBRARY opus)
            find_library(FLAC_LIBRARY FLAC)
            find_library(MPG123_LIBRARY mpg123)
            find_library(LAME_LIBRARY mp3lame)
            find_library(FDK_AAC_LIBRARY fdk-aac)

            if(OGG_LIBRARY)
                set(OGG_FOUND TRUE)
                set(OGG_LIBRARIES ${OGG_LIBRARY})
            endif()
            if(VORBIS_LIBRARY AND VORBISENC_LIBRARY)
                set(VORBIS_FOUND TRUE)
                set(VORBIS_LIBRARIES ${VORBIS_LIBRARY} ${VORBISENC_LIBRARY})
            endif()
            if(OPUS_LIBRARY)
                set(OPUS_FOUND TRUE)
                set(OPUS_LIBRARIES ${OPUS_LIBRARY})
            endif()
            if(FLAC_LIBRARY)
                set(FLAC_FOUND TRUE)
                set(FLAC_LIBRARIES ${FLAC_LIBRARY})
            endif()
            if(MPG123_LIBRARY)
                set(MPG123_FOUND TRUE)
                set(MPG123_LIBRARIES ${MPG123_LIBRARY})
            endif()
            if(LAME_LIBRARY)
                set(LAME_FOUND TRUE)
                set(LAME_LIBRARIES ${LAME_LIBRARY})
            endif()
            if(FDK_AAC_LIBRARY)
                set(FDK_AAC_FOUND TRUE)
                set(FDK_AAC_LIBRARIES ${FDK_AAC_LIBRARY})
            endif()
        endif()
    endif()
endif()

# ==============================================================================
# Vorbis Codec
# ==============================================================================
if(CODEC_VORBIS)
    if(IS_WASM)
        message(STATUS "Enabling Vorbis codec (bundled from source)")
        ensure_ogg()

        FetchContent_Declare(
            vorbis
            GIT_REPOSITORY https://github.com/xiph/vorbis.git
            GIT_TAG master
            GIT_SHALLOW TRUE
        )
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(vorbis)

        list(APPEND MUXAUDIO_SOURCES src/codec_vorbis.c)
        list(APPEND MUXAUDIO_DEFINES -DHAVE_VORBIS)
        list(APPEND MUXAUDIO_LIBRARIES vorbis vorbisenc ogg)
    else()
        if(OGG_FOUND AND VORBIS_FOUND)
            message(STATUS "Enabling Vorbis codec (system libraries)")
            list(APPEND MUXAUDIO_SOURCES src/codec_vorbis.c)
            list(APPEND MUXAUDIO_DEFINES -DHAVE_VORBIS)
            list(APPEND MUXAUDIO_LIBRARIES ${VORBIS_LIBRARIES} ${OGG_LIBRARIES})
            if(VORBIS_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${VORBIS_INCLUDE_DIRS})
            endif()
            if(OGG_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${OGG_INCLUDE_DIRS})
            endif()
        else()
            message(WARNING "Vorbis requested but libogg/libvorbis not found")
        endif()
    endif()
endif()

# ==============================================================================
# Opus Codec
# ==============================================================================
if(CODEC_OPUS)
    if(IS_WASM)
        message(STATUS "Enabling Opus codec (bundled from source)")
        ensure_ogg()

        FetchContent_Declare(
            opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG v1.5.2
            GIT_SHALLOW TRUE
        )
        set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(opus)

        list(APPEND MUXAUDIO_SOURCES src/codec_opus.c)
        list(APPEND MUXAUDIO_DEFINES -DHAVE_OPUS)
        # Link to opus target - it will provide include directories automatically
        list(APPEND MUXAUDIO_LIBRARIES opus ogg)
    else()
        if(OGG_FOUND AND OPUS_FOUND)
            message(STATUS "Enabling Opus codec (system libraries)")
            list(APPEND MUXAUDIO_SOURCES src/codec_opus.c)
            list(APPEND MUXAUDIO_DEFINES -DHAVE_OPUS)
            list(APPEND MUXAUDIO_LIBRARIES ${OPUS_LIBRARIES} ${OGG_LIBRARIES})
            if(OPUS_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${OPUS_INCLUDE_DIRS})
            endif()
            if(OGG_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${OGG_INCLUDE_DIRS})
            endif()
        else()
            message(WARNING "Opus requested but libogg/libopus not found")
        endif()
    endif()
endif()

# ==============================================================================
# FLAC Codec
# ==============================================================================
if(CODEC_FLAC)
    if(IS_WASM)
        message(STATUS "Enabling FLAC codec (bundled from source)")
        ensure_ogg()

        FetchContent_Declare(
            flac
            GIT_REPOSITORY https://github.com/xiph/flac.git
            GIT_TAG 1.5.0
            GIT_SHALLOW TRUE
        )
        set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
        set(WITH_OGG ON CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(flac)

        list(APPEND MUXAUDIO_SOURCES src/codec_flac.c)
        list(APPEND MUXAUDIO_DEFINES -DHAVE_FLAC)
        list(APPEND MUXAUDIO_LIBRARIES FLAC ogg)
    else()
        if(OGG_FOUND AND FLAC_FOUND)
            message(STATUS "Enabling FLAC codec (system libraries)")
            list(APPEND MUXAUDIO_SOURCES src/codec_flac.c)
            list(APPEND MUXAUDIO_DEFINES -DHAVE_FLAC)
            list(APPEND MUXAUDIO_LIBRARIES ${FLAC_LIBRARIES} ${OGG_LIBRARIES})
            if(FLAC_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${FLAC_INCLUDE_DIRS})
            endif()
            if(OGG_INCLUDE_DIRS)
                list(APPEND MUXAUDIO_INCLUDES ${OGG_INCLUDE_DIRS})
            endif()
        else()
            message(WARNING "FLAC requested but libogg/libFLAC not found")
        endif()
    endif()
endif()

# ==============================================================================
# MP3 Codec
# ==============================================================================
if(CODEC_MP3)
    set(MP3_ENABLED FALSE)

    # MP3 Decoding
    if(CODEC_MP3_DECODE)
        if(IS_WASM)
            message(STATUS "Enabling MP3 decoding (bundled mpg123 from SourceForge)")

            # mpg123 uses autotools, so we use ExternalProject to build it
            include(ExternalProject)

            ExternalProject_Add(
                mpg123_external
                URL https://downloads.sourceforge.net/project/mpg123/mpg123/1.32.3/mpg123-1.32.3.tar.bz2
                CONFIGURE_COMMAND emconfigure <SOURCE_DIR>/configure
                    --prefix=<INSTALL_DIR>
                    --disable-shared
                    --enable-static
                    --with-cpu=generic
                BUILD_COMMAND emmake make -j4
                INSTALL_COMMAND emmake make install
                BUILD_IN_SOURCE 0
            )

            ExternalProject_Get_Property(mpg123_external INSTALL_DIR)

            # Create imported library target
            add_library(mpg123 STATIC IMPORTED)
            set_target_properties(mpg123 PROPERTIES
                IMPORTED_LOCATION "${INSTALL_DIR}/lib/libmpg123.a"
            )
            add_dependencies(mpg123 mpg123_external)

            list(APPEND MUXAUDIO_DEFINES -DHAVE_MP3_DECODE)
            list(APPEND MUXAUDIO_LIBRARIES mpg123)
            list(APPEND MUXAUDIO_INCLUDES "${INSTALL_DIR}/include")
            set(MP3_ENABLED TRUE)
        else()
            if(MPG123_FOUND)
                message(STATUS "Enabling MP3 decoding (system mpg123)")
                list(APPEND MUXAUDIO_DEFINES -DHAVE_MP3_DECODE)
                list(APPEND MUXAUDIO_LIBRARIES ${MPG123_LIBRARIES})
                if(MPG123_INCLUDE_DIRS)
                    list(APPEND MUXAUDIO_INCLUDES ${MPG123_INCLUDE_DIRS})
                endif()
                set(MP3_ENABLED TRUE)
            else()
                message(WARNING "MP3 decoding requested but libmpg123 not found")
            endif()
        endif()
    endif()

    # MP3 Encoding
    if(CODEC_MP3_ENCODE)
        if(IS_WASM)
            message(WARNING "MP3 encoding (LAME) not available for WASM builds")
            message(WARNING "LAME does not have CMake build support")
        else()
            if(LAME_FOUND)
                message(STATUS "Enabling MP3 encoding (system LAME)")
                list(APPEND MUXAUDIO_DEFINES -DHAVE_MP3_ENCODE)
                list(APPEND MUXAUDIO_LIBRARIES ${LAME_LIBRARIES})
                if(LAME_INCLUDE_DIRS)
                    list(APPEND MUXAUDIO_INCLUDES ${LAME_INCLUDE_DIRS})
                endif()
                set(MP3_ENABLED TRUE)
            else()
                message(WARNING "MP3 encoding requested but libmp3lame not found")
            endif()
        endif()
    endif()

    # Add codec source if either encode or decode is enabled
    if(MP3_ENABLED)
        list(APPEND MUXAUDIO_SOURCES src/codec_mp3.c)
        list(APPEND MUXAUDIO_DEFINES -DHAVE_MP3)
    endif()
endif()

# ==============================================================================
# AAC Codec (Native only)
# ==============================================================================
if(CODEC_AAC AND NOT IS_WASM)
    if(FDK_AAC_FOUND)
        message(STATUS "Enabling AAC codec (system fdk-aac)")
        list(APPEND MUXAUDIO_SOURCES src/codec_aac.c)
        list(APPEND MUXAUDIO_DEFINES -DHAVE_AAC)
        list(APPEND MUXAUDIO_LIBRARIES ${FDK_AAC_LIBRARIES})
        if(FDK_AAC_INCLUDE_DIRS)
            list(APPEND MUXAUDIO_INCLUDES ${FDK_AAC_INCLUDE_DIRS})
        endif()
    else()
        message(WARNING "AAC requested but libfdk-aac not found")
    endif()
endif()

# ==============================================================================
# Build Targets
# ==============================================================================

if(IS_WASM)
    # ========================================
    # WASM Build: Static library + WASM module
    # ========================================

    # Static library (bundled codecs)
    add_library(muxaudio-static STATIC ${MUXAUDIO_SOURCES})

    target_include_directories(muxaudio-static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${MUXAUDIO_INCLUDES}
    )

    target_compile_definitions(muxaudio-static PRIVATE ${MUXAUDIO_DEFINES})

    if(MUXAUDIO_LIBRARIES)
        target_link_libraries(muxaudio-static PUBLIC ${MUXAUDIO_LIBRARIES})
    endif()

    # WASM module (bundled codecs)
    add_executable(${WASM_OUTPUT_NAME} ${MUXAUDIO_SOURCES})

    target_include_directories(${WASM_OUTPUT_NAME}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${MUXAUDIO_INCLUDES}
    )

    target_compile_definitions(${WASM_OUTPUT_NAME} PRIVATE ${MUXAUDIO_DEFINES})

    if(MUXAUDIO_LIBRARIES)
        target_link_libraries(${WASM_OUTPUT_NAME} PRIVATE ${MUXAUDIO_LIBRARIES})
    endif()

    # Exported functions for JavaScript
    set(EXPORTED_FUNCTIONS
        _malloc
        _free
        _mux_encoder_new
        _mux_encoder_destroy
        _mux_encoder_encode
        _mux_encoder_read
        _mux_encoder_finalize
        _mux_encoder_get_error
        _mux_encoder_clear_error
        _mux_decoder_new
        _mux_decoder_destroy
        _mux_decoder_decode
        _mux_decoder_read
        _mux_decoder_finalize
        _mux_decoder_get_error
        _mux_decoder_clear_error
        _mux_codec_from_name
        _mux_codec_to_name
        _mux_list_codecs
        _mux_get_encoder_params
        _mux_get_decoder_params
        _mux_get_supported_sample_rates
        _mux_error_string
    )

    # Convert list to JSON array format
    string(REPLACE ";" "," EXPORTED_FUNCTIONS_STR "${EXPORTED_FUNCTIONS}")
    set(EXPORTED_FUNCTIONS_JSON "[\\\"${EXPORTED_FUNCTIONS_STR}\\\"]")
    string(REPLACE "," "\\\",\\\"" EXPORTED_FUNCTIONS_JSON "${EXPORTED_FUNCTIONS_JSON}")

    # Emscripten link flags
    set(EMSCRIPTEN_LINK_FLAGS
        -sWASM=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createMuxAudioWasm
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=${EXPORTED_FUNCTIONS_JSON}
        -sEXPORTED_RUNTIME_METHODS=['cwrap','setValue','getValue','UTF8ToString']
        -sENVIRONMENT=web,worker
    )

    # Optimization
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND EMSCRIPTEN_LINK_FLAGS -O0 -g -sASSERTIONS=1)
    else()
        list(APPEND EMSCRIPTEN_LINK_FLAGS -O3)
    endif()

    # Apply link flags
    string(REPLACE ";" " " EMSCRIPTEN_LINK_FLAGS_STR "${EMSCRIPTEN_LINK_FLAGS}")
    set_target_properties(${WASM_OUTPUT_NAME} PROPERTIES
        SUFFIX ".js"
        LINK_FLAGS "${EMSCRIPTEN_LINK_FLAGS_STR}"
    )

    # Print configuration summary
    message(STATUS "")
    message(STATUS "=== MuxAudio WASM Build ===")
    message(STATUS "Targets:")
    message(STATUS "  - muxaudio-static: Static library with bundled codecs")
    message(STATUS "  - ${WASM_OUTPUT_NAME}: WASM module (${WASM_OUTPUT_NAME}.js / ${WASM_OUTPUT_NAME}.wasm)")
    message(STATUS "")
    message(STATUS "Codecs enabled:")
    message(STATUS "  - PCM: ON (built-in)")
    if(CODEC_VORBIS)
        message(STATUS "  - Vorbis: ON (bundled)")
    else()
        message(STATUS "  - Vorbis: OFF")
    endif()
    if(CODEC_OPUS)
        message(STATUS "  - Opus: ON (bundled)")
    else()
        message(STATUS "  - Opus: OFF")
    endif()
    if(CODEC_FLAC)
        message(STATUS "  - FLAC: ON (bundled)")
    else()
        message(STATUS "  - FLAC: OFF")
    endif()
    if(CODEC_MP3_DECODE)
        message(STATUS "  - MP3: ON (decode only, bundled)")
    else()
        message(STATUS "  - MP3: OFF")
    endif()
    message(STATUS "")
    message(STATUS "Dependencies cached in build/_deps/")
    message(STATUS "===========================")
    message(STATUS "")

else()
    # ========================================
    # Native Build: Shared or Fully Static
    # ========================================

    set(INSTALL_TARGETS "")

    # Shared library (external codec DLLs/SOs)
    if(BUILD_SHARED)
        add_library(muxaudio SHARED ${MUXAUDIO_SOURCES})

        target_include_directories(muxaudio
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${MUXAUDIO_INCLUDES}
        )

        target_compile_definitions(muxaudio PRIVATE ${MUXAUDIO_DEFINES})

        if(MUXAUDIO_LIBRARIES)
            target_link_libraries(muxaudio PRIVATE ${MUXAUDIO_LIBRARIES})
        endif()

        list(APPEND INSTALL_TARGETS muxaudio)
    endif()

    # Fully static library (codecs embedded)
    if(BUILD_STATIC_FULL)
        add_library(muxaudio-static STATIC ${MUXAUDIO_SOURCES})

        target_include_directories(muxaudio-static
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${MUXAUDIO_INCLUDES}
        )

        target_compile_definitions(muxaudio-static PRIVATE ${MUXAUDIO_DEFINES})

        if(MUXAUDIO_LIBRARIES)
            # For fully static, link codecs privately (embedded)
            target_link_libraries(muxaudio-static PRIVATE ${MUXAUDIO_LIBRARIES})
        endif()

        list(APPEND INSTALL_TARGETS muxaudio-static)
    endif()

    # Installation
    if(INSTALL_TARGETS)
        install(TARGETS ${INSTALL_TARGETS}
            EXPORT muxaudio-targets
            LIBRARY DESTINATION lib
            ARCHIVE DESTINATION lib
            RUNTIME DESTINATION bin
        )

        install(FILES include/mux.h
            DESTINATION include
        )
    endif()

    # Command-line tools
    if(BUILD_TOOLS)
        # Link to whichever library is built (prefer shared)
        set(MUXAUDIO_LINK_TARGET "")
        if(BUILD_SHARED)
            set(MUXAUDIO_LINK_TARGET muxaudio)
        elseif(BUILD_STATIC_FULL)
            set(MUXAUDIO_LINK_TARGET muxaudio-static)
        endif()

        if(MUXAUDIO_LINK_TARGET)
            add_executable(mux tools/mux.c)
            target_link_libraries(mux ${MUXAUDIO_LINK_TARGET})

            add_executable(demux tools/demux.c)
            target_link_libraries(demux ${MUXAUDIO_LINK_TARGET})

            install(TARGETS mux demux
                RUNTIME DESTINATION bin
            )
        endif()
    endif()

    # Tests
    if(BUILD_TESTS)
        # Link to whichever library is built (prefer shared)
        set(MUXAUDIO_LINK_TARGET "")
        if(BUILD_SHARED)
            set(MUXAUDIO_LINK_TARGET muxaudio)
        elseif(BUILD_STATIC_FULL)
            set(MUXAUDIO_LINK_TARGET muxaudio-static)
        endif()

        if(MUXAUDIO_LINK_TARGET)
            # Test utilities library
            add_library(test_utils STATIC tests/test_utils.c)
            target_include_directories(test_utils PUBLIC tests)
            target_link_libraries(test_utils m)

            # Core tests
            add_executable(test_pcm tests/test_pcm.c)
            target_link_libraries(test_pcm ${MUXAUDIO_LINK_TARGET})

            add_executable(test_errors tests/test_errors.c)
            target_link_libraries(test_errors ${MUXAUDIO_LINK_TARGET})

            add_executable(test_waveforms tests/test_waveforms.c)
            target_link_libraries(test_waveforms ${MUXAUDIO_LINK_TARGET} test_utils m)

            # Codec-specific tests
            if(LAME_FOUND AND MPG123_FOUND)
                add_executable(test_mp3 tests/test_mp3.c)
                target_link_libraries(test_mp3 ${MUXAUDIO_LINK_TARGET} m)

                add_executable(test_mp3_simple tests/test_mp3_simple.c)
                target_link_libraries(test_mp3_simple ${MUXAUDIO_LINK_TARGET})

                add_executable(test_lossy_validation tests/test_lossy_validation.c)
                target_link_libraries(test_lossy_validation ${MUXAUDIO_LINK_TARGET} test_utils m)
            endif()

            if(VORBIS_FOUND)
                add_executable(test_vorbis_simple tests/test_vorbis_simple.c)
                target_link_libraries(test_vorbis_simple ${MUXAUDIO_LINK_TARGET} m)

                add_executable(test_vorbis_validation tests/test_vorbis_validation.c)
                target_link_libraries(test_vorbis_validation ${MUXAUDIO_LINK_TARGET} test_utils m)
            endif()

            if(OPUS_FOUND)
                add_executable(test_opus_simple tests/test_opus_simple.c)
                target_link_libraries(test_opus_simple ${MUXAUDIO_LINK_TARGET} m)
            endif()

            if(FLAC_FOUND)
                add_executable(test_flac_simple tests/test_flac_simple.c)
                target_link_libraries(test_flac_simple ${MUXAUDIO_LINK_TARGET} m)

                add_executable(test_flac_validation tests/test_flac_validation.c)
                target_link_libraries(test_flac_validation ${MUXAUDIO_LINK_TARGET} test_utils m)
            endif()

            if(FDK_AAC_FOUND)
                add_executable(test_aac_simple tests/test_aac_simple.c)
                target_link_libraries(test_aac_simple ${MUXAUDIO_LINK_TARGET} m)
            endif()

            # G.711 tests (always available)
            add_executable(test_g711 tests/test_g711.c)
            target_link_libraries(test_g711 ${MUXAUDIO_LINK_TARGET} m)
        endif()
    endif()

    # Print configuration summary
    message(STATUS "")
    message(STATUS "=== MuxAudio Native Build ===")
    message(STATUS "Targets:")
    if(BUILD_SHARED)
        message(STATUS "  - muxaudio: Shared library with external codec DLLs/SOs")
    endif()
    if(BUILD_STATIC_FULL)
        message(STATUS "  - muxaudio-static: Fully static library (codecs embedded)")
    endif()
    if(BUILD_TOOLS)
        message(STATUS "  - mux, demux: Command-line tools")
    endif()
    message(STATUS "")
    message(STATUS "=============================")
    message(STATUS "")
endif()
