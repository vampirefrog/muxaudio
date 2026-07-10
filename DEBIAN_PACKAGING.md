# Debian Package Build

MuxAudio provides Debian packages for easy installation on Debian, Ubuntu, and derivative distributions.

## Package Structure

The build creates three binary packages:

### 1. `libmuxaudio0`
Runtime library package containing the shared library.

**Contents:**
- `/usr/lib/libmuxaudio.so` - Shared library

**Dependencies:**
- Automatically depends on codec libraries (libogg, libvorbis, libopus, libflac, libmpg123, libmp3lame, libfdk-aac)

### 2. `libmuxaudio-dev`
Development package containing headers and static library.

**Contents:**
- `/usr/include/mux.h` - C header file
- `/usr/lib/libmuxaudio-static.a` - Static library

**Dependencies:**
- `libmuxaudio0` (= same version)
- All codec development packages

### 3. `muxaudio-tools`
Command-line tools package.

**Contents:**
- `/usr/bin/mux` - Multiplexer tool
- `/usr/bin/demux` - Demultiplexer tool

**Dependencies:**
- `libmuxaudio0` (= same version)

## Building Locally

### Prerequisites

```bash
sudo apt-get install \
  debhelper \
  devscripts \
  build-essential \
  cmake \
  pkg-config \
  libogg-dev \
  libvorbis-dev \
  libopus-dev \
  libflac-dev \
  libmpg123-dev \
  libmp3lame-dev \
  libfdk-aac-dev
```

### Build Process

```bash
# From the repository root
dpkg-buildpackage -us -uc -b
```

This creates:
- `../libmuxaudio0_*.deb`
- `../libmuxaudio-dev_*.deb`
- `../muxaudio-tools_*.deb`
- Debug symbol packages (`*-dbgsym_*.deb`)

### Clean Build Artifacts

```bash
debian/rules clean
rm ../*.deb ../*.buildinfo ../*.changes
```

## Installation

### From GitHub Releases

Download the `.deb` files from the latest release and install:

```bash
# Install all packages
sudo apt install ./libmuxaudio0_*.deb ./libmuxaudio-dev_*.deb ./muxaudio-tools_*.deb

# Or install only what you need
sudo apt install ./libmuxaudio0_*.deb ./muxaudio-tools_*.deb  # Runtime + tools
sudo apt install ./libmuxaudio0_*.deb ./libmuxaudio-dev_*.deb # Runtime + dev
```

### Verification

After installation:

```bash
# Check library
ldconfig -p | grep muxaudio

# Check tools
which mux demux

# Check header
ls /usr/include/mux.h
```

## Usage After Installation

### Command-Line Tools

```bash
# Encode audio with side channel
cat audio.raw | mux --codec mp3 --streams 2 3< metadata.dat > output.mux

# Decode multiplexed stream
cat output.mux | demux --codec mp3 --streams 2 > audio.raw 3> metadata.dat
```

### Development

```c
#include <mux.h>

// Link with: -lmuxaudio
struct mux_encoder *enc = mux_encoder_new(...);
```

Compile:
```bash
gcc myapp.c -o myapp -lmuxaudio
```

## Package Maintenance

### Updating Version

Edit `debian/changelog`:

```bash
dch -v 0.2.0-1 "New upstream release"
```

Or manually update the first line:
```
muxaudio (0.2.0-1) unstable; urgency=medium
```

### Package Standards

- Debhelper compatibility level: 13
- Standards-Version: 4.6.0
- Source format: 3.0 (native)
- License: GPL-3.0-or-later

## GitHub Actions Integration

The release workflow automatically builds deb packages on Ubuntu and uploads them to GitHub releases.

See `.github/workflows/release.yml` job `build-deb` for the CI build process.

## Troubleshooting

### Missing Dependencies

If you get "unmet dependencies" errors:

```bash
sudo apt-get install -f
```

### Library Not Found

After installing `libmuxaudio0`, if programs can't find it:

```bash
sudo ldconfig
```

### Lintian Warnings

Check package quality:

```bash
lintian ../libmuxaudio0_*.deb
lintian ../libmuxaudio-dev_*.deb
lintian ../muxaudio-tools_*.deb
```

## Files

The Debian packaging files are in the `debian/` directory:

- `debian/control` - Package metadata and dependencies
- `debian/rules` - Build instructions
- `debian/changelog` - Version history
- `debian/copyright` - License information
- `debian/libmuxaudio0.install` - Runtime library files
- `debian/libmuxaudio-dev.install` - Development files
- `debian/muxaudio-tools.install` - Tool binaries
- `debian/source/format` - Source package format
