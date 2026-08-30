# Node-aware Pipeline Editing and Mask Authoring 总体方案

Date: 2026-08-29

Status: NM0 complete；NM1 in progress；NM2–NM8 planned。NML cancelled (2026-08-30).

本方案承接 [GPU DAG 编辑管线重构 Phase 计划](gpu_dag_pipeline_rebuild_phase_plan.md)。前一份
计划建立了 `PipelineDocument`、`PipelineGraph`、GPU execution plan、三后端管线、MaskStore
和默认三节点文档；本方案负责把这些底层能力提升为可被用户直接操作的节点编辑、节点感知
调整面板、多蒙版绘制、编辑历史和 Version 工作流。

本文件定义背景、产品语义、目标架构、跨模块边界、一级 Phase 顺序、主要调用链、风险和
最终验收范围。它固定 `NM0` 到 `NM8` 的阶段门槛，但不预先写每个阶段内部的详细执行步骤。
一级 Phase `NML`（旧 stage 存储升级到默认 DAG）于 2026-08-30 取消。产品不打开 DAG document
之前的 stage-only 项目，也不迁移旧 mini-git commit。后续 Phase 删除 CPU stage 表，不做旧
项目升级。详细措辞见 [NM1 执行方案第 3.1 节](node_mask_editor/phase_nm1_pipeline_document_editing_plan.md)。
开始某个一级 Phase 前，再根据当时的代码状态创建对应执行方案；该文件继续拆成
`NMx.1`、`NMx.2` 等具体子 Phase，并决定实际 PR/branch 粒度。执行方案可以调整内部拆分，
但不得无说明地越过本文的一级依赖或改变已经锁定的产品语义。

相关设计：

- [Alcedo Studio QML Visual Identity](../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md)
- [Phase 6C Mini-Git History and Pipeline Snapshot Plan](../ui/phase_6c_mini_git_history_and_pipeline_snapshot_plan.md)
- [Editor Single Live Pipeline + WAL + Checkpoint](../ui/editor_single_live_pipeline_wal_checkpoint_plan.md)
- [Phase 7A History/Versions Recovery and Editor Performance Plan](../ui/phase_7a_history_versions_repair_and_ui_refactor_plan.md)
- [QuickQanava repository](https://github.com/cneben/QuickQanava)
- [QuickQanava graph documentation](https://cneben.github.io/QuickQanava/graph.html)
- [QuickQanava custom topology documentation](https://cneben.github.io/QuickQanava/advanced.html)

---

## 1. 最终结果

用户打开一张图片或一个 Version 时，编辑器加载该 Version 对应的完整
`PipelineDocument`。默认或新建 Version 从当前三节点结构开始：

```text
Develop
   ↓
Default Color Grade
   ↓
DRT and Post Processing
```

用户可以：

- 在左侧工具轨中打开 Nodes 面板；
- 使用 QuickQanava 选择节点、创建 Color Grade、删除 Color Grade 和重新连接主链；
- 选择任意 Color Grade 后，在右侧面板调整属于该节点的参数；
- 为一个 Color Grade 创建多个 Brush、Radial 或 Linear Gradient 蒙版；
- 在 edit viewer 上直接绘制或修改蒙版，并同时看到实际调色结果预览和 QSG 编辑辅助层；
- Undo/Redo 节点、参数和蒙版操作；
- 在历史面板中看清操作目标节点和具体变化；
- 为不同 Version 保存不同的节点、连接、节点参数和蒙版；
- 把一份可转移的节点管线粘贴为目标图片的一个新 Version。

节点选择、节点位置、画布平移/缩放和临时蒙版控制点属于编辑器会话状态。它们不改变照片，
不进入编辑历史，也不触发管线渲染。

---

## 2. 当前实现背景

这一阶段不能只在 QML 中加入一个 graph view。当前实现仍有以下限制，必须在节点 UI 接入
之前或同时解决。

### 2.1 PipelineDocument 还不是唯一编辑状态

`PipelineMgmtService` 仍把 legacy CPU stage 作为历史和 QML 参数修改的主要状态，并把结果
重新镜像到 format v2 `PipelineDocument`。这意味着节点 UI 如果直接修改
`PipelineDocument`，传统 adjustment UI 如果继续修改 stage，两者会形成两个可写来源。

目标状态必须改为：

```text
PipelineDocument
  = 当前图片、当前 Version、当前 working head 的唯一可写编辑状态

GPU runtime / execution plan / QML projection / history projection
  = PipelineDocument 的派生状态
```

旧 stage adapter 不得继续接收 UI 写入后再反向覆盖新图。

### 2.2 PipelineGraph 只有添加和连接

现有 `PipelineGraph` 只提供 `AddNode()`、`Connect()`，并公开可变节点容器。它没有完整的
删除、断开、替换连接、原子验证和失败回滚能力。节点编辑器需要领域层操作，不能让 QML
直接改 `Nodes()` 或 `Edges()`。

### 2.3 编译器和三后端只执行一个 Primary Grade

当前 `GraphCompiler` 通过固定 ID 查找 `grade.primary`，只编译一个
`primary_grade_adjustments`，并在第一个匹配的 mask edge 后停止。CUDA、OpenCL、Metal
运行路径也按单一 primary grade 绑定参数、缓存和输出。

因此，多节点 UI 之前必须让 compiler、execution plan、内容 key、dirty 传播和三后端执行
真正按主链上的 Color Grade 列表工作。

### 2.4 ColorGradeNodeModel 的职责过宽

现有默认 Color Grade 同时包含普通调色、Clarity、Sharpen、Halation 和 Film Grain。产品
要求中间 Color Grade 不拥有这些后处理项，它们只属于最后的 DRT/Post endpoint。

如果只在 QML 中隐藏控件，序列化、粘贴、历史回放和 GPU 编译仍然可以把这些调整放进中间
节点，所以必须在 model、注册表和编译规则中明确移动所有权。

### 2.5 参数 Patch 没有节点身份

现有 `EditorAdjustmentPatch` 只有 `field_key`、JSON 参数和 settled 标志。它默认每个字段在
整条管线中只有一个实例，不能表达“修改哪个 Color Grade 的哪一个 adjustment”，也不能
稳定表达一个 Mask 的参数。

### 2.6 历史 payload 仍按 stage/operator 标识字段

现有普通 edit payload 依赖 `OperatorType + PipelineStageName + field_name`。节点创建、删除、
重新连接、多个相同类型的调整和多个 Mask 都无法由这个身份模型准确表达。

现有 adjustment transfer merge 也以 stage/operator 字段冲突为核心。两个独立图之间没有
足够自然、稳定的自动合并定义，因此本方案删除新的管线 merge 产品操作，只保留 Paste。

### 2.7 MaskStore 写入语义不适合直接 Undo raster stroke

现有 `MaskStore::Save()` 接受调用者提供的 key，并原子替换同名文件。如果 history 只保存
这个 key，后续 stroke 覆盖同一文件后，Undo 无法恢复旧像素。

Raster mask 必须变为不可变、按内容寻址的资源：每次 settled stroke 产生一个新
`MaskAssetKey`，历史只在新旧 key 之间切换。

### 2.8 Viewer 已有正确的扩展位置

现有 viewer 已经分离：

```text
EditorViewportItem             实际照片帧
EditorInteractionController    图像空间与输入状态
EditorOverlayItem              retained QSG 辅助几何
```

蒙版 authoring 应继续扩展这个结构，而不是在 QML 中创建第二套坐标系统或把辅助层烘焙到
最终照片帧。

---

## 3. 锁定术语

### 3.1 节点

- **Develop endpoint**：每张图片唯一、不可删除、不可替换的第一个节点。
- **Color Grade**：用户可创建、选择、删除和重新连接的中间调色节点。
- **Clean Color Grade**：用户新建的普通可变 Color Grade，所有参数从无视觉变化状态开始。
- **Default Color Grade**：新图片或新建默认 Version 中的初始 Color Grade，允许带产品默认
  曝光和饱和度。
- **DRT/Post endpoint**：每张图片唯一、不可删除、不可替换的最后节点。UI 显示名为
  `DRT and Post Processing`。
- **Image backbone**：从 Develop 到 DRT/Post 的唯一有效图像主链。

创建新 Color Grade 的代码和文档统一使用 `Clean`。Clean 节点是普通、可编辑、可删除的
节点；这个名称只说明初始参数不改变画面，不赋予节点任何特殊生命周期或只读语义。

### 3.2 蒙版

- **Mask**：属于一个 Color Grade 的局部作用范围。
- **Mask source**：Brush、Radial 或 Linear Gradient 产生的基础 coverage。
- **Color Range**：直接属于一个 Mask 的可选颜色范围限制。
- **Luminance Range**：直接属于一个 Mask 的可选明度范围限制。
- **Union**：同一 Color Grade 内多个启用 Mask 的唯一组合方式。
- **Mask editing overlay**：在 viewer 上由 QSG 绘制的蒙版编辑辅助层。
- **Mask coverage preview**：未来 Color Range/Luminance Range 需要的内容相关像素覆盖显示。

第一版模型不提供通用范围修饰列表或通用组合模式；两个 Range 是 Mask 的直接字段，多个
Mask 的唯一组合语义是 Union。

### 3.3 编辑和历史

- **Provisional mutation**：输入序列进行中、已经影响实时预览、尚未生成 edit commit 的值。
- **Settled mutation**：pointer release、键盘确认或既定 idle 边界后形成的最终变化。
- **Pipeline edit batch**：一个用户操作产生的一组不可拆分 typed mutations，并对应一个
  edit commit。
- **Graph layout state**：QuickQanava 节点位置、画布平移和缩放；它是 UI 状态，不是照片
  编辑数据。

---

## 4. 产品范围

### 4.1 本方案包含

- QuickQanava 依赖、构建、QML module 和安装打包；
- Nodes 左侧面板；
- Color Grade 创建、删除、选择和主链重新连接；
- node-aware adjustment stack；
- 多 Color Grade compiler 和 CUDA/OpenCL/Metal 执行；
- 每个 Color Grade 的多 Mask 数据；
- Brush、Radial、Linear Gradient 绘制和编辑；
- QSG mask editing overlay；
- Color Range/Luminance Range 的数据位置、序列化位置和 typed target 预留；
- typed pipeline mutation；
- 节点和蒙版 edit history；
- 每个 Version 对应一份 DAG；
- Paste 创建新 Version；
- 当前格式数据升级、存储恢复、重开和跨后端验证。

### 4.2 本方案不包含

- 任意图像分支、并行 grade 分支或多输入 compositor；
- 用户可创建的 Develop 或 DRT/Post endpoint；
- 中间 Color Grade 的独立 Clarity、Sharpen、Halation 或 Film Grain；
- Union 以外的 Mask 布尔运算或可配置组合方式；
- Color Range/Luminance Range 的第一版 UI 和最终选择算法；
- AI segmentation mask；
- 在节点编辑器中显示内部 GPU pass；
- detached HEAD；
- 新的管线 merge 操作；
- 打开或升级 DAG document 之前的 stage-only 项目；
- 把旧 mini-git commit 从 stage 身份迁移成 DAG mutation。

---

## 5. 目标分层

```text
QML
  EditorWorkspaceRail / NodeEditorPanel / EditorAdjustmentStack / Viewer handlers
          ↓ only app service APIs
Application layer
  EditorNodeController
  EditorAdjustmentContext
  EditorPipelineCommandService
  EditorMaskAuthoringController
  EditorHistory projection
          ↓ typed mutations
Edit model
  PipelineDocument
  PipelineGraph
  DevelopNodeModel
  ColorGradeNodeModel + MaskStack
  Drt/Post model
          ↓ compile
Runtime
  GraphCompiler
  ExecutionPlan
  CUDA / OpenCL / Metal workspaces
          ↓ assets
MaskStore
```

QuickQanava 处于 QML 和 app projection 之间。它负责节点图的视觉交互，不拥有
`PipelineDocument`，也不决定一条 edge 是否有效。

UI 层只能调用 app service。QuickQanava 的 node/edge 对象是投影视图，不能被 history、
serialization 或 renderer 当作领域数据。

---

## 6. 第一版图结构不变量

底层类型继续使用 DAG，但第一版产品允许的 scene-image 拓扑是单主链：

```text
Develop -> ColorGrade[0] -> ... -> ColorGrade[n] -> DRT/Post
```

必须始终满足：

1. 恰好一个 Develop endpoint；
2. 恰好一个 DRT/Post endpoint；
3. Develop 没有 scene-image 输入；
4. DRT/Post 没有 scene-image 输出；
5. 每个 Color Grade 恰好一个有效 scene-image 输入和一个输出；
6. 从 Develop 沿 scene-image edge 只能得到一条无环路径；
7. 该路径必须经过全部 Color Grade 并最终到达 DRT/Post；
8. scene-image 端口不允许 fan-out 或 fan-in；
9. Mask 不作为可自由连接的顶层 graph node；
10. 图结构修改必须先在候选文档上完成全部验证，再替换 live document；
11. 失败的修改不能留下部分 edge、history commit 或半更新的 UI projection。

这个限制不是退回串行 stage。NodeId、edge、拓扑 hash、编译顺序、Version 文档仍然是图数据；
它只是明确第一版没有 compositor，避免向用户展示没有图像合成定义的分支。

### 6.1 领域操作

`PipelineGraph` 不直接向 QML 暴露容器修改。至少提供以下语义完整的命令：

```text
AddCleanColorGrade(before_node_id)
RemoveColorGradeAndBridge(node_id)
ReconnectColorGrade(node_id, new_predecessor_id, new_successor_id)
RenameColorGrade(node_id, display_name)
SetColorGradeEnabled(node_id, enabled)
```

`RemoveColorGradeAndBridge` 是一个原子操作：删除节点及其相邻两条 scene-image edge，然后把
前驱连接到后继。不能让 UI 依次发出 disconnect、remove、connect，因为任何中间状态都会
形成无效文档。

第一版重新连接只改变主链顺序。QuickQanava 的 connector 请求经 backend 验证后，可以转换
为一次 `ReconnectColorGrade`；不允许形成任意分支。

### 6.2 节点稳定身份

- `NodeId` 在该节点生命周期内稳定；
- 节点改名不改变 `NodeId`；
- Version 分支共享 commit 时共享原 NodeId；
- Paste 到另一个图片 root 时重新映射 NodeId，避免跨 root 冲突；
- history payload、render content key、QML selection 和 graph layout 都用 NodeId，不用列表
  index 作为身份；
- 删除后 Undo 恢复原 NodeId、完整节点数据和原 edge。

---

## 7. 参数所有权

参数必须由模型和编译器强制归属，不能只靠 QML 隐藏。

| 参数组 | Develop | Color Grade | DRT/Post | Document/global |
| --- | ---: | ---: | ---: | ---: |
| RAW Decode | 是，仅第一个 endpoint | 否 | 否 | 否 |
| RAW white balance / camera profile | 是 | 否 | 否 | 否 |
| Lens calibration | 是 | 否 | 否 | 否 |
| CAT02 creative white balance | 否 | 是 | 否 | 否 |
| Exposure / Contrast / White / Black | 否 | 是 | 否 | 否 |
| Shadows / Highlights / Curve / HLS | 否 | 是 | 否 | 否 |
| Saturation / Vibrance / Color Wheel / LUT | 否 | 是 | 否 | 否 |
| Color Grade mix / masks | 否 | 是 | 否 | 否 |
| Clarity | 否 | 否 | 是 | 否 |
| Sharpen | 否 | 否 | 是 | 否 |
| Halation | 否 | 否 | 是 | 否 |
| Film Grain | 否 | 否 | 是 | 否 |
| DRT method / output transform | 否 | 否 | 是 | 否 |
| Crop / rotation / image geometry | 否 | 否 | 否 | 是 |

DRT/Post endpoint 可以继续保留现有序列化 type ID，以降低当前文档升级成本，但其 model 和
execution plan 必须实际拥有后处理参数。中间 Color Grade 的 adjustment catalog 不得创建
Clarity、Sharpen、Halation 或 Film Grain 实例。

### 7.1 默认三节点和 Clean Color Grade

必须有两个不同的创建入口：

```cpp
auto CreateDefaultPipelineDocument(...) -> PipelineDocument;
auto CreateCleanColorGradeNode(NodeId id) -> std::unique_ptr<ColorGradeNodeModel>;
```

或者等价的 `ColorGradeNodeModel::MakeClean()`。调用意图必须从名字和类型上明确。

`CreateDefaultPipelineDocument()`：

- 创建 Develop、Default Color Grade、DRT/Post；
- Default Color Grade 使用当前产品基线，包括 Exposure `+1.5 EV` 和 Saturation `+30`
  对应的新模型数值；
- 为图片 root 记录当时的完整默认文档，后续代码默认值变化不能重写已存在 root。

`CreateCleanColorGradeNode()`：

- 创建普通可变节点；
- Exposure 为 `0 EV`；
- Saturation 为无变化值；
- 其他 Color Grade adjustment 全部为无视觉变化状态；
- `enabled = true`、`mix = 1`、masks 为空；
- 不包含任何 Develop 或 DRT/Post 专属 adjustment；
- 新建过程只产生一次 graph mutation、一次历史提交和一次 Quality render。

UI 不得创建默认节点后再用多个参数 Patch 把它改成 Clean。

### 7.2 DRT/Post 的内部执行顺序

`DRT and Post Processing` 是一个用户可见 endpoint，不要求 compiler 把它压成一个 GPU
kernel。Clarity、Sharpen、Halation、Film Grain 和 DRT 可以继续编译为多个内部 pass，并按
各自需要在 scene-referred 或 display-referred 边界执行。

把这些参数从 Color Grade 移到 DRT/Post 时，必须先用现有产品路径和 reference fixture 固定
当前计算顺序，再把同一顺序表达为 DRT/Post 的内部 execution plan。不能因为 UI 中它们都在
最后一个节点，就未经验证地把所有算法机械移动到 DRT 之后。对用户而言它们只属于最后一个
节点；对 runtime 而言可以有清楚命名的 pre-DRT、DRT 和 post-DRT pass。

---

## 8. Color Grade 多蒙版模型

### 8.1 所有权

Mask 直接属于 Color Grade：

```text
ColorGradeNodeModel
  adjustments
  mix
  masks[]
    MaskId
    display_name
    enabled
    opacity
    source
    color_range
    luminance_range
```

这样做的原因：

- Mask 表达“这个 Color Grade 在哪里生效”，生命周期天然随节点；
- Color Range/Luminance Range 需要读取该节点的上游输入；
- 用户从选中节点的右侧面板创建 Mask，不需要在 graph view 中管理第二套 mask edge；
- 删除 Color Grade 时可以把节点和全部 Mask 作为一个历史对象恢复；
- 第一版 graph view 只显示实际图像处理主链。

### 8.2 MaskSource

第一版 source variant：

```cpp
using MaskSource = std::variant<BrushMaskSource,
                                RadialMaskSource,
                                LinearGradientMaskSource>;
```

Brush 的长期结果是不可变 R8 asset；当前输入序列还可以持有临时 stroke 和 dirty rectangle。
Radial 与 Linear Gradient 保存解析参数，在 ReferenceSpace 中求值。

所有 source 使用稳定的归一化图像空间或现有 ReferenceSpace 约定。viewer zoom、pan、DPR、
动态渲染分辨率和 ROI 不得改变长期参数含义。

### 8.3 Color Range 和 Luminance Range

每个 Mask 只预留两个明确字段：

```cpp
std::optional<ColorRangeModel> color_range;
std::optional<LuminanceRangeModel> luminance_range;
```

当前 UI 不提供它们，默认都为 disabled/null。新 schema、history target、content key 和未来
coverage preview API 必须知道这两个直接字段的位置，但本阶段不建立通用 modifier 列表，
也不猜测更多 range 类型。

未来启用后，单个 Mask 的 coverage 顺序为：

```text
source coverage
  × Color Range coverage（未启用时为 1）
  × Luminance Range coverage（未启用时为 1）
  × opacity
```

Color Range 和 Luminance Range 读取该 Color Grade 的上游输入，而不是节点完成调色后的输出，
从而避免 range 随自身调整形成反馈环。

### 8.4 Union-only

同一节点的多个 Mask 只用于扩展生效范围：

```text
effective_mask = max(enabled_mask_0,
                     enabled_mask_1,
                     ...)
```

不保存 combine mode，也不为其他 Mask 组合方式预留第一版 UI。

边界行为：

```text
masks 为空                         -> effective coverage = 1
masks 非空但全部 disabled           -> effective coverage = 0
至少一个 Mask enabled              -> enabled Mask coverage 的 Union
```

Mask 顺序用于 UI、选择、键盘导航、历史描述和确定性序列化，但不影响 Union 的像素结果。
因此仅调整列表显示顺序不触发渲染，也不进入照片编辑历史。

### 8.5 Color Grade Mix

最终节点混合继续使用：

```text
node_coverage = clamp(effective_mask × grade_mix, 0, 1)
output = input + node_coverage × (adjusted - input)
```

无 Mask 时 `effective_mask = 1`。禁用节点直接复用输入，不执行不必要的 grade passes。

---

## 9. MaskStore 与 raster Undo

Mask asset 必须不可变并按内容寻址：

```text
MaskStore::Put(descriptor, pixels)
  -> 计算内容 key
  -> 已存在则验证并复用
  -> 不存在则写完整临时文件、flush、原子发布
  -> 返回 MaskAssetKey
```

不得用同一个 key 覆盖不同像素。一次 settled brush stroke：

1. 输入序列开始时记录旧 `MaskAssetKey`；
2. pointer move 使用临时 raster buffer 和 dirty rectangle 更新实际预览；
3. pointer release 生成完整新 asset；
4. `Put()` 返回新 key；
5. history 记录 `old_key -> new_key`；
6. Undo/Redo 只替换引用；
7. 请求 Quality render。

Mask 资源回收必须按可达性进行，至少扫描：

- 每个 image root；
- 所有 Version head 可达的 commit；
- 当前 working state；
- 未 materialize 的 recovery records；
- 正在进行的 mask authoring session。

不能根据 host/GPU LRU 删除磁盘用户数据。资源回收必须是独立、可审计的 clean-exit 或维护
操作。

---

## 10. Viewer 预览和 QSG 编辑辅助层

### 10.1 两类视觉结果

蒙版编辑时同时存在两类内容。

实际调色结果：

```text
当前 PipelineDocument
  -> 当前 Color Grade 参数
  -> 当前临时或 settled Mask
  -> Interactive / Quality render
  -> EditorViewportItem
```

蒙版编辑辅助层：

```text
Brush cursor / path / radius / feather
Radial center / radii / rotation / feather / handles
Linear Gradient line / direction / transition / handles
  -> EditorOverlayItem retained QSG nodes
```

这里的 overlay 特指 viewer 上方的 QSG mask editing overlay；实际调色结果仍然是
`EditorViewportItem` 显示的管线渲染帧。

### 10.2 简单 source 直接由 QSG 绘制

Brush、Radial 和 Linear Gradient 的辅助范围都可以由 QSG 直接绘制：

- Brush cursor 和已经采样的 path/dabs 使用 retained geometry；
- Brush radius 和 feather 使用与实际 mask 相同的半径、硬度和 feather 参数；
- Radial 使用同一 center、major/minor radius、rotation 和 feather 公式；
- Linear Gradient 使用同一方向、起止位置、transition 和 feather 公式；
- 控制点和边界使用已有 overlay 视觉 token；
- overlay 更新不得等待 pipeline render 完成。

QSG 和实际 Mask evaluator 必须共享参数定义、坐标映射和数学函数说明。Brush QSG 与 raster
生成器消费同一份归一化 `BrushStroke` samples，不能分别从原始 pointer event 推导两条 path。

### 10.3 内容相关 coverage 留给未来请求

未来 Color Range 或 Luminance Range 启用后，选中像素取决于该节点上游图像内容。届时可以
增加 `MaskCoveragePreviewRequest`，把内容相关 coverage texture 交给 viewer 显示。

当前 Brush/Radial/Linear Gradient 阶段不需要通用 `MaskPreviewRequest`。不能为了简单几何
辅助层建立第二条后端 render 通道。

### 10.4 Overlay 状态规则

| 操作 | Mask editing overlay | 实际管线预览 |
| --- | ---: | ---: |
| 选中 Mask | 显示 | 不必立即渲染 |
| 绘制 Brush | 显示 | Interactive |
| 修改 Brush radius/feather | 显示 | Interactive |
| 移动/缩放/旋转 Radial | 显示 | Interactive |
| 修改 Radial feather | 显示 | Interactive |
| 修改 Linear Gradient | 显示 | Interactive |
| 修改 Mask opacity | 显示 | Interactive |
| 未来修改 Color/Luminance Range | 显示内容相关 coverage | Interactive |
| 修改 Exposure/Curve/LUT 等调色参数 | 隐藏 | Interactive |
| pointer release / settled edit | 根据当前 authoring mode 显示 | Quality |
| 删除/重新连接节点 | 隐藏 | Quality |
| Escape 取消输入序列 | 恢复输入前状态 | 不生成 history commit |

开始调色参数输入时隐藏 overlay，但保留 selected Mask 身份。输入结束后不自动重新显示；用户
重新进入 Mask 工具或 viewer authoring mode 时再显示，避免遮挡调色判断。

---

## 11. Typed mutation 与统一参数接口

### 11.1 参数 target

统一参数修改接口保留现有 provisional/settled 节奏，但 target 必须从纯字符串升级为明确身份：

```text
EditorParameterTarget
  owner_kind
    Document
    Develop
    ColorGrade
    ColorGradeMask
    DrtPost
  node_id                  // Document target 为空
  adjustment_instance_id   // 调整参数时存在
  mask_id                  // Mask 参数时存在
  field_key
```

每个 patch 必须携带完整 `EditorParameterTarget`。缺少 `owner_kind`、`node_id` 或 Color Grade
的 `adjustment_instance_id` 时拒绝写入；不根据 field catalog 或选中节点填入缺省 target。
输入序列的第一个合法 patch 锁定 target；同一序列的后续完整 patch 复用该锁定。详见 NM1
执行方案第 3.1 节。

### 11.2 Mutation 分类

```text
Parameter mutation
  SetAdjustmentField
  SetNodeEnabled
  SetNodeMix
  SetMaskField

Graph mutation
  AddCleanColorGrade
  RemoveColorGradeAndBridge
  ReconnectColorGrade

Mask mutation
  AddMask
  RemoveMask
  ReplaceMaskSourceParams
  ReplaceMaskAsset
```

Mask 列表 UI 重排和 QuickQanava layout 不属于照片 mutation。

### 11.3 原子执行

Graph 和 Mask 结构修改使用 prepared operation：

1. clone 当前 `PipelineDocument`；
2. 对候选文档应用 typed mutation；
3. 验证 graph、参数所有权、MaskId 唯一性和 asset 引用；
4. 为当前 backend 准备或验证新的 static execution plan；
5. 构造 forward/inverse history payload；
6. 只有全部准备成功后，原子发布 document、working head 和 projection revision；
7. 提交 Quality render；
8. 任一步失败都保留旧 document、旧 history tip 和最后一帧。

不允许通过捕获异常后继续使用旧 stage 或较弱管线路径来隐藏失败。

---

## 12. Render reason 和质量规则

新增明确的内容修改原因，名称可在实现阶段按现有 enum 风格落地：

```text
InteractiveMaskEdit
SettledMaskEdit
GraphTopologyChanged
VersionDocumentChanged
PastedPipelineDocument
```

质量规则：

| 修改 | Quality |
| --- | --- |
| adjustment slider provisional | Interactive |
| adjustment slider settled | Quality |
| Mask transform/feather/opacity provisional | Interactive |
| Mask transform/feather/opacity settled | Quality |
| Brush stroke provisional dirty region | Interactive |
| Brush stroke settled asset | Quality |
| 添加/删除 Mask | Quality |
| 添加/删除/重新连接 Color Grade | Quality |
| Undo/Redo | Quality |
| Version checkout | Quality |
| Paste 新 Version | Quality |
| node selection / rename | 不渲染；rename 只进 history 与否由 13.4 决定 |
| graph node layout / pan / zoom | 不渲染 |

结构变化使 static plan key 变化并重新编译。纯参数值、简单 mask 几何参数和 raster asset key
变化只更新参数/content key，不应误触发与拓扑无关的 compiler 工作。

---

## 13. 编辑历史

### 13.1 一个用户操作对应一个 commit

历史单位改为 `PipelineEditBatch`。它可以包含多个低层 mutation，但必须对应一个完整的用户
意图，例如删除节点同时移除两条 edge 并增加 bridge edge。

```text
PipelineEditBatch
  operation_kind
  typed forward mutations
  typed inverse mutations or complete before/after object
  display metadata keys
```

commit hash 对 canonical typed payload 计算。历史回放不能重新推断删除前的节点、Mask 或 edge。

### 13.2 必须进入历史的操作

- Color Grade 添加、删除、重新连接；
- Color Grade enabled、mix 和参数修改；
- Mask 添加、删除；
- Mask source 参数修改；
- settled Brush stroke 形成的新 asset key；
- Mask enabled、opacity、invert、feather 等长期参数；
- 未来 Color Range 和 Luminance Range 修改；
- 影响导出结果的 Document/Develop/DRT/Post 参数。

### 13.3 不进入照片历史的状态

- selected node / selected Mask；
- Nodes 面板展开状态；
- QuickQanava node position、graph pan 和 zoom；
- hover、focus、正在拖动的连接预览；
- QSG overlay 是否显示；
- provisional pointer samples；
- Mask 列表仅用于显示的顺序。

### 13.4 Rename 的处理

Color Grade display name 会出现在 history 和 node panel 中，但不影响像素。第一版建议把 rename
保存进 Version 对应的 `PipelineDocument`，并形成可 Undo 的 metadata edit commit；它不触发
render。这样 Version checkout 可以恢复当时的节点名称，历史描述也保持稳定。

### 13.5 History projection

history row 从 typed payload 派生可本地化描述，例如：

```text
Exposure · Warm Face: +0.30 → +0.55
Added Color Grade · Sky
Removed Color Grade · Background
Reconnected Color Grade · Skin after Base Grade
Added Radial Mask · Warm Face
Brush stroke · Warm Face / Mask 2
Feather · Warm Face / Radial 1: 24 px → 48 px
```

不要把已本地化字符串写入 commit payload。payload 保存稳定 ID、operation kind 和数值；QML
projection 根据当前 locale 生成文本。

History model 只在 history revision 变化时重建受影响的 row，不能因 Interactive render、
busy state 或 QSG overlay 更新而全量刷新。

---

## 14. Version 和 Paste

### 14.1 每个 Version 对应一份 DAG

Version 仍然是 named ref，history 仍然拥有唯一 HEAD。语义改为：

```text
image root
  = 导入元数据解析完成后的不可变默认 PipelineDocument

Version head
  = 从 root 沿 first-parent commits 重放后得到的 PipelineDocument

serialized_pipeline_state
  = 与 materialized head/chain 匹配的 PipelineDocument checkpoint
```

新建默认 Version 指向当前三节点 root。新建 Version at root 也恢复三节点 DAG，而不是复制当前
working graph。

### 14.2 Pipeline DAG merge 取消

Adjustment Transfer 产品层只保留 Paste：

- UI 删除 Merge 入口、冲突对话框和 merge-specific 状态；
- service 不再创建新的 two-parent pipeline merge commit；
- 不定义 NodeId 匹配、edge 冲突、Mask 冲突或两个 DAG 的自动合并；
- history 存储如果已经包含旧 two-parent merge commit，默认保留读取和 first-parent replay
  能力，避免现有项目无法打开；
- 是否在下一次项目格式升级中彻底转换旧 merge commit，必须在数据迁移测试后单独决定，不能
  静默丢弃。

取消新的 merge 产品操作不等于把 history commit graph 改成线性数组。Version 分支和共享
祖先仍然保留。

### 14.3 Paste 语义

Paste 是“把一份可转移 PipelineDocument 写成目标图片的新 Version”，不是把它与当前图合并：

1. 读取目标图片不可变 root；
2. 保留目标 Develop endpoint 及其 RAW metadata/camera profile/lens identity；
3. 从 transfer package 导入 Color Grade 主链、参数、Mask 和 DRT/Post 可转移参数；
4. 按目标 root 重新映射 NodeId、AdjustmentInstanceId 和 MaskId；
5. 复制或复用内容寻址 MaskAsset；
6. 验证目标 image backbone 和参数所有权；
7. 创建一个新的 named Version；
8. 将该 Version 设为 active；
9. 进行一次原子的 history/pipeline/storage publication；
10. 请求 Quality render。

Document geometry 默认不随 Paste 转移，避免把源图片比例和 crop 直接套到目标图片；如果产品
以后需要“同时粘贴 Geometry”，应作为明确选项和独立 typed mutation 加入，不应隐式发生。

Paste 失败时不得创建空 Version、部分 Mask asset 引用或移动 active Version。

---

## 15. QuickQanava 集成边界

### 15.1 依赖策略

- 使用上游 QuickQanava，不实现自有 graph canvas、edge router、port hit testing、selection、
  pan 或 zoom 组件；
- 固定经过验证的 tag/commit，不在 configure 时跟随浮动分支；
- 优先作为仓库 submodule/vendor source 接入，构建不依赖在线 FetchContent；
- 记录 BSD-3-Clause license 和第三方 notices；
- 验证 Qt 6.9.3、C++20 主工程、Windows/MSVC 和 macOS/Clang；
- 验证静态/动态 QML module 的 import path、资源、install 和 package；
- production QML 继续使用 Qt Quick Controls Basic，不因上游示例改成 Material。

### 15.2 Adapter

```text
PipelineDocument
  -> EditorNodeGraphProjection
  -> AlcedoQanGraph adapter
  -> QuickQanava GraphView / node / port / edge delegates
```

允许为 QuickQanava extension point 编写薄的 Alcedo delegate，以显示节点标题、状态、端口和
VI token；不允许重新实现节点拖动、edge 绘制、connector 或 graph navigation。

建议关闭 QuickQanava 默认的直接建边行为：

```text
connectorCreateDefaultEdge = false
connectorRequestEdgeCreation
  -> EditorNodeController validate
  -> typed reconnect mutation
  -> PipelineDocument publish
  -> graph projection revision
```

视觉 connector 在 backend 成功前只表示候选连接。失败时恢复原图并显示精确原因。

### 15.3 Projection 更新

- 参数 slider 变化不重建 graph projection；
- node name/enabled/mask count 只更新对应 node roles；
- graph topology revision 才添加/删除 node 和 edge projection；
- Version checkout 替换整份 projection，但保留同 Version layout state；
- Qan object lifetime 不能成为 NodeId lifetime；
- QML 不保存裸 C++ model 指针跨异步操作。

---

## 16. Nodes 面板

Nodes 作为左侧 `EditorWorkspaceRail` 的第三个可展开页面，与 History、Versions 同属一个视觉
和生命周期体系：

```text
Rail
  History
  Versions
  Nodes
  Background Tasks
```

需要把当前 `historyPanelPage` 概念改成中性的 rail page state，例如
`editorToolPanelPage`。合法值至少为 `""`、`history`、`versions`、`nodes`。

布局规则：

- 使用 `cardSurfaceColor`、`cardBorderColor`、`panelRadius` 和现有 fold motion；
- 默认宽度 `editorSidePanelWidth`；
- 如果 320 px 对 node graph 不够，允许在现有 `editorSidePanelWidthMin/Max` 范围内由用户调整，
  不能为 Nodes 建立并行宽度体系；
- panel fully closed 时卸载 graph delegates，layout state 存在 rail/controller；
- reduced motion 时立即到达终态；
- 节点/端口/删除/添加必须支持键盘操作和可见 focus；
- tooltip、Accessible.name 和错误信息必须可本地化；
- 不依靠 hover 才显示唯一操作入口。

第一版主链默认纵向排列，Develop 在上、DRT/Post 在下。节点位置是用户可调的 UI layout，
按 `(project, image, version, NodeId)` 保存，不进入 Version DAG 或 edit history。

Endpoint node 显示锁定/不可删除状态。Color Grade 显示 enabled、Mask 数量和当前选择。添加按钮
创建 Clean Color Grade，并插入到当前 Color Grade 后或 DRT/Post 前；具体规则必须在 UI 和
keyboard action 中一致。

---

## 17. Node-aware Adjustment Stack

引入 `EditorAdjustmentContext`，至少发布：

```text
selected_node_id
selected_node_kind
selected_mask_id
node_display_name
capabilities
adjustment_snapshot
context_revision
```

右侧 `EditorAdjustmentStack.qml` 不再假设一个全局参数表，而是按 context 显示可用 panel：

| 选中对象 | 可用 panel |
| --- | --- |
| Develop | RAW Decode、Develop-owned controls、Geometry |
| Color Grade | Tone、Look、LUT、Masks、Geometry |
| DRT/Post | DRT/Display、Clarity、Sharpen、Halation、Film Grain、Geometry |

Geometry 是 Document/global 参数，所以在三个 context 都可以访问；它不因节点选择而创建三份
实例。

每个 panel 的 `loadFromSnapshot()` 改为读取当前 context 的 snapshot。Panel 初次构建、
session rebind、Version checkout、node selection 和 Undo/Redo 后都要加载正确值，但 load-only
路径不能提交 history 或 render。

当用户切换节点时：

1. 结束或取消当前 provisional input sequence；
2. 更新 selected NodeId；
3. 发布新的 context snapshot；
4. adjustment stack 选择该节点支持的首选 panel，或保留仍然合法的 panel；
5. 只更新 UI，不触发照片渲染。

删除 selected Color Grade 后，选择其后继 Color Grade；若没有则选择前驱；再没有则选择
DRT/Post。Version checkout 后优先恢复该 Version 的最近选择，找不到时选择 Default/第一个
Color Grade。

---

## 18. Mask authoring 状态机

建议由 `EditorMaskAuthoringController` 统一管理，而不是把状态分散到 QML handlers：

```text
Inactive
Selected
Creating
Editing
Painting
Settling
Failed
```

状态至少锁定：

```text
session_generation
version_id / working head generation
NodeId
MaskId
Mask source kind
before snapshot
provisional params or stroke
dirty rectangle
```

创建流程：

```text
select Color Grade
  -> choose Brush / Radial / Linear Gradient
  -> AddMask typed mutation
  -> enter authoring mode
  -> viewer input updates provisional source
  -> QSG overlay updates immediately
  -> pipeline Interactive preview
  -> settle to one history commit and Quality render
```

需要明确以下抢占行为：

- image switch、Version checkout、Undo/Redo、node deletion 前必须完成或取消 authoring；
- stale session generation 的异步 render/result 不得更新新图片 overlay；
- Escape 恢复 before snapshot；
- Delete 删除 selected Mask，但不能删除错误节点中同 index 的 Mask；
- viewer pan/zoom 与 Mask authoring 使用明确 mode/focus 路由，不能同时解释同一个 pointer input；
- keyboard 用户可以选择 Mask、移动控制点、调整数值并退出模式。

---

## 19. 序列化与数据升级

新 PipelineDocument schema 至少保存：

- document geometry；
- Develop model；
- 有序 Color Grade nodes；
- scene-image edges；
- 每个 Color Grade 的 adjustment 列表、enabled、mix；
- 每个 Color Grade 的 masks；
- MaskId、source、enabled、opacity；
- `color_range` 和 `luminance_range` 的直接字段；
- raster `MaskAssetKey` 和 descriptor；
- DRT/Post 参数和后处理参数。

不保存：

- QuickQanava object；
- graph selection、hover、focus；
- graph pan/zoom；
- QSG vertices/material instances；
- provisional stroke；
- dirty bits、GPU handles、cache state；
- render request；
- panel active page。

由于当前 format v2 使用单 `grade.primary` 和顶层 MaskNode，新模型需要提升 document format。
提供一个明确、单向的当前 v2 -> 新格式转换：

- `grade.primary` 成为 Default Color Grade；
- 当前唯一 mask edge 转成该 grade 的第一个 Mask；
- Clarity/Sharpen/Halation/Film Grain 从 primary grade 移到 DRT/Post；
- 保留节点参数和 MaskAssetKey；
- 无法满足新不变量的数据返回明确升级错误；
- 旧 stage JSON 仍按 GPU DAG 计划返回 unsupported-format error，不恢复 legacy 编辑路径。

转换必须有 golden JSON、round-trip、错误注入和真实项目 reopen 测试。

---

## 20. 主要调用链

### 20.1 新建 Color Grade

```text
NodeEditorPanel / QuickQanava action
  -> EditorNodeController::AddCleanColorGrade
  -> EditorPipelineCommandService::Prepare
  -> Clone PipelineDocument
  -> CreateCleanColorGradeNode
  -> Insert node + reconnect candidate backbone
  -> Validate graph and ownership
  -> Compile/validate static plan
  -> Build PipelineEditBatch
  -> append journal + move active Version head
  -> publish PipelineDocument + graph/context/history revisions
  -> submit GraphTopologyChanged Quality render
```

### 20.2 Node-aware slider

```text
AdjustmentSlider
  -> context-bound value model
  -> unified submit(target, value, settled)
  -> EditorSessionEditController
  -> mutate selected NodeId + AdjustmentInstanceId
  -> provisional: InteractiveAdjustment render
  -> settled: one typed edit commit + Quality render
```

### 20.3 Radial Mask 编辑

```text
Masks panel selects Radial
  -> AddMask(NodeId, MaskId, Radial params)
  -> viewer authoring mode
  -> pointer input mapped to ReferenceSpace
  -> provisional params shared by QSG + pipeline
  -> QSG overlay next frame
  -> InteractiveMaskEdit render
  -> pointer release
  -> one edit commit
  -> SettledMaskEdit Quality render
```

### 20.4 Brush stroke

```text
pointer samples
  -> normalized BrushStroke
  -> QSG retained path/dabs
  -> temporary raster dirty rectangle
  -> InteractiveMaskEdit render
  -> pointer release
  -> MaskStore::Put complete R8 asset
  -> ReplaceMaskAsset(old_key, new_key)
  -> one edit commit
  -> Quality render
```

### 20.5 Version checkout

```text
Version selected
  -> finish/cancel provisional input
  -> reconstruct PipelineDocument from root + first-parent commits
  -> validate graph/assets
  -> prepare runtime execution plan
  -> atomically publish active Version + document
  -> replace node/context/history projections
  -> VersionDocumentChanged Quality render
```

### 20.6 Paste

```text
Adjustment Transfer Paste
  -> read target root PipelineDocument
  -> import transferable grade chain + DRT/Post params + Mask assets
  -> remap IDs
  -> validate and prepare runtime
  -> create and activate new Version
  -> atomic storage publication
  -> PastedPipelineDocument Quality render
```

---

## 21. 一级 Phase 总表

本总体方案锁定以下一级 Phase。它们是架构和产品能力的顺序，不等同于一个 PR；每个 Phase
将来都有自己的执行方案，执行方案内部再拆具体子 Phase 和可评审 PR。

本次只预留文件路径，不创建空的执行方案。开始对应 Phase 时创建文件，并把表中的代码路径
改成 Markdown 链接。

| Phase | Status | 未来执行方案 | 阶段结果 |
| --- | --- | --- | --- |
| NM0 — QuickQanava Integration Baseline | complete | [node_mask_editor/phase_nm0_quickqanava_integration_plan.md](node_mask_editor/phase_nm0_quickqanava_integration_plan.md) | 固定依赖、构建和 package 路径，证明官方组件可被 production QML 使用 |
| NM1 — PipelineDocument Editing Foundation | in progress | [node_mask_editor/phase_nm1_pipeline_document_editing_plan.md](node_mask_editor/phase_nm1_pipeline_document_editing_plan.md) | PipelineDocument 成为新编辑写入权威；图命令含完整删除；thumbnail DAG；无 stage 产品镜像；无图存储拒绝打开 |
| NML — Legacy Stage Compatibility and Default DAG Upgrade | cancelled 2026-08-30 | — | 不升级、不打开 DAG document 之前的 stage-only 项目；不迁移 mini-git commit |
| NM2 — Multi-Grade Runtime and Ownership | planned | `node_mask_editor/phase_nm2_multi_grade_runtime_plan.md` | compiler 和三后端真正执行多 Color Grade，并落实参数所有权 |
| NM3 — Multi-Mask Model and Runtime | planned | `node_mask_editor/phase_nm3_multi_mask_runtime_plan.md` | 每节点多 Mask、Union、Range 字段和不可变 raster asset 完整可用 |
| NM4 — History, Version, Recovery, and Paste | planned | `node_mask_editor/phase_nm4_history_version_paste_plan.md` | typed history、每 Version 一 DAG、recovery 和 Paste-only 完成切换 |
| NM5 — QuickQanava Nodes Panel | planned | `node_mask_editor/phase_nm5_nodes_panel_plan.md` | 左侧 Nodes 面板连接真实 command/history/render 路径 |
| NM6 — Node-aware Adjustment Stack | planned | `node_mask_editor/phase_nm6_node_aware_adjustments_plan.md` | 右侧参数面板按 Develop/Color Grade/DRT/Post context 工作 |
| NM7 — Viewer Mask Authoring | planned | `node_mask_editor/phase_nm7_viewer_mask_authoring_plan.md` | Brush/Radial/Linear、QSG overlay、Interactive/Quality 和 history 完整连通 |
| NM8 — Product Qualification and Cutover | planned | `node_mask_editor/phase_nm8_product_qualification_plan.md` | 三后端、真实 RAW、reopen、Version/Paste、性能和 package 验收完成 |

### 21.1 Phase NM0 — QuickQanava Integration Baseline

配置记录：[Phase NM0 QuickQanava integration](node_mask_editor/phase_nm0_quickqanava_integration_plan.md)。
NM0 没有执行子 Phase；工作内容是固定上游 checkout、CMake 接入和 license 记录。

**为什么先做：** QuickQanava 是明确指定的 node editor 基础。必须先证明固定版本能够在当前
Qt 6.9.3、Basic style、Windows/MSVC、macOS/Clang、QML module 和最终 package 中正常工作，
否则后续 UI 架构会建立在未经验证的依赖假设上。

**阶段边界：** 完成第三方依赖、license、构建、QML import、安装打包和一个只读的最小 graph
projection harness。这个 harness 只证明官方 node/port/edge/selection/pan/zoom 能力；不接入
production 节点修改，不创建临时领域模型，也不实现自有 graph canvas。

**退出条件：** Windows 和 macOS build/package 都能加载固定 QuickQanava module；最小测试
能够从只读 projection 创建、选择和销毁 node/edge，没有 QML import 或资源路径错误。

NM0 按依赖固定落地：submodule pin、CMake 静态 QML module、license。原始退出条件里的只读
harness 和 package 加载需要 `alcedo_main` 链接该 module，本次不声称完成；由 NM5 首次生产
链接和 NM8 资格验证覆盖。

##### Phase NM0 completion record (2026-08-29)

**Status:** complete — pinned QuickQanava tag `2.50` (`56bdf78d5b1d41fb60ae3b8ea2292df45787ecff`), CMake `add_subdirectory(src)`, BSD-3-Clause / bezier MIT notices. No production import.

**Primary success call chain:**

```text
git submodule update --init alcedo_studio/src/third_party/QuickQanava
  -> alcedo_studio/src/third_party/CMakeLists.txt
  -> include AlcedoQuickQanava.cmake
  -> qt_add_qml_module(QuickQanava STATIC URI QuickQanava)
  -> targets QuickQanava / QuickQanavaplugin
```

**Primary failure call chain:**

```text
missing checkout
  -> FATAL_ERROR
  -> configure stops; no FetchContent
```

**What was proven (executed tests):** see [NM0 configuration record](node_mask_editor/phase_nm0_quickqanava_integration_plan.md).

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target QuickQanava
```

Windows debug linked `QuickQanava.lib`. macOS not built on this machine.

**Checklist / exit condition:** dependency pin complete. Harness and package load not claimed.

**Residual gaps:** no production link; no QML harness; macOS not built here; upstream `CanvasNodeTemplate.qml` still imports Material.

##### Phase NM0 completion record (2026-08-29, macOS)

**Status:** complete — macOS Clang debug configured and linked the same pin. No production import.

**Primary success call chain:**

```text
git submodule update --init alcedo_studio/src/third_party/QuickQanava
  -> cmake --preset macos_debug
  -> include AlcedoQuickQanava.cmake
  -> qt_add_qml_module(QuickQanava STATIC URI QuickQanava)
  -> cmake --build --preset macos_debug --target QuickQanava
  -> libQuickQanava.a
```

**Primary failure call chain:**

```text
missing checkout
  -> FATAL_ERROR
  -> configure stops; no FetchContent
```

macOS CI inits the gitlink from `scripts/ci_prepare_third_party.sh`.

**What was proven (executed tests):** see [NM0 configuration record](node_mask_editor/phase_nm0_quickqanava_integration_plan.md).

Commands:

```text
cmake --preset macos_debug
cmake --build --preset macos_debug --target QuickQanava --parallel 8
```

macOS debug linked `libQuickQanava.a` (Homebrew Qt 6.9.2, clang 21.1.1).

**Checklist / exit condition:** dependency pin complete on Windows and macOS debug. Harness and package load not claimed.

**Residual gaps:** no production link; no QML harness; upstream `CanvasNodeTemplate.qml` still imports Material.

### 21.2 Phase NM1 — PipelineDocument Editing Foundation

执行方案：[Phase NM1 PipelineDocument editing foundation](node_mask_editor/phase_nm1_pipeline_document_editing_plan.md)。

**为什么排在 UI 前：** 当前 legacy stage 和 `PipelineDocument` 仍然都可能影响编辑状态。
如果先做 Nodes panel，QuickQanava 会成为第三个状态来源。

**阶段边界：** 让 `PipelineDocument` 成为新编辑、history live mutation 和 thumbnail 像素的
写入/渲染权威；增加候选文档 mutation、完整 graph validation、原子 Add/Remove/Reconnect
（含删除 Default / `grade.primary`）、stable IDs、Clean Color Grade 创建入口、typed
parameter target 和失败回滚。产品不保留 CPU stage 表作为旧项目兼容镜像。进程内 stage 表
可以暂时留在代码中，直到后续 Phase 删除。NM 执行期间不要求现有 adjustment UI 继续可用；
每个 patch 必须带完整 typed target。Thumbnail 必须 DAG 渲染，不得先转到 stage 再出图。无可用
图的存储拒绝打开。

**退出条件：** 新文档编辑不被 live stages 覆盖；无效 mutation 不改变 document/history
head；默认三节点和 Clean node 行为有 model tests；thumbnail/analysis 走 DAG；UI 还未开放
新增节点入口。产品不打开 DAG document 之前的项目。

### 21.2a Phase NML — 已取消（2026-08-30）

后续 Phase 将删除 CPU stage 表。产品 stage 镜像会变成随后删除的额外工作。把旧 stage
项目升级为默认 DAG 会改写参数和 mini-git commit。工作量大。本产品不保留这些旧项目。

替代规则：

- 存储含有可用 format v2 图：打开。文档是编辑状态。
- 存储只有 stage JSON 或 stage adapter、没有可用图：不要打开。返回真实错误。不要 import
  成默认三节点图。不要改写 mini-git commit。
- 后续 Phase 删除 CPU stage 表。该 Phase 不是旧项目升级。

不要创建 `phase_nml_legacy_stage_dag_upgrade_plan.md`。

### 21.3 Phase NM2 — Multi-Grade Runtime and Parameter Ownership

**为什么排在节点操作前：** 数据模型能够保存多个节点，不代表 renderer 真正执行多个节点。
在 backend 完成前开放 Add/Reconnect 会制造保存后存在、画面中无效的节点。

**阶段边界：** GraphCompiler 按 image backbone 生成有序 grade plans；execution plan、
GraphValueId、ParameterArena、content key、dirty 传播、cache 和 CUDA/OpenCL/Metal 都按 NodeId
独立工作。Clarity、Sharpen、Halation、Film Grain 移入 DRT/Post model，并保留经过 reference
验证的内部计算顺序。

**退出条件：** 两个以上 Color Grade 在三后端产生顺序正确的结果；调整一个 node 只使该
node 及下游失效；中间 node 无法保存或编译 DRT/Post 专属调整；仍不开放 production Nodes
panel。

### 21.4 Phase NM3 — Multi-Mask Model and Runtime

**为什么排在 history 改造前：** History payload 必须引用最终 MaskId、Mask schema 和
MaskAssetKey 行为。先按旧单 mask 模型写 history 会造成第二次数据格式切换。

**阶段边界：** Mask 变为 Color Grade 内部有序列表；支持 Brush、Radial、Linear Gradient；
多个启用 Mask 只做 Union；直接保留 Color Range/Luminance Range 字段；compiler 和三后端按
每 node masks 计算 coverage；MaskStore 改为不可变、按内容寻址；active raster dirty rectangle
进入 runtime。

**退出条件：** 多 Mask Union、zero/all-disabled 边界、三种 source、feather、三后端一致性和
immutable asset Undo 前置能力都有测试；尚不开放 viewer 绘制 UI。

### 21.5 Phase NM4 — History, Version, Recovery, and Paste

**为什么排在 UI 前：** 一旦用户能够新增节点或绘制 Mask，这些操作就必须立即具备 Undo、
Version、journal、recovery 和 reopen 语义，不能把持久性留到 UI 之后补做。

**阶段边界：** 普通 stage/operator payload 切换为 typed `PipelineEditBatch`；结构操作保存完整
forward/inverse 数据；root、Version head 和 serialized checkpoint 都使用新
`PipelineDocument`；完成 format v2 升级；Adjustment Transfer 删除新的 merge 创建路径，只
保留创建新 Version 的 Paste；旧可达 merge commit 继续可读。

**退出条件：** Add/Delete/Reconnect/Mask asset replacement 能在无 production UI 的 service
integration tests 中 Undo/Redo、branch、checkout、recover 和 reopen；Paste 保留目标 Develop
metadata，失败不创建部分 Version。

### 21.6 Phase NM5 — QuickQanava Nodes Panel

**为什么此时才开放：** 到这里 node mutation 已经有真实多节点 backend 和完整 history/
Version 行为，Nodes panel 不会暴露半完成产品操作。

**阶段边界：** 在 `EditorWorkspaceRail` 增加 Nodes 页面；建立 `EditorNodeGraphProjection`、
Alcedo Qan adapter、node selection、Clean node 添加、删除、重新连接、rename 和 layout state；
所有 connector 请求先经 app service；只使用 QuickQanava 的 graph interaction primitives。

**退出条件：** Nodes panel 的所有可见命令都走 NM1–NM4 路径；失败不留下视觉 edge；panel
Loader、Basic style、AppTheme、keyboard、accessibility、reduced motion 和 package 测试通过。

### 21.7 Phase NM6 — Node-aware Adjustment Stack

**为什么排在 Nodes panel 后：** adjustment context 需要稳定的 selected NodeId、node kind、
删除后选择规则和 Version checkout selection 恢复。先改右侧 panel 会继续依赖隐式 primary
grade。

**阶段边界：** 引入 `EditorAdjustmentContext`；Develop、Color Grade 和 DRT/Post 显示各自合法
panels；Geometry 保持 document/global；每个 panel 的 `loadFromSnapshot()` 读取当前 context；
统一 slider API 锁定 NodeId/AdjustmentInstanceId；切换节点只更新 UI，不渲染。

**退出条件：** 三类 node context、selection restore、panel re-entry、Version checkout、
load-only、provisional/settled 和 history target 测试通过；Color Grade context 中不存在
RAW Decode 或 DRT/Post 专属控件。

### 21.8 Phase NM7 — Viewer Mask Authoring

**为什么最后接入交互：** viewer authoring 同时依赖 selected node context、多 Mask runtime、
immutable MaskStore、typed history 和 Interactive/Quality render。任何一项缺失都会产生无法
恢复或无法正确预览的编辑。

**阶段边界：** Masks panel 的 style action、Brush/Radial/Linear 创建、viewer input routing、
ReferenceSpace mapping、QSG mask editing overlay、临时 raster dirty rectangle、settled asset、
Escape 取消、模式抢占和 stale session fencing 完整接入。调色参数输入期间隐藏 overlay。

**退出条件：** 三种 Mask 都能在真实 viewer 中编辑；QSG 与 evaluator 参数一致；pointer
release 一次 commit；settled 后 Quality；Escape 无 commit；image/Version 切换不接受旧结果。

### 21.9 Phase NM8 — Product Qualification and Cutover

**为什么独立：** 单元和组件测试不能证明 QuickQanava package、真实 RAW、多 backend、history
recovery、Mask asset 和最终帧在同一产品路径中一致。

**阶段边界：** 执行第 23 节全部测试和真实 RAW E2E；记录 Windows/CUDA、OpenCL、macOS/Metal、
package、reopen、Version、Paste、性能、内存和 cache 证据；删除在前面 Phase 明确替代的旧 UI/
service 路径；更新本文 completion record。

**退出条件：** 第 26 节全局完成条件全部满足；不存在 legacy stage 回写、单 primary grade 产品
分支、可变 raster key、新 merge UI 或其他 backend/CPU 替代路径。

---

## 22. Phase 顺序、执行方案和 PR 粒度

### 22.1 锁定主序列

```text
NM0 QuickQanava baseline
  -> NM1 PipelineDocument editing foundation
  -> NM2 Multi-grade runtime and ownership
  -> NM3 Multi-mask model and runtime
  -> NM4 History, Version, recovery, and Paste
  -> NM5 QuickQanava Nodes panel
  -> NM6 Node-aware adjustment stack
  -> NM7 Viewer mask authoring
  -> NM8 Product qualification and cutover
```

NML 已取消。NM2 假定已打开的项目带有可用 DAG 文档。没有图的项目不会进入 NM2。后续
Phase 删除 CPU stage 表。该删除不是旧项目升级。

上一个 Phase 的退出条件是下一个 Phase 的输入。可以在前一个 Phase 接近完成时做只读审计或
准备下一份执行方案，但不能提前向 production 暴露依赖尚未完成的操作。若实施证据证明必须
改变一级顺序，先更新本总体方案并说明原因，不能只在执行分支中悄悄换序。

### 22.2 未来执行方案结构

每个一级 Phase 开始前创建表中预留的执行方案。该文件至少包含：

```text
Phase NMx execution plan
  current source audit at start date
  exact scope and explicit exclusions
  NMx.1 ... NMx.n ordered implementation phases
  per-subphase primary call chain
  per-subphase files and APIs
  test and evidence requirements
  completion records
```

总体方案只跟踪 `NMx` 状态。具体实现进度、临时发现、提交和测试命令写在对应执行方案中，
避免把本文件变成不断变化的开发日志。

### 22.3 PR 和 branch 规则

一级 Phase 通常包含多个 PR，不以整个 `NMx` 或整个 node/mask 项目命名一条长期开发分支。
branch 应描述当前 PR 的实际结果，例如：

```text
docs/node-mask-editor-master-plan
build/quickqanava-qml-module
refactor/pipeline-document-write-authority
feature/multi-grade-execution-plan
feature/color-grade-mask-union
refactor/pipeline-edit-payload
feature/editor-nodes-rail-panel
feature/node-adjustment-context
feature/radial-mask-viewer-authoring
```

实施以逐个合入 main 的 PR 为默认方式。只有同一执行方案内部确实无法独立评审的 2–3 个紧密
子 Phase 才建立短 stack；底层 PR 合入后立即把剩余分支 rebase 到最新 main。不得建立一条从
NM0 一直延伸到 NM8 的长期 stacked PR 链。

每个 PR 必须：

- 对应一个明确的 `NMx.y` 子 Phase 或其可独立测试部分；
- 保持当前 product path 可构建、可测试；
- 不暴露依赖尚未完成的用户入口；
- 不加入 CPU、legacy stage 或其他 backend 替代路径；
- 在对应执行方案记录调用链、测试命令和未完成项。

---

## 23. 测试和证据总表

### 23.1 Model/property tests

- 任意合法主链经过 Add/Remove/Reconnect 后仍满足全部不变量；
- 任意 mutation forward 后 inverse，canonical document hash 恢复原值；
- 无效连接不改变 document hash、history head 或 projection revision；
- Clean Color Grade 的全部参数为无视觉变化状态；
- default document 只在 Default Color Grade 带 `+1.5 EV` / `+30` 基线；
- 中间 Color Grade 无 DRT/Post 专属 adjustment；
- 多 Mask Union 与 CPU reference 一致；
- zero masks、all-disabled masks、one/many enabled masks 的边界行为；
- MaskStore 同内容复用 key，不同内容不能覆盖旧 key；
- v2 -> 新格式转换和 JSON round-trip。

### 23.2 Runtime/backend tests

- 两个以上 Color Grade 的顺序影响结果，重新连接后结果按新顺序变化；
- 每个 node 的参数 Patch 只使该 node 及下游失效；
- 一个 node 的多个 Mask 在 CUDA/OpenCL/Metal 上等于 Union reference；
- Radial、Linear Gradient 和 Brush feather 与 reference fixture 一致；
- topology change 重新编译 static plan，参数 change 不误编译；
- endpoint ownership 在 compiler 层拒绝非法 adjustment；
- backend 失败返回真实错误，不切换 CPU 或其他 backend。

### 23.3 History/version tests

- Add/Delete/Reconnect/Mask stroke 的一操作一 commit；
- Undo 删除恢复完整 NodeId、参数、Mask 和 edges；
- Redo 恢复相同 document hash；
- Version checkout 得到对应 DAG，而不是当前全局 graph；
- root Version 始终得到三节点默认文档；
- Paste 创建新 Version，保留目标 Develop metadata；
- Paste remap 后无 ID 冲突，Mask asset 可读取；
- 不再创建新的 merge commit；
- 旧可达 merge commit 仍可读取和 first-parent replay；
- journal/recovery/reopen 后 working head、chain hash、document hash 一致。

### 23.4 QML/UI tests

- Nodes rail page 的 open/close、Loader 生命周期和 scroll/layout 恢复；
- production style 为 Basic，无 Material import；
- QuickQanava node/edge 数量与 projection 一致；
- connector 请求先经过 backend，失败不留下视觉 edge；
- node selection 切换 adjustment context，不触发 render；
- 每个 context 只显示合法 panels；
- `loadFromSnapshot()` 不提交 edit；
- panel re-entry 和 Version checkout 恢复正确 selection/value；
- keyboard focus 顺序、Delete/Escape、可访问名称和 localized tooltip；
- reduced motion 时 panel 和 selection 状态直接到达终态。

### 23.5 Viewer tests

- item/logical -> image UV -> ReferenceSpace 映射在 zoom/pan/DPR 下稳定；
- QSG Radial/Linear 边界与实际 evaluator 使用相同参数；
- Brush QSG 和 rasterizer 使用同一 sample 序列；
- mask input 期间 overlay 可见，grade slider 期间隐藏；
- stale session/result 不污染新 image/Version；
- pointer release 只产生一次 settled commit；
- Escape 恢复 before state 且不产生 commit；
- dirty rectangle 更新不上传整张 raster，除非 feather 传播确实要求全量更新。

### 23.6 Product E2E

至少包含以下真实 RAW 流程：

1. 导入并打开真实 Bayer RAW；
2. 验证 default 三节点和默认 `+1.5 EV` / `+30`；
3. 新建 Clean Color Grade，验证画面不发生变化；
4. 修改新节点 Exposure，验证只有该节点参数变化；
5. 添加 Radial、Linear Gradient 和 Brush Mask；
6. 验证多个 Mask 扩展同一节点范围；
7. 调整大小/位置/feather 时显示 overlay 和实际 Interactive 结果；
8. 调整 grade 参数时 overlay 隐藏；
9. 删除、重新连接节点并 Undo/Redo；
10. 创建/切换 Version，验证不同 DAG；
11. Paste 到另一张 RAW 的新 Version，验证目标 RAW metadata 保留；
12. 保存、关闭、重开，验证 history、Version、DAG、Mask assets 和最终帧；
13. 在 Windows/CUDA 与 macOS/Metal 执行产品路径；OpenCL 执行对应集成和一致性测试。

---

## 24. 性能和资源目标

- QSG overlay 对 pointer input 的反馈必须在下一次 scene graph frame 出现，不等待 pipeline；
- graph layout/selection 不提交 render intent；
- parameter-only change 不重建全部 QuickQanava graph；
- history projection 不监听每帧 render/busy 通知；
- Brush provisional update 合并 dirty rectangle；
- settled stroke 只保存一次完整新 asset；
- graph static plan 只在 topology/adjustment structure 改变时重建；
- 多 Color Grade 结果缓存按 NodeId + input content + node params/mask content 分层；
- node deletion/reconnect 允许淘汰不可达 node cache，但必须遵守 GPU submission lease；
- panel close 后没有残留 Qan delegates 或 QML connection 泄漏；
- N8 必须记录同一真实 RAW、同一 viewport、同一 backend 的单节点基线和多节点/多 Mask 数据。

在 N8 之前由现有产品 baseline 确定具体毫秒和内存预算；不能用降低 decode 分辨率、质量或
切换 backend 的方式达标。

---

## 25. 主要风险

### 25.1 双写来源未完全删除

症状：node panel 修改新 document，adjustment panel 仍修改 legacy stage，下一帧或保存时覆盖。

处理：NM1 让新编辑、history live 和 thumbnail 以 `PipelineDocument` 为权威。产品不保留
stage 镜像。NM1.5 从产品 Apply/Save 移除 stage 覆盖文档。无图存储拒绝打开。N5/N6
production 接入以 NM1 写入权威为前置条件。后续 Phase 删除 CPU stage 表。

### 25.2 多 Grade 只在 compiler 表面循环

症状：execution plan 有多个条目，但 backend 仍复用 `primary_grade_output`、一份参数 arena 或
固定 cache key，导致节点互相覆盖。

处理：N2 对 GraphValueId、参数 offset、内容 key、pass 输出和三后端逐 node 验证。

### 25.3 QSG overlay 与实际 Mask 偏离

症状：控制点显示范围与最终导出 coverage 不一致。

处理：共享坐标、参数和公式；golden/reference tests 同时验证 overlay 输入与 evaluator 输出。

### 25.4 Brush asset 可变导致 Undo 损坏

处理：N3 在 UI 绘制前完成 content-addressed immutable `Put()`；不允许临时继续覆盖同 key。

### 25.5 QuickQanava 变成第二份 graph 状态

处理：adapter 单向投影，connector 请求经 backend；Version/Undo 后从 document revision 同步，
不从 Qan object 反向重建领域图。

### 25.6 History payload 过度依赖 JSON diff

处理：typed target + typed operation，结构删除保存完整 before/after 对象；canonical payload
测试确保 hash 稳定。

### 25.7 Paste 误复制源 RAW 状态

处理：始终从目标 root 建文档并保留目标 Develop；transfer package 显式列出可转移内容。

### 25.8 UI panel 太窄

处理：先按现有 320 px VI 建立纵向主链；在 token 允许的 260–460 px 范围内提供受控 resize，
不通过缩小 hit target 或字体解决。

---

## 26. 全局完成条件

- [ ] `PipelineDocument` 是 node、adjustment、history、Version 和 render 的唯一可写编辑状态。
- [ ] QuickQanava 是唯一 node graph UI 基础，不存在自研 graph canvas/connector。
- [ ] Develop 与 DRT/Post 唯一且不可删除，全部 Color Grade 构成有效 image backbone。
- [ ] 用户新建的是 Clean Color Grade，不继承 `+1.5 EV` / `+30`。
- [ ] DRT/Post 专属调整不能出现在中间 Color Grade。
- [ ] 一个 Color Grade 支持多个 Mask，组合只有 Union。
- [ ] `color_range` 和 `luminance_range` 是 Mask 的两个直接预留字段。
- [ ] Brush/Radial/Linear Gradient 的简单辅助范围由 QSG 直接绘制。
- [ ] 实际蒙版调整通过统一接口产生 Interactive/Quality 结果。
- [ ] 调色参数输入时不显示 mask editing overlay。
- [ ] node topology 修改直接请求 Quality render。
- [ ] node、Mask 和参数操作都能生成准确 history row 并 Undo/Redo。
- [ ] 每个 Version 对应自己的 DAG，root/new default Version 对应三节点文档。
- [ ] Adjustment Transfer 只创建 Paste Version，不再创建新的 pipeline merge commit。
- [ ] Mask raster asset 不可变，stroke Undo/Redo 不依赖被覆盖文件。
- [ ] CUDA、OpenCL、Metal 不使用 CPU 或其他 backend 替代路径。
- [ ] 无可用图的 stage-only 存储拒绝打开；产品路径不使用 stage 镜像。
- [ ] Windows/macOS package 可加载 QuickQanava QML module 和全部新 panel。
- [ ] 真实 RAW E2E、reopen、Version、Paste、性能和资源证据全部记录在最终资格验证记录中。

---

## 27. 本总方案的维护方式

- 本文件保持为总体设计依据，不在这里追踪每个提交的临时执行步骤；
- 开始某个 `NMx` Phase 时，在第 21 节预留路径创建执行方案，把代码路径改成可点击链接，并
  根据当时的代码审计定义 `NMx.1` 到 `NMx.n`；
- 一级 Phase 状态在第 21 节维护为 `planned`、`in progress` 或 `complete`；具体子 Phase
  状态、提交、测试命令和临时发现只写入对应执行方案；
- 实施中发现需要改变锁定语义时，先更新本文并记录原因，再调整执行方案；
- 仅实现细节、文件拆分或同一 `NMx` 内部子 Phase 顺序变化，不要求反复修改本文；
- 最终产品资格验证完成后，在本文更新总体 Status、主要调用链、真实测试命令、性能数据和
  仍然存在的限制。
