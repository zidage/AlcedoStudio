# Sleeve Album Membership Filesystem Refactor Plan

Date: 2026-05-25

## Background

当前 Sleeve 内部文件系统已经具备接近硬链接的底层形态：

- `Element` 存储文件和文件夹身份。
- `FolderContent(folder_id, element_id)` 存储目录到 element 的引用关系。
- `FileImage(file_id, image_id)` 将 `SleeveFile` 绑定到实际图片。
- 编辑历史、pipeline、image pool 等上层服务基本以 `file_id` / `element_id` 为身份。

新的产品语义希望把 Root 文件夹改成“全部图片”，子文件夹改成相册/子集：

- 所有导入图片都属于 Root。
- 子文件夹只保存 Root 中 File 的引用。
- 用户在任意子相册中编辑图片后，Root 下的同一图片也应看到相同结果。
- 用户可以右键将某个文件添加到其他目录。
- 实际 UI 暂时只支持两级结构，但底层仍保留多级文件树能力，方便未来扩展。
- 搜索应优先以 Root 全量图片为基础，子相册检索通过 join membership 条件完成。

现有实现的主要风险是 `SleeveElement::ref_count_` 同时承担了目录引用计数、CoW 共享判断和删除生命周期判断。当前 `PathResolver::ResolveForWrite()` 会在 `ref_count_ > 1` 时触发 CoW。如果直接把“多个相册引用同一个文件”映射到现有 `ref_count_`，用户在子相册编辑图片时会被复制成新的 File，违背“同一图片多处可见并共享修改”的目标语义。

## Decision

不要实现 POSIX 风格的 symbolic link。目标设计应采用 DAM 常见的 collection membership model：

- `File` 是照片的逻辑身份。
- `FolderContent` / `AlbumMembership` 是某个相册是否包含该 File 的关系。
- “添加到其他目录”只是增加 membership，不复制 File，也不创建 path-based symlink。
- CoW 只服务于显式“复制为独立副本”或未来的 payload 共享优化，不服务于相册归属。

对用户而言，这个行为接近硬链接：同一张照片出现在多个相册，编辑任一位置会影响同一 File identity。对实现而言，应避免叫做 symlink，因为 symlink 会引入目标路径失效、重命名解析、broken link、递归解析和 UI id 歧义。

## Target Semantics

Root 语义：

- Root 表示“全部图片”。
- Root 可以实现为虚拟视图：查询所有 `Element.type = FILE` 的 File。
- 如果短期为了兼容现有 `ListFolderContent("/")` 选择 materialized Root membership，则必须维护不变量：每个 live File 都有一条 `FolderContent(root_id, file_id)`。

子文件夹/相册语义：

- 子文件夹表示相册或图片子集。
- 子文件夹中只允许引用 File membership。UI 层第一阶段只暴露两级结构。
- 底层可以继续支持 Folder element，以便未来做多级相册树。

导入语义：

- 导入目标无论是 Root 还是子相册，都先创建一个新的 File identity。
- 新 File 必须进入 Root。
- 如果导入目标是子相册，再额外插入 `FolderContent(target_folder_id, file_id)`。
- 导入失败回滚时，要同时移除 Root membership、目标相册 membership、FileImage、ImagePool 占位对象和相关 history/pipeline。

删除语义：

- 在子相册删除图片：只删除 `FolderContent(album_id, file_id)`，不删除 File 本体。
- 在 Root 删除图片：删除 File 本体，并级联移除所有相册 membership、FileImage、edit history、pipeline、image pool/storage 绑定。
- API 必须区分 `UnlinkFileFromFolder(folder_id, file_id)` 和 `DeleteFileEverywhere(file_id)`，不能只靠 `DeleteElement(file_id)` 表达两种行为。

编辑语义：

- 相册 membership 不触发 CoW。
- 编辑任意相册中的同一 File，修改的是同一个 edit history / pipeline。
- Root 和所有包含该 File 的相册都显示同一编辑结果。

复制语义：

- “添加到相册”不复制。
- “复制为独立副本”才创建新的 File identity。
- 独立副本可以初始共享底层 original image payload，但 edit history / pipeline 必须独立。
- 如果未来实现 payload-level CoW，只有独立副本共享 payload 时才使用 CoW ref count。

## Data Model

第一阶段可以保留现有表名，但要明确 `FolderContent` 是 membership 关系，不是拥有关系：

```sql
FolderContent(
  folder_id INTEGER NOT NULL,
  element_id INTEGER NOT NULL,
  added_time TIMESTAMP,
  sort_key BIGINT,
  PRIMARY KEY(folder_id, element_id)
);

CREATE INDEX idx_folder_content_folder ON FolderContent(folder_id);
CREATE INDEX idx_folder_content_element ON FolderContent(element_id);
```

如果后续需要同一 File 在不同相册里有不同显示名、排序名或局部标注，可以迁移到显式 link row：

```sql
FolderContent(
  link_id INTEGER PRIMARY KEY,
  folder_id INTEGER NOT NULL,
  element_id INTEGER NOT NULL,
  display_name TEXT,
  added_time TIMESTAMP,
  sort_key BIGINT,
  UNIQUE(folder_id, element_id),
  UNIQUE(folder_id, display_name)
);
```

第一阶段不建议引入 `link_id`，除非 UI 已经需要 per-album display name。保持 `(folder_id, element_id)` 主键更简单，也更贴合搜索 join。

## CoW Boundary

当前 `SleeveElement::ref_count_` 的职责需要拆开。目标状态应至少区分：

- membership/link count：由 `FolderContent` 行数表达，或者以单独字段缓存。
- CoW ref count：只用于独立副本共享底层 payload，不用于相册 membership。
- object cache lifetime：由 `NodeStorageHandler` / cache 策略管理，不应混入文件系统语义计数。

推荐长期模型：

```sql
FileContent(
  content_id INTEGER PRIMARY KEY,
  image_id INTEGER NOT NULL,
  ref_count INTEGER NOT NULL
);

FileImage(
  file_id INTEGER PRIMARY KEY,
  content_id INTEGER NOT NULL
);
```

在该模型中：

- `LinkFileToFolder(file_id, folder_id)` 不增加 `FileContent.ref_count`。
- `DuplicateFile(file_id)` 创建新的 `file_id`，初始可以引用同一 `content_id` 并增加 `FileContent.ref_count`。
- 编辑 duplicate 时，如果 `FileContent.ref_count > 1`，才 detach payload。
- 编辑普通 album membership 时，不触发 detach，因为它不是副本。

短期如果不引入 `FileContent` 表，也至少应做到：

- `FolderContent` membership 变化不再让 `ResolveForWrite()` 对 File 触发 CoW。
- `Copy()` / `DuplicateFileToFolder()` 成为显式独立副本 API。
- `SleeveFile::Copy()` 不应共享 mutable edit history pointer；独立副本需要 fork 或 clone history。

## API Plan

新增或重命名 FileSystem / SleeveService 层 API：

```cpp
auto CreateFileInLibrary(file_name_t name) -> std::shared_ptr<SleeveFile>;
void LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
void UnlinkFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
auto DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
    -> std::shared_ptr<SleeveFile>;
void DeleteFileEverywhere(sl_element_id_t file_id);
```

路径 API 保留给 folder tree 和兼容场景，但相册图片操作优先使用 id-based API：

- UI 列表项持有 `file_id` 和当前 `folder_id`。
- 删除按钮根据当前 folder scope 调用 unlink 或 delete-everywhere。
- “添加到相册”通过 `file_id + target_folder_id` 执行。
- 编辑器打开图片时只关心 `file_id`，不关心它来自哪个相册路径。

旧 API 迁移建议：

- `FileSystem::Copy(from, dest)` 改语义或废弃。对于 File，它应调用 `LinkFileToFolder`；对于 Folder，第一阶段可以禁用跨 folder copy，避免递归 membership 语义不清。
- `FileSystem::Delete(path)` 继续可用于 folder 删除；删除 File path 时根据 parent 是否 Root 决定 unlink/delete-everywhere。
- `FileSystem::Delete(element_id)` 在多相册语义下不应用于 UI 普通删除。保留时应明确表示 delete everywhere。

## Search Plan

Root 全部图片搜索：

```sql
SELECT e.id
FROM Element e
JOIN FileImage fi ON fi.file_id = e.id
JOIN Image i ON i.id = fi.image_id
WHERE e.type = 0
  AND <filter>;
```

相册内搜索：

```sql
SELECT e.id
FROM FolderContent fc
JOIN Element e ON e.id = fc.element_id
JOIN FileImage fi ON fi.file_id = e.id
JOIN Image i ON i.id = fi.image_id
WHERE fc.folder_id = ?
  AND e.type = 0
  AND <filter>;
```

实现要求：

- `FilterCombo` 继续生成 filter predicate，但 scope 由查询构造层决定。
- Root scope 不强制依赖 `FolderContent(0, file_id)`，长期优先虚拟 Root 查询。
- 子相册 scope 只多一个 `FolderContent` join 和 `folder_id` 条件。
- 统计、筛选、缩略图分页都复用同一套 scope query builder。

## Lazy Loading And Cache Plan

当前 `NodeStorageHandler::EnsureChildrenLoaded()` 使用 `ContentSize() == 0` 判断是否已加载，这会混淆“空文件夹”和“未加载文件夹”。应改为显式状态：

```cpp
bool children_loaded_ = false;
```

第一阶段：

- `SleeveFolder` 增加 `children_loaded_`。
- DB-backed load 完成后标记 loaded。
- 空文件夹也可以被正确标记为 loaded。

长期：

- Root 不应把所有图片都加载进 `SleeveFolder::contents_`。
- 缩略图网格、搜索和统计应走 DB-first 分页查询。
- `storage_` 只作为 object cache，不作为完整数据库镜像。
- 为 `NodeStorageHandler` 增加容量上限或 LRU 策略，避免大库长期驻留所有 `SleeveElement`。

## Storage Container Plan

不建议把 `storage_` 从 `unordered_map<id, shared_ptr<SleeveElement>>` 重构成可 compact 的 dense array。原因：

- `element_id` 已经是 DB 主键和外键，必须稳定。
- UI、history、pipeline、FileImage、FolderContent 都依赖稳定 id。
- compact/renumber 会带来全库外键重写、缓存失效和项目包迁移风险。

建议：

- 保持 id 单调递增，不复用。
- `storage_` 继续使用 `unordered_map` 作为对象缓存。
- 删除后使用 tombstone / `SyncFlag::DELETED` / DB 删除，再通过 database `VACUUM` 或 project repack 回收空间。
- 如果未来需要数组形式，也只能使用 `vector<optional<...>>` 或 sparse set，不允许改变已分配 id。

## Current Progress

最新已检查基线 commit：

- `8bb9b857 refact: Refactor sleeve_service_test.cpp to enhance database interaction and improve test coverage`

该 commit 和后续 Phase 1 closeout 工作区改动已经落地了 Phase 1 的核心基础设施：

- `FileSystem` / `SleeveService` 已新增 id-based membership API：
  - `CreateFileInLibrary`
  - `LinkFileToFolder`
  - `UnlinkFileFromFolder`
  - `DuplicateFileToFolder`
  - `DeleteFileEverywhere`
- `FileSystem::Delete(path)` 已经可以根据 parent scope 区分 Root 删除和子相册 unlink。
- `FileSystem::Copy(from, dest)` 对 File 已经转为 link membership，而不是复制 File identity。
- `SleeveServiceTest` 已覆盖 link、unlink、delete-everywhere、copy/link、duplicate 等基础语义。
- `ImportService::ImportToFolder` 已改为先在 library/root 创建 File，再按需 link 到目标相册。
- 导入到不存在目标时会先校验目标 folder，不再先创建 Root 文件后失败。
- metadata/import 失败回滚已改为按 `element_id` 调用 `DeleteFileEverywhere`，避免子相册导入失败后遗留 Root membership。
- `ImportServiceTest` 已补充：
  - 导入不存在目标不会残留 Root 文件。
  - 导入子相册后，Root 和目标相册看到同一 `element_id`。
  - sync 前后 Root/album membership 都保持正确。
- `EditHistoryMapper::GetEditHistoryByFileId` 在无记录时返回 `nullptr`，避免手工 filesystem 测试文件被加载时构造默认 `EditHistory` 导致崩溃。
- `ElementStore::UpdateElement` 仅在文件已有 history 时更新 history，避免对无 history 文件执行无意义 upsert。
- `PathResolver::Tree()` 输出已排序，避免 DB 恢复后 membership 枚举顺序导致测试不稳定。
- `FolderContent(folder_id, element_id)` 新项目 schema 已直接创建 `PRIMARY KEY(folder_id, element_id)`，并保留 `folder_id` / `element_id` 查询索引。
- 旧项目不做数据 migration；`project_file_version`、`project_file_min_supported_version`、`project_file_max_supported_version` 均已递增到 `0.2.5`，`0.2.4` 会被版本检查拒绝打开。
- `Database` 不再对已有 DB 执行 `FolderContent` 去重/backfill migration。
- `SleeveFSTest` 已补齐 FileSystem 层的新 membership/link 语义测试：
  - 同一 File link 到多个 album 仍保持单一 `file_id`。
  - 重复 link 同一 File 到同一 album 幂等。
  - `FileSystem::Copy(file_path, album_path)` 是 membership link，不创建新 File identity。
  - 子相册删除只 unlink，Root 删除 delete-everywhere。
  - album membership 不增加 `ref_count_`，不触发 CoW 共享判断。
- `SleeveFolder` 已增加 `children_loaded_`，`NodeStorageHandler::EnsureChildrenLoaded()` 不再用 `ContentSize() == 0` 区分“空文件夹”和“未加载文件夹”。

已验证：

- `SleeveFSTest.exe`: 12/12 passed
- `SleeveServiceTest.exe`: 19/19 passed
- `ImportServiceTest.exe`: 13/13 passed
- `git diff --check`: passed

Phase 1 当前可视为完成。结合本次复审，后续结构项现更新为：

- Phase 2 的主线能力已经落地：列表项携带 `file_id + folder_id`，编辑器入口不再依赖 album path，添加到相册/按 scope 删除/search/stats 的 scope query 也已接通。
- Phase 2 仍未彻底完成的只剩“大库路径”相关内容：缩略图 grid 仍会在 `ReloadCurrentFolder()` 时全量加载当前 scope 的文件，再在内存中重建可见列表；真正的 DB-first 分页尚未完成。
- `ref_count_`、`ResolveForWrite()` 和 `SleeveFile::Copy()` 的 CoW/duplicate 边界还没有彻底拆分。
- Root 仍是 materialized membership；虚拟 Root 视图、bounded object cache 和大库性能治理应单独后置。

## Migration Phases

### Phase 1: Current Closeout And Schema Hardening

目标：把当前已经实现的 membership/import 语义收口成可长期依赖的基础，不再留下“运行时看起来可用，但 DB/schema/test 没锁住”的风险。旧项目不进入本阶段 migration；通过递增项目文件版本并提高最小支持版本，旧 schema 项目直接不支持打开。

范围：

- 收口 `FolderContent` 数据约束。
- 收口导入、link/unlink/delete-everywhere 的 DB 恢复行为。
- 收口编辑共享可见性测试。
- 收口重复 membership 的幂等或错误语义。

详细工作项：

- 检查现有 `FolderContent` schema 是否已经有 `PRIMARY KEY(folder_id, element_id)` 或等价唯一约束。
- 新项目 schema 必须直接创建 `PRIMARY KEY(folder_id, element_id)`，并创建 `folder_id` 和 `element_id` 查询索引。
- 递增 `project_file_version` / `project_file_min_supported_version`，明确旧 schema 项目不支持打开；不实现旧 `FolderContent` 去重或 backfill migration。
- 决定 Root 的第一版实现方式：
  - 如果继续 materialized Root membership，新版本项目的所有创建/导入路径必须补齐 live File 的 Root membership。
  - 如果改为虚拟 Root 查询，`ListFolderEntries("/")`、搜索和统计必须统一走 Root scope query。
- 补齐 DB 重启恢复测试：
  - 导入到 Root 后关闭/重开 DB，Root 能看到同一 File。
  - 导入到子相册后关闭/重开 DB，Root 和相册都能看到同一 `file_id`。
  - 从子相册 unlink 后关闭/重开 DB，Root 仍能看到 File，相册看不到。
  - 从 Root delete-everywhere 后关闭/重开 DB，所有相册都看不到 File。
- 补齐编辑共享可见性测试：
  - 同一 File link 到两个相册。
  - 从任一相册加载并修改 edit history / pipeline。
  - Root 和另一个相册读取到同一 `file_id` 和同一编辑结果。
- 明确重复 link 行为：
  - 推荐 `LinkFileToFolder(file_id, folder_id)` 幂等。
  - 如果选择抛错，错误类型和调用方处理必须稳定。
  - 对应增加重复添加测试。
- 保留当前 `ImportService` 修复，并扩展失败路径测试：
  - 目标 folder 不存在时不创建 Root 文件。
  - metadata 失败时 `DeleteFileEverywhere(file_id)` 清理 Root、目标相册、FileImage 和 placeholder。

Acceptance criteria:

- DB 层无法持久化重复 `(folder_id, element_id)`。
- 旧项目因 `project_file_version` 低于当前最小支持版本而被拒绝打开，不做隐式 DB migration。
- 同一 File 可以稳定出现在 Root 和多个相册，重启后仍成立。
- 子相册删除只 unlink，Root 删除 delete-everywhere。
- 导入失败不会留下孤儿 membership、无 image 绑定 File 或不可见 placeholder。
- 相册 membership 不触发 CoW，并且编辑共享可见性有测试覆盖。

### Phase 2: Album Scope API, UI, Search And Stats

目标：把“照片身份是 `file_id`，相册只是 scope/membership”贯穿到应用服务、UI、搜索、统计和当前列表 reload，避免继续用 path 表达照片身份。

范围：

- UI 和 AlbumBrowseService 迁到 id-based scope。
- “添加到相册”和删除操作接入 membership API。
- 搜索、统计和当前列表 reload 统一使用 scope query builder。
- Root 和 album 的列表/查询语义保持一致。

详细工作项：

- `AlbumBrowseService` 列表项返回：
  - `file_id`
  - 当前 `folder_id` 或 folder path
  - 当前 scope 类型：Root 或 album
  - UI 仍需要的显示名、缩略图 id、metadata 摘要
- UI 打开编辑器时只传 `file_id`，不再依赖 album path 定位照片。
- UI 删除路径改为：
  - 当前 scope 是 Root：调用 `DeleteFileEverywhere(file_id)`。
  - 当前 scope 是 album：调用 `UnlinkFileFromFolder(file_id, folder_id)`。
- 增加“添加到相册”入口：
  - 选择目标 folder。
  - 调用 `LinkFileToFolder(file_id, target_folder_id)`。
  - 对重复添加给出稳定结果。
- 建立 shared scope query builder：
  - Root scope 查询所有 live File，或查询 materialized Root membership。
  - Album scope 通过 `FolderContent.folder_id = ?` join File。
  - filter predicate 仍由 `FilterCombo` 或现有 filter builder 生成。
- 迁移搜索和统计：
  - `BuildFolderStats`
  - `GetElementIdsInFolderByFilter`
  - 缩略图 grid reload（分页留给 Phase 4）
  - 任何 album count 或 filtered count
- 保留 path API 作为兼容层，但新 UI 和新服务逻辑不再依赖 path 表达图片身份。

当前审核补充（2026-05-25）：

已确认落地：

- `AlbumBrowseService` 的 file view 已携带 `file_id`、`folder_id` 和 scope type。
- UI 打开编辑器已通过 `elementId/fileId` 进入，不再从 album path 解析照片身份。
- UI “添加到相册”已接入 `LinkFilesToFolder` / `LinkFileToFolder`。
- UI 删除已按当前 folder scope 进入 `DeleteFilesInFolderByElementIds`，Root scope 调 delete-everywhere，album scope 调 unlink。
- `BuildFolderStats` 和 `GetElementIdsInFolderByFilter` 已共用 storage 层 scoped file query；Phase 2 的 `AlbumScopeFilterUsesMembershipOnly` 测试通过。
- `FilterServiceTest` 已补充到 CTest discovery，避免 filter scope 测试被漏跑。
- `AlbumBackend::ReloadCurrentFolder()` 已从 `CurrentFolderFsPath()` + `ListFilesInFolder(path)` 切到 `CurrentFolderElementId()` + `ListFilesInFolderById(folder_id)`，列表数据源已改为 DB scoped list。
- `SleeveFilterService` 已增加按 folder scope 或全量清理 filter result cache 的 API；UI 添加到相册和删除路径已在 membership 变更后调用 cache invalidation。

本次审核结论（2026-05-25）：

- Phase 2 的核心语义已经完成。
- 原计划中仍带有“大库性能/分页”性质的尾项不适合继续算在 Phase 2 closeout 内，现移至后续阶段。

以下为已完成工作项：

filter cache 自动失效：

- `AlbumBrowseService` 新增 `filter_service_` 依赖，在 `LinkFilesToFolder`、`DeleteFilesInFolderByElementIds`、`DeleteFiles`、`DeleteFilesByElementIds` 四个 mutation 方法中自动调用 `InvalidateResultCache`。
- `ProjectService` 在创建 `AlbumBrowseService` 时注入 `filter_service_`。
- `ImageStore` 中移除冗余的手动 `InvalidateResultCache` 调用。
- 新增 `FilterServiceTests.AutoInvalidationOnLink` 测试，验证 `AlbumBrowseService::LinkFilesToFolder` 自动触发 cache 失效。

stats-bar DB-first 筛选：

- `ElementStore` 新增 `ListFilteredFileIds(folder_id, extra_filter_where)` 方法，复用 `BuildScopedFileQuery` 基础设施。
- `StatsEngine` 新增 `BuildStatsFilterWhere()` 方法，将当前 stats-bar filter 值转换为 SQL WHERE clause。
- `StatsEngine::RebuildThumbnailView()` 在 stats-bar filter 激活时，走 DB 查询路径获取匹配的 element ID，再基于 `all_images_` 构建缩略图视图；无 filter 时保持原有快速路径。

UI 边缘测试覆盖：

- `AddToAlbumTwiceIsIdempotent`：同一文件重复添加到同一相册，幂等，ShownCount 保持为 1。
- `DeleteFromRootRemovesFromAllAlbums`：Root 删除后，所有相册中该文件消失。
- `StatsFilterConsistencyAfterMembershipChange`：membership 变更后 `ShownCount()` == `TotalPhotoCount()`。
- `AddToAlbumSurvivesReload`：添加到相册后保存并重载，文件仍在相册中。

移出 Phase 2：

移至 Phase 3：

- `ref_count_` / `ResolveForWrite()` / `SleeveFile::Copy()` CoW 边界拆分。
- duplicate 的 edit history / pipeline 独立性收口。

移至 Phase 4（新增）：

- 缩略图 grid DB-first 分页查询（`ReloadCurrentFolder()` 仍全量加载 `all_images_`）。
- Root 虚拟视图替代 materialized Root membership。
- object cache 容量控制与 LRU 策略。
- 大库场景下的 DB-first list/search/stats/pagination 一致性治理。

验证结果（2026-05-25）：

- `FilterServiceTest`: 20/20 passed（新增 AutoInvalidationOnLink）
- `AlbumBackendImageDeleteTest`: 7/7 passed（新增 4 个边缘测试）
- `SleeveFSTest`: 12/12 passed
- `SleeveServiceTest`: 19/19 passed
- `ImportServiceTest`: 13/13 passed
- `AlbumBackendFolderTest`: 8/8 passed
- 总计 35/35 通过，0 失败。

Acceptance criteria:

- UI 可以把同一 File 添加到多个相册。
- UI 从相册删除图片不会删除 Root 中的 File。
- UI 从 Root 删除图片会从所有相册消失，并有明确确认路径。
- Root 搜索覆盖全库 live File。
- 子相册搜索只返回当前相册 membership 中的 File。
- 统计、筛选、当前列表 reload 使用同一 scope 定义，不出现列表和计数不一致。
- 编辑器入口只依赖 `file_id`。

### Phase 3: CoW Boundary And Duplicate Semantics

目标：先把 membership、duplicate 和 CoW 的语义边界拆清楚，保证“添加到相册”和“复制独立副本”在底层不会再共用同一套含混语义。

范围：

- 拆分 `ref_count_` 的职责。
- 明确 duplicate 和 album membership 的差异。
- 修正 `SleeveFile::Copy()` / duplicate 的 edit history 和 pipeline 独立性。

详细工作项：

- 停止用 `SleeveElement::ref_count_` 判断 album membership 是否共享。
- 将 CoW 触发限制到显式 duplicate / payload sharing 场景。
- 审核 `ResolveForWrite()`：
  - album membership 写入不触发 File CoW。
  - duplicate 写入如果共享 payload，才允许 detach。
- 修正 `SleeveFile::Copy()` 或替换为明确 duplicate API：
  - “添加到相册”绝不复制 File identity。
  - “复制为独立副本”创建新 `file_id`。
  - 新副本的 edit history / pipeline 独立。
- 如果 Phase 3 只追求语义正确性，可以先不引入 `FileContent` 表，直接让 duplicate 生成完全独立的 File/pipeline/history。
- 如果 duplicate 仍要初始共享 payload，则至少把 payload-level sharing 与 album membership 明确隔离，不再复用同一个 `ref_count_` 语义。

Acceptance criteria:

- 同一 File 的多相册 membership 不会因为写入而复制 File。
- 显式 duplicate 后，两个 File 的 edit history / pipeline 独立。
- 如存在 payload-level sharing，只在 duplicate detach 时触发。
- duplicate 语义不再依赖 path-based copy 的隐式行为。

当前审核补充（2026-05-25）：

已确认落地：

- `PathResolver::ResolveForWrite()` 不再因为 File `ref_count_` 触发隐式 CoW；共享写入只对 Folder identity 生效。
- Folder CoW 只传播 child folder 的 `ref_count_`，不再把 file membership 误当成 payload/shared-file 计数。
- 删除 album folder 只移除该 folder scope 下的 membership；library/root 中的 File 不会被级联误删。
- `DuplicateFileToFolder()` 现在会：
  - 为新副本分配新的 `file_id`。
  - 维护 Root membership，并在 Root/target folder 间统一处理命名冲突。
  - clone source edit history 到新的 file identity。
  - 通过 `SleeveServiceImpl` clone source pipeline snapshot 到新的 file identity。
- `SleeveServiceTest` 已新增 Phase 3 覆盖：
  - `DeletingAlbumFolderKeepsLinkedLibraryFiles`
  - `FolderCopyWriteKeepsNestedFileIdentity`
  - `ExplicitDuplicateClonesStateAndKeepsHistoryAndPipelineIndependent`

验证结果（2026-05-25）：

- `SleeveServiceTest`: 22/22 passed
- `PipelineMapperTest`: 8/8 passed
- `EditHistoryMgmtServiceTest`: 4/4 passed

### Phase 4: Root Virtual View, DB-First Pagination And Bounded Cache

目标：在不再混入 membership/duplicate 语义改造的前提下，单独解决 Root 虚拟视图、大库分页和对象缓存边界，让列表/筛选/分页在大库下仍保持一致和可控成本。

范围：

- Root 从 materialized membership 迁到虚拟视图，或至少把两种实现统一封装到同一 scope query 层。
- 缩略图 grid 改为 DB-first 分页，不再要求 `ReloadCurrentFolder()` 先全量填充 `all_images_`。
- `storage_` 从“完整数据库镜像倾向”退回到 bounded object cache。
- 评估是否需要 `FileContent` 表承载 payload-level sharing 优化。

详细工作项：

- 为 Root/album 抽象统一的 paged scope query builder：
  - list
  - filtered list
  - count
  - stats
  - thumbnail pagination
- 将 `AlbumBackend::ReloadCurrentFolder()` 从“全量 list + 内存重建 visible list”改成分页/窗口化加载。
- 把 `StatsEngine` 当前“DB 查 filtered ids，再与 `all_images_` 交集”的做法进一步收敛为真正的 paged DB-first 视图模型。
- 让 `storage_` 只缓存当前活跃窗口和必要对象；为 `NodeStorageHandler` 增加容量上限或 LRU。
- 如果决定实现 payload 共享优化，再引入 `FileContent(content_id, image_id, ref_count)` 一类模型，把 payload ref count 从 element membership 里彻底剥离。

Acceptance criteria:

- Root scope 不依赖 `FolderContent(root_id, file_id)` 也能稳定列出全库 live File。
- 大库下切换 Root/album、搜索、stats 和缩略图分页不需要一次性加载当前 scope 的全部 File。
- 列表、计数、筛选结果和缩略图分页基于同一 scope/paging 定义，不出现跨页或跨缓存不一致。
- `storage_` 有明确容量边界，不再默认长期持有全库 element 对象。

当前审核补充（2026-05-25）：

已按保守方案落地：

- `BuildScopedFileQuery(folder_id=0)` 已改为 Root 虚拟 File 视图：
  - Root 查询直接从 `Element -> FileImage -> Image` 列出 live File。
  - Root list / filter / stats / count 不再依赖 `FolderContent(0, file_id)`。
  - 子相册仍通过 `FolderContent.folder_id = ?` join membership。
- `ElementStore` 新增 DB-first 分页/计数 API：
  - `ListFilesInFolderPage(folder_id, offset, limit, extra_filter_where)`
  - `CountFilesInFolder(folder_id, extra_filter_where)`
  - 原 `ListFilesInFolder(folder_id)` 保留为兼容全量 wrapper。
- `AlbumBrowseService` 新增对应 paged list/count API，并保留旧 `ListFilesInFolderById(folder_id)` 兼容现有调用。
- `AlbumBackend::ReloadCurrentFolder()` 不再全量加载当前 scope：
  - 首次加载 1000 条轻量 file metadata。
  - `visible_thumbnails_` 只从当前已加载窗口构建。
  - `totalCount` 来自 DB count，而不是 `all_images_.size()`。
  - QML grid/list 滚动接近底部时调用 `LoadMoreThumbnails()` 追加下一批 1000 条。
- stats-bar 筛选复用同一 `BuildStatsFilterWhere()`：
  - 筛选后先走 DB count。
  - 再按同一 filter where 分页加载 thumbnail metadata。
  - 不再先查询全量 filtered ids 再和 `all_images_` 做内存交集。
- `ThumbnailService` 不做分页职责扩张：
  - 继续只管理可见 thumbnail request、pin 和 LRU。
  - cache capacity 仍由 UI visible cell hint 控制。
  - 避免把 album list/window state 引入复杂的缩略图渲染模块。

本次有意不做：

- 不引入激进的 object-cache LRU / eviction，因为当前 UI 窗口只持有轻量 metadata，1000 条左右的单批缓存成本可接受。
- 不改 `thumbnail_service.cpp` 的调度模型；缩略图缓存已经是按可见区域和 resolution tier 管理。
- 不引入 `FileContent` / payload-level sharing 表；Phase 3 已把 duplicate/CoW 语义收口，payload 共享优化继续后置。

新增/更新测试：

- `RootScopeUsesVirtualFileView`：手工移除 Root membership 后，Root DB scope 仍能通过虚拟视图列出 File，stats count 也正确。
- `PagedScopeListUsesStableOrderAndCount`：验证 paged list 的稳定 `ORDER BY e.id`、offset/limit 和 count 一致性。
- 回归验证 album import / folder / delete / thumbnail 相关测试，确认 1000 条窗口化不会破坏现有小库行为。

验证结果（2026-05-25）：

- `cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4`: passed
- `ctest --test-dir build/debug -R "FilterServiceTest|AlbumBackendImageDeleteTest|AlbumBackendImportTest|AlbumBackendFolderTest|AlbumBackendThumbnailTest" --output-on-failure`: 54/54 passed（1 skipped: Metal thumbnail lifecycle on non-Metal environment）
- `git diff --check`: passed

## Compatibility Notes

- 本次 membership schema change 不提供旧项目数据迁移路径。
- 通过递增 `project_file_version` 和 `project_file_min_supported_version`，旧项目在 metadata 版本检查阶段被拒绝打开。
- 如果当前 `ref_count_` 数据已经因为历史 CoW/Copy 行为不准确，不能直接拿它作为新 link count 的来源。
- `SleeveBase` 旧接口和 `FileSystem` 新接口存在重复实现，重构时应优先让应用层只依赖 `FileSystem` / `SleeveService`，再决定是否删除或降级 `SleeveBase`。

## Open Questions

- Root 是否第一版就改为虚拟视图，还是先保留 materialized Root membership 以降低改动量？
- 是否允许子文件夹里再创建子文件夹，还是只在数据层保留能力、UI 层禁止？
- 同一 File 在不同相册是否需要不同排序、显示名或封面状态？
- 独立副本的 edit history 是 fork 当前 version tree，还是只复制当前 pipeline snapshot？
- Delete from Root 是否应总是强确认，并提示会从所有相册移除？

## Recommended Next PR Scope

下一 PR 建议只做 Phase 3，不再把大库分页和缓存治理混进来：

- 审核并收紧 `PathResolver::ResolveForWrite()` 的 CoW 触发条件：
  - album membership 写入不触发 File CoW。
  - duplicate/payload sharing 才允许 detach。
- 修正 `SleeveFile::Copy()` 或直接收口为显式 duplicate API：
  - 新副本必须拥有独立 `file_id`。
  - 新副本必须拥有独立 edit history / pipeline。
- 补齐 duplicate 语义测试：
  - duplicate 后两个 File 修改互不影响。
  - album link 后在任一 album 修改仍共享同一 File。
- 保持 Phase 3 边界清晰：不要在同一 PR 里同时做 Root 虚拟视图、DB-first 缩略图分页或 object cache LRU；这些留给 Phase 4。

交接给 Phase 3/4 的关键假设：

- 旧项目不支持打开，版本线从 `0.2.5` 开始。
- Root 当前仍是 materialized membership，这是可接受的过渡实现，不应和 CoW/duplicate 语义改造绑在同一 PR。
- 当前底层允许多级 folder tree，但 UI 第一阶段仍可以只暴露两级相册结构。
- 编辑器和 edit history/pipeline 已以 `file_id` 为身份工作；后续 Phase 3 的重点是修正 duplicate/CoW，Phase 4 再处理大库查询与分页。
