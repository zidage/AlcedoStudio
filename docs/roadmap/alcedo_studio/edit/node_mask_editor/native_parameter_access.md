# Native parameter access

This note is the maintenance map for live parameter edits. JSON remains a
serialization format. It is not the in-process Model API.

## Scalar edit

Example: exposure slider.

```text
QML / EditorAdjustmentValueModel local value
  -> EditorSessionController::submitWrite (EditorScalarWrite)
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorPendingInputQueue::AdmitFieldChange
  -> TakeReadyBatch (move the field write)
  -> EditorSessionEditController::HandlePendingSequence
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> ApplyEditorParameterWrite -> ExposureModel::SetValue
  -> RemirrorEditorParameterToExecutor (CPU operator JSON at this boundary)
  -> serial render with live_parameters_applied
  -> CommitAdjustment (history before/after JSON)
```

`submitPatch` exists only for QML collection objects (RAW, ODT, lens, Geometry).
It parses JSON once on the GUI thread into the same `submitWrite` payload.

## Complex edit

Example: Color Grade curve, or an already-resolved GPU struct.

Curve write uses `EditorCurveWrite` and `CurveModel::ApplyUpdate`. GPU Grade
packing then reads the owner:

```text
owner Model::Read
  -> MakeGradeRuntimeParams / MakeGradeNeighborParams
  -> BindOrRefreshGradeRuntimeSlot
  -> ParameterArena::WritePackedSlot
  -> UploadDirty
```

CameraColor and DRT output do not wrap a Model DTO. They write the packed GPU
struct with `BindOrWritePackedSlot`. DRT still builds GPU tables through
`DrtNodeModel::ToJson()` → `ODT_Op` at that GPU-prep boundary.

## Add an operator or panel

1. Own the value on a Graph Node Model (typed setter / `ApplyUpdate`, dirty bits, `Read`).
2. Add a write alternative in `EditorParameterWrite`, parse it in
   `ParseEditorParameterWrite`, apply it in `ApplyEditorParameterWrite`.
3. Register one `EditorPanelAdapter` on a copy of `EditorPanelAdapterTable`
   (`Production().Add(...)`). Do not add a process-wide JSON property tree.
4. QML: bind `selectedPath`, restore with `loadFromSnapshot`, submit with
   `submitWrite` (or `submitPatch` only when the panel still collects a related-field object).
5. GPU: Grade-like operators pack through `MakeGradeRuntimeParams`. Already-resolved
   GPU structs use `BindOrWritePackedSlot`. Do not call `MakeFullDto` on the live path.

Panel reads go `ReadEditorPanelField` / `ProjectCurrentPanelFields` → existing QML
models. Reads use `NodeId` plus `AdjustmentInstanceId`. They do not pick the first
instance of an operator type.

## Serialization boundaries

These JSON surfaces stay. They are not live Model writes.

- History / WAL / project / import-export (`ToJson` / `LoadJson`,
  `ApplyEditorParameterPatch` as JSON → typed parse)
- CPU executor remirror (`ReadEditorParameterJson` → `SetOperator` / `GetParams`)
- Committed snapshot per-field `params_json` (history restore)
- QML `submitPatch` collection parse
- DRT GPU table prep (`ToJson` → `ODT_Op`)

Live `EditorRenderAdjustmentSnapshot::params_json` stays empty.
`MakeFullDto` remains on `IOperatorModel` for persistence and device recovery.
Live packing and panel projection must not call it.
