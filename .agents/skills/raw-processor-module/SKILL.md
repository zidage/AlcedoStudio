---
name: raw-processor-module
description: Use when modifying the RAW Processor module in alcedo, shared Metal GPU utilities, or Metal RAW shaders and their CMake wiring.
---

# Raw Processor Module

Use this skill when working under `alcedo_studio/src/decoders/processor/` for RAW decode and
demosaic work.

## Workflow

- Keep RAW pipeline entrypoint changes in
  `alcedo_studio/src/decoders/processor/raw_processor.cpp`.
- Keep RAW GPU operator code under `alcedo_studio/src/decoders/processor/operators/gpu/`.
- For Metal implementations in the RAW Processor module, place shader sources in
  `alcedo_studio/src/decoders/processor/operators/gpu/metal_shader/`.
- When adding or renaming a RAW Processor Metal shader, update
  `alcedo_studio/src/CMakeLists.txt` so the `.metal` file is compiled to `.air`, linked to
  `.metallib`, added to `RawProcessorOpMetalShaders`, and exposed to the matching C++ source via
  `target_compile_definitions(...)`.
- Keep shared Metal image geometry helpers such as crop, resize, and warp outside
  `edit/operators/`; place them under `alcedo_studio/src/metal/metal_utils/` with a dedicated
  utility name such as `geometry_utils`, and keep operators focused on orchestration.

## Rules

- Match RAW Metal operator behavior to the corresponding CPU or CUDA implementation before changing pipeline flow.
- Prefer adding dedicated RAW operator entrypoints instead of putting Metal shader dispatch directly into `raw_processor.cpp`.
- If a RAW Metal operator changes output format or dimensions, update the RAW Processor integration and any RAW-stage tests in the same change.
- Do not create Metal compute pipelines on every operator invocation. Shared Metal utilities must retrieve immutable pipeline states through `ComputePipelineCache` (or an equivalent centralized cache) so they are safely reused across concurrent command buffers.
