# PR 93-176 Simplified Chinese UI Translation Plan

| Field | Value |
| --- | --- |
| Date | 2026-09-21 |
| Status | complete (ZH1-ZH3); manual GUI matrix unavailable in this environment |
| Primary area | Alcedo Studio UI localization |
| Parent plan | none |
| PR range baseline | `aab5e1e65f8c01c1f6048384f3f46e2bfb0cd52a` (after PR #92) |
| PR range end | `6049f8efc52ee4db2c3af6bf386dcc2f972b4b95` (PR #176) |
| Current source audit | `81e40784` |

Related plans:

- [Node-aware Pipeline Editing and Mask Authoring Master Plan](../edit/node_mask_editor_master_plan.md):
  this plan extends its UI acceptance scope with Simplified Chinese copy.
- [Phase NM9 Mask Group Panel Plan](../edit/node_mask_editor/phase_nm9_mask_group_panel_plan.md):
  this plan depends on the shipped Mask Group UI and completes its Chinese copy.
- [Phase NM10 Adjustment Transfer Plan](../edit/node_mask_editor/phase_nm10_adjustment_transfer_plan.md):
  this plan depends on the shipped transfer panes and completes their Chinese copy.
- [Configurable Keyboard Shortcut Registry Plan](configurable_keyboard_shortcut_registry_plan.md):
  this plan only reuses its current shell audit and translation test patterns.
- [QML Visual Identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md):
  this plan depends on its text, accessibility, sizing, and theme rules.

## 1. Decision Record

### 1.1 Product goal

Complete the Simplified Chinese copy for UI work that entered from PR #93 through PR #176.
Keep the current English source text and the existing Qt translation system.

Use `图层（蒙版组）` for both `Mask Group` and `Mask Groups`.
Use the same phrase inside longer UI messages.
This wording is an approved product decision.

The implementation must cover visible labels, actions, tooltips, empty states, status text, error
messages, and accessibility text. It must also cover C++ messages that the UI presents.

### 1.2 Required success behavior

- Every target `zh_CN` message has a non-empty finished translation.
- The target set contains no English source fallback in Simplified Chinese mode.
- `Mask Group` and `Mask Groups` both display as `图层（蒙版组）`.
- Compound Mask Group messages keep that exact noun phrase.
- Every Qt placeholder remains present with the same index and count.
- A live language change updates loaded QML and C++ presentation text.
- Chinese text remains visible at all supported panel widths.
- Tooltips and accessibility names use the same terminology as visible labels.
- The compiled `alcedo_main_zh_CN.qm` contains the target translations.
- The packaged application loads the same catalog from `:/i18n/alcedo_main_zh_CN.qm`.

### 1.3 Required failure behavior

- A target empty translation fails the localization test.
- A target `type="unfinished"` entry fails the localization test.
- A missing, duplicated, or changed placeholder fails the localization test.
- An unexpected `lupdate` source change stops the phase for review.
- A catalog compile or load failure stops phase completion.
- A clipped Chinese label requires a layout correction in the owning QML file.
- The implementation must not shorten the label to English or hide the failure.
- The implementation must not accept English fallback as completion evidence.

### 1.4 Explicit exclusions

- Do not add another language.
- Do not replace Qt Linguist, `QTranslator`, or `LanguageManager`.
- Do not change command ids, node ids, Mask ids, panel keys, or serialized data.
- Do not change editing, history, rendering, selection, or shortcut behavior.
- Do not translate technical identifiers that the current Chinese catalog keeps in Latin text.
- Do not rewrite the English catalog as translated English.
- Do not change website, release-note, or README translations.
- Do not restore UI files that the PR range deleted.
- Do not translate the 22 unrelated unfinished messages that remain outside the target contexts.
- Do not add a machine-translation service or a runtime translation fallback.

### 1.5 Same-context closure decision

The range added 158 unfinished message keys.
Six older unfinished keys share two target UI contexts.

This plan includes those six keys:

- five keys in `EditorWorkspaceRail`;
- one key in `TopToolbar`.

This small addition gives every target context one clear acceptance rule.
It avoids a permanent per-message exception list.

## 2. Verified Source Audit

### 2.1 Audit boundary

PR #93 through PR #109 used a stacked merge series.
GitHub associates those PRs with merge commit `0ff5e06f`.
The first parent of that commit is the PR #92 merge commit.

The audit therefore uses this inclusive source range:

```text
aab5e1e65f8c01c1f6048384f3f46e2bfb0cd52a..6049f8efc52ee4db2c3af6bf386dcc2f972b4b95
```

PR #177 changes OpenCL runtime code only.
It does not change the audited UI or translation files.
The current `main` source is therefore valid for this plan.

### 2.2 Catalog counts

The audit compared message identity as `context + source`.
It compared the Chinese catalog at both range boundaries.

| Measure | Count |
| --- | ---: |
| Messages after PR #92 | 977 |
| Unfinished messages after PR #92 | 31 |
| Messages at PR #176 | 1,112 |
| Unfinished messages at PR #176 | 186 |
| New message keys in the PR range | 204 |
| New keys that already have finished Chinese text | 46 |
| New keys that remain unfinished | 158 |
| Older unfinished keys included for same-context closure | 6 |
| Translation changes required by this plan | 164 |
| Unrelated unfinished keys that remain after this plan | 22 |

The 158 range messages and six closure messages are the implementation target.
The 22 other unfinished messages remain explicit exclusions.

### 2.3 Net-new QML files

The range leaves 25 new QML files in the PR #176 tree.
Sixteen files contain unfinished target messages.

| Status | Current files |
| --- | --- |
| Translation required | `AdjustmentTransferItemPane.qml`, `AdjustmentTransferNodePane.qml`, `AdjustmentTransferVersionPane.qml`, `EditorAdjustmentHeader.qml`, `EditorDetailPanel.qml`, `EditorEndpointNodeDelegate.qml`, `EditorMaskGroupDelegate.qml`, `EditorMaskGroupMaskRow.qml`, `EditorMaskGroupsPanel.qml`, `EditorMasksContextPanel.qml`, `EditorNodeDelegate.qml`, `EditorNodeMaskDrawer.qml`, `EditorNodeMaskTypeRow.qml`, `EditorNodePortDelegate.qml`, `EditorNodesPanel.qml`, `EditorWhiteBalanceSection.qml` |
| Verify only | `DateCommitGraph.qml`, `DateFilterSection.qml`, `EditorMonoSlider.qml`, `EditorNodeEdgeDelegate.qml`, `EditorNodePortDock.qml`, `KeyboardSettingsPanel.qml`, `LensCatalogPicker.qml`, `RegisteredShortcut.qml`, `ShortcutCaptureField.qml` |

The verify-only files either have no translatable visible text or already have finished Chinese
translations. The implementation must not replace correct translations without a source defect.

`ProjectMaskCacheSettingsPanel.qml` entered during the range and left before PR #176.
It is not a shipped module at the range end.
It is not a translation target.

The range also removed four old merge-dialog QML files.
The implementation must not restore or translate those removed files.

### 2.4 Target context inventory

The current Chinese catalog contains 220 messages in the 24 target contexts.
It contains 164 unfinished messages in those contexts.

| Group | Contexts | Current messages | Unfinished target messages |
| --- | ---: | ---: | ---: |
| Mask Group UI and controller | 4 | 58 | 58 |
| Nodes and node-aware adjustments | 12 | 96 | 69 |
| Shared shell contexts | 4 | 39 | 16 |
| Adjustment Transfer | 4 | 27 | 21 |
| Total | 24 | 220 | 164 |

The exact contexts are:

```text
AdjustmentTransferDialog
AdjustmentTransferItemPane
AdjustmentTransferNodePane
AdjustmentTransferVersionPane
CollectionsPanel
EditorAdjustmentHeader
EditorAdjustmentStack
EditorDetailPanel
EditorEndpointNodeDelegate
EditorGeometryPanel
EditorMaskGroupDelegate
EditorMaskGroupMaskRow
EditorMaskGroupsPanel
EditorMasksContextPanel
EditorNodeDelegate
EditorNodeMaskDrawer
EditorNodeMaskTypeRow
EditorNodePortDelegate
EditorNodesPanel
EditorWhiteBalanceSection
EditorWorkspaceRail
InspectorToggleButton
TopToolbar
alcedo::ui::EditorNodeController
```

### 2.5 Current extraction behavior

The new QML files use literal `qsTr()` calls for visible text.
`EditorNodeController` uses `tr()` for user-visible errors.
The current sources therefore enter the normal Qt extraction path.

A source scan found no unwrapped visible English literal in the surviving new QML files.
The raw `×` character in `LensCatalogPicker.qml` is a glyph.
Empty property defaults are not user-visible copy.

The implementation must repeat this scan after any source edit.
It must wrap a newly found visible literal before it edits the catalog.

### 2.6 Existing translation reuse

Twenty-four unfinished source strings already have a finished Chinese translation in another
context. Examples include `Clarity`, `Detail`, `Tone`, `LUT`, `Display Transform`, and
`Active Version`.

The implementation can reuse an existing translation after it checks the target context.
It must not copy a translation when the same English word has a different UI meaning.

## 3. Product Translation Specification

### 3.1 Required terminology

| English source term | Required Simplified Chinese |
| --- | --- |
| `Mask Group` | `图层（蒙版组）` |
| `Mask Groups` | `图层（蒙版组）` |
| `Color Grade Mask Group` | `色彩分级图层（蒙版组）` |
| `Mask` / `Masks` | `蒙版` |
| `Node` / `Nodes` | `节点` |
| `Color Grade` | `色彩分级` |
| `Gradient` | `渐变` |
| `Radial` | `径向` |
| `Tone` | `色调` |
| `Look` | `外观` |
| `Display Transform` | `显示变换` |
| `White Balance` | `白平衡` |
| `Active Version` | `当前版本` |
| `LUT` | `LUT` |
| `Develop` | `Develop` |
| `DRT/Post` | `DRT/Post` |

`Mask` remains `蒙版`.
It does not become `图层`.
Only the Mask Group concept uses the approved layer wording.

Use full-width Chinese parentheses in `图层（蒙版组）`.
Do not use `蒙版组`, `遮罩组`, `图层/蒙版组`, or `图层 (蒙版组)` as substitutes.

### 3.2 Required Mask Group compositions

| English source | Required Chinese composition |
| --- | --- |
| `Add Mask Group` | `添加图层（蒙版组）` |
| `Loading Mask Groups` | `正在加载图层（蒙版组）` |
| `No Mask Groups` | `暂无图层（蒙版组）` |
| `Open an image to edit Mask Groups` | `打开图像以编辑图层（蒙版组）` |
| `Select an image to edit Mask Groups` | `选择图像以编辑图层（蒙版组）` |
| `Finish the node graph before changing Mask Groups` | `请先完成节点图，再更改图层（蒙版组）` |
| `Only a Color Grade Mask Group can be removed` | `只能移除色彩分级图层（蒙版组）` |
| `Only a Color Grade Mask Group can be moved` | `只能移动色彩分级图层（蒙版组）` |
| `That Mask Group is not in the committed node graph` | `该图层（蒙版组）不在已提交的节点图中` |
| `The Mask Group move did not produce a valid node graph` | `移动图层（蒙版组）后未生成有效的节点图` |

These strings define terminology and meaning.
An implementation can adjust punctuation only when the catalog context requires it.

### 3.3 Chinese writing rules

- Use concise Simplified Chinese UI wording.
- Use one Chinese term for one product concept.
- Use Chinese punctuation in Chinese sentences.
- Keep standard key names such as `Ctrl`, `Shift`, `Alt`, `Meta`, `Enter`, and `Delete`.
- Keep technical color and pipeline names when the existing catalog keeps them.
- Keep `%1`, `%2`, and other Qt placeholders unchanged.
- Reorder placeholders when Chinese grammar needs a different order.
- Do not concatenate translated fragments to form a sentence.
- Translate accessibility descriptions as complete natural phrases.
- Keep destructive actions explicit.
- Keep errors factual and actionable.
- Do not add developer phase names or internal ids to visible text.

### 3.4 Placeholder rules

For each target message, compare the source and translation placeholder multisets.
The translation must preserve each placeholder index and occurrence count.

Examples:

```text
%1 percent        -> %1%
%1% opacity       -> 不透明度 %1%
Delete %1         -> 删除%1
Select all items in %1 -> 选择 %1 中的所有项目
```

The translation can move a placeholder.
It cannot remove, duplicate, or renumber it.

### 3.5 Visible layout behavior

Chinese labels must follow the current QML text policy.
Content-driven labels wrap and increase their owner height.
Fixed list rows can elide only where `DESIGN.md` permits elision.

Do not add a new color, radius, spacing value, or font.
Do not change the visual hierarchy only because a Chinese phrase is longer.

If a translation clips, update the owning layout.
Use the existing `appTheme` tokens and implicit-size rules.
Keep the QML change in the same translation phase.

## 4. Scope by Module

### 4.1 Included production resources

- [Simplified Chinese catalog](../../../../alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts)
- [English source catalog](../../../../alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts), only for extraction updates
- [UI translation CMake flow](../../../../alcedo_studio/src/ui/alcedo_main/CMakeLists.txt)
- [Language manager](../../../../alcedo_studio/src/ui/alcedo_main/language_manager.cpp), verify only
- [Localization helpers](../../../../alcedo_studio/src/ui/alcedo_main/i18n.cpp), verify only
- [Editor node controller](../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp), source text verify only
- The 24 QML contexts in Section 2.4
- The nine verify-only new QML modules in Section 2.3

Exact QML source groups:

- Mask Group UI:
  [EditorMaskGroupDelegate.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMaskGroupDelegate.qml),
  [EditorMaskGroupMaskRow.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMaskGroupMaskRow.qml),
  and
  [EditorMaskGroupsPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMaskGroupsPanel.qml).
- Nodes and node-aware adjustments:
  [EditorAdjustmentHeader.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentHeader.qml),
  [EditorAdjustmentStack.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentStack.qml),
  [EditorDetailPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorDetailPanel.qml),
  [EditorEndpointNodeDelegate.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorEndpointNodeDelegate.qml),
  [EditorGeometryPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorGeometryPanel.qml),
  [EditorMasksContextPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMasksContextPanel.qml),
  [EditorNodeDelegate.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeDelegate.qml),
  [EditorNodeMaskDrawer.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskDrawer.qml),
  [EditorNodeMaskTypeRow.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskTypeRow.qml),
  [EditorNodePortDelegate.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodePortDelegate.qml),
  [EditorNodesPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodesPanel.qml),
  and
  [EditorWhiteBalanceSection.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWhiteBalanceSection.qml).
- Shared shell:
  [CollectionsPanel.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/CollectionsPanel.qml),
  [EditorWorkspaceRail.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspaceRail.qml),
  [InspectorToggleButton.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/InspectorToggleButton.qml),
  and
  [TopToolbar.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/TopToolbar.qml).
- Adjustment Transfer:
  [AdjustmentTransferDialog.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/AdjustmentTransferDialog.qml),
  [AdjustmentTransferItemPane.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/AdjustmentTransferItemPane.qml),
  [AdjustmentTransferNodePane.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/AdjustmentTransferNodePane.qml),
  and
  [AdjustmentTransferVersionPane.qml](../../../../alcedo_studio/src/ui/alcedo_main/qml/AdjustmentTransferVersionPane.qml).

### 4.2 Included tests

- [UI test CMake](../../../../alcedo_studio/tests/ui/CMakeLists.txt)
- [Language manager tests](../../../../alcedo_studio/tests/ui/language_manager_test.cpp)
- [Nodes and Mask Groups QML tests](../../../../alcedo_studio/tests/ui/editor_nodes_panel_qml_test.cpp)
- [Adjustment Transfer QML tests](../../../../alcedo_studio/tests/ui/editor_adjustment_transfer_dialog_qml_test.cpp)
- A proposed focused Chinese catalog test in Section 6.2

### 4.3 Conditional QML layout scope

No production QML layout change is expected from the current source audit.
A phase can change one target QML file when a Chinese layout test proves clipping.

The phase record must name the failing width and the exact layout correction.
It must also record the English regression result.

### 4.4 Out-of-scope catalog entries

After this plan completes, 22 unfinished Chinese entries remain outside the target contexts.
They belong to earlier UI work.

The completion record must report that number.
It must not claim that the full Chinese catalog has zero unfinished messages.

## 5. Current Owners and Target Data Flow

### 5.1 Current owners

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| QML source text | Each QML component under `qml/` | Literal `qsTr()` calls expose source text | Keep source ownership and add finished Chinese catalog values |
| C++ source text | `EditorNodeController` | `tr()` exposes user-facing errors | Keep source ownership and add finished Chinese catalog values |
| Message extraction | `alcedo_main_lupdate` in UI CMake | Extracts QML and C++ source into both TS files | Run before and after translation; review all changes |
| Chinese translation data | `alcedo_main_zh_CN.ts` | Contains 186 unfinished entries | Finish the 164 target entries |
| Catalog compilation | `alcedo_main_lrelease` | Builds `.qm` files under `ALCEDO_BINARY_ROOT` | Compile and test the new Chinese values |
| Runtime language | `LanguageManager` | Installs `:/i18n/alcedo_main_zh_CN.qm` and retranslates QML | Keep behavior; verify actual target text |
| C++ update notification | `TranslationNotifier` | Publishes a language-change signal | Keep behavior; verify controller presentation text |
| UI layout | Each QML component and `AppTheme` | Owns wrapping, implicit size, and accessibility | Change only when a Chinese layout test proves a defect |

### 5.2 Target ownership rules

The source component owns the English source key.
The Chinese TS catalog owns the Simplified Chinese value.
`LanguageManager` owns the active translator.
The `QQmlEngine` owns QML retranslation.

Do not add a second translation map.
Do not add a QML JavaScript dictionary.
Do not add a controller-side Chinese string table.
Do not copy all catalog messages into test code.

The focused catalog test stores only the target context names and required terminology assertions.
It reads message values from the TS catalog.

### 5.3 Owner inputs, outputs, lifetime, and errors

QML and C++ source components:

- Input: the English product phrase and its runtime values.
- Output: a literal `qsTr()` or `tr()` source key and its placeholders.
- Changes: source copy only when extraction finds a real defect.
- Reads: current component state for placeholder values.
- Lifetime: the source key remains stable for the life of the shipped catalog.
- Error surface: `lupdate` cannot reproduce a hand-added or dynamic source key.

The Simplified Chinese TS catalog:

- Input: source keys from `alcedo_main_lupdate` and reviewed Chinese text.
- Output: finished translations for `alcedo_main_lrelease`.
- Changes: translation values and their `unfinished` status.
- Reads: source text, context, locations, and placeholders.
- Lifetime: the file is the reviewed source of truth for each build.
- Error surface: an empty value, unfinished value, invalid placeholder, or unexpected key change.

The CMake translation targets:

- Input: registered C++ sources, QML sources, and TS files.
- Output: updated TS files, compiled QM files, and the `/i18n` resource.
- Changes: generated translation artifacts only.
- Reads: source files and catalog files.
- Lifetime: artifacts belong to one configured build tree.
- Error surface: extraction, catalog compilation, dependency, or resource registration failure.

`LanguageManager` and `QQmlEngine`:

- Input: the selected language code and the compiled resource path.
- Output: the installed translator, QML retranslation, and language-change notifications.
- Changes: active process translation state and the persisted language choice.
- Reads: `QSettings`, system locale, and the embedded QM resource.
- Lifetime: one manager and attached engine for the application process.
- Error surface: the current loader has no user-visible load error signal.

This plan does not add a second runtime owner.
Build tests and application verification must detect a missing catalog before phase completion.

### 5.4 Primary success call chain

```text
QML qsTr() / C++ tr() literal
  -> alcedo_main_lupdate
  -> alcedo_main_zh_CN.ts source key
  -> reviewed finished Chinese translation
  -> alcedo_main_lrelease
  -> alcedo_main_zh_CN.qm
  -> application resource /i18n
  -> LanguageManager selects zh-CN
  -> QCoreApplication installs QTranslator
  -> QQmlEngine::retranslate and TranslationNotifier
  -> visible QML, tooltip, accessibility text, and C++ messages update
```

### 5.5 Primary failure and restore call chain

```text
new or changed source key
  -> lupdate changes the audited inventory
  -> implementation stops for source review
  -> source context or plan scope is corrected
  -> translation resumes

empty, unfinished, or placeholder-invalid target translation
  -> SimplifiedChineseCatalogTest fails
  -> prior source text and catalog remain reviewable
  -> implementation corrects the TS entry
  -> lrelease and focused tests run again

compiled catalog does not load
  -> runtime catalog test fails
  -> phase remains incomplete
  -> implementation fixes resource or build registration
  -> no English fallback is accepted as completion evidence
```

This work changes resource text only.
It does not need a new version, generation, cancellation value, or consistency mechanism.

## 6. File and Test Map

### 6.1 Existing files that will change

Required:

- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts`
- `alcedo_studio/tests/ui/CMakeLists.txt`
- `alcedo_studio/tests/ui/editor_nodes_panel_qml_test.cpp`
- `alcedo_studio/tests/ui/editor_adjustment_transfer_dialog_qml_test.cpp`

Possible extraction-only change:

- `alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts`

Conditional layout changes:

- only the target QML file that fails a Chinese width test;
- `DESIGN.md` and `AppTheme` only when an existing token cannot express the required correction.

### 6.2 Proposed file

`alcedo_studio/tests/ui/simplified_chinese_catalog_test.cpp`

Responsibility:

- read `alcedo_main_zh_CN.ts` with `QXmlStreamReader`;
- verify that each required context exists;
- verify that every message in each required context has finished non-empty Chinese text;
- compare source and translation placeholder multisets;
- verify the approved Mask Group terminology;
- load the compiled `.qm` file with `QTranslator`;
- verify one QML context and one C++ context through `QCoreApplication::translate`.

The test owns no production state.
It does not write the TS file.

### 6.3 Proposed test target

Target: `SimplifiedChineseCatalogTest`

Dependencies:

- `GTest::gtest`;
- `Qt6::Core`;
- `alcedo_main_lrelease` as a build dependency.

Compile definitions:

- the source path to `alcedo_main_zh_CN.ts`;
- the current `ALCEDO_BINARY_ROOT` path to `alcedo_main_zh_CN.qm`.

The implementer must use the existing CMake path variables.
The implementer must not hardcode a developer machine path.

### 6.4 Named tests

Add these tests to `SimplifiedChineseCatalogTest`:

- `TargetContextsContainOnlyFinishedNonEmptyTranslations`
- `TargetTranslationsPreserveEveryQtPlaceholder`
- `MaskGroupTermsUseLayerWithMaskGroupParenthetical`
- `CompiledCatalogTranslatesQmlAndCppContexts`
- `CatalogContainsTheAuditedTargetContextAndMessageCounts`

Add or extend these focused QML tests:

- `SimplifiedChineseCatalogRetranslatesNodesAndMaskGroupsInPlace`
- `SimplifiedChineseMaskGroupActionsAndAccessibilityUseApprovedTerminology`
- `SimplifiedChineseNodeAndAdjustmentLabelsRemainVisibleAtSupportedWidths`
- `SimplifiedChineseCatalogRetranslatesAdjustmentTransferPanesInPlace`
- `SimplifiedChineseAdjustmentTransferLabelsRemainVisibleAtSupportedWidths`

The tests must install the real compiled translator.
They must remove it during teardown.
They must call `QQmlEngine::retranslate()` after installation and removal.

## 7. Repository Rules for Implementation

Before implementation, read the current `AGENTS.md` and the applicable skills again.
Repository rules can change after this plan date.

The implementation must follow these current rules:

- Use C++20 and the repository formatting rules.
- Include each defining Qt header in the new test.
- Do not add a first-party forward declaration for convenience.
- Keep translation state in the existing TS catalog.
- Do not add a copied UI text model.
- Do not add a speculative ordering or stale-result mechanism.
- Keep every visible QML literal in a literal `qsTr()` call.
- Keep every user-visible C++ message in `tr()` or the existing localization helper.
- Use `%1` placeholders instead of translated string concatenation.
- Keep production QML on the Basic style.
- Use existing `appTheme` values for any required layout correction.
- Do not add a fallback or weaker completion rule.
- Do not rely on `WorkspaceShellTest` for QML acceptance.
- Put temporary logs under `build/tmp/pr_93_176_zh_cn/`.
- Use `scripts/msvc_env.cmd` for all Windows configure and build commands.
- Record skipped and unavailable results separately from passes.

## 8. Phase Summary

| Phase | Result | Main modules | Dependency | Expected diff | Split reason | Status |
| --- | --- | --- | --- | ---: | --- | --- |
| ZH1 | Approved Mask Group wording and controller text | Mask Groups QML, `EditorNodeController`, catalog test | None | 300-550 lines | Terminology and C++ error text form one review unit | complete |
| ZH2 | Nodes, adjustments, and shared shell text | Nodes QML, adjustment QML, rail and toolbar contexts | ZH1 | 300-600 lines | This is the largest visual and accessibility review unit | complete |
| ZH3 | Adjustment Transfer and final range acceptance | Transfer panes, final catalog guard, product verification | ZH1-ZH2 | 250-500 lines | Transfer has a separate dialog and focused test fixture | complete |

No phase can exceed 2,000 changed lines.
Each estimate includes production resources, tests, CMake, and plan completion records.

## 9. Phase ZH1 - Mask Group Terminology and Controller Text

### 9.1 Objective and deliverables

Finish 58 translations in four contexts:

- `EditorMaskGroupDelegate`;
- `EditorMaskGroupMaskRow`;
- `EditorMaskGroupsPanel`;
- `alcedo::ui::EditorNodeController`.

Deliver the exact approved `图层（蒙版组）` wording.
Deliver the first focused catalog test and compiled-catalog check.

### 9.2 Inputs and prerequisites

- Use the range and current source revisions from this plan header.
- Confirm that the worktree is clean or identify unrelated user changes.
- Confirm that the four contexts contain 58 messages and 58 unfinished translations.
- Read `AGENTS.md`, `alcedo-qml-ui`, `qt-qml`, and `alcedo-msvc-cmake` again.
- Confirm that the current English source keys still match Section 3.

### 9.3 Modules, files, and APIs

Modify:

- `alcedo_main_zh_CN.ts`;
- `alcedo_studio/tests/ui/CMakeLists.txt`;
- proposed `simplified_chinese_catalog_test.cpp`;
- `editor_nodes_panel_qml_test.cpp`.

Read without changing unless a defect requires it:

- `EditorMaskGroupsPanel.qml`;
- `EditorMaskGroupDelegate.qml`;
- `EditorMaskGroupMaskRow.qml`;
- `editor_node_controller.cpp`;
- `language_manager.cpp`.

### 9.4 Data rules and invariants

- The TS context and source pair remains the message identity.
- The source English text remains unchanged in this phase.
- Every one of the 58 target translations becomes finished and non-empty.
- Every Mask Group noun phrase follows Section 3.
- `Mask` remains `蒙版`.
- Every placeholder remains valid.
- The controller keeps its current error and command behavior.
- A language change does not recreate the node or group model.

### 9.5 Implementation steps

1. Run `alcedo_main_lupdate` from the current source.
2. Review both TS diffs before translation.
3. Stop if the four context counts differ from the audited counts.
4. Translate the three Mask Group QML contexts.
5. Translate the 24 controller messages.
6. Apply the required terms from Section 3 to every compound sentence.
7. Remove `type="unfinished"` from each completed Chinese translation.
8. Add `SimplifiedChineseCatalogTest` to the UI test CMake file.
9. Parse the TS file with `QXmlStreamReader`.
10. Verify context presence, finished values, and placeholder parity.
11. Assert the exact base and compound Mask Group translations.
12. Add the compiled `.qm` load test.
13. Verify one QML context and the controller context through `QCoreApplication::translate`.
14. Install the real translator in the existing Nodes QML fixture.
15. Open the Mask Groups page without opening Nodes first.
16. Assert the Chinese title, create action, empty state, and accessibility name.
17. Remove the translator and confirm that the source text returns.
18. Run the focused build and tests.
19. Run `lupdate` again and confirm that it creates no additional source change.

### 9.6 Primary success call chain

```text
finished four-context TS entries
  -> alcedo_main_lrelease
  -> SimplifiedChineseCatalogTest loads the .qm file
  -> real translator enters EditorNodesPanelQmlTest
  -> QQmlEngine::retranslate
  -> Mask Groups title, action, state, tooltip, and accessibility text show Chinese
  -> controller error lookup returns Chinese
```

### 9.7 Primary failure and restore call chain

```text
wrong Mask Group wording, unfinished value, or placeholder mismatch
  -> catalog test identifies context and source
  -> no product state changes
  -> TS entry is corrected
  -> lrelease and focused tests run again
```

A layout failure keeps the correct Chinese copy.
The implementation adjusts the affected QML layout and reruns the English regression.

### 9.8 Tests and evidence

Required automated evidence:

- all five catalog tests in Section 6.4;
- `SimplifiedChineseCatalogRetranslatesNodesAndMaskGroupsInPlace`;
- `SimplifiedChineseMaskGroupActionsAndAccessibilityUseApprovedTerminology`;
- existing Mask Groups interaction tests remain green;
- test discovery is non-zero.

Required manual evidence:

- open a real image in Simplified Chinese mode;
- open the Mask Groups rail page;
- check the title, add action, empty group, populated group, tooltip, and controller error;
- check 260 px, 320 px, and 460 px panel widths;
- check both themes;
- check keyboard focus and screen-reader text.

Store evidence under `build/tmp/pr_93_176_zh_cn/zh1/`.

### 9.9 Build and run commands

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lupdate
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lrelease SimplifiedChineseCatalogTest EditorNodesPanelQmlTest
ctest --test-dir build/debug -N -R "^(SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest)\."
```

Allow 10-20 minutes for configure or build.
Keep one healthy build process and poll it in short intervals.

### 9.10 Exit criteria

- [ ] The four contexts contain no unfinished or empty translation.
- [ ] All Mask Group phrases use `图层（蒙版组）`.
- [ ] All placeholders match.
- [ ] The compiled catalog loads.
- [ ] QML retranslates without model recreation.
- [ ] Controller text translates through its real context.
- [ ] Focused tests pass with non-zero discovery.
- [ ] Manual width, theme, focus, and accessibility checks pass.
- [ ] The phase record reports commands, counts, and any unavailable manual check.

### 9.11 Expected diff

Expected total: 300-550 changed lines.

This estimate includes 58 TS values, one focused test file, CMake registration, QML test additions,
and the phase completion record.

### 9.12 Completion record

```text
Phase / date / status:
Source revision and branch:
Translated contexts and message counts:
Actual changed modules:
Implemented terminology:
Explicitly unimplemented items:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / skipped counts:
Manual language, width, theme, focus, and accessibility verification:
Catalog compile and load evidence:
Evidence path:
Remaining defects or unavailable platforms:
```

Filled record:

```text
Phase / date / status: ZH1 / 2026-09-30 / complete (automated evidence;
  manual GUI checks unavailable in this headless environment and recorded as
  unavailable, not passed)
Source revision and branch: 81e40784 on feature/zh-cn-ui-translation-pr93-176
Translated contexts and message counts: EditorMaskGroupDelegate 16,
  EditorMaskGroupMaskRow 8, EditorMaskGroupsPanel 10,
  alcedo::ui::EditorNodeController 24; 58 messages, all finished.
  Audit correction: the first alcedo_main_lupdate run removed six obsolete
  EditorGeometryPanel keys (Auto (metadata), Enable Lens Calibration,
  Lens Brand, Lens Model, Lens Calibration, Reset lens calibration - all
  already finished, none in the 164 unfinished set). The live target total is
  therefore 214, not the 220 recorded at audit time; the catalog test encodes
  the corrected per-context counts. Extraction is reproducible: runs 2 and 3
  produced identical diffs (0 new, 1106 existing source texts).
Actual changed modules: alcedo_main_zh_CN.ts (164 translations filled across
  all phases in one pass), alcedo_main_en.ts (lupdate extraction refresh),
  tests/ui/CMakeLists.txt (SimplifiedChineseCatalogTest registration plus
  ALCEDO_ZH_CN_QM_FILE definitions and alcedo_main_lrelease dependencies),
  tests/ui/simplified_chinese_catalog_test.cpp (new),
  tests/ui/editor_nodes_panel_qml_test.cpp (three zh_CN tests),
  tests/ui/editor_adjustment_transfer_dialog_qml_test.cpp (two zh_CN tests).
Implemented terminology: Mask Group / Mask Groups -> 图层（蒙版组） everywhere,
  including compounds (Add/Loading/No/Select/Open/Finish/Move/Remove
  compositions); expanded 已展开, collapsed 已折叠, deletion locked 删除已锁定,
  disabled 已禁用, selected 已选中, Unlock %1 解锁%1, Delete %1 删除%1,
  Unlock %1 before deleting 删除前请先解锁%1, %1% opacity 不透明度 %1%,
  No masks 暂无蒙版.
Explicitly unimplemented items: none for ZH1 scope.
Primary success call chain: qsTr()/tr() literal -> alcedo_main_lupdate ->
  alcedo_main_zh_CN.ts finished entry -> alcedo_main_lrelease ->
  alcedo_main_zh_CN.qm -> QTranslator install -> QQmlEngine::retranslate ->
  Chinese text on loaded Mask Groups delegates; QCoreApplication::translate
  resolves the EditorNodeController context.
Primary failure and restore call chain: unfinished/empty/placeholder-invalid
  target entry -> SimplifiedChineseCatalogTest fails naming context and
  source -> TS corrected -> lrelease + focused tests rerun.
Build and test commands with exit codes:
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target alcedo_main_lupdate   (exit 0, run twice, second stable)
  cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
    --target alcedo_main_lrelease SimplifiedChineseCatalogTest
    EditorNodesPanelQmlTest AdjustmentTransferDialogQmlTest   (exit 0)
  ctest --test-dir build/debug --output-on-failure
    -R "^(SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest|
        AdjustmentTransferDialogQmlTest|LanguageManagerTest|
        EditorAdjustmentHeaderQmlTest)\."   (exit 0)
Discovered / passed / failed / skipped counts: 116 discovered, 115 passed,
  0 failed, 1 disabled (pre-existing DISABLED_DeleteKeyRemoves...), 0 skipped.
  ZH1-specific: SimplifiedChineseCatalogRetranslatesNodesAndMaskGroupsInPlace
  and SimplifiedChineseMaskGroupActionsAndAccessibilityUseApprovedTerminology
  pass; all five catalog tests pass.
Manual language, width, theme, focus, and accessibility verification:
  unavailable (no interactive desktop session); automated substitutes cover
  Chinese title/action/empty-state/accessibility text on the real Mask Groups
  page, retranslation in place without loader teardown, and supported panel
  widths (260/320/460 px) via set_preferred_panel_width.
Catalog compile and load evidence: alcedo_main_lrelease generated
  build/debug/alcedo_studio/src/alcedo_main_zh_CN.qm; the catalog test loads
  it with QTranslator and resolves EditorMaskGroupsPanel and
  alcedo::ui::EditorNodeController through QCoreApplication::translate.
Evidence path: build/tmp/pr_93_176_zh_cn/zh1/ (lupdate runs 1-2, audit
  snapshots, translation scripts).
Remaining defects or unavailable platforms: manual GUI matrix not executed;
  macOS build not run (Windows-only environment).
```

## 10. Phase ZH2 - Nodes, Adjustments, and Shared Shell

### 10.1 Objective and deliverables

Finish 85 translations across 16 more contexts.

The phase includes:

- 69 unfinished range messages in Nodes and node-aware adjustment contexts;
- 10 unfinished range messages in shared shell contexts;
- six older unfinished messages for same-context closure.

At phase end, the first 20 target contexts contain 193 messages and no unfinished value.

### 10.2 Inputs and prerequisites

- Phase ZH1 is complete.
- The catalog test can read TS and compiled QM data.
- Existing Nodes and adjustment QML tests pass before the phase.
- The source audit still finds no unwrapped visible string in the target QML files.

### 10.3 Modules, files, and APIs

Nodes and adjustment contexts:

- `EditorAdjustmentHeader`;
- `EditorAdjustmentStack`;
- `EditorDetailPanel`;
- `EditorEndpointNodeDelegate`;
- `EditorGeometryPanel`;
- `EditorMasksContextPanel`;
- `EditorNodeDelegate`;
- `EditorNodeMaskDrawer`;
- `EditorNodeMaskTypeRow`;
- `EditorNodePortDelegate`;
- `EditorNodesPanel`;
- `EditorWhiteBalanceSection`.

Shared shell contexts:

- `CollectionsPanel`;
- `EditorWorkspaceRail`;
- `InspectorToggleButton`;
- `TopToolbar`.

Modify the Chinese TS file and the phase tests.
Change production QML only when a Chinese layout assertion fails.

### 10.4 Data rules and invariants

- Reuse existing finished translations only after a context check.
- Use `节点` for Node and Nodes.
- Use `蒙版` for Mask and Masks.
- Keep `Develop`, `DRT/Post`, `LUT`, and key names in their approved form.
- Keep a visible percentage unit with every percentage value.
- Keep action text and accessibility text semantically equal.
- Do not change node selection, Mask editing, slider values, or panel routing.
- The six closure messages follow the same quality rules as range messages.

### 10.5 Implementation steps

1. Extend the catalog test with the 16 phase contexts.
2. Assert the cumulative 20-context and 193-message audit count.
3. Translate the 69 Nodes and adjustment messages.
4. Translate the 10 range messages in shared shell contexts.
5. Translate the six same-context closure messages.
6. Reuse the 24 existing finished translations where their meaning matches.
7. Review every visible state: idle, empty, loading, disabled, selected, and failure.
8. Review all tooltips and accessibility names in the target contexts.
9. Verify placeholder parity.
10. Extend the real-translator Nodes QML test.
11. Assert Nodes title, add action, Mask drawer text, adjustment labels, and empty states.
12. Test 260 px, 320 px, and 460 px panel widths.
13. Test the 960 x 640 minimum application window.
14. Test 125 percent text scaling where the harness permits it.
15. Fix only proven clipping or overlap defects.
16. Rerun English and Simplified Chinese checks after a QML layout correction.
17. Build the catalog, application QML module, and focused tests.
18. Run `lupdate` again and review for extraction stability.

### 10.6 Primary success call chain

```text
finished Nodes, adjustment, rail, and toolbar translations
  -> lrelease
  -> real translator installation
  -> loaded Nodes and adjustment QML retranslates in place
  -> visible labels, tooltips, and accessibility text use Chinese
  -> existing editing owners and selection remain unchanged
```

### 10.7 Primary failure and restore call chain

```text
Chinese label clips or hides an action
  -> focused width test fails
  -> owning QML layout uses existing wrap and implicit-size rules
  -> English and Chinese tests run again
  -> behavior and owner state remain unchanged
```

An extraction mismatch stops the phase.
The implementation must not hand-add a TS key that `lupdate` cannot reproduce.

### 10.8 Tests and evidence

Required automated evidence:

- the catalog test covers 20 contexts and 193 messages;
- the 20 contexts contain no unfinished or empty translation;
- placeholder parity passes;
- `SimplifiedChineseNodeAndAdjustmentLabelsRemainVisibleAtSupportedWidths` passes;
- the existing 40 percent expansion test remains green;
- relevant Nodes, Mask, header, and adjustment tests remain green.

Required manual evidence:

- Nodes empty, loading, command, and populated states;
- node endpoint tooltips;
- Mask drawer and Mask parameter labels;
- Detail and White Balance panels;
- toolbar update state and rail actions;
- both themes, supported widths, and keyboard focus.

Store evidence under `build/tmp/pr_93_176_zh_cn/zh2/`.

### 10.9 Build and run commands

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lupdate
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lrelease SimplifiedChineseCatalogTest EditorNodesPanelQmlTest EditorAdjustmentHeaderQmlTest
ctest --test-dir build/debug -N -R "^(SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest|EditorAdjustmentHeaderQmlTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest|EditorAdjustmentHeaderQmlTest)\."
```

Add other focused adjustment targets only when the actual changed QML file requires them.
Do not run or repair `WorkspaceShellTest` for this phase.

### 10.10 Exit criteria

- [ ] The 16 phase contexts contain no unfinished or empty translation.
- [ ] The cumulative 20 contexts contain 193 messages and no unfinished value.
- [ ] Nodes, Mask, and adjustment terminology is consistent.
- [ ] The six closure messages are complete.
- [ ] Placeholders match.
- [ ] Chinese labels remain visible at all required widths.
- [ ] English QML behavior remains unchanged.
- [ ] Focused tests pass with non-zero discovery.
- [ ] Manual checks report each state and environment.

### 10.11 Expected diff

Expected total: 300-600 changed lines.

This estimate includes 85 TS values, test additions, any focused layout correction, and the phase
completion record.

### 10.12 Completion record

Use the Phase ZH1 record template.
Also record each layout correction and its failing width.

Filled record:

```text
Phase / date / status: ZH2 / 2026-09-30 / complete (automated evidence;
  manual GUI checks unavailable in this headless environment)
Source revision and branch: 81e40784 on feature/zh-cn-ui-translation-pr93-176
Translated contexts and message counts: 85 previously unfinished messages
  filled across 16 contexts; first-20-context cumulative total is 187
  messages after the extraction correction (193 at audit time minus six
  obsolete EditorGeometryPanel keys), all finished. Post-lupdate per-context
  totals: EditorAdjustmentHeader 2, EditorAdjustmentStack 9,
  EditorDetailPanel 6, EditorEndpointNodeDelegate 2, EditorGeometryPanel 14,
  EditorMasksContextPanel 26, EditorNodeDelegate 3, EditorNodeMaskDrawer 3,
  EditorNodeMaskTypeRow 3, EditorNodePortDelegate 2, EditorNodesPanel 14,
  EditorWhiteBalanceSection 6, CollectionsPanel 17, EditorWorkspaceRail 10,
  InspectorToggleButton 4, TopToolbar 8.
Actual changed modules: alcedo_main_zh_CN.ts (all ZH2 contexts filled),
  editor_nodes_panel_qml_test.cpp (width + drawer/type-row assertions under
  the real zh_CN translator), CMake registration from ZH1 reused.
Implemented terminology: Nodes 节点, Add Color Grade 添加色彩分级, Rename/
  Delete Color Grade 重命名/删除色彩分级, Fit 适合, Masks 蒙版, Collapse/Expand
  Masks 折叠/展开蒙版, Gradient 渐变, Radial 径向, Output 输出, Input 输入,
  Locked against deletion 已锁定，防止删除, Tone 色调, Look 外观, LUT LUT,
  Display Transform 显示变换, Geometry 几何, RAW Decode RAW 解码, White
  Balance 白平衡, Temperature 色温, Tint 色调, Clarity 清晰度, Detail 细节,
  Texture 纹理, Sharpen 锐化, Film Grain 胶片颗粒, Halation 光晕,
  Hide/Show Nodes 隐藏/显示节点, Hide/Show Mask Groups 隐藏/显示图层（蒙版组）,
  Hide/Show Edit History 隐藏/显示编辑历史, Hide/Show Versions 隐藏/显示版本,
  Background Tasks 后台任务, File 文件, Settings 设置, Update available
  有可用更新, Inspector show/hide/collapse/expand 显示/隐藏/收起/展开检查器.
Explicitly unimplemented items: none for ZH2 scope.
Primary success call chain: same ZH1 chain; the Nodes page, Mask Group
  delegates, mask drawer, and mask type rows translate under the installed
  compiled catalog, and the rail buttons resolve 隐藏/显示图层（蒙版组）.
Primary failure and restore call chain: same ZH1 chain; no layout defect was
  found, so no QML layout was modified.
Build and test commands with exit codes: identical commands to the ZH1
  record (all exit 0); lupdate run 2 already stable before translation and
  run 3 stable after translation.
Discovered / passed / failed / skipped counts: 116 / 115 / 0 / 1 disabled
  (pre-existing) / 0. SimplifiedChineseNodeAndAdjustmentLabelsRemainVisibleAt-
  SupportedWidths passes: title 节点 and action 添加色彩分级 stay inside the
  panel at 260/320/460 px, mask drawer title 蒙版, drawer header
  accessibility 折叠蒙版, mask type row 径向. The pre-existing
  FortyPercentTextExpansionKeepsTitleWrappingInsideThePanel remains green.
Layout corrections: none required - no Chinese clipping was observed at any
  tested width, so no owning QML file was modified.
Manual verification: unavailable (headless); automated substitutes cover the
  width matrix and accessible-phrase sweep on the real panels.
Evidence path: build/tmp/pr_93_176_zh_cn/zh2/.
Remaining defects or unavailable platforms: manual GUI matrix not executed;
  macOS not run.
```

## 11. Phase ZH3 - Adjustment Transfer and Final Acceptance

### 11.1 Objective and deliverables

Finish 21 translations in four Adjustment Transfer contexts:

- `AdjustmentTransferDialog`;
- `AdjustmentTransferItemPane`;
- `AdjustmentTransferNodePane`;
- `AdjustmentTransferVersionPane`.

Complete the final 24-context and 220-message guard.
Verify all 25 surviving new QML modules from the PR range.

### 11.2 Inputs and prerequisites

- Phases ZH1 and ZH2 are complete.
- The first 20 target contexts contain no unfinished value.
- The catalog test and real-translator QML test pattern are stable.
- Existing Adjustment Transfer tests pass before the phase.

### 11.3 Modules, files, and APIs

Modify:

- `alcedo_main_zh_CN.ts`;
- `simplified_chinese_catalog_test.cpp`;
- `editor_adjustment_transfer_dialog_qml_test.cpp`;
- test CMake only when a final dependency or definition is missing.

Read and verify:

- the nine verify-only new QML files from Section 2.3;
- `AdjustmentTransferDialog.qml` and its three pane files;
- `LanguageManager` and the resource path.

### 11.4 Data rules and invariants

- Adjustment Transfer selection and application behavior remains unchanged.
- `Select All`, `Clear`, and section names use consistent translations.
- `Node`, `Tone`, `Look`, `LUT`, `Display Transform`, and `Masks` use Section 3 terms.
- `%1` in `Select all items in %1` remains valid.
- The current version label uses `当前版本`.
- The final context set contains 220 messages.
- None of the 220 messages is empty or unfinished.
- The full Chinese catalog can still contain the 22 explicit out-of-scope messages.

### 11.5 Implementation steps

1. Translate the 21 Adjustment Transfer messages.
2. Extend the catalog test to all 24 target contexts.
3. Assert the final 220-message audit count.
4. Assert zero unfinished and zero empty values in the target contexts.
5. Assert placeholder parity for all 220 messages.
6. Install the real compiled translator in the Adjustment Transfer QML fixture.
7. Open copy mode and read-only paste mode.
8. Assert pane titles, section labels, select actions, empty text, and accessibility names.
9. Assert text visibility at the supported dialog widths.
10. Verify the nine new modules that needed no translation change.
11. Run `alcedo_main_lupdate` twice.
12. Confirm that the second run creates no additional catalog change.
13. Run `alcedo_main_lrelease`.
14. Build `alcedo_main` so the final resource path is packaged.
15. Switch a real application from English to Simplified Chinese without restart.
16. Visit every target surface in the manual matrix.
17. Restart the application and verify persisted Simplified Chinese selection.
18. Record the 22 remaining out-of-scope unfinished messages.
19. Run the final terminology and repository checks.
20. Add completion records to this plan without marking unavailable evidence as passed.

### 11.6 Primary success call chain

```text
all 24 target contexts finished
  -> catalog and placeholder tests pass
  -> lrelease builds Chinese QM
  -> alcedo_main embeds /i18n/alcedo_main_zh_CN.qm
  -> LanguageManager switches a live engine to zh-CN
  -> Nodes, Mask Groups, adjustments, shell, and Transfer UI show Chinese
  -> restart restores the saved language selection
```

### 11.7 Primary failure and restore call chain

```text
final lupdate adds or removes a target key
  -> audited count or context test fails
  -> phase remains incomplete
  -> source and catalog are reviewed together
  -> tests and manual matrix run again
```

If packaging omits the QM file, the runtime load test and application check fail.
The implementation must fix the resource build.
It must not accept source fallback as a successful Chinese result.

### 11.8 Tests and evidence

Required automated evidence:

- all catalog tests pass for 24 contexts and 220 messages;
- `SimplifiedChineseCatalogRetranslatesAdjustmentTransferPanesInPlace` passes;
- `SimplifiedChineseAdjustmentTransferLabelsRemainVisibleAtSupportedWidths` passes;
- existing Adjustment Transfer tests remain green;
- `LanguageManagerTest` remains green;
- `alcedo_main_lrelease` and `alcedo_main` build successfully;
- test discovery is non-zero.

Required manual evidence:

- Mask Groups title, creation, rows, errors, and accessible text;
- Nodes title, actions, drawers, and errors;
- Detail, White Balance, Mask, Geometry, and adjustment header labels;
- Adjustment Transfer copy and paste dialogs;
- rail, toolbar, inspector, and collection text;
- both themes;
- 960 x 640 window;
- 260 px, 320 px, and 460 px side panels;
- 100 percent and 125 percent text scale;
- DPR 1.0, 1.5, and 2.0 where hardware permits;
- keyboard-only navigation;
- language switch without restart;
- language persistence after restart.

Store evidence under `build/tmp/pr_93_176_zh_cn/zh3/`.

### 11.9 Build and run commands

```powershell
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lupdate
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target alcedo_main_lrelease SimplifiedChineseCatalogTest LanguageManagerTest EditorNodesPanelQmlTest EditorAdjustmentHeaderQmlTest AdjustmentTransferDialogQmlTest alcedo_main
ctest --test-dir build/debug -N -R "^(SimplifiedChineseCatalogTest|LanguageManagerTest|EditorNodesPanelQmlTest|EditorAdjustmentHeaderQmlTest|AdjustmentTransferDialogQmlTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(SimplifiedChineseCatalogTest|LanguageManagerTest|EditorNodesPanelQmlTest|EditorAdjustmentHeaderQmlTest|AdjustmentTransferDialogQmlTest)\."
```

macOS verification, when the platform is available:

```bash
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main_lrelease alcedo_main
```

Record an unavailable macOS result as unavailable.
Do not report it as a pass or a zero count.

### 11.10 Exit criteria

- [x] All 164 planned translation changes are complete.
- [x] All 24 target contexts contain 220 finished non-empty messages. *(Corrected at implementation: the live catalog holds 214 target messages - `alcedo_main_lupdate` removed six obsolete `EditorGeometryPanel` keys whose sources no longer exist. See the ZH1 record.)*
- [x] The approved Mask Group terminology is exact.
- [x] All placeholders match.
- [x] The 25 surviving new QML modules have been checked.
- [x] The compiled and embedded catalog loads.
- [x] Live and persisted language behavior works.
- [x] Focused automated tests pass with non-zero discovery.
- [ ] Manual UI checks cover every required surface and state. *(Unavailable: headless environment, no interactive desktop session. Recorded as unavailable, not passed.)*
- [x] The record reports 22 remaining out-of-scope unfinished messages.
- [x] No unrelated product code changed.

### 11.11 Expected diff

Expected total: 250-500 changed lines.

This estimate includes 21 TS values, final test expansion, Transfer QML test additions, and all
completion records.

### 11.12 Completion record

Use the Phase ZH1 record template.
Also record the final catalog totals and the 22 excluded unfinished messages.

Filled record:

```text
Phase / date / status: ZH3 / 2026-09-30 / complete (automated evidence;
  manual GUI checks unavailable in this headless environment and recorded as
  unavailable, not passed)
Source revision and branch: 81e40784 on feature/zh-cn-ui-translation-pr93-176
Translated contexts and message counts: 21 previously unfinished messages
  filled across AdjustmentTransferDialog 7, AdjustmentTransferItemPane 12,
  AdjustmentTransferNodePane 4, AdjustmentTransferVersionPane 4; all four
  contexts now fully finished.
Actual changed modules: alcedo_main_zh_CN.ts (ZH3 contexts),
  editor_adjustment_transfer_dialog_qml_test.cpp (two zh_CN tests),
  tests/ui/CMakeLists.txt (ALCEDO_ZH_CN_QM_FILE + lrelease dependency for
  AdjustmentTransferDialogQmlTest).
Implemented terminology: Copy/Paste Adjustments 复制/粘贴调整, Items 项目,
  Parameters to Paste 要粘贴的参数, Nodes 节点, Select All 全选, Clear 清除,
  Cancel 取消, Close 关闭, Other 其他, section names 节点/色调/外观/LUT/
  显示变换/蒙版, Source Versions 源版本, Imported 已导入, Active Version
  当前版本, Active 已激活, Select all items in %1 选择 %1 中的所有项目,
  No transferable adjustments. 没有可转移的调整。, Transfer all masks in
  this node 转移此节点中的所有蒙版.
Explicitly unimplemented items: none for ZH3 scope.
Primary success call chain: all 24 target contexts finished -> catalog and
  placeholder tests pass -> lrelease builds zh_CN QM -> alcedo_main embeds
  /i18n/alcedo_main_zh_CN.qm (verified in generated
  .qt/rcc/alcedo_main_translations.qrc compiled into alcedo_main.exe) ->
  LanguageManager resolves zh-CN and loads the resource path ->
  QQmlEngine::retranslate flips the loaded Copy dialog in place ->
  persisted selection verified by LanguageManagerTest.
Primary failure and restore call chain: as planned; no final lupdate drift
  occurred (run 3 identical diff, 0 new, 1106 existing).
Build and test commands with exit codes:
  alcedo_main_lupdate (exit 0, run 3 stable - identical diff before/after)
  alcedo_main_lrelease + SimplifiedChineseCatalogTest +
    EditorNodesPanelQmlTest + AdjustmentTransferDialogQmlTest (exit 0)
  alcedo_main (exit 0; translations qrc compiled into the exe)
  ctest -N -R focused set: 116 discovered
  ctest --output-on-failure -R focused set: exit 0
Discovered / passed / failed / skipped counts: 116 / 115 / 0 / 1 disabled
  (pre-existing DISABLED_DeleteKeyRemovesSelectedGradeAndSelectsItsSuccessor)
  / 0. Both named ZH3 tests pass:
  SimplifiedChineseCatalogRetranslatesAdjustmentTransferPanesInPlace
  (copy mode: 源版本/节点 panes, 全选/清除/取消/复制调整, 关闭 accessible
  name, seven section names, 转移此节点中的所有蒙版 accessibleText, zh->en->
  zh flip in place on the same dialog object);
  SimplifiedChineseAdjustmentTransferLabelsRemainVisibleAtSupportedWidths
  (read-only paste mode: 粘贴调整/取消/关闭, 节点 pane title, hidden
  selection controls, 蒙版 section, empty-state 没有可转移的调整。 +
  要粘贴的参数, dialog stays inside the window at reduced width).
Verify-only modules: all nine (DateCommitGraph, DateFilterSection,
  EditorMonoSlider, EditorNodeEdgeDelegate, EditorNodePortDock,
  KeyboardSettingsPanel, LensCatalogPicker, RegisteredShortcut,
  ShortcutCaptureField) hold zero unfinished messages in the catalog.
Final catalog totals: 214 target messages across 24 contexts, 0 unfinished,
  0 empty; 22 unfinished remain outside the target contexts (expected
  out-of-scope set, unchanged).
Manual verification: unavailable (headless); the automated translator tests
  exercise the same real QML surfaces, accessibility text, and widths.
Evidence path: build/tmp/pr_93_176_zh_cn/zh3/ (build + ctest logs, lupdate
  run 3, final audit snapshot).
Remaining defects or unavailable platforms: manual GUI matrix not executed;
  macOS not run.
```

## 12. Cross-Phase Acceptance Matrix

| Behavior | Phase | Automated evidence | Manual evidence |
| --- | --- | --- | --- |
| `Mask Group` uses the approved wording | ZH1 | Exact catalog and QM assertions | Mask Groups title and actions |
| Controller Mask Group errors use the same wording | ZH1 | C++ translation-context assertion | Forced invalid group action |
| Empty, loading, and disabled group states use Chinese | ZH1 | Real-translator Mask Groups QML test | Real panel states |
| Nodes actions and accessibility use Chinese | ZH2 | Real-translator Nodes QML test | Keyboard and screen-reader review |
| Mask parameter labels use Chinese | ZH2 | Catalog and adjustment tests | Radial and Gradient selection |
| Detail and White Balance labels use Chinese | ZH2 | Catalog and header tests | Real adjustment panels |
| Shared rail and toolbar text use Chinese | ZH2 | Catalog context test | Live shell review |
| Long Chinese text remains visible | ZH2-ZH3 | Supported-width QML tests | Width and scale matrix |
| Adjustment Transfer panes use Chinese | ZH3 | Real-translator Transfer QML test | Copy and paste modes |
| Placeholders remain valid | ZH1-ZH3 | Generic placeholder multiset test | Values render in context |
| Live language change updates loaded UI | ZH1-ZH3 | Translator install and engine retranslate | English to Chinese switch |
| Language selection persists | ZH3 | Existing `LanguageManagerTest` | Application restart |
| Packaged catalog is present | ZH3 | App build and QM load | Installed app launch |
| Verify-only new modules remain correct | ZH3 | Final extraction review | Targeted product visit |
| Unrelated unfinished entries stay outside scope | ZH3 | Final catalog count report | Not applicable |

## 13. Build and Evidence Rules

Use the `win_debug` preset for focused automated verification.
Use the repository MSVC wrapper for configure and build.

Confirm test discovery before each run.
Record the discovered, passed, failed, disabled, and skipped counts.

Keep build and test logs under:

```text
build/tmp/pr_93_176_zh_cn/zh1/
build/tmp/pr_93_176_zh_cn/zh2/
build/tmp/pr_93_176_zh_cn/zh3/
```

Do not create logs or scripts at the repository root.
Do not commit build output or compiled QM files.

For every `lupdate` run:

1. Review new and removed source keys.
2. Review context changes.
3. Review location-only churn separately.
4. Confirm that no finished target translation returned to unfinished state.
5. Run `lupdate` a second time after edits.
6. Confirm that the second run is stable.

For every `lrelease` run:

1. Record the command exit code.
2. Record the Chinese finished and unfinished counts.
3. Confirm that target contexts have zero unfinished entries.
4. Report the 22 known out-of-scope unfinished entries.

Manual checks supplement automated checks.
They do not replace catalog, placeholder, compile, or load evidence.

## 14. Risks and Stop Conditions

| Risk | Detection signal | Required response |
| --- | --- | --- |
| PR range inventory drift | Context or message count differs | Stop and update the source audit before translation |
| Incorrect Mask Group wording | Exact terminology test fails | Correct every affected direct and compound translation |
| Placeholder loss | Placeholder multiset test fails | Restore the exact placeholder indexes and counts |
| Same English word has another meaning | Existing translation reads incorrectly in context | Write a context-correct Chinese translation |
| `lupdate` creates a large unrelated diff | TS diff contains unrelated source or location churn | Isolate the cause and keep only reproducible extraction changes |
| Chinese copy clips | Width or manual check hides text or actions | Correct the owning layout with existing theme rules |
| Accessibility diverges from visible text | Accessible-name assertion differs | Use the same approved terminology in both roles |
| QM file is stale | TS passes but compiled lookup returns source English | Rebuild `alcedo_main_lrelease` and fix target dependency |
| Resource is missing from the app | File load passes but resource load fails | Fix translation resource registration before completion |
| English catalog convention changes | English TS receives manual translated values | Restore the existing source-language convention |
| Phase diff can exceed 2,000 lines | Review estimate or actual diff approaches the limit | Split the phase before more implementation |

Stop implementation when the current source no longer matches the audited PR boundary.
Update this plan before implementation continues.

Stop implementation when a required UI string cannot enter `lupdate` through a literal source.
Fix the source extraction path before editing the TS file.

Stop implementation when a layout fix needs a new visual token.
Update `AppTheme` and `DESIGN.md` in the same phase before continuing.

## 15. Final Completion Record Template

```text
Plan / date / status:
Source revision and branch:
PR range baseline and end:
Actual changed modules:
Translated target contexts:
Range-added translations completed:
Same-context closure translations completed:
Final target message count:
Final target unfinished count:
Remaining out-of-scope unfinished count:
Approved Mask Group wording evidence:
Primary success call chain:
Primary failure and restore call chain:
Build and test commands with exit codes:
Discovered / passed / failed / disabled / skipped counts:
Lupdate stability result:
Lrelease and QM load result:
Manual language, layout, theme, focus, and accessibility verification:
Windows result:
macOS result or unavailable reason:
Evidence path:
Remaining defects:
```

Filled record:

```text
Plan / date / status: PR93-176 Simplified Chinese UI Translation /
  2026-09-30 / complete (ZH1-ZH3 automated evidence green; manual GUI
  matrix unavailable in this headless environment, recorded as unavailable)
Source revision and branch: 81e40784 on feature/zh-cn-ui-translation-pr93-176
PR range baseline and end: aab5e1e6 (after PR #92) .. 6049f8ef (PR #176)
Actual changed modules:
  - alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_zh_CN.ts (164
    translations filled; six obsolete EditorGeometryPanel keys removed by
    lupdate)
  - alcedo_studio/src/ui/alcedo_main/i18n/alcedo_main_en.ts (same lupdate
    extraction refresh; no manual English values added)
  - alcedo_studio/tests/ui/simplified_chinese_catalog_test.cpp (new, 311
    lines)
  - alcedo_studio/tests/ui/editor_nodes_panel_qml_test.cpp (+190 lines:
    ScopedZhCnCatalog helper and three zh_CN tests)
  - alcedo_studio/tests/ui/editor_adjustment_transfer_dialog_qml_test.cpp
    (+218 lines: AccessibleNameOf helper and two zh_CN tests)
  - alcedo_studio/tests/ui/CMakeLists.txt (+30 lines: new target, defines,
    lrelease dependencies)
  No production QML, C++, command IDs, node IDs, mask IDs, panel keys, or
  serialized data changed. No layout correction was needed.
Translated target contexts: all 24 audited contexts.
Range-added translations completed: 164 (58 ZH1 + 85 ZH2 + 21 ZH3 at audit
  time; all landed in one catalog pass on 2026-09-30).
Same-context closure translations completed: included in the 164 (six older
  unfinished messages in ZH2 contexts per Section 10.1).
Final target message count: 214 (audit-time 220 minus six obsolete
  EditorGeometryPanel keys removed by lupdate; verified reproducible across
  three runs).
Final target unfinished count: 0 (also 0 empty).
Remaining out-of-scope unfinished count: 22, unchanged.
Approved Mask Group wording evidence: catalog test asserts the exact
  compositions from Section 3.2; the live Mask Groups page shows
  图层（蒙版组） / 添加图层（蒙版组） / 暂无图层（蒙版组） under the compiled
  catalog;
  the accessible-phrase sweep rejects 蒙版组 outside the approved phrase and
  rejects 遮罩, " · ", and " | ".
Primary success call chain: qsTr()/tr() -> lupdate -> zh_CN.ts finished ->
  lrelease -> zh_CN.qm -> :/i18n/alcedo_main_zh_CN.qm in alcedo_main.exe ->
  LanguageManager zh-CN -> QTranslator + QQmlEngine::retranslate ->
  Chinese visible/accessibility text on every target surface.
Primary failure and restore call chain: unfinished/empty/placeholder-invalid
  entry -> SimplifiedChineseCatalogTest failure naming context and source ->
  TS corrected -> lrelease + focused tests rerun.
Build and test commands with exit codes:
  msvc_env.cmd --build --preset win_debug --parallel 4 --target
    alcedo_main_lupdate    exit 0 (three runs; runs 2-3 identical diff)
  ... --target alcedo_main_lrelease SimplifiedChineseCatalogTest
    EditorNodesPanelQmlTest AdjustmentTransferDialogQmlTest    exit 0
  ... --target AdjustmentTransferDialogQmlTest    exit 0 (rebuild after
    rename)
  ... --target alcedo_main    exit 0 (translations qrc compiled into exe)
  ctest --test-dir build/debug -N -R "^(SimplifiedChineseCatalogTest|
    LanguageManagerTest|EditorNodesPanelQmlTest|EditorAdjustmentHeaderQmlTest|
    AdjustmentTransferDialogQmlTest)\."    116 discovered
  ctest --test-dir build/debug --output-on-failure -R <same>    exit 0
Discovered / passed / failed / disabled / skipped counts: 116 / 115 / 0 / 1
  (pre-existing DISABLED_ DeleteKeyRemovesSelectedGradeAndSelectsItsSuccessor)
  / 0. One interim failure round: three new tests failed on first run
  (invokeMethod Q_RETURN_ARG(QString) mismatch on QML QVariant methods; mask
  type-row lookup needed QTRY wait) - fixed and all green on the recorded
  final run.
Lupdate stability result: run 3 produced an identical diff to the pre-run
  state (0 new, 1106 existing); extraction is idempotent.
Lrelease and QM load result: alcedo_main_zh_CN.qm regenerated; catalog test
  loads it with QTranslator and resolves QML and C++ contexts;
  :/i18n/alcedo_main_zh_CN.qm confirmed inside the generated
  alcedo_main_translations.qrc compiled into alcedo_main.exe.
Manual language, layout, theme, focus, and accessibility verification:
  unavailable (headless CI environment). Automated substitutes: real-
  translator QML tests on Mask Groups, Nodes, and the Transfer dialog cover
  visible text, Accessible.name/description, supported widths (260/320/460 px
  panels, narrowed dialog window), and zh->en->zh in-place retranslation.
  LanguageManagerTest covers zh-CN resolution and persisted selection.
Windows result: all automated evidence green on win_debug (MSVC + Qt 6.9.3).
macOS result or unavailable reason: unavailable - no macOS host in this
  environment; not run and not claimed.
Evidence path: build/tmp/pr_93_176_zh_cn/{zh1,zh2,zh3}/ including lupdate
  logs, build logs, ctest logs, and audit snapshots.
Remaining defects: none known. The 22 out-of-scope unfinished catalog entries
  remain by design. Interactive manual verification (real image session, both
  themes, 960x640 window, 100%/125% text scale, DPR variants, keyboard-only
  walk, screen reader) is still pending for a workstation pass.
```

Keep historical failures and unavailable checks in the record.
Do not remove them when a later run passes.
