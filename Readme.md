# Project+ Dolphin Fork

![Untitled-1](https://github.com/user-attachments/assets/3bfb148a-85d1-489d-96ae-1b7892acdc11)


This fork is a heavily modified version of the Dolphin Emulator designed for usage with Project+. It includes the following features:

  * Branding, theming and iconography updates to match the Project+ aesthetic
  * Netplay button on the toolbar
  * Update button on the toolbar
  * Specific aspect ratios (19:15 and 69:40 for regular and widescreen, respectively)
  * Discord Rich Presence support for Project+
  * Gamecube Adapter polling rate listed in controller settings
  * Pre-configured hotkeys for volume up/down and opening netplay chat
  * Automatic copying of netplay lobby code on netplay window startup
  * 2 buffer as default netplay setting (equivalent to 4 in FPM)
  * Separate Minimum and Player buffers in netplay window
  * Client side music toggle in netplay window
  * Spectator button in netplay window
  * Automatic export of Brawl vault data at the end of netplay sessions
  * Alternative implementation for automatic updates that fetch the latest GitHub release
 
This project would not be possible without the work done by the [Faster Melee](https://github.com/FasterMelee/Ishiiruka), [Faster PM](https://github.com/jlambert360/Ishiiruka), [Slippi](https://github.com/project-slippi/dolphin), and [Mario Party Netplay](https://github.com/MarioPartyNetplay/Dolphin-MPN) teams. 

# Dolphin - A GameCube and Wii Emulator

[Homepage](https://dolphin-emu.org/) | [Project Site](https://github.com/dolphin-emu/dolphin) | [Buildbot](https://dolphin.ci/) | [Forums](https://forums.dolphin-emu.org/) | [Wiki](https://wiki.dolphin-emu.org/) | [GitHub Wiki](https://github.com/dolphin-emu/dolphin/wiki) | [Issue Tracker](https://bugs.dolphin-emu.org/projects/emulator/issues) | [Coding Style](https://github.com/dolphin-emu/dolphin/blob/master/Contributing.md) | [Transifex Page](https://app.transifex.com/dolphinemu/dolphin-emu/dashboard/) | [Analytics](https://mon.dolphin-emu.org/)

Dolphin is an emulator for running GameCube and Wii games on Windows,
Linux, macOS, and recent Android devices. It's licensed under the terms
of the GNU General Public License, version 2 or later (GPLv2+).

Please read the [FAQ](https://dolphin-emu.org/docs/faq/) before using Dolphin.

## System Requirements

### Desktop

* OS
    * Windows (10 1903 or higher).
    * Linux.
    * macOS (11.0 Big Sur or higher).
    * Unix-like systems other than Linux are not officially supported but might work.
* Processor
    * A CPU with SSE2 support.
    * A modern CPU (3 GHz and Dual Core, not older than 2008) is highly recommended.
* Graphics
    * A reasonably modern graphics card (Direct3D 11.1 / OpenGL 3.3).
    * A graphics card that supports Direct3D 11.1 / OpenGL 4.4 is recommended.

### Android

* OS
    * Android (7.0 Nougat or higher).
* Processor
    * A processor with support for 64-bit applications (either ARMv8 or x86-64).
* Graphics
    * A graphics processor that supports OpenGL ES 3.0 or higher. Performance varies heavily with [driver quality](https://dolphin-emu.org/blog/2013/09/26/dolphin-emulator-and-opengl-drivers-hall-fameshame/).
    * A graphics processor that supports standard desktop OpenGL features is recommended for best performance.

Dolphin can only be installed on devices that satisfy the above requirements. Attempting to install on an unsupported device will fail and display an error message.

## Building

You may find building instructions on the appropriate wiki page for your operating system:

* [Windows](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Windows)
* [Linux](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux)
* [macOS](https://github.com/dolphin-emu/dolphin/wiki/Building-for-macOS)
* [Android](#android-specific-instructions) <!-- TODO: Create a "Building for Android" wiki page and link it here -->
* [OpenBSD](https://github.com/dolphin-emu/dolphin/wiki/Building-for-OpenBSD) (unsupported)

Before building, make sure to pull all submodules:

```sh
git submodule update --init --recursive
```

The "Release" solution configuration includes performance optimizations for the best user experience but complicates debugging Dolphin.
The "Debug" solution configuration is significantly slower, more verbose and less permissive but makes debugging Dolphin easier.

## Building for Linux and macOS

Dolphin requires [CMake](https://cmake.org/) for systems other than Windows. 
You need a recent version of GCC or Clang with decent c++20 support. CMake will
inform you if your compiler is too old.
Many libraries are bundled with Dolphin and used if they're not installed on 
your system. CMake will inform you if a bundled library is used or if you need
to install any missing packages yourself. You may refer to the [wiki](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux) for more information.

Make sure to pull submodules before building:
```sh
git submodule update --init --recursive
```

### CI dependency builds

The Windows, macOS, and Linux AppImage jobs build this repository's checked-out
revision. They restore prebuilt Release libraries for fmt, glslang, libusb, SDL3,
SFML's networking/system subset, and mbedTLS. The first run builds these libraries
from the pinned `Externals` sources. Later runs reuse the installed headers,
static libraries, and CMake package files. Application-only source changes do not
invalidate the dependency bundle.

The cache key includes the dependency sources and build recipe, the configured
compiler and SDK, the runner image version, and architecture. Each restored bundle
is checked by compiling, linking, and running a small consumer before Dolphin
uses it. Successful bundles are saved immediately, so a later application build
failure does not discard them. GitHub cache eviction or a toolchain/dependency
change can still require rebuilding. The manually dispatched **Prepare Native
Dependencies** workflow can populate the caches for all three platforms.

macOS produces two separate native builds: `x86_64` on `macos-15-intel` and
`arm64` on `macos-15` for Apple Silicon (M-series). Both use standard
[GitHub-hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The dependency workflow covers both runners, and their library, Qt, MoltenVK,
and compiler caches are separate. The Intel Qt/MoltenVK/compiler cache keys are
unchanged. Download the artifact containing `macOS-arm64` for an M-series Mac
or `macOS-x86_64` for an Intel Mac; these are not universal app bundles.

Both macOS jobs verify the architecture of Qt, MoltenVK, and every bundled
Mach-O binary before uploading. The app is ad-hoc signed using Dolphin's existing
JIT entitlements, then its signature and `--version` startup are checked on the
native runner. No Apple signing certificate is required, but these builds are
not notarized. These checks do not test gameplay or cross-architecture rollback
determinism; those still require runtime testing.

Qt is prebuilt on all three platforms: Windows uses its pinned Qt submodule,
while macOS and Linux download and cache Qt binaries. macOS also caches the
Dolphin-patched MoltenVK library separately. Other bundled dependencies continue
to use the normal build. All three native jobs additionally cache compiled
objects: Windows uses Ninja/MSVC with sccache, and Linux/macOS use ccache. Each
has a 2 GB local compiler cache saved through Actions; Windows keeps the existing
`/Z7` debug information and Release optimization flags. This does not change the
compiler or disable release optimizations.

Compiler-cache keys are platform/architecture-specific, with a snapshot per
commit and retry. An unchanged rerun can restore an exact snapshot; a new commit
restores the latest compatible-platform snapshot and lets the compiler cache
validate each object. Exact hits are not uploaded again. Partial caches from a
failed compilation are saved, but a failure before compilation does not save an
empty compiler cache. Cache statistics appear in both the logs and job summary.

To measure improvements, compare a cold build, an unchanged rerun, and a small
C++-only change on the same runner/toolchain. Check cache hits as well as total
job time: dependency setup, linking and packaging are not compiler-cache hits.
The manual dependency workflow must use the same runner and MSVC environment as
the application job. Run it on the default branch to seed caches for PRs; a cache
created on an unrelated feature branch is not shared with every other branch.

Local workflow checks: `actionlint .github/workflows/ci.yml .github/workflows/dependencies.yml`
and `python3 .github/workflows/scripts/test_ci.py` (requires PyYAML; install `pwsh`
to also check Windows script syntax, argument quoting, and failure propagation).
With Clang and `lipo` or `llvm-lipo` installed, the tests also compile small
Mach-O fixtures and execute the Qt architecture check with the real tool.
These checks do not replace Windows/macOS hosted builds and artifact smoke tests.

Flatpak downloads the prebuilt SDK selected by its manifest and reuses its own
builder cache. It no longer installs an unrelated host toolchain or the old 5.15
SDK. Its manifest is pinned to this repository and the workflow commit.

To use an installed bundle manually, pass
`-DDOLPHIN_DEPENDENCY_PREFIX=/path/to/bundle -DCMAKE_BUILD_TYPE=Release` to CMake.
For Visual Studio, also pass `-DCMAKE_CONFIGURATION_TYPES=Release`. The bundle
must match the target architecture and compiler version. Leaving the prefix
unset preserves the ordinary source/system-library build.

### macOS Build Steps:

A binary supporting a single architecture can be built using the following steps: 

1. `mkdir build`
2. `cd build`
3. `cmake ..`
4. `make -j $(sysctl -n hw.logicalcpu)`

An application bundle will be created in `./Binaries`.

A script is also provided to build universal binaries supporting both x64 and ARM in the same
application bundle using the following steps:

1. `mkdir build`
2. `cd build`
3. `python ../BuildMacOSUniversalBinary.py`
4. Universal binaries will be available in the `universal` folder

Doing this is more complex as it requires installation of library dependencies for both x64 and ARM (or universal library
equivalents) and may require specifying additional arguments to point to relevant library locations. 
Execute BuildMacOSUniversalBinary.py --help for more details.  

### Linux Global Build Steps:

To install to your system.

1. `mkdir build`
2. `cd build`
3. `cmake ..`
4. `make -j $(nproc)`
5. `sudo make install`

### Linux Local Build Steps:

Useful for development as root access is not required.

1. `mkdir Build`
2. `cd Build`
3. `cmake .. -DLINUX_LOCAL_DEV=true`
4. `make -j $(nproc)`
5. `ln -s ../../Data/Sys Binaries/`

### Linux Portable Build Steps:

Can be stored on external storage and used on different Linux systems.
Or useful for having multiple distinct Dolphin setups for testing/development/TAS.

1. `mkdir Build`
2. `cd Build`
3. `cmake .. -DLINUX_LOCAL_DEV=true`
4. `make -j $(nproc)`
5. `cp -r ../Data/Sys/ ../Data/user/ ../Data/portable.txt Binaries/`


### Linux AppImage Build Steps:

Used for distribution of Project+ builds.

1. `mkdir Build`
2. `cd Build`
3. `cmake .. -DLINUX_LOCAL_DEV=true -DCMAKE_INSTALL_PREFIX=/usr -GNinja`
4. `../BuildLinuxAppImage.sh`

## Building for Android

These instructions assume familiarity with Android development. If you do not have an
Android dev environment set up, see [AndroidSetup.md](AndroidSetup.md).

If using Android Studio, import the Gradle project located in `./Source/Android`.

Android apps are compiled using a build system called Gradle. Dolphin's native component,
however, is compiled using CMake. The Gradle script will attempt to run a CMake build
automatically while building the Java code.

## Uninstalling

On Windows, simply remove the extracted directory, unless it was installed with the NSIS installer,
in which case you can uninstall Dolphin like any other Windows application.

Linux users can run `cat install_manifest.txt | xargs -d '\n' rm` as root from the build directory
to uninstall Dolphin from their system.

macOS users can simply delete Dolphin.app to uninstall it.

Additionally, you'll want to remove the global user directory if you don't plan on reinstalling Dolphin.

## Command Line Usage

```
Usage: Dolphin.exe [options]... [FILE]...

Options:
  --version             show program's version number and exit
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path
  -m MOVIE, --movie=MOVIE
                        Play a movie file
  -e <file>, --exec=<file>
                        Load the specified file
  -n <16-character ASCII title ID>, --nand_title=<16-character ASCII title ID>
                        Launch a NAND title
  -C <System>.<Section>.<Key>=<Value>, --config=<System>.<Section>.<Key>=<Value>
                        Set a configuration option
  -s <file>, --save_state=<file>
                        Load the initial save state
  -d, --debugger        Show the debugger pane and additional View menu options
  -l, --logger          Open the logger
  -b, --batch           Run Dolphin without the user interface (Requires
                        --exec or --nand-title)
  -c, --confirm         Set Confirm on Stop
  -v VIDEO_BACKEND, --video_backend=VIDEO_BACKEND
                        Specify a video backend
  -a AUDIO_EMULATION, --audio_emulation=AUDIO_EMULATION
                        Choose audio emulation from [HLE|LLE]
```

Available DSP emulation engines are HLE (High Level Emulation) and
LLE (Low Level Emulation). HLE is faster but less accurate whereas
LLE is slower but close to perfect. Note that LLE has two submodes (Interpreter and Recompiler)
but they cannot be selected from the command line.

Available video backends are "D3D" and "D3D12" (they are only available on Windows), "OGL", and "Vulkan".
There's also "Null", which will not render anything, and
"Software Renderer", which uses the CPU for rendering and
is intended for debugging purposes only.

## DolphinTool Usage
```
usage: dolphin-tool COMMAND -h

commands supported: [convert, verify, header, extract]
```

```
Usage: convert [options]... [FILE]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FILE, --output=FILE
                        Path to the destination FILE.
  -f FORMAT, --format=FORMAT
                        Container format to use. Default is RVZ. [iso|gcz|wia|rvz]
  -s, --scrub           Scrub junk data as part of conversion.
  -b BLOCK_SIZE, --block_size=BLOCK_SIZE
                        Block size for GCZ/WIA/RVZ formats, as an integer.
                        Suggested value for RVZ: 131072 (128 KiB)
  -c COMPRESSION, --compression=COMPRESSION
                        Compression method to use when converting to WIA/RVZ.
                        Suggested value for RVZ: zstd [none|zstd|bzip|lzma|lzma2]
  -l COMPRESSION_LEVEL, --compression_level=COMPRESSION_LEVEL
                        Level of compression for the selected method. Ignored
                        if 'none'. Suggested value for zstd: 5
```

```
Usage: verify [options]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -a ALGORITHM, --algorithm=ALGORITHM
                        Optional. Compute and print the digest using the
                        selected algorithm, then exit. [crc32|md5|sha1|rchash]
```

```
Usage: header [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -b, --block_size      Optional. Print the block size of GCZ/WIA/RVZ formats,
then exit.
  -c, --compression     Optional. Print the compression method of GCZ/WIA/RVZ
                        formats, then exit.
  -l, --compression_level
                        Optional. Print the level of compression for WIA/RVZ
                        formats, then exit.
```

```
Usage: extract [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FOLDER, --output=FOLDER
                        Path to the destination FOLDER.
  -p PARTITION, --partition=PARTITION
                        Which specific partition you want to extract.
  -s SINGLE, --single=SINGLE
                        Which specific file/directory you want to extract.
  -l, --list            List all files in volume/partition. Will print the
                        directory/file specified with --single if defined.
  -q, --quiet           Mute all messages except for errors.
  -g, --gameonly        Only extracts the DATA partition.
```
