# Roadmap

## Recently Completed

### v0.1.2 release

- [x] Added OpenDRT support and rendering transform selection UI.
- [x] Added image deletion inside projects.
- [x] Improved filter UI usability.
- [x] Landed follow-up color-management fixes around the release.

### Workflow polish after v0.1.2

- [x] Added reset adjustments in the editor.
- [x] Added aspect ratio selection.
- [x] Added waiting animation for thumbnails.
- [x] Optimized high-resolution camera loading time.

### macOS / Metal bring-up on main

- [x] Added compile support for macOS.
- [x] Added initial Metal support for image processing.
- [x] Added Metal raw processing utilities.
- [x] Added Metal support for image resizing and lens calibration.
- [x] Integrated Metal into the pipeline.
- [x] Fixed geometry pipeline path issues and cleaned up Metal shader compilation.
- [x] Added macOS packaging logic.

### Website and localization

- [x] Added the demo website.
- [x] Added i18n support for the website.
- [x] Added deployment workflow for the website.
- [x] Updated the zh-CN website font stack.

## Still Planned

- [ ] Add JSON serialization for pipeline configurations.
- [ ] Extend HDR workflow and output.
- [ ] Continue semantic search and AI-assisted tagging work; see the
      [semantic generation plan](alcedo_studio/ai/semantic_generation_search_plan.md) and
      [AI sidecar backend plan](alcedo_studio/ai/ai_sidecar_backend_plan.md).
- [ ] Integrate the panorama stitching module into the main workflow.
- [ ] Keep closing the feature and stability gap between the Windows/CUDA and macOS/Metal paths.
