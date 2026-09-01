# Phase NM3 — Multi-Mask Model and Runtime

Date: 2026-09-01

Status: NM3.1 complete; NM3.2–NM3.5 planned

Prerequisite: NM2 complete on CUDA, OpenCL, and Metal.

## 1. Purpose

Give each Color Grade an ordered list of Masks.
Support Brush, Radial, and Linear Gradient sources.
Combine all enabled Masks with Union.
Keep Color Range and Luminance Range as direct Mask fields.

Make raster assets immutable and content-addressed.
Pass active Brush pixels and dirty rectangles as render-request data.
Execute the same Mask rules on CUDA, OpenCL, and Metal.

NM3 does not add Mask controls or viewer input.
NM3 does not add history payloads or Version recovery.
It supplies the final model and runtime identities that NM4 and NM7 need.

## 2. Required context

Read the applicable sections before each sub-phase.
Follow a link when it defines an owner, dependency, or failure rule.
Check current callers and tests before a change.
Do not infer a component's purpose from its name.

| Source | Required context |
| --- | --- |
| [Node and mask master plan](../node_mask_editor_master_plan.md) | Sections 3, 8–12, 18–20, 21.4, 23, and 24 define Mask behavior, assets, requests, and exit conditions. |
| [NM2 execution plan](phase_nm2_multi_grade_runtime_plan.md) | Sections 2–4 define the current multi-Grade plan, content keys, resources, and three native backends. |
| [NM1 execution plan](phase_nm1_pipeline_document_editing_plan.md) | Sections 3–5 define live document edits. Section 10.5 defines request data and resource release. |
| [GPU DAG plan](../gpu_dag_pipeline_rebuild_phase_plan.md) | Sections 12–17 and 25–27 define passes, values, Mask runtime work, caches, and failure behavior. |
| [Metal execution plan](../gpu_dag_metal_migration_phase_plan.md) | Metal command ownership, staging, resource completion, and numerical evidence. |
| [OpenCL execution plan](../gpu_dag_opencl_migration_phase_plan.md) | OpenCL program registration, resource ownership, and numerical evidence. |
| [Single live pipeline plan](../../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | One live edit object, task-owned request data, and saved checkpoint identity. |

The master plan defines current product behavior.
NM2 defines the current Grade runtime.
This plan replaces only the current single-Mask representation.

Do not restore top-level Mask nodes as a second model.
Do not create a second document or executor for Mask preview.
Do not switch to CPU or another backend after a native failure.

### 2.1 Current source map

The paths below show the implementation on 2026-09-01.
Check them again at the implementation revision.

| Component | Source and current responsibility |
| --- | --- |
| Color Grade model | [color_grade_node_model.hpp](../../../../../alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp) owns adjustments, enabled, mix, and one optional graph Mask input. |
| Analytic Mask model | [analytic_mask_node_model.hpp](../../../../../alcedo_studio/src/include/edit/graph/analytic_mask_node_model.hpp) defines a top-level Radial or `GraduatedNd` node. |
| Raster Mask model | [raster_mask_node_model.hpp](../../../../../alcedo_studio/src/include/edit/graph/raster_mask_node_model.hpp) defines a top-level node with one asset key. |
| Document I/O | [pipeline_document.cpp](../../../../../alcedo_studio/src/edit/graph/pipeline_document.cpp) reads and writes Mask nodes and Mask edges in the graph arrays. |
| Graph validation | [pipeline_graph.cpp](../../../../../alcedo_studio/src/edit/graph/pipeline_graph.cpp) validates top-level Mask nodes and Mask edges as normal graph data. |
| Compiler | [graph_compiler.cpp](../../../../../alcedo_studio/src/edit/runtime/graph_compiler.cpp) finds the first incoming Mask edge for each Grade. |
| Execution plan | [execution_plan.hpp](../../../../../alcedo_studio/src/include/edit/runtime/execution_plan.hpp) stores one optional `CompiledMask` and one Mask output per Grade. |
| Content identity | [result_content_key.cpp](../../../../../alcedo_studio/src/edit/runtime/result_content_key.cpp) hashes one Mask node into one Grade key. |
| Shared execution | [plan_executor.hpp](../../../../../alcedo_studio/src/include/edit/runtime/plan_executor.hpp) evaluates or reuses one Mask output before each Grade. |
| Raster assets | [mask_store.hpp](../../../../../alcedo_studio/src/include/edit/mask/mask_store.hpp) exposes `Save()` with a caller-supplied key. |
| Product renderer | [renderer.hpp](../../../../../alcedo_studio/src/include/edit/runtime/renderer.hpp) owns a temporary `MaskStore` and passes it to the native device. |
| Request data | [pipeline_apply_request.hpp](../../../../../alcedo_studio/src/include/edit/pipeline/pipeline_apply_request.hpp) carries geometry, output, cache, and cancellation data. |
| CUDA Mask runtime | [cuda_mask_pass.cu](../../../../../alcedo_studio/src/edit/runtime/cuda/cuda_mask_pass.cu) evaluates one analytic or raster Mask. |
| OpenCL Mask runtime | [opencl_mask_pass.cpp](../../../../../alcedo_studio/src/edit/runtime/opencl/opencl_mask_pass.cpp) implements the same single-Mask path. |
| Metal Mask runtime | [metal_mask_pass.mm](../../../../../alcedo_studio/src/edit/runtime/metal/metal_mask_pass.mm) implements the same single-Mask path. |
| Mask resource cache | [mask_texture_cache.hpp](../../../../../alcedo_studio/src/include/edit/runtime/mask_texture_cache.hpp) caches persistent raster textures by `MaskAssetKey`. |
| Existing Mask tests | [mask_store_test.cpp](../../../../../alcedo_studio/tests/edit/mask/mask_store_test.cpp) and the native Mask test files cover one source at a time. |

### 2.2 Current behavior

The document can store several top-level Mask nodes.
A Color Grade has only one optional `mask` input port.
Graph validation permits only one edge on that port.
The compiler stops after the first matching Mask edge.

The current analytic node supports Radial and `GraduatedNd` parameters.
The current raster node stores an asset key, bounds, feather, and invert.
The native paths already support partial R8 uploads in direct tests.

The current `MaskStore::Save()` writes to a caller-selected key.
It atomically replaces an existing file with that key.
This behavior cannot preserve old pixels for later Undo.

The current dirty-rectangle argument does not enter `PipelineApplyRequest`.
The pass encoder calls the native Mask function without that argument.
The renderer therefore cannot send active Brush updates through the product path.

### 2.3 Existing limits

- A Grade cannot own more than one effective Mask.
- A Mask has no stable `MaskId` separate from `NodeId`.
- Mask list order and Mask graph topology use the same representation.
- The compiler has no Union output.
- The result cache has no per-Mask source key.
- A caller can replace pixels under an existing raster key.
- Partial upload can modify a texture that represents a persistent asset key.
- The product request has no active raster value or dirty rectangle.
- Color Range and Luminance Range have no direct serialized fields.
- Existing `GraduatedNd` names do not match the product term Linear Gradient.

## 3. Terms and scope

| Term | Meaning |
| --- | --- |
| Mask | One local coverage item that belongs to one Color Grade. |
| MaskId | Stable identity for one Mask during its lifetime. |
| Source | Brush, Radial, or Linear Gradient coverage data. |
| Persistent raster | An immutable R8 asset in `MaskStore`. |
| Active raster | Request-owned Brush pixels for one current input sequence. |
| Source coverage | Coverage from one source after source feather and invert. |
| Effective Mask coverage | Source coverage after range fields and opacity. |
| Union coverage | Maximum effective coverage from all enabled Masks in one Grade. |
| Range input | The connected scene input of the owning Color Grade. |
| Dirty rectangle | One raster region that changed since the prior active revision. |
| Static structure | Mask IDs, source kinds, and pass dependencies that require compilation. |
| Pixel identity | Source values, enabled state, opacity, assets, and active revisions that affect pixels. |

### 3.1 Included work

- Add stable `MaskId` values.
- Store an ordered Mask list inside each Color Grade.
- Add Brush, Radial, and Linear Gradient source variants.
- Add direct Color Range and Luminance Range fields.
- Validate Mask values and duplicate identities.
- Save and load the new Grade-owned Mask schema.
- Remove top-level Mask nodes and Mask edges from the supported document shape.
- Compile each Mask source and one Union output per Grade.
- Execute zero, disabled, one, or several enabled Masks.
- Preserve UI list order without making it pixel order.
- Make raster assets immutable and content-addressed.
- Pass active raster values and dirty rectangles as task-owned request data.
- Keep persistent and active raster textures in separate cache entries.
- Run native CUDA, OpenCL, and Metal Mask paths.
- Prove behavior, numerical agreement, cache safety, and resource release.

### 3.2 Excluded work

- Mask panels, viewer tools, pointer input, and QSG overlay.
- Typed history batches, Undo/Redo, Version recovery, and Paste.
- A project-level asset reachability scan or deletion command.
- Color Range and Luminance Range selection algorithms.
- Content-dependent range preview textures.
- Boolean operations other than Union.
- Mask graph nodes, Mask graph edges, or free Mask connectors.
- AI segmentation.
- CPU product execution or another-backend replacement.
- Final project metadata release and old-project conversion.

## 4. Design requirements

### 4.1 Grade-owned Mask model

Add a GPU-free Mask model under `edit/mask/`.
Keep the model independent of Qt and native GPU types.

Use this logical shape:

```cpp
class MaskId;

struct BrushMaskSource {
  std::optional<MaskAssetKey> asset_key;
  MaskAssetDescriptor descriptor;
  float feather_radius = 0.0f;
};

struct RadialMaskSource {
  float center_x = 0.5f;
  float center_y = 0.5f;
  float major_radius = 0.5f;
  float minor_radius = 0.5f;
  float rotation = 0.0f;
  float inner_feather = 0.0f;
  float outer_feather = 0.0f;
};

struct LinearGradientMaskSource {
  float origin_x = 0.5f;
  float origin_y = 0.5f;
  float normal_x = 0.0f;
  float normal_y = 1.0f;
  float transition_distance = 0.2f;
  float start_value = 1.0f;
  float end_value = 0.0f;
};

using MaskSource = std::variant<BrushMaskSource,
                                RadialMaskSource,
                                LinearGradientMaskSource>;

struct ColorRangeModel {
  bool enabled = false;
};

struct LuminanceRangeModel {
  bool enabled = false;
};

struct MaskModel {
  MaskId id;
  std::string display_name;
  bool enabled = true;
  float opacity = 1.0f;
  bool invert = false;
  MaskSource source;
  std::optional<ColorRangeModel> color_range;
  std::optional<LuminanceRangeModel> luminance_range;
};
```

This shape fixes identity and ownership.
The implementation can adjust field grouping when tests show a clear need.
It must not change the locked coverage rules.

Use `MaskId` for model lookup, runtime events, content keys, requests, and later history.
Do not use a list index as identity.
Reject an empty `MaskId`.
Reject duplicate `MaskId` values inside one Grade.

A Brush can have no asset before its first settled stroke.
That source produces zero coverage.
Do not create a fake all-zero asset for this state.

`ColorGradeNodeModel` owns `std::vector<MaskModel>`.
Remove its optional graph Mask input port.
Do not keep a hidden legacy Mask pointer or edge.

### 4.2 Model operations and validation

Provide focused model operations:

```text
AddMask(mask, index)
RemoveMask(mask_id)
ReplaceMaskSource(mask_id, source)
SetMaskEnabled(mask_id, enabled)
SetMaskOpacity(mask_id, opacity)
SetMaskInvert(mask_id, invert)
MoveMaskForDisplay(mask_id, index)
FindMask(mask_id)
```

The exact return type can follow current graph-command error style.
Each operation must preserve the original list after failure.
Do not validate with a full document JSON copy.

Apply these value rules:

- All floating-point values must be finite.
- Opacity must stay in `[0, 1]`.
- Normalized positions and bounds must use the established coordinate rules.
- Radii and feather values must be nonnegative.
- A Linear Gradient normal must have a valid direction.
- Raster width and height must satisfy the existing maximum-axis rule.
- A persistent Brush key must match its stored descriptor.
- A non-null range field must contain only its supported fields.

NM3 supports only disabled range models.
Reject `enabled = true` before GPU work.
Do not guess color samples, thresholds, softness, or luminance units.

Mask list order supports display and later keyboard navigation.
`MoveMaskForDisplay` changes serialization order only.
It does not mark pixel content dirty.
It does not request a render.
NM4 preserves this order in saved document data without a photo edit commit.

### 4.3 Coverage rules

Evaluate one Mask in this order:

```text
raw source coverage
  -> source feather
  -> invert when enabled
  -> multiply Color Range coverage, or 1 when absent
  -> multiply Luminance Range coverage, or 1 when absent
  -> multiply opacity
  -> clamp to [0, 1]
```

NM3 range fields are absent or disabled.
They contribute `1`.

Combine enabled Masks with this rule:

```text
union = max(mask_0, mask_1, ...)
```

Use these exact boundaries:

```text
Mask list is empty                -> Grade coverage is 1
Mask list is not empty
and all Masks are disabled        -> Grade coverage is 0
One or more Masks are enabled     -> maximum enabled Mask coverage
Brush has no asset                -> that Brush coverage is 0
```

Disabled Masks do not load assets.
They do not allocate source textures.
They do not run source kernels.

The Grade keeps the NM2 mix rule:

```text
weight = clamp(union * grade_mix, 0, 1)
output = input + weight * (adjusted - input)
```

Do not apply opacity after the Grade mix.
Do not sum Mask coverage.
Do not save a combine-mode field.

### 4.4 Serialization and document validation

Write `masks` inside each Color Grade object.
Write every Mask in display order.
Write an explicit source discriminator.

Use this logical JSON shape:

```json
{
  "id": "grade.face",
  "type": "color_grade",
  "masks": [
    {
      "id": "mask.face.radial",
      "display_name": "Face",
      "enabled": true,
      "opacity": 0.8,
      "invert": false,
      "source": {
        "kind": "radial",
        "center_x": 0.5,
        "center_y": 0.45,
        "major_radius": 0.3,
        "minor_radius": 0.2,
        "rotation": 0.0,
        "inner_feather": 0.0,
        "outer_feather": 0.15
      },
      "color_range": null,
      "luminance_range": null
    }
  ]
}
```

Use the repository's actual node type text.
The sample does not authorize a new type string.

Always write both direct range fields.
Write `null` when a range is absent.
Accept only the defined disabled object when it is present.

Reject these inputs at the read boundary:

- Missing or non-array `masks` on a Color Grade.
- Empty or duplicate Mask IDs.
- Unknown source kinds.
- Invalid or non-finite source values.
- Enabled range objects.
- A top-level Analytic or Raster Mask node.
- Any graph edge with Mask data type.
- A Color Grade `mask` input port from the old shape.

Do not convert old Mask nodes into the new list.
Do not move data silently.
Return the actual unsupported-document error.

Update the current development document version when the reader needs a clean schema boundary.
Do not release the final project metadata version.
NM4 owns that release.

### 4.5 Immutable raster assets

Replace caller-selected `Save()` with a content-addressed operation:

```cpp
auto Put(const MaskAssetDescriptor& descriptor,
         std::span<const std::uint8_t> pixels) -> MaskAssetKey;
```

Build the key from these canonical bytes:

1. Asset format version.
2. Pixel format identifier for tightly packed R8.
3. Width and height in fixed byte order.
4. Reference bounds as canonical float bits.
5. Pixel byte count.
6. All pixel bytes in row order.

Use the established xxHash dependency with a 128-bit digest.
Encode the full digest as lowercase hexadecimal text.
Do not use the runtime 64-bit `ContentKey` as a disk asset key.

When the destination exists, load and compare its descriptor and pixels.
Return the key only when all bytes match.
Report a collision or corrupt file as an error.
Do not replace that file.

When the destination does not exist:

1. Write one complete sibling file.
2. Flush the file.
3. Close the file.
4. Publish it without replacing an existing destination.
5. Handle a concurrent winner by reading and comparing the published file.
6. Remove only the failed temporary file.

The operation must be safe when two writers store the same content.
It must fail when existing bytes do not match the calculated key.

Keep host-cache eviction separate from disk deletion.
Do not add a disk deletion API in NM3.
NM4 needs history reachability before safe asset cleanup can exist.

### 4.6 Active raster request data

Add one immutable request value for active Brush pixels.
Keep it outside `PipelineDocument`.
Keep it outside persistent `MaskStore`.

Use this logical shape:

```text
ActiveRasterMaskInput
  owner_node_id
  mask_id
  session_generation
  content_revision
  descriptor
  shared immutable R8 pixels
  dirty_rectangle
```

One render request can carry zero or more active raster inputs.
Reject duplicate `(NodeId, MaskId)` entries.
Reject an entry when its Grade or Mask does not exist.
Reject an entry when the Mask source is not Brush.

The producer increments `content_revision` for each pixel change.
The producer changes `session_generation` for a new authoring session.
The request owns the pixel snapshot through shared immutable storage.
Do not pass a raw pointer to a mutable authoring buffer.

Put this data on `PipelineApplyRequest` or an equivalent task-owned value.
Pass it through `Renderer`, `PlanExecutor`, and the native Mask encoder.
Do not store it on the shared executor between calls.

An active input overrides only the matching Brush source.
Other Masks still read their saved model values.
The override never changes the saved asset key.

### 4.7 Dirty-rectangle rules

Use a rectangle in raster-asset texel space.
Clip it to the active descriptor extent.
Reject a descriptor or byte count mismatch.

Keep persistent and active textures separate:

```text
persistent texture key = MaskAssetKey
active texture key     = NodeId + MaskId + session_generation
```

Never patch a persistent texture under an immutable `MaskAssetKey`.
Create or replace the active texture on the first revision.
Upload only the dirty rectangle on later matching revisions.

Recalculate Brush feather for the affected region.
Expand the work region by the required feather radius.
Use a full region only when the signed-distance method requires it.
Record that choice in test evidence.

Recalculate Union for the affected output region.
Read all enabled Mask results in that region.
This rule permits coverage to decrease after an erasing Brush update.

Use a full Union region after these changes:

- Add or remove a Mask.
- Enable or disable a Mask.
- Change opacity or invert.
- Change an analytic source.
- Change Brush descriptor or feather.
- Start a new active session generation.

An empty dirty rectangle with a higher revision is invalid.
The caller must state the changed region.

### 4.8 Compiler and execution plan

Remove `CompileIncomingMask()` and Mask-edge traversal.
Compile the owning Grade's Mask list directly.

Add these logical compiled types:

```text
CompiledMaskSource
  owner_node_id
  mask_id
  source_kind
  enabled
  source_output
  range_input

CompiledMaskStack
  owner_node_id
  sources sorted by MaskId for runtime determinism
  union_output
```

Store display order only in the model.
Sort compiled sources by `MaskId`.
This sort makes runtime order independent from UI order.

Give each Mask pass an explicit `MaskId` owner field.
Do not reuse a list index in pass identity.
Derive internal `GraphValueId` ports from the stable Mask identity.
Keep the owning `NodeId` as the graph-value producer.

Use explicit values for these steps:

```text
source evaluate
  -> optional Brush feather
  -> per-Mask effective coverage
  -> Union
  -> Grade mix
```

Add a `MaskUnion` pass kind.
Keep `MaskFeather` only when it describes distinct GPU work.
Do not emit two producers for the same `GraphValueId`.

Bind `range_input` to the owning Grade's `scene_input`.
NM3 does not execute an enabled range.
The binding fixes the future dependency and prevents feedback from Grade output.

Static structure includes:

- Mask IDs.
- Source kinds.
- Required source and Union passes.
- Range-field presence.

Static structure excludes:

- Display order.
- Display names.
- Enabled values.
- Opacity and invert.
- Analytic parameters.
- Asset keys.
- Active raster revisions.

Adding or removing a Mask rebuilds the static plan.
Changing a source kind rebuilds the static plan.
Changing values only changes pixel keys.

### 4.9 Content keys and cache behavior

Create one source key for each Mask.
Create one Union key for each Grade.

Each source key includes:

1. Owning `NodeId` and `MaskId`.
2. Source kind and implementation version.
3. Source parameters.
4. Enabled, opacity, and invert values when they affect that source output.
5. Persistent asset key or active raster identity.
6. Descriptor and render geometry.
7. Range-field state and connected range-input key.

An active raster identity includes session generation and content revision.
The dirty rectangle does not define content.
It defines upload work only.

Build the Union key from enabled source keys.
Sort these keys by `MaskId`, not by hash value.
Do not include display order or display names.

Use a defined constant key for all-disabled coverage.
Use no Mask key for an empty list.
An empty list must preserve the NM2 unmasked Grade key behavior.

Changing one Mask invalidates that source, the Union, the owning Grade, and descendants.
It must not invalidate upstream scene results or another Grade's Mask outputs.

Do not publish partial source or Union results after a later failure.
Keep prior valid results available.
Release active textures when their session generation ends and GPU use completes.

### 4.10 Native backend behavior

Use one shared model-to-plan path for all backends.
Use one CPU reference evaluator only in tests.
Do not call it from product rendering.

CUDA, OpenCL, and Metal must implement:

- Radial coverage with the current coordinate meaning.
- Linear Gradient coverage with the renamed product term.
- Brush R8 sampling with the existing reference-space mapping.
- Source feather and invert.
- Per-Mask opacity.
- R8 Union with maximum coverage.
- Empty-list and all-disabled boundaries.
- Active raster partial upload.
- Correct resource completion before reuse or release.

Use the same normalization and edge behavior on all backends.
Document the R8 rounding rule.
Use a tolerance no larger than one R8 code value for coverage comparisons.

Update OpenCL program registration for new kernels.
Update Metal shader build wiring for new functions.
Keep CUDA kernels in the existing runtime Mask module.

Do not create a pipeline object for each invocation.
Use the established native program and pipeline caches.

### 4.11 Service and failure boundaries

Keep one live document and executor per image.
The current render lock protects document reads and task setup.
Each task owns its active raster request values.

Thumbnail, analysis, and export use settled assets only.
Reject active raster inputs on bypass tasks unless a caller explicitly requests preview semantics.
Do not let a background task read an editor authoring buffer.

Report these errors without replacement behavior:

- Invalid Mask model or duplicate identity.
- Missing or corrupt asset.
- Asset descriptor mismatch.
- Unsupported enabled range.
- Invalid active raster target or revision.
- Native allocation, upload, kernel, submission, or sink failure.

Cancel unpublished writes after failure.
Keep the prior displayed frame and prior valid cache entries.
Do not change the document or asset key because GPU work failed.

### 4.12 Code naming rule

Do not put `NM3`, `Phase 3`, or another phase identifier in code.
This rule applies to production and test code.
It includes filenames, targets, identifiers, comments, strings, and generated files.

Name each artifact for its behavior.
Examples include `mask_model`, `mask_union`, `active_raster_mask`, and `multi_mask_runtime_test_support`.
Phase identifiers can appear in this roadmap file and its completion records.

## 5. Transition boundaries

### 5.1 Boundary from NM2

Keep the ordered multi-Grade backbone and DRT/Post ownership.
Keep each Grade's connected `scene_input` and `scene_output`.
Replace only the Grade Mask binding and Mask execution work.

The empty Mask list must match NM2 unmasked pixels.
The one-Mask case must match the current native reference within tolerance.
Record any intentional naming-only JSON change.

### 5.2 Boundary to NM4

NM3 supplies stable `MaskId`, `MaskSource`, `MaskAssetKey`, and model operations.
NM4 records those values in typed forward and inverse changes.

NM3 does not create edit commits.
NM3 does not scan history reachability.
NM3 does not delete disk assets.

### 5.3 Boundary to NM7

NM3 supplies active raster request data and dirty-rectangle execution.
NM7 owns pointer samples, temporary Brush raster generation, and QSG overlay.

NM7 must call `MaskStore::Put()` only for a settled Brush result.
It then replaces the saved Brush asset key through the NM4 history path.

## 6. Sub-phase sequence

| Phase | Status | Result |
| --- | --- | --- |
| NM3.1 | complete | Grade-owned Mask identities, sources, validation, and document schema. |
| NM3.2 | planned | Immutable raster assets and task-owned active raster inputs. |
| NM3.3 | planned | Multi-Mask compiler, keys, Union plan, cache, and lifetime rules. |
| NM3.4 | planned | Native CUDA, OpenCL, and Metal source evaluation and Union execution. |
| NM3.5 | planned | Model, storage, native, service, failure, and resource qualification. |

Implement these sub-phases in order.
Keep each interface change buildable on all enabled platforms.
Change direct callers with the interface.
Do not keep a second single-Mask renderer during transition.

### 6.1 NM3.1 — Grade-owned Mask model and schema

**Work**

1. Add `MaskId`, source variants, range placeholders, and `MaskModel`.
2. Add the ordered Mask list and focused operations to `ColorGradeNodeModel`.
3. Remove the Color Grade graph Mask input.
4. Remove supported top-level Analytic and Raster Mask nodes.
5. Move current Radial, Linear Gradient, Brush, feather, and invert values into the new model.
6. Add model validation and local failure restoration.
7. Update document write and read rules.
8. Reject the old Mask-node shape without conversion.
9. Add model, operation, validation, and round-trip tests.

**Primary files**

- Add purpose-named model files under `src/include/edit/mask/` and `src/edit/mask/`.
- Update `color_grade_node_model.hpp` and `color_grade_node_model.cpp`.
- Update `pipeline_document.cpp` and document validation tests.
- Update `pipeline_graph.cpp` only to remove obsolete Mask-edge assumptions.
- Remove obsolete Mask-node sources from `EditGraph` after all callers move.

Do not name a new file or target after this sub-phase.

**Primary success call chain**

```text
ColorGradeNodeModel::AddMask
  -> validate MaskId, common fields, and source fields
  -> reserve list storage
  -> insert one owned MaskModel
  -> mark Mask structure dirty
  -> serialize inside the owning Color Grade
  -> read the same Mask list and validate the document
```

**Primary failure call chain**

```text
duplicate MaskId / invalid value / old top-level Mask node or edge
  -> model or document validation error
  -> restore the original Mask list when a live operation started
  -> no partial document and no render request
```

**Exit conditions**

- [x] One Color Grade owns zero, one, or many Masks with stable IDs.
- [x] Brush, Radial, and Linear Gradient use one source variant.
- [x] Color Range and Luminance Range exist as direct fields.
- [x] Enabled range values fail before GPU work.
- [x] Duplicate IDs and invalid values leave the model unchanged.
- [x] JSON round-trip preserves display order and every supported field.
- [x] Top-level Mask nodes and Mask edges fail with an explicit error.
- [x] No supported code path retains the old single-Mask graph ownership.

##### Phase NM3.1 completion record (2026-09-01)

**Status:** complete — Grade-owned `MaskModel` list, source variants, range placeholders, document format 3, and first-enabled Mask compile/GPU path.

**Primary success call chain:**

```text
ColorGradeNodeModel::AddMask
  -> ValidateMaskModel + unique MaskId
  -> reserve then insert MaskModel
  -> HashGraphTopology (MaskId + source kind, sorted by MaskId)
  -> Color Grade JSON "masks"
  -> PipelineDocument::FromJson validates and restores the list
```

**Primary failure call chain:**

```text
duplicate MaskId / invalid value / enabled range / Analytic or Raster node / "mask" edge
  -> std::runtime_error at ColorGradeNodeModel or PipelineDocument::FromJson
  -> live ops leave the original Mask list unchanged; FromJson produces no document
  -> no render
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MultipleMasksBelongToOneColorGrade` | `GpuDagModelGraphTest` | PASS |
| `DuplicateMaskIdLeavesGradeUnchanged` | `GpuDagModelGraphTest` | PASS |
| `InvalidMaskValuesFailBeforeDocumentMutation` | `GpuDagModelGraphTest` | PASS |
| `MaskListRoundTripPreservesSourcesOrderAndRangeFields` | `GpuDagModelGraphTest` | PASS |
| `TopLevelMaskNodesAndEdgesAreRejected` | `GpuDagModelGraphTest` | PASS |
| `EnabledRangeFailsBeforeGpuWork` | `GpuDagModelGraphTest` | PASS |
| Color Grade has no graph `mask` port | `ColorGradeHasNoMaskInputPort` | PASS |
| Display reorder keeps `MaskId` | `MoveMaskForDisplayDoesNotChangeMaskIdentity` | PASS |
| Compiler, static plan cache, content keys | `GpuDagRawInputTest` | 72/72 PASS |
| Legacy stage JSON vs document format | `PipelineMapperTest` | 30/30 PASS (2 skipped) |
| CUDA Mask sampling / mix | `GpuDagCudaMaskTest` | 11/11 PASS |
| CUDA primary + multi-Grade | `GpuDagCudaPrimaryGradeTest` | 35/35 PASS |
| CUDA DRT product / result cache | `GpuDagCudaDrtProductTest` | 49/49 PASS |
| OpenCL Grade + Mask + multi-Grade | `GpuDagOpenClGradeTest` | 47/47 PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest GpuDagRawInputTest PipelineMapperTest GpuDagCudaMaskTest GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest GpuDagOpenClGradeTest
ctest --test-dir build/debug --output-on-failure -R GpuDagModelGraphTest
ctest --test-dir build/debug --output-on-failure -R GpuDagRawInputTest
ctest --test-dir build/debug --output-on-failure -R PipelineMapperTest
ctest --test-dir build/debug --output-on-failure -R GpuDagCudaMaskTest
ctest --test-dir build/debug --output-on-failure -R GpuDagCudaPrimaryGradeTest
ctest --test-dir build/debug --output-on-failure -R GpuDagCudaDrtProductTest
ctest --test-dir build/debug --output-on-failure -R GpuDagOpenClGradeTest
```

Suite totals: `GpuDagModelGraphTest` 58/58; `GpuDagRawInputTest` 72/72; `PipelineMapperTest` 30/30 (2 skipped); `GpuDagCudaMaskTest` 11/11; `GpuDagCudaPrimaryGradeTest` 35/35; `GpuDagCudaDrtProductTest` 49/49; `GpuDagOpenClGradeTest` 47/47.

Metal Mask / multi-Grade tests were not executed (Windows host). Sources were updated with the same Grade-owned lookup as CUDA and OpenCL.

**Checklist / exit condition:** all NM3.1 boxes checked.

**LOC note (grill-code-review):** `mask_model.cpp` 402, `mask_model.hpp` 155, `color_grade_node_model.cpp` 305, `color_grade_node_model.hpp` 154, `graph_compiler.cpp` 430, `result_content_key.cpp` 430, `pipeline_document.cpp` 238, `color_grade_mask_model_test.cpp` 224. No changed file exceeds ~1000 LOC. Deleted Analytic/Raster Mask node sources after callers moved.

**Remaining gaps:** Union, `MaskUnion`, per-Mask `GraphValueId`s, `MaskStore::Put()`, and active raster request data stay in NM3.2–NM3.4. Empty Mask list and all-disabled list still compile as no `CompiledMask` (full Grade coverage). Product all-disabled zero-coverage is not implemented here.

### 6.2 NM3.2 — Immutable assets and active raster inputs

**Work**

1. Add canonical asset-byte construction.
2. Replace `Save()` with content-addressed `Put()`.
3. Add non-replacing atomic publication and concurrent-writer checks.
4. Verify existing assets before key reuse.
5. Keep host-cache eviction independent from disk files.
6. Add the active raster request value.
7. Pass the request through Apply, Renderer, device, and pass encoders.
8. Add separate active texture identity and lifetime.
9. Add asset, request, cache-separation, and failure tests.

**Primary files**

- Update `mask_asset.hpp`, `mask_store.hpp`, and `mask_store.cpp`.
- Update `pipeline_apply_request.hpp` and direct request builders.
- Update `renderer.hpp`, `renderer.inl.hpp`, and native device entrypoints.
- Update `mask_texture_cache.hpp` or add one focused active-raster cache type.
- Update `EditMaskStore`, runtime, and test target dependencies for xxHash.

**Primary success call chain**

```text
settled R8 pixels + descriptor
  -> MaskStore::Put
  -> canonical bytes and 128-bit content digest
  -> non-replacing temporary-file publication
  -> verify existing or newly published bytes
  -> return immutable MaskAssetKey

active Brush pixels
  -> PipelineApplyRequest active raster input
  -> Renderer and PlanExecutor
  -> active texture for NodeId + MaskId + session generation
  -> dirty rectangle upload
```

**Primary failure call chain**

```text
invalid pixels / corrupt existing file / digest collision / publish failure
  -> remove only the incomplete temporary file
  -> preserve every existing asset
  -> return the actual storage error

invalid active target / duplicate input / stale descriptor
  -> reject before native upload
  -> preserve persistent texture and saved asset key
```

**Exit conditions**

- [ ] Equal descriptors and pixels return one equal key.
- [ ] Different descriptors or pixels return different keys.
- [ ] Existing mismatched bytes never get replaced.
- [ ] Interrupted writes leave no published partial asset.
- [ ] Concurrent equal writers produce one verified asset.
- [ ] Host-cache eviction never deletes disk data.
- [ ] Active pixels travel as immutable task-owned request data.
- [ ] Active uploads never patch a persistent asset texture.

### 6.3 NM3.3 — Compiler, keys, Union plan, and lifetime

**Work**

1. Replace one `CompiledMask` with compiled source and stack values.
2. Compile sources from each Grade's Mask list.
3. Sort runtime sources by `MaskId`.
4. Bind range input to the Grade's connected scene input.
5. Add distinct source, feather, effective, and Union outputs.
6. Add `MaskUnion` pass identity and validation.
7. Build per-source and per-Union content keys.
8. Keep display order out of static and pixel identity.
9. Propagate local invalidation through the owning Grade and descendants.
10. Track source, Union, alias, active texture, and Grade consumers.
11. Add compiler, key, plan-validation, and resource tests.

**Primary files**

- Update `pass_kind.hpp`, `execution_plan.hpp`, and `execution_plan.cpp`.
- Update `graph_compiler.cpp` and static-plan tests.
- Update `result_content_key.hpp` and `result_content_key.cpp`.
- Update `plan_executor.hpp`, pass statistics, and workspace lifetime helpers.
- Keep all Mask values under the owning Grade's runtime identity.

**Primary success call chain**

```text
GraphCompiler::CompileStatic
  -> walk Color Grades in backbone order
  -> read each Grade-owned Mask list
  -> sort compiled sources by MaskId
  -> create source and Union values with explicit dependencies
  -> validate producers and consumers
  -> BuildFrameResultContentKeys
  -> source keys -> Union key -> Grade key -> descendant keys
```

**Primary failure call chain**

```text
duplicate compiled value / missing Union input / wrong range input / unknown source kind
  -> ValidateExecutionPlan or compiler error
  -> fail before GPU work
  -> retain the prior static plan and published results
```

**Exit conditions**

- [ ] Empty Mask lists compile without a Mask pass and use full Grade coverage.
- [ ] Nonempty all-disabled lists compile a defined zero-coverage result.
- [ ] Enabled sources have stable `MaskId` pass ownership.
- [ ] List reorder changes neither static key nor pixel key.
- [ ] Add, remove, or source-kind changes rebuild the static plan.
- [ ] Value changes keep the static plan and update only required content keys.
- [ ] Range input always equals the owning Grade's scene input.
- [ ] Failed planning or execution publishes no partial Mask result.
- [ ] Active and persistent resources live through their final GPU readers.

### 6.4 NM3.4 — Native multi-Mask execution

**Work**

1. Adapt current Radial and raster evaluators to `MaskModel`.
2. Rename the current `GraduatedNd` runtime meaning to Linear Gradient.
3. Execute each enabled source by stable identity.
4. Apply feather, invert, and opacity in the required order.
5. Add native maximum-Union kernels.
6. Implement empty-list and all-disabled boundaries.
7. Implement active raster partial upload and Union region updates.
8. Update OpenCL program registration.
9. Update Metal shader build and pipeline lookup.
10. Update CUDA, OpenCL, and Metal behavior tests from one shared matrix.

**Primary files**

- Update the CUDA, OpenCL, and Metal Mask pass sources from Section 2.1.
- Update native Mask headers and pass encoders.
- Update OpenCL `mask.cl` and program-name registration.
- Update Metal `mask.metal` and existing shader target wiring.
- Add a purpose-named shared test-support header.

**Primary success call chain**

```text
PlanExecutor for one Color Grade
  -> evaluate or reuse every enabled Mask source
  -> apply Brush feather or analytic transition
  -> apply invert and opacity
  -> native maximum Union into one R8 output
  -> Grade mixes against its own scene input
  -> downstream Grade or DRT/Post
```

**Primary failure call chain**

```text
missing asset / upload failure / kernel failure / submission failure
  -> native error reaches PlanExecutor
  -> CancelRender
  -> discard unpublished source, Union, and Grade writes
  -> retain prior valid results
  -> no CPU or other-backend replacement
```

**Exit conditions**

- [ ] CUDA executes all three source kinds and Union.
- [ ] OpenCL executes all three source kinds and Union.
- [ ] Metal executes all three source kinds and Union.
- [ ] Empty, all-disabled, and enabled boundaries match the shared reference.
- [ ] Feather, invert, opacity, and Union use the same order on all backends.
- [ ] One Mask edit leaves sibling source results reusable.
- [ ] Active Brush upload uses the required dirty region.
- [ ] Coverage differs by no more than the declared R8 tolerance.
- [ ] Native failures use no weaker rendering path.

### 6.5 NM3.5 — Qualification

**Work**

1. Run the acceptance matrix in Section 7.
2. Run model and document save/load checks.
3. Run immutable-store failure and reopen checks.
4. Run CUDA, OpenCL, and Metal numerical tests.
5. Exercise editor-session, thumbnail, analysis, and export request boundaries.
6. Inject asset, upload, execution, and sink failures.
7. Measure one, several, and disabled Mask cases.
8. Measure full and partial Brush updates on the same device.
9. Check resource release after session replacement and cancellation.
10. Record commands, devices, tolerances, bytes, and unavailable evidence.

**Exit conditions**

- [ ] All Section 7 behavior has executed evidence.
- [ ] CUDA, OpenCL, and Metal have native numerical evidence.
- [ ] Active partial upload evidence reports actual transferred bytes.
- [ ] Resource evidence waits for GPU completion.
- [ ] Service tasks do not receive active editor raster data.
- [ ] Persistent assets remain immutable across failure and reopen.
- [ ] No Mask UI, history payload, Version logic, or range algorithm enters NM3.
- [ ] Missing hardware or fixtures remain explicit qualification gaps.

## 7. Acceptance matrix

The names below specify assertion goals.
Use these names or equally precise behavior names.
Do not put a phase identifier in any test name, target, file, or fixture.

| Required behavior | Evidence and assertion goal |
| --- | --- |
| Grade ownership | `MultipleMasksBelongToOneColorGrade`: add three source kinds and find each by stable `MaskId`. |
| Duplicate identity | `DuplicateMaskIdLeavesGradeUnchanged`: compare the full Mask list before and after failure. |
| Value validation | `InvalidMaskValuesFailBeforeDocumentMutation`: cover non-finite values, bounds, opacity, and direction. |
| Round-trip | `MaskListRoundTripPreservesSourcesOrderAndRangeFields`: compare all supported fields and IDs. |
| Old shape rejection | `TopLevelMaskNodesAndEdgesAreRejected`: load the old node and edge shape and inspect the error. |
| Empty boundary | `EmptyMaskListUsesFullGradeCoverage`: compare pixels and Grade key with the unmasked reference. |
| Disabled boundary | `AllDisabledMasksUseZeroGradeCoverage`: final pixels equal the Grade input. |
| Union | `EnabledMasksUseMaximumCoverage`: compare each pixel with an independent maximum reference. |
| Source kinds | `BrushRadialAndLinearGradientShareUnionRules`: compare separate and combined source results. |
| Opacity and invert | `MaskOpacityAndInvertApplyBeforeUnion`: use values that expose an ordering error. |
| Display reorder | `MaskDisplayReorderKeepsStaticAndPixelKeys`: verify JSON order changes and runtime keys do not. |
| Local invalidation | `OneMaskEditReusesSiblingAndUpstreamResults`: inspect execute and reuse events. |
| Range boundary | `EnabledRangeFailsBeforeGpuWork`: verify direct field identity and explicit unsupported behavior. |
| Range input | `RangeInputUsesOwningGradeSceneInput`: inspect the compiled binding after a preceding Grade edit. |
| Asset deduplication | `EqualRasterContentReturnsOneAssetKey`: compare keys, files, descriptors, and pixels. |
| Asset identity | `DescriptorOrPixelChangeReturnsDifferentAssetKey`: vary one canonical field at a time. |
| Asset immutability | `ExistingAssetBytesCannotBeReplaced`: place mismatched bytes and preserve them after failure. |
| Atomic publish | `InterruptedRasterWritePublishesNoAsset`: inject failure before publish and reopen the store. |
| Concurrent publish | `ConcurrentEqualRasterWritesProduceOneVerifiedAsset`: run two writers to one root. |
| Persistent texture safety | `ActiveRasterUpdateNeverPatchesPersistentTexture`: inspect resource identity and old pixels. |
| Partial upload | `ActiveRasterRevisionUploadsOnlyDirtyRectangle`: assert host-to-device bytes and final coverage. |
| Coverage decrease | `DirtyUnionRegionCanDecreaseCoverage`: erase Brush pixels and compare the changed region. |
| Session fencing | `NewActiveRasterGenerationReplacesOldPreviewTexture`: reject or ignore the old generation result. |
| Cache publication | `MaskFailurePublishesNoSourceUnionOrGradeWrites`: retain prior valid keys after failure. |
| Resource lifetime | `ActiveMaskTexturesReleaseAfterGpuCompletion`: wait, replace the session, and inspect bytes. |
| Request isolation | `BackgroundRenderUsesSettledAssetsOnly`: run editor preview beside thumbnail or export. |
| CUDA native | `CudaMultiMaskUnionMatchesReference`: cover all boundary and source cases. |
| OpenCL native | `OpenClMultiMaskUnionMatchesReference`: run the same behavior matrix. |
| Metal native | `MetalMultiMaskUnionMatchesReference`: run the same behavior matrix. |

### 7.1 Numerical evidence

Use deterministic synthetic coverage for arithmetic and Union tests.
Calculate expected values outside the native implementation helper.

Use source patterns that distinguish maximum from sum.
Use opacity and invert values that distinguish operation order.
Include exact zero, exact one, and one-code-value R8 boundaries.

Declare the tolerance before a comparison.
For R8 coverage, use at most one code value unless a documented filter needs more.
Check finite values and dimensions before pixel error.

Run at least these native cases:

1. Empty Mask list.
2. Three disabled Masks.
3. One Radial Mask.
4. One Linear Gradient Mask.
5. One settled Brush asset.
6. Three enabled source kinds in one Union.
7. Two Grades with separate Mask lists.
8. One active Brush dirty update.
9. One active Brush update that decreases coverage.
10. One failed asset or native operation.

Cross-backend agreement does not prove correctness.
Compare each backend with the independent reference.

### 7.2 Resource and performance evidence

Measure one, four, and eight Masks on one Grade.
Use fixed image, geometry, source values, and device settings.

Record these values:

- Source execute and reuse counts.
- Union execute and reuse counts.
- Host-to-device bytes.
- R8 texture bytes.
- Signed-distance temporary bytes.
- Active texture bytes.
- Published result count.
- GPU completion point before release.
- Full render time and active-update time.

Compare a full Brush upload with a small dirty rectangle.
The partial case must transfer only its clipped rectangle bytes.
Record any full feather recomputation and its reason.

Do not reduce decode resolution or quality for better results.
Do not change the backend or algorithm.

### 7.3 Existing targets and commands

Use [edit test registration](../../../../../alcedo_studio/tests/edit/CMakeLists.txt) to resolve targets.
Use [app test registration](../../../../../alcedo_studio/tests/app/CMakeLists.txt) for request isolation.

| Existing target | Relevant coverage |
| --- | --- |
| `GpuDagMaskStoreTest` | Asset validation, persistence, cache behavior, and atomic publication. |
| `GpuDagModelGraphTest` | Grade-owned Mask model, source validation, and document rules. |
| `GpuDagRawInputTest` | Compiler, static-plan identity, content keys, and invalidation. |
| `GpuDagCudaMaskTest` | CUDA source evaluation, feather, Union, and dirty upload. |
| `GpuDagOpenClMaskTest` | OpenCL source evaluation, feather, Union, and dirty upload. |
| `GpuDagMetalGradeTest` | Metal Grade integration with Mask coverage. |
| `GpuDagCudaWorkspaceTest`, `GpuDagOpenClWorkspaceTest`, `GpuDagMetalWorkspaceTest` | Texture identity, completion, and release. |
| `PipelineMapperTest` | Current document save and load behavior. |
| `ThumbnailServiceTest`, `PipelineFrameSinkTest`, `ExportRecipeTest` | Settled-asset request isolation. |

Check the current target list after any rename.
Register each new test in a purpose-named target.

Run focused Windows checks from the repository root:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagMaskStoreTest GpuDagModelGraphTest GpuDagRawInputTest GpuDagCudaMaskTest GpuDagOpenClMaskTest GpuDagCudaWorkspaceTest GpuDagOpenClWorkspaceTest PipelineMapperTest
ctest --test-dir build/debug -R "GpuDagMaskStoreTest|GpuDagModelGraphTest|GpuDagRawInputTest|GpuDagCudaMaskTest|GpuDagOpenClMaskTest|GpuDagCudaWorkspaceTest|GpuDagOpenClWorkspaceTest|PipelineMapperTest" --output-on-failure
```

Run Metal checks on macOS:

```text
cmake --preset macos_debug_tests
cmake --build --preset macos_debug_tests --parallel 8 --target GpuDagMaskStoreTest GpuDagModelGraphTest GpuDagRawInputTest GpuDagMetalWorkspaceTest GpuDagMetalGradeTest GpuDagMetalRendererTest PipelineMapperTest
ctest --test-dir build/macos-debug-tests -R "GpuDagMaskStoreTest|GpuDagModelGraphTest|GpuDagRawInputTest|GpuDagMetal|PipelineMapperTest" --output-on-failure
```

Confirm the preset build directory before CTest.
Run affected thumbnail, analysis, export, and scheduler tests after focused tests pass.

Store temporary logs under `build/tmp/multi_mask_runtime/`.
Keep permanent numerical fixtures in established test resource directories.
Do not mark skipped native tests as passed.

## 8. Required call chains

### 8.1 Settled multi-Mask render

```text
PipelineApplyRequest with no active raster input
  -> shared Renderer and static-plan lookup
  -> compile or reuse Grade-owned Mask stack
  -> load or reuse each enabled persistent Brush asset
  -> evaluate analytic and Brush sources
  -> apply feather, invert, range identity, and opacity
  -> maximum Union
  -> Grade mix against its connected scene input
  -> downstream Grades and DRT/Post
  -> sink success
  -> publish completed results
```

### 8.2 Active Brush preview

```text
task-owned active raster input
  -> validate NodeId, MaskId, generation, revision, descriptor, and bytes
  -> acquire active preview texture
  -> upload the dirty rectangle
  -> update affected Brush feather region
  -> update affected Union region from all enabled Masks
  -> Interactive Grade result
  -> sink success
  -> keep saved MaskAssetKey unchanged
```

### 8.3 Settled raster publication

```text
complete R8 pixels + descriptor
  -> MaskStore::Put
  -> canonical content digest
  -> verify or atomically publish immutable file
  -> return new MaskAssetKey
  -> NM4 ReplaceMaskAsset change in a later phase
  -> Quality render without active override
```

### 8.4 Failure and cancellation

```text
invalid model, document, range, or active request
  -> fail before GPU work

missing or corrupt persistent asset
  -> fail before source publication

upload, kernel, submission, or sink failure
  -> cancel request
  -> discard unpublished source, Union, and Grade writes
  -> wait for required GPU completion
  -> release task-owned and active resources safely
  -> retain prior valid results and saved document
  -> report the actual error
```

## 9. NM3 completion criteria

- [ ] Every Color Grade owns an ordered list of stable `MaskId` values.
- [ ] Brush, Radial, and Linear Gradient use one typed source variant.
- [ ] Color Range and Luminance Range are direct Mask fields.
- [ ] The runtime rejects enabled ranges until their algorithms exist.
- [ ] Top-level Mask nodes and Mask edges are not part of the supported document.
- [ ] Empty lists, all-disabled lists, and enabled lists use the exact coverage rules.
- [ ] Multiple enabled Masks combine only with maximum Union.
- [ ] Display reorder changes neither pixels nor runtime keys.
- [ ] `MaskStore::Put()` derives immutable keys from descriptor and R8 pixels.
- [ ] Existing assets cannot be overwritten with different content.
- [ ] Active Brush pixels and dirty rectangles are task-owned request data.
- [ ] Active updates never patch persistent asset textures.
- [ ] Content keys and invalidation stay local to the Mask owner and descendants.
- [ ] CUDA, OpenCL, and Metal match the independent reference within tolerance.
- [ ] Failure publishes no partial source, Union, Grade, or asset result.
- [ ] Resource evidence waits for native GPU completion.
- [ ] Thumbnail, analysis, and export use settled assets only.
- [ ] No code artifact contains a phase identifier.
- [ ] NM4 retains typed history, Version, recovery, Paste, and asset reachability work.
- [ ] NM7 retains viewer input, temporary raster generation, and QSG overlay work.

Record implementation results under the corresponding sub-phase.
Include source revision, commands, test results, and main call chains.
Leave a criterion unchecked when evidence is unavailable.
