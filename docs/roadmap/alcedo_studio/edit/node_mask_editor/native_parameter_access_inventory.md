# Native Parameter Access Production Path Inventory

Date: 2026-09-05

This inventory records the production paths observed while executing the native parameter access
prerequisite. It is a characterization of the current `main`-based implementation. It does not
introduce a new parameter API, move ownership, or re-label persistence JSON as a Model interface.

## Evidence boundary

The current editor has two different kinds of JSON work:

1. Normal editing and panel projection still use JSON-shaped values as an in-process carrier. The
   target Model is read with `ToJson()`, partial input is merged into that object, and the result is
   written with `LoadJson()`.
2. Project, WAL, import/export, and history payloads are legitimate serialization boundaries. Their
   continued use is not evidence that panel projection or live parameter application is typed.

The characterization tests keep these observations separate. The target-model read/load counts are
asserted around parameter read/apply calls, while `CanonicalPipelineDocumentJson()` is exercised in
a separate persistence assertion. Later migration work can require zero target-model JSON calls
without forbidding the persistence assertion.

## Production call chains

### User input to live Model and render

```text
EditorTonePanel / EditorLookPanel / LUTPanel / RAW / Display / Geometry QML
  -> EditorAdjustmentValueModel / EditorAdjustmentEnumModel / curve model / panel paramsBuilder
  -> EditorSessionController::submitPatch(fieldKey, paramsJson, settled)
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorPendingInputQueue::AdmitFieldChange
  -> EditorSessionCommandQueue::PostCompletion on the session owner thread
  -> EditorSessionService::TryConsumePendingInput
  -> EditorSessionEditController::HandlePendingSequence
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> ApplyEditorParameterPatch (live PipelineDocument)
  -> MirrorPatchToExecutor -> ApplyEditorAdjustmentSnapshot (CPU pipeline)
  -> existing render scheduler / present completion
```

For a settled input, `HandlePendingSequence` calls `CommitAdjustment` after the preview mutation.
The commit captures before/after values in the existing `PipelineEditBatch`, publishes the history
entry, and then the same serial render owner routes the settled render.

### Live state to panel values

```text
live CPU pipeline / history working state
  -> MakeAdjustmentSnapshotFromLivePipeline
  -> field operator GetParams() for each registered panel field
  -> EditorRenderAdjustmentSnapshot::patches (JSON field payloads)
  -> EditorHistoryProjection::ReadAdjustmentSnapshot
  -> EditorSessionService::adjustment_snapshot
  -> EditorSessionController::OnBackendChanged
  -> BuildSnapshotMap (QJsonDocument parse -> QVariantMap)
  -> panel loadFromSnapshot / load-only Model setters
```

`EditorRenderAdjustmentSnapshot::params_json` also receives `ExportPipelineParams().dump()` in
`MakeAdjustmentSnapshotFromLivePipeline`. That full stage document remains useful for checkpoint
serialization, but `BuildSnapshotMap` currently projects panel values from the per-field `patches`
array instead of directly reading a typed Graph Node Model.

## Carrier matrix

| Parameter carrier | Current UI entry and fields | Current owner and actual fields | Current JSON/copy point | Thread boundary and final consumers |
| --- | --- | --- | --- | --- |
| Scalar | `EditorTonePanel.qml` (`exposure`, `contrast`, `highlights`, `shadows`, `white`, `black`) and `EditorLookPanel.qml` (`saturation`, `vibrance`, `clarity`, `sharpen`, `film_grain`, `halation`) | `ColorGradeNodeModel` adjustment instance for Grade fields; `DrtNodeModel` adjustment instance for DRT/Post fields; scalar payload value in the concrete operator Model | `EditorAdjustmentValueModel::numericParamsJson()` or a QML `paramsBuilder`; `ApplyModelPatch` calls target `ToJson`, merges, then `LoadJson`; read path calls `ReadEditorParameterJson` and target `ToJson` | GUI submit -> session owner queue -> live render owner under the pipeline lock; CPU stage parameters, history before/after JSON, and rendered frame consume the result |
| Enum / toggle | RAW method and switches, display method/encoding/limiting selections, lens enable and color-temperature mode | `DevelopNodeModel::Params()`, `DrtNodeModel::Params()`, or the corresponding adjustment instance | `EditorAdjustmentEnumModel::enumParamsJson()` / `EditorAdjustmentToggleModel::toggleParamsJson()`; nested panel builders can copy the complete nested object; target Model JSON is read and reloaded by the same apply path | GUI -> session owner; CPU operator setup, history, and image-loading or display stages consume it |
| Curve / related container | `EditorTonePanel.qml` and `EditorToneCurveModel` field `curve` | Color Grade curve adjustment instance; `CurveModel` owns the point container | `EditorToneCurveModel::buildParamsJson()` serializes the points; `CurveModel::ToJson()` / `LoadJson()` carries the target model; the current patch replaces the whole curve field even when one point moved | GUI pointer sequence -> owner consume -> document/history -> CPU stage; the current path copies the point array as the required field payload |
| LUT | `LUTPanel.qml`, field `lut` (canonical operator key `ocio_lmt`) | Color Grade LMT adjustment instance; `LmtModel` owns the cube path and enable state | Catalog selection creates an operator-shaped JSON payload; `EditorAdjustmentDocumentParamsFromWrite` maps `cube_path` / `value`; target Model JSON is still merged and reloaded | GUI -> session owner -> live Grade Model -> CPU/GPU parameter preparation and history/WAL |
| RAW | `EditorRawDecodePanel.qml`, field `raw_decode` | The document's `DevelopNodeModel::Params()`; RAW method, highlight reconstruction, camera-WB and user-WB fields | `buildRawParams()` copies the panel's nested `raw` object and calls `JSON.stringify`; `ApplyEditorParameterPatch` reads and reloads the complete Develop params JSON; the executor mirror maps the same field to the Image Loading stage | GUI -> owner thread and render lock; RAW/Image Loading stage and subsequent render consume it; image-local metadata must remain in the same owner |
| ODT / display | `EditorDisplayTransformPanel.qml`, field `odt` | The document's DRT/Post `Params()`; method, encoding, EOTF, peak luminance, limiting space, and OpenDRT choices | `buildOdtParams()` builds the complete nested `odt` object; `DrtNodeModel::Params()` goes through `ToJson`/merge/`LoadJson`; panel projection parses per-field JSON into `QVariantMap` | GUI -> owner thread -> DRT/Output Transform CPU/GPU preparation, history, and rendered output |
| Lens | `EditorGeometryPanel.qml`, field `lens_calib` | The document's Develop parameters, including lens metadata and calibration settings | `buildLensParams()` starts from `lensCatalog.defaultParamsJson`, copies or updates nested fields, and calls `JSON.stringify`; current apply reads and reloads the complete Develop params JSON | GUI -> owner thread and render lock -> image-loading/lens stage, CPU/GPU preparation, and history |
| Geometry | `EditorGeometryPanel.qml`, field `crop_rotate` | `PipelineDocument::Geometry()` / `ImageGeometryModel`; crop rectangle, rotation, and expand-to-fit | `buildCropParams()` creates the complete geometry object; `ApplyEditorParameterPatch` performs `Geometry().ToJson()` -> merge -> `ImageGeometryModel::FromJson()` -> assignment; no Graph Node instance is created | GUI -> owner thread -> document geometry and render scheduling; geometry is document-owned and is not a panel-local copy |

## Ownership, copies, and boundaries

| Existing interface | Observed responsibility | P1 disposition |
| --- | --- | --- |
| `EditorAdjustmentPatch::params_json` | Carries one field's UI write through the existing session boundary | Retain as the current input carrier for characterization; replace the ordinary editing carrier in the later queue/Model migration, not in this inventory change |
| `EditorPendingInputQueue` | Coalesces by field and stores the latest string payload; `Peek()` copies queued sequences; `TakeReadyBatch()` moves sealed sequences but copies the open sequence before clearing it | Retain ordering, release, cancel, and node identity behavior; record the copy points for the queue migration |
| `EditorRenderAdjustmentSnapshot::patches` | Carries the changed field payloads to render and panel presentation | Retain the existing render sequencing; later projection work must replace JSON field parsing with scoped typed values |
| `EditorRenderAdjustmentSnapshot::params_json` | Receives a full `ExportPipelineParams()` document during live snapshot refresh | Preserve at checkpoint and persistence boundaries only; it must not become the ordinary panel getter |
| `ApplyEditorParameterPatch` / `ReadEditorParameterJson` | Current live write and read boundary for document-owned parameter targets | P2 replaces the target Model JSON merge/reload and supplies focused validation/read operations; P1 records the exact current calls |
| `MakeAdjustmentSnapshotFromLivePipeline` / `BuildSnapshotMap` | Current CPU operator JSON -> field patch -> Qt `QVariantMap` presentation path | P4 replaces the panel projection path with selected-owner typed adapters while retaining load-only UI semantics |
| `PipelineEditBatch` and journal/WAL payloads | Stores before/after values and durable history operations | Preserve as the serialization boundary; do not remove it merely because live Model access becomes typed |
| `MakeFullDto` / `ParameterArena` | Runtime full parameter preparation and GPU upload path | Not migrated by P1; P5 owns the measured removal or narrowing of unnecessary full DTO preparation |

## Failure and preserved-behavior evidence

The existing tests establish the current safety behavior that P1 must not disturb:

- `EditorPipelineCommandServiceTest.ThrowingSetterRestoresOnlyAffectedModelParameters` proves a
  throwing Model setter restores the affected Model and leaves sibling instances/topology intact.
- `EditorPipelineCommandServiceTest.InvalidCompoundParameterDoesNotPartiallyApplyOrDirtyModel`
  proves malformed related fields are rejected before a dirty update is published.
- `EditorPipelineCommandServiceTest.GeometryAndDevelopRejectInvalidValuesBeforeAnyWrite` proves
  document Geometry and Develop validation retain the document on invalid input.
- `EditorPendingInputTest.QueuedItemCarriesOnlyChangedFieldPayload` and the surrounding queue tests
  prove field coalescing, distinct-field retention, release ordering, and target identity behavior.
- `EditorAdjustmentModelTest` and `EditorAdjustmentSnapshotQmlTest` prove local drag/enum/toggle
  submission and load-only panel restoration do not silently submit while loading.

The new characterization assertions in `EditorPipelineCommandServiceTest` make the failure evidence
executable:

- `ApplyingScalarPatchReadsAndReloadsTargetModelJson` observes one target `ToJson()` and one
  `LoadJson()` for a scalar write.
- `ReadingScalarParameterUsesModelJsonWhileDocumentPersistenceUsesDocumentJson` observes a target
  `ToJson()` for a panel read, then separately observes document serialization. This separation is
  the guard point for later zero-JSON projection/application assertions.

## Explicit non-goals for this inventory

- No typed Model API, reflection table, generic message router, or new parameter mirror is added.
- No QML layout or panel capability change is made.
- No history, WAL, import/export, or project serialization format is removed.
- No fallback backend, reduced decode quality, or alternate rendering path is introduced.
