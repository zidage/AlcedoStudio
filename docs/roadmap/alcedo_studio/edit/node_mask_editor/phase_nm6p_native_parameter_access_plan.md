# Phase NM6.P — 原生参数读写、面板投射与结果缓存修正

Date: 2026-09-05

Status: NM6.P1–NM6.P7 complete (NM6.4P implemented).

Parent: [NM6 execution plan](phase_nm6_node_aware_adjustments_plan.md).
Dependency: NM6.4 → NM6.P1–P6 → NM6.4P (NM6.P7) → NM6.5 → NM6.6 → NM6.7 → NM6.8 → NM6.9.

2026-09-06 范围补充：P1–P6 的完成记录保留。新增 P7 作为 NM6.4P 的唯一实施工作，
修正依赖版本与结果保留脱节的问题，并落实 QualityBase 在 RAW Develop 之后旁路结果缓存。
P7 已完成；NM6.5/6.6 仍未开始。

## 1. 为什么单列阶段

NM6.2 已经把滑动输入放进队列，NM6.3 建立串行消费，NM6.4 建立依赖版本。
这些成果保留，但它们不等于已经完成原生参数读写。当前生产链仍携带 JSON 字符串，
应用修改时仍读取完整 Model JSON 并合并回写，运行时仍有完整 DTO 读取。
这是横跨 Model、输入队列、history 边界、面板读取与 GPU 参数准备的工作，不能塞进
NM6.6 的节点上下文接入，更不能以新增适配层后保留旧链的方式宣布完成。

本阶段的目标是让开发者能够沿一条明确调用链回答：哪个 Model 拥有这个值，谁读取它，
哪个操作修改它，什么时候允许修改，以及哪里需要序列化。软件处理本进程内的图像编辑，
不需要按网络协议栈或通用 CRUD 框架组织参数更新。

## 2. 已查证的历史与教训

以下为 2026-09-05 本地源码/Git/roadmap 检查，不是重新验证旧阶段所有测试，也不据此
推断改动作者。没有统计完整历史的滑块净增/累计行数，不宣称具体上万行数值。

| Evidence | What it establishes | Consequence for this phase |
| --- | --- | --- |
| [CQ plan](../../ui/editor_session_command_queue_and_lock_simplification_plan.md), CQ1 residuals and CQ2 | 曾专门移除 GUI render-lock waits；CQ1 记录过 fake port 测试不能替代真实生产证明。 | 队列的单元测试不能证明整条 UI → owner 链不阻塞；必须覆盖生产入口。 |
| [render simplification plan](../../ui/editor_render_path_simplification_plan.md), R1–R4 | 明确指出 session/render 拆分后又增长了多层调度，并要求减少复杂度。 | 复用已完成的调度，不增加另一层请求/响应分发。 |
| [single live pipeline plan](../../ui/editor_single_live_pipeline_wal_checkpoint_plan.md) | 明确只有一个 live 参数状态，JSON 是持久化表示，历史身份不是逐次参数内容哈希。 | 本阶段不创建平行参数状态，不把序列化格式当内部 Model API。 |
| [7A repair plan](../../ui/phase_7a_history_versions_repair_and_ui_refactor_plan.md) | 记录了 history/presentation 重建与残留争用，并保留产品资格验证未完成项。 | 必须测真实使用链及其刷新范围，不能用增加一个 revision 信号替代读取成本检查。 |
| Git `273a32c7`, 2026-08-30 | 引入完整目标及 live PipelineDocument 写入。 | 保留精确目标，不恢复隐式 primary Grade。 |
| Git `ce7dd6b1`, 2026-08-30; blame ApplyModelPatch | 为原地修改及局部恢复加入 ToJson → copy → merge → LoadJson，当前仍在。 | “原地写同一个对象”不证明没有整个参数体中转。替换实际更新实现。 |
| Git `fa71cc54`, 2026-09-05; blame EditorPendingFieldChange | NM6.2 队列中的参数值仍是 params_json。 | typed identity 与 typed value 是不同完成项；保留队列行为，修改其值载体。 |
| Git `4f2b0760`, 2026-09-05 | NM6.3 serial adjustment consumption。 | 不重做消费/pacing/完成状态机。新参数操作在已有 owner 边界执行。 |

实施者先重新执行这些文件的 git log/blame，记录实际基点。旧方案中的完成记录保留历史
真实性；对于被本阶段替代的入口标明去向，不能改写旧记录来声称当时已解决当前问题。

## 3. 最终结构：数据、读取、修改各有明确归属

```text
滑动：控件本地值 → 待处理的具体字段修改 → 现有串行 owner
      → Model 明确更新操作 → 现有失效处理 → 渲染

读取：所选 Node → 实际 AdjustmentInstance → Model 类型化读取
      → 面板适配函数 → 现有 QML 展示模型

提交：同一次编辑的最终变化 → 现有 history Commit → WAL/项目序列化
```

| Component | Responsibility | Must not own |
| --- | --- | --- |
| Graph Node / operator Model | 参数值、类型、有效范围及局部不变量 | Qt 控件、面板布局、另一份 JSON 参数状态 |
| Existing session input queue | 尚未消费的具体修改和结束顺序 | live 参数全集、网络式路由、第二个调度器 |
| Existing serial owner | 验证并应用一批修改、历史边界、失效通知 | 每层重新封装的一份参数包 |
| Panel adapter | 实际 Model 与具体展示属性间的映射 | 整个节点副本、通用属性数据库 |
| Existing QML models | 展示所需的值与当前本地输入 | 图的可写入口、领域 Model 裸指针 |
| Serialization code | 项目、WAL、导入导出等明确边界 | 面板读取和普通参数应用 |

### 3.1 具体类型优先，不做通用消息系统

重用已有 NodeId、AdjustmentInstanceId、算子类型和字段枚举。标量用数值，选项用 enum，
相关字段用最小的类型化修改结构。不同操作需要统一队列存储时允许有限的 std::variant；
它应枚举实际编辑操作，而不是 string method + map arguments 的通用调用格式。
无需为每个标量增加一个 service、port、factory 或多态 command 类。

Patch/Commit 继续作为更新单位；Patch 描述改了什么，Commit 表示这次编辑结束后的历史
记录。真实的外部 JSON Patch 在边界解析一次，转换为相同的内部修改。保留项目/WAL格式。
不允许为应用一个 Patch 调用整个 Model 的 ToJson、复制合并后 LoadJson。

setter/getter 优先使用现有接口。曲线等容器使用 owner 范围内的 const 读取，或传实际改变
的点/新曲线；移动一个点不替换无关状态。完整替换确实需要新数据时转移其所有权。
原子更新先验证相关字段和资源，再完成一次 owner 更新；保留 NM4 必需的撤销信息。
不得将每次 provisional 修改的完整 JSON 备份作为通用失败恢复手段。

### 3.2 面板投射直接读取 Model

按实际节点的 AdjustmentInstanceId 读取，不能仅按算子类型猜测第一个实例。
使用具体适配函数和一个小的静态能力表即可。新增面板注册其支持节点、读取函数和展示
组件；复用标量/枚举映射，不新建动态 schema、通用反射或 JSON 属性树。
核心 Model 不依赖 Qt。通过已有应用层边界读取，不让 QML 越层访问可变图。

切换节点时在安全 owner 边界一致地读取该节点各个支持面板需要的值，填充现有展示模型。
必要的标量/展示数据传递允许；禁止 Node → JSON → Panel，以及 Node → 全部 FullDto
→ 新面板 DTO 集合 → Panel。若跨线程需要独立值，在定义处说明最小字段、源 owner、
一致性、GUI 投递寿命和过期丢弃规则；不保留 live 引用，不新增可写参数镜像。
普通编辑只回显必要字段，不刷新全部面板，也不覆盖更晚的本地输入。

### 3.3 复用串行渲染，而不是再造请求状态机

在 NM6.3 的帧间消费点应用修改。下一帧前参数保持稳定。内部是直接函数调用；只在
GUI/owner 的实际线程边界排队。保留现有取消/session校验、GPU完成和16 ms行为。
不增加消息 broker、通用 middleware、跨层确认/重试、参数路由服务或另一个事件总线。
错误由实际操作返回并交给现有 session 处理，不在每一层再次包装相同结果。

## 4. 子阶段与交付顺序

### NM6.P1 — 画清生产读写链并固定失败证据

**修改范围：** 核对并记录 scalar、enum、curve、LUT、RAW/ODT/lens、Geometry 的读写入口。
为每种载体记录 owner、实际字段、复制位置、JSON调用、线程边界与最终消费者。
只记录真实路径；不先增加抽象接口。

**主要调用链：** AdjustmentSlider/具体面板 → adjustment model → session → pending input
→ owner consume → ApplyModelPatch/history；反向追踪当前 snapshot/JSON → panel 路径。

**文件：** editor_adjustment_models、editor_session_controller、editor_pending_input、
editor_session_edit_controller、editor_pipeline_command_service、editor_adjustment_pipeline、
editor_history_mutation、EditorTonePanel/Look/LUT/RAW/Display/Geometry。

**记录：** [Native parameter access production path inventory](native_parameter_access_inventory.md)。

**验收：** 有当前全状态转换与复制的可运行证据，及保留行为的测试。测试允许历史持久化
序列化，却能单独禁止投射/应用参数路径的全量 JSON。明确每个旧接口的删除或边界保留位置。

#### NM6.P1 完成记录（2026-09-05）

**状态：** complete。

**分支：** 已从 `main` 的 `5c8acc56` 创建 `feature/nm6-native-parameter-access`，作为
整个 NM6.P 的持续工作分支；P2–P6 应继续在此分支推进，不再为 NM6.Px 单独拆分分支。

**交付内容：**

- 新增 [Native parameter access production path inventory](native_parameter_access_inventory.md)，
  逐项记录 scalar、enum/toggle、curve、LUT、RAW、ODT/display、lens 和 Geometry 的
  UI入口、实际 Model owner、字段、复制点、JSON边界、线程切换和最终消费者。
- 记录了成功链：

  ```text
  QML panel/model
    -> EditorSessionController::submitPatch
    -> EditorSessionService::EnqueueAdjustmentInput
    -> EditorPendingInputQueue::AdmitFieldChange
    -> session-owner PostCompletion / TryConsumePendingInput
    -> EditorSessionEditController::HandlePendingSequence
    -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
    -> ApplyEditorParameterPatch + MirrorPatchToExecutor
    -> existing render route
  ```

- 记录了失败链：目标或字段校验失败时在 Model 修改前返回；Model setter 抛错时由
  `ApplyModelPatch` 恢复受影响的 Model；executor mirror 失败时恢复 document；session edit
  controller 将错误标记为 rejected 并恢复未完成预览，不进入新的渲染/历史提交。
- 在现有 `EditorPipelineCommandServiceTest` 中增加可运行的当前行为证据：scalar 应用明确
  观察一次目标 `ToJson()` 和一次 `LoadJson()`；面板读取的目标 Model JSON 与单独的
  `CanonicalPipelineDocumentJson()` 持久化序列化分别计数，证明两者不是同一个验收断言。
- 对现有接口给出去向：普通编辑的 `params_json`、pending field payload、render snapshot
  patch 和 `ApplyEditorParameterPatch`/`ReadEditorParameterJson` 留作 P2–P4 的替换对象；
  PipelineEditBatch/WAL、checkpoint、import/export 等保留为明确序列化边界；
  `MakeFullDto`/`ParameterArena` 留给 P5 的运行时成本清理。

**验证命令与结果：** 使用 Windows/MSVC 包装脚本构建，所有测试二进制直接运行；本仓库当前
没有为这些目标注册 CTest 条目。

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPipelineCommandServiceTest
  -> PASS

cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPendingInputTest --target EditorPendingInputSessionTest --target EditorSerialInputBoundaryTest --target EditorAdjustmentPipelineTest --target EditorAdjustmentModelTest --target EditorAdjustmentSnapshotQmlTest
  -> PASS

EditorPipelineCommandServiceTest.exe --gtest_color=no
  -> 12/12 passed
EditorPendingInputTest.exe --gtest_color=no
  -> 11/11 passed
EditorPendingInputSessionTest.exe --gtest_color=no
  -> 3/3 passed
EditorAdjustmentPipelineTest.exe --gtest_color=no
  -> 7/7 passed
EditorAdjustmentModelTest.exe --gtest_color=no
  -> 13/13 passed
EditorAdjustmentSnapshotQmlTest.exe --gtest_color=no
  -> 1/1 passed
EditorSerialInputBoundaryTest.exe --gtest_color=no
  -> 7/7 passed
```

合计 54/54 passed。新增的两个参数命令服务测试与现有的 setter 恢复、复合值拒绝、
Geometry/Develop 预写校验、队列合并/释放、QML load-only 恢复测试共同固定了 P1 的失败
证据和保留行为。构建输出只有仓库已有的 MSVC/CUDA/第三方 warning，没有编译错误。

**P1 退出检查：**

- [x] 完成八类参数载体的生产读写入口和 owner/copy/thread/consumer inventory。
- [x] 完成 UI → session → pending input → owner consume → history/model → render 的真实链路记录。
- [x] 完成 live read/projection → snapshot → `BuildSnapshotMap` → QML load-only 的反向链路记录。
- [x] 以可执行计数测试区分普通参数读写中的完整 JSON 与合法 document persistence JSON。
- [x] 为当前旧接口标明保留边界或后续替换阶段；P1 没有新增双轨接口或降级路径。

**残留范围：** P1 没有提前实现 P2 的 typed Model update、P3 的 typed queue payload、P4
的 typed panel projection、P5 的 runtime full DTO narrowing 或 P6 的旧路径删除。这些是
明确的后续交付，不把当前 JSON 读写记录误报为已完成的原生访问。

### NM6.P2 — 通过 Model 的具体操作应用参数

**修改范围：** 补足 typed read/update API；以完整目标和实际字段修改替换全状态 JSON
merge/LoadJson。先完成一个 scalar 和一个相关字段/复杂容器用例，再覆盖全部现有算子。

**主要调用链：** typed Patch → existing owner → validate → Model update → dirty/invalidation。
历史提交仍使用已有 NM4 表示和持久化。对存储输入保留单次解析边界。

**文件：** include/edit/operators/models、graph endpoint models、editor_pipeline_command_service、
history 参数读取/应用边界及现有 Model tests。MakeFullDto 不作为新 getter 的实现。

**验收：** 普通和复杂修改不转换整个 Model JSON；失败不留部分更新；不影响无关字段；
相同规范化值不触发新失效。旧完整状态更新 helper 的生产调用者有明确迁移结果。

#### NM6.P2 完成记录（2026-09-06）

**状态：** complete。

**交付内容：**

- 为 scalar、Curve、LMT、HLS、Color Wheel、CAT02、Sharpen、Develop、Geometry 和 DRT/ODT
  补足具名读取与更新入口。`OperatorModelBase::MutateWithDirtyFields` 在同一 owner 锁内
  完成字段更新和 dirty mask 计算；等价规范化值返回空 mask，不触发新的 dirty/invalidation。
- `ApplyEditorParameterPatch` 改为按字段解析 JSON 边界对象，再调用具体 Model 操作；普通
  应用路径不再执行 `ToJson → merge → LoadJson`，也不构造完整 Model JSON 作为写入中转。
  复合输入先完整校验，再执行一次 owner update，因此非法数组、别名冲突、枚举或字段类型
  错误都在 Model 修改前返回。
- Color Grade 的 scalar/curve/LUT/色轮/HLS/CAT02、DRT/Post 的 scalar/Sharpen、Develop
  的 RAW/color temperature/lens calibration、Document Geometry 和 ODT 均已接入对应
  owner。无关字段与其它 Model 保持不变。
- `ReadEditorParameterJson`、Model `ToJson`/`LoadJson` 和历史 before/after 表示继续作为
  项目、历史、WAL、导入导出的 JSON 边界；历史回放先做现值校验，再把目标值交给 typed
  update。`MakeFullDto` 没有被用作新 getter 的实现。

**成功调用链：**

```text
QML/session pending edit
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> ApplyEditorParameterPatch
  -> field parser / target owner lookup
  -> ScalarOperatorModel::SetValue, CurveModel::SetPoints, or the matching typed ApplyUpdate
  -> OperatorModelBase::MutateWithDirtyFields
  -> dirty patch -> existing render and history flow

Develop field
  -> ApplyEditorParameterPatch
  -> DevelopParamsModel::ApplyRawDecodeUpdate /
     ApplyColorTemperatureUpdate /
     ApplyLensCalibrationUpdate
  -> MutateWithDirtyFields

ODT field
  -> ApplyEditorParameterPatch
  -> DrtParamsModel::ApplyUpdate
  -> MutateWithDirtyFields

History replay
  -> pipeline_history_applier.cpp::ApplySetParameter
  -> ReadEditorParameterJson (expected-side JSON boundary)
  -> ApplyEditorParameterPatch
  -> typed owner update
```

失败链为：目标校验或 field parser 失败 → 在 owner update 前返回；复合值的所有字段均已
解析并校验后才进入一次 typed update。历史或 executor 后续步骤失败时，现有调用方继续使用
原有的 document restore/rejection 路径，不会把部分字段作为成功结果发布。

**验证命令与结果：**

```text
cmd /c scripts\msvc_env.cmd --build build/debug --target EditorPipelineCommandServiceTest --parallel 4
  -> PASS
ctest --test-dir build/debug -R '^EditorPipelineCommandServiceTest\.' --output-on-failure
  -> 19/19 passed

cmd /c scripts\msvc_env.cmd --build build/debug --target EditorSessionHistoryPortTest --parallel 4
  -> PASS
ctest --test-dir build/debug -R '^EditorSessionHistoryPortTest\.' --output-on-failure
  -> 75/75 passed
ctest --test-dir build/debug -R '^PipelineHistoryApplierTest\.' --output-on-failure
  -> 12/12 passed
```

构建输出只有仓库已有的 `vswhere` 查找、MSVC `getenv`、CUDA 架构和第三方头文件 warning，
没有编译或测试错误。

**P2 退出检查：**

- [x] scalar 与复杂字段都通过具体 owner 的 typed read/update API 读写。
- [x] 普通 `ApplyEditorParameterPatch` 不再对目标 Model 做完整 JSON 转换、合并和回写。
- [x] 复合输入在第一次 owner mutation 前完成完整解析；非法输入不会留下部分更新。
- [x] 无关字段、无关 Model 和 graph topology 保持不变；相同规范化 scalar 不触发 dirty。
- [x] 历史/项目/WAL/import/export 的 JSON 表示仍在明确边界保留，历史回放已接入 typed apply。
- [x] 目标测试、历史端口测试和历史回放测试均以实际目标构建并通过。

**LOC 与维护说明：** 本次实现涉及 21 个 C++ 源/头/测试文件，另更新本计划记录；新增的
JSON 代码是字段边界解析器，不是新的全量 State/Context/Payload 镜像。字段更新载体只保留
调用一次 owner operation 所需的 optional 字段；完整 JSON 只在既有持久化/历史边界出现。

**残留范围：** P2 有意不改队列中的 `params_json` 载体、P4 的 typed panel projection、
P5 的 runtime full DTO narrowing 和 P6 的旧路径清理；这些仍由 P3–P6 完成。历史和项目
持久化的 JSON 边界是本阶段保留的明确接口，不属于普通 live Model 写入路径。

### NM6.P3 — 队列直接持有最小修改，完整切换写入路径

**修改范围：** 既有队列采用最小类型化值/操作，消费时转移字段集合。删除多次转换和
复制的 pending field → JSON patch → render adjustment 中转。保留现有合并与结束顺序。
所有生产控件写入切换到 P2 操作，不能仅新建一个未接入的 typed submit 方法。

**主要调用链：** local UI value → existing queue → owner consume → P2 Model update。

**文件：** editor_adjustment_submitter/models/controller、editor_pending_input、session service/
edit controller、具体 QML 编辑入口。复用调度器，不再增加 command queue 层。

**验收：** scalar/enum/toggle/curve/LUT/RAW/ODT/lens/Geometry 的真实入口均走新路径；
独立字段不丢失、release保留、live写入仍在帧间、一次输入序列提交一次。队列不复制
整批再清空原值。超出本阶段的持久化接口保留不代表旧普通参数通道可以继续使用。

#### NM6.P3 完成记录（2026-09-06）

**状态：** complete。live 写入路径已切到 typed field write；面板 JSON 投射、运行时完整
DTO 和旧 helper 删除仍由 P4–P6 负责。

**交付内容：**

- 队列项 `EditorPendingFieldChange` 只持有 `EditorParameterWrite`，不再持有 live
  `params_json`。`AdmitFieldChange` 拒绝缺少 typed write 的输入。`TakeReadyBatch`
  对未封口序列执行 `fields = std::move(open_->fields)`，并清空索引，序列保持打开。
- `HandlePatch` / `PatchFromPendingField` / `CaptureAdjustmentBeforePreview` 复制 typed
  write，不再把 pending field 先编成 JSON 再应用。Capture 调用
  `ApplyEditorParameterWrite`；Commit 从 live Model 读取 after 值，不再用 settled JSON
  二次回写。缺少 write 时 Capture/Commit 均失败且不发布历史。
- 生产控件：`IEditorAdjustmentSubmitter::submitWrite` 是 GUI 入队入口。C++ 模型直接
  构造 `EditorScalarWrite` / `EditorEnumWrite` / `EditorToggleWrite` / `EditorCurveWrite` /
  `EditorLutWrite` / `SharpenUpdate` / `HlsUpdate` / `ColorWheelUpdate` /
  `DevelopColorTemperatureUpdate`。QML 收集边界（RAW / ODT / lens / Geometry crop）仍可
  `submitPatch`，但只在 GUI 线程解析一次后转入 `submitWrite`。队列与 owner 消费不再走
  JSON 合并。Tone/Look 标量不再包一层 `paramsBuilder`。
- `EditorRenderAdjustmentSnapshot::params_json` 仍作为 P4 投射载体保留；live 入队不得
  依赖它。历史 / WAL / 项目 JSON 边界保留，不等于 live 通道可以继续用 JSON。

**成功调用链：**

```text
QML/model local value
  -> EditorSessionController::submitWrite
     (submitPatch only at QML collection boundary: ParseEditorParameterWrite once)
  -> EditorSessionService::EnqueueAdjustmentInput
  -> EditorPendingInputQueue::AdmitFieldChange (typed write required)
  -> EditorPendingInputQueue::TakeReadyBatch (move open fields)
  -> EditorSessionEditController::HandlePendingSequence
  -> PatchFromPendingField (copy write, not JSON)
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview
  -> ApplyEditorParameterWrite
  -> MirrorTargetToExecutor / Remirror
  -> existing serial render (live_parameters_applied)
```

**失败调用链：**

```text
missing typed write or invalid QML JSON
  -> AdmitFieldChange / submitPatch reject on GUI thread
  -> document and history head unchanged

ApplyEditorParameterWrite fails
  -> Capture restores before JSON
  -> HandlePatch Rejected; no new render or history commit

executor remirror fails
  -> document restored from before JSON
  -> no published preview

CommitAdjustment without write, or before JSON equals after JSON
  -> no history publish (missing write fails; identical values skip a new commit)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| missing write rejected; consume moves fields; 9 independent typed writes (scalar/curve/LUT/RAW/ODT/lens/Geometry/enum/toggle) | `EditorPendingInputTest` (`RejectsMissingTypedFieldWrite`, `TakeReadyBatchTransfersOpenFieldWritesAndLeavesSequenceOpen`, `OpenSequenceHoldsIndependentTypedWritesUntilMovedOnConsume`) | PASS |
| merge, release, cancel, node-switch, independent fields | `EditorPendingInputTest` + `EditorPendingInputSessionTest` | PASS |
| scalar / enum / toggle native `submitWrite`; one settled on release | `EditorAdjustmentModelTest` | PASS |
| curve / LUT / color temp / HLS / CDL native writes; one settled | `EditorToneCurveModelTest`, `EditorLookModelTest` | PASS |
| Capture applies typed write with empty `params_json`; invalid write leaves live value and history head | `EditorSessionHistoryPortTest` (`CaptureAppliesTypedScalarWriteWhenParamsJsonIsEmpty`, `InvalidParameterLeavesLiveValueAndHistoryHeadUnchanged`, `SettledAdjustmentCreatesOneCommitAndUndoRedoMovesHead`) | PASS |
| inter-frame consume; release commits once | `SerialFrameConsumptionTest` (`ReleaseBeforeFirstPreviewCommitsFinalValuesOnce`) | PASS |
| RAW panel → `DevelopRawDecodeUpdate` | `EditorRawDecodePanelQmlTest.UserChangesSubmitCompleteRawOperatorParams` | PASS |
| Geometry crop → `ImageGeometryUpdate`; lens → `DevelopLensCalibrationUpdate` | `EditorGeometryPanelQmlTest` (crop confirm + `LensSelectionKeepsLegacyDefaultsAndIsAvailableWhenDisabled`) | PASS |
| ODT method → `DrtParameterUpdate` | `EditorDisplayTransformSnapshotQmlTest.MethodChangeEnqueuesDrtParameterUpdate` | PASS |
| dual-slider typed submit + snapshot cascade stays pumpable | `EditorLookPanelInteractionTest.RapidMultiSliderHandoffKeepsEventLoopResponsiveUnderSnapshotCascade` | PASS after model-drag fallback (offscreen Loader mouse did not enqueue) |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target <write-path binaries>
ctest --test-dir build/debug --output-on-failure
  -> write-path + history subset 115/115 (build/tmp/nm6p/p3_core_ctest2.txt)

ctest --test-dir build/debug -R '^EditorSessionHistoryPortTest\.' --output-on-failure
  -> 76/76 (build/tmp/nm6p/p3_history_ctest.txt)

set QT_QPA_PLATFORM=offscreen
ctest --test-dir build/debug --output-on-failure
  -> QML/session sweep 143/145 (build/tmp/nm6p/p3_qml_ctest.txt);
     RapidMultiSlider re-run PASS (build/tmp/nm6p/p3_look_retry2.txt)
```

**Checklist / exit condition:** P3 正文无 checkbox。验收项均有具名测试：八类真实入口走 typed
write；独立字段保留；release 保留；帧间 live 写入；一次序列一次提交；队列 move 而非
复制后清空。生产控件写入均入队 P2 Model 操作，不是闲置 typed API。

**LOC note (grill-code-review):** `editor_parameter_write.cpp` 1003 行（历史/QML 收集边界
解析器占大部分，接近拆分阈值）；`editor_pending_input.cpp` 225；
`editor_session_edit_controller.cpp` 303；`editor_adjustment_models.cpp` 460。
既有大文件：`editor_session_controller.cpp` 1403、`editor_history_mutation.cpp` 1013、
`editor_pipeline_command_service.cpp` 1049。P3 未再新增全量 State/Context/Payload 镜像。
解析器拆分留待 P6，不在本阶段把 JSON 边界解析器拆成空转发文件。

**Residual gaps:** P4 面板 typed 投射、P5 运行时完整 DTO、P6 删除旧 helper。
`EditorRenderAdjustmentSnapshot::params_json` 仍给面板加载用。
`EditorSessionCommandQueueBaselineTest.RapidImageSelectionKeepsRunningTargetAndReplacesOnlyUnstartedSelection`
失败是图像切换/命令队列选择晋升，测试自身已标明未完成，不是 live 参数写入路径。
Look 双滑块在 offscreen Loader 上 QTest 鼠标未产生 enqueue，测试回退到模型
`beginDrag`/`updateDrag`/`finishDrag` 证明 typed `submitWrite`；不恢复 JSON live 通道。

### NM6.P4 — 直接投射 Model 到现有面板模型

**修改范围：** 实现第3.2节所述具体适配读取，替换全节点/算子JSON和完整DTO中转。
统一少量能力/适配注册。先支持现有上下文；NM6.6只负责将其接到真正的所选节点。
给后者提供明确的 NodeId/实例读取入口，不依赖隐式 PrimaryGrade。

**主要调用链：** owner scoped Node read → typed adapter → GUI presentation values。

**文件：** editor_adjustment_pipeline、session presentation读取、各面板/model load-only API。
保留现有控件与布局；不在本阶段重做NM6.7的节点标题/EXIF布局。

**验收：** 多种面板一次加载正确；无 ToJson/LoadJson/全DTO投射；QML加载不提交；
跨线程旧投递被丢弃；面板类型扩展示例只增加具体适配/注册而无需改全局JSON解析器。

##### Phase NM6.P4 completion record (2026-09-06)

**Status:** complete — typed Model → existing panel projection; GUI no longer parses node/operator JSON for `loadFromSnapshot`.

**Primary success call chain:**

```text
owner lock / live PipelineDocument
  -> ReadEditorPanelField / ProjectCurrentPanelFields
     (explicit NodeId + AdjustmentInstanceId; current-panel helper only)
  -> HistoryWorkingState.panel_projection
     (full project on refresh; UpsertEditorPanelField after live write)
  -> EditorSessionService::panel_projection (stamp session_generation)
  -> EditorSessionController discard-or-apply
  -> PanelProjectionToVariantMap / ApplyPanelProjectionToSnapshotMap
  -> existing QML loadFromSnapshot (load-only)
```

**Primary failure call chain:**

```text
incomplete target / missing node / missing instance / wrong owner
  -> ReadEditorPanelField / ProjectEditorPanelFields return false
  -> output projection left unchanged; no panel value written

stale session_generation != EditorSessionController::SessionEpoch
  -> OnBackendChanged drops the copy
  -> cached adjustmentSnapshot unchanged

QML loadFromSnapshot / setSelectedPath
  -> load-only setters; submitCount stays 0
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ProjectsToneLookLutRawOdtAndGeometryFromExplicitInstances` | `EditorPanelProjectionTest` | PASS |
| `ReadsNamedInstanceNotFirstOperatorOfType` | `EditorPanelProjectionTest` | PASS |
| `MissingNodeOrInstanceFailsBeforeAnyPanelValue` | `EditorPanelProjectionTest` | PASS |
| `PanelProjectionDoesNotCallModelJsonOrFullDto` | `EditorPanelProjectionTest` | PASS |
| `AdditionalPanelAdapterDoesNotChangeParameterWriteParser` | `EditorPanelProjectionTest` | PASS |
| `StaleSessionGenerationLeavesPanelSnapshotUnchanged` | `EditorSessionControllerPhase5ATest` | PASS |
| `SameSessionProjectionMergesChangedFieldsOnly` | `EditorSessionControllerPhase5ATest` | PASS |
| `NewSessionGenerationReplacesPanelSnapshot` | `EditorSessionControllerPhase5ATest` | PASS |
| `SnapshotIncludesTypedPanelValues` | `EditorSessionControllerPhase5ATest` | PASS |
| `QmlLoadFromTypedProjectionDoesNotSubmit` | `EditorAdjustmentSnapshotQmlTest` | PASS |
| `LiveWriteProjectsTypedExposureWithoutReadingParamsJson` | `EditorSessionHistoryPortTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPanelProjectionTest EditorSessionControllerPhase5ATest EditorAdjustmentSnapshotQmlTest EditorSessionHistoryPortTest
build\debug\alcedo_studio\tests\app\EditorPanelProjectionTest_runtime\EditorPanelProjectionTest.exe
build\debug\alcedo_studio\tests\app\EditorSessionControllerPhase5ATest_runtime\EditorSessionControllerPhase5ATest.exe
set QT_QPA_PLATFORM=offscreen
build\debug\alcedo_studio\tests\ui\EditorAdjustmentSnapshotQmlTest_runtime\EditorAdjustmentSnapshotQmlTest.exe
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
```

Logs: `build/tmp/nm6p/p4_projection.txt`, `p4_controller.txt`, `p4_qml.txt`, `p4_history.txt`.

Suite totals: `EditorPanelProjectionTest` 5/5; `EditorSessionControllerPhase5ATest` 49/49; `EditorAdjustmentSnapshotQmlTest` 2/2; `EditorSessionHistoryPortTest` 77/77.

**Checklist / exit condition:** P4 正文无 checkbox。验收项均有具名测试：多面板一次 typed 读取；显式实例而非 first-by-type；缺 node/instance 不写任何面板值；投射路径 ToJson/MakeFullDto 为 0 且 `CanonicalPipelineDocumentJson` 仍可单独序列化；新适配只 `table.Add` 不改 `ParseEditorParameterWrite`；过期 `session_generation` 丢弃；同会话只合并变更字段；新会话替换；QML load-only 不 submit。生产 `OnBackendChanged` 不再 `QJsonDocument::fromJson` 解析 `params_json`。

**LOC note (grill-code-review):** `editor_panel_projection.cpp` 556、`editor_panel_projection.hpp` 231；`editor_panel_presentation.cpp` 160；`editor_session_controller.cpp` 1391（去掉 JSON `BuildSnapshotMap`）；`editor_history_mutation.cpp` 1038；`editor_panel_projection_test.cpp` 383。既有大文件 `editor_session_service.cpp` 1800、`editor_session_controller_phase5a_test.cpp` 1566 未再并入新的全量参数镜像。控制器仍兼生命周期与面板投递；P4 只替换读取载体，不在本阶段拆控制器。

**Residual gaps:** Geometry `source_size` / `aspect_ratio_preset` 仍在 CPU `CropRotateOp`，不在 `ImageGeometryModel`；镜头 maker/model 仍是图像本地 CPU extras，不在 `DevelopParamsModel`。P4 只投射 document 拥有的 crop/rotation/`lens_enabled`。`MakeAdjustmentSnapshotFromLivePipeline` 与 `EditorRenderAdjustmentSnapshot::params_json` 仍给渲染/持久化快照，删除留给 P6。所选节点路由由 NM6.6 把目标列表传入 `ProjectEditorPanelFields`。P6 旧 helper 删除仍 planned。

### NM6.P5 — 去掉运行时完整参数体中转

**修改范围：** MakeGradeRuntimeParams、neighbor参数准备从安全 owner读取必要字段，
直接填既有GPU参数布局。只更新发生变化的槽位。清除为读取几个值而生成FullDto的路径。

**主要调用链：** stable Model values → necessary GPU parameter fields → existing ParameterArena。

**文件：** adjustment_runtime、operator_model_base读取接口、三backend现有Grade参数准备。
仅统一读数/打包入口，不提前改NM6.5的Grade/LLF执行编排。

**验收：** 未改槽位不重新打包；必要GPU上传之外无完整Model复制；CPU准备数值与
现有期望参数一致；CUDA/OpenCL/Metal调用同一类型化参数准备代码。

##### Phase NM6.P5 completion record (2026-09-06)

**Status:** complete — Grade GPU packing reads owner fields into the existing ParameterArena layout; unchanged slots are not repacked.

**Primary success call chain:**

```text
stable Color Grade / DRT-Post Model (owner lock)
  -> MakeGradeRuntimeParams / MakeGradeNeighborParams
     (OperatorModelBase::Read of only GPU-needed fields)
  -> BindOrRefreshGradeRuntimeSlot
     (pack only when the slot is missing or the Model is dirty)
  -> ParameterArena::WritePackedSlot
  -> ParameterArena::UploadDirty
  -> PendingParameterPatch::Commit
  -> existing CUDA / OpenCL / Metal Grade dispatch

LLF slider / Metal neighborhood enable
  -> PackedGradeControlValue from the arena host mirror
  -> no second Model FullDto copy
```

**Primary failure call chain:**

```text
Model type does not match AdjustmentBehavior, or DTO-only stand-in
  -> MakeGradeRuntimeParams throws before any slot write
  -> MakeFullDto is not called

WritePackedSlot size mismatch
  -> ParameterArena throws; host slot unchanged

UploadDirty throws after a dirty pack
  -> PendingParameterPatch destructor RestoreDirty
  -> Model remains dirty for retry
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PackedGradeParamsMatchOwnerFieldsWithoutFullDtoCopy` | `GpuDagRawInputTest` | PASS |
| `NeighborhoodParamsReadOwnerFieldsWithoutFullDtoCopy` | `GpuDagRawInputTest` | PASS |
| `GradePackingRejectsDtoOnlyModelWithoutReadingFullDto` | `GpuDagRawInputTest` | PASS |
| `GradePackingRejectsMismatchedModelTypeWithoutFullDtoCopy` | `GpuDagRawInputTest` | PASS |
| `GradeRuntimeSlotWritesPackedBytesOnlyWhenDirty` | `GpuDagRawInputTest` | PASS |
| `WritePackedSlotRejectsSizeMismatch` | `GpuDagRawInputTest` | PASS |
| `TakeDirtyFieldsClearsBitsWithoutCopyingFullDto` | `GpuDagModelGraphTest` | PASS |
| `CudaGradeParameterBindDoesNotCopyFullDto` + exposure-only upload + neighbor/LLF pixels | `GpuDagCudaPrimaryGradeTest` | PASS 36/36 |
| `OpenClGradeParameterBindDoesNotCopyFullDto` + OpenCL Grade/LLF/multi-Grade | `GpuDagOpenClGradeTest` | PASS 60/60 |
| existing ParameterArena dirty-range / no-upload tests | `GpuDagCudaWorkspaceTest` | PASS 24/24 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagRawInputTest GpuDagModelGraphTest GpuDagCudaPrimaryGradeTest GpuDagCudaWorkspaceTest GpuDagOpenClGradeTest
build\debug\alcedo_studio\tests\edit\GpuDagRawInputTest_runtime\GpuDagRawInputTest.exe --gtest_filter=GpuDagAdjustmentRuntime.*
build\debug\alcedo_studio\tests\edit\GpuDagModelGraphTest_runtime\GpuDagModelGraphTest.exe --gtest_filter=GpuDagModelGraph.TakeDirtyFields*
build\debug\alcedo_studio\tests\edit\GpuDagCudaWorkspaceTest_runtime\GpuDagCudaWorkspaceTest.exe --gtest_filter=CudaWorkspaceFixture.*
build\debug\alcedo_studio\tests\edit\GpuDagCudaPrimaryGradeTest_runtime\GpuDagCudaPrimaryGradeTest.exe
build\debug\alcedo_studio\tests\edit\GpuDagOpenClGradeTest_runtime\GpuDagOpenClGradeTest.exe
```

Logs: `build/tmp/nm6p/p5_adjustment_runtime.txt`, `p5_dirty_fields.txt`, `p5_cuda_workspace.txt`, `p5_cuda_grade.txt`, `p5_opencl_grade.txt`.

Suite totals: `GpuDagAdjustmentRuntime` 9/9; dirty-field subset 4/4; `GpuDagCudaWorkspaceTest` 24/24; `GpuDagCudaPrimaryGradeTest` 36/36; `GpuDagOpenClGradeTest` 60/60.

**Checklist / exit condition:** P5 正文无 checkbox。验收项均有具名测试：未改槽位不 `WritePackedSlot`；packing 路径 `MakeFullDto` 计数为 0；CPU packed values 与 owner getters 一致；CUDA 与 OpenCL 生产 Grade 入口调用同一 `BindOrRefreshGradeRuntimeSlot` / `MakeGradeNeighborParams`。Metal 源码已接入同一 helper，本机 Windows 未执行 Metal 二进制。

**LOC note (grill-code-review):** `adjustment_runtime.cpp` 307；`grade_parameter_slot.hpp` 62；`parameter_arena.hpp` 207；`cuda_primary_grade_pass.cu` 537；`opencl_primary_grade_pass.cpp` 514；`metal_primary_grade_pass.mm` 453；`adjustment_runtime_test.cpp` 281。均低于 1000 行拆分阈值。未新增全量 State/Context/Payload 镜像。

**Residual gaps:** Develop / DRT output 仍把已解析的 GPU 结构写入 `InitializeFromFullDto`（不是 `MakeFullDto` 读几个标量）；字段级 Model payload 上传仍给 workspace Sharpen 测试使用。删除这些剩余 helper 属于 P6。`EditorRenderAdjustmentSnapshot::params_json` 仍给 P6。Metal Grade packing 未在本机执行。无 `grade.primary` 节点时 `PrimaryGrade()` 仍回退到 backbone 上第一个 Color Grade；相关测试改为断言 `FindNode("grade.primary") == nullptr`，不在本阶段改回退规则。

### NM6.P6 — 删除被替代路径并验证整条生产链

**修改范围：** 完成旧接口去向表，删除无生产用途的JSON投射/全状态merge更新/重复DTO
包装与转发helper；修正测试伪实现，避免它们旁路真实参数路径。写最终简短维护说明。

**主要调用链：** 真实控件 → existing queue → Model owner → render → release/history
→ Undo/reopen → typed read → 真实面板。三后端保持既定质量和所有权行为。

**验收：** P1每个旧入口都有删除、明确序列化边界保留或真实新调用方；不能以“以后清理”
结束此阶段。真实QML/session测试与像素/参数回归通过。明确统计中转类型、JSON调用、
完整payload复制和无意义转发的去除情况，不以新增测试数量或总行数作为可维护性证明。

维护说明：[Native parameter access](native_parameter_access.md)。去向表写在
[inventory destinations](native_parameter_access_inventory.md#destinations-of-the-recorded-interfaces)。

##### Phase NM6.P6 completion record (2026-09-06)

**Status:** complete — deleted leftover JSON/DTO helpers; live packing writes packed GPU structs; live snapshot no longer dumps stage JSON.

**Primary success call chain:**

```text
QML/model local value
  -> submitWrite (submitPatch only at QML JSON collection boundary)
  -> EnqueueAdjustmentInput / AdmitFieldChange (typed write required)
  -> TakeReadyBatch (move fields)
  -> HandlePendingSequence
  -> CaptureAdjustmentBeforePreview
  -> ApplyEditorParameterWrite
  -> RemirrorEditorParameterToExecutor (ReadEditorParameterJson -> CPU SetOperator)
  -> serial render with live_parameters_applied
  -> CommitAdjustment (history before/after JSON)
  -> ProjectCurrentPanelFields / ReadEditorPanelField -> QML loadFromSnapshot

GPU CameraColor / DRT output
  -> already-resolved GPU struct
  -> ParameterArena::BindOrWritePackedSlot
  -> UploadDirty
  -> kernel dispatch (MakeFullDto count stays 0)
```

**Primary failure call chain:**

```text
missing typed write or invalid QML JSON
  -> AdmitFieldChange / submitPatch reject on GUI thread
  -> document and history head unchanged

ApplyEditorParameterWrite fails
  -> Capture restores before JSON
  -> HandlePatch Rejected; no new render or history commit

packed slot size mismatch or UploadDirty throws
  -> ParameterArena throws / restores pending ranges
  -> TakePendingDirtyFields destructor RestoreDirty when used
```

**Removed carriers (not test count / LOC):**

- Forwarding alias `PublishEditorParameterPatch`
- `ParameterArena::InitializeFromFullDto`, `ApplyPatch`, `CopyFields`
- Live `ExportPipelineParams().dump()` into `EditorRenderAdjustmentSnapshot::params_json`
- `OperatorParamDto` / `TypedOperatorParamPayload` wrap of `CameraColorGpuParams` and DRT GPU structs on CUDA/OpenCL/Metal
- Workspace test helper `UploadFullAndClearDirty` (field-range DTO `ApplyPatch`)

JSON that remains is listed in the inventory destination table (history/WAL/project, CPU remirror, `submitPatch` parse, DRT `ToJson` → `ODT_Op`).

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `InitialAdjustmentSnapshotContainsEverySupportedFieldBeforeAnyRender` (`params_json` empty; per-field JSON kept) | `EditorSessionHistoryPortTest` | PASS 77/77 |
| `CameraColorPackedSlotWriteDoesNotCopyFullDto` | `GpuDagRawInputTest` (`GpuDagAdjustmentRuntime`) | PASS 10/10 |
| `CudaCameraColorPackedWriteDoesNotCopyFullDto` / `CudaDrtPackedWriteDoesNotCopyFullDto` + Grade pixels | `GpuDagCudaPrimaryGradeTest` | PASS 38/38 |
| `OpenClCameraColorPackedWriteDoesNotCopyFullDto` + Grade pixels | `GpuDagOpenClGradeTest` | PASS 61/61 |
| `CudaDrtPackedWriteDoesNotCopyFullDto` | `GpuDagCudaDrtProductTest` | PASS 52/52 |
| `OpenClDrtPackedWriteDoesNotCopyFullDto` | `GpuDagOpenClDrtProductTest` | PASS 16/16 |
| `ParameterArenaWritePackedSlotUploadsBoundSlotOnce` + failure restore | `GpuDagCudaWorkspaceTest` | PASS 24/24 |
| OpenCL packed-slot upload + failure restore | `GpuDagOpenClWorkspaceTest` | PASS (packed-slot tests); see residuals for unrelated header scan |
| typed write / JSON parse boundary | `EditorPipelineCommandServiceTest` | PASS 19/19 |
| queue typed writes | `EditorPendingInputTest` | PASS 13/13 |
| typed panel projection | `EditorPanelProjectionTest` | PASS 5/5 |
| session controller + QML snapshot restore | `EditorSessionControllerPhase5ATest` / `EditorAdjustmentSnapshotQmlTest` | PASS 49/49 and 2/2 |
| Model DTO API still covered (`TakePendingParameterPatch`) | `GpuDagModelGraphTest` | PASS 66/66 |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target EditorPipelineCommandServiceTest GpuDagRawInputTest GpuDagModelGraphTest GpuDagCudaWorkspaceTest GpuDagOpenClWorkspaceTest GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest GpuDagCudaDrtProductTest GpuDagOpenClDrtProductTest EditorSessionHistoryPortTest EditorPanelProjectionTest EditorSessionControllerPhase5ATest EditorAdjustmentSnapshotQmlTest EditorPendingInputTest

build\debug\alcedo_studio\tests\app\EditorPipelineCommandServiceTest_runtime\EditorPipelineCommandServiceTest.exe
build\debug\alcedo_studio\tests\edit\GpuDagRawInputTest_runtime\GpuDagRawInputTest.exe --gtest_filter=GpuDagAdjustmentRuntime.*
build\debug\alcedo_studio\tests\edit\GpuDagCudaPrimaryGradeTest_runtime\GpuDagCudaPrimaryGradeTest.exe
build\debug\alcedo_studio\tests\ui\EditorSessionHistoryPortTest_runtime\EditorSessionHistoryPortTest.exe
```

Logs: `build/tmp/nm6p/p6_*.txt`.

**Checklist / exit condition:** P6 正文无 checkbox。P1 旧入口均已删除、保留为序列化边界、或写出当前调用方。维护说明覆盖 scalar/complex 链、如何加算子/面板、序列化边界。生产链由 session/QML/history 与 CUDA/OpenCL 像素及 `MakeFullDto` 计数共同证明。Metal 源码已接入同一 packed write，本机 Windows 未执行 Metal 二进制。

**LOC note (grill-code-review):** `editor_parameter_write.cpp` 257（仅 typed apply）；`editor_parameter_write_parse.cpp` 780（JSON 收集边界）；`parameter_arena.hpp` 205；`cuda_camera_color_pass.cu` 107；`cuda_drt_pass.cu` 215；`opencl_develop_pass.cpp` 616；`opencl_drt_pass.cpp` 256；`metal_develop_pass.mm` 520；`metal_drt_pass.mm` 273；`editor_history_shared_helpers.cpp` 513。均低于 1000 行。解析与 apply 按职责拆分，不是空转发。未新增全量 State/Context/Payload 镜像。

**Residual gaps:** DRT GPU 表仍经 `ToJson` → `ODT_Op`（GPU 准备边界，不是 live Model 写入）。CameraColor 每帧重写 packed slot（`const PipelineDocument&`，无 dirty-take）。CPU remirror 仍 `ReadEditorParameterJson`。`submitPatch` 仍是 QML 收集边界。Geometry `source_size` / lens maker-model 仍是 CPU extras。无 `grade.primary` 时 `PrimaryGrade()` 仍回退到第一个 Color Grade。Metal packing 未在本机执行。`GpuDagOpenClWorkspace.RendererTemplateInstantiatesOpenClWithoutCudaOrMetalHeaders` 因 `renderer.hpp` 注释含 `Metal/` 失败，本阶段未改该头文件，与参数路径无关。NM6.5/6.6 未开始。

### NM6.P7 — NM6.4P 依赖驱动的结果保留与 QualityBase 缓存旁路

**状态：** complete 2026-09-06。产品规则已落地；验收矩阵在 Windows/MSVC 上执行（CUDA/OpenCL/CPU session）。Metal 测试已写入 `metal_renderer_test.cpp`，本机未执行。

**问题与证据：** NM6.4 提交 `90f6cca2` 删除 `current_` 的当前结果保护，改为只跳过
有 write slot 的输出。`GraphImageCache::EvictCompletedUnleased` 不读取依赖失效状态，
下游分配可以按 LRU 删除有效 Develop。其调用先于 `TexturePool::Acquire` 的空闲复用，
且删除 published lease 不立即减少 UsedBytes，导致额外删除甚至清空结果。
2026-09-06 审查使用生产缓存头文件与轻量测试纹理后端，新编译的最小复现分别观察到：
已有匹配空闲纹理仍删除 Develop；只需回收一个分配却删除全部三个已发布结果。
临时证据位于 `build/tmp/nm64-cache-review/cache_retention_check.cpp`，不作为永久测试依赖。
现有 debug 二进制中 RuntimeInvalidation 23/23 与所选 CUDA 缓存 3/3 通过，不能证明预算
压力下的结果保留。实施时在实际基点重建测试，加入永久回归用例。

**确定的产品规则：**

1. 结果是否需要重算由现有 owner 依赖版本和像素表示决定，不由最近访问时间决定。
   当前图像的有效 `develop:sensor_linear` 必须保留；下游参数变化或纹理申请不得淘汰它。
   有效的 Interactive `geometry:scene_source`（2560px）及其余保留结果同样不参加预算 LRU。
   Develop、Geometry 或源数据实际变化时，按真实依赖使相应结果失效，不保留错误旧结果。
2. 已失效的下游旧结果，在最后一个 GPU/显示读者释放后回收或复用。有效内容、未发布写入、
   正在读取的旧输出和空闲分配具有不同生命周期；不得以旧内容失效为由覆盖仍被读取的内存。
   不保留历史参数版本，也不增加全量参数副本或独立失效图。
3. QualityBase 可以读取有效 RAW Develop；需要重算 Develop 时按相同成功发布规则更新它。
   旁路边界是 `develop:sensor_linear` 之后、Geometry/resize 之前，不是名称为
   `develop:image` 的 CameraToAP1 输出之后。
4. QualityBase 的 Geometry、CameraToAP1、Grade、LLF source/result、Mask 及 DRT/Post
   全部旁路持久结果缓存的读取与写入，涵盖图像、缓冲区和结果元数据。结果仅供本次提交
   内部消费者和显示端使用；按最后使用点释放，最终输出保留至显示端归还使用权。
   任务内部传递刚产生的结果不是跨帧缓存读取。参数槽上传与空闲纹理分配复用仍可使用。
5. 不为 QualityBase/4K 保留跨帧结果槽位，也不按输出角色复制整套参数或执行器。
   QualityBase 不得替换、驱逐、清空 Interactive 结果，亦不得把其下游 completed revision
   标记为已完成。真正的参数修改仍先传播失效；旁路不掩盖 Interactive 缓存已经过期的事实。
6. 连续 QualityBase 多见于 Develop/Geometry 调整，下游本就需要重算；另一条生产序列是
   QualityBase → Interactive → QualityBase，Interactive 使用自身 resize 后的结果，
   无需保留 QualityBase 下游缓存。不得以兼容此序列为由增加 4K 常驻槽位。
7. 先复用空闲分配，再回收已失效且无读者的结果。底层只可对无结果所有者且无读者的空闲
   分配选择回收次序；不能反过来扩大管线重算范围。必需保留结果和在用资源无法满足新分配时，
   返回真实资源不足错误，不删除有效 Develop，不降低分辨率或改用其他后端继续。

**实施顺序与职责：**

| 工作 | 现有归属与修改位置 | 必须交付的行为 |
| --- | --- | --- |
| 固定回归证据 | `graph_image_cache_test.cpp`、`runtime_invalidation_test.cpp`、`cuda_result_cache_test.cpp` | 将临时复现转成永久断言；加入近预算、混合尺寸、连续输入和角色切换 |
| 依赖驱动的保留与回收 | `runtime_invalidation.hpp/.cpp`、`graph_image_cache.hpp`、`texture_pool.hpp`、`basic_render_workspace.hpp` | 通过现有 owner 的版本/表示查询决定回收；删除有效结果 LRU；正确处理共享 lease、读者和实际可释放字节 |
| 明确传递任务策略 | 现有 render request/role、产品 renderer、`plan_executor.hpp` | 从 QualityBase 的实际角色导出有限结果读写策略，传到共享执行路径；不得按 4096 尺寸猜角色，不新增调度器 |
| 下游执行与存储 | CUDA/OpenCL/Metal 的 pass encoder、local tone、Mask，`node_result_cache.hpp` 等实际结果所有者 | 下游仅使用本次提交的输出；不读写 Interactive 持久结果；临时结果不复制已有图像内容 |
| 完成与失败处理 | 现有 PublishResults、CompleteMatchingImages、CancelRender、present 完成路径 | 仅发布允许保留且真正完成的写入；失败不发布半成品；最后读者完成后释放 QualityBase 结果 |
| 删除被替代路径 | 上述生产文件及测试注册 | 删除 result `lru_tick`/淘汰决策和无条件下游 cache bind/record/publish；保留依赖版本与现有 GPU 完成机制 |

不要直接把整次 QualityBase 切到现有全量 BypassSessionCache：那会一并失去 Develop 复用。
也不能只关闭 PlanExecutor 的外层命中判断；LLF、Mask 等内部路径和批量发布必须遵守
同一策略。复用提交内写入/资源所有权能力，不为旁路创建另一个 live document 或长期工作区。
每个新操作说明 owner、线程、可变状态、读者完成点、失败与释放规则。

**主调用链：**

```text
owner 应用修改 → CollectAndPropagate → 识别任务角色与结果保留策略
  → Develop 有效则复用，失效则执行并按完成规则发布
  → Interactive：复用有效 2560px/上游结果，只重算失效下游，发布当前结果
  → QualityBase：Geometry 起只执行提交内计算，不查找/发布持久结果
  → present 完成 → 释放本次临时输出与已无读者的失效结果
失败/取消 → 不发布未完成结果 → 保留未受影响的上游与 Interactive 结果
```

**必须执行的验收矩阵（已实现并在下方完成记录中执行）：**

| 测试/场景 | 关键断言 |
| --- | --- |
| `DownstreamEditsPreserveValidDevelopAndInteractiveResizeNearBudget` | 真实尺寸输入或等价受限预算，连续下游写入；Develop/resize revision 和资源保持有效，首次后 execute 为 0，缓存像素与独立重算一致 |
| `FreeMatchingAllocationPreservesAllValidPipelineResults` | 存在空闲匹配分配时直接复用；不删除有效结果、不额外分配 |
| `InvalidMixedSizeResultsReleaseOnlyRequiredUnleasedAllocations` | 混合尺寸与共享 lease；仅回收无读者失效存储，满足请求就停止，有效结果不变 |
| `QualityBaseBypassesEveryResultCacheAfterSensorDevelop` | 预置可命中的下游结果；Geometry 起所有图像/LLF/Mask/值元数据持久缓存查找与发布调用计数均为 0；Develop 可命中 |
| `InteractiveQualityBaseInteractiveReuses2560PixelResults` | 首次 Interactive 建立缓存；QualityBase 不替换 handle、表示或 completed revision；返回 Interactive 复用仍有效结果；无跨帧 4K 条目 |
| `QualityBaseMutationInvalidatesOnlyDependentInteractiveResults` | 下游改动保留 Develop/resize；Geometry 改动保留 RAW Develop 并使几何依赖失效；RAW Develop 改动到达全部后代；不能靠旁路掩盖 stale 结果 |
| `RepeatedQualityBaseRendersReleaseDownstreamStorageAfterPresentation` | Develop/Geometry 连续调整及 QualityBase/Interactive 交替；持久下游无 QualityBase 条目，临时占用在最后读者完成后归还，不随帧数增长 |
| `QualityBaseFailurePreservesUnchangedInteractiveResults` | encode/upload/present 失败与取消；不错误推进 completed，不清除未受影响缓存，显示中旧输出不被覆盖 |
| `RequiredLiveResourcesReportAllocationFailureWithoutEvictingDevelop` | 注入资源不足；真实错误可观察，无有效上游淘汰、降质或后端替换 |
| `QualityBasePixelsMatchFreshExecutionWithinDeclaredTolerance` | 多 Grade、LLF、Mask、DRT/Post、crop/resize 的旁路输出与同参数同质量独立执行比较；测试声明误差容限与输入，禁止只有 no-throw/有限值断言 |

用真实 session 调度验证角色传递，并在 CUDA/OpenCL/Metal 检查共享规则与后端内部路径。
Windows/MSVC 与 macOS 按现有构建技能执行；记录每个后端通过、失败、跳过及未执行范围，
不能用头文件检查替代 Metal 运行证据。计数区分版本失效、表示不匹配、任务主动旁路和资源
生命周期释放，不把主动旁路算成异常 cache miss。保留原 NM6.9 的更广泛像素资格矩阵，
但本修复需要的上述行为与像素测试必须在 P7 完成，不能推迟。

**退出条件：** 产品路径使用以上规则、旧淘汰/发布路径删除、永久测试注册且执行，完成记录
包含实际基点/文件/调用链/命令/结果/占用数据与残留限制。

##### Phase NM6.P7 completion record (2026-09-06)

**Status:** complete — dependency-owned result retention and QualityBase cache bypass after `develop:sensor_linear`.

**Primary success call chain:**

```text
owner mutation
  -> RuntimeInvalidationState::CollectAndPropagate
  -> Renderer::Render (FrameRole -> ResultPersistenceScopeForRole)
  -> PlanExecutor::Execute (SetResultPersistence)
  -> BindOrMiss: persist sensor_linear only on QualityBase; Interactive binds all current results
  -> GraphImageCache::AcquireTextureForWrite: free matching alloc, then invalid unleased reclaim
  -> Encode / RecordUnpublished
  -> Present or host download
  -> PublishSuccessfulSubmission (SensorDevelopOnly publishes sensor_linear only)
  -> DiscardUnpublished / ReleaseUnpublishedExcept(display)
```

**Primary failure call chain:**

```text
Present/Download/encode throws
  -> CancelRender or WaitIdle + DiscardUnpublished
  -> Interactive published revisions unchanged
  -> std::runtime_error (no Develop eviction, no quality/backend substitute)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DownstreamEditsPreserveValidDevelopAndInteractiveResizeNearBudget` | `GpuDagCudaDrtProductTest` | PASS |
| `FreeMatchingAllocationPreservesAllValidPipelineResults` | `GpuDagCudaWorkspaceTest` | PASS |
| `InvalidMixedSizeResultsReleaseOnlyRequiredUnleasedAllocations` | `GpuDagCudaWorkspaceTest` | PASS |
| `RequiredLiveResourcesReportAllocationFailureWithoutEvictingDevelop` | `GpuDagCudaWorkspaceTest` | PASS |
| `QualityBaseBypassesEveryResultCacheAfterSensorDevelop` | CUDA + OpenCL product | PASS |
| `InteractiveQualityBaseInteractiveReuses2560PixelResults` | CUDA + OpenCL product | PASS |
| `QualityBaseMutationInvalidatesOnlyDependentInteractiveResults` | `GpuDagRawInputTest` + CUDA product | PASS |
| `RepeatedQualityBaseRendersReleaseDownstreamStorageAfterPresentation` | `GpuDagCudaDrtProductTest` | PASS |
| `QualityBaseFailurePreservesUnchangedInteractiveResults` | `GpuDagCudaDrtProductTest` | PASS |
| `QualityBasePixelsMatchFreshExecutionWithinDeclaredTolerance` | CUDA + OpenCL product | PASS |
| `QualityBasePreviewUsesSessionCacheAndSensorDevelopPersistence` | `PipelineFrameSinkTest` | PASS |
| `QualityBaseRolePersistsOnlySensorDevelop` | `GpuDagRawInputTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagCudaWorkspaceTest --target GpuDagCudaDrtProductTest --target GpuDagRawInputTest --target PipelineFrameSinkTest --target GpuDagOpenClDrtProductTest
ctest --test-dir build/debug --output-on-failure -R GraphImageCacheRetention|ResultPersistence.QualityBase|QualityBaseMutationInvalidates|QualityBasePreviewUsesSessionCache|DownstreamEditsPreserveValidDevelop|QualityBaseBypassesEveryResultCache|InteractiveQualityBaseInteractiveReuses|RepeatedQualityBaseRendersRelease|QualityBaseFailurePreserves|QualityBasePixelsMatchFresh --timeout 180
```

Suite totals: focused P7 filter 17/17 PASS; `CudaResultCacheProductFixture` 30/30 PASS. Pixel comparison: 32×32 Bayer fixture, QualityBase `max_edge=32`, absolute RGB tolerance `1e-4` on host `CV_32FC4` ACES download versus `BypassSessionCache` of the same request. Near-budget CUDA case kept Develop/geometry execute at 0 after the first Interactive fill. Texture pool `UsedBytes` after Interactive cycles did not grow past the first-cycle high-water.

**Checklist / exit condition:** all P7 acceptance names above executed on Windows/MSVC. Metal counterparts are compiled into `GpuDagMetalRendererTest` source and were not run on this host.

**LOC note (grill-code-review):** `graph_image_cache.hpp` ~502 lines (one cache type); `plan_executor.hpp` ~301; `basic_render_workspace.hpp` ~296; `result_persistence.hpp` ~48; `cuda_result_cache_test.cpp` ~950 (under 1000, still one product-cache fixture). No new snapshot/mirror of live document state.

**Remaining gaps:** Metal GPU execution of the three QualityBase tests was not run (Windows host, Metal target not built). NM6.9 wider RAW pixel matrix is unchanged. Session scheduler still uses `kQualityBasePreviewMaxLongEdge` 4096 with `UseSessionCache`; QualityBase is not switched to `BypassSessionCache`. Texture-pool LRU remains only for unowned, unleased textures.

##### Phase NM6.P7 completion record (2026-09-06, live-write LRU budget)

**Status:** complete — macOS Metal CI `CiRawWorkflowTest` failed after the first P7 landing because `GraphImageCache::AcquireTextureForWrite` treated the Metal 256 MiB texture-pool LRU target as a hard cap on live in-flight writes. The runner still had ~4.7 GiB device memory free (`images=pub0/write1`, one ~22.6 MiB leased write). The next pipeline stage needed another live texture, peak usage crossed 256 MiB, and the cache threw `insufficient texture memory to allocate a live result`.

**Primary success call chain:**

```text
CiRawWorkflowTest one-shot / product render
  -> PlanExecutor::Execute
  -> GraphImageCache::AcquireTextureForWrite
  -> matching free reuse, then invalid unleased reclaim, then EvictUntil idle
  -> TexturePool::Acquire (may exceed LRU byte budget while valid or in-use textures stay leased)
  -> backend CreateTexture2D
```

**Primary failure call chain:**

```text
HostTextureBackend::CreateTexture2D injected failure
  -> std::runtime_error ("HostTextureBackend: injected allocation failure")
  -> published Develop handle and revision unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `HeldUnpublishedWriteAllowsNextStagePastLruBudget` | `GraphImageCacheRetentionTest` | PASS |
| `LiveWriteExceedsLruBudgetWithoutEvictingValidDevelop` | `GraphImageCacheRetentionTest` | PASS |
| `RequiredLiveResourcesReportAllocationFailureWithoutEvictingDevelop` | `GraphImageCacheRetentionTest` | PASS (injected `CreateTexture2D` failure; LRU budget left large enough that a software-budget throw cannot satisfy the assertion) |
| `FreeMatchingAllocationPreservesAllValidPipelineResults` | `GraphImageCacheRetentionTest` | PASS |
| `InvalidMixedSizeResultsReleaseOnlyRequiredUnleasedAllocations` | `GraphImageCacheRetentionTest` | PASS |
| `QualityBaseWriteDoesNotReplaceValidInteractiveGeometry` | `GraphImageCacheRetentionTest` | PASS |
| `DefaultPipelineRendersCiRawFixture` | `CiRawWorkflowTest` | PASS |
| `SchedulerProducesThumbnailAndFastPreview` | `CiRawWorkflowTest` | PASS |

Commands:

```text
cmake --preset macos_debug_tests
cmake --build build/macos-debug-tests --parallel 8 --target GraphImageCacheRetentionTest --target CiRawWorkflowTest
ctest --test-dir build/macos-debug-tests --output-on-failure --timeout 180 -R 'GraphImageCacheRetention|CiRawWorkflowTest.DefaultPipelineRendersCiRawFixture|CiRawWorkflowTest.SchedulerProducesThumbnailAndFastPreview'
```

Suite totals: 8/8 PASS. Logs: `build/tmp/ci-raw-budget-fix/`. Host retention tests now live in `GraphImageCacheRetentionTest` (`ci_core_flow` + `ci_core` aggregate) so macOS CI builds and runs them without CUDA.

**Checklist / exit condition:** software LRU budget is no longer a false allocation failure; valid Develop stays leased; real allocation failure is injected at `CreateTexture2D`; the two previously failing Metal CI RAW workflow tests pass on this host.

**LOC note (grill-code-review):** `graph_image_cache.hpp` 543; `texture_pool.hpp` 371; `graph_image_cache_retention_test.cpp` 299; `graph_image_cache_test.cpp` 249 (CUDA workspace tests only). No new snapshot/mirror of live document state.

**Remaining gaps:** Metal QualityBase product tests from the first P7 record were not re-run here. CUDA `GpuDagCudaWorkspaceTest` no longer owns the host retention cases; they run on `GraphImageCacheRetentionTest` on every CI OS.

## 5. 人类可读与长期维护的退出条件

- 开发者从滑块开始能沿直接调用读到一次Model更新；每次线程切换均有具体原因。
- 新的参数类型不需要改一个多用途字符串路由器；复杂参数有自己的具名操作与不变量。
- 关键新增/修改类型文档说明owner、可变性、线程、必要拷贝与释放点。注释解释原因，
  不以typed、snapshot等标签代替实际字段说明。
- 测试分别证明输入顺序、Model更新、面板投射、持久化和像素结果；最终有生产链证据。
  禁止只有fake-port测试通过就宣布GUI/worker问题解决。
- 维护说明只需列出一个scalar编辑和一个复杂编辑的实际调用链、如何新增算子/面板、
  序列化边界。若必须理解一组通用协议概念才能加曝光控制，本阶段未达到目的。
- 不新建全量State/Context/Payload集合。必要UI值投递、历史before/after和GPU上传按
  AGENTS.md说明具体需求；不把“线程安全”当作复制整个对象的理由。
- 原有16 ms与单帧所有权、缓存依赖版本、Develop-only Geometry、历史格式全部保留。

## 6. 验证与完成记录

复用 [NM6 测试与性能要求](phase_nm6_node_aware_adjustments_plan.md#8-required-test-and-performance-evidence)。
使用项目Windows包装命令/对应macOS presets，记录真实目标、命令、fixture、revision、
CPU准备成本与GPU结果。中间文件只写 build/tmp/nm6p/。P1 已执行 Windows/MSVC 构建和
相关产品测试；后续阶段继续补充对应的 CPU/GPU 与真实 QML/session 证据。静态检查定位
JSON/DTO调用后必须分类实际边界，不能一概删除合法ToJson/LoadJson。

| Sub-phase | Status | Actual call chain / revision | Evidence | Deleted/replaced paths |
| --- | --- | --- | --- | --- |
| NM6.P1 | complete 2026-09-05 | `feature/nm6-native-parameter-access`, base `main` @ `5c8acc56`; QML → session → pending input → owner consume → history/model → render；live state → snapshot → panel | inventory + `EditorPipelineCommandServiceTest` 的 2 个新增计数断言；7 个相关测试目标共 54/54 passed | P1 未删除生产路径；普通 JSON 应用/投射迁移到 P2/P4，队列载体迁移到 P3，运行时 DTO 清理迁移到 P5，最终删除迁移到 P6；项目/WAL/checkpoint/import/export JSON 保留 |
| NM6.P2 | complete 2026-09-06 | `feature/nm6-native-parameter-access`; QML/session pending edit → `EditorHistoryMutation::CaptureAdjustmentBeforePreview` → `ApplyEditorParameterPatch` → field parser → concrete Model typed update → dirty patch/render/history；history replay 的 expected-side JSON boundary → typed apply | `EditorPipelineCommandServiceTest` 19/19、`EditorSessionHistoryPortTest` 75/75、`PipelineHistoryApplierTest` 12/12；Windows/MSVC target builds passed | 普通 Model 写入的完整 JSON merge/LoadJson 已替换为 typed owner operations；`ReadEditorParameterJson`、ToJson/LoadJson、history/project/WAL/import/export JSON boundary 保留；P3 queue、P4 projection、P5 runtime DTO、P6 cleanup remain planned |
| NM6.P3 | complete 2026-09-06 | `feature/nm6-native-parameter-access`; QML/model `submitWrite` → pending typed fields → move on consume → `ApplyEditorParameterWrite` → serial render | write-path 115/115、history 76/76、QML/session 143/145 then RapidMultiSlider PASS; see P3 completion record | live queue/`HandlePatch`/Capture 不再经 pending JSON；`submitPatch` 仅 GUI 收集边界一次解析；snapshot `params_json` 留待 P4；历史/WAL/项目 JSON 保留 |
| NM6.P4 | complete 2026-09-06 | `feature/nm6-native-parameter-access`; owner lock → typed adapter (`NodeId`+instance) → `EditorPanelProjection` → session stamp → controller discard/merge/replace → existing QML `loadFromSnapshot` | `EditorPanelProjectionTest` 5/5、`EditorSessionControllerPhase5ATest` 49/49、`EditorAdjustmentSnapshotQmlTest` 2/2、`EditorSessionHistoryPortTest` 77/77；Windows/MSVC target builds passed | GUI 面板加载不再经 `params_json` / `BuildSnapshotMap`；`ToJson`/`MakeFullDto` 不在投射路径；render snapshot JSON 与 history/WAL/project JSON 保留至 P6；NM6.6 所选节点尚未接入 |
| NM6.P5 | complete 2026-09-06 | `feature/nm6-native-parameter-access`; owner `Read` → `MakeGradeRuntimeParams` / `MakeGradeNeighborParams` → `BindOrRefreshGradeRuntimeSlot` → `ParameterArena::WritePackedSlot` → `UploadDirty`; LLF/Metal enable from packed host slot | `GpuDagAdjustmentRuntime` 9/9、dirty-field 4/4、`GpuDagCudaWorkspaceTest` 24/24、`GpuDagCudaPrimaryGradeTest` 36/36、`GpuDagOpenClGradeTest` 60/60；Windows/MSVC target builds passed | Grade 生产路径不再 `MakeFullDto` 或把 `GradeAdjustmentParams` 包成 OperatorParamDto；未改槽位不重新打包；Develop/DRT JSON-resolved GPU params 与 `InitializeFromFullDto` 留待 P6 |
| NM6.P6 | complete 2026-09-06 | `feature/nm6-native-parameter-access`; QML/model `submitWrite` → queue → `ApplyEditorParameterWrite` → remirror/render/history → typed panel read; CameraColor/DRT `BindOrWritePackedSlot` | history 77/77、CUDA Grade 38/38、OpenCL Grade 61/61、CUDA DRT 52/52、OpenCL DRT 16/16、session 49/49、QML snapshot 2/2；见 P6 completion record | 删除 `PublishEditorParameterPatch`、`InitializeFromFullDto`/`ApplyPatch`/`CopyFields`、live `ExportPipelineParams` dump、CameraColor/DRT DTO wrap；JSON 仅保留序列化/CPU remirror/`submitPatch`/DRT `ODT_Op` 边界 |
| NM6.P7 / NM6.4P | complete 2026-09-06 | `feature/nm6-native-parameter-access`; owner 依赖失效 → `ResultPersistenceScopeForRole` → GraphImageCache 保留/回收 → QualityBase Develop 后旁路 → present/release | P7 filter 17/17 PASS; CUDA product cache 30/30 PASS; Metal tests present, not executed | 删除 published-result LRU (`EvictCompletedUnleased`)；QualityBase 不查找/发布 Geometry 及下游；无 4K 保留槽位 |
| NM6.P7 live-write LRU budget | complete 2026-09-06 | `AcquireTextureForWrite` → free/invalid reclaim → `TexturePool::Acquire` (live writes may exceed LRU) | `GraphImageCacheRetentionTest` 6/6 PASS; `CiRawWorkflowTest` DefaultPipeline + Scheduler 2/2 PASS on `macos-debug-tests` | 删除把 LRU 字节预算当硬上限的软件失败；真实失败改为注入 `CreateTexture2D` |

NM6.P整体（含P7）完成后才能把NM6.5/6.6标记为可开始。保留NM6.1–NM6.4原完成记录；新的范围和
证据写在这里。NM6.P 使用同一个 `feature/nm6-native-parameter-access` 分支作为整体
reviewable PR，NM6.Px 不单独创建分支或 PR；每个切换仍必须删除对应被替代的生产路径，
不保留双轨入口或未授权的替代实现。
