cmake_minimum_required(VERSION 3.10)
project(muxaudio VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries" ON)
option(BUILD_WASM "Build for WebAssembly" OFF)

# Compiler flags - Linux kernel style
if(NOT MSVC)
	add_compile_options(
		-Wall
		-Wextra
		-Werror
		-Wno-unused-parameter
		-Wstrict-prototypes
		-Wmissing-prototypes
	)
endif()

# Find dependencies
find_library(LAME_LIBRARY mp3lame)
find_library(MPG123_LIBRARY mpg123)
find_library(OGG_LIBRARY ogg)
find_library(VORBIS_LIBRARY vorbis)
find_library(VORBISENC_LIBRARY vorbisenc)
find_library(OPUS_LIBRARY opus)
find_library(FLAC_LIBRARY FLAC)
find_library(FDK_AAC_LIBRARY fdk-aac)

if(NOT LAME_LIBRARY)
	message(WARNING "libmp3lame not found - MP3 encoding will not be available")
endif()

if(NOT MPG123_LIBRARY)
	message(WARNING "libmpg123 not found - MP3 decoding will not be available")
endif()

if(NOT OGG_LIBRARY OR NOT VORBIS_LIBRARY OR NOT VORBISENC_LIBRARY)
	message(WARNING "libogg/libvorbis not found - Vorbis codec will not be available")
endif()

if(NOT OGG_LIBRARY OR NOT OPUS_LIBRARY)
	message(WARNING "libogg/libopus not found - Opus codec will not be available")
endif()

if(NOT OGG_LIBRARY OR NOT FLAC_LIBRARY)
	message(WARNING "libogg/libFLAC not found - FLAC codec will not be available")
endif()

if(NOT FDK_AAC_LIBRARY)
	message(WARNING "libfdk-aac not found - AAC codec will not be available")
endif()

# Source files
set(MUXAUDIO_SOURCES
	src/core.c
	src/buffer.c
	src/error.c
	src/mux_leb128.c
	src/codec_pcm.c
)

if(LAME_LIBRARY OR MPG123_LIBRARY)
	list(APPEND MUXAUDIO_SOURCES src/codec_mp3.c)
	add_compile_definitions(HAVE_MP3)
endif()

if(OGG_LIBRARY AND VORBIS_LIBRARY AND VORBISENC_LIBRARY)
	list(APPEND MUXAUDIO_SOURCES src/codec_vorbis.c)
	add_compile_definitions(HAVE_VORBIS)
endif()

if(OGG_LIBRARY AND OPUS_LIBRARY)
	list(APPEND MUXAUDIO_SOURCES src/codec_opus.c)
	add_compile_definitions(HAVE_OPUS)
endif()

if(OGG_LIBRARY AND FLAC_LIBRARY)
	list(APPEND MUXAUDIO_SOURCES src/codec_flac.c)
	add_compile_definitions(HAVE_FLAC)
endif()

if(FDK_AAC_LIBRARY)
	list(APPEND MUXAUDIO_SOURCES src/codec_aac.c)
	add_compile_definitions(HAVE_AAC)
endif()

# Library target
add_library(muxaudio ${MUXAUDIO_SOURCES})

target_include_directories(muxaudio
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(LAME_LIBRARY)
	target_link_libraries(muxaudio ${LAME_LIBRARY})
endif()

if(MPG123_LIBRARY)
	target_link_libraries(muxaudio ${MPG123_LIBRARY})
endif()

if(OGG_LIBRARY AND VORBIS_LIBRARY AND VORBISENC_LIBRARY)
	target_link_libraries(muxaudio ${OGG_LIBRARY} ${VORBIS_LIBRARY} ${VORBISENC_LIBRARY})
endif()

if(OGG_LIBRARY AND OPUS_LIBRARY)
	target_link_libraries(muxaudio ${OGG_LIBRARY} ${OPUS_LIBRARY})
endif()

if(OGG_LIBRARY AND FLAC_LIBRARY)
	target_link_libraries(muxaudio ${OGG_LIBRARY} ${FLAC_LIBRARY})
endif()

if(FDK_AAC_LIBRARY)
	target_link_libraries(muxaudio ${FDK_AAC_LIBRARY})
endif()

# Installation
install(TARGETS muxaudio
	EXPORT muxaudio-targets
	LIBRARY DESTINATION lib
	ARCHIVE DESTINATION lib
	RUNTIME DESTINATION bin
)

install(FILES include/mux.h
	DESTINATION include
)

# Command-line tools
add_executable(mux tools/mux.c)
target_link_libraries(mux muxaudio)

add_executable(demux tools/demux.c)
target_link_libraries(demux muxaudio)

install(TARGETS mux demux
	RUNTIME DESTINATION bin
)

# Test utilities library
add_library(test_utils STATIC tests/test_utils.c)
target_include_directories(test_utils PUBLIC tests)
target_link_libraries(test_utils m)

# Examples/tests
add_executable(test_pcm tests/test_pcm.c)
target_link_libraries(test_pcm muxaudio)

add_executable(test_errors tests/test_errors.c)
target_link_libraries(test_errors muxaudio)

add_executable(test_waveforms tests/test_waveforms.c)
target_link_libraries(test_waveforms muxaudio test_utils m)

if(LAME_LIBRARY AND MPG123_LIBRARY)
	add_executable(test_mp3 tests/test_mp3.c)
	target_link_libraries(test_mp3 muxaudio m)

	add_executable(test_mp3_simple tests/test_mp3_simple.c)
	target_link_libraries(test_mp3_simple muxaudio)

	add_executable(test_lossy_validation tests/test_lossy_validation.c)
	target_link_libraries(test_lossy_validation muxaudio test_utils m)
endif()

if(OGG_LIBRARY AND VORBIS_LIBRARY AND VORBISENC_LIBRARY)
	add_executable(test_vorbis_simple tests/test_vorbis_simple.c)
	target_link_libraries(test_vorbis_simple muxaudio m)

	add_executable(test_vorbis_validation tests/test_vorbis_validation.c)
	target_link_libraries(test_vorbis_validation muxaudio test_utils m)
endif()

if(OGG_LIBRARY AND OPUS_LIBRARY)
	add_executable(test_opus_simple tests/test_opus_simple.c)
	target_link_libraries(test_opus_simple muxaudio m)
endif()

if(OGG_LIBRARY AND FLAC_LIBRARY)
	add_executable(test_flac_simple tests/test_flac_simple.c)
	target_link_libraries(test_flac_simple muxaudio m)

	add_executable(test_flac_validation tests/test_flac_validation.c)
	target_link_libraries(test_flac_validation muxaudio test_utils m)
endif()

if(FDK_AAC_LIBRARY)
	add_executable(test_aac_simple tests/test_aac_simple.c)
	target_link_libraries(test_aac_simple muxaudio m)
endif()
