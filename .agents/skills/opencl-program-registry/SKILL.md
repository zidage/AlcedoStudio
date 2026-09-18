---
name: opencl-program-registry
description: Use when adding, moving, or reviewing Alcedo OpenCL operator kernels, OpenCL program manifests, raw_processor_opencl, OpenCL GPUPipelineImpl, scope analyzer OpenCL code, or the OpenClBackendProgramRegistry/OpenClProgramLibrary runtime registration flow.
---

# OpenCL Program Registry

Use this skill for Alcedo OpenCL backend work where kernels must be registered at app/project
lifecycle time and later compiled through `OpenClProgramLibrary`.

## Core Rule

Keep OpenCL program registration centralized and long-lived.

- Register OpenCL source programs through `OpenClBackendProgramRegistry`.
- Activate manifests through `RegisterOpenClBackendPrograms()`.
- Do not register programs from short-lived instances such as `RawProcessor`, `GPUPipelineImpl`,
  scope analyzer objects, or per-frame/per-image execution paths.
- Keep `OpenClProgramLibrary` generic: it owns descriptors, lazy build, warm-up, and program cache;
  it must not depend on RAW, edit pipeline, or scope analyzer modules.

## Lifecycle

Use this order for runtime preparation:

1. Register builtin OpenCL manifests in `RegisterBuiltinOpenClProgramManifests()`.
2. Activate all manifests with `OpenClBackendProgramRegistry::Instance().RegisterAllPrograms()`.
3. Initialize `OpenClContext`.
4. Warm up required startup programs through `OpenClProgramLibrary::WarmUpRequiredPrograms()`.

`PrepareOpenClRuntime()` should surface failures. `TryPrepareOpenClRuntime()` is a fallback probe and
must catch registration, context initialization, and warm-up failures by returning `false`.

## Manifest Pattern

Create module-specific manifest files that only describe OpenCL programs. They should not allocate
buffers, create processors, build pipelines, or run kernels.

Preferred locations:

- RAW processor: `alcedo_studio/src/decoders/processor/operators/gpu/opencl_raw_programs.cpp`
- Edit pipeline: `alcedo_studio/src/edit/pipeline/opencl_pipeline_programs.cpp`
- Scope analyzer: `alcedo_studio/src/edit/scope/opencl_scope_programs.cpp`
- Shared aggregation point: `alcedo_studio/src/opencl/opencl_backend_program_registry.cpp`

Example shape:

```cpp
void RegisterBuiltinOpenClProgramManifests() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpenClBackendProgramRegistry::Instance().RegisterManifest(OpenClProgramManifest{
        .name = "raw_processor",
        .programs =
            {
                OpenClProgramDescriptor{
                    .name = "raw_processor_core",
                    .source_paths = {ALCEDO_OPENCL_RAW_PROCESSOR_CORE_CL},
                    .build_options = "-cl-std=CL1.2",
                    .required_at_startup = false,
                },
            },
    });
  });
}
```

## Program Grouping

Register OpenCL programs by compilation unit, not by C++ operator object.

Good grouping examples:

- `raw_processor_core.cl`: to-linear, clamp, pack RGBA, inverse camera multipliers
- `raw_processor_debayer.cl`: Bayer RCD and X-Trans interpolation
- `raw_processor_highlight.cl`: highlight accumulation and reconstruction
- `edit_pipeline_fused.cl`: fused RGBA32F edit pipeline
- `edit_pipeline_detail.cl`: neighbor/detail passes such as blur/apply
- `scope_analyzer.cl`: histogram, waveform, vectorscope, chromaticity kernels

Avoid one kernel per program unless build options or dependency boundaries truly require it.

## Naming

Keep names centralized and stable.

- Define program names and kernel names in the owning OpenCL module header.
- Do not scatter string literals like `"raw_debayer_rcd"` throughout execution code.
- Use clear program names such as `raw_processor_core`, `edit_pipeline_fused`, or
  `scope_analyzer`.
- Manifest names are module names; program names are OpenCL compilation units.

## CMake

When adding OpenCL manifest source files:

- Add the manifest implementation to an OpenCL runtime/backend target, not to short-lived operator
  implementation targets when that would create dependency cycles.
- Keep manifest sources dependent only on OpenCL runtime headers and path/build-option constants.
- If `.cl` sources are read at runtime, make their paths available via compile definitions or a
  packaging/install resource path.
- Update packaging later if `.cl` files must be available outside the source tree.

## Tests

For registry tests, prefer mock manifests that point at temporary `.cl` files, then call
`RegisterOpenClBackendPrograms()` and execute real OpenCL kernels. This verifies the production
registration path without depending on unfinished RAW or pipeline kernels.

Typical validation:

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target OpenClRuntimeTest --parallel 4
ctest --test-dir build/debug -R "OpenCl(Runtime|ProgramLibrary)Test" --output-on-failure
```

Use the `alcedo-msvc-cmake` skill for Windows/MSVC configure and build commands.

## Anti-Patterns

- Do not call `OpenClProgramLibrary::RegisterProgram()` directly from `raw_processor_opencl.cpp`,
  `pipeline_opencl_impl.cpp`, or per-object constructors.
- Do not rely on static object registration for OpenCL programs.
- Do not make `OpenClProgramLibrary` include RAW, edit pipeline, scope analyzer, or UI headers.
- Do not compile or create kernels on every invocation when a runtime cache can own immutable state.
- Do not let app/UI layers know individual kernel or program names.
