# Native macOS downloader

The macOS bundle uses aria2 1.37.0 for AI Mask, CLIP and LUT downloads. It contains arm64 code,
uses AppleTLS, and targets macOS 13.3. c-ares 1.34.5 is linked statically. zlib, libc++, Security
and CoreFoundation come from macOS. No Homebrew library is required on the user's Mac.

## Build and publish

On an Apple Silicon Mac with Xcode command-line tools and CMake:

```sh
PATH=/opt/homebrew/bin:$PATH bash scripts/build_aria2_macos.sh --jobs 4
```

The script checks both upstream source archives with SHA-256. It builds in `build/aria2-macos`
and writes two archives plus `SHA256SUMS` to `build/aria2-macos/dist`. Allow 1 GiB of free space;
the recorded build used 127 MiB. The binary archive includes the licenses, build recipe and
`BUILD-INFO.txt` with the compiler, SDK, configure flags and build machine information.
The sources archive contains the exact aria2 and c-ares source archives plus the build recipe.
The recipe uses SDK zlib and disables XML-RPC, Metalink, BitTorrent, SFTP and SQLite cookies.
JSON-RPC over HTTP, HTTPS, compression, checksums, async DNS and segmented downloads remain enabled.

Publish both archives and `SHA256SUMS` together as a dependency release:

[aria2 1.37.0 macOS arm64 r1](https://github.com/zidage/AlcedoStudio/releases/tag/aria2-1.37.0-macos-arm64-r1).
Keep the corresponding source archive available with the binary. For a new build, use a new
release tag and update the pinned URL and SHA-256 in `fetch_aria2.cmake`.

The application configure step downloads the binary archive and checks its SHA-256. It also
checks cached archives and replaces results cached from the former Intel 1.35.0 download.
An explicit `ALCEDO_ARIA2C_BINARY` outside the module's `aria2-download` directory still overrides
the fetch. On macOS, overrides must contain arm64. A failed fetch or checksum stops configuration.
The existing bundle signing step signs aria2c with the other executables.

## Checks

These checks need no application build:

```sh
cmake -DALCEDO_TEST_ARIA2_ARCHIVE="$PWD/build/aria2-macos/dist/aria2-1.37.0-macos-arm64.tar.gz" \
  -P scripts/tests/fetch_aria2_test.cmake
bash scripts/tests/verify_macos_architectures_test.sh
```

The CMake checks cover extraction, explicit override, disabled fetch, missing override, old-cache
migration and rejection of a corrupt archive. Run them on Windows with the official
`aria2-1.37.0-win-64bit-build1.zip` to check the unchanged Windows payload and its pinned checksum.
The shell check rejects an Intel downloader and an Intel Mach-O in Resources/qml.

After the normal application install, run `scripts/verify_macos_install_tree.sh`. It checks every
Mach-O file in the app for arm64, dependency closure, the downloader version and AppleTLS, bundle
signatures, QML imports and CLI session start/stop with a clean PATH.

## Verification record (2026-10-29)

The build ran on Apple Silicon with macOS 27.0.1 (26A434), Xcode 26.3 and Apple clang 17.0.0.
`lipo` reported arm64. `vtool` reported a minimum macOS version of 13.3. `otool -L` listed only
system libraries. `arch -arm64 aria2c --version` reported 1.37.0, AppleTLS and c-ares 1.34.5.

The six CMake fetch checks passed on Windows and macOS. The architecture rejection checks passed
on macOS. The install-tree check rejected the former Intel downloader. It passed on an APFS copy
of the existing release app after the downloader and notices were replaced and the app was signed
ad hoc. This check included all Mach-O files, signatures, dependencies, QML imports and clean-PATH
CLI session start/stop. The application itself was not rebuilt.

A small Qt client linked the existing production DownloadService library and used the downloader
inside that app copy, with a clean PATH. All transfers passed the service's size and SHA-256 checks:

| Payload | Downloaded bytes | Archive check |
| --- | ---: | --- |
| AI Mask macOS model set v1 r1 | 390,518,274 | 7z extraction passed |
| spektrafilm LUT package 2026.09.30.1 | 98,886,202 | 7z extraction passed |
| SigLIP2 CoreML model, tokenizer and manifest | 355,750,271 | Three ZIP integrity checks and extractions passed |

Each group was deleted before the next group started. These checks verify real HTTPS transfers
and archive extraction; they do not verify activation from the Settings UI. Rosetta was installed
on this Mac, so a machine with Rosetta absent was not tested. The tested downloader has only arm64
code. No Developer ID identity was available, so Developer ID signing was not tested.
