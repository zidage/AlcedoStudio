# Profile tone stage (archived)

This directory keeps the profile tone stage of the DCP camera profiles, kept for reference only.
The stage applied the `ProfileToneCurve` of a DCP profile at the entry of the display transform,
on CUDA, OpenCL, and Metal (DCP profile tone curve study, phases T2 and T3,
[#863](https://github.com/zidage/AlcedoStudio/issues/863) and
[#864](https://github.com/zidage/AlcedoStudio/issues/864)). The profile curve now reaches the render
as points of the RGB curve of the Tone Curve, which the user sees and edits
(`edit/runtime/profile_tone_curve_import.hpp`,
[#911](https://github.com/zidage/AlcedoStudio/issues/911);
`docs/roadmap/alcedo_studio/raw-processing/dcp_profile_tone_curve_study.md`, section 11).

- The files are not built, installed, or packaged. No CMake target, source list, test
  registration, OpenCL program, or Metal library refers to this directory.
- The files keep their original paths relative to `alcedo_studio/`, for example
  `alcedo_studio/src/edit/runtime/profile_tone_stage.cpp` is now
  `alcedo_studio/deprecated/profile_tone_stage/src/edit/runtime/profile_tone_stage.cpp`. They moved
  with `git mv` and no content change, so `git log --follow` shows their history.
- The kernel code that called the stage (`drt.cl`, `drt.metal`, `cuda_drt_pass.cu`, the DRT passes,
  `DrtPostExecutor`), the stage term of the DRT display key, and the host reference function
  `ApplyProfileToneStageReference` were removed from files that stay; commit `8e1766300` holds them.
- Do not add a `CMakeLists.txt` here.

| Archived path | What it held |
| --- | --- |
| `src/include/edit/runtime/profile_tone_stage.hpp` | `ResolveProfileToneStage` (which documents ran the stage), `PackProfileToneGpuData`, `UploadProfileToneGpuData` |
| `src/edit/runtime/profile_tone_stage.cpp` | The stage resolve and the GPU buffer: matrices, anchor, curve table, inverse neutral table |
| `src/include/edit/runtime/profile_tone_gpu_math.h` | One implementation of the stage for the host, CUDA, OpenCL C, and Metal: highlight shoulder, DNG SDK baseline RGB tone, inverse neutral response |
| `tests/edit/runtime/profile_tone_gpu_test_support.hpp` | DRT-only render of an uploaded AP1 row, shared by the three backend tests |
| `tests/edit/runtime/cuda_profile_tone_test.cpp` | CUDA stage against the scalar reference |
| `tests/edit/runtime/opencl_profile_tone_test.cpp` | OpenCL stage against the scalar reference |
| `tests/edit/runtime/metal_profile_tone_test.cpp` | Metal stage against the scalar reference |
