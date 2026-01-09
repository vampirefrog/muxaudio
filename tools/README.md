# muxaudio Command-Line Tools

Two command-line utilities for multiplexing audio with side channel data.

## Tools

### muxaudio-encode

Encodes raw PCM audio from stdin with optional side channel data from fd 3, outputting a multiplexed stream to stdout.

**Usage:**
```bash
muxaudio-encode [options] < audio.raw 3< metadata.txt > output.mux
```

**Options:**
- `-c, --codec CODEC` - Codec to use: pcm, mp3, vorbis, opus, flac (default: flac)
- `-r, --rate RATE` - Sample rate in Hz (default: 44100)
- `-n, --channels NUM` - Number of channels (default: 2)
- `-b, --bitrate KBPS` - Bitrate in kbps for lossy codecs (default: 128)
- `-l, --level LEVEL` - Compression level 0-8 for FLAC (default: 5)
- `-h, --help` - Show help

**Input:**
- stdin: Raw PCM audio (int16, interleaved)
- fd 3: Side channel data (optional)

**Output:**
- stdout: Multiplexed stream

### muxaudio-decode

Decodes a multiplexed stream from stdin, outputting raw PCM audio to stdout and side channel data to fd 3.

**Usage:**
```bash
muxaudio-decode [options] < input.mux > audio.raw 3> metadata.txt
```

**Options:**
- `-c, --codec CODEC` - Codec to use: pcm, mp3, vorbis, opus, flac (default: flac)
- `-v, --verbose` - Print stream information to stderr
- `-h, --help` - Show help

**Input:**
- stdin: Multiplexed stream

**Output:**
- stdout: Raw PCM audio (int16, interleaved)
- fd 3: Side channel data (if present in stream)

## Examples

### Basic encoding and decoding

```bash
# Encode audio with FLAC (lossless)
muxaudio-encode -c flac -r 44100 -n 2 < input.raw > output.mux

# Decode back to audio
muxaudio-decode -c flac < output.mux > decoded.raw
```

### With side channel data

```bash
# Encode audio with metadata on fd 3
muxaudio-encode -c flac -r 44100 -n 2 < audio.raw 3< metadata.txt > output.mux

# Decode audio and metadata
muxaudio-decode -c flac < output.mux > audio.raw 3> metadata.txt
```

### Different codecs

```bash
# FLAC (lossless, best compression)
muxaudio-encode -c flac -r 44100 -n 2 -l 8 < input.raw > output.flac.mux

# Opus (lossy, good for speech)
muxaudio-encode -c opus -r 48000 -n 2 -b 96 < input.raw > output.opus.mux

# MP3 (lossy, widely compatible)
muxaudio-encode -c mp3 -r 44100 -n 2 -b 192 < input.raw > output.mp3.mux

# PCM (uncompressed)
muxaudio-encode -c pcm -r 44100 -n 2 < input.raw > output.pcm.mux
```

### Pipeline processing

```bash
# Process audio through a pipeline
generate_audio | muxaudio-encode -c flac -r 44100 -n 2 | muxaudio-decode -c flac | play_audio
```

### Real-time streaming

```bash
# Stream from microphone, encode, and save
arecord -f S16_LE -r 44100 -c 2 | muxaudio-encode -c opus -r 44100 -n 2 -b 64 > stream.mux

# Decode and play back
muxaudio-decode -c opus < stream.mux | aplay -f S16_LE -r 44100 -c 2
```

### Adding timestamped metadata

```bash
# Create a script that generates timestamps
(
  while true; do
    echo "$(date +%s.%N): frame marker"
    sleep 0.1
  done
) 3>&1 | muxaudio-encode -c flac -r 44100 -n 2 < audio.raw > timestamped.mux
```

## Codec Comparison

Based on 1 second of audio (44100 Hz, stereo, ~176 KB):

| Codec   | Size    | Ratio  | Type     | Use Case                    |
|---------|---------|--------|----------|-----------------------------|
| PCM     | 176 KB  | 100%   | Lossless | No compression needed       |
| FLAC    | 13 KB   | 7.3%   | Lossless | Archive, master copies      |
| Opus    | 14 KB   | 8.2%   | Lossy    | VoIP, streaming, low latency|
| MP3     | 25 KB   | 14.6%  | Lossy    | Music, wide compatibility   |

## Notes

- All tools use stdin/stdout, making them perfect for Unix pipelines
- Side channel data on fd 3 is optional - tools work fine without it
- FLAC provides lossless compression (perfect reconstruction)
- Opus and MP3 are lossy but provide higher compression ratios
- PCM provides no compression but is useful for testing
