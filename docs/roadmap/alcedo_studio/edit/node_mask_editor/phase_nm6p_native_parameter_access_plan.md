# Phase NM6.P — 原生参数读写与面板投射

Date: 2026-09-05

Status: NM6.P1 complete; NM6.P2–NM6.P6 remain planned.

Parent: [NM6 execution plan](phase_nm6_node_aware_adjustments_plan.md).
Dependency: NM6.4 → NM6.P → NM6.5 → NM6.6 → NM6.7 → NM6.8 → NM6.9.

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

### NM6.P4 — 直接投射 Model 到现有面板模型

**修改范围：** 实现第3.2节所述具体适配读取，替换全节点/算子JSON和完整DTO中转。
统一少量能力/适配注册。先支持现有上下文；NM6.6只负责将其接到真正的所选节点。
给后者提供明确的 NodeId/实例读取入口，不依赖隐式 PrimaryGrade。

**主要调用链：** owner scoped Node read → typed adapter → GUI presentation values。

**文件：** editor_adjustment_pipeline、session presentation读取、各面板/model load-only API。
保留现有控件与布局；不在本阶段重做NM6.7的节点标题/EXIF布局。

**验收：** 多种面板一次加载正确；无 ToJson/LoadJson/全DTO投射；QML加载不提交；
跨线程旧投递被丢弃；面板类型扩展示例只增加具体适配/注册而无需改全局JSON解析器。

### NM6.P5 — 去掉运行时完整参数体中转

**修改范围：** MakeGradeRuntimeParams、neighbor参数准备从安全 owner读取必要字段，
直接填既有GPU参数布局。只更新发生变化的槽位。清除为读取几个值而生成FullDto的路径。

**主要调用链：** stable Model values → necessary GPU parameter fields → existing ParameterArena。

**文件：** adjustment_runtime、operator_model_base读取接口、三backend现有Grade参数准备。
仅统一读数/打包入口，不提前改NM6.5的Grade/LLF执行编排。

**验收：** 未改槽位不重新打包；必要GPU上传之外无完整Model复制；CPU准备数值与
现有期望参数一致；CUDA/OpenCL/Metal调用同一类型化参数准备代码。

### NM6.P6 — 删除被替代路径并验证整条生产链

**修改范围：** 完成旧接口去向表，删除无生产用途的JSON投射/全状态merge更新/重复DTO
包装与转发helper；修正测试伪实现，避免它们旁路真实参数路径。写最终简短维护说明。

**主要调用链：** 真实控件 → existing queue → Model owner → render → release/history
→ Undo/reopen → typed read → 真实面板。三后端保持既定质量和所有权行为。

**验收：** P1每个旧入口都有删除、明确序列化边界保留或真实新调用方；不能以“以后清理”
结束此阶段。真实QML/session测试与像素/参数回归通过。明确统计中转类型、JSON调用、
完整payload复制和无意义转发的去除情况，不以新增测试数量或总行数作为可维护性证明。

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
| NM6.P2 | planned | — | — | — |
| NM6.P3 | planned | — | — | — |
| NM6.P4 | planned | — | — | — |
| NM6.P5 | planned | — | — | — |
| NM6.P6 | planned | — | — | — |

NM6.P整体完成后才能把NM6.5/6.6标记为可开始。保留NM6.1–NM6.4原完成记录；新的范围和
证据写在这里。NM6.P 使用同一个 `feature/nm6-native-parameter-access` 分支作为整体
reviewable PR，NM6.Px 不单独创建分支或 PR；每个切换仍必须删除对应被替代的生产路径，
不保留双轨入口或未授权的替代实现。
