# Geometry, Crop, and Rotate Improvements

Date: 2026-11-14

Revised: 2026-11-18

Status: UI layout approved from the standalone QML preview. F1 is complete.
The later numerical design and new geometry fields remain proposals.

Parent: Standalone extension to the editor workspace roadmap.

Original audit: upstream `main`, `e86edddba6f1559a1444222cb09de679d15592a9`.
Implementation base and revision audit: upstream `main`, `d587167860c03aaf338fa208353fc1be6ac58fea`.
This base includes PR #943 at `f599ab8c9ed3a51d73babbd57c33cd2700cc6a5a`.
Source inspection and upstream test reports do not supply local execution evidence.

Implementation branch for F1: `feature/geometry-crop-transform-f1`.
Design branch: `docs/geometry-crop-transform-improvements`.
Tracking issues: [manual transforms, composition guides, and aspect controls #954](https://github.com/zidage/AlcedoStudio/issues/954)
and [whole-image orientation #459](https://github.com/zidage/AlcedoStudio/issues/459).

## 1. Requested result

Make the Geometry tab useful for composition, whole-image orientation, and manual perspective correction.
Preserve the existing crop and rotation workflow.

- Add Grid, Ratio, Spiral, Diagonal, and Triangle composition guides beside Thirds.
  Adobe calls the two mathematical composition guides **Golden Ratio** and **Golden Spiral**.
  These names describe mathematical composition geometry.
  Add None, visibility preferences, and orientation controls for asymmetric guides.
- Add Horizontal, Vertical, Aspect, Scale, X Offset, and Y Offset image transforms.
  Horizontal and Vertical change perspective. They are not image flips or crop coordinates.
  Add Constrain Crop to keep the output frame inside valid transformed coverage.
- Move `1.5:1 (native or VistaVision)` directly after `Custom` and before `2.35:1 (35mm)`.
- Add Lock aspect ratio and preserve the effective ratio after Swap Aspect and reopen.
- Include [issue #459](https://github.com/zidage/AlcedoStudio/issues/459): rotate the image and
  canvas together without losing content. Preserve the result on reopen and export.
- Add compact Crop actions for Rotate Left, Rotate Right, Flip Horizontal, Flip Vertical,
  and Swap Aspect. Give Swap Aspect the scoped `X` shortcut.
- Use Lightroom, Lightroom Classic, and LightCraft as behavior references.
- Reuse current owners for parameters, document changes, history, rendering, and export.

The user approved the preview's panel layout on 2026-11-16.
The approval covers the control groups, menus, shared control styling, and resizable scroll area.
It does not establish image-processing correctness or approve a persisted format.
The [standalone QML preview](https://github.com/ClearSights/AlcedoStudio/tree/6ce287c033eddffdd0e6b483066f47fc1f4bba41/ui_previews/geometry)
and its launcher stay on the design branch. Use the preview as a layout reference.
Its static Thirds overlay, sample values, state selector, and text action symbols are preview aids.
Do not add those aids to the product or treat them as implementation evidence.
Use the product behavior specified below when connecting the approved layout.
Automatic Upright, Guided Upright, and new lens distortion controls are comparison context.
They are outside this requested manual-transform and orientation change.
The existing lens owner keeps optical distortion correction.
Deliver whole-image rotation under #459. Review the flip actions in a separate follow-on PR.
Deliver the aspect controls, selectable guides, and manual transforms under #954.
Reuse PR #943 for crop pointer interaction, handles, cursors, and the automatic rotation grid.
Keep this plan's manual transforms, selectable composition guides, aspect order, and orientation actions.

## 2. Upstream research and related work

The research used the fetched upstream `main`, open issues, open pull requests, and discussions.
It also searched closed issues for earlier crop-overlay and perspective requests.
Search terms included crop, geometry, transform, perspective, overlay, Upright, and keystone.
Search results describe the state on the date above. They are not a permanent backlog inventory.

| Upstream item | Observed scope | Relationship to this proposal |
| --- | --- | --- |
| [QML workspace plan, Phase 5A](qml_editor_rhi_unified_workspace_plan.md) | Crop mask, guides, handles, input mapping, and retained Qt Quick geometry. Its completion record describes the thirds grid. | Extend the existing overlay renderer. Do not create a second viewer. |
| [GPU pipeline removal plan, Section 6.7](../edit/gpu_dag_final_removal_phase_plan.md#67-uncropped-source-frame-for-the-geometry-panel) | Geometry editing requests an uncropped, unrotated source frame. Normal renders apply document crop and rotation. | Retain this behavior for identity orientation and default transforms. Section 5.5 extends it with orientation and a transformed editing view. |
| [Automation plan, AU8a and AU8b](../automation/cli_mcp_automation_plan.md) | The parameter catalog owns crop ranges, options, conversions, and constraints. The QML panel consumes that catalog. | Extend the catalog and command path. Do not duplicate ranges or aspect lists in QML. |
| [Shortcut registry plan](configurable_keyboard_shortcut_registry_plan.md) | Named commands, input scopes, conflict checks, and saved bindings. Text fields retain text input. | Register composition-guide and Swap Aspect commands through this owner. |
| [Comparison plan](editor_adjustment_comparison_plan.md) | Image placement uses resolved render geometry. The current implementation rejects projective placement. | Include comparison placement in the projective mapping work. |
| [Shared slider PR #923](https://github.com/zidage/AlcedoStudio/pull/923) | `AdjustmentSlider` now wraps `AlcedoSlider`. Models expose hard limits and soft rail limits. `CollapsibleSection` adds its body insets. | Use the current wrappers and model properties. Do not copy the former slider or add section padding twice. |
| [GPU release PR #937](https://github.com/zidage/AlcedoStudio/pull/937) | Export and semantic generation can hold editor GPU resources. Rendering resumes after the last holder releases them. | Retain suspension routing for Geometry editing. Resource suspension is distinct from a transform validation or execution failure. |
| [Crop interaction PR #943](https://github.com/zidage/AlcedoStudio/pull/943), addressing [#942](https://github.com/zidage/AlcedoStudio/issues/942) | Merged corner rotation zones, edge bands, rotation-drag limits, L brackets, outside-image hover, and an automatic fine rotation grid. | Reuse the existing interaction and test cases under Section 2.1. |
| [Feature #459](https://github.com/zidage/AlcedoStudio/issues/459) | Rotate the entire image and canvas together, without cropping. | Review the whole-image rotation change separately from #954. |
| [Feature #954](https://github.com/zidage/AlcedoStudio/issues/954) | Manual transforms, selectable composition guides, and crop aspect changes. | Review each subfeature separately. See Section 7 for the merge order. |
| [Open issue #712](https://github.com/zidage/AlcedoStudio/issues/712) | Reports older viewer test registration gaps. The audited source now registers `EditorOverlayInteractionTest` and `OverlayCursorTest`. | Confirm current CMake registration and discovered case names. Do not infer unavailable targets or passing tests from the historical issue. |
| [Open issue #869](https://github.com/zidage/AlcedoStudio/issues/869) | Metal geometry resample and camera-color failures on main. | Record the existing issue when collecting Metal evidence. Do not assume every failure is new. |

The 2026-11-16 review recorded four document defects:
[aspect restore #946](https://github.com/zidage/AlcedoStudio/issues/946),
[phase prerequisites #947](https://github.com/zidage/AlcedoStudio/issues/947),
[mapping specification #948](https://github.com/zidage/AlcedoStudio/issues/948), and
[format migration #949](https://github.com/zidage/AlcedoStudio/issues/949).
This revision corrects the plan. It does not claim product fixes or close these reports.
The upstream synchronization also recorded the preview's shared-slider dependency in
[issue #950](https://github.com/zidage/AlcedoStudio/issues/950).

### 2.1 PR #943 dependency and scope boundary

PR #943 is part of the implementation base. Use its landed APIs and test cases.
Verify full-frame corner access with the current viewport margin during the affected phases.

PR #943 owns these behaviors. Do not implement parallel versions in this plan:

- Crop corner, edge, and rotation-zone hit testing and outside-image hover cursors.
- Press-to-edge offsets, rotation start thresholds, near-center holds, re-anchoring, and zero snapping.
- The per-drag straightening limit, L brackets, edge grips, and angle-badge placement.
- Rotation-active state, the automatic fine grid, its reference lines, and repaint on release.

Reuse `CropGeometry::HitTestWidgetGeometry`, `CropHitTestResult::rotate_hit`,
`CropInteractionController::RotationActive()`, and the existing `crop_rotation_active` projection.
Reuse `CropGeometry::CropAngleBadgeWidgetPoint`, QML `angleBadgeItemPos`, and the
`fine_grid_triangles` drawing output in the existing overlay item.
Use the PR's constants and shared helpers. Do not restore the removed top rotation handle or stem.
Its 45-degree limit applies to one pointer straightening drag. It does not limit the stored angle,
numeric angle entry, discrete quarter turns, or manual perspective fields.

This plan adds composition-guide paths, preference controls, and projective coordinate integration.
Section 5.3 defines their interaction with the existing automatic rotation aid.
Keep PR #943's focused interaction tests. Add only assertions for the new guide and transform inputs.
The PR reports interaction, cursor, and overlay test results; rerun the affected registered cases
after integration before claiming that this branch preserves them.

## 3. External behavior references

| Product | Verified behavior | Use in Alcedo |
| --- | --- | --- |
| [Lightroom desktop crop and geometry](https://helpx.adobe.com/lightroom/desktop/edit-photos/crop-rotate-geometry.html) | Composition-guide selection, aspect lock, crop orientation, straightening, manual perspective and framing, and crop constraints. | Keep Crop and Transform together under Geometry. Separate crop framing from image distortion. |
| [Lightroom Classic Transform](https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/guided-upright-perspective-correction.html) | Manual Vertical, Horizontal, Rotate, Aspect, Scale, and two offsets. Constrain Crop removes uncovered edges. Upright is a separate correction method. | Add the six requested manual fields. Keep the existing rotation value as the only straightening angle. |
| [Lightroom Classic shortcuts](https://helpx.adobe.com/lightroom-classic/desktop/introduction-to-lightroom-classic/keyboard-shortcuts.html) | `O` cycles crop guides. `Shift+O` changes guide orientation. `X` swaps crop orientation. | Propose scoped `O`, `Shift+O`, and `X` commands. Do not take keys from text editing or mask editing. |
| [LightCraft Crop actions](https://github.com/storytold/lightcraft/blob/f72ba11a2939653c866c07cf9189845ec31f1a50/crates/ui-egui/src/panels/right/crop.rs) | Crop contains Rotate Left, Rotate Right, Flip H, Flip V, and Swap Aspect (X). Buttons call named image or crop commands. | Add one compact action row in Crop & Rotate. Keep image orientation separate from crop-aspect swapping. |
| [LightCraft composition-guide source](https://github.com/storytold/lightcraft/blob/f72ba11a2939653c866c07cf9189845ec31f1a50/crates/ui-egui/src/panels/crop_overlay.rs) | Thirds, Grid, Ratio, Diagonal, Triangle, Spiral, and None. Paths use crop-relative coordinates. Tests cover shape, bounds, and orientation. | Use the same separation between guide construction and screen placement. Verify geometry directly. |
| [LightCraft manual-transform source](https://github.com/storytold/lightcraft/blob/f72ba11a2939653c866c07cf9189845ec31f1a50/crates/pipeline/src/transform.rs) | Manual controls compose into a projective matrix. Coordinates are centered and normalized to half the long edge. | Use one resolved image mapping. Avoid several independent image resampling operations. |

LightCraft was inspected at `f72ba11a2939653c866c07cf9189845ec31f1a50`.
Its source supports Diagonal, Triangle, and Spiral even though its README summary lists fewer guides.
These references establish behavior. They do not establish Adobe's private transform equations.
The numerical choices below are Alcedo proposals, not claims of Adobe pixel compatibility.
Do not copy LightCraft's backend substitution policy into Alcedo.

## 4. Current source audit

Paths in this section start at `alcedo_studio/`.

| Area | Current owner and file | Verified behavior and required extension |
| --- | --- | --- |
| Panel | `src/ui/alcedo_main/qml/EditorGeometryPanel.qml` | Reads crop ranges and aspect options from the catalog. Loads saved crop fields without submitting an edit. Add Crop actions and a distinct Transform section. |
| Aspect order | `src/ui/alcedo_main/editor_support/modules/geometry.cpp`, `CropAspectPresetOptions()` | Twelve options. The 1.5:1 option is eleventh. Move the option record without changing its identifier or ratio. |
| Aspect orientation | `src/app/editor_parameter_catalog.cpp`, `CropLockedAspectRatio()` and `ConstrainCrop()` | Fixed presets select landscape or portrait from source dimensions. Custom uses its stored width and height. Lock and Swap Aspect must retain the accepted effective ratio. |
| Guides | `src/ui/editor_rhi/editor_overlay_item.cpp`, `BuildOverlaySceneGeometry()` | The overlay generates Thirds and the automatic fine rotation grid. Add selectable composition paths through the same overlay owner. |
| Interaction | `src/ui/edit_viewer/crop_interaction_controller.cpp`, `src/ui/editor_rhi/editor_interaction_controller.cpp` | The merged code uses corner rotation zones and guarded drag behavior. Extend the coordinate mappings for orientation and perspective through these owners. |
| Parameters | `src/app/editor_parameter_catalog.cpp` | Owns the `crop_rotate` object, UI defaults, validation, conversion, and crop constraints. Add transform metadata and focused writes. |
| Numeric controls | `src/ui/alcedo_main/qml/AdjustmentSlider.qml`, `AlcedoSlider.qml`; `src/include/ui/alcedo_main/album_backend/editor_adjustment_models.hpp` | The wrapper uses `minimum`/`maximum` and `railMinimum`/`railMaximum`. Rail and value drags use the same owner drag sequence. Add transform metadata without replacing these controls. |
| Document | `src/include/edit/graph/image_geometry_model.hpp` | Owns crop rectangle, rotation, aspect preset, and aspect ratio. `ImageGeometryUpdate` updates selected fields. Extend this owner. |
| Saved format | `src/edit/graph/pipeline_document.cpp`, `ValidateDocumentShape()` and `PipelineDocument::FromJson()`; `src/edit/history/pipeline_document_checkpoint.cpp` | The reader requires the current document format. Root and checkpoint readers also check format fields and canonical JSON. Extend these owners together before writing new geometry. |
| Preferences and keys | `src/ui/alcedo_main/album_backend/editor_behavior_preferences.cpp`; `src/ui/alcedo_main/shortcut_registration.cpp`; `src/include/ui/alcedo_main/shortcut_registry.hpp` | `EditorBehaviorPreferences` uses `QSettings`. Add guide preferences there. Add a Geometry input scope and named commands to the existing shortcut registry. |
| Whole-image orientation | `src/include/edit/graph/image_geometry_model.hpp`; `src/edit/geometry/render_geometry_resolver.cpp` | The document has no discrete rotation or reflection field. The resolver has a `RotatedSourceBounds` output mode, but this is not a persisted orientation command. Add document-owned orientation without changing crop straightening. |
| Mutation | `src/app/editor_parameter_write_parse.cpp`; `src/app/editor_parameter_write.cpp` | `ParseGeometryUpdate()` creates a focused update. The write path calls `document.Geometry().ApplyUpdate()`. Preserve one validated mutation. |
| Geometry | `src/edit/geometry/render_geometry_resolver.cpp`; `src/include/edit/geometry/types.hpp` | Builds crop and rotation mappings. `TransformPoint()` omits homogeneous division. `InvertAffine()` assumes an affine matrix. Add explicit projective operations. |
| GPU sampling | `src/edit/runtime/cuda/geometry_resample.cu`; `src/edit/runtime/metal/shader/geometry_resample.metal`; `src/edit/runtime/opencl/shader/geometry_camera.cl` | Current geometry sampling needs a projective audit. CUDA passes six matrix coefficients. Metal declares six coefficients. Nine stored host coefficients alone do not provide perspective sampling. |
| Comparison | `src/ui/alcedo_main/album_backend/comparison_presentation_image.cpp` | Rejects a projective map. Extend placement and its QML consumer together. |
| Presentation scheduling | `src/ui/alcedo_main/album_backend/editor_session_render_scheduler_port.cpp`; `src/app/editor_session_render_controller.cpp` | The scheduler reroutes frame requests while editor GPU resources are released for export or semantic generation. Use the current `SetSuspendedForGpuRelease` path and holder lifecycle. |
| Tests | `tests/ui/editor_geometry_panel_qml_test.cpp`, `editor_geometry_math_test.cpp`, `editor_crop_interaction_test.cpp`; `tests/app/editor_geometry_overlay_pipeline_test.cpp`; `tests/edit/geometry/` | Existing coverage is for crop and rotation. New perspective assertions must drive production owners and render paths. |

Current registered targets include `EditorGeometryMathTest`, `EditorGeometryPanelQmlTest`,
`EditorCropInteractionTest`, `EditorGeometryOverlayPipelineTest`, and `GpuDagGeometryTest`.
`GpuDagGeometryTest` includes crop-frame, resolver, and document-geometry-use tests.
The current CMake list also registers `EditorOverlayInteractionTest` and `OverlayCursorTest`.
`EditorGeometryOverlayPipelineTest` currently checks CUDA resize behavior.
Its target name alone is not crop-overlay integration evidence. Add the named integration cases below.
This audit reads source. It does not claim that these tests passed during document creation.

The current base retains the existing import, document, and graph compiler owners for RAW import
defaults, profile tone-curve import, and Color Grade visibility.
Geometry edits and resets must preserve those fields and the active graph's visibility state.

## 5. Proposed product design

### 5.1 Panel structure and controls

Keep the Geometry tab and its current navigation position.
Use the existing adjustment section components and `AppTheme` values.
Follow [DESIGN.md](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md).
Use Qt Quick Controls Basic, `CollapsibleSection`, `AdjustmentSlider`, `AdjustmentCombo`,
`ThemeCheckBox`, and `IconActionButton`. Do not introduce a second control style.
`AdjustmentSlider` must retain the shared `AlcedoSlider` implementation.
Pass only the content column's height to `CollapsibleSection.bodyContentHeight`.
The section adds its own `spaceSm` body insets.
Both sections start expanded. Use the existing 320 px preferred panel width and 260–460 px limits.
Reset actions use the existing reset icon, with the distinct accessible names shown below.

```text
Geometry                                      Reset All
  Crop & Rotate                               Reset
    [Rotate Left] [Rotate Right] [Flip H] [Flip V] [Swap Aspect]
    Aspect [preset]
    Lock aspect ratio [checked when the preset is not Free]
    Custom width / height, when Custom is selected
    Existing crop position, size, and rotation controls
    Guide [Thirds]      Change guide orientation
    Show guide [Always / During input / Never]
  Transform                                   Reset
    Vertical             [slider and numeric value]
    Horizontal           [slider and numeric value]
    Aspect               [slider and numeric value]
    Scale                [slider and numeric value]
    X Offset             [slider and numeric value]
    Y Offset             [slider and numeric value]
    Constrain Crop       [checked]
```

Retain the existing angle control in Crop & Rotate as the only continuous straightening angle.
Store discrete whole-image orientation separately, as specified in Section 5.6.
Use a tooltip to distinguish crop aspect ratio from Transform Aspect.
The first selects the output frame shape. The second stretches image content.
Use a separate tooltip to distinguish crop X/Y from image X/Y Offset.

Transform sliders support numeric entry, keyboard adjustment, and reset to their default.
Use the shared rail drag, numeric value drag, Shift adjustment, and double-click reset behavior.
Use `beginDrag`, `updateDrag`, and `finishDrag` for both drag inputs.
Typed values use `editValue` and `commitImmediately`; rail arrow keys use the model's settled-edit path.
The transform ranges in Section 5.4 are the hard and soft limits alike.
Set `railMinimum = minimum` and `railMaximum = maximum` for these models.
Do not inherit Tone's extra ranges or add Geometry-specific soft limits.
The numeric readout follows the shared control's font, tabular figures, and signed display.
Release of a drag submits one settled history edit through the existing adjustment path.
Reset Transform restores the six manual fields and Constrain Crop to their defaults.
It preserves straightening and discrete orientation. It preserves framing unless the enabled
constraint must adjust it. Publish that adjustment in the same owner operation.
Reset Crop & Rotate requests a full frame, zero straightening, identity orientation, and Free aspect.
It preserves the manual fields and Constrain Crop. If those fields require a constrained frame,
resolve that frame in the same operation. Do not promise a full frame outside valid coverage.
Reset All restores all image-geometry defaults in one owner operation.
Each effective reset creates one history edit. A reset of unchanged values creates none.
Do not implement a group reset by submitting each field's reset separately.
All three resets preserve guide preferences and section expansion state.
Restoring a document, switching images, and opening a panel must not submit an edit.

Disable image-dependent controls until source dimensions and an editable document are available.
Keep the current editor action policy as the authority for edit availability.
Keep numeric values visible during a render. Show a real validation or render error near the section.
Use accessible names for every slider, selector, and icon action.
Keep the normal focus order from top to bottom. Scroll the panel in a narrow window.
Do not shrink labels until they become unreadable.

### 5.2 Aspect menu order

The proposed complete order is:

1. Free
2. Custom
3. 1.5:1 (native or VistaVision)
4. 2.35:1 (35mm)
5. 1:1
6. 16:9
7. 1.9:1 (IMAX)
8. 1.85:1 (DCI)
9. 2.2:1 (70mm)
10. 1.43:1 (70mm IMAX)
11. 4:3 (35mm)
12. 2.76:1 (Panavision Ultra)

Preserve `ratio_1_5_1_native_vistavision` and all other saved option identifiers.
Restore selection by identifier, never by the old list index.
The label retains the current wording. Its nominal shape is always 3:2.
Landscape uses 3:2 and portrait uses 2:3. It does not detect an arbitrary native source ratio.

Use the existing `aspect_ratio` width and height to store the effective ratio in pre-orientation axes.
The fixed preset identifier names the nominal shape. Its stored ratio selects that shape or its reciprocal.
The catalog must use this stored choice on subsequent edits and restore.
Do not derive it again from source dimensions after Swap Aspect.
When the user first selects a fixed preset, orient its shape to the displayed source dimensions.
For legacy documents, preserve the existing source-based orientation during format conversion.
Do not change the stored crop rectangle during that conversion.
The catalog projects the effective ratio into displayed axes after whole-image orientation.
Custom width and height show that displayed ratio and convert writes back to the stored axes.

The Lock control is a projection of the preset, not a second persisted Boolean.
Free is unlocked. Custom and fixed presets are locked.
Unlocking selects Free and keeps the current frame.
Locking Free selects Custom with the current frame's displayed pixel ratio and keeps that frame.
Selecting a fixed preset or entering Custom dimensions enables the lock.
Each action uses one validated crop-owner update and one settled history edit.

### 5.3 Composition guides

Offer None, Thirds, Grid, Ratio, Spiral, Diagonal, and Triangle. Default to Thirds.
Use the full mathematical labels from Section 1 in visible UI copy.

| Guide | Required geometry |
| --- | --- |
| Thirds | Two horizontal and two vertical lines at 1/3 and 2/3. |
| Grid | Seven internal horizontal and seven internal vertical lines, at multiples of 1/8. |
| Ratio | Lines at `1/phi^2` and `1/phi`, where `phi = (1 + sqrt(5)) / 2`. |
| Diagonal | Lines at 45 degrees in crop pixel space from each corner to the opposite boundary. A normalized X alone is insufficient on a nonsquare crop. |
| Triangle | A main diagonal and perpendiculars from the other two corners. Change orientation selects the other main diagonal. |
| Spiral | A complete quarter-arc composition guide derived from a phi-proportioned rectangle. Fit it to the crop. Keep the full curve on square, portrait, and landscape crops. Provide four mirrored orientations. |

Generate paths in crop-relative coordinates. Map them through the current crop placement.
Clip paths to the crop boundary. Preserve constant screen-space stroke width at each zoom and DPR.
Use existing overlay styling. Guides must not capture pointer input or change the image pixels.
Keep guide selection, orientation, and visibility in the existing application settings owner.
They are user preferences. Do not add them to image history, document JSON, or export settings.
Default visibility is Always. Always draws while Geometry editing is active.
During input draws while crop, straightening, or manual-transform input is active.
This includes pointer drags, active numeric editing, and keyboard edits until their settled commit.
Hover and menu opening do not count as image-editing input.
Never draws no composition guide. None also draws no composition guide in every visibility mode.
The crop boundary, handles, and PR #943's automatic rotation aid remain independent of this preference.
Store Triangle orientation as 0 or 1 and Spiral orientation as 0 through 3.
Preserve each asymmetric guide's orientation when the user selects another guide.
The orientation action is disabled for symmetric guides. Triangle has two distinct states.
`O` cycles the offered guide sequence, including None. It does not change the visibility preference.
`Shift+O` changes the current asymmetric guide orientation.
Register these actions with crop-editing scope. Mask mode and editable fields retain their own keys.
Missing settings use the documented defaults. Invalid saved values produce a visible preference error.
Offer Reset Guide beside that error. It restores Thirds, Always, and both orientation defaults.
Do not draw a substitute guide while an invalid preference awaits correction.

#### 5.3.1 Composition guides and the automatic rotation grid

The selectable Grid is a composition guide with seven internal lines on each axis.
PR #943's fine grid is an automatic straightening aid with approximately 28 px cells.
It is not another selector entry. Reuse its existing geometry generator and scene node.
Drive it from `CropInteractionController::RotationActive()` through `crop_rotation_active`.
Do not add a second rotation-active flag, drag controller, or QML grid renderer.
Numeric angle editing and manual-transform input do not activate that pointer-only rotation aid.

Composition-guide visibility applies only to the selected composition guide.
During an active rotation drag, keep the PR's fine grid and stronger thirds reference lines visible
even when the selected guide is None, visibility is Never, or a guide preference is invalid.
When a composition guide is also visible, draw it through the same crop placement and clipping.
If the selected guide is Thirds, draw the coincident thirds lines once with the inherited reference style.
When the rotation drag ends, remove only the automatic aid through the PR's existing repaint path.
The selected composition guide then follows its own visibility preference.
Guide selection, orientation, visibility, and Reset Guide must not change crop input or rotation-active state.

### 5.4 Manual transforms

Proposed catalog values use UI units below. Persist the corresponding named values in document geometry.

| Field | UI range | Default | Meaning |
| --- | --- | --- | --- |
| Vertical | -100 to +100 | 0 | Negative values widen the top. Positive values widen the bottom. |
| Horizontal | -100 to +100 | 0 | Negative values widen the left. Positive values widen the right. |
| Aspect | -100 to +100 | 0 | Positive values stretch height and compress width. Negative values reverse that change. |
| Scale | 50 to 150 percent | 100 | Uniform scale about the image center. Separate from viewport zoom. |
| X Offset | -100 to +100 | 0 | Positive values move image content right. |
| Y Offset | -100 to +100 | 0 | Positive values move image content up. |
| Constrain Crop | Boolean | true | Keep the output frame inside valid transformed image coverage. |

Use a 0.1 step and one decimal place for numeric fields.
UI limits do not authorize singular mappings. Validate the composed result before mutation.
Use the following equations in the resolver and in independent numerical test fixtures.
They define Alcedo's proposed mapping. They do not promise Adobe pixel compatibility.

#### 5.4.1 Coordinate and composition rules

Let `W` and `H` be the full reference dimensions after decode orientation.
Let `L = max(W, H)` and `q = L/2`.
Map source pixel-center coordinates to `p = ((x-W/2)/q, (y-H/2)/q)`.
Positive X points right. Positive Y points down.
Keep the current lens-correction order. These equations describe the geometry stage after it.
One offset unit means 0.25 percent of the source long edge.
For Aspect `a`, use `k = 2^(a/200)`, then scale X by `1/k` and Y by `k`.
Use uniform scale `s = scale/100` and translation `d = (0.005*offset_x, -0.005*offset_y)`.

For perspective, let `h = 0.5*horizontal/100`, `v = 0.5*vertical/100`, and `f = 1.5`.
Use a right-handed 3D rotation `Rx(-v) * Ry(h)` on the plane `(p.x, p.y, 0)`.
Project with denominator `f + z`. The equivalent normalized 2D map is:

```text
n = 1 / sqrt(cos(h) * cos(v))
P = [ n*cos(h)             0          0 ]
    [ -n*sin(v)*sin(h)     n*cos(v)   0 ]
    [ -cos(v)*sin(h)/f     -sin(v)/f  1 ]
```

Divide the first two coordinates by the third. The source center maps to zero.
The factor `n` makes the local area scale one at that center.
Positive Vertical widens the bottom. Positive Horizontal widens the right.
Use column vectors. The forward image map before crop is `T(d) * S(s) * A(k) * R(c) * P`.
Here `A(k) = diag(1/k, k, 1)` and `R(c)` applies existing straightening around the crop center `c`.
Express `c` in the same normalized reference plane. Keep that pivot for nondefault perspective too.
This removes an implicit pivot choice and reproduces the existing pivot when `P` is identity.
Crop framing retains the existing normalized rectangle in the reference-sized output plane.
Convert back to reference pixels, apply crop placement, and then apply discrete orientation.
Use the existing extent-rounding rule once per final output dimension.

#### 5.4.2 Displayed controls and stored values

Store manual fields in the pre-orientation reference axes.
Let `O` be the signed 2D axis permutation of the current discrete orientation.
Project `(horizontal, vertical)` through `O` for the displayed perspective controls.
Project `(offset_x, -offset_y)` through `O`, then reverse the displayed Y sign for Y Offset.
Negate Aspect when `O` exchanges the axes. Multiply the displayed straightening angle by `det(O)`.
Scale is unchanged. Convert UI writes through the inverse of these rules.
The table's directional meanings apply to the displayed image.
Build the perspective matrix from stored values in the specified order, not from displayed values.
Do not modify stored transform values or their composition order when orientation changes.
An old document with default transform values must produce its existing pixels and dimensions.
One resolved mapping drives rendering, mask sampling, input mapping, ROI, and export.
Do not add another lens-correction pass.

#### 5.4.3 Crop constraints

With Constrain Crop enabled, intersect valid transformed coverage with the requested frame.
Keep its center when possible. Shrink the frame uniformly to preserve the locked aspect ratio.
In Free mode, preserve the requested frame's current pixel ratio during this automatic shrink.
Translate it only when the requested center cannot produce a valid positive frame.
Choose the nearest feasible center to the requested center in reference pixels.
Choose the largest valid frame at that center. Break equal-distance ties by smaller Y, then smaller X.
Constrain coverage after perspective, straightening, aspect, scale, and offsets, before orientation.
Persist any resulting crop change with the transform change in the same owner operation.
With Constrain Crop disabled, retain the requested frame and show uncovered regions explicitly.
Use the current renderer's opaque black border policy in preview and export.
Do not synthesize missing image content.

### 5.5 Editing view and failure behavior

For default transform values, retain the existing uncropped source editing view without straightening.
Apply the discrete orientation to that editing view. Use its oriented dimensions for viewport fitting.
For nondefault transforms, use a transformed editing view that omits final crop and straightening.
It applies perspective, aspect, scale, and offsets. The overlay represents crop and straightening.
Apply the discrete orientation to the image and overlay together.
Let `E` map source coordinates to the editing image and `F` map them to the final output frame.
Map the final crop boundary and guide paths into the editing image with `E * inverse(F)`.
Use the viewport placement after this map. Map pointer positions back through the corresponding inverse.
Do not assume that an angle and an axis-aligned rectangle can represent the resulting overlay.
Use the same paths for drawing and crop hit testing. Guides themselves do not receive input.
Pass the mapped crop quad to PR #943's hit testing, brackets, grips, and rotation-grid drawing.
Retain its corner-before-edge-before-rotation priority, outside-image cursors, and angle-badge helper.
Extend coordinate conversion for the new transform; do not replace the inherited pointer state machine.
Fit the editing view to the transformed source bounds, with finite extents from the resolver.
This explicitly extends the earlier uncropped-source rule to support the new image operations.
Closing Geometry returns to the normal complete document render without changing the document.

Reject NaN, infinity, out-of-range values, singular matrices, and a projective horizon crossing the
sampled frame. Compute validation and matrix inversion in double precision.
For homogeneous point `u`, reject `abs(w) <= 1e-8 * norm(row3) * norm(u)`, using Euclidean vector norms.
Reject a denominator sign change across the sampled polygon, including inverse mapping of crop corners.
For inversion, reject `abs(det(M)) <= 1e-12 * norm(M)^3`, using the Frobenius matrix norm.
Report the failing condition. Do not clamp a singular matrix into an accepted transform.
Reject an impossible constrained crop before changing the document or history.
Report backend execution failures through the existing render error surface.
Do not present an old image as the successful result of a failed transform.
Do not change backend, decode resolution, or image quality after failure.
An existing `PresentationSuspended` result during export or semantic GPU release is not a failure.
Retain its existing pending-render and resume behavior through the render controller.
After the last GPU holder releases its resources, reroute the current Geometry request on the same backend.
Do not report a suspended frame as a completed transform or replace it with another render path.

### 5.6 Crop shortcuts and whole-image orientation

In the complete layout, use five `IconActionButton` actions below the Crop & Rotate heading.
Give each action an accessible name and a tooltip with its operation.
Use the existing compact hit, optical, and spacing tokens. Keep the order shown below.
At narrow widths, wrap the row into two rows in the same order.
Wrap only when the five compact hit targets do not fit. The standard panel can keep one row.
Keep all five actions available. Do not require the user to open a menu for these actions.
The preview's text symbols are not final icons. Obtain explicit asset approval under `DESIGN.md`
before adding or newly assigning the five SVGs. Reuse the existing reset icon for reset actions.

| Action | Required result |
| --- | --- |
| Rotate Left | Turn the current image and its canvas 90 degrees counterclockwise. Swap width and height. |
| Rotate Right | Turn the current image and its canvas 90 degrees clockwise. Swap width and height. |
| Flip Horizontal | Mirror the image left to right in its current displayed orientation. Keep canvas dimensions. |
| Flip Vertical | Mirror the image top to bottom in its current displayed orientation. Keep canvas dimensions. |
| Swap Aspect | Exchange the crop frame's displayed width and height ratio. Keep image content orientation unchanged. Use scoped `X`. |

The rotate actions satisfy issue #459 without a crop drag or an extra confirmation.
With default manual transforms and no crop, each quarter turn preserves every source corner and edge.
The action must not shrink the crop, zoom to fill the former canvas, or discard image content.
Starting from an existing crop, rotate or reflect the current framed result and its canvas together.
With manual transforms, preserve that accepted result, including any explicit uncovered border.
Keep the same selected source content. Do not clear the crop or change the straightening angle.
Compose actions in displayed coordinates. Rotate Left and Rotate Right are exact inverses.
Two equal flips and four equal quarter turns return the original orientation exactly.
Two Rotate Right clicks provide the 180-degree operation requested by issue #459.
Flips act on the current view axes after any quarter turn.

Persist one eight-state orientation value in `ImageGeometryModel`.
Use the values `identity`, `rotate_90_cw`, `rotate_180`, `rotate_90_ccw`,
`flip_horizontal`, `flip_vertical`, `transpose`, and `transverse`.
This value represents rotations and reflections of the rendered frame after decode orientation.
Keep camera orientation metadata unchanged. Do not encode these commands as accumulated float angles.
Compose each action with the current orientation and store the resulting exact state.
Resolve its mapping from pixel centers and extents. A quarter turn is a pixel permutation.
Combine that mapping with the existing geometry sample; do not allocate another full-image copy.
Do not change pre-orientation crop coordinates or transform values to execute an orientation action.
Project crop dimensions and effective aspect ratio into displayed coordinates through the catalog.
Keep continuous straightening in its stored axes; only its UI projection changes under reflection.

Swap Aspect changes framing only. It does not rotate or flip the image.
Preserve the selected preset identifier and exchange its effective displayed ratio.
Write the reciprocal back through the catalog to the existing stored `aspect_ratio` fields.
For Custom, exchange the effective width and height. Preserve the frame center when possible.
In Free mode, exchange the current displayed frame dimensions and remain in Free mode.
Apply the existing crop coverage constraint to the new frame through its owner.
A square frame produces no edit. An impossible frame produces a validation error before mutation.
The button and `X` call the same named command. Text fields and other editing tools retain `X`.

Each effective action creates one settled history edit. Undo and Redo restore it exactly.
Disable the actions when no editable image or source dimensions are available.
Do not require active crop handles to execute an image orientation command.
Save, reopen, version switching, comparison, masks, preview, and export must use the same orientation.
A failed owner operation changes neither geometry nor history. A render failure reports its real error.

## 6. Ownership, data flow, and implementation scope

`ImageGeometryModel` remains the persistent image-geometry owner.
Extend `ImageGeometryUpdate` with optional transform fields, discrete orientation, and the crop constraint flag.
Use proposed JSON keys `vertical`, `horizontal`, `aspect`, `scale`, `offset_x`, `offset_y`,
`constrain_crop`, and `orientation` inside existing document geometry.
Store manual scalar fields in the table's UI units, including Scale with default 100.
Convert to normalized geometry units only in the resolver.
Reuse `aspect_ratio` for effective crop orientation as defined in Section 5.2.
Do not add a separate crop-lock Boolean or a second aspect-orientation field.
Missing new fields load their defaults. Validate present values and reject invalid types.
Do not silently discard new fields through an older application version.

### 6.1 Saved-format ownership

Use `PipelineDocument`'s existing `format_version` and `kPipelineDocumentFormatVersion`.
The constant is in `src/include/edit/history/pipeline_history_format.hpp` and is 7 at this audit.
F1 prepares the shared readers and keeps current output versions and encodings.
O2 advances the document and project metadata versions for exact orientation.
O3 uses O2's eight-state representation for flips.
A2 advances them for effective fixed-aspect semantics.
T5 advances them for manual transforms and Constrain Crop.
Each release must reject a later revision that it cannot interpret.
Use the existing project and document versions to distinguish these encodings.
Advance the existing project metadata version with each new encoding through `pipeline_history_format.hpp`.
Update its supported-version bounds and the project open/save checks together.
A prior application must reject the project before replaying an unsupported geometry batch.
Do not depend only on a document checkpoint to detect an incompatible journal edit.
Keep the packed-project header policy unchanged unless its representation changes.

Extend `ValidateDocumentShape`, `PipelineDocument::FromJson`, and the geometry reader together.
Read supported earlier revisions through explicit version-aware conversion.
This applies to the project versions currently supported at the audit and the new revisions above.
It does not add support for older project versions that the current product already rejects.
Keep their identity-transform rendering and their existing crop pixels unchanged.
Legacy fixed-aspect conversion needs source dimensions and the existing source-orientation rule.
Perform that conversion through the application owner when dimensions are available.
Missing dimensions reject a new orientation or aspect command before mutation.

Include `pipeline_document_checkpoint.cpp`, root-state encoding and decoding, Version state,
history replay, and `app/document_transfer.cpp` in the compatibility audit and implementation.
Validate canonical JSON against its stored revision before conversion.
Never compare an old saved root with a latest-format reserialization as its integrity check.
Retain the original stored root bytes and commit hashes. Do not rewrite historical objects on open.
New roots, checkpoints, and transfer packages must declare their nested document revision accurately.
Apply any live-document conversion with the accepted owner operation, before publication.
Opening an old document must not create an image-history edit.

Verify old-document load, history replay, save/reopen, and transfer at each format step.
Verify rejection with the actual prior-revision reader against a new-format fixture.
Record that reader's source revision. A new-reader rejection test alone does not prove old-reader behavior.

### 6.2 Commands and publication

The catalog exposes a proposed `geometry_transform` field beside the compatible `crop_rotate` field.
Both write to the same document owner. The new field returns only the requested change description.
Expose proposed `geometry.rotate_left`, `geometry.rotate_right`, `geometry.flip_horizontal`,
`geometry.flip_vertical`, and `geometry.swap_aspect` commands through the existing command service.
Use focused owner operations such as `RotateImageQuarterTurn(direction)`, `FlipImage(axis)`,
and `SwapCropAspect()`; their input is the operation, not a copied geometry object.
Each operation returns the existing command success or error result after validation.
The image-geometry owner composes orientation. The crop owner validates Swap Aspect framing.
Here the crop owner means the same `ImageGeometryModel` update path and its geometry validation helper.
It does not mean a new service or a second mutable crop model.
The operations run on the existing editor command executor and publish after the complete change.
Do not copy a document or a complete geometry object to update one value.
QML models remain a read projection. Settings own guide preferences. Resolved geometry owns matrices.
No new state mirror, image buffer copy, executor, or speculative concurrency mechanism is required.
For O1 and T1–T4, extend the existing scalar `ImageGeometryParams` render input for internal tests.
This input is an existing render description. It must not become a second persistent geometry model.
O2 and T5 project their accepted document fields into that input through its current owner.

```text
QML control or automation command
  -> EditorParameterCatalog validation and conversion
  -> ParseEditorParameterWrite / ParseGeometryUpdate
  -> existing editor command and history owner
  -> ImageGeometryModel::ApplyUpdate (one complete validated change)
  -> graph compilation / ResolveRenderGeometry
  -> selected backend sampling -> frame publication
  -> panel projection, mask placement, comparison, and export use the resolved mapping

Invalid input or constraint failure
  -> validation error -> no document mutation -> no new history entry

GPU execution failure after an accepted edit
  -> real render error -> edit remains available for correction or Undo
  -> no successful frame publication -> no backend substitution
```

Existing files in Section 4 change in their owning phase.
Also audit `resolved_render_geometry.hpp`, graph geometry keys, mask sampling, viewport coverage,
`comparison_presentation_image.cpp`, `EditorComparisonCanvas.qml`, and export geometry requests.
Projective mappings invalidate affine-only scale estimates. Evaluate local scale with the projective
Jacobian and derive conservative sampling bounds over the requested region.
Do not pass a projective matrix into an affine-only helper.
Affine view fitting can retain its existing math where the last matrix row remains `[0, 0, 1]`.
New proposed files are `edit/geometry/projective_transform.{hpp,cpp}`,
`ui/edit_viewer/crop_guide_paths.{hpp,cpp}`, and focused tests for these operations.
The guide helper generates only selectable composition paths. PR #943 retains automatic grid generation.
Place headers under the matching `src/include/` directories.
Keep new modules small. Include defining headers under the repository's header rules.

## 7. Consecutive implementation PRs

Reread `AGENTS.md`, `CONTRIBUTING.md`, and the applicable skills before each PR.
Use `alcedo-qml-ui`, `alcedo-msvc-cmake`, and `opencl-program-registry` where their scopes apply.
Review and merge one row per PR. Base each PR on the preceding merged work in the order shown.
The dependency column records the work each PR needs. The review order also keeps shared files
and format revisions easy to review. See each PR's completion record for its status.

| PR | Result | Main owners | Required prior PRs | Expected changed lines | Review scope |
| --- | --- | --- | --- | ---: | --- |
| F1 | Shared geometry compatibility readers | Document, project, history, transfer | None | 750–1400 | Prepare existing owners for version-specific reads. |
| F2 | Shared crop-editing shortcut scope | Shortcut registry, editor focus routing | None | 200–550 | Set the common key priority for Swap Aspect and guides. |
| O1 | Exact orientation mapping | Resolver, rendering and coordinate consumers | None | 900–1650 | Prove the mapping before adding storage and controls. |
| O2 | Rotate Left and Rotate Right | Geometry document, commands, Crop panel | F1, O1 | 1050–1750 | Deliver lossless whole-image quarter turns under #459. |
| O3 | Flip Horizontal and Flip Vertical | Geometry commands, Crop panel | O2 | 350–850 | Add flips through the same exact orientation path. |
| A1 | Crop preset menu order | Aspect option table, catalog, Crop panel test | None | 80–300 | Review the menu order and stable identifiers. |
| A2 | Lock aspect ratio and Swap Aspect | Crop owner, catalog, commands, history, Crop panel | F1, F2 | 1000–1750 | Deliver crop framing and its saved effective ratio. |
| G1 | Selectable composition guides | Settings, shortcut registry, overlay, Crop panel | F2 | 1150–1750 | Deliver the guides without image or render-format changes. |
| T1 | Projective equations and frame resolution | Geometry math and resolver | None | 950–1700 | Prove the numerical behavior. |
| T2 | Projective sampling on each backend | CUDA, Metal, OpenCL geometry passes | T1 | 1000–1700 | Verify one coordinate convention on all backends. |
| T3 | Projective masks, comparison, ROI, and export | Geometry consumers | T1, T2 | 950–1650 | Verify final-frame consumers. |
| T4 | Transformed editing view and crop mapping | Viewer request, overlay, crop input | T1, T2 | 850–1500 | Verify editing-view consumers and inherited crop input. |
| T5 | Durable transform writes | Document, catalog, command service, history | F1, T1–T4 | 1100–1800 | Publish a complete owner write after all render consumers work. |
| T6 | Transform controls and resets | Geometry panel, adjustment models, command service | T5 | 900–1650 | Complete the manual-transform workflow. |

O1–O3, A1–A2, G1, and T1–T6 are separate review tracks. Aspect and guide controls need no
orientation commands. The guides use the merged crop overlay. The transform math and render
consumers accept identity orientation and the current crop behavior when those are the only inputs.
When the displayed-axis mapping from O1 is available, reuse it. Test the combined result whenever
a PR is the first to have both features. T4 owns guide placement with the transformed editing view
when G1 is present. T6 owns visibility during transform input and the complete reset scopes.
If G1 lands after T6, connect the transformed placement and input visibility in G1.

O2, A2, and T5 publish successive format revisions in the order shown. Take the revision values
and supported older revisions from the merged base of each writer PR. If the order changes,
update Section 6.1 first and repeat the actual prior-reader checks. A1 and G1 require no new
document or project version. O2, O3, and A2 introduce UI actions only when their own commands work.
Add each action to the common row in Section 5.6. Show the Transform section first in T6.
Use the tracker links for partial work. Use a closing reference for #954 only after all its
acceptance criteria have evidence. Track the #459 result separately.

Each estimate includes production code, tests, build changes, resources, and documentation.
Reestimate against the merged base before coding. Split work by owner if the estimated diff
can exceed 2000 lines. Keep each atomic update, its failure behavior, and its tests together.
For each PR, use the build and test commands in Section 8 with the targets below. Record the
actual commands, results, and any unavailable evidence in that PR's Section 9 record.

### F1. Shared geometry compatibility readers

**Deliverable and inputs**

Prepare the readers and compatibility tests on the audited upstream base. Keep the bytes and
format versions written by the current product.

**Owners and steps**

Use `pipeline_history_format.hpp`, `PipelineDocument::FromJson`, `ValidateDocumentShape`,
`pipeline_document_checkpoint.cpp`, the project open/save checks, Mini-Git materialization,
and `app/document_transfer.cpp`. Make the document reader validate the version and select its
reader before geometry decoding. Check that the nested document version agrees with its root,
checkpoint, or transfer version field where that field exists. Validate stored canonical bytes
before any live conversion. Put each compatibility check with its existing owner. Keep the
accepted revision set limited to the formats already supported.

**Success and failure**

Project or transfer open → stored version and bytes checked → current document published.
An unsupported project version, invalid immutable root, or invalid transfer stops that load.
A checkpoint decoder rejects a revision mismatch or noncanonical document before publication.
The existing history loader reconstructs an unusable checkpoint from the verified root and commits.

**Verification and exit**

Add `AcceptedProjectKeepsCanonicalRootBytesAndCommitHash`,
`CheckpointRejectsMismatchedNestedDocumentVersion`, and
`TransferRejectsUnsupportedDocumentWithoutApplyingChanges`.
Build `ProjectCompatibilityTest`, `PipelineDocumentCheckpointTest`,
`EditorMiniGitMaterializerTest`, and `DocumentTransferTest`. Compare current written bytes against
the same fixture on the base revision. Verify that replay, reopen, and transfer retain their current behavior.

#### F1 completion record (2026-11-18)

**Status** Implemented on `feature/geometry-crop-transform-f1` from `a993eaa5`.

**Implementation** `PipelineDocument` keeps the stored revision and uses it to select the geometry
reader. Roots and checkpoints use that revision in their metadata. Root hashing uses the same
revision. The transfer reader validates the source revision before reading the selected fields.
Transfer packages carry selected fields and a source revision. They contain no nested document.
Project open and save already use the shared project metadata limits. F1 keeps those values and
the current document encoding. The application moves the validated root into the history owner.

**Primary success chain**

```text
Project open -> project metadata check -> immutable root decoder
  -> PipelineDocument::FromJson -> stored revision selects the crop and rotation reader
  -> canonical document comparison -> root hash check -> history load
  -> matching checkpoint or replay of verified commits -> committed document

Transfer import -> schema and source revision checks -> selected fields -> validated package
```

**Primary failure chain**

```text
Invalid root revision, mismatched revision, or noncanonical document -> root rejected
  -> history load error before publication
Invalid checkpoint -> checkpoint rejected -> existing history reconstruction from root and commits
Unsupported transfer revision -> import or paste validation error -> no Version or commit added
```

**Verification**

| Required result | Observed result |
| --- | --- |
| Canonical root bytes and commit hash after project reopen | `AcceptedProjectKeepsCanonicalRootBytesAndCommitHash` passed. The packed project kept its stored root bytes, root ID, commit hashes, and committed document after save and reopen. |
| Revision checks before using a nested document | `CheckpointRejectsMismatchedNestedDocumentVersion`, `DocumentRejectsFutureAndOutOfRangeVersions`, and `RootRejectsOutOfRangeDeclaredDocumentVersion` passed. |
| Canonical bytes before history uses a document | `RootAndCheckpointRejectNoncanonicalStoredGeometry` passed. The document, root, and checkpoint writers matched the committed current-format byte fixtures. |
| Transfer rejection and unchanged target | `TransferRejectsUnsupportedDocumentWithoutApplyingChanges` and `ImportRejectsFutureAndOutOfRangeDocumentVersion` passed. The target document and its Version and commit state kept their prior values. |
| Replay of saved history | All 17 `EditorMiniGitMaterializerTest` cases passed. All six `ProjectCompatibilityTest` cases passed, including the render comparison. |

Windows Release with MSVC 14.44 and CUDA 12.8. Build and discovery passed.

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_release --target ProjectCompatibilityTest PipelineDocumentCheckpointTest EditorMiniGitMaterializerTest DocumentTransferTest --parallel 4
$pattern = '^(ProjectCompatibilityTest|PipelineDocumentCheckpointTest|EditorMiniGitMaterializerTest|DocumentTransferTest)\.'
ctest --test-dir build/release -N -R $pattern
ctest --test-dir build/release -R $pattern --output-on-failure --no-tests=error -j 1
```

The targeted run recorded 85 passes, four failures, and zero skips out of 89. A rebuild from the
exact base sources ran the two affected suites and recorded 56 passes and the same four failures
out of 60. All 60 shared results matched. The failing assertions reported identical
serialized values and hashes in both builds. The three historical fixture failures are
tracked in [#553](https://github.com/zidage/AlcedoStudio/issues/553). The transfer key assertion
is tracked in [#484](https://github.com/zidage/AlcedoStudio/issues/484) and
[#554](https://github.com/zidage/AlcedoStudio/issues/554).

The commit review moved root ID verification into the decoder. It reuses the bytes from the
canonical document check and validates the graph once. The decoder checks the stored element ID
before converting it to 32 bits. The review added cases for a hash mismatch and an element ID
that would truncate to the saved owner. The targeted results above precede these review edits.
The commit request did not run a build or tests.

The production files each contain fewer than 700 lines. The existing transfer test file has
1,173 lines after adding two version tests. A future split can put package serialization and
validation tests in a separate source file from paste planning and application tests.

### F2. Shared crop-editing shortcut scope

**Deliverable and inputs**

Add a crop-editing scope to the current shortcut registry. Prepare it for A2 and G1.
Keep all existing bindings. Add the feature commands with their own PRs.

**Owners and steps**

Use `shortcut_registry.hpp`, `shortcut_registry.cpp`, and the editor input routing in
`EditorWorkspace.qml`. Read the current crop mode and focus state through their owners.
Make crop commands eligible only during crop editing. Give shortcut capture, text editing,
and active mask editing priority over crop commands. Keep conflict checks and saved
overrides in the registry. Use its existing scope compatibility and activation path.
Define the priority once for both feature PRs.

**Success and failure**

Editor focus and active tool → registry scope selection → eligible command dispatch.
An inactive crop scope leaves the key to the existing input owner. Binding conflicts use
the existing registry error.

**Verification and exit**

Build `ShortcutRegistryTest` and `ShortcutSettingsQmlTest`. Add
`CropShortcutScopeYieldsToTextInputAndMaskEditing` and
`CropShortcutScopeLeavesExistingBindingsUnchanged`. Use test bindings to drive the registry
with and without an eligible crop owner. Verify the existing binding table and stored overrides.
Completion record pending. Use Section 9.

### O1. Exact orientation mapping

**Deliverable and inputs**

Implement all eight pixel-center mappings in Section 5.6. The existing document still supplies
identity orientation to production render requests.

**Owners and steps**

Extend the existing `ImageGeometryParams`, `ResolveRenderGeometry`, and resolved mapping types.
Compose the orientation after crop and straightening. Calculate the final extents and inverse at
the resolver. Reuse the existing affine sampling path. Feed the same mapping to mask, comparison,
export, and viewport placement. Fit the uncropped editing image in oriented dimensions.
Use mapped corners with the inherited crop hit test, brackets, and angle-badge helper.

**Success and failure**

Internal orientation input → resolver → oriented extents and inverse → existing render and
coordinate consumers. Invalid extents or mapping return the existing geometry error without
publishing a frame.

**Verification and exit**

Add `QuarterTurnSwapsCanvasAndPreservesEverySourcePixel`,
`AllEightOrientationsMapLabeledCornersExactly`, and
`OrientationPreviewMasksComparisonAndExportShareMapping`. Use an odd-sized asymmetric image.
Check all eight orientations and exact pure-orientation pixels. Verify identity parity with the
current renderer. Build `GpuDagGeometryTest`, `EditorComparisonGeometryTest`,
`EditorCropInteractionTest`, `ExportServiceTest`, and the applicable backend targets in T2.
Completion record pending. Use Section 9.

### O2. Whole-image rotation

**Deliverable and inputs**

Deliver Rotate Left and Rotate Right under #459 with F1's readers and O1's mapping. Two
right turns provide 180-degree rotation. The saved eight-state representation uses Section 5.6.

**Owners and steps**

Add `orientation` to `ImageGeometryModel` and its focused update. Advance the formats under
Section 6.1. Connect `RotateImageQuarterTurn(direction)` to the two named commands and the
existing command executor. Include orientation in the geometry content key and the current
document-to-render projection. Preserve the underlying crop rectangle and straightening value.
Project existing crop dimensions, aspect controls, and the angle through the current orientation.
Add the two accessible Crop buttons and scoped reset behavior. Confirm approval for their SVGs
under `DESIGN.md` before assignment.

**Success and failure**

Crop action → command → focused owner update → one history edit → oriented render and export.
Missing dimensions or an invalid update returns a command error before mutation. An accepted
edit with a subsequent backend failure reports the render error and stays available for Undo.

**Verification and exit**

Add `FourQuarterTurnsRestoreExactOrientation`,
`OrientationActionsPreserveExistingCropContentAndStraightening`, and
`QuarterTurnsRestoreOnReopenAndExport`. Verify exact pixels for uncropped quarter turns,
including both directions and 180 degrees. Verify Undo, Redo, version switching, comparison,
export, and rejection by the actual prior reader. Build the F1 and O1 targets plus
`EditorParameterCatalogTest`, `EditorGeometryPanelQmlTest`, `EditorOverlayInteractionTest`,
and `OverlayCursorTest`. Check both buttons on a full-frame crop without activating the handles.
Completion record pending. Use Section 9.

### O3. Whole-image flips

**Deliverable and inputs**

Add Flip Horizontal and Flip Vertical through O2's eight-state orientation write path.

**Owners and steps**

Add `FlipImage(axis)` to the orientation command owner. Compose the reflection in displayed axes.
Retain the existing mapping through save, restore, history, and export. Append the two accessible
buttons to the Crop action row. Confirm SVG approval under `DESIGN.md` before assignment.

**Success and failure**

Flip action → exact orientation composition → one owner update and history edit → the same
resolved frame with reflected placement. An unavailable image or a rejected command keeps
the current content and history. Use O2's render error path.

**Verification and exit**

Add `FlipAfterQuarterTurnUsesDisplayedAxes`,
`TwoMatchingFlipsRestoreExactOrientation`, and
`FlipPreservesCanvasAndRestoresAfterReopen`. Check both flips across all eight states,
with an existing crop and through export. Build `GpuDagGeometryTest`,
`EditorParameterCatalogTest`, `EditorGeometryPanelQmlTest`, and the O2 history and export targets
that exercise flip writes. Confirm both accessible names and manual button order.
Completion record pending. Use Section 9.

### A1. Crop preset menu order

**Deliverable and inputs**

Move the VistaVision option to the position in Section 5.2 using the current catalog.

**Owners and steps**

Move the existing record in `CropAspectPresetOptions()`. Keep its identifier, translation key,
and nominal dimensions. Confirm that `EditorGeometryPanel.qml` selects and restores the record
by identifier through the catalog.

**Success and failure**

Option table → catalog → menu and restored selection by identifier. An invalid identifier
continues through the existing validation path.

**Verification and exit**

Add `AspectMenuPlacesVistaVisionBetweenCustomAnd235` and
`SavedPresetSelectionSurvivesMenuReorder`. Build `EditorGeometryMathTest`,
`EditorParameterCatalogTest`, and `EditorGeometryPanelQmlTest`. Check the full menu order and
one restore without a submitted edit.
Completion record pending. Use Section 9.

### A2. Aspect lock and Swap Aspect

**Deliverable and inputs**

Deliver Lock aspect ratio and Swap Aspect, including scoped `X`. Use F1's compatibility
readers, F2's scope, and the current crop owner. A1 supplies the display order in this sequence.

**Owners and steps**

Use `CropLockedAspectRatio()`, `ConstrainCrop()`, `ImageGeometryUpdate`,
`ParseGeometryUpdate()`, `EditorGeometryPanel.qml`, and `shortcut_registration.cpp`.
Advance the versions for effective fixed-aspect semantics under Section 6.1. Resolve a legacy
fixed ratio with the legacy source rule when source dimensions are available. Preserve the
stored crop rectangle and historical root bytes. Use the accepted effective ratio on later
edits, save, and restore. Derive Lock from Free, Custom, or fixed preset. Give the button and
`X` the same `SwapCropAspect()` command. Use orientation's displayed-axis projection when present.
Validate the new crop and update its ratio and frame together. Add the accessible action to
the Crop row after SVG approval.

**Success and failure**

Lock or Swap Aspect → catalog conversion → crop validation → one geometry edit and settled
history entry → restored panel projection. A square swap creates no edit. An impossible frame
or absent source dimensions returns an error before mutation.

**Verification and exit**

Add `SwapAspectChangesFrameWithoutRotatingImage`,
`FixedPresetSwapSurvivesFurtherCropEditsAndReopen`, and
`LockToggleKeepsFrameAndUsesFreeOrCustomPreset`. Check fixed, Custom, Free, and square cases,
legacy portrait and landscape, Undo and Redo, and the actual prior reader. If O2 is present,
verify reciprocal ratios after quarter turns and reflections. Build the F1 targets,
`EditorParameterCatalogTest`, `EditorGeometryPanelQmlTest`, and `ShortcutRegistryTest`.
Confirm `X` retains text entry and mask-tool priority.
Completion record pending. Use Section 9.

### G1. Selectable composition guides

**Deliverable and inputs**

Deliver all choices, visibility modes, orientations, shortcuts, and explicit preference recovery
in Section 5.3. Use F2's scope and the overlay and automatic rotation grid merged in #943.

**Owners and steps**

Add the proposed `crop_guide_paths.{hpp,cpp}` for the crop-relative paths.
Keep preference storage in `EditorBehaviorPreferences`. Register scoped `O` and `Shift+O`.
Project the current crop or straightening input through the interaction owner for During input.
Connect choices and Reset Guide in the Crop panel. Draw through `BuildOverlaySceneGeometry()`.
Reuse `fine_grid_triangles` and the existing rotation-active projection. Draw coincident Thirds
once. Keep crop handles and the fine grid independent of the preferences.

**Success and failure**

Selector or shortcut → preference owner → composition paths → overlay lines.
Invalid saved values show Reset Guide with no substitute selection. Invalid dimensions produce
no composition paths until valid dimensions arrive. The rotation aid remains independent.

**Verification and exit**

Add `CompositionGuidesStayInsideCropAtAllOrientations` (within `1e-6` normalized units),
`DiagonalAnglesAndTrianglePerpendicularsMatchCropPixels` (within `1e-4` radians),
`GuidePreferenceChangesDoNotCreateHistoryEntries`, and
`InvalidGuidePreferenceRequiresExplicitReset`. Verify None, Never, and invalid preferences
during rotate drag. Check deduplication, release repaint, preference persistence, and export
exclusion. Build the proposed `CropGuidePathsTest`, `EditorGeometryPanelQmlTest`,
`EditorCropInteractionTest`, `EditorOverlayInteractionTest`, `OverlayCursorTest`,
and `ShortcutRegistryTest`.
Check the displayed paths at DPR 1, 1.5, and 2.
Completion record pending. Use Section 9.

### T1. Projective equations and frame resolution

**Deliverable and inputs**

Implement Sections 5.4 and 5.5 validation and geometry math through internal render inputs.

**Owners and steps**

Put the matrix operations in the proposed `edit/geometry/projective_transform.{hpp,cpp}`.
Use `types.hpp`, `ResolveRenderGeometry`, and existing crop coverage math. Implement the
projection, pivot, order, inverse, and Jacobian in double precision. Resolve the constrained
frame through one owner operation. Calculate conservative bounds and round final extents once.

**Success and failure**

Finite scalar values → composed matrix → validated mappings, coverage, and crop.
A singular map, a horizon crossing, or an empty constrained frame returns a specific error.

**Verification and exit**

Build `GpuDagGeometryTest`. Add `ProjectivePointRoundTripMatchesWithinTolerance`
(`1e-4` source pixel), `PerspectiveSignsAndAspectScaleOffsetsMatchSpecification`,
`CombinedPerspectiveMatchesSpecifiedRotationOrder`,
`ConstrainedFrameUsesNearestCenterAndPreservesRequestedRatio`, and
`InvalidProjectiveMappingReturnsErrorWithoutGeometry`. Check landscape, portrait, square, odd
extents, combined extrema, and default pixel and dimension parity.
Completion record pending. Use Section 9.

### T2. Projective sampling on CUDA, Metal, and OpenCL

**Deliverable and inputs**

Execute T1 mappings on every production backend through the existing geometry sample.

**Owners and steps**

Update the current host geometry pass parameters and the CUDA, Metal, and OpenCL files in
Section 4. Pass nine coefficients with matching host and shader layout. Divide by the
homogeneous coordinate before sampling. Preserve the current interpolation and black border
policy. Register and install changed program resources through the existing backend owners.

**Success and failure**

Resolved geometry → selected backend → source sample → output buffer.
A resource or dispatch failure returns the render error before successful frame publication.

**Verification and exit**

Add `ProjectiveResampleMatchesIndependentPixelOracle` and
`BackendFailureReportsErrorWithoutPublishingFrame`. Use ramps and labeled corners. Compare
valid interiors within `1e-3` source pixel and `2e-4` in float ramp values. Assert the expected
border pixels separately. Build and run `GpuDagCudaDevelopTest`, `GpuDagMetalDevelopTest`, and
`GpuDagOpenClDevelopTest` on their supported configurations. Record unavailable backends and
compare existing failures with upstream reports such as #869.
Completion record pending. Use Section 9.

### T3. Projective final-frame consumers

**Deliverable and inputs**

Bring masks, comparison, ROI, and export onto the same final-frame mapping from T1 and T2.

**Owners and steps**

Audit resolved geometry in mask sampling and overlays, viewport coverage, ROI and sampling
footprints, `comparison_presentation_image.cpp`, `EditorComparisonCanvas.qml`, and export
geometry requests. Preserve mask source coordinates. Evaluate spatial scale with the projective
Jacobian where coverage needs it. Extend comparison placement and QML together. Compare each
consumer against the same labeled source positions. Keep projective maps out of affine-only APIs.

**Success and failure**

Resolved final map → mask placement and sample footprints → aligned preview, comparison, and
export. An invalid map returns a geometry error before presenting the affected result.

**Verification and exit**

Add `MaskRemainsOnSourceFeatureAfterPerspectiveChange` for analytic and raster masks,
`PerspectiveRoiPixelsMatchCompleteImageWithinTolerance` (interior float pixels within `2e-4`),
and `PerspectiveComparisonAndExportUseResolvedGeometry`. Check all eight orientations when
O1 is present. Build `EditorComparisonGeometryTest`, `ExportServiceTest`,
`GpuDagCudaDocumentGeometryRequestTest` where configured, and the registered direct mask and
viewport coverage tests, including `MaskCoveragePresentationTest` and
`ViewportCoverageCompositeTest`.
Completion record pending. Use Section 9.

### T4. Transformed editing view and crop input

**Deliverable and inputs**

Implement Section 5.5's editing view from T1 and T2 while retaining the crop interaction from #943.

**Owners and steps**

Use the existing render request controller, `EditorGeometryOverlayPipelineTest` integration
path, crop interaction controller, and overlay item. Project the final frame with
`E * inverse(F)`. Draw and hit test the same mapped quad. Convert pointer positions through
the inverse. Map selectable guides through the same placement when G1 has landed.
Keep hit zones, straightening thresholds, and fine-grid spacing in widget logical pixels.
Use the existing GPU-release suspension and resume path.

**Success and failure**

Geometry editing request → transformed source bounds → view fit → mapped overlay and inverse
input. An invalid mapping returns the real error and provides no misleading input geometry.
GPU release suspends the request. The last release resumes it on the same backend.

**Verification and exit**

Add `EditingCropBoundaryMapsBackToFinalOutputCorners`,
`PointerRoundTripTargetsRenderedSourcePixel` (within `0.25` displayed pixel), and
`TransformedCropPreservesInheritedHitZonesAndRotationAid`. Confirm bracket shape, hover
cursors, angle badge, release repaint, and full-frame corner access. Run the
`EditorGeometryOverlayPipelineTest`, `EditorCropInteractionTest`,
`EditorOverlayInteractionTest`, and `OverlayCursorTest` targets. Include combined perspective,
aspect, and straightening and the eight orientations when O1 is present.
Completion record pending. Use Section 9.

### T5. Transform writes, history, and compatibility

**Deliverable and inputs**

Persist manual transform fields and Constrain Crop only after F1 and T1–T4 supply the
compatible readers and complete render path.

**Owners and steps**

Add the fields to `ImageGeometryModel` and `ImageGeometryUpdate`. Advance the versions under
Section 6.1. Connect the transform catalog, parser, and service writes to the resolver's
validation. Commit the accepted transform and any adjusted crop in one owner operation.
Add all pixel-changing fields to the current geometry keys and history replay. Publish only
after the complete change. Verify programmatic writes before adding QML controls in T6.

**Success and failure**

Catalog patch → parser → owner validation → complete update → history → resolved render.
An invalid field, impossible crop, or persistence failure returns the existing error with
no partially published write. Use the existing render error path after an accepted edit.

**Verification and exit**

Add `TransformWritePreservesUnspecifiedGeometryFields`,
`TransformAndConstrainedCropUndoTogether`, `SavedTransformsRestoreAfterReopen`, and
`OldGeometryLoadsIdentityTransforms`. Exercise transfer, version selection, journal replay,
old canonical bytes, and the actual prior reader. Build the F1 targets,
`EditorParameterCatalogTest`, and registered owner and automation write tests.
Completion record pending. Use Section 9.

### T6. Transform controls and product acceptance

**Deliverable and inputs**

Expose the complete manual Transform section in Section 5.1 through T5's owner write path.

**Owners and steps**

Use `EditorGeometryPanel.qml`, the current adjustment models, the catalog, and
`AdjustmentSlider`. Add the six sliders and Constrain Crop, translated accessible names,
tooltips, and explicit errors. Bind document restore as a read. Use the shared callbacks for
one settled history entry per drag. Add the Transform, Crop & Rotate, and Reset All operations
through the geometry owner. Attach active transform input to guide visibility when G1 is present.

**Success and failure**

Control input → shared command → resolved render → control and overlay projection.
A rejected input shows the prior accepted value and the validation error. A backend error
is visible. Undo uses the normal history path.

**Verification and exit**

Add `TransformPanelRestoreDoesNotSubmitEdits`,
`TransformDragCreatesOneSettledHistoryEntry` for rail and value drags,
`ResetTransformPreservesCropAndRotation`,
`ResetCropWithManualTransformsKeepsConstrainedCoverage`,
`SectionResetsPreserveGuidesAndCreateOneHistoryEdit`,
`TransformControlsUseDisplayedAxesAfterOrientation`,
`GeometryResetPreservesImportLookAndGradeVisibility`, and
`GeometryPresentationResumesAfterLastGpuReleaseHolder`. Build `alcedo_main`,
`EditorGeometryPanelQmlTest`, `EditorCropInteractionTest`, and
`EditorGeometryOverlayPipelineTest`. Verify the combined acceptance criteria in Section 8
with landscape, portrait, and architectural images. Include displayed axes after rotate and flip,
Swap Aspect with nondefault transforms, guide visibility, focus, narrow layout, and export.
Completion record pending. Use Section 9.

## 8. Verification and engineering limits

The roadmap revision uses source inspection and diff checks. Use the commands below to collect
implementation evidence in each PR.

Use PowerShell from the repository root on Windows.
The example uses the Qt installation verified by the standalone preview.
Use the existing build's configured Qt path when it differs. Do not change Qt versions to run a phase.

```powershell
Set-Location C:\Users\Haidong\Data\Photography\AlcedoStudio
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorGeometryMathTest EditorGeometryPanelQmlTest GpuDagGeometryTest --parallel 4
$env:PATH = "$PWD/build/debug/vcpkg_installed/x64-windows/debug/bin;$env:PATH"
ctest --test-dir build/debug -N -R '^(EditorGeometryMathTest|EditorGeometryPanelQmlTest|GpuDagGeometryTest)\.'
ctest --test-dir build/debug -R '^(EditorGeometryMathTest|EditorGeometryPanelQmlTest|GpuDagGeometryTest)\.' --output-on-failure -j 1
```

Replace the target list and anchored expression with each phase's confirmed targets.
Do not configure a second build tree. Allow at least 20 minutes for a CUDA build invocation.
Poll the same healthy build process. Never build and test concurrently in one build directory.
Run GPU tests serially. Check runtime DLL copies if behavior does not match the rebuilt source.
On macOS, use `macos_debug` with tests enabled in `build/macos-debug` for Metal evidence.
Only the user can request a full test suite. Focused suites do not count as a full-suite result.

Use C++ owner and service tests for input behavior. Do not depend on unreliable offscreen
`WorkspaceShellTest` synthetic input. Record skipped QML coverage honestly.
For changes that affect #943's behavior, discover the current `EditorCropInteractionTest`, `EditorOverlayInteractionTest`,
and `OverlayCursorTest` cases. Retain its edge-versus-rotation, center-hold, per-drag-limit,
zero-snap, rotation-grid release, corner-bracket, and outside-image cursor assertions.
Run those focused regressions alongside new composition-guide and transform cases.
Confirm that the inherited crop-mode viewport margin permits full-frame corner-zone access.
If that remains unresolved, track the existing #942 requirement before accepting the dependency.
Put temporary evidence under `build/tmp/geometry_crop_transform/` and remove task output after completion.
Read any CRLF file as bytes before editing. Convert it to LF in the required separate commit.
Do not format untouched lines or rename unrelated roadmap files.

| Acceptance area | Required observable result |
| --- | --- |
| Defaults and compatibility | Existing documents render the same pixels and dimensions. Each format change preserves historical object identity. The actual prior reader rejects each unsupported revision. |
| Composition | Every selected guide has correct paths, orientation, clipping, and preference persistence. Guides never enter exports. |
| PR #943 integration | Inherited hit zones, rotation guards, brackets, cursors, angle badge, and automatic grid remain intact. Composition preferences do not suppress the active rotation aid. Coincident Thirds strokes appear once. |
| Aspect selection | The complete sequence matches Section 5.2. Reopen restores both preset identity and effective ratio. Lock and Swap Aspect keep their specified states through further edits. |
| Whole-image orientation / #459 | With default manual transforms and no crop, quarter turns preserve every source pixel and swap dimensions. With an existing crop or transform, they preserve the accepted framed result. Two right turns give 180 degrees. Reopen and export preserve orientation. |
| Crop shorthand | All five actions are visible and accessible. Flips use displayed axes. Swap Aspect and scoped `X` change framing without image rotation. |
| Boundaries and invalid input | Combined limits either produce valid finite geometry or a specific rejection before mutation. |
| Crop constraints | Enabled mode contains no uncovered source area. Disabled mode retains framing and explicit black borders. |
| History and versions | One settled edit records transform and crop together. Undo, Redo, reset, and version selection restore all fields. |
| Section resets | Each effective reset creates one owner update and one history edit. Guides remain unchanged. Crop constraints adjust framing only as specified in Section 5.1. |
| Rendering and input | CUDA, Metal, OpenCL, masks, pointer mapping, ROI, comparison, and export use the same mapping. |
| Failures | Validation and render errors remain visible. No backend, algorithm, or quality substitution occurs. |
| Accessibility | All new controls have names and keyboard behavior. Text editing and mask shortcuts retain priority. |

Real engineering risks are affine assumptions, projective horizons, crop coverage near extreme
perspective, mask alignment, orientation composition, and old-reader handling.
These are required implementation work.
If an audited owner or call chain has changed, update this proposal before changing code.
If a required compatibility mechanism is absent, specify its exact owner change before persisting fields.
Do not mark a backend complete from source inspection or from another backend's test results.

## 9. Completion evidence template

Complete this record for each implemented phase. Do not fill it during proposal creation.

```text
Phase / date / status:
Source revision and branch:
Actual changed modules and diff size:
Implemented behavior:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual verification, photographs, zoom, and DPR:
Backend and full-resolution export evidence:
Evidence path and cleanup:
Unimplemented requirements and unavailable platforms:
Related issue numbers:
```
