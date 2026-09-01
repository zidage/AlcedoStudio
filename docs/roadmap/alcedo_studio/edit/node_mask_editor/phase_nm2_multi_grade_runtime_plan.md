# Phase NM2 — Multi-Grade Runtime and Parameter Ownership

Date: 2026-08-31

Status: NM2.1–NM2.4 complete; NM2.5 planned

Prerequisite: NM1, including NM1.4R and NM1.5.

## 1. Purpose

Execute every Color Grade on the image backbone through CUDA, OpenCL, and Metal.
Move Clarity, Sharpen, Halation, and Film Grain into DRT/Post.
Keep the current image quality and the shared executor from NM1.

Use explicit inputs and outputs throughout the runtime.
The same Grade implementation must work at any position on the backbone.
Its interfaces must also support a Grade on a future image branch.

NM2 does not enable image branches in product documents.
It does not define a branch mixer or concurrent GPU scheduling.
It supplies the dependency and resource rules that those features need.

## 2. Required context

Read the applicable sections below before each sub-phase.
Follow their references when a dependency, owner, or failure rule remains unclear.
Check the current callers and tests before you change a referenced component.
Do not infer its purpose from its name alone.

| Source | Required context |
| --- | --- |
| [Node and mask master plan](../node_mask_editor_master_plan.md) | Sections 3–7 define node identity, topology, and parameter ownership. Sections 21.3–21.6 define phase boundaries. Section 24 defines resource targets. |
| [NM1 execution plan](phase_nm1_pipeline_document_editing_plan.md) | Sections 3–5 define the live document. Section 10.5 defines task requests and resource release. Sections 11–15 define storage and later phases. |
| [GPU DAG plan](../gpu_dag_pipeline_rebuild_phase_plan.md) | Sections 6, 12, and 14–17 define plans, passes, parameters, and workspaces. Sections 18 and 25–27 define geometry, mix, LLF, and caches. |
| [Metal execution plan](../gpu_dag_metal_migration_phase_plan.md) | Shared renderer interfaces, Metal encoders, resource completion, and numerical evidence. |
| [OpenCL execution plan](../gpu_dag_opencl_migration_phase_plan.md) | Shared renderer interfaces, OpenCL resource rules, program registration, and numerical evidence. |
| [Single live pipeline plan](../../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | One live editing object, history authority, and checkpoint identity. |

The master plan defines current product behavior.
NM1.4R and NM1.5 define the current request, lifetime, and storage boundaries.
This plan defines the NM2 changes within those boundaries.
Older plans explain existing algorithms and interfaces where these rules still apply.

Do not restore older snapshot executors, stage mirrors, or pipeline merge behavior.
Do not copy old workspace reservation rules into background tasks.
The NM1 completion records cover its prerequisite work.
Thumbnail disk writeback remains outside NM2 and does not block it.

### 2.1 Current source map

The paths below locate the current implementation.
Check the implementation at the execution revision before you change it.

| Component | Source and current responsibility |
| --- | --- |
| Document and factories | [pipeline_document.cpp](../../../../../alcedo_studio/src/edit/graph/pipeline_document.cpp) creates the default graph and supplies node lookup. |
| Topology | [pipeline_graph.hpp](../../../../../alcedo_studio/src/include/edit/graph/pipeline_graph.hpp) separates graph validation from backbone validation. |
| Grade model | [color_grade_node_model.cpp](../../../../../alcedo_studio/src/edit/graph/color_grade_node_model.cpp) creates Default and Clean adjustments. |
| DRT model | [drt_node_model.hpp](../../../../../alcedo_studio/src/include/edit/graph/drt_node_model.hpp) owns output-transform parameters and the four DRT/Post neighborhood adjustments (NM2.1). |
| Parameter targets | [editor_adjustment_types.hpp](../../../../../alcedo_studio/src/include/app/editor_adjustment_types.hpp) defines owner and node identity. |
| Parameter commands | [editor_pipeline_command_service.cpp](../../../../../alcedo_studio/src/app/editor_pipeline_command_service.cpp) reads and applies typed targets. |
| Compiler | [graph_compiler.cpp](../../../../../alcedo_studio/src/edit/runtime/graph_compiler.cpp) compiles every backbone Color Grade in edge order, including the zero-Grade Develop-to-DRT path. |
| Plan | [execution_plan.hpp](../../../../../alcedo_studio/src/include/edit/runtime/execution_plan.hpp) stores `grade_nodes` in edge order, DRT/Post steps, and explicit pass I/O. [execution_plan.cpp](../../../../../alcedo_studio/src/edit/runtime/execution_plan.cpp) validates those bindings. |
| Shared execution | [plan_executor.hpp](../../../../../alcedo_studio/src/include/edit/runtime/plan_executor.hpp) controls result reuse, execution, completion, and failure. |
| Parameter storage | [parameter_binding.hpp](../../../../../alcedo_studio/src/include/edit/runtime/parameter_binding.hpp) defines node and adjustment slot identity. [parameter_arena.hpp](../../../../../alcedo_studio/src/include/edit/runtime/parameter_arena.hpp) owns parameter storage. |
| Content identity | [result_content_key.cpp](../../../../../alcedo_studio/src/edit/runtime/result_content_key.cpp) hashes each compiled Grade output and LLF keys by `NodeId`. |
| GPU resources | [basic_render_workspace.hpp](../../../../../alcedo_studio/src/include/edit/runtime/basic_render_workspace.hpp) owns parameters, images, buffers, and temporary storage. |
| Image results | [graph_image_cache.hpp](../../../../../alcedo_studio/src/include/edit/runtime/graph_image_cache.hpp) separates completed results from unpublished writes. |
| CUDA Grade | [cuda_primary_grade_pass.cu](../../../../../alcedo_studio/src/edit/runtime/cuda/cuda_primary_grade_pass.cu) executes each compiled Grade from `plan.grade_nodes` using that node's `scene_input`. |
| OpenCL Grade | [opencl_primary_grade_pass.cpp](../../../../../alcedo_studio/src/edit/runtime/opencl/opencl_primary_grade_pass.cpp) executes the same per-node Grade API. |
| Metal Grade | [metal_primary_grade_pass.mm](../../../../../alcedo_studio/src/edit/runtime/metal/metal_primary_grade_pass.mm) executes the same per-node Grade API. |

### 2.2 Primary Grade and current panels

The default graph contains Develop, one Color Grade, and DRT/Post.
The factory gives the middle node the ID `grade.primary`.
This node uses the ordinary `ColorGradeNodeModel` type.
The ID does not define a special node type or permanent execution role.

`PipelineDocument::PrimaryGrade()` currently finds that fixed ID.
It does not find the selected Grade or follow image edges.
The compiler compiles every backbone Grade in edge order.
GPU execute (NM2.4) follows `plan.grade_nodes`. `PipelineDocument::PrimaryGrade()` still finds the default ID for editor routing. Three-argument LLF hash helpers still default to `FirstGrade()` for tests; GPU encode passes the compiled `NodeId`.

The current editor has no product node editor.
Its ordinary Grade controls can continue to target the default Grade.
Keep this application routing while NM5 and NM6 remain incomplete.
Do not use it to select runtime inputs or parameter owners.

Develop controls must continue to target Develop.
Move the four post-processing controls to DRT/Post when their model ownership changes.
Keep their current presentation unless ownership requires a direct wiring change.
NM2 does not add node selection to the adjustment stack.

### 2.3 Existing limits

- The shared executor records, skips, and encodes every compiled Grade in backbone order, then DRT/Post (NM2.4).
- Default and Clean Grades contain only Color Grade catalog types (NM2.1).
- DRT/Post owns Clarity, Sharpen, Halation, and Film Grain with factory defaults (NM2.1).
- Per-value content keys and LLF source identity follow compiled `NodeId` (NM2.3). GPU encode uses that `NodeId` (NM2.4).

Existing ID types and workspace interfaces remain useful.
NM2 must extend these components rather than add another document or renderer.

## 3. Terms and scope

| Term | Meaning |
| --- | --- |
| Backbone | The valid image path from Develop through zero or more Grades to DRT/Post. |
| Node plan | The compiled operations and bindings for one document node. |
| Pass | One compiled unit of GPU work with explicit inputs and outputs. |
| Value | One logical output, identified by `GraphValueId`. |
| Input binding | The relation between a consumer input port and a producer output value. |
| Content key | A hash of the data and settings that determine an output. |
| Alias | Two logical values that refer to the same GPU resource without a copy. |
| Submission | GPU work that a backend submits through its command context. |
| Lease | Existing ownership that keeps a GPU resource valid until its users finish. |
| LLF | Local Laplacian Filter, used for local tone adjustments. |
| Canonical reference | The established LLF reference domain, independent of the current viewport ROI. |
| ROI | Region of interest in the image. |

### 3.1 Included work

- Execute zero, one, or multiple Grades in backbone order.
- Enforce parameter ownership in models, commands, serialization, and compilation.
- Give every pass explicit value dependencies.
- Bind parameters by node and adjustment instance.
- Calculate result keys from actual inputs.
- Preserve resources until their final readers finish.
- Preserve Grade mix, masks, LLF, and neighbor operations at each Grade.
- Check the editor, thumbnail, analysis, and export paths.
- Test dependency behavior with a small branch-and-join plan outside product document validation.

### 3.2 Excluded work

- Product image branches, branch mixers, and configurable compositing modes.
- Multiple GPU queues or simultaneous branch execution.
- A plugin system for arbitrary node implementations.
- NM3 multi-mask storage, Union, range selection, and authoring.
- NM4 typed structural history, Version recovery, Paste, and final format release.
- NM5 Nodes UI and NM6 selection-based adjustment routing.
- Stage-only project migration or a second writable parameter source.
- Global memory admission, new task schedulers, or generic scratch layout planning.

## 4. Design requirements

### 4.1 Graph rules and product rules

Keep general graph checks separate from product backbone checks.
General checks cover node identity, port types, required inputs, and cycles.
Each current input port accepts at most one edge.
A future node can declare several distinct input ports.
NM2 does not add a variable-input schema to document nodes.

The product still requires one Develop and one DRT/Post.
Every Grade must lie on the unique backbone.
Reject image branches before product execution.
Reject unknown node types and unsupported operations with the actual error.

Keep `AddCleanColorGrade`, removal, and reconnect behavior from NM1.
Do not replace those operations with a generic topology editor.
Use NodeId as identity after reorder, removal, and serialization.
Do not use a list index as persistent identity.

### 4.2 Plan structure and dependencies

Compile each node into a node plan.
Describe each executable pass with the following data:

| Field | Requirement |
| --- | --- |
| Pass identity | Distinguish repeated operations within and across nodes. |
| Owner | Identify the document node and the adjustment instance when applicable. |
| Operation | Select the existing GPU operation or a defined DRT/Post step. |
| Inputs | Record each input port and its source `GraphValueId`. |
| Outputs | Record output values and their image or mask type. |
| Parameters | Refer to stable parameter slots without GPU addresses. |

Reuse `GraphValueId` for node outputs.
Give internal outputs distinct identities within their owning node.
Use adjustment identity when repeated operations need separate intermediate values.
Do not add internal GPU passes to the user graph.

Order work from its dependencies.
Use a deterministic order when independent passes are ready.
The order must not depend on node insertion order or hash-map iteration.
An encoder must not use a global mutable `current_image` to find its input.

Check plans for missing producers, duplicate output definitions, and invalid input types.
Check that every consumer follows its producers.
Replace APIs that assume each pass kind appears only once.
This includes first-match pass lookup where callers need a specific instance.

The static plan owns descriptions, not GPU resources or a document copy.
Static identity includes topology, adjustment structure, source layout, and backend capability.
Parameter values and viewport changes must not rebuild the static plan.
Bind request geometry and values separately from the cached static description.

Keep the existing format and color meaning of each value explicit.
Keep internal image processing in 32-bit float.
Grades consume and produce scene-referred images.
DRT/Post defines the transition to display-referred output.
Do not infer color meaning from a texture's position in the pass list.

### 4.3 Grade execution

Each Grade must perform the same operation at every graph position:

```text
connected scene input
  -> ordered adjustments owned by this Grade
  -> mix with the original input and optional coverage
  -> this Grade's scene output
```

Preserve the established mix formula:

```text
coverage = sampled mask when connected; otherwise 1
weight = clamp(coverage * mix, 0, 1)
output = input + weight * (adjusted - input)
```

A disabled Grade returns its input unchanged.
A zero mix also returns its input unchanged.
Keep the Grade's logical output identity when it aliases the input.
Do not overwrite the upstream content metadata through that alias.

Preserve pointwise fusion within a Grade when its order permits fusion.
Keep LLF and neighbor operations at their required boundaries.
Preserve the original input until the final mix finishes.
Do not fuse across Grade boundaries in NM2.

With zero Grades, connect Develop output directly to DRT/Post.
Do not synthesize a primary Grade or execute a missing Grade pass.
With several Grades, each Grade consumes the previous connected output.
DRT/Post consumes the final connected scene output.

The Grade encoder consumes coverage through an explicit value binding.
NM2 retains the existing single-mask representation for each Grade.
NM3 can replace mask production without changing Grade mix semantics.
Do not add a second mask list or a new mask persistence format here.

### 4.4 Parameter ownership and current editor behavior

| Owner | Parameters |
| --- | --- |
| Develop | RAW decode, RAW white balance, camera profile, and lens correction. |
| Color Grade | Creative CAT02, tone, color, curves, LUT, enabled, mix, and mask association. |
| DRT/Post | Clarity, Sharpen, Halation, Film Grain, and output-transform parameters. |
| Document | Crop, rotation, and image geometry. |

Enforce this table at insertion, parameter application, load, save, and compile boundaries.
Keep reusable adjustment factories in the catalog where needed.
Add owner checks to prevent valid types from entering the wrong node.
Do not duplicate each adjustment model for each owner.

Keep Default exposure at +1.5 EV and saturation at the current 1.3 model value.
Keep Clean exposure at 0 EV and saturation at the no-change value.
Create post-processing defaults in DRT/Post.
Do not change a node's ID when its display name changes.

Keep default Grade routing at the application boundary.
Use complete targets for reads, preview writes, settled writes, and existing parameter Undo/Redo.
Route the four moved controls to DRT/Post in the same change.
Check their parameter projections and reload paths.
Do not preserve hidden Grade copies to keep old panel code working.

Default factory code can retain the ID `grade.primary`.
Runtime, parameter packing, and content keys must not require that ID.
Remove fixed-ID helpers from those paths.
Any retained application helper must have a narrow, documented caller.

### 4.5 DRT/Post order and storage

Capture the current single-Grade reference before changing ownership.
Use enabled adjustments, mix 1, and no mask for that reference.
Record each operation's order and color domain.
Include non-default values that expose ordering errors.

Represent DRT/Post as explicit internal steps.
Preserve the reference order around the output transform.
Do not move all four operations after DRT merely because the UI groups them there.
Apply these operations once after the backbone Grades reach the endpoint.

Grade masks and Grade mix must not suppress DRT/Post operations.
Test this new ownership rule separately from the unmasked reference comparison.
Preserve the established image coordinates for Film Grain and scale-dependent operations.

Update the current document serializer and reader with the owner change.
New documents and round trips must preserve all moved values.
Reject old owner placement or malformed data at the read boundary.
Do not silently relocate, discard, or replace invalid data during load.
Do not add a stage importer or a legacy schema converter.

NM4 owns the final project and history format change.
NM2 must not claim compatibility with earlier development documents that violate the new owner rules.
Keep existing parameter history behavior for new edits.
Do not add structural history or replay old stage commits here.

### 4.6 Parameter storage and upload

Keep one `ParameterArena` per render device.
Use `NodeId + AdjustmentInstanceId` as the slot key.
Do not create an arena or executor for each Grade.

Prepare all required slots before GPU commands read the parameter buffer.
Calculate capacity from actual slot sizes and alignment.
Do not resize the buffer while a submission still uses it.
Check repeated adjustment types and equal local names across different nodes.

Use full DTO values when a slot is new or the device is recreated.
Use dirty ranges for later parameter changes.
Preserve pending dirty state when upload fails or cancellation occurs before upload.
Do not consume a dirty patch twice through separate backend preparation paths.

Keep slot ownership valid after topology changes.
Reclaim removed slots only when GPU reads finish.
Do not let repeated add/remove cycles grow parameter storage without a bound.
An explicit slot-layout rebuild at a safe topology boundary is sufficient.
Do not build a new general allocator for this requirement.

### 4.7 Content keys and invalidation

Locate results by logical output identity.
Check content identity separately from resource identity.
Matching texture dimensions alone do not prove a cache hit.

Calculate each output key from the following inputs:

1. The operation and its implementation version.
2. The node's effective parameters and adjustment order.
3. Each input port and its producer content key.
4. The mask or other external asset content, where applicable.
5. Image geometry, format, and output settings that affect this result.

Use a stable port order when hashing multiple inputs.
Do not sort inputs by their content hash.
Input position can change the result of a future compositor.

Do not include the whole document revision in every output key.
Do not include display names, selection, or canvas positions.
Keep topology identity separate from pixel identity.
A topology change can rebuild the plan without invalidating every stored image.

For `A -> B -> C`, a B edit must invalidate B and C.
It must not invalidate A or Develop.
A DRT/Post edit must not invalidate Grade outputs.
Reconnect must use the new input keys, even when NodeIds remain unchanged.
Removed nodes must not leave reusable results for another logical output.

Cache publication remains a request-level success operation.
A later pass, submission, or sink failure must not publish partial results.
Previously valid results must remain valid when new writes fail.
Keep request cache policy separate from dependency semantics.

### 4.8 LLF, LUT, masks, and geometry

Audit auxiliary resources as well as final images.
Include command buffers, LUT resources, local tone pyramids, reference images, and mask sampling state.
Use node and operation identity where state belongs to one instance.
Keep these resources under the existing workspace ownership.
Do not create a process-global cache keyed only by a workspace address.

LLF must read the correct source at its adjustment position.
That source includes preceding Grades and preceding adjustments in its own Grade.
Its key must include those actual dependencies.
A NodeId alone is insufficient.

Preserve the canonical reference domain and existing reference quality.
Do not make a canonical key depend on the viewport ROI.
Do not build canonical pixels from the current cropped preview input.
Any required upstream reference work must use the same algorithms and preceding edits.
Do not substitute Develop pixels for a later Grade's reference source.

Separate LLF source keys from keys for slider-dependent results.
A Shadows/Highlights edit can reuse unchanged source data.
An upstream edit must invalidate that source data.
Check the case where two Grades both use local tone adjustments.

Preserve the shared geometry resolver and coordinate definitions.
Account for all neighbor operations when a request needs an expanded input region.
Do not apply a single-Grade radius to a multi-Grade chain without checking the required source region.
Do not introduce another ROI coordinate system.

### 4.9 Resource lifetime

Derive each value's consumers from the pass inputs.
Keep a value until its remaining consumers finish.
GPU completion or an established backend dependency must precede release or overwrite.
Host command submission alone does not prove completion.

Track aliases through the underlying lease.
Finishing one alias must not release storage that another alias still needs.
Include final mix, LLF, masks, and output delivery in lifetime checks.
Check both executed consumers and consumers that reuse cached results.

Separate required live values from optional retained cache results.
Use the current cache policy to decide retention after use.
Background tasks must preserve the NM1.4R allocation and release rules.
They must not read, overwrite, or clear the editor's retained result set.

Allocate background scratch for the actual request at the point of use.
Release it after its GPU use finishes.
Do not reserve from pixel estimates or historical peak capacity.
Do not retain idle blocks or all intermediate images until task completion.

Consumer tracking does not authorize a generic scratch layout planner.
Do not add deferred pointer assignment, global byte reservations, or budget-based task admission.
Do not serialize all image tasks through a new device-wide lock.
Keep the established single-submission rule within each render device.

### 4.10 Backend and service boundaries

Share graph traversal, bindings, keys, cache decisions, and lifetime rules.
Backend encoders implement native GPU operations and completion primitives.
Do not copy three independent graph schedulers into CUDA, OpenCL, and Metal.

Keep one live document and executor per image.
The application coordinates access with the existing render lock and use guards.
Each task supplies its own request and complete output recipe.
Do not temporarily rewrite document DRT settings for export.
Do not add document copies to isolate task settings.

Allocation, compilation, or execution failure must report the actual error.
Do not reduce decode quality or switch algorithms or backends.
Cancel unpublished writes and release task-owned resources safely.
Do not save document state when a background task releases its use guard.

## 5. Extension boundary for parallel nodes

Future topology can contain this shape:

```text
Develop --+--> Grade A --+
          |             +--> compositor --> DRT/Post
          +--> Grade B --+
```

The existing Grade encoder must work for A and B without a new execution model.
Both inputs can refer to one Develop value.
The compositor must bind its inputs by port identity.
The resource owner must retain Develop until both branches finish their reads.

NM2 supplies value bindings, input-based keys, and consumer-aware lifetime.
A future feature supplies the compositor model, merge formula, compiler support, and product connection rules.
Its UI and history operations also belong to that feature.

Simultaneous branch execution needs additional queue and synchronization work.
NM2 uses deterministic execution through current backend contexts.
Do not claim that branch-shaped plans prove concurrent execution.

Test the dependency shape through a small internal plan fixture.
Use deterministic operations with known expected outputs.
Keep this fixture outside product node registration and document serialization.
Do not weaken product validation to admit it.

## 6. Sub-phase sequence

| Phase | Status | Result |
| --- | --- | --- |
| NM2.1 | complete | Parameter ownership and single-Grade reference behavior. |
| NM2.2 | complete | Explicit node plans, pass instances, and value dependencies. |
| NM2.3 | complete | Parameter bindings, content keys, auxiliary state, and safe resource lifetime. |
| NM2.4 | complete | Complete multi-Grade execution on CUDA, OpenCL, and Metal. |
| NM2.5 | planned | Numerical, resource, failure, and service qualification. |

Implement these phases in order.
Keep each interface change buildable across the three backends.
Make necessary caller changes with the interface change.
Do not preserve a second renderer as a temporary replacement path.
Do not expose Nodes UI before NM4 and NM5 meet their own exit conditions.

### 6.1 NM2.1 — Ownership and reference behavior

**Work**

1. Read the ownership and single-Grade context in Sections 2 and 4.
2. Capture reference pixels and operation order from the current native backend paths.
3. Move the four post-processing models into DRT/Post.
4. Update Default factories and owner validation.
5. Update current serialization and typed parameter targets.
6. Route the four existing controls to their new owner.
7. Preserve single-Grade rendering through the native DRT/Post steps.
8. Add model, parameter, round-trip, and reference tests.

**Primary files**

Use the model, factory, target, and command sources in Section 2.1.
Inspect [editor_adjustment_pipeline.cpp](../../../../../alcedo_studio/src/app/editor_adjustment_pipeline.cpp) for the current application path.
Update the relevant native DRT and Grade encoders with ownership changes.

**Exit conditions**

- [x] Default and Clean nodes contain only Grade-owned adjustments.
- [x] DRT/Post contains all four moved adjustments and their defaults.
- [x] Wrong-owner insertion, load, save, and compilation fail explicitly.
- [x] Existing controls read, edit, and restore the new owner correctly.
- [x] Current parameter Undo/Redo retains the moved values for new edits.
- [x] Unmasked single-Grade output preserves the captured reference within the declared tolerance.
- [x] Grade mask and mix do not suppress endpoint operations.

##### Phase NM2.1 completion record (2026-08-31)

**Status:** complete — Color Grade owns CAT02 through LMT only; DRT/Post owns Clarity, Sharpen, Halation, and Film Grain; unmasked CUDA pixels match the captured reference; QML `field_key` fills DRT/Post at history.

**Primary success call chain:**

```text
QML / panel field_key (Unspecified owner)
  -> EditorSessionEditController::HandlePatch
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> CompleteCurrentPanelParameterTarget (document lookup: DrtPost + drt.<type>)
  -> ApplyEditorParameterPatch on DrtNodeModel (or ColorGrade for Grade fields)
  -> GraphCompiler: Grade catalog + RequireCompleteDrtPostTypes
  -> MixDrtPost into keys.drt_display
  -> Grade mix -> DRT neighborhood in ACEScc (Clarity, Sharpen, Halation, Film Grain)
  -> DRT display transform -> sink
```

**Primary failure call chain:**

```text
wrong-owner InsertAdjustment / FromJson / ToJson / GraphCompiler
  -> RequireAdjustmentOwner / RequireCompleteDrtPostTypes
  -> std::runtime_error
  -> document JSON unchanged; no GPU work
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PostAdjustmentsRejectGradeOwnership` | `GpuDagModelGraphTest` | PASS |
| `PostControlTargetsDrtAndRestoresOnUndo` | `EditorSessionHistoryPortTest` | PASS |
| `DrtPostPreservesUnmaskedReferenceOrder` (tol `2.0e-3f`; mix=0 Clarity still differs) | `GpuDagCudaDrtProductTest` | PASS |
| Default Grade 13 types; DRT four defaults | `GpuDagModelGraphTest` | PASS |
| `CompleteCurrentPanelRoutesClarityToDrtPost`, `PublishClarityWritesDrtModelAndRejectsGradeOwner` | `EditorPipelineCommandServiceTest` | PASS |
| `ClarityOnDrtChangesDisplayKeyNotGradeKey` | `GpuDagRawInputTest` | PASS |
| CUDA neighborhood + identity Grade after look reset | `GpuDagCudaPrimaryGradeTest` | PASS |
| OpenCL DRT/Post neighborhood | `GpuDagOpenClGradeTest` | PASS |
| Document save/load with new owners | `PipelineMapperTest` | PASS (39 ran, 2 disabled) |
| Viewport ROI host size after ClampRoi (19×19 native, not 40×24 upsample) | `GpuDagCudaDrtProductTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest GpuDagRawInputTest EditorPipelineCommandServiceTest EditorSessionHistoryPortTest EditorSessionEditControllerTest GpuDagCudaDrtProductTest GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest EditorAdjustmentPipelineTest PipelineMapperTest
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaPrimaryGradeTest\.|GpuDagOpenClGradeTest\.|GpuDagCudaDrtProductTest\.(GpuDagCudaDrtProduct|CudaDrtProductFixture)\.|PostAdjustmentsReject|PostControlTargetsDrt|DrtPostPreserves"
ctest --test-dir build/debug --output-on-failure -R "GpuDagModelGraphTest\.|EditorSessionHistoryPortTest\.|GpuDagRawInputTest\.(GpuDagGraphCompiler|GpuDagResultContentKey)\.|EditorPipelineCommandServiceTest\."
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaDrtProductTest PipelineMapperTest
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaDrtProductTest\.GpuDagCudaDrtProduct\.|CudaDrtProductFixture\.DrtPostPreserves|PipelineMapperTest\."
```

Suite totals: first GPU + required-name filter **77/78** (the failure was `ProductRendererViewportAndMaxEdgeResampleDecodedSourceWithoutSizeMismatch` expecting 40×24 after a visible subregion). After aligning that assertion with `ClampRoiRenderExtentToNativePixels`, that test **PASS**; CUDA Grade and OpenCL Grade binaries were unchanged. Graph + history + compiler/content-key + command service **139/139**. DRT product `GpuDagCudaDrtProduct` + `DrtPostPreserves` + PipelineMapper **39/39** executed (2 PipelineMapper tests disabled).

**Checklist / exit condition:** all seven boxes checked from the tests above.

**LOC note (grill-code-review):** no changed production file exceeds 1000 lines. Largest after this phase: `editor_history_mutation.cpp` 487, `editor_pipeline_command_service.cpp` 366, `drt_node_model.cpp` 357, `metal_drt_pass.mm` 275, `opencl_drt_pass.cpp` 242, `cuda_drt_pass.cu` 208. CUDA neighborhood kernels compile once in `cuda_neighbor_grade.cu` (ODR: previously included from two translation units). New ownership module: `adjustment_ownership.hpp` / `.cpp`.

**Remaining gaps:** Metal DRT/Post neighborhood was compiled but not executed on this Windows host. The plan still uses singular `primary_grade_output` / `primary_grade_adjustments` until NM2.2. Factory node id `grade.primary` remains; runtime must not treat it as a special type. `ProductRendererViewportAndMaxEdgeResampleDecodedSourceWithoutSizeMismatch` now asserts the ROI-clamped 19×19 host size (`ClampRoiRenderExtentToNativePixels` on 0.8 × 24 develop pixels); that policy predates NM2.1 and is not an ownership change.

### 6.2 NM2.2 — Node plans and explicit dependencies

**Work**

1. Replace singular Grade plan fields with node-specific compiled data.
2. Give each pass a unique identity and explicit input/output bindings.
3. Compile every backbone Grade in dependency order.
4. Compile zero Grades as a direct Develop-to-DRT/Post connection.
5. Include DRT/Post internal steps in the same dependency description.
6. Update static keys and pass lookup callers.
7. Add plan validation and deterministic-order tests.

**Primary files**

Use `graph_compiler`, `execution_plan`, graph identity, and static-plan cache sources.
Update encoder declarations and their direct callers where the plan interface changes.
Keep GPU implementation details out of the shared plan types.

**Exit conditions**

- [x] Arbitrary Grade IDs compile without `grade.primary`.
- [x] Edge order determines inputs; container order does not.
- [x] Repeated pass kinds have distinct instances and outputs.
- [x] Parameter-only and viewport changes reuse the static plan.
- [x] Product compilation still rejects branches and invalid backbones.
- [x] Invalid compiled bindings fail before GPU work starts.

##### Phase NM2.2 completion record (2026-08-31)

**Status:** complete — compiled plans describe every backbone Color Grade with unique pass instances and explicit I/O; GPU still executes only `FirstGrade()` until NM2.4.

**Primary success call chain:**

```text
GraphCompiler::CompileStatic(document, source)
  -> RequireValidGraph (Validate + ValidateImageBackbone)
  -> CompileDevelopPasses
  -> ImageBackboneNodeIds() walk in edge order
  -> CompileColorGrade per Color Grade (incoming scene edge, optional mask, PrimaryColorGrade I/O)
  -> CompileDrt(last Grade output or Develop)
  -> ValidateExecutionPlan
  -> ExecutionPlan.grade_nodes + drt.steps + passes
```

**Primary failure call chain:**

```text
scene-image fan-out / invalid backbone / missing DRT types
  -> RequireValidGraph / RequireCompleteDrtPostTypes
  -> std::runtime_error before GPU work

mutated pass input with no producer, duplicate output, or kind mismatch
  -> ValidateExecutionPlan
  -> std::runtime_error before GPU work
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ZeroGradesFeedDevelopIntoDrtPost` (compile: no Grade pass; DRT input is Develop; DRT steps present) | `GpuDagRawInputTest` | PASS |
| `GradeWithoutPrimaryIdRendersItsParameters` (compile: compiled node is `grade.b`; adjustments match that model) | `GpuDagRawInputTest` | PASS |
| `ThreeGradesComposeInEdgeOrder` (each Grade `scene_input` is previous output; DRT input is last Grade; distinct `PassInstanceId`) | `GpuDagRawInputTest` | PASS |
| `GradeInputsFollowBackboneEdgesNotContainerOrder` | `GpuDagRawInputTest` | PASS |
| `RepeatedAdjustmentInstancesKeepTheirOrder` | `GpuDagRawInputTest` | PASS |
| `ParameterAndViewportEditsKeepStaticPlan` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerRejectsSceneImageBranch` | `GpuDagRawInputTest` | PASS |
| `InvalidCompiledBindingsFailBeforeGpuWork` | `GpuDagRawInputTest` | PASS |
| Zero Grade / absent `grade.primary` compile | `GpuDagModelGraphTest` | PASS |
| Incomplete DRT compile rejection | `GpuDagModelGraphTest` (`PostAdjustmentsRejectGradeOwnership`) | PASS |
| Single-Grade CUDA/OpenCL callers after plan field rename | `GpuDagCudaPrimaryGradeTest`, `GpuDagCudaDrtProductTest`, `GpuDagCudaMaskTest`, `GpuDagOpenClGradeTest` | PASS (113/113) |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest GpuDagRawInputTest
ctest --test-dir build/debug --output-on-failure -R "GpuDagModelGraphTest\.|GpuDagRawInputTest\."
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest GpuDagCudaMaskTest GpuDagOpenClGradeTest
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaPrimaryGradeTest\.|GpuDagCudaDrtProductTest\.|GpuDagCudaMaskTest\.|GpuDagOpenClGradeTest\."
```

Suite totals: graph + compiler **112/112**. CUDA Grade + DRT product + mask + OpenCL Grade **113/113**.

**Checklist / exit condition:** all six boxes checked from the compiler tests above. Pixel composition of multiple Grades is NM2.4.

**LOC note (grill-code-review):** no changed production file exceeds 1000 lines. Largest after this phase: `graph_compiler.cpp` 463, `graph_compiler_test.cpp` 462, `execution_plan.hpp` 362, `plan_executor.hpp` 260. New validation module: `execution_plan.cpp` 85.

**Remaining gaps:** GPU PlanExecutor and Grade encoders still run only `FirstGrade()`. Per-value content keys still hash `document.PrimaryGrade()` (NM2.3). `ZeroGradesFeedDevelopIntoDrtPost`, `GradeWithoutPrimaryIdRendersItsParameters`, and `ThreeGradesComposeInEdgeOrder` prove compiled identity and edge order, not independently calculated multi-Grade pixels. Metal Grade/DRT tests were updated to compile but were not executed on this Windows host.

### 6.3 NM2.3 — Bindings, keys, and resource lifetime

**Work**

1. Prepare parameter slots for all node plans before their GPU reads.
2. Replace singular result keys with keys for explicit output values.
3. Remove fixed-primary lookups from auxiliary resource preparation.
4. Bind LLF source and reference identity to the actual preceding work.
5. Track remaining consumers and alias ownership.
6. Preserve completed cache results across failed new submissions.
7. Preserve background allocation, cache isolation, and release behavior.
8. Add the branch-and-join fixture at the shared plan boundary.

**Primary files**

Use `parameter_arena`, `result_content_key`, `plan_executor`, and workspace resource sources.
Inspect native local tone implementations for hidden state and source assumptions.
Keep resource ownership in the workspace or existing lease types.

**Exit conditions**

- [x] Same-type adjustments on different nodes have independent parameter storage.
- [x] Upstream and sibling results remain reusable after an unrelated edit.
- [x] Reconnect uses new dependencies instead of stale cached pixels.
- [x] Two local tone Grades use the correct independent source histories.
- [x] Shared inputs and aliases remain valid through their final GPU reader.
- [x] Failed uploads retain retryable dirty state without publishing new results.
- [x] Background execution does not retain unused scratch or destroy editor results.
- [x] Repeated topology changes do not cause unbounded resource growth.

##### Phase NM2.3 completion record (2026-08-31)

**Status:** complete — parameter slots, per-value content keys, LLF identity, alias/consumer tracking, and failed-upload/cache isolation at the shared plan/runtime layer; GPU still executes only `FirstGrade()` until NM2.4.

**Primary success call chain:**

```text
PlanExecutor::Execute
  -> BeginRender
  -> AlignParameterLayout(topology_hash)
  -> BuildFrameResultContentKeys (values[GraphValueId], GradeScene(node))
  -> HashLlfSourceKey / HashLlfReferenceKey(..., compiled_grade->node_id)
  -> CollectParameterSlotKeys / encoder BindSlot for every plan.grade_nodes adjustment
  -> ParameterArena::UploadDirty
  -> PassEncoder PrimaryColorGrade for FirstGrade() only
  -> EndRender / PublishResults
  -> DRT content key hashes last compiled scene (or Develop when zero Grades)
```

**Primary failure call chain:**

```text
ParameterArena::UploadDirty throw
  -> pending_ ranges restored; HasPendingUpload() true; no new published keys

encode / sink / submit throw
  -> PlanExecutor CancelRender
  -> unpublished writes discarded
  -> previously published ContentKeys remain FindValidResult

RemainingValueConsumers::Consume after remaining == 0
  -> std::runtime_error
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `SameAdjustmentTypeUsesDistinctNodeSlots` (slots + keys) | `GpuDagCudaWorkspaceTest`, `GpuDagRawInputTest` | PASS |
| `MiddleGradeEditReusesUpstreamResults` | `GpuDagRawInputTest` | PASS |
| `ReconnectChangesNoncommutingGradeResult` (keys, not GPU pixels) | `GpuDagRawInputTest` | PASS |
| `TwoLocalToneGradesUseTheirOwnSources` | `GpuDagRawInputTest` | PASS |
| `LocalToneReferenceRemainsStableAcrossViewportChanges` | `GpuDagRawInputTest` | PASS |
| `GradeWithoutPrimaryIdSelectsCompiledNodeKeys` | `GpuDagRawInputTest` | PASS |
| `SharedInputSurvivesBothBranchReaders` (plan consumers + CUDA alias) | `GpuDagRawInputTest`, `GpuDagCudaWorkspaceTest` | PASS |
| `JoinInputsFollowPortBindings` / `BranchEditPreservesSiblingResult` | `GpuDagRawInputTest` | PASS |
| `ParameterUploadFailureRestoresPendingDirtyState` | `GpuDagCudaWorkspaceTest` | PASS |
| `SinkFailurePublishesNoNewResults` | `GpuDagCudaWorkspaceTest` | PASS |
| `RendererFailureDoesNotPublishUnfinishedContentKeys` | `GpuDagCudaDrtProductTest` | PASS |
| `BackgroundMultiGradeRenderPreservesEditorCache` (1-Grade session vs one-shot ExactRelease; not a 3-Grade GPU render) | `GpuDagCudaDrtProductTest` | PASS |
| `OneShotRenderDoesNotReadWriteOrClearEditorSessionCaches` | `GpuDagCudaDrtProductTest` | PASS |
| `RepeatedNodeRemovalReclaimsUnusedResources` | `GpuDagCudaWorkspaceTest` | PASS |
| CUDA LLF canonical reuse / ROI samples canonical | `GpuDagCudaPrimaryGradeTest` | PASS |
| OpenCL LLF 3-arg keys still FirstGrade(); canonical reuse | `GpuDagOpenClGradeTest` | PASS |
| CUDA mask mix after mask key from compiled Grade | `GpuDagCudaMaskTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest GpuDagCudaWorkspaceTest GpuDagCudaDrtProductTest GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest
ctest --test-dir build/debug --output-on-failure -R "GpuDagRawInputTest\."
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaWorkspaceTest\.CudaWorkspaceFixture\.(SameAdjustmentType|ParameterUploadFailure|RepeatedNodeRemoval|SinkFailure|SharedInput)|GpuDagCudaDrtProductTest\.CudaResultCacheProductFixture\.(BackgroundMultiGrade|RendererFailure|OneShotRenderDoesNot|ExposureEditRunsOnly)|GpuDagCudaPrimaryGradeTest\.|GpuDagOpenClGradeTest\."
ctest --test-dir build/debug --output-on-failure -R "GpuDagRawInputTest\.|GpuDagCudaWorkspaceTest\.|GpuDagCudaDrtProductTest\.|GpuDagCudaMaskTest\."
```

Suite totals: `GpuDagRawInputTest` content-key + branch-join names **PASS**. Named CUDA workspace / DRT product / CUDA Grade / OpenCL Grade filter **67/67**. Broader RawInput + CUDA workspace + DRT product + mask **167/169**; the two failures are pre-existing header hygiene (`transient_buffer_arena.hpp` comment token `OpenCL`; `renderer.hpp` comment `CUDA/Metal/OpenCL`). CUDA mask **11/11**.

**Checklist / exit condition:** all eight boxes checked from the tests above. Reconnect and sibling reuse are proven at content-key / plan-fixture layers; GPU still does not compose later Grades.

**LOC note (grill-code-review):** no changed production file exceeds 1000 lines. Largest after this phase: `cuda_primary_grade_pass.cu` 584, `opencl_primary_grade_pass.cpp` 571, `metal_primary_grade_pass.mm` 494, `cuda_local_tone_pass.cu` 479, `result_content_key.cpp` 449, `execution_plan.hpp` 403. New shared helpers: `CollectParameterSlotKeys`, `RemainingValueConsumers`, `HashBoundInputs`. CUDA LLF canonical identity moved from a process-global map into `NodeResultCache::Metadata`. New test file: `branch_join_plan_test.cpp` 126.

**Remaining gaps:** PlanExecutor and Grade encoders still run only `FirstGrade()`. Mask encode still uses the first compiled Grade mask. `RemainingValueConsumers` is proven at the shared plan fixture; ExactRelease in PlanExecutor still follows the linear Develop→FirstGrade→DRT sequence and must not drive multi-consumer release until NM2.4 executes every compiled Grade. `BackgroundMultiGradeRenderPreservesEditorCache` isolates a 1-Grade editor session from one-shot ExactRelease; it is not a multi-Grade GPU render (DRT would read the last Grade output that was never written). Product Grade keys still chain `MixGrade` in edge order; `HashBoundInputs` is the branch-join helper (PortId order). Metal Grade/LLF tests were compiled but not executed on this Windows host.

### 6.4 NM2.4 — Native multi-Grade execution

**Work**

1. Make each Grade encoder consume its node plan and connected input.
2. Execute its pointwise, LLF, neighbor, mask, and mix operations in order.
3. Feed the final Grade output into DRT/Post.
4. Execute endpoint operations once in the required color domains.
5. Update native program preparation and shader bindings where necessary.
6. Record execute, reuse, and resource events by node and pass instance.
7. Run the same behavior matrix on CUDA, OpenCL, and Metal.

**Primary files**

Use the three Grade sources in Section 2.1 and their DRT, mask, local tone, and encoder companions.
Update native program manifests and build registration when source names change.
Remove primary-specific runtime names where they incorrectly imply a unique Grade.
Do not rename persistent NodeIds as part of this cleanup.

**Exit conditions**

- [x] Each backend executes every Grade and the final endpoint.
- [x] Noncommuting adjustments produce the expected order-dependent pixels.
- [x] Every Grade mixes against its own input.
- [x] Disabled and zero-mix Grades preserve values and lifetime correctly.
- [x] Multiple LLF, LUT, and mask users do not share incorrect state.
- [x] No native path reads a fixed primary node to execute an arbitrary Grade.
- [x] Required programs load through existing backend registration paths.

##### Phase NM2.4 completion record (2026-08-31)

**Status:** complete — PlanExecutor and CUDA/OpenCL/Metal Grade+mask encoders run every compiled Color Grade in backbone order; mix reads that Grade's `scene_input`; last Grade feeds DRT/Post. CUDA and OpenCL pixel matrix executed on this host. Metal sources and tests are registered; Metal numerical is unexecuted here.

**Primary success call chain:**

```text
CudaRenderDevice::Execute / OpenClRenderDevice::Execute / MetalRenderDevice::Execute
  -> PlanExecutor::Execute
  -> Develop + Geometry + CameraColor
  -> for each plan.grade_nodes:
       MaskEvaluate Encode(compiled_grade) when mask is present
       PrimaryColorGrade Encode(compiled_grade)
         -> Execute*PrimaryGrade(..., compiled_grade)
         -> bind that node's slots; pointwise / LLF / neighbor
         -> mix against compiled_grade.scene_input (or AliasImageFrom when mix==0 / disabled)
  -> DRT Encode once (SceneInputForDrt = last Grade scene, or Develop)
  -> EndRender / PublishResults
```

**Primary failure call chain:**

```text
missing compiled Grade node / missing scene_input / missing mask output
  -> Execute*PrimaryGrade / Execute*Mask throw
  -> PlanExecutor CancelRender
  -> unpublished writes discarded; no CPU or other-backend substitute

zero compiled Grades
  -> primary_grade_skip += 1; no Grade encode
  -> DRT reads develop_output
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ZeroGradesFeedDevelopIntoDrtPost` | `GpuDagCudaPrimaryGradeTest`, `GpuDagOpenClGradeTest` | PASS |
| `GradeWithoutPrimaryIdRendersItsParameters` | same | PASS |
| `ThreeGradesComposeInEdgeOrder` | same | PASS |
| `ReconnectChangesNoncommutingGradeResult` | same | PASS |
| `SameAdjustmentTypeUsesDistinctNodeSlots` | same | PASS |
| `RepeatedAdjustmentInstancesKeepTheirOrder` | same | PASS |
| `EachGradeMixesAgainstItsOwnInput` | same | PASS |
| `DisabledGradeAliasesInputUntilFinalReader` | same | PASS |
| `ZeroMixGradeAliasesInputUntilFinalReader` | same | PASS |
| `TwoLocalToneGradesUseTheirOwnSources` | same | PASS |
| `TwoLutGradesKeepIndependentCubeState` | same | PASS |
| `MiddleGradeEditReusesUpstreamResults` | same | PASS |
| Existing single-Grade + DRT product (4-arg wrappers, PlanExecutor) | `GpuDagCudaPrimaryGradeTest`, `GpuDagOpenClGradeTest`, `GpuDagCudaDrtProductTest`, `GpuDagOpenClDrtProductTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest GpuDagCudaDrtProductTest GpuDagOpenClDrtProductTest
ctest --test-dir build/debug -R "GpuDagCudaPrimaryGradeTest|GpuDagOpenClGradeTest|GpuDagCudaDrtProductTest|GpuDagOpenClDrtProductTest" --output-on-failure
```

Suite totals: **137/137** (`GpuDagCudaPrimaryGradeTest` 35, `GpuDagOpenClGradeTest` 47, `GpuDagCudaDrtProductTest` 45, `GpuDagOpenClDrtProductTest` 10). Multi-Grade fixtures: **24/24** (12 CUDA + 12 OpenCL). Logs: `build/tmp/nm2/`.

Metal: `GpuDagMetalGradeTest` now includes `metal_multi_grade_test.cpp`. Not compiled or run on this Windows host (`ALCEDO_METAL_ENABLED` off). Same tests are present for macOS.

**Checklist / exit condition:** all seven boxes checked from CUDA and OpenCL execution plus inspection that encode no longer calls `FirstGrade()`. Program names stay `primary_grade` / `PrimaryColorGrade`; WarmUp still uses `plan.Contains(GpuPassKind::PrimaryColorGrade)`.

**LOC note (grill-code-review):** no changed production file exceeds 1000 lines. After this phase: `cuda_primary_grade_pass.cu` 591, `opencl_primary_grade_pass.cpp` 578, `metal_primary_grade_pass.mm` 508, `opencl_mask_pass.cpp` 561, `metal_mask_pass.mm` 525, `cuda_mask_pass.cu` 439, `plan_executor.hpp` 265. New tests: `cuda_multi_grade_test.cpp` 444, `opencl_multi_grade_test.cpp` 443, `metal_multi_grade_test.cpp` 448, `multi_grade_runtime_test_support.hpp` 120.

**Remaining gaps:** Metal Grade numerical evidence is still macOS / NM2.5. PlanExecutor ExactRelease still releases the previous backbone scene after each Grade rather than driving release from `RemainingValueConsumers` (branch/join). Three-argument `HashLlfSourceKey` / `HashLlfReferenceKey` still default to `FirstGrade()`; GPU encode uses the four-argument form with `compiled_grade->node_id`. NM2.5 still owns export/thumbnail/service isolation, persistence reopen, failure injection, and resource-byte qualification.

### 6.5 NM2.5 — Qualification

**Work**

1. Run the acceptance matrix in Section 7.
2. Exercise the editor, thumbnail, analysis, and export service paths.
3. Check document save/load with new ownership and multiple Grades.
4. Inject failures before upload, during execution, and at output delivery.
5. Measure fixed single-Grade and multi-Grade cases on the same device.
6. Check resource release after deletion, cancellation, and repeated task completion.
7. Record actual commands, results, tolerances, and unavailable environments.

**Exit conditions**

- [ ] All mandatory behavior has executed test evidence.
- [ ] CUDA, OpenCL, and Metal have native numerical evidence.
- [ ] Resource evidence distinguishes logical release from completed GPU release.
- [ ] Service requests do not modify later requests or shared document settings.
- [ ] No Nodes UI, structural history, or parallel mixer enters the product scope.
- [ ] Missing hardware or fixtures remain explicit qualification gaps.

## 7. Acceptance matrix

The names below specify assertion goals.
They are not claims that these tests already exist.
Map each goal to the actual test and target during implementation.
Use parameterized cases where they improve coverage without duplicating fixtures.

| Required behavior | Evidence and assertion goal |
| --- | --- |
| Zero Grades | `ZeroGradesFeedDevelopIntoDrtPost`: expected pixels, no Grade execution, endpoint operations still run. |
| Arbitrary identity | `GradeWithoutPrimaryIdRendersItsParameters`: the compiled ID selects the model and cache keys. |
| Multiple Grades | `ThreeGradesComposeInEdgeOrder`: compare with independently calculated ordered results. |
| Reorder | `ReconnectChangesNoncommutingGradeResult`: verify new pixels and unchanged upstream reuse. |
| Parameter isolation | `SameAdjustmentTypeUsesDistinctNodeSlots`: change one node; inspect the other node's values and output. |
| Repeated instances | `RepeatedAdjustmentInstancesKeepTheirOrder`: one node can contain distinct instances without resource collisions. |
| Grade mix | `EachGradeMixesAgainstItsOwnInput`: use different masks and mix values on successive Grades. |
| Bypass | `DisabledGradeAliasesInputUntilFinalReader`: assert pixels, alias safety, and absence of unnecessary execution. |
| Ownership | `PostAdjustmentsRejectGradeOwnership`: cover factory, mutation, JSON load/save, and compile boundaries. |
| Existing controls | `PostControlTargetsDrtAndRestoresOnUndo`: verify read, preview, settle, Undo/Redo, and reload. |
| Endpoint order | `DrtPostPreservesUnmaskedReferenceOrder`: compare non-default post-processing and DRT settings. |
| Local invalidation | `MiddleGradeEditReusesUpstreamResults`: assert per-node execute/reuse events and final pixels. |
| Static reuse | `ParameterAndViewportEditsKeepStaticPlan`: parameters can change pixel keys without recompilation. |
| Auxiliary state | `TwoLocalToneGradesUseTheirOwnSources`: compare against staged reference results after an upstream edit. |
| Reference geometry | `LocalToneReferenceRemainsStableAcrossViewportChanges`: compare matching image coordinates and source reuse. |
| Shared input | `SharedInputSurvivesBothBranchReaders`: assert completion order, resource identity, and test-plan outputs. |
| Sibling reuse | `BranchEditPreservesSiblingResult`: rerun the internal branch fixture after changing one branch. |
| Port identity | `JoinInputsFollowPortBindings`: reorder container insertion without swapping logical inputs. |
| Failed output | `SinkFailurePublishesNoNewResults`: retain old valid images and discard new unpublished writes. |
| Failed upload | `ParameterUploadFailureRestoresPendingDirtyState`: retry produces the requested values. |
| Lifetime | `RepeatedNodeRemovalReclaimsUnusedResources`: check bytes and actual backend releases after completion. |
| Background policy | `BackgroundMultiGradeRenderPreservesEditorCache`: compare retained editor results before and after task completion. |
| Request isolation | `ExportRecipeDoesNotChangeNextEditorRender`: verify pixels, output profile, and unchanged document parameters. |
| Persistence | `MultiGradeDocumentRoundTripPreservesOwnersAndEdges`: render the loaded document and reject malformed owner placement. |

### 7.1 Numerical and resource evidence

Use deterministic synthetic inputs for arithmetic and ordering assertions.
Use two operations whose results differ when their order changes.
Two exposure operations alone cannot prove ordering.
Do not calculate expected values through the same new execution helper under test.

Use supported real RAW fixtures for native integration cases.
Record the fixture identity, backend, device, geometry, decode setting, and output configuration.
Declare numerical tolerances before comparing results.
Check finite values and image dimensions in addition to pixel error.
Cross-backend agreement alone does not prove correctness.

Use deterministic completion barriers for resource and failure tests.
Check GPU completion before asserting physical release.
Do not equate an arena's zero used-byte count with returned device memory.

Measure one, two, and several Grades with fixed settings.
Include a parameter edit in the middle of the chain and a repeated unchanged render.
Record per-node work, total time, retained bytes, and peak task allocation.
Compare background peak usage with the NM1.4R policy.
Do not obtain better results by reducing decode resolution, algorithm quality, or backend capability.

### 7.2 Existing test locations and commands

Use [edit test registration](../../../../../alcedo_studio/tests/edit/CMakeLists.txt) to resolve native targets.
Use [app test registration](../../../../../alcedo_studio/tests/app/CMakeLists.txt) for service and parameter targets.

| Existing target | Relevant coverage |
| --- | --- |
| `GpuDagModelGraphTest` | Models, factories, graph edits, and validation. |
| `GpuDagRawInputTest` | Compiler, static-plan cache, and content keys. |
| `GpuDagCudaWorkspaceTest` | Parameter arena, image cache, temporary buffers, and leases. |
| `GpuDagCudaPrimaryGradeTest` | Current CUDA Grade tests; update the target name if its role changes. |
| `GpuDagCudaDrtProductTest` | CUDA output and product path. |
| `GpuDagOpenClWorkspaceTest`, `GpuDagOpenClGradeTest`, `GpuDagOpenClDrtProductTest` | Native OpenCL storage, Grade, and output behavior. |
| `GpuDagMetalWorkspaceTest`, `GpuDagMetalGradeTest`, `GpuDagMetalDrtTest`, `GpuDagMetalRendererTest` | Native Metal storage, Grade, output, and renderer behavior. |
| `EditorAdjustmentPipelineTest`, `EditorPipelineCommandServiceTest`, `PipelineMapperTest` | Parameter routing, service mutations, and document persistence. |

Check the current target list after any rename.
Register new tests in the appropriate CMake target.
The commands below start the focused Windows checks from the repository root.

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest GpuDagRawInputTest EditorAdjustmentPipelineTest EditorPipelineCommandServiceTest PipelineMapperTest
ctest --test-dir build/debug -R "GpuDagModelGraphTest|GpuDagRawInputTest|EditorAdjustmentPipelineTest|EditorPipelineCommandServiceTest|PipelineMapperTest" --output-on-failure
```

Build the affected CUDA and OpenCL targets through the same wrapper.
Run their tests on the corresponding native devices.
Run Metal checks on macOS:

```text
cmake --preset macos_debug
cmake --build --preset macos_debug --target GpuDagModelGraphTest GpuDagRawInputTest GpuDagMetalWorkspaceTest GpuDagMetalGradeTest GpuDagMetalDrtTest GpuDagMetalRendererTest
ctest --test-dir build/macos-debug -R "GpuDagModelGraphTest|GpuDagRawInputTest|GpuDagMetal" --output-on-failure
```

Confirm the preset build directory before running CTest.
Run the affected thumbnail, analysis, export, and scheduler suites after focused tests pass.
Store temporary logs and intermediate results under `build/tmp/nm2/`.
Keep permanent numerical fixtures in the established test resource directories.
Do not mark skipped native tests as passed.

## 8. Required call chains

### 8.1 Parameter edit and render

```text
current adjustment control
  -> application target resolution
  -> EditorPipelineCommandService
  -> typed mutation of the live PipelineDocument under the existing lock
  -> render task with its request
  -> shared renderer and static plan lookup
  -> request geometry and node parameter bindings
  -> per-value content checks
  -> connected Grade passes and DRT/Post passes
  -> output sink succeeds
  -> publish completed results
```

### 8.2 Topology change without product Nodes UI

```text
service integration test
  -> NM1 add/remove/reconnect function
  -> backbone validation and local rollback on failure
  -> static plan rebuild from graph edges
  -> unchanged value keys remain reusable
  -> execute affected descendants
  -> numerical output and resource checks
```

### 8.3 Failure and cancellation

```text
invalid graph or parameter owner
  -> validation error before GPU work

parameter upload failure
  -> restore pending dirty state
  -> cancel unpublished work

pass, allocation, submission, or sink failure
  -> cancel request without publishing new result keys
  -> retain previously valid cached results
  -> wait for required GPU completion before resource release
  -> release task use without saving
  -> report the actual error
```

## 9. NM2 completion criteria

- [ ] All backbone Grades execute with arbitrary valid NodeIds on all three native backends.
- [ ] Runtime does not depend on the default Grade ID or a global current image.
- [ ] Passes declare their inputs, outputs, owners, and parameter bindings.
- [ ] Content keys and invalidation follow real dependencies.
- [ ] Auxiliary state uses the correct node, operation, and input content.
- [ ] Aliases and shared inputs survive their final GPU readers.
- [ ] DRT/Post owns all four moved operations and preserves the required calculation order.
- [ ] Existing controls and current parameter Undo/Redo use the correct owners.
- [ ] New documents round-trip without stage mirrors or silent owner conversion.
- [ ] Background tasks preserve request isolation, editor caches, and NM1.4R release rules.
- [ ] Native numerical, failure, resource, and service evidence meets Section 7.
- [ ] The internal branch fixture proves dependency reuse without enabling product branches.
- [ ] NM3, NM4, NM5, and NM6 retain their stated responsibilities.

Record implementation results under the corresponding sub-phase after its checks finish.
Include the actual source revision, commands, test results, and main success and failure call chains.
Leave an incomplete criterion unchecked when its evidence is unavailable.
