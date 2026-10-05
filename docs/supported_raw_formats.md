# Supported RAW and Image Formats

Alcedo Studio can import the RAW and general image formats listed below. The RAW pipeline is built on a patched fork of [LibRaw](https://github.com/zidage/LibRaw), which provides the underlying decoder, metadata extraction, and camera-specific handling.

## Import File Extensions

A folder scan lists files by extension (case-insensitive). The file content then decides whether
import accepts a file: a JPEG renamed `.nef` imports as a JPEG, or is skipped when JPEG is not a
selected type. The table is the same as `ImportFileCategory` in
`alcedo_studio/src/include/type/supported_file_type.hpp`.

### RAW

| Extension | Format |
| --- | --- |
| `.3fr` | Hasselblad RAW |
| `.arw` | Sony α RAW |
| `.cr2` | Canon RAW 2 |
| `.cr3` | Canon RAW 3 |
| `.crw` | Canon RAW (CIFF) |
| `.dcr` | Kodak RAW |
| `.dng` | Adobe Digital Negative |
| `.erf` | Epson RAW |
| `.fff` | Hasselblad 3F / Imacon RAW |
| `.iiq` | Phase One RAW |
| `.kdc` | Kodak RAW |
| `.mef` | Mamiya RAW |
| `.mos` | Leaf RAW |
| `.mrw` | Minolta RAW |
| `.nef` | Nikon Electronic Format |
| `.nrw` | Nikon RAW (compact cameras) |
| `.orf` | Olympus / OM System RAW |
| `.pef` | Pentax RAW |
| `.raf` | Fujifilm RAW |
| `.raw` | Generic / Panasonic / Leica RAW |
| `.rw2` | Panasonic RAW |
| `.rwl` | Leica RAW |
| `.sr2` | Sony RAW 2 |
| `.srf` | Sony RAW |
| `.srw` | Samsung RAW |
| `.x3f` | Sigma RAW |

### Raster images

| Extensions | Format | Notes |
| --- | --- | --- |
| `.jpg` `.jpeg` `.jpe` `.jfif` | JPEG | 8-bit; CMYK is not supported |
| `.tif` `.tiff` | TIFF | 8/16-bit and float; a TIFF with camera data imports as RAW |
| `.png` | PNG | 8/16-bit; alpha is discarded |
| `.exr` | OpenEXR | Scene-linear; imported as HDR |

The source color (embedded ICC profile, PNG and EXR color tags) is read at import and converted
to the working space through the inverse ACES 2.0 output transform. The editor can override it
with the input profile menu.

## Underlying RAW Decoder Support

The bundled LibRaw fork covers the major vendor formats, including:

- **Canon**: CR2, CR3, CRN (embedded RAW), sRAW/mRAW
- **Nikon**: NEF, NEFX (Pixel Shift merged), and **Nikon High-Efficiency (HE / HE\*) NEF**
- **Sony**: ARW, compressed/uncompressed, and YCC pseudo-RAW
- **Fujifilm**: RAF, including X-Trans and 14-bit high-resolution files
- **Panasonic**: RW2 / .raw, including Panasonic encoding 8
- **Olympus / OM System**: ORF / high-resolution files
- **Leica, Hasselblad, Phase One, Pentax, Sigma, Samsung, Blackmagic, and others**
- **DNG 1.7** (including JPEG-XL compressed DNG when compiled with Adobe DNG SDK)
- **Smartphone and drone DNGs**: Apple, Google, DJI, Skydio, GoPro (via GoPro SDK)

> For camera-model-level compatibility, see [supported_cameras.md](supported_cameras.md).

## Exclusive: Nikon HE / HE\* NEF Support

Nikon High-Efficiency (`HE`) and High-Efficiency★ (`HE*`) NEFs use a JPEG-XS-like compressed codestream inside the standard TIFF/NEF container. As of the upstream LibRaw 0.22 release, these files are **not supported** for the affected cameras.

Alcedo Studio ships a patched LibRaw fork that decodes HE / HE\* NEFs without relying on Nikon NX Studio or any closed SDK. Validated samples include:

- Nikon Z 8
- Nikon Z 9
- Nikon Z 6 III
- Nikon Z 50 II

The decoder handles the dynamic per-precinct `Bp/Br` regimes that Nikon uses across these models, so both the smaller `HE` files and the even more compact `HE*` files can be opened, demosaiced, and graded inside Alcedo Studio.

The patched decoder lives in the project's LibRaw fork:  
**https://github.com/zidage/LibRaw**

## Export Formats

For export, Alcedo Studio supports:

| Format | Notes |
| --- | --- |
| JPEG | 8-bit, quality-controlled |
| PNG | 8/16/32-bit, compression level |
| TIFF | 8/16/32-bit, LZW/ZIP/no compression |
| WEBP | 8-bit, quality-controlled |
| Ultra HDR | JPEG with gain-map (Android Ultra HDR) |

HDR export can be written as an Ultra HDR gain-map JPEG or as an embedded-profile image, depending on the chosen export mode.
