# AGENTS.md

This file provides guidance to AI agents (Kimi, Claude, Codex, etc.) when working with code in this repository.

## Project Overview

**Alcedo Studio** is a RAW photo editor and digital asset management (DAM) system written in C++20. It features CUDA-accelerated (Windows) and Metal-accelerated (macOS) image processing and a DuckDB-backed asset management system ("Sleeve"). The entire UI uses Qt 6 QML / Qt Quick, including the album browser, editor workspace, adjustment panels, and dialogs, with C++ backends and rendering integration.

## Execution and Product Rules

### Complete the requested capability

Agents working in this repository must be decisive and implementation-driven. Do not respond to product or engineering direction with passive staged deferrals such as "first avoid this", "later maybe add this", "medium term", "long term", or similar framing that delays the requested capability after the user has made the product goal clear.

When the user names a concrete integration or capability, treat it as the target and work out the implementation path, constraints, tests, and risks directly. If there are real blockers, state them as concrete engineering facts and propose the closest viable implementation, not a soft retreat to a weaker product.

### No fallback without explicit authorization

Do **not** add, restore, or "temporarily" use any fallback, degraded path, silent substitute, or weaker stand-in unless the user has **explicitly** allowed that specific fallback in this conversation (or in an existing, already-landed product rule they pointed at).

This includes, and is not limited to:

- Lowering decode / render / quality settings to hide slowness (for example changing Interactive `DecodeRes::FULL` to `HALF` so Neural Engine or a slow GPU path does not run)
- Falling back from Neural Engine / GPU / CUDA to Legacy, CPU, another backend, or a cheaper operator when the requested path fails or looks expensive
- Catch-and-continue that swallows the real error and proceeds on a substitute implementation
- Preview-only, downsample-only, or "good enough for now" substitutes for a requested full-quality path
- Retrying a different algorithm, resolution, or backend after a failure without being told to

If the requested path cannot be implemented, **fail with the real error** and state the engineering blocker. Do not ship a weaker product and call it a fix. Performance of a CUDA **debug** build is not a reason to change product decode or quality policy.

## Code Style and Data Updates

These rules apply to all project-authored code, including tests and tooling. They are project-wide
requirements and do not depend on a task-specific skill being activated.

### Language, formatting, and member names

- Use C++20 and the repository's clang-format configuration (Google style, 100-column limit).
- Follow the repository's clang-tidy rules. Private members use a trailing `_`; public and
  protected members do not.
- Name types, functions, variables, and tests after the data they represent or the operation they
  perform. Apply the terminology rules below to identifiers as well as prose.

### Include the defining header; do not forward-declare by default

Do **not** add a `class` / `struct` / `enum` forward declaration to skip an include. Include the
header that defines the type.

A forward declaration is allowed only when every condition below is true:

- The type is used solely as a pointer or reference in that header (no members, `sizeof`,
  `std::optional`/`std::vector`/`std::unique_ptr` destruction, or inline method bodies that need
  a complete type).
- Including the defining header would create a real include cycle, or the header is a PIMPL / ABI
  boundary that must keep the type incomplete.
- The corresponding `.cpp` includes the defining header.

Compile-time savings, shorter includes, or "it only appears in a signature" is not enough. Qt and
other framework headers may already forward-declare their own types; do not copy that pattern into
Alcedo-owned headers.

### Update existing data through its owner

**Do not create snapshots, mirror structs, or temporary copies of existing data structures just
to read, pass, or update their state.** Renaming a copy to `State`, `Context`, or `Payload` does not
make it acceptable. The rule concerns duplicated state, not the spelling of `snapshot`.

Use this order when implementing a read or update:

1. Identify the existing data structure and the service or component that owns it.
2. Read through the owner's API or a const reference/view with a valid lifetime and appropriate
   synchronization. Do not expose mutable internals or retain references beyond their safe lifetime.
3. Apply changes through a focused operation on that owner. Pass the fields or change description
   needed for the operation instead of copying the entire object, editing the copy, and writing it back.
4. Validate inputs before mutation and preserve the owner's invariants. Publish notifications only
   after the complete update is visible.

An **atomic operation** here means one logically indivisible update: observers cannot see partial
state, and concurrent changes cannot be silently overwritten. Use the existing transaction, lock,
or owning-thread mechanism as appropriate. This does not require `std::atomic` for every field and
does not authorize unsynchronized in-place writes. Keep failure behavior explicit; a failed update
must not leave partially applied state.

### Necessary copies and snapshots

A snapshot is allowed only when a concrete requirement needs independent state, such as a
consistent point-in-time package export. Before introducing one:

- Explain why an owner operation, scoped read, or minimal change description cannot satisfy the
  requirement. Convenience, avoiding an API change, or a generic claim of thread safety is insufficient.
- Document the purpose, captured fields, owner, lifetime, and consistency mechanism at the defining
  type or creation site. Copy only the data the requirement needs.
- Define whether the captured state is immutable or independently editable, and how it is released.
  Do not write an old copy back over live state without explicit conflict handling.
- Reuse an existing representation that meets the requirement rather than adding another parallel
  representation. An existing snapshot API is not permission to introduce more snapshots.

Ordinary scalar/value parameters and results are not prohibited. Review copies that duplicate an
existing object's state, especially whole objects, containers, and image buffers. Required algorithm
output buffers are not snapshots merely because they use separate storage.

## Naming and Terminology

### Scope and exceptions

The table below defines the scope of each restriction. Matching is case-insensitive and includes
compound identifiers and derived forms where applicable. Use names that state the actual operation,
artifact, comparison, or guarantee; replacing one vague label with another does not satisfy the rule.

Exact external API/framework/model identifiers are allowed where interoperability requires their
spelling. Keep that spelling at the integration site; do not propagate it into Alcedo-owned names or
surrounding prose. These rules may quote prohibited terms to define or explain the prohibition.

### Prohibited terms and concrete replacements

| Term | Scope | Required wording or behavior |
| --- | --- | --- |
| `hydration`, `hydrate`, and derived forms | Project-authored identifiers, tests, comments, documentation, plans, and user-facing text | Name the operation: read, load, populate, restore, or apply. |
| `gesture` and derived forms | Project-authored identifiers, tests, comments, documentation, plans, and user-facing text | Name the actual input: drag, pinch, input sequence, pointer release, or settled edit. |
| `golden` | Project-authored identifiers, test/target names, filenames, comments, documentation, plans, and user-facing text | State what is stored or verified: expected pixel values, serialized output, a reference image comparison with a stated tolerance, or a specific rendering result. |
| `smoke` | Test names, targets, files, and documentation | State the behavior and expected result. A name that only means "something ran" is insufficient. |
| `contract`, `contracts` | Everything under `docs/roadmap/`, including prose, headings, link labels, and filenames | Name the exact artifact or guarantee: interface, API, schema, protocol, invariant, behavior specification, acceptance criterion, compatibility requirement, or performance target. |
| `seed` used as a verb for a non-random operation | Project-authored code identifiers, tests, and comments | Use populate, insert, initialize, restore, copy, or apply. Conventional inputs to random-number generators, cryptographic primitives, and deterministic fuzz runs remain allowed. |

### Test and reference-data names

Every test must state the behavior, regression, invariant, or property it verifies. Unit,
integration, regression, property, and benchmark are useful categories, but each still needs an
explicit assertion goal.

Examples of concrete names:

- `NeuralEngineDemosaicsRealBayerRawPatchToValidRgb`
- `LoadFailureReportsErrorAndKeepsCacheCold`
- `RenderedPixelsMatchReferenceWithinTolerance`
- `SerializedEditHistoryMatchesExpectedJson`

For stored expected results, name the actual contents and purpose, such as
`expected_edit_history.json` or `exposure_plus_one_expected_pixels.exr`. A file comparison alone
does not explain the behavior being checked: the test must identify the output, comparison rule,
and tolerance where relevant. Do not mechanically replace `golden` with `baseline` or `reference`
without making those details clear.

### Checks before completing relevant edits

- Search first-party source, tests, docs, and plans for prohibited project-wide terminology. Review
  matches against the scope and external-identifier exception above.
- Replace prohibited names in touched code and update their callers, test registrations, and links
  together. When touching a non-conventional `seed` verb, rename it in the same change.
- For roadmap edits, search the entire `docs/roadmap/` tree, including filenames. Rename violating
  linked files and update their references.
- Review added copies and snapshot types against the data-update rules above; verify that each
  exception has a concrete need and a defined consistency mechanism.
- Review new `class` / `struct` / `enum` forward declarations in touched first-party headers.
  Replace them with the defining include unless the cycle / PIMPL exception above applies.

## Workspace and Branches

### Branch names

Branch names must describe the feature, fix, refactor, or other engineering purpose of the work.
Do not include `codex` in a branch name. Use a functional name such as
`fix/neighbor-operator-ping-pong` or `feature/opencl-program-cache`.

### Temporary files and local state

Do **not** create temporary directories or ad-hoc dump files at the repository root
(for example `/tmp`, `tmp/`, root-level `*.log`, harness dumps, one-off scripts, or
phase review JSON/CSV dumps).

**Always use `build/tmp/`** (create it if needed). This covers:

- configure/build/test logs
- CMake/`ctest` manifest captures and review evidence
- temporary scripts and intermediate tooling output
- any other short-lived working files that must not be committed

`build/` is gitignored. Prefer paths such as `build/tmp/<task_name>/...` so tasks
do not overwrite each other. Do not commit contents of `build/tmp/`.

Agent tool local state (`.uv-cache/`, `.uv-python/`, `.scratch/`, `skills-lock.json`)
is also gitignored. Shared repository skills are canonical under `.agents/skills/` and remain
trackable. Do not copy them into `.claude/skills/` or `.codex/skills/`; those tool directories are
reserved for local state and ignored through their nested `.gitignore` files.

## Build Commands

### Windows (MSVC + CUDA)

```bash
# Configure (debug)
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"

# Build (debug)
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4

# Configure + build (release)
cmd /c scripts\msvc_env.cmd --preset win_release -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4

# Install + package (using convenience script; auto-detects WiX/NSIS)
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1 -BuildDir build/release -Preset win_release

# Or manually:
cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install
cpack --config build/release/CPackConfig.cmake
```

### macOS (Metal)

```bash
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main

cmake --preset macos_release
cmake --build --preset macos_release
cmake --install build/macos-release
cd build/macos-release && cpack -G DragNDrop
```

### Formatting & Static Analysis

```bash
cmake --build build/debug --target format   # clang-format (Google style, 100-col)
cmake --build build/debug --target tidy     # clang-tidy with auto-fixes
```

### Tests

```bash
# Run all tests
ctest --test-dir build/debug --output-on-failure

# Run a single test binary (example)
./build/debug/tests/test_exposure_op
```

Test names and reference-data files must follow **Naming and Terminology** above.

WebGPU RAW tests must heap-allocate `LibRaw` raw processors (for example with
`std::make_unique<LibRaw>()`). Do not stack-allocate `LibRaw` in WebGPU-related tests; Dawn +
LibRaw test paths have hit stack overflows in this repository.

## Architecture

The codebase follows a strict layered architecture. Higher layers depend only on the layer directly below them.

### Layer 1 — Core Data Structures (`image/`, `include/image/`)
- **Image / ImageBuffer**: Core image representation with embedded metadata
- **ImagePool**: 3-tier LRU cache (metadata → thumbnail → full-res) coordinating memory across the app

### Layer 2 — Image Processing Pipeline (`edit/`)
- **EditPipeline**: Orchestrates ~30 edit operators with CPU / CUDA / Metal execution paths
- **Operators** (`edit/operators/`): One file per operation (exposure, contrast, curves, color temp, LUT, lens calibration, crop/rotate, ACES output, etc.)
- **GPU kernels**: CUDA sources in `edit/operators/GPU_kernels/cuda/`; Metal shaders in `edit/operators/GPU_kernels/metal_shader/` compiled to `.metallib` at build time
- **EditHistory / Version**: Git-like version tree with unlimited undo/redo and branching

### Layer 3 — Application Services (`app/`, `include/app/`)
These façade services are the **only** API surface the UI layer may call. They insulate the UI from all infrastructure changes:
- `ProjectService`, `ImportService`, `ThumbnailService`, `ExportService`
- `EditHistoryMgmtService`, `PipelineMgmtService`
- `SleeveFilterService`, `FSService`, `SleeveManager`

### Layer 4 — Asset Management / "Sleeve" (`sleeve/`, `storage/`)
- **SleeveFS**: DuckDB-backed virtual filesystem with inode-like abstraction
- **SleeveFile / SleeveFolder**: Hierarchy nodes with metadata bindings
- **Storage**: DuckDB ORM layer with mappers and controllers (`storage/`)

### Layer 5 — UI (`ui/`)

- **alcedo_main**: Application entry point and unified QML / Qt Quick shell
- **alcedo_main/qml/**: Album browser, editor workspace, adjustment panels, version/history views, scopes, and dialogs
- **alcedo_main/album_backend/**: C++ models and controllers exposed to QML for library and editor operations
- **editor_rhi/**: `EditorViewportItem` and rendering integration for the Qt Quick editor viewport

## Key Technical Notes

- **Qt path is hardcoded** in `CMakeLists.txt` (~line 142) to `D:/misc/Qt/6.9.3/msvc2022_64`. Override with `-DCMAKE_PREFIX_PATH`.
- **Submodules** (`third_party/lensfun`, `third_party/libultrahdr`, `third_party/QuickQanava`) must be initialized before configuring: `git submodule update --init --recursive` for lensfun/libultrahdr, and `git submodule update --init alcedo_studio/src/third_party/QuickQanava` (no nested checkout).
- **Windows packages** are resolved via vcpkg; macOS via Homebrew.
- **CUDA** requires Toolkit 12.8 and compute capability ≥ 6.0. CUDA files have their own compile database entry.
- **SIMD**: The C++ build uses AVX/AVX2 SIMD flags.
- **32-bit float pipeline**: All internal image processing operates in 32-bit float; output rendering uses ACES 2.0 with optional CUBE LUT.

## Skills

Skills are reusable, composable capabilities that enhance agent abilities. Each skill is a self-contained directory with a `SKILL.md` file.
The canonical, complete catalog is `.agents/skills/`; the summaries below are highlights and are not
an exhaustive list. Agents should read the matching canonical `SKILL.md` before using a skill.

### alcedo-msvc-cmake
Use when working on alcedo with CMake on Windows/MSVC, especially when the user mentions MSVC, Windows, presets, Ninja, CUDA, or `scripts/msvc_env.cmd`, or when an agent would otherwise run bare cmake commands in this repository.

**Workflow:**
- Run commands from the repository root.
- Prefer the presets in `CMakePresets.json`: `win_debug` and `win_release`.
- Do not invoke bare `cmake` directly for configure/build/install when `scripts/msvc_env.cmd` is available.
- Use `cmd /c scripts\msvc_env.cmd ...` so Visual Studio, MSVC, and CUDA environment variables are initialized first.

**Command Templates:**
- Configure debug: `cmd /c scripts\msvc_env.cmd --preset win_debug`
- Configure release: `cmd /c scripts\msvc_env.cmd --preset win_release`
- Build debug: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4`
- Build release: `cmd /c scripts\msvc_env.cmd --build --preset win_release --parallel 4`
- Install release: `cmd /c scripts\msvc_env.cmd --install build/release --prefix build/install`
- Run tests after a debug build: `ctest --test-dir build/debug --output-on-failure`

**Rules:**
- Append user-provided `-D...` cache entries to the configure command after the preset.
- If the user asks for a `cmake --build build/...` style command, translate it to the wrapper form instead of changing the build intent.

### alcedo-qml-ui
Use when adding or editing Alcedo QML under `alcedo_main` (workspace, editor adjustment panels, LUT/Tone/Look, AppTheme / DESIGN.md VI, toolbar SVGs, snapshot restore). Canonical path: `.agents/skills/alcedo-qml-ui/SKILL.md`.

**Rules (summary — full skill is authoritative):**
- Production style is **Basic**, never Material for dense editor chrome.
- All colors/spaces/radii/type/icon sizes come from `appTheme` + `DESIGN.md`; new values go into AppTheme and DESIGN in the same change.
- Prefer `IconActionButton` over Material/Controls Button for structural SVGs.
- Each adjustment panel owns `loadFromSnapshot`; bind selection via a panel-level `selectedPath` (or equivalent) property so workspace re-entry restores highlight.
- Do not rebuild lists or force `ListView.Contain` on click selection (contentY jumps).
- Register new QML in `ALCEDO_MAIN_QML_FILES`; panel keys must be accepted by `NormalizeAdjustmentPanel`.
- If the wrapper fails before reaching CMake, inspect `scripts/msvc_env.cmd` before changing presets or toolchain arguments.

### raw-processor-module
Use when modifying the RAW Processor module in alcedo, shared Metal GPU utilities, or Metal RAW shaders and their CMake wiring.

**Workflow:**
- Keep RAW pipeline entrypoint changes in `alcedo_studio/src/decoders/processor/raw_processor.cpp`.
- Keep RAW GPU operator code under `alcedo_studio/src/decoders/processor/operators/gpu/`.
- For Metal implementations in the RAW Processor module, place shader sources in `alcedo_studio/src/decoders/processor/operators/gpu/metal_shader/`.
- When adding or renaming a RAW Processor Metal shader, update `alcedo_studio/src/CMakeLists.txt` so the `.metal` file is compiled to `.air`, linked to `.metallib`, added to `RawProcessorOpMetalShaders`, and exposed to the matching C++ source via `target_compile_definitions(...)`.
- Keep shared Metal image geometry helpers such as crop, resize, and warp outside `edit/operators/`; place them under `alcedo_studio/src/metal/metal_utils/` with a dedicated utility name such as `geometry_utils`, and keep operators focused on orchestration.

**Rules:**
- Match RAW Metal operator behavior to the corresponding CPU or CUDA implementation before changing pipeline flow.
- Prefer adding dedicated RAW operator entrypoints instead of putting Metal shader dispatch directly into `raw_processor.cpp`.
- If a RAW Metal operator changes output format or dimensions, update the RAW Processor integration and any RAW-stage tests in the same change.
- Do not create Metal compute pipelines on every operator invocation. Shared Metal utilities must retrieve immutable pipeline states through `ComputePipelineCache` (or an equivalent centralized cache) so they are safely reused across concurrent command buffers.

### gpt-taste
Elite UX/UI & Advanced GSAP Motion Engineer. Enforces Python-driven true randomization for layout variance, strict AIDA page structure, wide editorial typography (bans 6-line wraps), gapless bento grids, strict GSAP ScrollTriggers (pinning, stacking, scrubbing), inline micro-images, and massive section spacing.

Key directives:
- Simulate Python RNG before writing UI code to select Hero Architecture, Typography Stack, Component Architectures, and GSAP Paradigms.
- Follow AIDA structure: Attention (Hero), Interest (Bento), Desire (GSAP Scroll/Media), Action (Footer/Pricing).
- Hero H1 must never exceed 2-3 lines; use ultra-wide containers (`max-w-5xl`, `max-w-6xl`).
- Use `grid-flow-dense` on every Bento Grid; mathematically verify zero empty spaces.
- All motion must use real GSAP (`@gsap/react`, `ScrollTrigger`).
- Ban meta-labels like "SECTION 01", "QUESTION 05", etc.
- No emojis in code, comments, or output.

### high-end-visual-design
Teaches the AI to design like a high-end agency. Defines the exact fonts, spacing, shadows, card structures, and animations that make a website feel expensive. Blocks all common defaults that make AI designs look cheap or generic.

Key directives:
- Banned fonts: Inter, Roboto, Arial, Open Sans, Helvetica. Use premium fonts like `Geist`, `Clash Display`, `PP Editorial New`, or `Plus Jakarta Sans`.
- Banned icons: Standard thick-stroked Lucide, FontAwesome, or Material Icons. Use ultra-light, precise lines (e.g., Phosphor Light, Remix Line).
- Banned borders & shadows: Generic 1px solid gray borders, harsh dark drop shadows.
- Banned layouts: Edge-to-edge sticky navbars, symmetrical boring 3-column Bootstrap-style grids.
- Banned motion: Standard `linear` or `ease-in-out` transitions.
- Use "Double-Bezel" nested architecture for cards (outer shell + inner core).
- Use custom cubic-bezier curves for all transitions.
- Scroll entry animations via `IntersectionObserver` or Framer Motion's `whileInView`.
- GPU-safe animation: only `transform` and `opacity`.

### minimalist-ui
Clean editorial-style interfaces. Warm monochrome palette, typographic contrast, flat bento grids, muted pastels. No gradients, no heavy shadows.

Key directives:
- Banned fonts: Inter, Roboto, Open Sans. Use `SF Pro Display`, `Geist Sans`, `Helvetica Neue`, `Switzer`, or editorial serifs like `Lyon Text`, `Newsreader`, `Playfair Display`, `Instrument Serif`.
- Banned icons: Lucide, Feather, standard Heroicons. Use Phosphor Icons (Bold or Fill) or Radix UI Icons.
- Canvas: Pure White `#FFFFFF` or Warm Bone `#F7F6F3` / `#FBFBFA`.
- Structural borders: Ultra-light gray `#EAEAEA` or `rgba(0,0,0,0.06)`.
- Accents: Highly desaturated pastels only (Pale Red `#FDEBEC`, Pale Blue `#E1F3FE`, Pale Green `#EDF3EC`, Pale Yellow `#FBF3DB`).
- No gradients, neon colors, or 3D glassmorphism.
- No `rounded-full` for large containers, cards, or primary buttons.
- No emojis anywhere.
- Motion: quiet sophistication — fade-up on scroll (`translateY(12px)` + `opacity: 0`, 600ms, `cubic-bezier(0.16, 1, 0.3, 1)`).

### redesign-existing-projects
Upgrades existing websites and apps to premium quality. Audits current design, identifies generic AI patterns, and applies high-end design standards without breaking functionality. Works with any CSS framework or vanilla CSS.

Key workflow:
1. **Scan** — Read the codebase. Identify framework, styling method, and current design patterns.
2. **Diagnose** — List every generic pattern, weak point, and missing state.
3. **Fix** — Apply targeted upgrades working with the existing stack. Do not rewrite from scratch.

Audit areas: Typography, Color and Surfaces, Layout, Interactivity and States, Content, Component Patterns, Iconography, Code Quality.

Fix priority order:
1. Font swap
2. Color palette cleanup
3. Hover and active states
4. Layout and spacing
5. Replace generic components
6. Add loading, empty, and error states
7. Polish typography scale and spacing

Rules:
- Work with the existing tech stack. Do not migrate frameworks or styling libraries.
- Do not break existing functionality. Test after every change.
- Before importing any new library, check the project's dependency file first.
- If the project uses Tailwind, check the version (v3 vs v4) before modifying config.
- If the project has no framework, use vanilla CSS.
- Keep changes reviewable and focused. Small, targeted improvements over big rewrites.
