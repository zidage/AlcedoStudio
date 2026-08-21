<p align="center">
  <img src="docs/header.png" alt="Alcedo Studio" width="25%"/>
</p>

[Project website](https://aoraw.org/) | [项目网页](https://aoraw.org/zh-cn/)

<p align="right"><a href="./README.md"><strong>English</strong></a> | <a href="./README.zh-CN.md">简体中文</a></p>

![License](https://img.shields.io/badge/License-GPLv3-blue)
![CUDA](https://img.shields.io/badge/CUDA-12.8-76B900)
![C++](https://img.shields.io/badge/C++-20-blue)

**Alcedo Studio** is a free, open-source photography workstation for the day after a shoot.

Import a card of RAWs and the library is ready to browse. Grade files up to 150 megapixels in 32-bit float on the GPU.

The project is one [DuckDB](https://duckdb.org/) file with extra metadata. Album structure and the full edit history live inside it. One file to move and keep. No mess.

What's more, the grade follows a film-industry picture pipeline. Camera RAW becomes a scene-referred image. You work in a log space the way a DI suite grades ACEScc. Looks arrive as film-emulation LUTs. A display rendering transform then forms the picture for the monitor.

Windows 10/11 x64 and Apple Silicon. Current release: [v0.2.9](https://github.com/zidage/AlcedoStudio/releases/tag/v0.2.9).

---

https://github.com/user-attachments/assets/d70cd10d-2045-42f3-a67d-97ab3ef9874b

https://github.com/user-attachments/assets/ae0d9773-220e-4901-90f6-1989f58b0462

## Features

**Cameras, then RAW quality.** Tested across Canon, Nikon, Sony, Fujifilm, Panasonic, OM System, Leica, Hasselblad, Phase One (including IQ4 150MP), Pentax, Sigma, and phone / drone DNG. Lists: [supported formats](docs/supported_raw_formats.md) and [supported cameras](docs/supported_cameras.md). Nikon HE and HE★ NEFs from the Z 8, Z 9, Z 6 III, and Z 50 II decode through the project's [LibRaw fork](https://github.com/zidage/LibRaw) with special performance optimization.

Pick the demosaic: Default, RCD, or Neural Engine (a distilled [DemosaicNet](https://groups.csail.mit.edu/graphics/demosaicnet/) on the GPU, Bayer and X-Trans). Highlight reconstruction uses an improved inpaint-opposed method originally from darktable and RawTherapee.

**Smooth at 2.5K@60.** Drag a slider and the preview holds 2.5K at 60 FPS. When you stop, it can reach 4K, so your high-megapixel camera still looks like itself. An RGB histogram and a waveform update with the picture and guide the next move. CUDA on NVIDIA, OpenCL on other Windows GPUs, Metal on macOS. A cache keeps that performance with low memory. You set the thumbnail disk cache: location, size, quality, on or off, no mysterious disk space occupancy.

**A display rendering transform forms the picture.** Scene-linear RAW holds color and dynamic range a monitor can't show as-is. A DRT (picture formation) is the look decision that maps that range onto the display. See [Chris Brejon on picture formation](https://chrisbrejon.com/articles/what-makes-a-good-picture-formation/) for more information about DRT. You can use ACES 2.0 or OpenDRT, grading toward sRGB, wide-gamut, or HDR: choose the encoding color space, the EOTF, and HDR peak luminance. On macOS you even get an HDR preview while you edit.

**Film-emulation LUTs.** [CUBE LUTs generated from real film-stock spectral response](https://github.com/JanLohse/spectral_film_lut), plus grain and halation designed with physical properties.

**Looks you can keep, copy, and walk back.** Named versions hold different looks on a photo so you can compare. Copy a look onto another photo, or merge fields from one look into another. Every adjustment is a step you can undo. That history stays in the project file, so the undo trail is still there next month.

**Export with highly customizable configurations.** Format, size, naming, metadata, ICC, and alpha, from the inspector. JPEG, PNG, TIFF, or EXR (up to 32-bit). Original pixels, longest edge, pixel bounds, or print size with DPI. File names can use source name, capture date, camera, lens, exposure, rating, and sequence.

**Review and rate.** Connect an LLM you already use (OpenAI-compatible, Anthropic, or Volcengine Ark). It writes a description, a 1–5 star rating, and a short reason into EXIF. You set how strict the review is.

**Tag it. Find it.** Local multilingual CLIP models label the library. Type a scene, a camera, a date, or a phrase. The inspector maps the same library by capture date, camera, lens, labels, and rating. You can manage the CLIP models yourself by downloading a new model, activating an existing model for your library, and deleting a downloaded model to free up space.

**The library stays fast on a big shoot.** DuckDB is built so a whole card of photos can answer at once. Filter by Saturday, a 35mm, and five stars, and the grid updates. The inspector can tell you how many frames you shot on each body that week.

FTS is there so the words in a description or tag work as a query: "red umbrella" hits the caption, not only the filename. HNSW is there so photos that look alike sit near each other: a phrase finds frames by meaning, even when nobody typed that filename.

Put the file where you want. Move it. Back it up.

## System requirements

- **Windows**: 10/11 x64. NVIDIA GPU (compute capability 6.0+) for CUDA; other GPUs use OpenCL.
- **macOS**: Apple Silicon (M1 or newer), macOS 13.3 or later, Metal.
- 8GB RAM minimum (16GB+ for large libraries).
- Release builds install signed updates from Settings → Updates.

## Documentation

User guides and build notes: [documentation site](https://zidage.github.io/AlcedoStudio_docs/docs/intro). Source build: [docs/build_from_source.md](docs/build_from_source.md). Development plans made by AI agents: [docs/roadmap/roadmap.md](docs/roadmap/roadmap.md). Change logs: [docs/changelog/](docs/changelog/).

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

Alcedo Studio is licensed under GPL-3.0-only. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
