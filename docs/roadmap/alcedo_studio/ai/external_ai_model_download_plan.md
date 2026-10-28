# AI Models Page: One Download Place for CLIP and AI Mask Models - Phase Plan

Date: 2026-10-27. Status: AM1 complete (2026-10-27); AM2 complete, v1 r1 published
(2026-10-28); AM3 complete (2026-10-28); AM4 complete (2026-10-28); AM5 complete (2026-10-28).

Source audit: `main` `56df8883`.
Tracking: [#579](https://github.com/zidage/AlcedoStudio/issues/579).
Replaces: SM2 (segmentation model download) of the
[AI Segmentation Mask Master Plan](../edit/ai_segmentation_mask_master_plan.md), section 9.1.
Reuses: [LUT package system](../../../lut-package-system.md) and the
[update system](../../../update-system.md).

## 1. Required result

Settings has one **AI Models** page. It is the only place where the user downloads, updates,
removes, and activates external AI models. The page uses the layout, the components, and the icons
of the LUTs page (`LutSettingsPanel.qml`).

- **AI Mask** has one card. The card installs the complete AI Mask model set (SAM 2.1 tiny encoder
  and decoder, people and sky head, S3OD, sky head) in one download. There is no download per
  function (Subject, Sky, People). The files come from the Alcedo Cloudflare R2 bucket through a
  signed feed, as LUT packages and application updates do.
- The card shows the earliest AI Mask model set version that this build supports (now v1). A model
  set version change means that the inference code must change too; the app installs only a
  version that it supports.
- **CLIP** (Local Content Recognition) has one card per model profile of the platform. Install and
  activation are separate: a card downloads or removes its model; an installed model is activated
  for the open project on its card. The active model of the open project has a green border. The
  current model section with the combo box, the four buttons, and the status card is removed.
- CLIP models still download from Hugging Face. A combo box on the page selects the source
  (HF Mirror, Hugging Face, Sufy CDN, Custom).
- Before the first download of any model, the user accepts a download statement (section 4). Each
  card shows the license names of its models.
- When the AI Mask tool opens and no supported model set is installed, the tool shows that state
  and a button that opens the AI Models page. The tool does not download.

## 2. Current source facts

Paths are relative to `alcedo_studio/src/`.

### 2.1 CLIP models

- `app/model_asset_catalog.cpp:19-168` (`BuildProfiles`) and Rust
  `rust/puerh_mind/src/service/model_assets.rs:401-458` (`MODEL_PROFILES`) hold the four profiles,
  kept in agreement by hand. Per asset: `repo_id`, `revision`, `remote_path`, `size_bytes`,
  optional `sha256`. There is no license field.
- Platform filter: ONNX profiles on Windows, the SigLIP2 Core ML profile on macOS only
  (`model_asset_catalog.cpp:228-260`).
- Licenses (Hugging Face model cards, 2026-10-27): `plhery/mobileclip2-onnx` (default on Windows)
  `apple-amlr`; `jinaai/jina-clip-v2` `cc-by-nc-4.0`; SigLIP2 Apache-2.0 (the two repositories
  that Alcedo uses have no license tag; the upstream Google SigLIP2 weights are Apache-2.0).
  The app shows none of this today.
- Download: `ModelDownloadService` (`app/model_download_service.cpp:119-182`) on the shared
  `DownloadService` (aria2c, one job at a time). URL
  `{endpoint}/{repo_id}/resolve/{revision}/{remote_path}` (`model_asset_catalog.cpp:284-290`).
  Staging folder `<base>/.<profileId>.download`, promoted by rename, then
  `alcedo_model_manifest.json` is written (`model_asset_catalog.cpp:339-404`).
- Endpoint presets `mirror` (default), `huggingface`, `sufy`, `custom`
  (`ui/alcedo_main/album_backend/model_download_controller.cpp:364-385`). The same preset logic is
  copied in `app/project_service.cpp:39-84`.
- QSettings: `semantic/modelProfileId` (app-wide selected profile), `semantic/modelDirectory`
  (default `<appDir>/model`), `semantic/modelEndpointPreset`, `semantic/customModelEndpoint`
  (`model_download_controller.cpp:39-47`).
- Activation: `SemanticGenerationController::ActivateSelectedModel`
  (`semantic_generation_controller.cpp:386-580`) needs an open project: it registers the model in
  the project table `SemanticModel` (`storage/store/database.hpp:97-110`), computes the label
  prototypes when they are cold, and sets the project's active model key. "Active" is per project;
  "selected" is app-wide. `TryAutoActivateSelectedModel` (`:325-384`) activates the selected
  profile without a button when it is warm.
- The sidecar identity: `RuntimeOptionsForCurrentSidecarSnapshot` (`:297-306`) uses the active
  profile, else the selected profile. The AI Mask tool and image analysis use this function. The
  sidecar requires `--model-id` (`rust/puerh_mind/src/config.rs:175-176`).
- UI: `qml/SemanticGenerationSettingsPanel.qml:511-723` (Model section) and `ModelStatusCard`
  (`:855-1014`). The Activate button is enabled when the model is not installed; errors appear only
  in the status text. Delete has no confirmation.
- Tests: no test covers `ModelDownloadService`, `ModelDownloadController`,
  `ActivateSelectedModel`, or the panel.

### 2.2 AI Mask models

- No download exists. A developer copies the files into `<semantic/modelDirectory>/alcedo-ai-mask-v1`
  (`ui/alcedo_main/album_backend/application_module_host.cpp:400-404`); master plan D4 and D13.
- File set: `rust/puerh_mind/src/service/segmentation/model_set.rs:6,36,67-139`.
  Windows: `point_encoder.onnx`, `point_decoder.onnx`, `people_sky_head.onnx`,
  `people_sky_head.json`, `subject_masks.onnx`, `sky_head.onnx` (356,815,751 bytes in the developer
  copy). macOS: the same roles as `.mlmodelc` directories (`s3od.mlmodelc` for S3OD) and
  `people_sky_head.json`. Sizes and SHA-256: master plan section 8.2.1.
- `MODEL_SET_ID = "alcedo-ai-mask-v1"` is a constant; nothing checks a version.
  `AiSegmentationMaskSource::model_set_id` stores it in each Mask (`include/edit/mask/mask_model.hpp:118`).
- A missing model fails at load: "The AI Mask models could not be loaded from <dir>"
  (`ui/alcedo_main/album_backend/editor_ai_mask_tool.cpp:321-324`).
- Sky notice: `qml/EditorAiMaskPanel.qml:268-278` says "Do not use Sky for commercial work."
  (lab decision U18). Section 3 replaces this wording.

### 2.3 LUT and update distribution

- `LutPackageService` (`app/lut_package_service.cpp`): signed feed
  `https://static.aoraw.org/luts/v1/manifest.json` + `.sig` (Ed25519, the update key,
  `app/detached_signature.cpp`), `schema`, `kind`, `sequence` with the QSettings rollback key,
  one `.7z` artifact per package, libarchive extraction with path checks, inventory verification,
  atomic `installed.json` receipt (`app/lut_package_install.cpp`).
- `UpdateService` has its own copy of the small-file fetch (`app/update_service.cpp:218-255`; LUT
  `lut_package_service.cpp:399-431`).
- Publishing: `scripts/luts/prepare_lut_packages.py`, `scripts/luts/publish_lut_packages.py`,
  `scripts/update/publish_update.py` (`publish_object`: immutable keys, `alcedo-sha256` metadata,
  live manifest last). Operator-run; the private key is offline; no CI upload.
- UI: `qml/LutSettingsPanel.qml` with the inline components `BentoTile`, `Pill`, `PackageCard`,
  shared `ThemedProgressBar`, `AlertBadge`, `DialogActionButton`, `IconButton`,
  `FolderPathField`. `DESIGN.md:90-144` limits new pills, badges, status dots, and SVGs.

## 3. Decisions

User decisions of 2026-10-27:

| ID | Decision |
| --- | --- |
| A1 | One AI Models settings page for CLIP and AI Mask, in the style of the LUTs page, reusing its components and assets. |
| A2 | CLIP install and activation are separate. Activation is on the card. The active model has a green border. The current model interaction is removed. |
| A3 | CLIP activation stays per project. Without an open project, a card cannot activate; the border shows the active model of the open project. |
| A4 | CLIP models keep the Hugging Face download. A combo box selects the source. |
| A5 | AI Mask models come from R2 through a signed feed (LUT and update method). The app states the earliest supported AI Mask model set version: v1. |
| A6 | The AI Mask model set downloads as one package, not per function. |
| A7 | A download statement covers all external models. The user accepts it before the first download. The Sky paragraph does not forbid commercial use: the user decides, contacts the MaSS13K authors for commercial use, and Alcedo does not guarantee that its own trained models can be used commercially. This replaces the notice wording of lab decision U18. |
| A8 | When the AI Mask tool has no supported model set, it shows the state and opens the AI Models page. It does not download. This replaces master plan section 3.1 step 3 and the first two rows of section 3.3. |
| A9 | The statement closes lab decision U13 (S3OD training data lineage): S3OD is published, and its NOTICE names the training data lineage. |
| A10 | The own trained models (people and sky head, sky head) are published under GPL-3.0, the license of Alcedo Studio. |

Implementation decisions (this plan):

| ID | Decision |
| --- | --- |
| P1 | **Model set version and revision.** `model_set_version` (integer, now 1) changes only when the inference code must change (inputs, outputs, files, thresholds format). `revision` (`r1`, `r2`, ...) changes the weights in one version. The build declares `kAiMaskModelSetMinimumVersion = 1` and `kAiMaskModelSetMaximumVersion = 1` (C++), and Rust declares the same range. The card shows the minimum ("Supports AI Mask models v1 and later" while the maximum equals the minimum, then the range). |
| P2 | **Feed.** `https://static.aoraw.org/ai-models/v1/manifest.json` + `.sig`, CMake `ALCEDO_AI_MODEL_FEED_URL`, signed with the update key. `schema: 1`, `kind: "alcedo-ai-model-packages"`, `sequence` with QSettings `aiModels/packages/highestTrustedSequence` (only raised). A feed can list one entry per (`id`, `model_set_version`), so an old build still finds its version after a v2 is published. The app picks the highest listed version in its range. |
| P3 | **Package.** One `.7z` per platform (`windows-x86_64`, `macos-arm64`) per (`version`, `revision`). Root: `model_set.json`, `package-inventory.json` (path, size, SHA-256 of each file), the model files, `licenses/`, `NOTICE.txt`. Core ML `.mlmodelc` directories are inside the archive as directories; the inventory pins every file of them. |
| P4 | **Install location.** `<model directory>/alcedo-ai-mask-v<version>/`, the directory that the sidecar reads today. Staging in `<model directory>/.alcedo-ai-mask-v<version>.download/`, verified against the inventory, then promoted by rename; `installed.json` (revision, inventory SHA-256, artifact SHA-256) is the commit point. A failed or cancelled install leaves the installed set unchanged. |
| P5 | **Status.** Not installed, Current, Update available (newer revision or higher supported version), Update required (installed version below the minimum), Needs a newer Alcedo Studio (the feed lists only versions above the maximum and no supported set is installed), Repair (inventory mismatch), Downloading, Verifying, Installing, Removing, Error. Comparison reads the receipt and the feed; it hashes no files (LUT rule). |
| P6 | **Feed check time.** When the AI Models page opens (LUT rule). Never at startup. |
| P7 | **Model directory.** One directory setting for all AI models on the page. The QSettings key stays `semantic/modelDirectory` (persisted setting). Changing it does not move files (current rule). |
| P8 | **Statement record.** QSettings `aiModels/downloadStatementAcceptedVersion` (integer). The statement has version 1. A download starts only when the accepted version is at least the current version; else the statement dialog opens first. A text change that changes the meaning raises the version. |
| P9 | **One download at a time.** The shared `DownloadService` runs one job. A second download request shows "Another download is running" on its card (LUT rule). |
| P10 | **No selected CLIP profile.** The app-wide `semantic/modelProfileId` is retired; `TryAutoActivateSelectedModel` is removed. The sidecar identity is the active model of the open project. When the project has no active model, the sidecar starts without a semantic model: `--model-id` becomes optional in `alcedo_mind`, semantic RPCs then return `AI_ERROR_MODEL_NOT_READY`, and segmentation and image analysis run. |
| P11 | **Shared code.** The signed small-file fetch (manifest + `.sig`) moves from `LutPackageService` and `UpdateService` into one function that both and the AI model service use. The `.7z` verification and safe extraction of `lut_package_install.cpp` become a package-archive function used by LUT and AI model install. `PackageCard` moves out of `LutSettingsPanel.qml` into its own file used by both pages. |
| P12 | **Page position.** New category "AI Models" after "LUTs" (index 9, About moves to 10; `updatesCategory` and `lutCategory` stay). Icon: the existing `sparkles.svg` (no new SVG). Active border color: `appTheme.backgroundTaskFinishedColor` (green `#3FB950`) through a new theme property `activeModelBorderColor` that returns it. |
| P13 | **Licenses on cards.** The CLIP catalog (C++ and Rust) gains `license_name` and `license_url` per profile. The AI Mask feed entry lists `licenses` (name and file in the package). Cards show the names in the existing Pill row; the license link opens the URL or the installed file. |

## 4. Download statement (version 1)

English source text (the `.ts` gets the Chinese text by hand):

> **Before you download**
>
> The AI models on this page are not part of the Alcedo Studio installer. Alcedo Studio downloads
> them to this computer from third-party sites or from the Alcedo download server. Each model has
> its own license. The license names are on the model cards, and the AI Mask package contains the
> full license texts. You decide whether the models and their output can be used for your purpose,
> including commercial use.
>
> **AI Mask.** The Sky model and the people and sky locator are trained by Alcedo Studio. The Sky
> model is trained on the MaSS13K dataset (https://github.com/xiechenxi99/mass13k), which does not
> state a clear license. For commercial use of the Sky model or its output, contact the authors of
> the dataset. Alcedo Studio does not guarantee that the models it trains can be used for
> commercial work. The Subject model (S3OD) is trained on generated images and on labels from other
> datasets; see the NOTICE file of the package.
>
> Select "Accept and Download" to confirm that you have read this statement.

Chinese text:

> **下载前请阅读**
>
> 本页的 AI 模型不包含在 Alcedo Studio 安装包中，由 Alcedo Studio 从第三方网站或 Alcedo 下载服务器下载到本机。每个模型都受其自身许可证约束，许可证名称见各模型卡片，AI Mask 模型包内附有许可证全文。模型及其输出能否用于您的用途（包括商业用途），请您自行判断。
>
> **AI Mask。** 天空模型与人物/天空定位模型由 Alcedo Studio 训练。天空模型使用 MaSS13K 数据集（https://github.com/xiechenxi99/mass13k）训练，该数据集未明确声明许可证。如需将天空模型或其输出用于商业用途，请联系数据集作者。Alcedo Studio 不保证其训练的模型可用于商业活动。主体模型（S3OD）使用生成图像和来自其他数据集的标签训练，详见模型包内的 NOTICE 文件。
>
> 点击“同意并下载”即表示您已阅读本声明。

Editor Sky notice (replaces the U18 wording):

- English: "Sky: the license of the Sky model's training data is not clear. Decide yourself before
  commercial use; see Settings > AI Models."
- Chinese: "天空：天空模型训练数据的许可证不明确，商业使用前请自行判断，详见“设置 > AI 模型”。"

## 5. Phases

| Phase | Scope | Depends on |
| --- | --- | --- |
| AM1 | AI model package feed, install, status, statement record (C++) | - |
| AM2 | Publishing tools and the R2 publication of AI Mask v1 r1 | AM1 (feed schema and verifier) |
| AM3 | CLIP install per card and activation per project (C++ and Rust) | - |
| AM4 | AI Models settings page (QML) | AM1, AM3 |
| AM5 | AI Mask tool: installed model check, version check, Sky notice | AM1, AM4 |

AM1 and AM3 can run at the same time.

### 5.1 AM1: AI model package feed and install

Work:

- `AiModelPackageService` (`app/`), exposed as `appModules.aiModelPackages`: `enabled`,
  `checking`, `checked`, `lastError`, `packages` (rows as in `LutPackageService::packages`, plus
  `modelSetVersion`, `installedModelSetVersion`, `minimumSupportedVersion`,
  `maximumSupportedVersion`, `licenses`), `downloadStatementAccepted`;
  `checkPackages()`, `installPackage(id)`, `cancelInstall(id)`, `removePackage(id)`,
  `acceptDownloadStatement()`.
- Manifest verification `VerifyAiModelPackageManifest`: signature over the exact bytes, schema,
  kind, sequence, unique (`id`, `model_set_version`), HTTPS artifact on the feed host ending in
  `.7z`, size limits, SHA-256 format.
- Install as decision P4, through the package-archive function of P11, and with the
  `installed.json` commit. Interrupted activation recovers a committed receipt (LUT rule).
- `AiMaskModelInstallation` query (used by AM5): installed version, revision, and whether it is in
  the supported range; reads `installed.json` and `model_set.json`.
- Shared signed small-file fetch (P11) used by LUT, update, and AI model services.
- Background task kind `ModelDownload` for the AI Mask download, cancelable (current CLIP rule).
- Update the master plan section 9.1: SM2 is replaced by this plan.

Required tests (fake aria2, signed test feeds):

- The manifest rejects a changed byte, an older sequence, a foreign host, a non-`.7z` artifact,
  and a duplicate (`id`, `model_set_version`).
- The service picks the highest supported version and ignores versions outside the range.
- Status for each case of P5, including Update required and Needs a newer Alcedo Studio.
- An archive hash mismatch, a path traversal entry, and a cancel keep the installed set.
- Install does not start before the statement is accepted.
- Settings open checks once; startup does not check.
- AI model and application downloads share the one-job admission.

#### AM1 completion record (2026-10-27)

**Status:** complete. `AiModelPackageService` checks the signed feed, installs, updates, repairs,
and removes the AI Mask model set; the shared fetch and archive code serve LUT, update, and AI
model packages. The AI Models page (AM4) and the AI Mask tool check (AM5) are not part of AM1.

Details the plan did not specify, chosen in AM1:

- **Feed entry.** `id`, optional `name`, `model_set_version`, `revision` (`r1`, `r2`, ...; the
  pattern `r[1-9][0-9]*`), `licenses` (at least one `{name, file}`; `file` is a safe
  package-relative path), and `artifacts`, an object keyed by platform (`windows-x86_64`,
  `macos-arm64`). Each artifact has `url`, `size`, `sha256`, `file_count`, `unpacked_bytes`, and
  `inventory_sha256` (SHA-256 of the exact `package-inventory.json` bytes of that archive). AM2
  writes this format.
- **Package files.** `package-inventory.json`: `schema: 1`, `kind:
  "alcedo-ai-model-package-inventory"`, `id`, `model_set_version`, `revision`, `platform`,
  `files` (`path`, `size`, `sha256` of every other file in the archive). `model_set.json`:
  `schema: 1`, `kind: "alcedo-ai-model-set"`, `id`, `model_set_version`, `revision`; other
  fields are allowed for AM5. `installed.json` (written by the app, never in an archive): `kind:
  "alcedo-ai-model-package-receipt"` with the signed entry and artifact fields and the feed
  sequence.
- **Commit point (refines P4).** On Windows a rename cannot replace a directory. So the installer
  writes `installed.json` into the verified staging content, moves the installed directory to
  `.alcedo-ai-mask-v<N>.previous`, and moves the content into `alcedo-ai-mask-v<N>`. That second
  move is the commit point. Recovery (`RecoverAiModelPackageDirectory`, run before every install
  and removal and when Settings opens) restores `.previous` when the target is missing, removes it
  when the target exists, and removes uncommitted staging content and `.removing` leftovers. The
  downloaded archive stays in `.alcedo-ai-mask-v<N>.download/` after a cancellation (aria2 resumes
  it). It is removed after a rejection or a commit.
- **Repair (P5).** An installed version needs repair when `installed.json`, `model_set.json`, and
  `package-inventory.json` do not agree, or when a listed file is missing or has another size.
  Only file sizes are read; no file is hashed.
- **Installed versions.** Each version has its own directory, so the query reads every
  `alcedo-ai-mask-v<N>` directory. It reports the highest version in the supported range, the
  highest below it (Update required), and the highest above it (ignored).
- **Feed check time.** Until AM4 adds the page, the check runs when Settings opens, beside the LUT
  check (`SettingDialog.qml`). AM4 moves it to the opening of the AI Models category.
- **Statement.** `InstallPackage` returns false and emits `DownloadStatementRequired(id)` before
  the statement is accepted; AM4 opens the dialog on that signal or on the
  `downloadStatementAccepted` property.
- **Shared code (P11).** `FetchFeedFile` and `FetchSignedFeed` (`app/signed_feed_fetch`) replace
  the copies in `UpdateService` and `LutPackageService`; each service keeps its own error texts.
  `ExtractPackageArchive`, `VerifyPackageArchiveFile`, and `VerifyExtractedPackageFiles`
  (`app/package_archive`) serve `ExtractLutPackageArchive` and the AI model installer. The LUT
  archive transfer port moved out of the LUT service as `PackageArchiveDownloader` and
  `DownloadServicePackageArchiveDownloader` (`app/package_archive_download`), so both package
  services share it.
- **Background task.** `AiModelDownloadTaskPublisher` registers a cancelable `ModelDownload` task
  per install. The task holds the global `ChangeModelDownloadSettings` lock, so the model
  directory does not change during an install.
- **Model directory change.** The host calls `RefreshInstallations()` when
  `ModelDownloadController::ModelDownloadDirectory()` changes.
- **Not in AM1.** The Rust model set range and the `model_set.json` check in the sidecar are AM5
  work (section 5.5). The service texts get their Chinese entries with the page in AM4. The AI
  Mask tool still reads `alcedo-ai-mask-v1` directly until AM5.

**Primary success call chain:**

```text
Settings opens (SettingDialog.qml onVisibleChanged)
  -> AiModelPackageService::CheckPackages
     -> RecoverAiModelPackageDirectory + ReadAiModelInstallation (receipts, sizes)
     -> FetchSignedFeed -> FetchFeedFile (manifest.json, then manifest.json.sig, feed host only)
     -> VerifyAiModelPackageManifest (signature, schema, sequence, unique id+version, host, .7z)
     -> CompareAiModelPackage (SelectAiModelPackage: highest version in [1, 1] for the platform)
  -> packages row: status notInstalled / current / updateAvailable / ..., action install/update
installPackage(id) (statement accepted)
  -> DownloadServicePackageArchiveDownloader -> DownloadService (one job; size + SHA-256)
  -> AiModelDownloadTaskPublisher registers the ModelDownload background task
  -> worker thread: InstallAiModelPackageArchive
     -> VerifyPackageArchiveFile -> RecoverAiModelPackageDirectory
     -> ExtractPackageArchive (safe entries, signed limits) into .alcedo-ai-mask-v1.download/content
     -> inventory digest, inventory vs signed fields and files, model_set.json, license files
     -> installed.json -> move old set aside -> move content into alcedo-ai-mask-v1 (commit)
  -> FinishInstall -> RefreshInstallations -> status current; InstallFinished -> task succeeded
```

**Primary failure call chains:**

```text
Changed manifest byte / older sequence / foreign host / non-.7z / duplicate id+version
  -> VerifyAiModelPackageManifest fails -> lastError set, previous feed state kept, no download
Archive hash mismatch / unsafe or extra entry / inventory mismatch
  -> InstallAiModelPackageArchive removes the staging content and the archive
  -> installed set unchanged, status error with the reason, action retry
Cancel (download or before the commit)
  -> downloader Cancel or stop flag -> staging content removed, archive kept, installed set unchanged
  -> InstallFinished(canceled) -> background task canceled
Statement not accepted -> InstallPackage returns false, DownloadStatementRequired, nothing starts
Another transfer holds DownloadService -> InstallPackage returns false, "Another download is running"
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| Manifest rejects a changed byte | `AiModelPackageManifestTest.ChangedManifestByteFailsSignature` | PASS |
| Manifest rejects an older sequence | `AiModelPackageManifestTest.OlderSequenceThanTrustedIsRejected` | PASS |
| Manifest rejects a foreign host | `AiModelPackageManifestTest.ArchiveOnForeignHostIsRejected` | PASS |
| Manifest rejects a non-`.7z` artifact | `AiModelPackageManifestTest.ArchiveThatIsNot7zOrNotHttpsIsRejected` | PASS |
| Manifest rejects a duplicate (`id`, `model_set_version`) | `AiModelPackageManifestTest.DuplicateIdAndModelSetVersionIsRejected` | PASS |
| Highest supported version chosen, others ignored | `AiModelPackageManifestTest.SelectionPicksHighestSupportedVersionForPlatform`, `AiModelPackageServiceTest.InstallsHighestSupportedVersionIntoVersionDirectory` | PASS |
| Status: Not installed, Current | `AiModelPackageServiceTest.CheckRequestsManifestAndSignatureOnceWithoutDownloading`, `...InstallsHighestSupportedVersionIntoVersionDirectory` | PASS |
| Status: Update available | `AiModelPackageServiceTest.NewerRevisionIsUpdateAvailableAndUpdateReplacesInstalledSet` | PASS |
| Status: Update required | `AiModelPackageServiceTest.InstalledVersionBelowMinimumIsUpdateRequired` | PASS |
| Status: Needs a newer Alcedo Studio | `AiModelPackageServiceTest.FeedWithOnlyHigherVersionsNeedsNewerApplication` | PASS |
| Status: Repair | `AiModelPackageServiceTest.MissingOrResizedFileIsRepairWithoutHashing` | PASS |
| Status: Downloading, Verifying, Installing | `AiModelPackageServiceTest.InstallReportsDownloadVerifyAndInstallStages` | PASS |
| Status: Removing | `AiModelPackageServiceTest.RemoveDeletesInstalledSetWithoutFeedCheck` | PASS |
| Status: Error | `AiModelPackageServiceTest.ArchiveHashMismatchKeepsInstalledSet` | PASS |
| Hash mismatch keeps the installed set | `AiModelPackageServiceTest.ArchiveHashMismatchKeepsInstalledSet` | PASS |
| Path traversal entry keeps the installed set | `AiModelPackageServiceTest.PathTraversalEntryKeepsInstalledSet` | PASS |
| Extra or missing file is rejected | `AiModelPackageServiceTest.ExtraUnlistedOrMissingFileIsRejected` | PASS |
| Cancel keeps the installed set | `AiModelPackageServiceTest.CanceledDownloadKeepsInstalledSet`, `...CanceledInstallKeepsInstalledSetAndDownloadedArchive` | PASS |
| Interrupted replacement recovers | `AiModelPackageServiceTest.InterruptedReplacementRestoresInstalledSetOnCheck` | PASS |
| No install before the statement is accepted | `AiModelPackageServiceTest.InstallBeforeStatementAcceptedStartsNothing` | PASS |
| Settings open checks once; startup does not | `AiModelPackageServiceTest.StartupReadsLocalInstallationWithoutRequestingFeed`, `...CheckRequestsManifestAndSignatureOnceWithoutDownloading` | PASS |
| Tampered feed keeps the state | `AiModelPackageServiceTest.TamperedOrOlderFeedKeepsStateAndReportsError` | PASS |
| Background task kind `ModelDownload`, cancelable | `AiModelPackageServiceTest.DownloadIsCancelableModelDownloadBackgroundTask` | PASS |
| AI model and application downloads share the one-job admission | `AiModelPackageServiceTest.ApplicationAndAiModelDownloadsShareAdmission` | PASS |
| Shared fetch and archive code keep LUT and download behavior | `LutPackageServiceTest`, `LutPackageManifestTest`, `LutLibraryServiceTest`, `DownloadServiceTest` | PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AiModelPackageManifestTest
AiModelPackageServiceTest LutPackageServiceTest LutPackageManifestTest LutLibraryServiceTest
DownloadServiceTest alcedo_main`; `ctest --test-dir build/debug -R
"^(AiModelPackageManifestTest|AiModelPackageServiceTest|LutPackageManifestTest|LutPackageServiceTest|LutLibraryServiceTest|DownloadServiceTest)\."`.

**Residual gaps:** the Settings-open check is in QML and has no QML test; the C++ owner tests
cover the service rule. No live feed exists until AM2 publishes v1 r1. `UpdateService` has no
unit test; its move to `FetchFeedFile` is verified by the build only. The network path of
`FetchFeedFile` runs in the application, not in a test (the tests inject the fetch).

### 5.2 AM2: publishing and R2 publication

Work:

- `scripts/ai_models/prepare_ai_model_packages.py`: reads a Windows source directory (ONNX +
  JSON) and a macOS source directory (`.mlmodelc` + JSON), checks every file against the pinned
  sizes and SHA-256 of master plan section 8.2.1 (Core ML by the directory rule), writes
  `model_set.json`, `package-inventory.json`, `licenses/` (Apache-2.0 for SAM 2.1, MIT and the
  DINOv3 License for S3OD, GPL-3.0 for the own models), and `NOTICE.txt` (source, license, and
  training data of each model, the MaSS13K and S3OD lineage, the statement text of section 4),
  builds the two `.7z` archives, and signs the manifest with `alcedo_update_signer`.
- `scripts/ai_models/publish_ai_model_packages.py`: plan-only unless `--upload`; keys
  `ai-models/v1/packages/alcedo-ai-mask/v<version>/<revision>/alcedo-ai-mask-v<version>-<revision>-<platform>.7z`,
  `ai-models/v1/manifests/<sequence>/manifest.json[.sig]`, then the live `.sig` and the live
  `manifest.json` last. Reuses `publish_object` of `scripts/update/publish_update.py`.
- Publish v1 r1 for both platforms. The upload is an operator action with the user's R2
  credentials; the agent asks before it uploads.
- Add the model license texts to `third_party_licenses/` and `THIRD_PARTY_NOTICE.txt`.

Required tests (Python): the prepared archives verify from local files; a changed byte fails;
the publication plan lists the live manifest last; the C++ AM1 verifier accepts the Python
fixture manifest (shared fixture as in the LUT tests).

#### AM2 completion record (2026-10-28)

**Status:** complete. The prepare and publish tools build, sign, verify, and upload the AI Mask
model set v1 r1 for `windows-x86_64` and `macos-arm64`; feed sequence `20261028041321` is live at
`https://static.aoraw.org/ai-models/v1/manifest.json`.

Details the plan did not specify, chosen in AM2:

- **Model set definition.** `scripts/ai_models/ai_mask_model_set_v1.json` holds the package ID,
  name, version, revision, the license list (feed name, package path, source file, pinned
  SHA-256), the models by role with their file per platform, and the pinned files per platform.
  The tools read only this file; a new revision is a new definition.
- **Core ML pins (refines "Core ML by the directory rule").** Master plan section 8.2.1 pinned the
  SM1 compiles by directory SHA-256, but `coremldata.bin` changes at every compile, and the SM1
  compiles no longer exist (the Mac copy was converted and compiled again from the same
  checkpoints on 2026-10-26). AM2 pins the published copy file by file (path, size, SHA-256 of
  all four files of each `.mlmodelc`) and also checks its directory SHA-256 by the lab rule. The
  byte totals equal section 8.2.1; the sky head copy equals section 8.2.1 exactly (directory,
  `model.mil`, and `weights/weight.bin`). Section 8.2.1 now lists the published directory SHA-256
  next to the SM1 value. The Windows files equal section 8.2.1.
- **Package content.** Root files `model_set.json`, `package-inventory.json`, `NOTICE.txt`,
  `people_sky_head.json`, the model files, and `licenses/` with `SAM2-Apache-2.0.txt`,
  `S3OD-MIT.txt`, `DINOv3-License.md`, and `GPL-3.0.txt` (the root `LICENSE` text without the
  appended NVIDIA additional permission). `model_set.json` also names the `platform` and the
  `models` (role, path, licenses) for AM5. The feed `licenses` names are `Apache-2.0`, `MIT`,
  `DINOv3 License`, and `GPL-3.0`. `NOTICE.txt` is
  `scripts/ai_models/ai_mask_model_set_v1_NOTICE.txt`: source, license, changes, and training
  data of each model, the S3OD lineage (FLUX.1 Krea [dev] images, teacher labels from DIS5K,
  HR-SOD, UHRSD, DUTS), the MaSS13K lineage, and the statement of section 4 in English and
  Chinese (A7, A9, A10).
- **Reproducible archives.** The files are checked, copied into a staging directory, checked
  again, and get one fixed modification time, so the same input on the same OS gives the same
  archive bytes (py7zr writes OS file attributes, so archive bytes differ between Windows and
  macOS).
- **Shared fixture.** `alcedo_studio/tests/resources/ai_model_packages/` holds a feed and two
  archives that the Python code prepares from the real definition with small model files and
  short license texts, signed with the test key of `AiModelPackageServiceTest` (`KeyPair()`).
  Regenerate with `python scripts/ai_models/tests/ai_model_test_support.py --write-cpp-fixture`.
  The Python test compares the regenerated feed (without the archive size and SHA-256) and the
  archive content with the committed files.
- **Not in AM2.** The tools build one model set version per run. A later feed that must keep v1
  for old builds while it adds v2 (P2) needs a prepare run that lists both versions.

Operator steps (Windows; the private key stays offline):

```text
python scripts/ai_models/prepare_ai_model_packages.py
  --windows-source <dir with the ONNX files> --macos-source <dir with the .mlmodelc files>
  --private-key D:\secure\alcedo-update-private.seed
  --signer build/release/alcedo_studio/src/alcedo_update_signer.exe [--output-dir DIR]
python scripts/ai_models/publish_ai_model_packages.py --output-dir DIR            (plan only)
python scripts/ai_models/publish_ai_model_packages.py --output-dir DIR --upload   (R2_* in .env.test)
```

**Primary success call chain:**

```text
prepare_ai_model_packages.main
  -> load_definition (pins, licenses, models; every model file pinned on each platform)
  -> build_package per platform
     -> stage_package: check_source_file (size + SHA-256; .mlmodelc file by file + directory rule)
        -> copy -> check again -> model_set.json, NOTICE.txt, licenses/ (pinned license texts)
     -> build_inventory -> write_package_archive (fixed file time, LZMA2 preset 9)
     -> verify_package_archive (archive hash, safe entries, inventory digest and fields, files,
        model_set.json, license files)
  -> manifest_bytes (unique id + version) -> alcedo_update_signer sign -> verify_signature
publish_ai_model_packages.main
  -> verify_prepared_output (signature over the exact bytes, every archive again)
  -> planned_uploads: archives, archived .sig + manifest, live .sig, live manifest last
  -> --upload: publish_object per item (immutable keys refuse a different object)
App (AM1): AiModelPackageService::CheckPackages -> VerifyAiModelPackageManifest -> install
```

**Primary failure call chains:**

```text
Changed, missing, or extra source file -> check_source_file raises -> exit 1, no manifest
Changed archive byte / model byte / inventory -> verify_package_archive raises -> nothing signed
Manifest and signature differ, or no .sig -> publish stops before any upload
Upload failure -> publish_object raises -> the live manifest is not replaced (it is last)
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| The prepared archives verify from local files | `AiModelPackagePreparationTest.test_PreparedArchivesVerifyFromLocalFiles` | PASS |
| A changed byte fails | `...test_ChangedSourceByteStopsPreparationWithoutManifest`, `...test_ChangedArchiveByteFailsVerification`, `...test_PublicationRejectsChangedArchive` | PASS |
| An extra or missing Core ML file fails | `...test_ExtraOrMissingCoreMlFileStopsPreparation` | PASS |
| The publication plan lists the live manifest last | `...test_SignerSignsFeedAndPublicationPlanListsLiveManifestLast` (real `alcedo_update_signer`) | PASS |
| Same input, same archive bytes | `...test_SameInputGivesSameArchiveBytes` | PASS |
| The C++ AM1 verifier accepts the Python fixture manifest | `AiModelPackageManifestTest.PythonPreparedFeedVerifiesInCpp`, `AiModelPackageCppFixtureTest.test_CppFixtureIsTheCurrentPythonPreparedFeed` | PASS |
| The C++ installer installs the Python archives (both platforms) | `AiModelPackageServiceTest.PythonPreparedPackageInstallsOnEachPlatform` | PASS |
| The definition pins the section 8.2.1 files and the license texts | `AiMaskModelSetDefinitionTest.test_DefinitionPinsTheAcceptedModelFilesAndLicenseTexts` | PASS |
| NOTICE names every model, license, lineage, and the statement; no commercial-use ban | `AiMaskModelSetDefinitionTest.test_NoticeNamesEveryModelLicenseAndTheStatement` | PASS |
| AM1 tests unchanged | `AiModelPackageManifestTest`, `AiModelPackageServiceTest` (29 tests) | PASS |

Commands: `python -m unittest discover -s scripts/ai_models/tests` (10 tests);
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target AiModelPackageManifestTest
AiModelPackageServiceTest`; `ctest --test-dir build/debug -R
"^(AiModelPackageManifestTest|AiModelPackageServiceTest)\."`.

Publication of v1 r1 (2026-10-28): sources `D:/Download/alcedo_models/alcedo-ai-mask-v1` (Windows)
and the Mac copy `~/Documents/AlcedoModels/alcedo-ai-mask-v1`. Windows: 12 files, 356,878,760
bytes, archive 319,577,246 bytes, SHA-256
`f1f4d2465c9541161cf7ab6622a14c7f126719d778b338ecbd7055bda549a9a3`. macOS: 27 files, 433,589,987
bytes, archive 390,518,274 bytes, SHA-256
`b333e1fa6d2e4b8771f49880683f4384279f98dd91004af6e749071e6d686616`. Signed with the update key
and verified against `update_public_key.txt`; uploaded with the user's approval. After the
upload, the public `manifest.json` and `.sig` were downloaded from `static.aoraw.org`: their bytes
equal the prepared files, the signature is valid, and both downloaded archives pass
`verify_package_archive`.

**Residual gaps:** the Python tool tests do not run in CI (as the LUT tool tests). The installed
app has not installed the live package yet; AM1 (#580) is not merged, and the AI Models page is
AM4.

### 5.3 AM3: CLIP install per card and activation per project

Work:

- Catalog: `license_name`, `license_url` per profile in C++ and Rust, with a test that the two
  catalogs agree.
- `ModelDownloadController`: replace the selected-profile surface with a row list `models`
  (`profileId`, `displayName`, `language`, `sizeBytes`, `licenseName`, `licenseUrl`, `installed`,
  `status`, `progress`, byte/speed/ETA labels, `busy`, `error`) and `installModel(id)`,
  `cancelInstall(id)`, `removeModel(id)`. Keep the source preset properties and
  `effectiveModelEndpoint`, and keep the copy in `project_service.cpp` in agreement (move both to
  one function).
- `SemanticGenerationController::activateModel(profileId)`: the current activation steps for the
  given profile; rejects a model that is not installed, an already active model, and no open
  project, with an error on that card. Remove `ActivateSelectedModel`,
  `TryAutoActivateSelectedModel`, `selectedModelActive`.
- Remove `semantic/modelProfileId` use (P10). Sidecar identity from the project's active model;
  `alcedo_mind --model-id` optional (Rust config and inference start).
- Removing a model that is active in the open project: the card asks for confirmation; after
  removal the project has no active model (the save purge rule of `project_handler.cpp:360-382`
  stays).

Required tests: install, cancel, remove per profile (fake aria2); activation of an installed
model sets the project active key; activation without a project, of a missing model, or during
another activation fails with the reason; the sidecar starts without `--model-id` and returns
`AI_ERROR_MODEL_NOT_READY` for a semantic request; the runtime options use the project's active
model.

#### AM3 completion record (2026-10-28)

**Status:** complete. Each CLIP profile of the platform has its own install, cancel, and remove;
an installed model is activated per project with `activateModel(profileId)`; the app-wide
selected profile is gone, and the sidecar starts without a semantic model when the open project
has no active model. The AI Models page is AM4.

Details the plan did not specify, chosen in AM3:

- **Row list.** `ModelDownloadController::models` has one map per profile: `profileId`,
  `displayName`, `modelId`, `language`, `inferenceBackend`, `imageSize`, `sizeBytes`,
  `sizeLabel`, `licenseName`, `licenseUrl`, `installed`, `status` (`notInstalled`, `installed`,
  `downloading`, `canceling`), `busy`, `progress`, `phase`, `currentFile`, `bytesLabel`,
  `speedLabel`, `etaLabel`, `filesDone`, `filesTotal`, `message`, and `error`. `message` and
  `error` hold the last result of that row (download, removal, or activation); an activation
  failure is the `error` of the card that asked for it. `installModel`, `cancelInstall`,
  `removeModel`, and `refreshModels` follow the AM1 naming (C++ `InstallModel` and so on, QML
  lower case). `downloadingProfileId` and `modelDownloadRunning` stay for the page.
- **Install state.** A profile is installed when every catalog file has its catalog size and
  the resolved manifest matches the catalog (the former selected-profile check, now for every
  row). No file is hashed outside a download.
- **One download at a time (P9).** A second install, or an install while the shared
  `DownloadService` runs another owner's job, returns false with "Another download is running."
  on that row. A profile cannot be removed while it downloads ("Cancel the download before you
  remove the model.").
- **Catalog injection.** `ModelDownloadService` takes its profile list (default
  `SemanticModelProfiles()`), and the controller reads profiles only through it, so the tests
  install small profiles through the real `DownloadService`. The test aria2 stand-in
  `DownloadServiceRpcFakeAria2` answers the aria2 JSON-RPC calls and copies files from a source
  directory (`ALCEDO_FAKE_ARIA2_HOLD=1` keeps a download open for the cancel test).
- **Licenses (P13).** `license_name` and `license_url` per profile in C++ and Rust:
  MobileCLIP2 `apple-amlr` (`https://huggingface.co/apple/MobileCLIP2-S2/blob/main/LICENSE`),
  jina-clip-v2 `cc-by-nc-4.0` (`https://creativecommons.org/licenses/by-nc/4.0/`), SigLIP2 ONNX
  and Core ML `Apache-2.0` (`https://www.apache.org/licenses/LICENSE-2.0`).
  `alcedo_studio/tests/resources/clip_model_catalog/clip_model_profiles.json` holds the complete
  catalog; the Rust test and the C++ test each compare their table with it, so the two catalogs
  agree. After a catalog change, write the file with `ALCEDO_WRITE_CLIP_CATALOG_FIXTURE=1 cargo
  test --lib profile_table_equals_the_shared_clip_catalog_file`.
- **Source presets.** `NormalizedModelEndpointPreset`, `ModelEndpointForPreset`,
  `ConfiguredSemanticModelDirectory`, and `EffectiveSemanticModelEndpoint`
  (`app/model_asset_catalog`) replace the copies in `ModelDownloadController` and
  `project_service.cpp`. The QSettings keys are unchanged.
- **Activation.** `activateModel` rejects, with the reason on the row: no open project, a
  model that is not installed, the active model ("This model is already active for this
  project."), another activation ("Another model activation is running."), and a running
  generation. `activatingModelProfileId` names the card that shows the activation progress. A
  running sidecar of another model (or of no model) restarts with the model being activated
  before the label prototypes are computed; when another operation holds the sidecar, the
  activation fails with the reason instead of computing prototypes with the wrong model.
- **Removing the active model.** `RemoveModel` emits `ModelRemoved`;
  `SemanticGenerationController` then calls the new `SemanticModelRegistry::ClearActiveModel`
  when the removed profile is the active model of the open project. The rows of the model stay
  until the save purge (`PurgeUninstalledSemanticModels`) drops them, as before. The interim
  panel asks before every removal and names the active-model case.
- **No model (P10).** `AiSidecarRuntimeOptions::model_id` is empty by default and
  `BuildArguments` omits `--model-id` when it is empty. `RuntimeOptionsForCurrentSidecarSnapshot`
  returns the options of the project's active model, else options without a model
  (`model_root` = model directory, `require_model_info` false). `alcedo_mind` without
  `--model-id` (or `ALCEDO_MIND_MODEL_ID`) uses an engine without a model: the v2 embedding RPCs
  return the header `AI_STATUS_PROVIDER_UNAVAILABLE` + `AI_ERROR_MODEL_NOT_READY` with the
  reason, `GetModelInfo` returns `FAILED_PRECONDITION`. The v2 RPCs also report a configured
  profile that fails to prepare this way. The C++ batch client now fails every item with the
  header error instead of returning an empty list. Generation without an active model stops with
  "Activate an AI model in Settings before generating labels." before the sidecar starts.
- **Interim panel.** `SemanticGenerationSettingsPanel.qml` shows one card per profile (status,
  license and size, progress, Download/Cancel, Activate, Delete with confirmation, active model
  with the green border) instead of the combo box, the status card, and the four buttons, so
  the app works until AM4 moves this to the AI Models page. The new texts get their Chinese
  entries with the page in AM4.
- **Terminology.** `SemanticGrpcDependencySmokeTest` is now `SemanticGrpcDependencyLinkTest`
  (touched `tests/app/CMakeLists.txt`). The `EditorRhiContracts` library and test in
  `tests/ui/CMakeLists.txt` are outside AM3: issue
  [#582](https://github.com/zidage/AlcedoStudio/issues/582).

**Primary success call chain:**

```text
installModel(profileId)
  -> ModelDownloadController::InstallModel (one download at a time, row status downloading)
  -> ModelDownloadService::StartDownload -> DownloadService (aria2c, size + SHA-256 per file)
  -> background task ModelDownload (cancelable, affected target = profile)
  -> Finished: ValidateDownloadedProfile -> PromoteStagingRoot -> WriteResolvedManifest
  -> row installed, message "Model download complete."
activateModel(profileId) (open project, installed, not active)
  -> SemanticGenerationController::ActivateModel -> LoadResolvedManifest (catalog sizes)
  -> worker: UpsertModel -> label prototypes cached? else sidecar with this model
     (RuntimeOptionsForProfile) -> EnsureLabelPrototypes -> SetActiveModelKey
  -> activeModelProfileId = profile; sidecar identity = this model
removeModel(profileId) -> files removed -> ModelRemoved
  -> SemanticGenerationController::OnModelRemoved -> ClearActiveModel (when it was active)
Project without an active model
  -> RuntimeOptionsForCurrentSidecarSnapshot: no model_id -> alcedo_mind without --model-id
  -> AI Mask and image analysis run; semantic requests -> AI_ERROR_MODEL_NOT_READY
```

**Primary failure call chains:**

```text
Another download runs -> InstallModel false, row error "Another download is running."
Cancel -> DownloadService cancel -> Finished(false) -> row notInstalled, "Model download canceled."
Activation without a project / not installed / active / during another activation
  -> ActivateModel false, row error with the reason, project unchanged
Sidecar start fails or serves another model -> activation finishes failed, row error
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| Install per profile (fake aria2) | `ClipModelInstallActivationTest.ClipModelDownloadControllerTest.EachProfileInstallsIntoItsOwnDirectory` | PASS |
| Cancel per profile; one download at a time | `...ClipModelDownloadControllerTest.CancelKeepsProfileNotInstalledAndOneDownloadRunsAtATime` | PASS |
| Remove per profile | `...ClipModelDownloadControllerTest.RemoveDeletesOnlyThatProfileAndReportsIt` | PASS |
| Rows show the licenses | `...ClipModelDownloadControllerTest.RowsShowLicensesOfTheModelCards` | PASS |
| Activation sets the project active key | `...ClipModelActivationTest.ActivationOfInstalledModelSetsProjectActiveKey` | PASS |
| Activation without a project fails with the reason | `...ClipModelActivationTest.ActivationWithoutOpenProjectFailsWithReason` | PASS |
| Activation of a missing model fails with the reason | `...ClipModelActivationTest.ActivationOfModelThatIsNotInstalledFailsWithReason` | PASS |
| Activation during another activation fails with the reason | `...ActivationOfInstalledModelSetsProjectActiveKey` (second request) | PASS |
| Removing the active model leaves no active model | `...ClipModelActivationTest.RemovingActiveModelLeavesProjectWithoutActiveModel`, `SemanticStorageControllerTest.SemanticStoreTest.ClearActiveModelLeavesNoActiveModelAndKeepsRows` | PASS |
| The runtime options use the project's active model | `...ActivationOfInstalledModelSetsProjectActiveKey`, `...WithoutActiveModelSidecarStartsWithoutModelId` | PASS |
| The sidecar starts without `--model-id` | `AiSidecarRuntimeServiceTest.RuntimeArgumentsOmitModelIdWithoutActiveModel`, Rust `config::tests::starts_without_model_id_and_reports_no_semantic_model` | PASS |
| A semantic request returns `AI_ERROR_MODEL_NOT_READY` | Rust `server::semantic::tests::without_model_id_semantic_requests_return_model_not_ready`; live `AiSidecarRuntimeServiceLiveTest.RustRuntimeStartsWithoutModelIdAndRejectsSemanticRequests` (release `alcedo_mind.exe`) | PASS |
| The C++ and Rust catalogs agree | `ModelAssetCatalogTest.ProfileTableEqualsTheSharedClipCatalogFile`, Rust `model_assets::tests::profile_table_equals_the_shared_clip_catalog_file` | PASS |
| Regression | `AiSidecarRuntimeServiceTest`, `SemanticStorageControllerTest`, `ApplicationModuleHostLifecycleTest`, `DownloadServiceTest`, `SemanticGrpcDependencyLinkTest`, `AlbumBackendProjectTest` (catalog and activation manifest cases); `cargo test --lib -- --skip live_` (377 tests) | PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ModelAssetCatalogTest
ClipModelInstallActivationTest AiSidecarRuntimeServiceTest SemanticStorageControllerTest
SemanticGrpcDependencyLinkTest AlbumBackendProjectTest ApplicationModuleHostLifecycleTest
DownloadServiceTest alcedo_main`; `ctest --test-dir build/debug -R
"^(ModelAssetCatalogTest|ClipModelInstallActivationTest|SemanticStorageControllerTest|AiSidecarRuntimeServiceTest|SemanticGrpcDependencyLinkTest|ApplicationModuleHostLifecycleTest|DownloadServiceTest)\."`
with `ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH` set to the release `alcedo_mind.exe` (80 tests);
`cargo test --lib -- --skip live_`. With an installed SigLIP2 and `--model-id`, the live
embedding test loaded the model and returned an embedding; it fails only its fixed MobileCLIP
check of 512 dimensions (SigLIP2 has 768).

**Residual gaps:** the interim panel was checked with the `alcedo_main` build and `qmllint`
(no new warning class), not in the running application; the AI Models page is AM4. Activation
with cold label prototypes (sidecar start) is covered by the existing code path and the live
sidecar tests, not by an AM3 test with a real model. macOS was not built.

### 5.4 AM4: AI Models settings page

Work:

- `PackageCard.qml` (moved from `LutSettingsPanel.qml`, P11) with an `active` state (green border,
  P12) and a license Pill row.
- `AiModelsSettingsPanel.qml`, category P12:
  - Top row: model directory tile (`FolderPathField`, open folder, refresh) as the LUT library tile.
  - "AI Mask" section: one `PackageCard` (status, version line of P1, licenses, size, progress,
    Download / Update / Repair / Retry / Cancel / Remove with confirmation).
  - "Content Recognition (CLIP)" section: source combo box (and the custom URL field) in the
    section header; one card per profile with Download / Cancel / Remove (confirmation) and
    Activate. Activate is enabled only for an installed, not active model with an open project;
    without a project the card says "Open a project to activate". The card shows the activation
    progress.
  - "Download statement" text button in the page header that opens the statement read-only.
  - The statement dialog (section 4) with "Accept and Download" and "Cancel" opens on the first
    download request when the statement is not accepted.
- `SemanticGenerationSettingsPanel.qml`: remove the Model section; show the active model name of
  the project and a button that opens the AI Models page.
- Chinese translations in `alcedo_main_zh_CN.ts` by hand (no `lupdate`).

Required tests (C++ owner tests first; QML tests use the `WaitFor` and visual tree rules):
the page open checks the AI model feed once; the first download opens the statement and Cancel
starts nothing; Accept starts only the chosen download; the active CLIP card has the active
border; Activate is disabled without a project; Remove asks first; the compiled catalog
translates the statement.

#### AM4 completion record (2026-10-28)

**Status:** complete. Settings has the AI Models page (index 9, `sparkles.svg`; About is index
10) with the model directory tile, the AI Mask section, and the Content Recognition (CLIP)
section. Every card is the shared near-square `PackageCard`, so the CLIP cards and the AI Mask
card have the LUT card shape (issue [#584](https://github.com/zidage/AlcedoStudio/issues/584):
the interim list rows of AM3 are removed). The download statement opens before the first
download of any model. The Local Content Recognition page shows the active model of the project
and "Open AI Models".

Details the plan did not specify, chosen in AM4:

- **Shared components.** `PackageCard.qml` holds no service calls: the page sets the texts and
  the state, and the card reports Action, Cancel, Remove, Keep, and confirmed Remove through
  signals. Object names are `<namePrefix>:<key>` and `<namePrefix><Part>:<key>`, so the LUT page
  keeps its `lutSettingsPackage*` names and tests. The card adds a subtitle line, a license row
  (one Pill per license; a license with a URL opens it), a muted message line, and the `active`
  state (2 px `activeModelBorderColor` border). `BentoTile.qml` and `SystemActionButton.qml`
  also moved out of `LutSettingsPanel.qml`, so the model directory tile is the LUT library tile.
  The pill and license rows use `Instantiator`, not `Repeater` (offscreen test windows).
- **Theme.** `AppTheme::activeModelBorderColor` returns `backgroundTaskFinishedColor` (P12),
  recorded in `DESIGN.md`.
- **Feed check (P6).** `SettingDialog.showAiModelsPage()` checks the AI model feed the first
  time the page shows in each Settings opening (category change, or Settings opened on the page),
  and reads the CLIP install state again. The Settings-open check of AM1 is removed; the LUT
  check is unchanged.
- **Statement (P8).** The page sends every download (AI Mask install, update, repair, retry, and
  CLIP download) through `requestDownload`: before the acceptance, the statement opens and the
  download waits. "Accept and Download" calls `acceptDownloadStatement` and starts only that
  download; Cancel, Escape, or a click outside starts nothing. The owner also enforces the rule
  for CLIP: `ModelDownloadController::SetDownloadStatementCheck` (set by the host from
  `AiModelPackageService::download_statement_accepted`) makes `InstallModel` refuse with
  "Accept the download statement before you download a model." The statement texts are one
  `qsTr` per paragraph; the dataset URL is a link argument (`%1`). The AI Mask paragraph keeps
  the approved Chinese text of section 4 ("AI Mask"), while the page says "AI 蒙版" as the rest of
  the application does.
- **AI Mask card.** Version line of P1, a revision Pill (`v1 r1`, or `v1 r1 → v1 r2`), the
  archive size, the license names of the feed entry (a click opens the installed license file
  when the set is installed), the P5 status texts, and Remove with confirmation.
- **CLIP cards.** Subtitle = language; Pills = size and inference backend; one license Pill with
  the catalog URL. The main action is Download (not installed) or Activate (installed, not
  active); the active card has no action. Activate is enabled only with an open project
  (`appModules.project.serviceReady`), no running activation, and the `canChangeSemanticModel`
  policy; without a project the card shows "Open a project to activate this model." The active
  border needs an open project. Remove asks first and warns when the model is active.
- **Source.** The source combo box is in the CLIP section header; the custom URL field shows
  below it for "Custom".
- **Test factories.** `ApplicationModuleHost::LutServiceFactories` is now
  `PackageServiceFactories` with an `ai_model_package_options` factory (the host adds the shared
  downloader and the model directory when the options have none).
- **Translations.** The page, the statement, the card, the AM1 service texts
  (`alcedo::AiModelPackageService`), and the AM3 controller texts (`Alcedo` context) have Chinese
  entries, added by hand.

**Primary success call chain:**

```text
Settings > AI Models (category 9)
  -> SettingDialog.onCurrentCategoryChanged / onVisibleChanged -> showAiModelsPage
     -> modelDownload.refreshModels; aiModelPackages.checkPackages (once per opening)
  -> AiModelsSettingsPanel: AI Mask PackageCard (packages row), one PackageCard per CLIP row
Download / Update / Repair / Retry on a card
  -> requestDownload(kind, id): statement not accepted -> AiModelDownloadStatementDialog
  -> Accept and Download -> acceptDownloadStatement -> installPackage(id) or installModel(id)
Activate on an installed CLIP card (open project)
  -> semanticGeneration.activateModel(profileId) -> active border on that card
Remove -> card confirmation -> removePackage(id) or removeModel(profileId)
```

**Primary failure call chains:**

```text
Statement not accepted -> Cancel / Escape -> pendingDownload cleared, nothing starts
CLIP installModel without the statement (any caller) -> false, row error, no download
No open project -> Activate disabled, card message "Open a project to activate this model."
Feed check fails -> "The AI model check failed: %1" in the AI Mask header, Check again
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| The page open checks the AI model feed once | `AiModelsSettingsQmlTest.PageOpenChecksTheFeedOnceAndStartupDoesNotCheck` | PASS |
| The first download opens the statement and Cancel starts nothing (AI Mask and CLIP) | `AiModelsSettingsQmlTest.FirstDownloadOpensTheStatementAndCancelStartsNothing` | PASS |
| Accept starts only the chosen download | `AiModelsSettingsQmlTest.AcceptStartsOnlyTheChosenDownloadAndTheStatementStaysAccepted` | PASS |
| A CLIP download needs the statement in the owner | `ClipModelInstallActivationTest.ClipModelDownloadControllerTest.DownloadBeforeStatementAcceptedStartsNothing` | PASS |
| The active CLIP card has the active border | `AiModelsSettingsQmlTest.ActiveClipCardHasTheActiveBorderAndActivateStartsActivation` | PASS |
| The AI Mask card has the CLIP card shape | `AiModelsSettingsQmlTest.PageOpenChecksTheFeedOnceAndStartupDoesNotCheck` (width and height checks) | PASS |
| Activate is disabled without a project | `AiModelsSettingsQmlTest.ActivateIsDisabledWithoutAnOpenProject` | PASS |
| Remove asks first | `AiModelsSettingsQmlTest.RemoveAsksBeforeTheModelIsRemoved` | PASS |
| The compiled catalog translates the statement | `SimplifiedChineseCatalogTest.CompiledCatalogTranslatesTheAiModelDownloadStatement` | PASS |
| The shared `PackageCard` keeps the LUT page | `LutSettingsQmlTest` (5 tests) | PASS |
| Regression | `ClipModelInstallActivationTest`, `SimplifiedChineseCatalogTest`, `AutomationLutCommandsTest`, `AutomationProjectCommandsTest`, `ApplicationModuleHostLifecycleTest`, `ShortcutSettingsQmlTest` (About at index 10) | PASS, except below |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main
AiModelsSettingsQmlTest LutSettingsQmlTest ClipModelInstallActivationTest
SimplifiedChineseCatalogTest AutomationLutCommandsTest AutomationProjectCommandsTest
ApplicationModuleHostLifecycleTest ShortcutSettingsQmlTest`; `ctest --test-dir build/debug -j 1 -R
"^(AiModelsSettingsQmlTest|LutSettingsQmlTest|ClipModelInstallActivationTest|SimplifiedChineseCatalogTest|ShortcutSettingsQmlTest|AutomationLutCommandsTest|AutomationProjectCommandsTest|ApplicationModuleHostLifecycleTest|AiModelPackageServiceTest)\."`
(76 tests). The page was also checked in offscreen screenshots of the Settings dialog (cards,
statement dialog); the screenshots are not kept.

**Residual gaps and problems found:** `ShortcutSettingsQmlTest.RegistryRowsAppearInStableGroups`
fails because of the Comparison shortcut group, before and after this change (issue
[#585](https://github.com/zidage/AlcedoStudio/issues/585)). `AiModelPackageServiceTest` fails
intermittently at the AM1 commit move with "Access is denied" (issue
[#586](https://github.com/zidage/AlcedoStudio/issues/586)). A CLIP download after the acceptance
was not started in a QML test (it would run aria2c); the owner test covers it. The page was not
checked in the running application or on macOS.

### 5.5 AM5: AI Mask tool integration

Work:

- Before `LoadSegmentationModels`, the tool reads `AiMaskModelInstallation`. Not installed, Update
  required, or Needs a newer Alcedo Studio: the panel shows that state and "Open AI Models"
  (opens Settings at the AI Models category). No sidecar start, no session.
- The sidecar reads `model_set.json` at load and rejects a version outside its range with
  `AI_ERROR_MODEL_NOT_READY` and the reason. The developer-filled directory without
  `model_set.json` is no longer accepted; developers install through the feed or the AM2
  prepare output.
- Replace the Sky notice text (section 4) and its Chinese entry.
- Update the master plan sections 3.1, 3.3, and D5 text that A7 and A8 replace.

Required tests: the tool with no installed set shows the not-installed state and starts no
sidecar; a set below the minimum shows Update required; the sidecar rejects `model_set.json`
version 2 and a missing `model_set.json`; the Sky notice test and the catalog test check the new
text.

#### AM5 completion record (2026-10-28)

**Status:** complete. Before the sidecar starts, the AI Mask tool reads the installed AI Mask
model set. Without a complete set of a supported version, the panel shows the state and "Open AI
Models"; no sidecar starts and no AI Mask edit opens. The sidecar loads only a directory whose
`model_set.json` has a version in its range. The Sky notice has the wording of section 4. The
master plan sections 3.1, 3.3, D5, and the acceptance matrix row "Missing model" are updated.

Details the plan did not specify, chosen in AM5:

- **Owner of the readiness read.** `AiModelPackageService::ReadAiMaskModelSet()` reads the local
  installations again (`RefreshInstallations`, file metadata only, no network request) and maps
  them with the pure function `AiMaskModelSetReadinessFor(installation, compared status)` to
  `AiMaskModelSetReadiness` (`kReady` with the installation directory, `kNotInstalled`,
  `kUpdateRequired`, `kNeedsNewerApplication`, `kRepairRequired`). The tool receives it through
  `EditorAiMaskTool::Services::model_set`, which replaces `model_directory`; the fixed
  `alcedo-ai-mask-v1` path of the host is removed. Each Open reads the set again, so an install
  on the AI Models page is used at the next Open without a restart.
- **Repair also stops the Open.** An incomplete set of a supported version (P5 Repair) cannot
  load, so the tool shows "repair" with the same button instead of a load error.
- **Needs a newer Alcedo Studio** comes from the installation too: a set installed only above
  this build's maximum (by a newer build) gives that state without a feed check. Without any
  installed set, the state comes from the last feed comparison of the service (P5).
- **Place of the check.** `EditorAiMaskTool::OpenFor`, after the image and the Color Grade checks
  and before the runtime request and the AI Mask edit lease. The state is the property
  `modelSetState` (`notInstalled`, `updateRequired`, `needsNewerApplication`,
  `repairRequired`, or empty); the tool stays active, so the panel shows it, and Close clears it.
- **"Open AI Models".** The panel calls `host.openAiModelSettings()` (Main.qml ->
  `AppDialogs.openAiModelSettings()` -> Settings at `aiModelsCategory`) and closes the tool. The
  host reaches the panel through `EditorWorkspace` -> `EditorAdjustmentStack` ->
  `EditorMasksContextPanel` -> `EditorAiMaskPanel` (`host` property). While the state shows, the
  tasks, the prompt tools, and Confirm are hidden.
- **Sidecar check.** `model_set.rs` declares `MODEL_SET_PACKAGE_ID` (`alcedo-ai-mask`),
  `MODEL_SET_FILE`, `MODEL_SET_MINIMUM_VERSION = MODEL_SET_MAXIMUM_VERSION = 1`, and
  `model_set_version_from_json` (`schema` 1, `kind` `alcedo-ai-model-set`, `id`, positive
  `model_set_version`). `SegmentationEngine::load` runs `check_model_set_version` before the head
  thresholds and before any model file opens; every failure is `ModelNotReady`, which the server
  sends as `AI_ERROR_MODEL_NOT_READY` with the reason. `MODEL_SET_ID` (`alcedo-ai-mask-v1`, the
  identity stored in each AI Mask) is unchanged. The live port check
  (`tests/segmentation_port_check.rs`) now needs an installed set in
  `ALCEDO_SEGMENTATION_MODEL_DIR`.
- **Texts.** The four state texts, "Open AI Models", and the new Sky notice have Chinese entries
  in the `EditorAiMaskPanel` context, added by hand.

**Primary success call chain:**

```text
Header AI Mask button -> EditorAiMaskTool::open -> OpenFor
  -> image and Color Grade checks
  -> Services::model_set -> AiModelPackageService::ReadAiMaskModelSet
     -> RefreshInstallations (ReadAiModelInstallation) -> AiMaskModelSetReadinessFor -> kReady
  -> runtime -> OpenAiMaskEdit -> analysis render -> StartOnWorker
     -> LoadModels(<model directory>/alcedo-ai-mask-v1)
        -> sidecar SegmentationEngine::load -> check_model_set_version (v1 in [1, 1]) -> models
```

**Primary failure call chains:**

```text
No set / below minimum / only above maximum / incomplete
  -> OpenFor stops: modelSetState set, no runtime request, no edit lease
  -> EditorAiMaskPanel shows the state text and "Open AI Models"
     -> host.openAiModelSettings -> Settings > AI Models; tool.cancel
model_set.json missing, invalid, or version outside [1, 1] (a directory filled by hand)
  -> SegmentationEngine::load -> ModelNotReady(reason) -> AI_ERROR_MODEL_NOT_READY
  -> tool error "The AI Mask models could not be loaded from <dir>: <reason>"
```

**What was proven (executed tests):**

| Required test | Test | Result |
| --- | --- | --- |
| No installed set: not-installed state, no sidecar start | `EditorAiMaskModelSetTest.NoInstalledSetShowsNotInstalledAndStartsNoSidecar` (real `AiModelPackageService` over an empty model directory) | PASS |
| A set below the minimum: Update required | `EditorAiMaskModelSetTest.SetBelowTheMinimumShowsUpdateRequiredAndStartsNoSidecar` | PASS |
| An incomplete set: Repair; each Open reads the set again; a ready set opens the edit | `EditorAiMaskModelSetTest.IncompleteSetShowsRepairAndEachOpenReadsTheSetAgain` | PASS |
| The three tool tests fail without the check | same tests with the check bypassed: 3 FAILED ("no sidecar runtime was requested") | proven |
| Readiness from the owner (ready directory, repair, removed, no network request) | `AiModelPackageServiceTest.AiMaskToolReadsTheInstalledSetAgainAndItsDirectory` | PASS |
| Readiness for every range case | `AiModelPackageServiceTest.AiMaskToolReadinessFollowsTheSupportedRange`, `...FeedWithOnlyHigherVersionsNeedsNewerApplication` | PASS |
| The sidecar rejects `model_set.json` version 2 and a missing `model_set.json` | `engine::tests::load_rejects_missing_model_set_file_and_loads_no_model`, `engine::tests::load_rejects_model_set_version_outside_the_supported_range`, `server::segmentation::tests::server_rejects_missing_or_unsupported_model_set_as_model_not_ready` | PASS |
| The panel shows the state and "Open AI Models" opens Settings and closes the tool | `EditorAiMaskPanelQmlTest.MissingModelSetShowsItsStateAndOpenAiModelsOpensSettings` (fails on the AM4 panel) | PASS |
| The Sky notice test checks the new text | `EditorAiMaskPanelQmlTest.SkyTrainingDataNoticeIsVisibleBeforeFirstSkyRun` (fails on the AM4 panel) | PASS |
| The catalog test checks the new texts | `SimplifiedChineseCatalogTest.CompiledCatalogTranslatesTheSkyTrainingDataNotice`, `...CompiledCatalogTranslatesTheAiMaskModelSetStates` | PASS |
| Regression | `EditorMaskRangeAdapterTest` (the tool with an installed set still opens its edit), `EditorNodesPanelQmlTest`, `AiModelPackageServiceTest`, `SimplifiedChineseCatalogTest`, Rust `cargo test --lib segmentation` | PASS, except below |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main
AiModelPackageServiceTest EditorAiMaskModelSetTest EditorMaskRangeAdapterTest
EditorAiMaskPanelQmlTest SimplifiedChineseCatalogTest EditorNodesPanelQmlTest`; `ctest
--test-dir build/debug -j 1 -R
"^(AiModelPackageServiceTest|EditorAiMaskModelSetTest|EditorMaskRangeAdapterTest|EditorAiMaskPanelQmlTest|SimplifiedChineseCatalogTest|EditorNodesPanelQmlTest)\."`
(134 tests); `cargo test --lib segmentation` in `rust/puerh_mind` (87 tests) and
`cargo test --lib -- --skip live_` (380 tests, no warnings).

**Residual gaps and problems found:** `AiModelPackageServiceTest` failed intermittently at the
install commit move with "Access is denied" (`InstallReportsDownloadVerifyAndInstallStages`,
`MissingOrResizedFileIsRepairWithoutHashing`; each passed when it ran again), the known issue
[#586](https://github.com/zidage/AlcedoStudio/issues/586) (comment added). The tool was not
driven in the running application, and macOS was not built or run.

## 6. Tracking

Tracking issue #579. Phase issues: AM1 #574, AM2 #575, AM3 #576, AM4 #577, AM5 #578.
