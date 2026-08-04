# Changelog

## [0.2.8] (edc738eb..b4e6fc4b) — 2026-07-09 ~ 2026-08-04

### Features
- **Neural-network demosaicing**: Added a neural-network RAW demosaic method on CUDA, OpenCL, and Metal, using a tiled engine and a smaller, faster model that runs in about 300 ms per image. (`7f71b11f`, `5126d486`, `9c27b3d6`, `fc8dcab2`, `153c09ba`, `83b779ff`, `d95e756e`)
- **Unified single-window editor**: Replaced the old modal editor dialog with an editor that lives in the main window next to the library. The viewport renders through Qt RHI with zero-copy GPU presentation on every backend, with real-time preview, zoom and pan up to 16x, a crop/rotate overlay, and a bottom filmstrip that collapses out of the way. The Tone, Look, LUT, Display Transform, Geometry, and RAW Decode panels, plus scopes and the history/versions rail, were all rebuilt for the new window. (`56b92f43`, `d8069e9d`, `3a474475`, `130cf29b`, `ef0a384f`, `7d4ca660`, `792b16e4`, `3c72e0c5`, `71ba6fe6`, `d0cba60a`, `0944993b`, `f7bad872`, `784209e3`)
- **Git-style edit history and autosave**: Replaced the linear history with a Git-like commit graph — each edit becomes an immutable commit, you can branch and check out versions, undo/redo walks the graph, and paste/merge create new branches with a conflict resolver. Edits autosave to a recovery journal and come back cleanly after a crash. (`801f6093`, `c492b517`, `fe4d8a70`, `a0761eff`, `5c12f636`, `3beaae99`, `2471ec83`, `b360cb49`, `b4e6fc4b`)
- **Startup GPU backend selection**: The accelerator (CUDA / OpenCL / Metal) is now picked once at startup and locked in, with the chooser moved to Settings; the CUDA pipeline was split into runtime DLLs and the OpenCL runtime is shared across them. (`729315c9`, `1c33a6fb`, `ce4cfabe`)
- **macOS color management**: Restored display color management on the macOS editor path and present Metal frames zero-copy. (`3ba62cb5`, `ef0a384f`)
- **New app icon**: Shipped a new application icon for Windows and macOS. (`5a77392a`)

### UI
- **Editor polish**: Restyled the history panel as a git-graph timeline, redesigned the merge-conflict resolver, made filmstrip thumbnails borderless to match the library grid, shared dark context-menu controls and image actions with the editor filmstrip, deepened the theme greys, and polished the background-tasks UX. (`4c731753`, `0c9d63cb`, `a7b63449`, `2a1fe331`, `fc623e21`)
- **Localization**: Restored the QML localization workflow for the new editor. (`98516636`)

### Bug Fixes
- **Highlight reconstruction edge speckle**: Fixed outlier pixels (dark/bright dots) along highlight–shadow boundaries in the CUDA, OpenCL, and Metal highlight-reconstruction operators. The clip decision is now a soft smoothstep transition over the top 5% below the clip level instead of a hard threshold, the neighbourhood reference average excludes censored (saturated) and non-finite samples, the global chrominance statistic uses a uniform 0.2·clip sampling-ring floor and is only trusted above 30 samples, and white-balance ratios are clamped to a sane band. OpenCL/Metal also skip the reference average for unclipped pixels and drop two unused mask planes.
- **JPEG export crash on Windows**: Fixed a crash when exporting JPEGs (an Exiv2 crash) by rewriting the APP1 metadata segment. (`181c9572`)
- **macOS build with OpenCV 5**: Dropped an unused OpenCV `calib3d` dependency so the build configures again on Homebrew OpenCV 5, which split that module. (`54b690e6`)

### Documentation & Distribution
- **README and website**: Rewrote the READMEs in a plain style, refreshed the website with RAW-editor screenshots and feature copy, and removed the old site. (`059585a2`, `4e70cc13`, `128896f6`, `58867300`)
- **Release signing**: Set up R2 signing and synced release installers to R2 storage. (`4b020663`, `7296a433`)

## [0.2.7] (65f25b2..91008037) — 2026-06-24 ~ 2026-07-06

### Features
- **Generalized AI sidecar architecture**: Rebuilt the AI sidecar around a protocol-first, multi-provider design — a runtime control plane, credential vault, cancellation registry, and capability descriptors; a provider config loader/registry with built-in and user configs plus Alcedo image-analysis task schemas; OpenRouter and Volcengine HTTP drivers over reqwest + rustls; a C++ host service and client with an image encoder and in-flight gate; a prefill queue; DuckDB-backed AI understanding/rating storage with FTS search; a protocol-first preset model and a generic OpenAI-compatible driver; an AiCredentialStore; a standalone QML ImageAnalysisController; and persistence/EXIF-star wiring for analysis results. (`7e9483f5`, `5d2c8dbc`, `65b87fcd`, `da59a47e`, `a5a54357`, `7f52fb61`, `db242253`, `6dc60306`, `02a1f102`, `5a0ecc3c`, `47e28839`, `ee399503`, `cf0d6e10`, `2770c83c`, `280d9494`, `e6180db7`, `c2aa3fbe`, `b951f2f1`, `4e9b95c0`, `2f3f440e`)
- **AI image rating**: Added AI star rating with a configurable rating-severity option (Lite/Normal/XHigh) and a segmented severity slider, threaded through the proto, server, every cloud provider, the controller, and the inspector UI. (`431c41c9`, `47e96ab2`, `78370f66`, `6d4358d3`)
- **Batch image analysis with schema repair**: Added batch image analysis in the OpenAI-compatible provider with automatic schema repair on validation failures, JSON sanitization/extraction utilities that tolerate markdown-fenced and prose-embedded JSON, and prompt-profile-driven system prompts. (`1d68ccbb`, `92c9cf5b`, `dd42af20`, `7de2cc2d`)
- **Codex OAuth provider**: Added an OpenAI Codex OAuth provider with streaming SSE responses, DPAPI-backed large-credential storage, and Windows proxy passthrough into the sidecar. (`befd5661`)
- **Background task management**: Added a background task registry with a task bar and popover, an interaction-policy controller and project DB write barrier for lock-driven task gating, run-in-background for semantic generation, analysis-rendition rendering from pipeline snapshots without disturbing the live guard, and an interactive, cancellable sidecar boot for UI responsiveness. (`1ed12de4`, `2a6b9b11`, `7bbe5ace`, `672b5634`, `311cd5c2`, `1b8397c7`)
- **Field-specific and natural-language search**: Added per-field search settings and natural-language search dispatching in the global search dialog. (`435ef26d`)
- **Color-temperature accuracy**: Generated a Planckian-locus uv table from the CIE 1931 CMF for more accurate correlated-color-temperature mapping, and populated as-shot neutral from LibRaw camera multipliers for a correct white-balance starting point. (`992f628e`, `79ae6d9c`)
- **DuckDB FTS and macOS credentials**: Added DuckDB FTS extension loading and FTS-backed AI understanding persistence with FTS/VSS extension packaging for macOS and Windows, and added macOS keychain credential handling for the AI credential store. (`ffe1d9cc`, `bdfbb104`)

### UI
- **AI provider settings UI**: Added a ccswitch-style provider selector (AiProviderProfile), a dedicated AI provider settings panel, a searchable model selector, and config-panel refinements. (`da44a3d7`, `a566ae91`, `d771c266`, `4b887d43`, `6dc35e0c`, `a46a107d`, `c70f2598`)
- **Advanced analysis and inspector UI**: Added the advanced content analysis launcher dialog, an image detail panel showing AI descriptions, an image/album inspector switch, context-menu actions, and new AI-feature icons. (`4f5394e1`, `7e06bf14`, `5fe50abd`, `dc01ce87`)
- **Background task UI**: Added an IconButton component, task kind badges, a Details button for running image-analysis tasks, and run-in-background controls in the semantic generation dialog. (`672b5634`, `7bbe5ace`)

### Bug Fixes
- **Editor and preview stability**: Fixed ROI render failure after crop/rotation. (`9a1d682f`)
- **Cloud AI security and correctness**: Hardened cloud AI calling (explicit opt-in for non-loopback binds, provider URL validation, bounded response reads, image payload limits), switched the OpenAI-compatible driver to structured tool output for image analysis, removed the max-token cap that starved reasoning models (e.g. kimi-k2.6) of output budget, and fixed schema-checking logic. (`db66809c`, `249474ef`, `ed44c13e`, `a3a331c4`)
- **Sidecar boot**: Fixed sidecar boot parameter inconsistency. (`7ef39d35`)
- **DuckDB extensions**: Fixed DuckDB extension packing logic and Windows extension handling. (`5c91d542`, `91008037`)
- **macOS packaging**: Fixed macOS signing and removed unsupported macOS models. (`5a1424ec`, `c840749a`)
- **Background task and AI dialog fixes**: Fixed the incomplete phase 1/2 background-task implementation and the AI dialog ask logic. (`d12c02a6`, `91008037`)

## [0.2.6] (8399448..c4cf768) — 2026-06-11 ~ 2026-06-24

### Features
- **Film grain and halation effects**: Added film grain density/granularity controls and halation operators, wired them into edit history, and brought the effect pipeline across CPU/CUDA/OpenCL/Metal paths with backend alignment work. (`514a667`, `5803529`, `e943264`, `c0b3043`, `6c19966`, `15d9228`, `6f24a66`, `f614dae`, `c84016b`, `bac945c`, `97778e7`)
- **Semantic AI generation and search**: Added semantic image content detection, AI label generation with multiple model support, multilingual/Jina CLIP handling, label search/filter, semantic search dispatching, VSS/HNSW storage, better label prompts, label assignment improvements, and pruning for unused model labels. (`0304551`, `ee94c48`, `84ad809`, `9d13093`, `c152e86`, `529e587`, `7a3b3ea`, `3745126`, `4e06e46`, `3baaad7`, `6b24957`)
- **Model download and activation workflow**: Added the model downloader UI, aria2c-based download service, ETA display during generation, model download/activation UX improvements, activation prompts, and a dedicated activation dialog. (`a20a1ba`, `100a4a8`, `157eb5`, `70af466`, `882ce4b`, `0444a80`, `b0029c`)
- **RAW and camera compatibility**: Added Nikon HE/HE* support through a patched LibRaw path, updated the LibRaw submodule, and added native CoreML model support on macOS. (`419fb74`, `3880f2c`, `5f7e288`, `631fb11`, `b7d50a1`)
- **Editor productivity polish**: Added LUT favorites, collapsible edit sections, and an About panel for the application. (`9570898`, `0d41c03`, `81ea7d6`)

### UI
- **AI workflow polish**: Improved AI label generation UI, tag display, file information display, model switching clarity, model activation prompts, and generation/download feedback. (`aa53477`, `223c24d`, `3bd23ce`, `c41b24a`, `70af466`, `0444a80`, `b0029c`)
- **Website refresh**: Updated website design, copy, screenshots, and release information for the 0.2.6 cycle. (`0028db6`, `a0d4c01`)

### Performance
- **Semantic data and model pipeline speedups**: Improved async model execution, optimized database insertion, removed redundant data copies, and refactored backend paths for better multimodel support. (`bce8b9e`, `9c800f9`, `f1ec59f`, `99be227`)
- **GPU processing optimizations**: Optimized Metal Highlights/Shadows adjustment performance and improved the film grain algorithm. (`7206724`, `97778e7`)

### Bug Fixes
- **Release and packaging stability**: Fixed macOS install dependency resolution and verification, HNSW project packaging, CI compile failures, missing VSS setup, Windows/macOS model packaging scope, Windows install script preparation, and process exit-code reporting. (`9cc06a3`, `60faf7e`, `8a688cd`, `66c306e`, `8493f02`, `4878464`, `c4cf768`)
- **RAW, editor, and GPU stability**: Fixed CUDA highlight reconstruction runtime-switch failures, editor deadlocks/freezes, random illegal memory access, LibRaw-related segmentation faults, and incorrect crop offsets for some cameras. (`9cc0da9`, `48bf884`, `7cf756c`, `5aea74b`, `2d4ca5a`, `3880f2c`, `4262737`)
- **AI/model correctness**: Fixed AI panel generation timing, aria2c retry handling, model-name display inconsistencies, excessive label counts, DB connection ownership, and merge-conflict regressions in the semantic branch. (`60255ae`, `622c06d`, `102e4d5`, `2936946`, `cf5a823`, `6a756c5`, `bb0852f`)

### Documentation
- Added semantic generation/search planning, CUDA halation/film-grain merge handoff notes, website text updates, sponsorship metadata, and release engineering scripts for macOS/Windows install-tree verification. (`7b045c2`, `8f0004a`, `0028db6`, `a0d4c01`, `5ef4433`, `4878464`)

## [0.2.5] (8eed4a0..377df62) — 2026-05-30 ~ 2026-06-11

### Features
- **Highlights/Shadows local tone overhaul**: Rebuilt the Highlights and Shadows adjustment around LLF-style local tone processing, stronger shadow detail recovery, better highlight strength scaling, and more consistent CUDA/OpenCL/Metal behavior. (`2b232fc`, `1bf39fd`, `b7263e1`, `8fc7adb`, `61b4b30`, `06dcef5`)
- **Color mixer quality upgrade**: Migrated the color adjustment path toward OKLCh, refactored HLS/chroma handling, and fused saturation work into the shared color kernels for smoother hue/saturation edits. (`892e34b`, `6d91abe`, `4ea033b`)
- **Batch adjustment transfer**: Added copy/paste adjustment transfer from the album workflow, including merge/paste modes, clearer dialog copy, and service-level tests for preserving version history. (`3e00552`, `a010503`)
- **HDR export overhaul**: Reworked HDR export metadata, SDR/HDR parameter handling, UltraHDR writer paths, export queue UI, and writer coverage for more reliable HDR output. (`7a3cd51`)

### UI
- **Geometry/crop panel refactor**: Reorganized crop, rotation, reset, and geometry controls into a clearer dedicated panel with updated editor/viewer interaction coverage. (`44f686d`)
- **Preview and interaction polish**: Added point-to-point zoom preview scaling and improved tone slider settled-state handling for more stable continuous edits. (`9a09948`, `38b4046`)

### Performance
- **Local tone mapping performance**: Optimized LLF execution, Metal local tone mapping, and mask resolution bounds to reduce cost while preserving quality. (`f079b56`, `839f2e1`, `908aeee`, `9ac5d55`)

### Bug Fixes
- **Preview and cache correctness**: Fixed ROI request failures during H/S and geometry edits, LLF blending across ROI/resolution changes, simplified masking cache behavior, and corrected tone-mapping mask cache consistency. (`4001c1c`, `f4ba2c0`, `ac073cb`, `42ed19b`)
- **Workflow state fixes**: Fixed DRT parameter loss during adjustment overwrite, album rating scroll-position resets, and light theme color LED inconsistencies. (`2e776de`, `82ffbba`, `8489e4c`)
- **macOS and CI stability**: Fixed macOS compile issues, macOS HDR preview overexposure, third-party dependency wiring, and CI compile/test coverage. (`15b1101`, `bb29bc1`, `cdd30dd`, `dca770f`)

### Documentation
- Added merge-handoff and refactor planning documentation for the color-adjustment and tone-mapping work. (`cb70a8b`, `2237a8e`)

## [0.2.4] (f599007..a3575d39) — 2026-04-29 ~ 2026-05-30

### Features
- **PR-based feature integration**: Starting with this release cycle, new capabilities were primarily landed through pull requests, including editor refactors, OpenCL backend work, collection updates, thumbnail caching, advanced search, loading polish, and database performance improvements. (`8d09af8`, `9babfb0`, `2548cd4`, `d359241`, `81ad481`, `ed9d3b7`, `d4090f3`, `54ab6d1`, `0ee2835`, `a3575d3`)
- **OpenCL acceleration path**: Added OpenCL image containers, context/program management, RAW processing, point/linear-reference operators, highlight reconstruction, LMT with 3D LUT support, DRT operators, geometry/lens-calibration support, DNG warp rectilinear handling, scope analysis, OpenGL sharing, runtime backend switching, and packaging/source-path resolution for OpenCL shader assets. (`590930a`, `ac9cbb8`, `e57ffbd`, `d9522de`, `03cf061`, `8d897b3`, `1b30204`, `771d45c`, `034cf0b`, `5b17d6d`, `a2acab9`, `21aa488`)
- **Editor panel refactor**: Split editor state and panel ownership into dedicated tone, RAW decoding, geometry, DRT, color, and versioning components; introduced the render session/coordinator structure; added more ODT options and refreshed advanced-parameter accordion behavior. (`aaa25e0`, `078910c`, `b50bf70`, `1dedfb1`, `7eb3fe1`, `6b391fa`, `7845476`, `b2949ed`, `6415d10`, `f934633`, `2660a11`)
- **Versioning and project package upgrades**: Reworked edit-history semantics with log-only versioning support, Merkle-tree version hashes, UUID persistence, project file version/checksum validation, database checksum computation, and refactored project package save/load behavior. (`387edbf`, `8bc35ee`, `e9e39ba`, `4d8b67a`, `b43e43b`, `2baaae8`, `ccebd09`)
- **Album collections and Sleeve services**: Overhauled collection membership, folder listing, pagination, filter-service integration, cache invalidation, import root validation, schema hardening, duplicate/history handling, and batch database add/delete interfaces. (`a54358f`, `0a77e71`, `4dacf2a`, `435141b`, `5964e73`, `4f0136c`, `e370d12`)
- **Image rating and advanced search**: Added application-wide star ratings, rating filters, album statistics integration, global search with fuzzy and exact modes, improved global-search panel behavior, and thumbnail/grid zoom and layout animation controls. (`d2ce4b7`, `8baf1ba`, `29aefb1`, `4ead40f`, `18bd093`, `f7b2611`)
- **Thumbnail cache and loading experience**: Introduced `AlbumThumbnailModel`, disk-backed thumbnail caching, resolution-separated thumbnail requests, 8-bit thumbnail storage, improved thumbnail loading/selection behavior, and smoother project/OpenCL loading states. (`1c36515`, `83368da`, `618c5f6`, `553274e`, `43daa9e`, `b30c463`, `26c3d25`, `11c2ade`, `88cd276`)

### UI
- **Editor and metadata UI polish**: Improved image details dialog i18n and layout, switched data display typography to IBM Plex, moved color-temperature state into the tone panel, and fixed LUT selection reset behavior. (`b2956bb`, `cbb992c`, `55f4d87`, `c3a95f8`)
- **Website and distribution polish**: Updated website download links and macOS installation scripts, including LUT asset shipping for macOS packages. (`99ba3b8`, `1567f0e`, `3f51efb`)

### Performance
- **Database and browser performance**: Added batch database mutation APIs and optimized collection/search/thumbnail model paths to reduce UI stalls during large project browsing. (`e370d12`, `26c3d25`, `a3575d3`)
- **Pipeline and thumbnail lifecycle improvements**: Refactored pipeline frame-sink attachment/lifecycle management, separated thumbnail resolutions, removed redundant image loading, and added targeted unit coverage for sink and cache behavior. (`63258c0`, `d6dd172`, `0d1b2bf`, `19e9b05`)

### Bug Fixes
- **Metal and RAW processing fixes**: Fixed macOS/OpenCL compile issues, Metal RCD and lens-calibration shader behavior, CUDA RCD margin handling, lensfun correction alignment, and Metal RCD test assertions. (`a5436da`, `1fa062b`, `25ad658`, `107dcbd`, `fc97b01`, `a21d7ac`)
- **Project and thumbnail stability**: Fixed project loading around checksum mismatches, thumbnail-generation crashes, global-search missing thumbnails, and LUT selection reset discrepancies. (`83a8e50`, `cf659da`, `d0ffe14`, `c3a95f8`)
- **macOS CI and packaging stability**: Added CI workflow and fixed macOS CI/runtime failures around OpenMP, third-party CMake wiring, test builds, lensfun compile/rpath handling, and local OpenMP runtime behavior. (`2351cea`, `2a76a46`, `7adf31b`, `4e6c1c6`, `a34e826`, `a4349da`, `b57bcbf`, `65c6698`, `977a93a`)

### Miscellaneous
- **WebGPU path retired**: Removed the experimental WebGPU RAW processing path after evaluating it, and redirected GPU backend work toward OpenCL. (`884cf15`)
- **Documentation and planning**: Added editor, pipeline frame-sink, and sleeve album-membership refactor plans, plus phase status updates for the collection refactor. (`b57e39f`, `88afee4`, `7ad0363`, `aa9f3db`, `afce1df`)
- **Packaging scope**: Windows and macOS packages now ship only the curated Kodak, Fuji, and Agfa `.cube` LUTs, excluding `spektrafilm` and other legacy sample LUTs.

## [0.2.3] (21046ec..fd3f8f2) — 2026-04-08 ~ 2026-04-26

### Features
- **Project rebranded to Alcedo Studio**: Renamed the project from Puerh Lab to Alcedo Studio across the codebase, UI, and website, and added a new welcome screen. (`abdfa38`, `0ebb546`, `cc02941`)
- **WebGPU RAW processing backend**: Added experimental WebGPU support to the image buffer and introduced a full WebGPU-accelerated RAW decode pipeline — including RCD demosaic shaders, linear reference op, skeleton backend, and RCD demosaic performance tuning. (`3db42c6`, `8caa858`, `d09d5e5`, `4d7e041`, `f3cdde3`, `ab32232`)
- **Windows preview surface migrated to D3D12**: Ported the Windows preview surface from D3D11 to D3D12 in preparation for WebGPU support. (`1573e4a`)
- **Forward matrix support for RAW color**: Added forward matrix to the RAW color context and metadata extraction pipeline for improved color accuracy on supported cameras. (`0578f9d`)
- **DNG import and metadata improvements**: Optimized DNG file import performance, enhanced UI components, added DNG metadata extraction tests, and improved the DNG Converter recovery menu design. (`614bac2`, `ef33ff67`, `7481f37`)
- **Clarity operator improvements**: Improved Clarity operator quality and aligned its behavior between macOS and Windows. (`d7a79fe`, `efd30e4`)
- **Viewer pointer operations**: Added pinch-to-zoom and pan input support to the image viewer. (`37f58f7`)
- **OCIO configuration enhancements**: Improved OCIO configuration handling and cross-platform path management. (`26096ac`)
- **LUT search and panel updates**: Added search support to the LUT selector and refreshed the LUT selection panel UI. (`5bb41c4`, `992bcd3`)
- **Inference backend migrated to ONNX**: Replaced the previous inference sidecar backend with ONNX Runtime. (`cf3a12e`)
- **i18n support for adjustment panels**: Added localization coverage for all adjustment panel strings. (`5e8920f`)
- **macOS installation script update**: Updated the macOS installation helper script. (`fd3f8f2`)

### UI
- **Comprehensive UI overhaul**: Overhauled the main theme and inspector panel, redesigned the tone, geometry, scope, versioning, and export panels, updated slider and folder styles, redesigned history cards, updated data display fonts, and added a pipeline profiler readout. (`4fb8ad7`, `7d4b345`, `5450a54`, `5f9153d`, `ba568cd`, `15e04a8`, `42599fa`, `3c1fbdb`, `4ccdc3d`, `6269a5c`, `9c3a10f`, `a96f7e7`, `3fa0b81`, `723e894`, `b87cbc8`, `aedd372`, `b273134`, `8e3297d`)

### Performance
- **VRAM optimization for large images**: Reduced peak VRAM consumption when processing 100MP+ images and capped the preview render resolution at 8K. (`1a9f09a`, `e49f5e6`)
- **Highlight reconstruction CUDA optimization**: Further tuned the CUDA highlight reconstruction kernel for improved throughput. (`4277677`)
- **Thumbnail and decode optimizations**: Optimized thumbnail downsample logic and general decode pipeline efficiency. (`4046e82`)

### Bug Fixes
- **RAW color matrix resolution fixes**: Fixed CCM resolution errors for DNG files and general camera matrix matching. (`83370a8`, `060d887`)
- **D3D12 preview crash**: Fixed a crash-to-desktop when initializing the D3D12 preview surface. (`11af467`)
- **Lens correction crop**: Fixed broken crop output after lens correction is applied. (`66d6fbb`)
- **Curve control behavior and rendering**: Fixed curve control interaction behavior and panel corner rendering. (`767bcf9`, `b591a56`)
- **Editor font rendering on Windows**: Fixed incorrect font rendering in the editor on Windows. (`919fc88`)
- **Miscellaneous UI fixes**: Fixed incorrect collapse/expand button color and inconsistent panel headline design. (`17fe748`, `b97e016`)

## [0.2.2] (6def338..17363e4) — 2026-03-22 ~ 2026-04-08

### Features
- **Nikon HE / HE* RAW recovery workflow**: Added Nikon HE-compressed NEF detection during import, a guided Adobe DNG Converter recovery dialog, automatic project cleanup/reimport after conversion, and macOS support for the same flow. Linear DNG inputs are now accepted, so converted files can go straight back into the RAW pipeline. (`b8e4962`, `dc86707`, `d32992d`, `0f85b8a`)
- **Highlight reconstruction and tone refactor**: Reworked RAW highlight recovery on CUDA and Metal into a multi-pass clipped-mask/chrominance-accumulation pipeline, and refactored Highlights/Shadows adjustments around a shared tone curve with new tests for knee behavior and chroma preservation. (`352d3d2`, `a4218c5`, `478205b`, `624cc24`)
- **LUT browser & Look panel redesign**: Rebuilt the editor side-panel layout, split out a dedicated Look panel, and added a LUT catalog/browser with `.cube` header validation, missing/invalid state display, quick folder open/refresh actions, and better selection persistence. (`955b47d`, `b8e4962`, `83583f0`)
- **Color, export, and metadata upgrades**: Added ICC profile embedding on Windows, expanded built-in export profile support, added EXIF details/source-path UI, added macOS scopes, and improved camera metadata resolution for tricky bodies such as Hasselblad. (`6def338`, `93c0b08`, `1f36cd6`, `da0102d`, `912dc2d`)
- **Experimental PuerhMind additions**: Added the Rust-based semantic/inference sidecar, CLIP text/vision services, simple image labeling, and a macOS inference demo path. (`73ad1c4`, `48a794c`, `a5ed1f7`, `edac38d`)

### Performance
- **High-resolution RAW decode acceleration**: Split the CUDA RAW path into dedicated full-frame and tiled execution modes, added active-area-aware crop handling, and reduced peak cost for very large Bayer files. (`624cc24`, `2d80f39`)
- **Less GPU copying and redundant work**: Added GPU buffer sharing and no-op detection for geometry stages so resize/crop passes can skip redundant work or avoid extra copies when possible. (`624cc24`, `2d80f39`)
- **Kernel fusion and intermediate reuse**: Combined highlight correction with RGBA packing, introduced reusable CUDA/Metal workspaces, tightened several low-level RAW kernels, and improved thumbnail / inference-side throughput. (`624cc24`, `2d80f39`, `22bb73b`)

### Bug Fixes
- **Tone and color stability**: Fixed the contrast `-100` all-black issue, corrected color temperature UI refresh behavior, and improved camera matrix matching for Hasselblad files. (`6294602`, `83583f0`, `912dc2d`)
- **Workflow and platform stability**: Fixed export dialog layout/parameter issues, added source-missing notifications in the album UI, and added CUDA driver version requirement probing on startup. (`9ad8384`, `d3083ff`, `4c21e10`, `17363e4`)

## [0.2.1] (044f948..6d0ff5b) — 2026-03-20 ~ 2026-03-20

### Features
- **CUDA support for X-Trans RAW**: Extended the GPU RAW decode path to Fuji X-Trans sources instead of limiting CUDA acceleration to classic Bayer cameras. (`044f948`)

## [0.2.0] (03344c0..b8c2fa3) — 2026-03-07 ~ 2026-03-14

### Features
- **Cross-platform rendering expansion**: Added macOS build support and integrated the Metal pipeline (briefly: raw/resize/lens utilities, pipeline wiring, and performance/refactor passes) (`5eed41d`, `0a37cfa`, `aefa6f0`)
- **macOS visual pipeline upgrades**: Added basic color management and experimental HDR support on macOS (`880234c`, `4c879c3`)
- **Windows preview backend update**: Ported Windows preview surface to D3D11 (`3a079ad`)
- **Internationalization**: Added i18n support, language selection UI adjustments, and zh-CN font updates (`2caeaed`, `9657bf5`, `44b5401`)
- **New scopes & controls**: Added histogram/waveform display, aspect ratio selection, thumbnail waiting animation, and reset adjustments support (`5f47c71`, `e559e1e`, `85a4440`, `2c18f7f`)
- **Versioning UI refresh**: Improved versioning UI design (`a0a5931`)

### Bug Fixes
- **Windows build stability**: Fixed multiple Windows compile issues during cross-platform integration (`e794c4e`, `a6d8968`, `2eca003`, `4f89c41`)
- **Metal pipeline path fix**: Corrected wrong geometry pipeline path in Metal (`34242aa`)
- **Renderer include/path fixes**: Updated include path handling for OpenGL viewer renderer (`cefe155`)
- **Editor background issue**: Fixed editor background issue in reset-adjustments workflow (`2c18f7f`)

### Documentation
- Added changelog documentation (`805996f`)
- Added demo website and updated project website content (`f6b76d8`, `2eff447`)
- Updated README content (performance data, removed outdated video link) (`1699e67`, `adc1912`)

### Miscellaneous
- Added website deployment GitHub Actions workflow (`385ecec`)
- Added/updated dependency submodules (`metal-cpp`, `libultrahdr`) and Windows support integration (`7ffd7c5`, `65e1372`)
- Removed unnecessary `third_party` folder cleanup (`b8c2fa3`)

## [0.1.2] (846e9d3..03344c0) — 2026-03-01 ~ 2026-03-07

### Features
- **OpenDRT support**: Added support for OpenDRT (Open Display Rendering Transform), licensed under GPLv3 (`8c9e62a`)
- **Rendering transform selection**: Added support for selecting different rendering transforms (RT) in the pipeline (`6d94167`)
- **Image deletion**: Added support for deleting images from the project (`197df08`)
- **Filter UI improvements**: Improved the filter UI for better usability (`874c93b`)
- **Project font change**: Changed the font used in the project UI (`6c4c6ad`)

### Bug Fixes
- **CCT/Tint resolution**: Fixed color correlated temperature (CCT) and tint resolution calculation (`2d1efc9`)
- **File name display**: Fixed file name display issues in the editor and exporter (`20fe29b`)
- **Raw processing race conditions**: Fixed race conditions during raw image processing (`9dd3e42`)
- **Color management resolution**: Fixed name normalization error in color management resolution (`665b442`)

### Documentation
- Updated README with lensfun installation details (`537670d`)
- Updated core libraries listing in README (`c691484`)
- Updated lensfun build documentation (`3a40dd0`)
- Updated source dependencies information (`c96a980`)
- General README updates (`6a233e7`)

### Miscellaneous
- Updated LICENSE back to GPLv3 (`03344c0`)

---

# 更新日志

## [0.2.8] (edc738eb..b4e6fc4b) — 2026-07-09 ~ 2026-08-04

### 新功能
- **神经网络去马赛克**：新增基于神经网络的 RAW 去马赛克方法，支持 CUDA、OpenCL 与 Metal，采用分块引擎与更小更快的模型，单张约 300 ms。(`7f71b11f`, `5126d486`, `9c27b3d6`, `fc8dcab2`, `153c09ba`, `83b779ff`, `d95e756e`)
- **统一单窗口编辑器**：用主窗口内的编辑器取代旧的模态编辑对话框，与图库同处一个窗口。预览区通过 Qt RHI 在各后端零拷贝呈现，支持实时预览、最高 16 倍缩放与平移、裁切/旋转叠加层，以及可向下收起的底部胶片栏。色调、Look、LUT、显示变换、几何、RAW 解码等调整面板，以及示波器和历史/版本栏，均针对新窗口重新构建。(`56b92f43`, `d8069e9d`, `3a474475`, `130cf29b`, `ef0a384f`, `7d4ca660`, `792b16e4`, `3c72e0c5`, `71ba6fe6`, `d0cba60a`, `0944993b`, `f7bad872`, `784209e3`)
- **Git 式编辑历史与自动保存**：用 Git 式提交图取代线性历史——每次编辑生成不可变提交，可分叉与切换版本，撤销/重做沿图行走，粘贴/合并创建新分支并提供冲突解决。编辑自动保存到恢复日志，崩溃后可干净还原。(`801f6093`, `c492b517`, `fe4d8a70`, `a0761eff`, `5c12f636`, `3beaae99`, `2471ec83`, `b360cb49`, `b4e6fc4b`)
- **启动时选择 GPU 后端**：加速器（CUDA / OpenCL / Metal）改为启动时一次性选择并锁定，选择器移至设置；CUDA 流水线拆分为运行时 DLL，OpenCL 运行时在各 DLL 间共享。(`729315c9`, `1c33a6fb`, `ce4cfabe`)
- **macOS 色彩管理**：在 macOS 编辑器路径上恢复显示色彩管理，并以零拷贝方式呈现 Metal 帧。(`3ba62cb5`, `ef0a384f`)
- **新应用图标**：为 Windows 与 macOS 启用全新应用图标。(`5a77392a`)

### 界面
- **编辑器打磨**：将历史面板重制为 git-graph 时间线，重新设计合并冲突解决器，胶片栏缩略图改为无边框以匹配图库网格，将暗色上下文菜单控件与图片操作共享给编辑器胶片栏，加深主题灰度，并改进后台任务体验。(`4c731753`, `0c9d63cb`, `a7b63449`, `2a1fe331`, `fc623e21`)
- **本地化**：为新编辑器恢复 QML 本地化流程。(`98516636`)

### 缺陷修复
- **Windows 导出 JPEG 崩溃**：修复导出 JPEG 时的崩溃（Exiv2 崩溃），改为重写 APP1 元数据段。(`181c9572`)
- **macOS 适配 OpenCV 5**：移除未实际使用的 OpenCV `calib3d` 依赖，使构建在 Homebrew 的 OpenCV 5（该模块被拆分）下能再次完成配置。(`54b690e6`)

### 文档与发布
- **README 与网站**：以平实风格重写 README，为网站补充 RAW 编辑器截图与功能文案，并移除旧站。(`059585a2`, `4e70cc13`, `128896f6`, `58867300`)
- **发布签名**：配置 R2 签名环境并将发布安装包同步至 R2 存储。(`4b020663`, `7296a433`)

## [0.2.7] (65f25b2..91008037) — 2026-06-24 ~ 2026-07-06

### 新功能
- **通用化 AI sidecar 架构**：围绕协议优先的多供应商设计重建 AI sidecar —— 运行时控制平面、凭据保险库、取消注册表与能力描述符；带内置与用户配置的供应商配置加载/注册表及 Alcedo 图像分析任务 schema；基于 reqwest + rustls 的 OpenRouter 与 Volcengine HTTP 驱动；带图像编码器与在途门控的 C++ 宿主服务与客户端；预填充队列；带 FTS 搜索的 DuckDB AI 理解/评分存储；协议优先的预设模型与通用 OpenAI 兼容驱动；AiCredentialStore；独立的 QML ImageAnalysisController；以及分析结果的持久化/EXIF 星标写入。(`7e9483f5`, `5d2c8dbc`, `65b87fcd`, `da59a47e`, `a5a54357`, `7f52fb61`, `db242253`, `6dc60306`, `02a1f102`, `5a0ecc3c`, `47e28839`, `ee399503`, `cf0d6e10`, `2770c83c`, `280d9494`, `e6180db7`, `c2aa3fbe`, `b951f2f1`, `4e9b95c0`, `2f3f440e`)
- **AI 图像评分**：新增 AI 星级评分，带可配置的评分严苛程度选项（Lite/Normal/XHigh）与分段严苛程度滑块，贯穿 proto、服务端、所有云端供应商、控制器与检查器 UI。(`431c41c9`, `47e96ab2`, `78370f66`, `6d4358d3`)
- **批量图像分析与 schema 修复**：在 OpenAI 兼容供应商中新增批量图像分析，校验失败时自动修复 schema；新增 JSON 净化/抽取工具以容忍 markdown 代码块与正文内嵌 JSON；并引入由 prompt profile 驱动的系统提示词。(`1d68ccbb`, `92c9cf5b`, `dd42af20`, `7de2cc2d`)
- **Codex OAuth 供应商**：新增 OpenAI Codex OAuth 供应商，支持流式 SSE 响应、基于 DPAPI 的大凭据存储，并将 Windows 代理设置传入 sidecar。(`befd5661`)
- **后台任务管理**：新增后台任务注册表与任务栏/弹出面板、用于锁驱动任务门控的交互策略控制器与项目数据库写屏障、语义生成的"移至后台"能力、基于流水线快照的分析渲染（不影响实时 guard），以及可交互、可取消的 sidecar 启动以保持 UI 响应。(`1ed12de4`, `2a6b9b11`, `7bbe5ace`, `672b5634`, `311cd5c2`, `1b8397c7`)
- **按字段与自然语言搜索**：在全局搜索对话框中新增按字段搜索设置与自然语言搜索分发。(`435ef26d`)
- **色温精准度**：基于 CIE 1931 CMF 生成 Planckian 轨迹 uv 表以更准确地映射相关色温，并从 LibRaw 相机乘数填充 as-shot neutral 以获得正确的白平衡起点。(`992f628e`, `79ae6d9c`)
- **DuckDB FTS 与 macOS 凭据**：新增 DuckDB FTS 扩展加载与基于 FTS 的 AI 理解持久化（并为 macOS/Windows 打包 FTS/VSS 扩展），以及 AI 凭据存储的 macOS 钥匙串处理。(`ffe1d9cc`, `bdfbb104`)

### 界面
- **AI 供应商设置 UI**：新增 ccswitch 风格的供应商选择器（AiProviderProfile）、专用 AI 供应商设置面板、可搜索模型选择器，以及配置面板细节改进。(`da44a3d7`, `a566ae91`, `d771c266`, `4b887d43`, `6dc35e0c`, `a46a107d`, `c70f2598`)
- **高级分析与检查器 UI**：新增高级内容分析启动对话框、展示 AI 描述的图像详情面板、图像/相册检查器切换、上下文菜单操作，以及新的 AI 功能图标。(`4f5394e1`, `7e06bf14`, `5fe50abd`, `dc01ce87`)
- **后台任务 UI**：新增 IconButton 组件、任务类型徽章、运行中图像分析任务的"详情"按钮，以及语义生成对话框中的"移至后台"控件。(`672b5634`, `7bbe5ace`)

### 缺陷修复
- **编辑器与预览稳定性**：修复裁切/旋转后 ROI 渲染失败的问题。(`9a1d682f`)
- **云端 AI 安全与正确性**：加固云端 AI 调用（非回环绑定需显式 opt-in、供应商 URL 校验、响应读取上限、图像载荷限制），将 OpenAI 兼容驱动切换为图像分析的结构化工具输出，移除耗尽推理模型（如 kimi-k2.6）输出预算的 max-token 上限，并修复 schema 校验逻辑。(`db66809c`, `249474ef`, `ed44c13e`, `a3a331c4`)
- **Sidecar 启动**：修复 sidecar 启动参数不一致的问题。(`7ef39d35`)
- **DuckDB 扩展**：修复 DuckDB 扩展打包逻辑与 Windows 扩展处理。(`5c91d542`, `91008037`)
- **macOS 打包**：修复 macOS 签名问题并移除不支持的 macOS 模型。(`5a1424ec`, `c840749a`)
- **后台任务与 AI 对话框修复**：修复阶段 1/2 后台任务实现不完整的问题，以及 AI 对话框询问逻辑。(`d12c02a6`, `91008037`)

## [0.2.6] (8399448..c4cf768) — 2026-06-11 ~ 2026-06-24

### 新功能
- **胶片颗粒与 Halation 效果**：新增胶片颗粒密度/粒度控制和 Halation 算子，接入编辑历史，并将效果流水线扩展到 CPU/CUDA/OpenCL/Metal 路径，补齐多后端一致性。(`514a667`, `5803529`, `e943264`, `c0b3043`, `6c19966`, `15d9228`, `6f24a66`, `f614dae`, `c84016b`, `bac945c`, `97778e7`)
- **语义 AI 生成与搜索**：新增图像内容识别、AI 标签生成、多模型支持、多语言/Jina CLIP 路径、标签搜索/过滤、语义搜索分发、VSS/HNSW 存储、标签提示词优化、标签分配逻辑优化，以及未使用模型标签清理。(`0304551`, `ee94c48`, `84ad809`, `9d13093`, `c152e86`, `529e587`, `7a3b3ea`, `3745126`, `4e06e46`, `3baaad7`, `6b24957`)
- **模型下载与激活流程**：新增模型下载器 UI、基于 aria2c 的下载服务、生成 ETA 显示、下载/激活体验优化、模型激活提示，以及专用的激活对话框。(`a20a1ba`, `100a4a8`, `157eb5`, `70af466`, `882ce4b`, `0444a80`, `b0029c`)
- **RAW 与相机兼容性**：通过 patched LibRaw 路径加入 Nikon HE/HE* 支持，更新 LibRaw submodule，并为 macOS 加入原生 CoreML 模型支持。(`419fb74`, `3880f2c`, `5f7e288`, `631fb11`, `b7d50a1`)
- **编辑器效率改进**：新增 LUT 收藏、可折叠编辑分区，以及应用 About 面板。(`9570898`, `0d41c03`, `81ea7d6`)

### 界面
- **AI 工作流打磨**：改进 AI 标签生成界面、标签展示、文件信息展示、模型切换说明、模型激活提示，以及生成/下载反馈。(`aa53477`, `223c24d`, `3bd23ce`, `c41b24a`, `70af466`, `0444a80`, `b0029c`)
- **网站更新**：更新网站设计、文案、截图和 0.2.6 发布信息。(`0028db6`, `a0d4c01`)

### 性能优化
- **语义数据与模型流水线提速**：改进异步模型执行，优化数据库插入，移除冗余数据复制，并重构后端路径以更好支持多模型。(`bce8b9e`, `9c800f9`, `f1ec59f`, `99be227`)
- **GPU 处理优化**：优化 Metal 高光/阴影调整性能，并改进胶片颗粒算法。(`7206724`, `97778e7`)

### 缺陷修复
- **发布与打包稳定性**：修复 macOS 安装依赖解析与校验、包含 HNSW 的项目包保存、CI 编译失败、VSS 配置缺失、Windows/macOS 模型打包范围、Windows 安装脚本准备，以及进程退出码上报问题。(`9cc06a3`, `60faf7e`, `8a688cd`, `66c306e`, `8493f02`, `4878464`, `c4cf768`)
- **RAW、编辑器与 GPU 稳定性**：修复 CUDA 高光恢复运行时切换失败、编辑器死锁/卡死、随机非法内存访问、LibRaw 相关段错误，以及部分相机裁切偏移错误。(`9cc0da9`, `48bf884`, `7cf756c`, `5aea74b`, `2d4ca5a`, `3880f2c`, `4262737`)
- **AI/模型正确性**：修复 AI 面板生成时机、aria2c 重试逻辑、模型名称显示不一致、标签数量过多、数据库连接所有权，以及语义分支合并冲突引入的回归。(`60255ae`, `622c06d`, `102e4d5`, `2936946`, `cf5a823`, `6a756c5`, `bb0852f`)

### 文档
- 新增语义生成/搜索规划、CUDA Halation/胶片颗粒合并交接说明、网站文案更新、赞助元数据，以及 macOS/Windows 安装树校验相关发布工程脚本。(`7b045c2`, `8f0004a`, `0028db6`, `a0d4c01`, `5ef4433`, `4878464`)

## [0.2.5] (8eed4a0..377df62) — 2026-05-30 ~ 2026-06-11

### 新功能
- **高光阴影局部色调重构**：围绕 LLF 风格局部色调处理重建高光/阴影调整，增强暗部细节恢复、高光力度控制，并统一 CUDA/OpenCL/Metal 路径表现。(`2b232fc`, `1bf39fd`, `b7263e1`, `8fc7adb`, `61b4b30`, `06dcef5`)
- **混色器质量升级**：将色彩调整路径迁移到 OKLCh 思路，重构 HLS/色度处理，并把饱和度处理融合进共享色彩 kernel，改善色相与饱和度调整稳定性。(`892e34b`, `6d91abe`, `4ea033b`)
- **批处理参数复制/粘贴**：新增相册侧调整参数复制与粘贴流程，支持 merge/paste 策略、清晰的对话框说明，以及保留版本历史的服务层测试。(`3e00552`, `a010503`)
- **HDR 导出重构**：重做 HDR 导出元数据、SDR/HDR 参数处理、UltraHDR 写出路径、导出队列 UI 与 writer 测试覆盖，提升 HDR 输出可靠性。(`7a3cd51`)

### 界面
- **几何/裁切面板重构**：将裁切、旋转、重置与几何控制整理到更清晰的专用面板，并补充编辑器/预览交互覆盖。(`44f686d`)
- **预览与交互打磨**：新增点对点放大预览缩放，并改进色调滑块 settled 状态处理，让连续调整更稳定。(`9a09948`, `38b4046`)

### 性能优化
- **局部色调性能优化**：优化 LLF 执行、Metal 局部色调映射与遮罩分辨率上限，在保持质量的同时降低处理成本。(`f079b56`, `839f2e1`, `908aeee`, `9ac5d55`)

### 缺陷修复
- **预览与缓存正确性**：修复高光阴影/几何调整时 ROI 请求失效、不同 ROI/分辨率下 LLF 混合不一致、遮罩缓存策略复杂化，以及 tone mapping 遮罩缓存不一致问题。(`4001c1c`, `f4ba2c0`, `ac073cb`, `42ed19b`)
- **工作流状态修复**：修复调整参数覆盖时 DRT 参数丢失、相册评分导致滚动位置重置，以及浅色主题下颜色指示灯不一致的问题。(`2e776de`, `82ffbba`, `8489e4c`)
- **macOS 与 CI 稳定性**：修复 macOS 编译问题、macOS HDR 预览过曝、第三方依赖管理与 CI 编译/测试覆盖问题。(`15b1101`, `bb29bc1`, `cdd30dd`, `dca770f`)

### 文档
- 补充色彩调整与 tone mapping 重构的合并交接和阶段计划文档。(`cb70a8b`, `2237a8e`)

## [0.2.4] (f599007..a3575d39) — 2026-04-29 ~ 2026-05-30

### 新功能
- **以 Pull Request 为主的新功能合并流程**：本轮版本开始，主要功能通过 PR 合并进入主线，包括编辑器重构、OpenCL 后端、集合改造、缩略图缓存、高级搜索、加载体验与数据库性能优化等。(`8d09af8`, `9babfb0`, `2548cd4`, `d359241`, `81ad481`, `ed9d3b7`, `d4090f3`, `54ab6d1`, `0ee2835`, `a3575d3`)
- **OpenCL 加速路径**：新增 OpenCL 图像容器、上下文/程序库管理、RAW 处理、点运算/线性参考空间算子、高光恢复、带 3D LUT 的 LMT、DRT、几何与镜头校正、DNG warp rectilinear、示波器分析、OpenGL 共享、运行时后端切换，以及 OpenCL shader 的安装与源码路径解析。(`590930a`, `ac9cbb8`, `e57ffbd`, `d9522de`, `03cf061`, `8d897b3`, `1b30204`, `771d45c`, `034cf0b`, `5b17d6d`, `a2acab9`, `21aa488`)
- **编辑器面板重构**：将编辑器状态与面板职责拆分到色调、RAW 解码、几何、DRT、色彩、版本等专用组件中，引入 render session / coordinator 结构，新增更多 ODT 选项，并调整高级参数折叠面板行为。(`aaa25e0`, `078910c`, `b50bf70`, `1dedfb1`, `7eb3fe1`, `6b391fa`, `7845476`, `b2949ed`, `6415d10`, `f934633`, `2660a11`)
- **版本管理与项目包升级**：重构编辑历史语义，加入 log-only versioning、Merkle tree 版本哈希、项目 UUID 持久化、项目文件版本/校验和校验、数据库校验和计算，并重构项目包保存/加载逻辑。(`387edbf`, `8bc35ee`, `e9e39ba`, `4d8b67a`, `b43e43b`, `2baaae8`, `ccebd09`)
- **相册集合与 Sleeve 服务**：重构集合成员关系、文件夹列表、分页、筛选服务集成、缓存失效、导入根目录校验、schema 加固、重复文件/历史管理，以及数据库批量新增/删除接口。(`a54358f`, `0a77e71`, `4dacf2a`, `435141b`, `5964e73`, `4f0136c`, `e370d12`)
- **图片评分与高级搜索**：新增全应用星级评分、评分筛选、相册统计集成、带模糊/精确模式的全局搜索，改进全局搜索面板，并加入缩略图网格缩放与布局动画控制。(`d2ce4b7`, `8baf1ba`, `29aefb1`, `4ead40f`, `18bd093`, `f7b2611`)
- **缩略图缓存与加载体验**：新增 `AlbumThumbnailModel`、磁盘缩略图缓存、按分辨率区分的缩略图请求、8-bit 缩略图存储，改进缩略图加载/选择行为，并优化项目与 OpenCL 加载状态。(`1c36515`, `83368da`, `618c5f6`, `553274e`, `43daa9e`, `b30c463`, `26c3d25`, `11c2ade`, `88cd276`)

### 界面
- **编辑器与元数据界面优化**：改进图片详情对话框的本地化与布局，将数据显示字体切换为 IBM Plex，把色温状态迁移到色调面板，并修复 LUT 选择重置问题。(`b2956bb`, `cbb992c`, `55f4d87`, `c3a95f8`)
- **网站与发布体验优化**：更新网站下载链接与 macOS 安装脚本，其中包括 macOS 包中随附 LUT 资源。(`99ba3b8`, `1567f0e`, `3f51efb`)

### 性能优化
- **数据库与浏览性能**：新增数据库批量写入接口，并优化集合、搜索、缩略图模型路径，减少大型项目浏览时的 UI 卡顿。(`e370d12`, `26c3d25`, `a3575d3`)
- **流水线与缩略图生命周期优化**：重构 pipeline frame sink 的挂载与生命周期管理，区分不同缩略图分辨率，移除重复图像加载，并补充针对 sink 与缓存行为的单元测试。(`63258c0`, `d6dd172`, `0d1b2bf`, `19e9b05`)

### 缺陷修复
- **Metal 与 RAW 处理修复**：修复 macOS/OpenCL 编译问题、Metal RCD 与镜头校正 shader 行为、CUDA RCD 边界处理、lensfun 校正对齐，以及 Metal RCD 测试断言。(`a5436da`, `1fa062b`, `25ad658`, `107dcbd`, `fc97b01`, `a21d7ac`)
- **项目与缩略图稳定性**：修复校验和不匹配时的项目加载、缩略图生成崩溃、全局搜索缩略图缺失，以及 LUT 选择重置异常。(`83a8e50`, `cf659da`, `d0ffe14`, `c3a95f8`)
- **macOS CI 与打包稳定性**：新增 CI workflow，并修复 macOS CI/runtime 中的 OpenMP、third-party CMake、测试编译、lensfun 编译/rpath、本地 OpenMP runtime 等问题。(`2351cea`, `2a76a46`, `7adf31b`, `4e6c1c6`, `a34e826`, `a4349da`, `b57bcbf`, `65c6698`, `977a93a`)

### 其他
- **WebGPU 路径退场**：在评估后移除实验性 WebGPU RAW 处理路径，并将 GPU 后端工作转向 OpenCL。(`884cf15`)
- **文档与规划**：新增编辑器、pipeline frame sink、Sleeve 相册成员关系等重构计划，并补充集合重构阶段状态。(`b57e39f`, `88afee4`, `7ad0363`, `aa9f3db`, `afce1df`)
- **打包范围**：Windows 与 macOS 包现在只随附 Kodak、Fuji、Agfa 三组精选 `.cube` LUT，排除 `spektrafilm` 与其他旧示例 LUT。

## [0.2.3] (21046ec..fd3f8f2) — 2026-04-08 ~ 2026-04-26

### 新功能
- **项目更名为 Alcedo Studio**：将项目从 Puerh Lab 更名为 Alcedo Studio，涵盖代码库、UI 及网站，并新增欢迎页面。(`abdfa38`, `0ebb546`, `cc02941`)
- **WebGPU RAW 处理后端**：新增实验性 WebGPU 支持，为图像缓冲区引入完整的 WebGPU 加速 RAW 解码管线，包括 RCD 去马赛克着色器、线性参考算子、骨架后端及 RCD 去马赛克性能优化。(`3db42c6`, `8caa858`, `d09d5e5`, `4d7e041`, `f3cdde3`, `ab32232`)
- **Windows 预览曲面迁移至 D3D12**：将 Windows 预览 Surface 从 D3D11 迁移到 D3D12，为 WebGPU 支持做准备。(`1573e4a`)
- **RAW 色彩正向矩阵支持**：在 RAW 色彩上下文和元数据提取管线中加入正向矩阵，提升支持机型的色彩精准度。(`0578f9d`)
- **DNG 导入与元数据改进**：优化 DNG 文件导入性能，增强 UI 组件，新增 DNG 元数据提取测试，并改进 DNG Converter 恢复菜单设计。(`614bac2`, `ef33ff67`, `7481f37`)
- **清晰度算子改进**：提升清晰度算子质量，并统一 macOS 与 Windows 平台的行为表现。(`d7a79fe`, `efd30e4`)
- **预览区手势操作**：为图像预览区添加捏合缩放与平移手势支持。(`37f58f7`)
- **OCIO 配置增强**：改进 OCIO 配置处理方式与跨平台路径管理。(`26096ac`)
- **LUT 搜索与面板更新**：为 LUT 选择器添加搜索支持，并刷新 LUT 面板 UI。(`5bb41c4`, `992bcd3`)
- **推理后端迁移至 ONNX**：将推理 sidecar 后端替换为 ONNX Runtime。(`cf3a12e`)
- **调整面板 i18n 支持**：为所有调整面板字符串补全本地化支持。(`5e8920f`)
- **macOS 安装脚本更新**：更新 macOS 安装辅助脚本。(`fd3f8f2`)

### 界面
- **全面 UI 大改版**：重做主题与检查器面板，重新设计色调、几何、波形、版本控制及导出面板，更新滑块与文件夹样式，重设历史记录卡片，更新数据显示字体，并添加管线性能分析输出。(`4fb8ad7`, `7d4b345`, `5450a54`, `5f9153d`, `ba568cd`, `15e04a8`, `42599fa`, `3c1fbdb`, `4ccdc3d`, `6269a5c`, `9c3a10f`, `a96f7e7`, `3fa0b81`, `723e894`, `b87cbc8`, `aedd372`, `b273134`, `8e3297d`)

### 性能优化
- **大尺寸图像 VRAM 优化**：降低 100MP 以上图像处理时的峰值 VRAM 消耗，并将预览渲染分辨率上限设定为 8K。(`1a9f09a`, `e49f5e6`)
- **高光恢复 CUDA 优化**：进一步调优 CUDA 高光恢复内核，提升处理吞吐量。(`4277677`)
- **缩略图与解码优化**：优化缩略图降采样逻辑及整体解码管线效率。(`4046e82`)

### 缺陷修复
- **RAW 色彩矩阵解析修复**：修复 DNG 文件的 CCM 解析错误及通用相机矩阵匹配问题。(`83370a8`, `060d887`)
- **D3D12 预览崩溃**：修复 D3D12 预览 Surface 初始化时的崩溃问题。(`11af467`)
- **镜头校正裁切**：修复应用镜头校正后裁切输出异常的问题。(`66d6fbb`)
- **曲线控制行为与渲染**：修复曲线控制交互行为和面板圆角渲染问题。(`767bcf9`, `b591a56`)
- **Windows 编辑器字体渲染**：修复 Windows 上编辑器中字体渲染错误。(`919fc88`)
- **其他 UI 修复**：修复收起/展开按钮颜色错误及面板标题设计不一致问题。(`17fe748`, `b97e016`)

## [0.2.2] (6def338..17363e4) — 2026-03-22 ~ 2026-04-08

### 新功能
- **Nikon HE / HE* RAW 恢复流程**：在导入阶段新增 Nikon HE 压缩 NEF 检测，加入引导式 Adobe DNG Converter 恢复对话框，支持转换后自动清理项目占位并重新导入；同时补齐 macOS 对同一流程的支持。线性 DNG 现在也可直接重新进入 RAW 管线。 (`b8e4962`, `dc86707`, `d32992d`, `0f85b8a`)
- **高光恢复与明暗部算法重构**：将 CUDA / Metal 上的 RAW 高光恢复重写为“裁剪掩码 + 色度统计 + 重建”的多阶段流程，并把 Highlights / Shadows 调整重构为共享色调曲线，补充了针对 knee 行为与色彩保持的测试。 (`352d3d2`, `a4218c5`, `478205b`, `624cc24`)
- **LUT 浏览器与 Look 面板大改版**：重做编辑器侧边栏结构，拆出独立 Look 面板，并新增 LUT 目录浏览器，支持 `.cube` 头信息校验、缺失/损坏状态提示、快速打开文件夹/刷新，以及更稳定的当前选择保持。 (`955b47d`, `b8e4962`, `83583f0`)
- **色彩、导出与元数据链路升级**：新增 Windows ICC profile 嵌入、补充内置导出 profile、加入 EXIF 详情/源路径显示、补齐 macOS scopes，并改进 Hasselblad 等机型的元数据解析。 (`6def338`, `93c0b08`, `1f36cd6`, `da0102d`, `912dc2d`)
- **实验性 PuerhMind 能力接入**：加入 Rust 语义/推理 sidecar、CLIP 文本与视觉服务、基础图片标注能力，以及 macOS 推理 demo。 (`73ad1c4`, `48a794c`, `a5ed1f7`, `edac38d`)

### 性能优化
- **高分辨率 RAW 解码提速**：将 CUDA RAW 路径拆分为 full-frame 与 tiled 两种执行模式，引入基于 active area 的裁切处理，显著降低超大尺寸 Bayer 文件的峰值开销。 (`624cc24`, `2d80f39`)
- **减少 GPU 拷贝与空操作**：为几何阶段加入 GPU buffer 共享与 no-op 检测，让 resize / crop 在可跳过时直接跳过，在可复用时避免额外拷贝。 (`624cc24`, `2d80f39`)
- **融合内核与中间结果复用**：将高光校正与 RGBA 打包合并执行，引入可复用的 CUDA / Metal workspace，并同步优化多处底层 RAW kernel、缩略图链路与推理侧吞吐。 (`624cc24`, `2d80f39`, `22bb73b`)

### 缺陷修复
- **明暗部与色彩稳定性**：修复 Contrast 为 `-100` 时整张图变黑的问题，修正色温 UI 刷新异常，并改进 Hasselblad 文件的相机矩阵匹配。 (`6294602`, `83583f0`, `912dc2d`)
- **工作流与平台稳定性**：修复导出对话框布局与参数交互问题，补充相册中源文件缺失提示，并在启动时增加 CUDA 驱动版本要求探测。 (`9ad8384`, `d3083ff`, `4c21e10`, `17363e4`)

## [0.2.1] (044f948..6d0ff5b) — 2026-03-20 ~ 2026-03-20

### 新功能
- **CUDA 支持 X-Trans RAW**：将 GPU RAW 解码能力从经典 Bayer 机型扩展到 Fuji X-Trans 源文件。 (`044f948`)

## [0.2.0] (03344c0..b8c2fa3) — 2026-03-07 ~ 2026-03-14

### 新功能
- **跨平台渲染扩展**：新增 macOS 编译支持并完成 Metal 流水线集成（简述：Raw/缩放/镜头校正能力接入、流水线贯通，以及性能优化与重构） (`5eed41d`, `0a37cfa`, `aefa6f0`)
- **macOS 视觉流水线升级**：新增 macOS 基础色彩管理与实验性 HDR 支持 (`880234c`, `4c879c3`)
- **Windows 预览后端更新**：将 Windows 预览 Surface 移植到 D3D11 (`3a079ad`)
- **国际化支持**：新增 i18n、优化语言选择 UI，并更新 zh-CN 字体 (`2caeaed`, `9657bf5`, `44b5401`)
- **新示波与控制能力**：新增直方图/波形显示、画幅比例选择、缩略图等待动画，以及重置调整支持 (`5f47c71`, `e559e1e`, `85a4440`, `2c18f7f`)
- **版本信息界面优化**：改进 versioning UI 设计 (`a0a5931`)
- **改动重置支持**：支持用户通过双击滑块来重置调整参数。

### 缺陷修复
- **Windows 构建稳定性**：修复跨平台集成过程中多处 Windows 编译问题 (`e794c4e`, `a6d8968`, `2eca003`, `4f89c41`)
- **Metal 流水线路径修复**：修复 Metal 中几何管线路径错误 (`34242aa`)
- **渲染器包含路径修复**：修复 OpenGL viewer renderer 的 include/path 处理 (`cefe155`)
- **编辑器背景问题**：修复重置调整流程中的编辑器背景问题 (`2c18f7f`)

### 文档更新
- 新增 changelog 文档 (`805996f`)
- 新增 demo 网站并更新项目网站内容 (`f6b76d8`, `2eff447`)
- 更新 README（性能数据、移除过时视频链接） (`1699e67`, `adc1912`)

### 其他
- 新增网站部署 GitHub Actions 工作流 (`385ecec`)
- 新增/更新依赖子模块（`metal-cpp`、`libultrahdr`）并集成 Windows 支持 (`7ffd7c5`, `65e1372`)
- 清理并移除不再需要的 `third_party` 目录 (`b8c2fa3`)

## [0.1.2] (846e9d3..03344c0) — 2026-03-01 ~ 2026-03-07

### 新功能
- **OpenDRT 支持**：新增 OpenDRT（开放显示渲染变换）支持，采用 GPLv3 许可证 (`8c9e62a`)
- **渲染变换选择**：支持在流水线中选择不同的渲染变换（RT） (`6d94167`)
- **图片删除**：新增从项目中删除图片的功能 (`197df08`)
- **筛选器 UI 改进**：优化筛选器界面，提升可用性 (`874c93b`)
- **项目字体更换**：更换了项目 UI 使用的字体 (`6c4c6ad`)

### 缺陷修复
- **CCT/Tint 分辨率**：修复了色温（CCT）和色调（Tint）分辨率的计算问题 (`2d1efc9`)
- **文件名显示**：修复了编辑器和导出器中文件名显示异常的问题 (`20fe29b`)
- **Raw 处理竞态条件**：修复了 Raw 图像处理过程中的竞态条件 (`9dd3e42`)
- **色彩管理分辨率**：修复了色彩管理分辨率中名称归一化错误 (`665b442`)

### 文档更新
- 更新 README，添加 lensfun 安装说明 (`537670d`)
- 更新 README 中的核心库列表 (`c691484`)
- 更新 lensfun 构建文档 (`3a40dd0`)
- 更新源码依赖信息 (`c96a980`)
- 常规 README 更新 (`6a233e7`)

### 其他
- 将许可证恢复为 GPLv3 (`03344c0`)
