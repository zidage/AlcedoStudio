<p align="center">
  <img src="docs/header.png" alt="Alcedo Studio" width="25%"/>
</p>

[Project website](https://aoraw.org/) | [项目网页](https://aoraw.org/zh-cn/)

<p align="right"><a href="./README.md"><strong>English</strong></a> | <a href="./README.zh-CN.md">简体中文</a></p>

![License](https://img.shields.io/badge/License-GPLv3-blue)
![C++](https://img.shields.io/badge/C++-20-blue)

**Alcedo Studio** is a free, open-source RAW photo editor and album manager for the day after a shoot.

Import a card of RAW files and browse. Edit images up to 150 megapixels in a GPU-accelerated 32-bit float pipeline.

One file follows your photos. It is a [DuckDB](https://duckdb.org/) database that holds the album structure, metadata, named looks, and the complete edit history. Move it, back it up, put it where you want. Queries over a large library stay fast.

Alcedo also implements a film-industry color workflow: your RAW becomes a scene-linear image, you grade in ACEScc, film-emulation LUTs carry the look, and a display rendering transform makes the final picture. On macOS, the editor previews HDR directly.

Windows 10/11 x64 and Apple Silicon. Current release: [v0.2.9](https://github.com/zidage/AlcedoStudio/releases/tag/v0.2.9).

---

## Preview

Editor

https://github.com/user-attachments/assets/d70cd10d-2045-42f3-a67d-97ab3ef9874b

Album browser

https://github.com/user-attachments/assets/ae0d9773-220e-4901-90f6-1989f58b0462

## Features

**Work with most cameras.** Tested across Canon, Nikon, Sony, Fujifilm, Panasonic, OM System, Leica, Hasselblad, Phase One (including IQ4 150MP), Pentax, Sigma, and phone / drone DNG. Lists: [supported formats](docs/supported_raw_formats.md) and [supported cameras](docs/supported_cameras.md). Nikon HE and HE★ NEFs from the Z 8, Z 9, Z 6 III, and Z 50 II decode through the project's [LibRaw fork](https://github.com/zidage/LibRaw).

Alcedo provides RCD and Neural Engine demosaicing. Bayer files use RCD by default; Fujifilm X-Trans files use Neural Engine by default for better quality. Neural Engine runs a distilled [DemosaicNet](https://groups.csail.mit.edu/graphics/demosaicnet/) on the GPU and supports both Bayer and X-Trans. Highlight reconstruction uses an improved inpaint-opposed method adapted from darktable and RawTherapee.

**High-performance process core.** Editing remains fluid even with a 150-megapixel RAW. After the initial decode, interactive edits run from a downsized cache rather than the full-resolution source. Drag a slider and the preview holds 2.5K at 60 frames per second, regardless of the source RAW resolution. When you stop, the preview resolves at 4K, so your high-megapixel camera still shows what it captured. Image processing is GPU-accelerated by default, and the cache keeps memory use low.

**Color tools work in ACEScc.** Adjustments range from local tone mapping for highlights and shadows to CDL color wheels. An RGB histogram and waveform update with the image.

**A display rendering transform forms the picture.** Scene-linear RAW holds colors and dynamic range a monitor cannot show on its own. A DRT (Display Rendering Transform) is the look decision that maps that range onto the screen. The transform sits at the heart of modern film pipelines. Alcedo provides ACES 2.0 and OpenDRT. Grade toward sRGB, wide-gamut, or HDR, with control over the target color space, EOTF, and HDR peak luminance. On macOS, the preview supports HDR while you edit. For a longer introduction to picture formation, see [Chris Brejon's article](https://chrisbrejon.com/articles/what-makes-a-good-picture-formation/).

**Film-emulation LUTs.** [CUBE LUTs generated from real film-stock spectral responses](https://github.com/JanLohse/spectral_film_lut), plus grain and halation with a physical model behind them.

**Looks you keep, copy, and walk back.** A photo can hold several named looks, so you can compare them and come back later. You can copy a look onto another photo, or merge parts of one look into another. Every adjustment leaves a record, and every step can be undone. The complete history stays with the project file, so you can always undo your adjustments. The edit history is backed by a Git-like version control system. For more information, see [Edit History](https://zidage.github.io/AlcedoStudio_docs/en/docs/developer/edit-history-architecture).

**Export with reusable configurations.** Configure format, bit depth, size, naming, metadata, and ICC profile. Export JPEG, PNG, TIFF, or EXR at 8–32 bits, depending on the format. Set the export resolution. File names can draw from the source name, capture date, camera, lens, exposure, rating, and a running sequence.

**Describe, review, and rate.** Connect an LLM provider you already use, such as OpenCode Go. Alcedo can write a description, a 1–5-star rating, and a short reason for the rating. You control how strict the review is, and the analysis can run in the background while you browse or edit.

**Tag automatically.** Multilingual CLIP models run locally to tag each photo. You can manage the installed models and enable automatic tagging after import.

**Search by words, meaning, or metadata.** Full-text search covers LLM-generated descriptions and tags. CLIP vector search matches a natural-language query against image embeddings to find photos by visual meaning. EXIF search covers camera, lens, capture date, ISO, focal length, and aperture.

**Inspect the album.** The Album Inspector summarizes the library by capture date, camera, lens, labels, and rating. Selecting a group filters the album to those photos.

**The library stays fast.** The inode-like library is stored in a DuckDB database. FTS accelerates full-text lookup over LLM-generated descriptions and tags. HNSW accelerates CLIP vector search over image embeddings. These indexes keep full-text and vector queries responsive as the library grows.

## System requirements

- **Windows**: 10/11 x64. NVIDIA GPU (compute capability 6.0+) for CUDA; other GPUs use OpenCL.
- **macOS**: Apple Silicon (M1 or newer), macOS 13.3 or later, Metal.
- 8 GB RAM minimum (16 GB or more if your library is large).
- Release builds install signed updates from Settings → Updates.

## Documentation

User guides and build notes: [documentation site](https://zidage.github.io/AlcedoStudio_docs/docs/intro). Source build: [docs/build_from_source.md](docs/build_from_source.md). Change logs: [docs/changelog/](docs/changelog/).

## Acknowledgements

- Film LUTs from [JanLohse/spectral_film_lut](https://github.com/JanLohse/spectral_film_lut).
- Camera color matrices from [rawtoaces-data](https://github.com/AcademySoftwareFoundation/rawtoaces-data).
- Neural demosaic student models distilled from [mgharbi/demosaicnet](https://github.com/mgharbi/demosaicnet) ([Gharbi et al., 2016](https://groups.csail.mit.edu/graphics/demosaicnet/)).
- Inpaint-opposed highlight reconstruction adapted from [darktable opposed.c](https://github.com/darktable-org/darktable/blob/master/src/iop/hlreconstruct/opposed.c) and [RawTherapee](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc).
- RCD demosaic from [LuisSR/RCD-Demosaicing](https://github.com/LuisSR/RCD-Demosaicing).
- OpenDRT ported from [jedypod/open-display-transform](https://github.com/jedypod/open-display-transform).
- ACES 2.0 from [aces-aswf/aces-core](https://github.com/aces-aswf/aces-core).
- Film grain based on [Realistic Film Grain Rendering](https://doi.org/10.5201/ipol.2017.192) (IPOL 2017).

## License

Alcedo Studio is licensed under the GNU General Public License v3.0 (GPL-3.0-only). See [LICENSE](LICENSE) and [NOTICE](NOTICE).
