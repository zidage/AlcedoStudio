# Deprecated legacy pipeline archive

This directory keeps the source files of the legacy stage pipeline and the legacy history store
after the GPU DAG replaced them. The files are **deprecated**.

- They are not built, formatted, tidied, installed, or packaged. No CMake target, source list,
  include directory, compile definition, install rule, package rule, `format` target, or `tidy`
  target refers to this directory.
- They may not compile against the current tree. Nothing in `alcedo_studio/src` or
  `alcedo_studio/tests` includes a file from here.
- Each file keeps its original path relative to `alcedo_studio/`. For example,
  `alcedo_studio/src/edit/pipeline/pipeline_stage.cpp` is now
  `alcedo_studio/deprecated/legacy_pipeline/src/edit/pipeline/pipeline_stage.cpp`. Files were
  moved with `git mv` and no content change, so `git log --follow` shows their history.
- Do not add a `CMakeLists.txt` here.
- Restoring any file into the build requires a new approved plan.

The static test `DeprecatedLegacyArchiveIsOutsideCompileGraph`
(`alcedo_studio/tests/ci/legacy_removal_source_checks.cmake`) fails when a CMake file names this
directory, when a CMake file is added here, or when the compile database lists a file from here.

## Groups and the commits that removed them from the build

The plan is [Phase G10 — Legacy Pipeline Removal](../../../docs/roadmap/alcedo_studio/edit/gpu_dag_final_removal_phase_plan.md).

| Phase | Group | Commit |
| --- | --- | --- |
| G10.1 | `tests/edit/pipeline/cuda_preview_vram_reclamation_test.cu` | `fd7dae19` |
| G10.2 | `tests/raw/opencl_cuda_rcd_compare_test.cpp` | `2536417b` |
| G10.4 | Legacy history store: edit-history object, table mapper, transactions, `Version`, the image journal and its writer port, the history management service, the materializer, the recovery metadata mapper, and their tests | `cdf356ee` |
| G10.7 | `include/edit/pipeline/pipeline.hpp` (stage interface) and the editor `pipeline_controller` | `99d79190` |
| G10.9 | The stage and `GPUPipelineWrapper`; the CUDA, OpenCL, and Metal legacy pipeline implementations; the legacy pipeline headers under `include/edit/pipeline/`; every legacy operator class with `op_base.hpp`, `op_kernel.hpp`, and `operator_factory`; the CPU and CUDA operator kernels and parameter headers; `LegacyPipelineImporter`; the unused CUDA AHD debayer; and the tests whose subject is one of these files | moved in `685ffc11`, build references removed in `6c87ec6a` |
| G10.10 | The legacy OpenCL `edit_pipeline` program manifest and its nine programs; `fused_pipeline.metal` and the eight legacy Metal operator shaders; `RawProcessor` with its CUDA, Metal, and OpenCL backends; `RawDecoder`; the CPU RAW operators; the RAW GPU wrappers that only `RawProcessor` called (CUDA downsample and rotate; Metal and OpenCL to-linear, RCD, X-Trans, highlight, and reference-space conversion; the Metal and OpenCL Neural Engine entry points); and the tests, benchmarks, and previews whose subject is one of these files | moved in `3ca68f1a` and `636c603b`, build references removed in `636c603b` |
