# Development Roadmap

Roadmap documents are stored under the source directory that primarily owns the work. Cross-module
plans live with the dominant owner and declare secondary consumers in their metadata. Project-wide
release planning stays at this directory root; plans for external repositories use a separate
top-level category.

## `alcedo_studio/src/ai`

- [AI Sidecar Backend Integration Plan](alcedo_studio/src/ai/ai_sidecar_backend_plan.md)
- [AI Sidecar Phase 0 Contract](alcedo_studio/src/ai/ai_sidecar_phase0_contract.md)
- [Semantic Generation and Search Integration Plan](alcedo_studio/src/ai/semantic_generation_search_plan.md)

## `alcedo_studio/src/decoders/processor/nn`

- [CUDA CNN Forward Framework + DemosaicNet Plan](alcedo_studio/src/decoders/processor/nn/cuda_nn_forward_demosaicnet_plan.md)
- [CUDA DemosaicNet Performance Follow-up](alcedo_studio/src/decoders/processor/nn/cuda_demosaicnet_performance_next.md)
- [OpenCL NN Forward DemosaicNet Migration and Performance Recovery](alcedo_studio/src/decoders/processor/nn/opencl_nn_forward_demosaicnet_plan.md)

## `alcedo_studio/src/ui/alcedo_main`

- [AI Sidecar Frontend Plan](alcedo_studio/src/ui/alcedo_main/ai_sidecar_frontend_plan.md)
- [Background Tasks and Declarative UI State Plan](alcedo_studio/src/ui/alcedo_main/background_tasks_ui_state_plan.md)

## External website

- [Alcedo Studio Website Redesign Plan](website/alcedo_website_redesign_plan.md)
- [Alcedo Studio Website Public Copy](website/alcedo_website_public_copy.md)

## Project-wide

- [Release and product roadmap](roadmap.md)

## Placement rules

- Mirror the primary owning path below `alcedo_studio/src/` when adding a new implementation plan.
- Keep tightly related plans together so relative links survive moves.
- Put cross-cutting work under the dominant source owner and list other modules in the document's
  `Primary roadmap owner` metadata.
- Keep only navigation and genuinely project-wide roadmaps at this root.
- Do not create monthly buckets; source ownership is the stable classification.
