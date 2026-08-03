# Development Roadmap

Roadmap documents are grouped by product area. The directory structure does not mirror the source
tree. Project-wide release planning stays at this directory root, and plans for external projects
use their own top-level category.

## Alcedo Studio — AI

- [AI Sidecar Backend Integration Plan](alcedo_studio/ai/ai_sidecar_backend_plan.md)
- [AI Sidecar Phase 0 Requirements](alcedo_studio/ai/ai_sidecar_phase0_requirements.md)
- [Semantic Generation and Search Integration Plan](alcedo_studio/ai/semantic_generation_search_plan.md)

## Alcedo Studio — RAW processing and DemosaicNet

- [CUDA CNN Forward Framework + DemosaicNet Plan](alcedo_studio/raw-processing/demosaicnet/cuda_nn_forward_demosaicnet_plan.md)
- [CUDA DemosaicNet Performance Follow-up](alcedo_studio/raw-processing/demosaicnet/cuda_demosaicnet_performance_next.md)
- [OpenCL NN Forward DemosaicNet Migration and Performance Recovery](alcedo_studio/raw-processing/demosaicnet/opencl_nn_forward_demosaicnet_plan.md)
- [Metal MPSGraph DemosaicNet Plan](alcedo_studio/raw-processing/demosaicnet/metal_nn_forward_demosaicnet_plan.md)

## Alcedo Studio — UI

- [AI Sidecar Frontend Plan](alcedo_studio/ui/ai_sidecar_frontend_plan.md)
- [Background Tasks and Declarative UI State Plan](alcedo_studio/ui/background_tasks_ui_state_plan.md)
- [Editor Session Command Queue and Lock Simplification Plan](alcedo_studio/ui/editor_session_command_queue_and_lock_simplification_plan.md)
- [QML Editor and Qt RHI Unified Workspace Refactor Plan](alcedo_studio/ui/qml_editor_rhi_unified_workspace_plan.md)
- [Editor Single Live Pipeline + WAL + Checkpoint Simplification Plan](alcedo_studio/ui/editor_single_live_pipeline_wal_checkpoint_plan.md)
  (includes **Final locked identity model**: history owns HEAD; pipeline = params table; chain hash unit = one commit)

## External website

- [Alcedo Studio Website Redesign Plan](website/alcedo_website_redesign_plan.md)
- [Alcedo Studio Website Public Copy](website/alcedo_website_public_copy.md)

## Project-wide

- [Release and product roadmap](roadmap.md)

## Placement rules

- Use exact engineering terms such as interface, API, schema, protocol, invariant, behavior
  specification, acceptance criterion, compatibility requirement, or performance target. The
  generic English noun formed by `c` + `ontract` (including its plural and casing variants) is not
  allowed in roadmap prose, headings, link labels, or filenames.
- Choose a plain product-area folder such as `ai`, `raw-processing`, or `ui`.
- Add a topic folder when several closely related plans need to stay together.
- Keep tightly related plans together so relative links remain short and stable.
- Put cross-cutting work under the product area that owns the user-visible result and list other
  affected areas near the top of the document.
- Keep only navigation and genuinely project-wide roadmaps at this root.
- Do not mirror `src`, `include`, or implementation-language directories.
- Do not create monthly buckets.
