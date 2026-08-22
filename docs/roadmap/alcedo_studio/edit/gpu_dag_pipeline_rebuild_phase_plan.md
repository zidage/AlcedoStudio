# GPU DAG 编辑管线重构 Phase 计划

Date: 2026-08-22

Status: 设计阶段。实现尚未开始。

Delivery: Stacked PR。当前文档分支 `feature/gpu-pipeline-dag-redesign` 是整个堆栈的根。

Primary owner: Alcedo Studio 编辑管线。

Affected areas:

- `alcedo_studio/src/edit/operators/`
- `alcedo_studio/src/edit/pipeline/`
- `alcedo_studio/src/include/edit/operators/`
- `alcedo_studio/src/include/edit/pipeline/`
- `alcedo_studio/src/decoders/processor/`
- CUDA、OpenCL 和 Metal 编辑后端
- `PipelineMgmtService` 创建和持有管线的路径
- 编辑历史使用的管线 JSON
- 新的蒙版存储模块

UI scope:

- 本计划不修改 QML。
- 本计划不修改 Qt Widgets 编辑面板。
- 本计划不增加 UI 胶水代码。
- app 层只负责创建服务、创建管线和传递已有的编辑数据。

## 1. 目标

本计划用一个简单的 GPU DAG 替换当前基于 `PipelineStage`、`IOperatorBase::Apply` 和
`OperatorParams` 总结构的编辑管线。

完成后：

1. 默认管线只有三个用户可见节点：
   `Develop Endpoint -> Primary Color Grade -> DRT Endpoint`。
2. `edit/operators/` 只保存参数 Model、默认值、JSON 转换和 DTO 创建逻辑。
3. 参数 Model 不接收图像，也没有 `Apply` 或 `ApplyGPU`。
4. PipelineStage、stage 排序、merged stage 和左右 stage 缓存传递全部删除。
5. 节点连接决定图像流顺序。
6. 调色节点内部的明确列表决定该节点内的调整顺序。
7. GraphCompiler 把节点图编译成后端 Pass。
8. GPU workspace 统一管理参数、临时缓冲区、纹理和 KV 缓存。
9. Model 使用 dirty bit 或 dirty field mask。实现中不使用参数计数器。
10. 当前管线只允许一个 in-flight 帧。
11. LibRaw open、unpack 和 RAW 降采样在编辑管线之前完成。
12. Develop、调色、蒙版、Mix 和 DRT 的像素计算只在 GPU 上运行。
13. 用户裁切、旋转、视图 ROI 和动态分辨率由一个坐标模块计算。
14. 上述几何变化只产生一次图像重采样。
15. Rasterized Mask 使用 R8 数据，任意一条边不大于 4096。
16. 持久蒙版存储与 GPU 纹理 LRU 分开。
17. CUDA 先完成完整参考实现。OpenCL 和 Metal 随后使用同一套图、Model、DTO、
    workspace 模板和坐标逻辑。

## 2. 不在本计划中的工作

- 不实现完整的 Nuke 节点能力。
- 不实现多种 Mix 模式。
- 不实现多个 in-flight 帧。
- 不保留 CPU 图像处理后端。
- 不让 CPU 参与 Develop、调色、蒙版、Mix 或 DRT 像素计算。
- 不实现 AI 图像分割模型。
- 不修改编辑器 UI。
- 不修改现有调色面板的视觉设计。
- 不在每个后端建立单独的 MaskService。
- 不在每个算子中建立自己的 GPU 内存池。
- 不在每个算子中建立自己的长期缓存。
- 不把 ROI 字段复制进每一个算子参数。

## 3. 已确定的设计

| 领域 | 决定 |
| --- | --- |
| 默认图 | `Develop -> Primary Color Grade -> DRT` |
| 端点数量 | 一个 Develop 根节点，一个 DRT 输出节点 |
| 中间节点 | 一个或多个 ColorGradeNode；默认创建一个 |
| 蒙版 | 独立节点，通过可选 mask 输入连接到 ColorGradeNode |
| Mix | 只支持 Normal；未连接蒙版时使用常数 1 |
| 算子 | 纯参数 Model，不执行图像处理 |
| DTO | 不可变参数数据或 dirty Patch，不包含 GPU 类型 |
| 参数更新 | dirty bit 或 dirty field mask |
| 图修改 | 一个 `topology_dirty` 标记 |
| 并行帧 | 一个 in-flight 帧 |
| GPU 参数区 | 每个 RenderDevice 一份稳定 ParameterArena |
| GPU 内存 | `BasicRenderWorkspace<Backend>` 统一管理 |
| 后端复用 | C++ 模板和 Backend Traits |
| GPU 运行入口 | 最外层使用类型隐藏；模板内部使用静态类型 |
| GPU 缓存 | workspace 内的通用 KV cache 和 texture LRU |
| 持久蒙版 | app 之下的 MaskStore 模块 |
| 主存蒙版缓存 | MaskStore 可以按字节预算保存 R8 副本 |
| GPU 蒙版缓存 | workspace 按 GPU 字节预算管理 |
| Rasterized Mask | R8 UNORM；任意一条边不大于 4096 |
| Feather | GPU exact signed Euclidean distance field；持久数据仍为 R8 |
| RAW 输入 | LibRaw open、unpack、active area 和降采样在管线外 |
| Develop 输出 | scene-linear ACES AP1 |
| 中间调色 | scene-linear ACES AP1 |
| 局部色温 | 基于 CAT02，以 AP1 白点为参考 |
| DRT | ACES 2.0 或 OpenDRT |
| Geometry | 管线文档的全局 ImageGeometryModel，不是第四个用户节点 |
| ROI | RenderGeometryResolver 统一计算 |
| 动态分辨率 | RenderRequest 输入；不保存进持久参数 |
| 图像重采样 | GraphCompiler 插入一个内部 GeometryResamplePass |
| 后端顺序 | CUDA，然后 OpenCL，然后 Metal |
| 稳定运行 | 每帧不得创建和销毁 GPU 缓冲区或纹理 |

## 4. 当前结构及需要删除的问题

### 4.1 OperatorParams 同时保存太多类型的数据

`alcedo_studio/src/include/edit/operators/op_base.hpp` 中的 `OperatorParams` 当前同时保存：

- 用户调整值；
- RAW metadata；
- 运行时颜色矩阵；
- ROI；
- 输出缩放；
- LLF 数据；
- 后端派生值；
- 缓存 key；
- dirty 状态。

它随后被转换成 CUDA、OpenCL 和 Metal 的大参数结构。渲染路径会重复写入和上传大量没有
变化的数据。

目标结构不创建一个新的总参数结构。每个 Model 只拥有自己的参数。RenderContext 只拥有
当前渲染数据。PreparedRawInput 只拥有输入图像数据。

### 4.2 IOperatorBase 同时表示参数和执行

当前 `IOperatorBase` 同时包含：

- `Apply`
- `ApplyGPU`
- JSON 读取和写入
- `SetGlobalParams`
- `EnableGlobalParams`
- stage
- priority
- operator type
- 历史合并辅助逻辑

目标结构把这些职责分开：

- OperatorModel：参数。
- NodeModel：图中的节点。
- DTO：Model 到运行时的数据。
- RuntimeDefinition：某种调整在一个后端上的执行入口。
- Pass：GPU 工作。
- 历史服务：编辑历史与参数合并。

### 4.3 PipelineStage 混合图顺序、缓存和执行

当前 PipelineStage 保存：

- operator map；
- 前后 stage 指针；
- merged stage；
- stage cache；
- CPU/GPU 选择；
- scratch memory；
- GPU wrapper；
- stage 参数导入和导出。

目标结构删除整个 PipelineStage 类型。节点图保存用户顺序。ExecutionPlan 保存 GPU 工作
顺序。workspace 保存 GPU 资源和缓存。

### 4.4 LLF 自己管理 scratch 和缓存

当前 LLF 分配自己的临时 GPU 内存，并维护自己的参考数据缓存。目标结构让 LLF 从
workspace 请求临时内存和 KV 数据。LLF 不持有内存池。

### 4.5 ROI 在多个位置重复解释

当前 `x/y/scale/reference size` 会进入 RenderDesc、ViewportRenderRegion、OperatorParams、
后端参数和 LLF 缓存逻辑。

目标结构只让 RenderGeometryResolver 解释这些数据。其他模块读取
`ResolvedRenderGeometry` 或 `TextureSamplingPlan`。

## 5. 目标架构

```text
Application
├── PipelineMgmtService
├── MaskStore 配置
└── RenderDevice 创建与生命周期
          │
          ├─────────────────────┐
          ▼                     ▼
    PipelineDocument         MaskStore
    ├── PipelineGraph        ├── R8 文件
    └── ImageGeometryModel   └── 主存 R8 LRU
          │                     │
          └──────────┬──────────┘
                     ▼
               GraphCompiler
                     │
                     ▼
               ExecutionPlan
                     │
                     ▼
          RenderDevice<Backend>
                     │
                     ▼
       BasicRenderWorkspace<Backend>
       ├── ParameterArena
       ├── TransientBufferArena
       ├── TexturePool
       ├── MaskTextureCache
       ├── NodeResultCache
       └── SharedGpuResources
```

图像数据流：

```text
文件字节
   │
   ▼
RawInputLoader，CPU，编辑管线外
   ├── LibRaw open
   ├── LibRaw unpack
   ├── active area
   ├── DecodeRes 降采样
   └── PreparedRawInput
            │
            ▼
┌─────────────────────┐
│ Develop Endpoint    │
│ GPU                 │
│ 输出 AP1 scene RGB  │
└──────────┬──────────┘
           │
           │ 内部 GeometryResamplePass
           ▼
┌─────────────────────┐
│ Primary Color Grade │◀──── optional MaskNode
│ GPU                 │
│ mix = 1.0           │
└──────────┬──────────┘
           │
           ▼
┌─────────────────────┐
│ DRT Endpoint        │
│ GPU                 │
│ ACES 2.0/OpenDRT    │
└──────────┬──────────┘
           │
           ▼
 display-referred GPU texture
```

## 6. 核心类型

### 6.1 Model

Model 保存用户可以修改和序列化的参数。

Model 可以：

- 保存参数；
- 检查和限制参数；
- 提供默认值；
- 写入 JSON；
- 从 JSON 读取；
- 标记 dirty 字段；
- 生成完整 DTO；
- 取出 dirty Patch；
- 判断当前状态是否为默认值。

Model 不可以：

- 接收 ImageBuffer；
- 包含 CUDA、OpenCL 或 Metal 类型；
- 分配 GPU 内存；
- 创建纹理；
- 提交 GPU 命令；
- 查询 workspace；
- 保存 RenderContext；
- 保存当前 ROI；
- 执行 Apply。

### 6.2 DTO

DTO 是 Model 与 Pipeline Runtime 之间的数据。

DTO 只包含：

- 稳定类型 ID；
- 数据版本；
- 不可变参数值；
- 对于 Patch，包含 dirty field mask。

DTO 不包含：

- JSON；
- GPU 指针；
- GPU 资源；
- workspace 指针；
- app service；
- UI 状态；
- 参数计数器。

### 6.3 Node

Node 是用户可以组装和序列化的图对象。

Node 包含：

- NodeId；
- 节点类型 ID；
- 输入端口；
- 输出端口；
- 参数 Model；
- 对于 ColorGradeNode，可选蒙版输入和 mix。

Node 不执行 GPU 工作。

### 6.4 Pass

Pass 是 GraphCompiler 生成的后端工作。

Pass 可以是：

- UploadRawPass；
- DevelopLinearizePass；
- HighlightRecoveryPass；
- DemosaicPass；
- CameraToAp1Pass；
- GeometryResamplePass；
- PointAdjustmentPass；
- LocalToneReferencePass；
- LocalToneApplyPass；
- MaskRasterPass；
- MaskFeatherPass；
- MixPass；
- DrtPass。

Pass 不保存到节点图 JSON。Pass 没有固定的全局分组。

### 6.5 ExecutionPlan

ExecutionPlan 保存：

- 拓扑排序后的节点工作；
- 后端 Pass；
- 节点输出到 workspace value key 的映射；
- 每个参数字段在 ParameterArena 中的位置；
- 临时内存峰值；
- 纹理需求；
- 节点及其下游的 dirty 传播表。

ExecutionPlan 不拥有 GPU 内存。

### 6.6 RenderContext

RenderContext 保存一帧的只读数据：

```cpp
struct RenderContext {
  SourceContentKey source_key;
  RenderQuality quality;
  const ResolvedRenderGeometry& geometry;
};
```

它不保存算子参数，也不保存参数 dirty 状态。

## 7. 默认三节点图

默认 PipelineDocument 创建：

```text
NodeId: develop
Type: alcedo.node.develop

NodeId: grade.primary
Type: alcedo.node.color_grade

NodeId: drt
Type: alcedo.node.drt
```

默认连接：

```text
develop.image -> grade.primary.image
grade.primary.image -> drt.image
```

默认图没有 MaskNode。未连接的 mask 输入使用常数 1。

### 7.1 Develop Endpoint

Develop 接收 PreparedRawInput 或已解码的直接 RGB 输入。

RAW 模式参数包括：

- 高光恢复；
- 解拜尔方法；
- AI 降噪；
- RAW 白平衡；
- 相机颜色矩阵选择；
- 镜头校正；
- AP1 转换。

Develop 不执行：

- LibRaw open；
- LibRaw unpack；
- RAW DecodeRes 降采样。

Develop 输出固定为：

```text
float RGB
scene-linear
ACES AP1 primaries
ACES white point
```

### 7.2 Primary Color Grade

Primary Color Grade 默认包含明确的调整顺序：

1. CAT02 scene white balance；
2. Exposure；
3. Contrast；
4. White；
5. Black；
6. Shadows；
7. Highlights 和 LLF；
8. Curve；
9. HLS；
10. Saturation；
11. Vibrance；
12. Color Wheel；
13. LMT 或 LUT；
14. Clarity；
15. Sharpen；
16. Halation；
17. Film Grain。

每个调整是独立 Model。ColorGradeNode 不包含新的总参数结构。

默认值是 identity 或当前产品默认值：

```text
CAT02 temperature offset = 0
CAT02 tint offset = 0
Exposure = 0 EV
Contrast = 0
White = identity
Black = identity
Shadows = 0
Highlights = 0
Curve = identity
Saturation = 1
Vibrance = 0
Clarity = 0
Sharpen amount = 0
Halation strength = 0
Film Grain strength = 0
mix = 1
mask = disconnected
```

Model 在默认状态下仍然存在。这样第一次修改滑块只产生参数 Patch，不改变图结构。

### 7.3 DRT Endpoint

DRT 参数包括：

- ACES 2.0 或 OpenDRT；
- encoding space；
- EOTF；
- limiting space；
- peak luminance；
- OpenDRT look 和 tonescale 参数。

DRT 接收 AP1 scene-linear 图像，输出 display-referred GPU texture。

### 7.4 端点规则

- 图中必须有一个 Develop Endpoint。
- Develop 没有图像输入端口。
- 图中必须有一个 DRT Endpoint。
- DRT 有一个图像输入端口。
- DRT 没有 scene-referred 图像输出端口。
- 所有显示路径必须从 Develop 到达 DRT。
- ColorGradeNode 必须位于这条路径中或被明确绕过。
- MaskNode 只能连接到 mask 输入。
- 当前 Mix 只支持 Normal。

## 8. 纯参数 OperatorModel

目标基础接口：

```cpp
class IOperatorModel {
 public:
  virtual ~IOperatorModel() = default;

  [[nodiscard]] virtual auto Type() const -> OperatorTypeId = 0;
  [[nodiscard]] virtual auto IsDefault() const -> bool = 0;
  [[nodiscard]] virtual auto IsDirty() const -> bool = 0;

  [[nodiscard]] virtual auto MakeFullDto() const
      -> OperatorParamDto = 0;

  virtual auto TakeDirtyPatch()
      -> std::optional<OperatorParamPatchDto> = 0;

  virtual void RestoreDirty(DirtyFieldMask fields) = 0;
  virtual void MarkAllDirty() = 0;

  [[nodiscard]] virtual auto ToJson() const
      -> nlohmann::json = 0;

  virtual void LoadJson(const nlohmann::json& json) = 0;
};
```

从接口中删除：

```text
Apply
ApplyGPU
SetGlobalParams
EnableGlobalParams
GetPriorityLevel
GetStage
```

同时删除：

```text
OperatorBase<Derived> 的 stage 和 priority 辅助函数
OpStream
OperatorParams
GPUOperatorParams 总结构
OpenCL 总参数结构
Metal 总参数结构
```

### 8.1 Dirty field mask

一个字段只有 clean 和 dirty 两种状态。

示例：

```cpp
enum class Cat02WhiteBalanceDirty : std::uint32_t {
  None = 0,
  Enabled = 1U << 0,
  Temperature = 1U << 1,
  Tint = 1U << 2,
  All = Enabled | Temperature | Tint,
};
```

连续修改同一个字段不会增加计数：

```text
temperature = 0.1 -> dirty
temperature = 0.2 -> dirty
temperature = 0.3 -> dirty

下一帧只发送 temperature = 0.3
```

### 8.2 TakeDirtyPatch

`TakeDirtyPatch` 在一个短锁中执行：

1. 读取 dirty mask；
2. 复制当前最新值；
3. 清除已经取出的 dirty bits；
4. 返回不可变 Patch。

如果 UI 在该操作之后再次修改参数，setter 会重新设置对应 dirty bit。

如果上传前取消或失败，运行时调用 `RestoreDirty`。

### 8.3 完整 DTO

`MakeFullDto` 用于：

- 初次建立 ExecutionPlan；
- 新建 ParameterArena；
- GPU 设备切换；
- GPU 设备丢失后的恢复；
- 载入一份完整管线文档。

它不依赖 dirty 状态。

## 9. ColorGradeNodeModel

ColorGradeNodeModel 保存一个明确的调整列表：

```cpp
struct AdjustmentModelEntry {
  AdjustmentInstanceId instance_id;
  std::unique_ptr<IOperatorModel> model;
};

class ColorGradeNodeModel final : public INodeModel {
 public:
  static auto Defaults() -> ColorGradeNodeModel;

  auto Adjustments()
      -> std::span<std::unique_ptr<IOperatorModel>>;

  void InsertAdjustment(
      std::size_t index,
      std::unique_ptr<IOperatorModel> model);

  void RemoveAdjustment(AdjustmentInstanceId id);
  void MoveAdjustment(AdjustmentInstanceId id, std::size_t index);

  void SetEnabled(bool enabled);
  void SetMix(float mix);

 private:
  std::vector<AdjustmentModelEntry> adjustments_;

  bool enabled_ = true;
  float mix_ = 1.0f;

  bool enabled_dirty_ = true;
  bool mix_dirty_ = true;
};
```

同一种调整可以有多个实例，因此参数更新使用 `AdjustmentInstanceId`，不只使用类型 ID。

调整列表的顺序是用户数据。GraphCompiler 可以合并连续的 pointwise 行为，但不能改变计算
顺序。

## 10. DTO 与参数 Patch

完整 DTO：

```cpp
struct OperatorParamDto {
  OperatorTypeId type;
  std::uint32_t data_version = 1;
  std::shared_ptr<const IOperatorParamPayload> payload;
};
```

参数 Patch：

```cpp
struct OperatorParamPatchDto {
  NodeId node_id;
  AdjustmentInstanceId adjustment_id;
  OperatorTypeId type;
  DirtyFieldMask dirty_fields;
  std::shared_ptr<const IOperatorParamPayload> payload;
};
```

图结构 Patch 不与参数 Patch 混合。添加、删除或移动节点时，只设置：

```cpp
bool topology_dirty = true;
```

多次图修改仍然只保留一个 dirty 状态。

## 11. 类型 ID 与注册

不再使用需要集中修改的 `OperatorType` enum。

每种调整使用稳定字符串 ID：

```cpp
inline constexpr OperatorTypeId kExposureType{
    "alcedo.adjustment.exposure"};

inline constexpr OperatorTypeId kCat02WhiteBalanceType{
    "alcedo.adjustment.cat02_white_balance"};
```

运行时可以保存字符串的 64 位哈希。注册时检查重复。JSON 保存原始字符串。

注册数据：

```cpp
struct AdjustmentDefinition {
  OperatorTypeId type;
  std::string_view display_name;

  OperatorModelFactory create_default_model;

  CudaAdjustmentFactory create_cuda_runtime;
  OpenClAdjustmentFactory create_opencl_runtime;
  MetalAdjustmentFactory create_metal_runtime;
};
```

新增一种调整时：

1. 编写 Params、Model 和 DTO；
2. 编写当前目标后端的 runtime 行为；
3. 在 builtin adjustment catalog 中增加一项；
4. 如果它属于默认调色节点，在默认明确列表中增加一项。

不需要修改：

- 中央 enum；
- stage enum；
- stage 排序；
- merged stream；
- OperatorParams；
- 后端总参数转换；
- PipelineStage cache；
- GraphCompiler 类型 switch。

## 12. PipelineGraph

PipelineGraph 是一个简单 DAG。

端口类型：

```cpp
enum class PortDataType {
  SceneImage,
  DisplayImage,
  Mask,
};
```

基本节点：

```text
DevelopEndpointNode
ColorGradeNode
DrtEndpointNode
AnalyticMaskNode
RasterMaskNode
```

当前不支持：

- 任意多输入图像合成；
- 多种 blend mode；
- feedback edge；
- 用户自定义 shader；
- 动态循环；
- 子图宏；
- 通用脚本节点。

### 12.1 编译流程

```text
PipelineDocument
  -> 检查节点和端口
  -> 拓扑排序
  -> 解析 Model DTO
  -> 查询 Runtime Registry
  -> 建立参数位置
  -> 计算临时内存峰值
  -> 生成 Pass
  -> 生成 dirty 传播表
  -> ExecutionPlan
```

### 12.2 参数变化

参数变化不重新编译 DAG：

```text
Model setter
  -> dirty bit
  -> TakeDirtyPatch
  -> ExecutionPlan 查找 ParameterBinding
  -> ParameterArena 写入字段
  -> 标记 GPU dirty range
```

### 12.3 图变化

这些操作设置 `topology_dirty`：

- 添加或删除节点；
- 连接或断开端口；
- 改变调色节点内部调整顺序；
- 添加或删除调色节点内部的调整实例；
- 连接或移除蒙版输入。

GraphCompiler 成功生成新计划后清除 `topology_dirty`。失败时保留旧计划和 dirty 状态。

## 13. PipelineStage 删除

最终代码不得再包含：

```text
PipelineStageName
PipelineStage
PipelineStage::StageRole
Merged_Stage
OperatorEntry 的 stage 排序
prev_stage_
next_stage_
dependents_
input_cache_valid_
output_cache_valid_
ApplyStage
RefreshGlobalParams
SetExecutionStages
```

节点之间通过 `GraphValueId` 传递结果：

```cpp
struct GraphValueId {
  NodeId producer;
  PortId output_port;
};
```

消费者直接从 workspace 的 KV cache 获取输入：

```cpp
auto input = workspace.Values().Get(input_value_id);
```

没有左右 stage 握手。

## 14. GPU Pass 与调整融合

用户图中的 Node 数量不等于 GPU Pass 数量。

默认三节点图在 CUDA 上可能生成：

```text
UploadRawPass
DevelopLinearizePass
HighlightRecoveryPass
DemosaicPass
CameraToAp1Pass
GeometryResamplePass
PointAdjustmentPass
LocalToneReferencePass
LocalToneApplyPass
DetailAdjustmentPass
DrtPass
```

连续 pointwise 调整可以融合：

```text
CAT02 white balance
Exposure
Contrast
White
Black
Curve
HLS
Saturation
Vibrance
Color Wheel
LMT
```

需要邻域数据或独立中间纹理的调整生成独立 Pass：

```text
LLF
Clarity
Sharpen
Halation
Film Grain
Mask Feather
```

融合只是一种编译结果。它不改变 Model，也不改变 JSON 顺序。

## 15. 模板化 GPU workspace

目标类型：

```cpp
template <class Backend>
class BasicRenderWorkspace {
 public:
  using Buffer = typename Backend::Buffer;
  using Texture2D = typename Backend::Texture2D;
  using CommandContext = typename Backend::CommandContext;

  auto Parameters() -> ParameterArena<Backend>&;
  auto TransientBuffers() -> TransientBufferArena<Backend>&;
  auto Textures() -> TexturePool<Backend>&;
  auto MaskTextures() -> MaskTextureCache<Backend>&;
  auto Values() -> NodeResultCache<Backend>&;

  void BeginRender(CommandContext& command_context);
  void EndRender(CommandContext& command_context);
};
```

别名：

```cpp
using CudaRenderWorkspace =
    BasicRenderWorkspace<CudaBackend>;

using OpenClRenderWorkspace =
    BasicRenderWorkspace<OpenClBackend>;

using MetalRenderWorkspace =
    BasicRenderWorkspace<MetalBackend>;
```

Backend Traits 只提供基础操作：

- 创建和释放 buffer；
- 创建 R8、RGBA 和浮点 texture；
- 上传 buffer range；
- 上传 R8 texture 或 dirty rect；
- 建立 mask mip levels；
- 查询 GPU 资源字节数；
- 提交命令；
- 等待本次提交完成；
- 查询资源是否仍被本次提交使用。

通用模板负责：

- grow-only buffer；
- bump allocation；
- texture pool；
- LRU；
- resource lease；
- KV cache；
- dirty range 合并；
- 参数位置；
- 预算检查。

后端不能建立自己的 MaskService 或独立 LRU 实现。

### 15.1 Runtime 外层

不同后端的原生头文件不能全部进入一个普通 C++ 编译单元。最外层使用一次类型隐藏：

```cpp
class IRenderDevice {
 public:
  virtual ~IRenderDevice() = default;

  virtual void Execute(
      const ExecutionPlan& plan,
      const RenderRequest& request) = 0;
};

template <class Backend>
class RenderDevice final : public IRenderDevice {
 private:
  Backend backend_;
  BasicRenderWorkspace<Backend> workspace_;
};
```

模板实例分别位于 `.cu`、`.cpp` 和 `.mm` 文件。

### 15.2 Workspace 生命周期

RenderDevice 拥有 workspace。PipelineDocument 和 ExecutionPlan 不拥有 workspace。

因此：

- 重建节点图不会释放 workspace；
- 切换参数不会释放 workspace；
- 切换 ROI 不会释放 workspace；
- 动态分辨率变化不会释放 workspace；
- 关闭一个 ExecutionPlan 不会清空所有 GPU 缓存；
- GPU 设备切换或设备丢失时才重建 workspace。

## 16. 单 in-flight 与 dirty 参数上传

每个 RenderDevice 只有一份 ParameterArena。

渲染顺序：

```text
等待前一次 GPU 提交完成
  -> BeginRender
  -> 收集 dirty Patch
  -> 更新 ParameterArena 主存镜像
  -> 合并 dirty ranges
  -> 只上传这些 ranges
  -> 编码 GPU Pass
  -> 提交
  -> EndRender
```

不实现：

- in-flight lane；
- 参数 ring；
- per-frame 参数副本；
- 参数计数器；
- 参数更新 FIFO。

### 16.1 参数 Patch 的安全处理

`TakeDirtyPatch` 取出最新数据并清除相应 dirty bits。

如果上传之前发生取消或失败：

```text
PendingParameterPatch destructor
  -> RestoreDirty
```

如果上传成功，调用 `Commit`。

如果 UI 在 Patch 取出后继续修改参数，setter 会再次设置 dirty bit。下一帧发送新值。

### 16.2 初次构建和设备重建

初次构建或 GPU 设备重建不读取 dirty 状态。运行时对全部 Model 调用 `MakeFullDto`。

## 17. ParameterArena

GraphCompiler 为每个调整实例分配稳定位置：

```cpp
struct ParameterBinding {
  std::uint32_t offset;
  std::uint32_t size;
  std::span<const ParameterFieldBinding> fields;
};

struct ParameterFieldBinding {
  DirtyFieldMask dirty_bit;
  std::uint32_t source_offset;
  std::uint32_t destination_offset;
  std::uint32_t size;
};
```

ParameterArena：

- 预留峰值空间；
- 只在没有活动 GPU 提交时增长；
- 稳定运行时不增长；
- 合并相邻 dirty ranges；
- 不在每帧创建 buffer；
- 不在每帧复制完整参数区。

## 18. RenderGeometryResolver

RenderGeometryResolver 统一处理：

- RAW 解码比例；
- full reference extent；
- 用户裁切；
- 用户旋转；
- view crop；
- viewport target extent；
- 动态分辨率；
- filter footprint；
- LLF 参考采样；
- Rasterized Mask 采样；
- Analytic Mask 坐标。

它只计算矩阵、矩形和尺寸。它不处理图像像素，也不分配 GPU 内存。

### 18.1 坐标空间

`DecodedSpace`：

- LibRaw 解包和降采样后的实际像素。

`ReferenceSpace`：

- 完整图像的稳定逻辑空间；
- 使用标准化坐标 `[0, 1]`；
- 不随 preview DecodeRes 改变；
- 持久蒙版和用户 crop 使用此空间。

`EditSpace`：

- 用户 crop 和 rotation 后的空间。

`RenderSpace`：

- view crop 和动态分辨率后的 GPU 输出空间。

`MaskTextureSpace`：

- 某个 R8 mask texture 的 UV 空间。

### 18.2 输入

```cpp
struct SourceGeometry {
  Extent2D decoded_extent;
  Extent2D full_reference_extent;
  RectI sensor_active_area;
  std::uint8_t downsample_passes;
  Matrix3x3 decoded_to_reference;
};

struct ImageGeometryParams {
  NormalizedRect crop_rect;
  float rotation_degrees;
  bool expand_to_fit;
};

struct ViewRequest {
  NormalizedRect visible_rect_in_edit_space;
  Extent2D viewport_extent;
};

struct ResolutionRequest {
  float render_scale;
  std::uint32_t max_edge;
  RenderQuality quality;
};
```

### 18.3 输出

```cpp
struct ResolvedRenderGeometry {
  Extent2D decoded_extent;
  Extent2D full_reference_extent;
  Extent2D edit_extent;
  Extent2D render_extent;

  Matrix3x3 decoded_to_reference;
  Matrix3x3 reference_to_edit;
  Matrix3x3 edit_to_render;

  Matrix3x3 reference_to_render;
  Matrix3x3 render_to_reference;
  Matrix3x3 render_to_decoded;

  RectI required_decoded_region;
  RectI required_reference_region;

  GpuRenderGeometry gpu_data;
};
```

### 18.4 一次图像重采样

用户 crop、rotation、view crop 和 dynamic resolution 不分别生成图像操作。

GraphCompiler 在 Develop 输出和第一个 ColorGradeNode 之间插入一个内部
`GeometryResamplePass`：

```text
Develop output
  -> one GeometryResamplePass
  -> RenderSpace image
  -> ColorGradeNode
```

GeometryResamplePass 不是用户节点，也不是 stage。默认图仍然只有三个节点。

### 18.5 像素中心和矩形

- 像素采样点使用 `(x + 0.5, y + 0.5)`。
- 整数区域使用左闭右开形式。
- 所有 rounding 只在 RenderGeometryResolver 中完成一次。
- 非直角旋转用四个角计算轴对齐范围。
- 输入范围按滤波半径扩大后再限制到有效图像范围。

### 18.6 SamplingFootprint

运行时行为声明需要的邻域：

```cpp
struct SamplingFootprint {
  float radius_x;
  float radius_y;
  bool requires_full_reference;
};
```

Pointwise 行为使用零半径。Blur 和 feather 使用实际半径。LLF 可以请求完整低分辨率参考。

## 19. RAW 输入与 Develop 边界

新增管线外输入类型：

```cpp
struct PreparedRawInput {
  HostImagePlane pixels;

  Extent2D decoded_extent;
  Extent2D full_reference_extent;
  RectI sensor_active_area;

  RawCfaPattern cfa_pattern;
  CfaPhase cfa_phase;
  std::uint8_t downsample_passes;

  RawBlackLevel black_level;
  float white_level;

  RawColorMetadata color_metadata;
  RawCameraMetadata camera_metadata;

  SourceContentKey content_key;
};
```

RawInputLoader 在 CPU 上执行：

```text
open file or buffer
LibRaw open
LibRaw unpack
active area crop
DecodeRes downsample
CFA pattern and phase update
PreparedRawInput creation
```

这些操作不属于编辑管线。

Develop Endpoint 从上传 PreparedRawInput 开始。之后的图像计算只使用 GPU。

## 20. CAT02 scene white balance

新增的 CAT02 色温调整属于 ColorGradeNode，不属于 Develop。

它与 RAW white balance 分开：

| 项目 | RAW white balance | CAT02 scene white balance |
| --- | --- | --- |
| 所属位置 | Develop Endpoint | ColorGradeNode |
| 输入 | camera/CFA 数据 | AP1 scene-linear RGB |
| 参考 | 相机 metadata 或用户 RAW CCT | AP1 白点 |
| 蒙版 | 不支持局部 mask | 支持 ColorGradeNode mask |
| 主要用途 | 胶片 develop | scene-referred 局部或全局调色 |

默认 CAT02 偏移为 0。

## 21. MaskStore

MaskStore 是 app 之下的独立模块。app 创建实例，并把读取接口交给管线。

MaskStore 不包含 GPU 头文件。

职责：

- 用户配置的蒙版根目录；
- MaskAssetKey；
- R8 文件读取；
- R8 文件写入；
- 文件头和尺寸检查；
- 内容哈希检查；
- 主存 R8 LRU；
- 写临时文件后替换目标文件；
- 按字节预算清理主存副本。

磁盘文件是用户数据，不能因为 LRU 预算不足而自动删除。

建议接口：

```cpp
struct RasterMaskDescriptor {
  Extent2D extent;
  RectF reference_bounds;
  PixelFormat format = PixelFormat::R8Unorm;
  std::uint32_t data_version = 1;
};

struct RasterMaskData {
  MaskAssetKey key;
  RasterMaskDescriptor descriptor;
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
};

class IMaskStore {
 public:
  virtual ~IMaskStore() = default;

  virtual auto Load(const MaskAssetKey& key)
      -> RasterMaskData = 0;

  virtual auto Save(
      const RasterMaskDescriptor& descriptor,
      std::span<const std::uint8_t> pixels)
      -> MaskAssetKey = 0;
};
```

尺寸规则：

```text
width <= 4096
height <= 4096
format == R8 UNORM
row bytes == width
pixel bytes == width * height
```

## 22. GPU Mask Texture Cache

GPU mask texture LRU 属于 `BasicRenderWorkspace<Backend>`。

获取流程：

```text
RasterMaskNode
  -> GraphAssetTable 中的 RasterMaskData
  -> workspace.MaskTextures().Acquire
  -> R8 GPU texture
  -> TextureLease
```

TextureLease 在本次 GPU 提交完成前固定资源。LRU 只能清理没有被当前提交使用的纹理。

MaskService 或 MaskStore 不知道：

- CUDA texture object；
- OpenCL image；
- Metal texture；
- GPU 字节预算；
- GPU LRU；
- GPU completion event。

### 22.1 Rasterized Mask

Rasterized Mask 的长期数据是 R8。

当前支持来源：

- brush path rasterization；
- 导入的 R8 mask；
- 将来的 AI segmentation 输出。

GPU feather 可以使用临时 R16 或 R32 数据，但这些数据只存在于 workspace。最终采样纹理
可以保持 R8。

### 22.2 活动蒙版

用户绘制尚未保存的 raster mask 使用 dirty rectangle：

```cpp
dirty_rect = Union(dirty_rect, changed_rect);
```

下一次渲染只上传 dirty rectangle。多次绘制在一帧前合并为一个区域。

保存时计算新的 MaskAssetKey。

### 22.3 Feather 算法

Rasterized Mask feather 不使用 Gaussian blur。Blur 会让大半径边缘产生不稳定的形状，
也不能直接表达 ReferenceSpace 中的准确距离。

目标实现使用 exact signed Euclidean distance field：

1. 输入是原始 R8 coverage texture。
2. 保留原始抗锯齿 coverage。
3. 分别找出 mask 内部和外部的边界距离。
4. 使用 PBA+ 形式的 GPU Parallel Banding Algorithm 计算 exact Euclidean distance。
5. 合并内部和外部距离，生成 signed distance texture。
6. 使用 ReferenceSpace 中的 feather radius 计算最终 coverage。
7. 输出供调色采样的 R8 texture。

PBA 的原始研究描述了 GPU exact Euclidean distance transform。PBA+ 是该方法的后续改进。
实现依据：

- [NUS Parallel Banding Algorithm project page](https://www.comp.nus.edu.sg/~tants/pba.html)
- [Parallel Banding Algorithm paper](https://www.comp.nus.edu.sg/~tants/pba_files/pba.pdf)

GPU 派生资源：

```text
BaseR8
  -> boundary classification
  -> inside exact distance
  -> outside exact distance
  -> SignedDistanceR32F
  -> feather evaluation
  -> FeatheredR8
```

规则：

- BaseR8 是持久数据。
- SignedDistanceR32F 和 FeatheredR8 是 workspace 资源。
- SignedDistanceR32F 的 key 只依赖 MaskAssetKey。
- FeatheredR8 的 key 依赖 MaskAssetKey 和 feather 参数。
- 改变 feather radius 复用 SignedDistanceR32F。
- 改变 viewport ROI 或 dynamic resolution 不重新计算 distance field。
- feather radius 使用 ReferenceSpace pixels。
- resolver 把 ReferenceSpace radius 转成 mask texel 距离。
- radius 为 0 时直接采样 BaseR8。
- 活动蒙版只在 dirty rectangle 影响的区域和所需传播范围内更新；如果变化可能影响整个
  distance field，则重新运行完整 distance transform。
- 所有中间纹理由 workspace 提前保留。稳定运行时不创建 GPU texture。
- 实现根据公开算法说明独立编写，不复制研究项目的源代码。

CUDA 先实现和调优 PBA+。OpenCL 和 Metal 使用相同的分带、合并、距离计算和 coverage
规则。三个后端使用 CPU exact EDT fixture 作为测试参考，但产品渲染不执行 CPU feather。

## 23. Mask 采样

TextureSamplingPlan：

```cpp
struct TextureSamplingPlan {
  Matrix3x3 render_to_texture_uv;
  Vector2 uv_dx;
  Vector2 uv_dy;
  float mip_level;
  TextureFilter filter;
};
```

Rasterized Mask：

```text
Render pixel
  -> render_to_reference
  -> reference_bounds
  -> mask UV
  -> R8 texture sample
```

关系：

```cpp
render_to_mask_uv =
    reference_to_mask_uv *
    render_to_reference;
```

动态分辨率变化时：

- R8 texture 保持不变；
- feather 派生 texture 保持不变；
- 只更新 TextureSamplingPlan。

Analytic Mask：

- 使用 `render_to_reference`；
- 在 ReferenceSpace 计算椭圆、ND 线和 feather；
- 不创建 raster texture，除非编译器判断预计算更快。

## 24. Mask 类型

### 24.1 Radial Mask

参数：

```text
center in ReferenceSpace
major radius
minor radius
rotation
inner feather
outer feather
invert
```

### 24.2 Graduated ND Mask

参数：

```text
origin line
normal direction
transition distance
start value
end value
invert
```

### 24.3 Raster Mask

参数：

```text
MaskAssetKey
reference bounds
feather radius in reference pixels
invert
```

## 25. 统一的 ColorGrade Mix

每个 ColorGradeNode 使用同一个公式：

```cpp
effective_mask =
    mask_connected
        ? clamp(sampled_mask * mix, 0.0f, 1.0f)
        : clamp(mix, 0.0f, 1.0f);

output =
    input + effective_mask * (adjusted - input);
```

默认：

```text
mask disconnected
mix = 1
enabled = true
```

编译器可以在安全时把 Mix 融入最后一个调整 Pass。

## 26. LLF

LLF 不再保存：

- 自己的 scratch allocator；
- 自己的 GPU buffer pool；
- 自己的明暗 mask KV cache；
- 自己的 ROI 解释；
- 自己的 reference size 逻辑。

LLF 使用：

- workspace transient buffers；
- workspace node result KV cache；
- ResolvedRenderGeometry；
- TextureSamplingPlan；
- 一个完整或低分辨率 reference resource。

LLF reference cache 不包含当前 viewport ROI。用户平移或缩放视图时可以复用它。

## 27. Cache 和 dirty 传播

参数 dirty 后，GraphDirtyTracker 标记当前节点和所有下游节点：

```text
Develop dirty:
  Develop dirty
  all ColorGrade nodes dirty
  DRT dirty

Grade B dirty:
  upstream nodes stay valid
  Grade B dirty
  downstream nodes dirty

DRT dirty:
  upstream nodes stay valid
  DRT dirty
```

MaskNode dirty 时，只标记读取该 mask 的 ColorGradeNode 及其下游。

### 27.1 缓存身份

缓存使用实际输入值或内容 hash，不使用递增数字。

| 资源 | 是否包含 viewport ROI | key 来源 |
| --- | ---: | --- |
| 磁盘 R8 数据 | 否 | R8 descriptor 和 pixels |
| 主存 R8 数据 | 否 | MaskAssetKey |
| GPU base R8 texture | 否 | MaskAssetKey |
| feather texture | 否 | MaskAssetKey 和 feather 参数 |
| mask mip levels | 否 | MaskAssetKey |
| mask sampling plan | 是 | descriptor 和 ResolvedRenderGeometry |
| LLF reference | 否 | input content 和 LLF 参数 |
| LLF 当前输出 | 是 | reference resource 和当前 geometry |
| 普通节点输出 | 视行为而定 | input content、params 和 geometry |

同一组实际输入产生相同 key。视图回到原位置时可以再次使用相同数据。

## 28. 序列化

PipelineDocument 保存：

- format version；
- ImageGeometryModel；
- nodes；
- edges；
- 每个 Node 的参数；
- ColorGradeNode 内部明确的 adjustment 顺序；
- MaskAssetKey 和 mask descriptor；
- Develop 和 DRT 参数。

PipelineDocument 不保存：

- stage；
- priority；
- GPU 参数 offset；
- GPU 资源；
- workspace state；
- cache valid flag；
- viewport ROI；
- dynamic render resolution；
- in-flight state；
- dirty bits。

默认 JSON 形状：

```json
{
  "format_version": 2,
  "geometry": {
    "crop_rect": [0.0, 0.0, 1.0, 1.0],
    "rotation_degrees": 0.0,
    "expand_to_fit": true
  },
  "nodes": [
    {
      "id": "develop",
      "type": "alcedo.node.develop",
      "params": {}
    },
    {
      "id": "grade.primary",
      "type": "alcedo.node.color_grade",
      "enabled": true,
      "mix": 1.0,
      "adjustments": [
        {
          "id": "grade.primary.cat02_wb",
          "type": "alcedo.adjustment.cat02_white_balance",
          "params": {
            "temperature_offset": 0.0,
            "tint_offset": 0.0
          }
        },
        {
          "id": "grade.primary.exposure",
          "type": "alcedo.adjustment.exposure",
          "params": {
            "exposure_ev": 0.0
          }
        }
      ]
    },
    {
      "id": "drt",
      "type": "alcedo.node.drt",
      "params": {
        "method": "open_drt"
      }
    }
  ],
  "edges": [
    {
      "from": ["develop", "image"],
      "to": ["grade.primary", "image"]
    },
    {
      "from": ["grade.primary", "image"],
      "to": ["drt", "image"]
    }
  ]
}
```

### 28.1 旧数据读取

迁移只提供一个方向：

```text
旧 stage JSON
  -> LegacyPipelineImporter
  -> 新 PipelineDocument
```

新 PipelineDocument 不再写回旧 stage JSON。

导入规则：

- RAW 和镜头参数进入 DevelopModel；
- crop 和 rotation 进入 ImageGeometryModel；
- scene-referred 调整进入 Primary Color Grade；
- ODT 或 OpenDRT 参数进入 DrtModel；
- 缺失字段使用新默认值；
- 未知旧 operator 记录错误并停止导入，不静默丢弃用户参数。

## 29. CPU 路径删除

最终实现不提供以下图像处理入口：

```text
IOperatorBase::Apply
CPUPipelineExecutor 图像计算
CPU tile operator stream
CPU fallback after GPU operator failure
CPU LLF
CPU mask feather
CPU DRT
```

允许 CPU 执行：

- 文件读取；
- LibRaw open 和 unpack；
- RAW DecodeRes 降采样；
- JSON 读取和写入；
- 参数 Model 更新；
- GraphCompiler 控制逻辑；
- 坐标矩阵计算；
- GPU 命令编码；
- 最终显示提交所需的 host 工作。

GPU 后端失败时返回明确错误。它不能静默改用 CPU 图像处理。

## 30. CUDA、OpenCL 和 Metal

### 30.1 CUDA

CUDA 是第一份完整实现。它定义：

- 默认三节点图的参考行为；
- dirty 参数上传；
- workspace 生命周期；
- geometry 和 ROI；
- R8 mask；
- feather；
- Mix；
- LLF；
- DRT；
- 缓存和分配指标。

### 30.2 OpenCL

OpenCL 使用同一个：

- PipelineDocument；
- GraphCompiler；
- OperatorModel；
- DTO；
- dirty Patch；
- RenderGeometryResolver；
- BasicRenderWorkspace 模板；
- MaskStore；
- 缓存策略。

OpenCL 只实现 Backend Traits、Pass encoder、kernel 和原生资源包装。

OpenCL 1.2 没有可移植的 mipmapped image 路径时，MaskTexture 可以内部保存多个
`image2d` level。上层仍然使用同一个 TextureSamplingPlan。

新的 OpenCL kernel 必须通过项目的 OpenCL program registry 注册。

### 30.3 Metal

Metal 使用同一个上层设计。

Metal 后端文件使用 Objective-C++。Metal texture 和 buffer 使用轻量包装类型，不暴露给
Model、DTO 或 GraphCompiler。

Metal compute pipeline state 必须使用共享的 pipeline cache。不能在每次 Pass 执行时创建。

## 31. Stacked PR 工作方式

本计划使用 Stacked PR，因为单个 PR 无法让 Model、图、workspace、CUDA、蒙版、
OpenCL、Metal 和旧代码删除都保持可读。

规则：

1. 当前文档 PR 是堆栈根。
2. 每个实现 PR 只包含一个明确的系统切片。
3. 每个子 PR 的 base 是前一个 feature 分支。
4. 每个 PR 必须单独构建并运行自己的测试。
5. 子 PR 描述顶部列出整个堆栈，并标出当前 PR。
6. 评审从堆栈底部开始。
7. 父 PR 更新后，子分支使用 rebase 更新。
8. 父 PR 合并到 main 后，直接子分支 rebase 到新的 main。
9. 使用 `--force-with-lease` 更新已经 rebase 的远端子分支。
10. 不把 OpenCL 或 Metal 移植混入 CUDA 行为 PR。
11. 不在后端 PR 中修改 UI。
12. 最终清理 PR 删除过渡适配器和旧实现。

推荐 PR 描述头：

```text
Stack:
[x] GPU DAG design
[ ] Model and graph foundation        <- current
[ ] CUDA workspace
[ ] Render geometry
[ ] CUDA Develop
[ ] CUDA Color Grade
[ ] CUDA Mask and Mix
[ ] CUDA DRT and product path
[ ] OpenCL
[ ] Metal
[ ] Final removal and qualification
```

## 32. Stacked PR 总表

| Phase | 分支 | Base | 主要结果 |
| --- | --- | --- | --- |
| G0 | `feature/gpu-pipeline-dag-redesign` | `main` | 本设计文档和 roadmap 索引 |
| G1 | `feature/gpu-dag-model-graph` | G0 | Model、DTO、DAG、默认三节点图、JSON |
| G2 | `feature/gpu-dag-cuda-workspace` | G1 | Backend Traits、CUDA workspace、dirty 参数区、KV cache |
| G3 | `feature/gpu-dag-render-geometry` | G2 | ROI 坐标系统和单次 GeometryResamplePass |
| G4 | `feature/gpu-dag-cuda-develop` | G3 | RawInputLoader 边界和 CUDA Develop Endpoint |
| G5 | `feature/gpu-dag-cuda-grade` | G4 | CUDA 调色、CAT02、LLF workspace 化 |
| G6 | `feature/gpu-dag-cuda-mask-mix` | G5 | MaskStore、R8 mask、feather、Mix |
| G7 | `feature/gpu-dag-cuda-drt-product` | G6 | CUDA DRT 和 app 管线路径切换 |
| G8 | `feature/gpu-dag-opencl` | G7 | OpenCL 完整移植 |
| G9 | `feature/gpu-dag-metal` | G8 | Metal 完整移植 |
| G10 | `feature/gpu-dag-final-removal` | G9 | 删除旧 stage、CPU 图像路径和过渡代码；全平台验证 |

## 33. Phase G0 — 设计与堆栈根

Branch: `feature/gpu-pipeline-dag-redesign`

Status: 本文档创建后完成。

工作：

- 写入本 Phase 计划；
- 在 roadmap README 中增加 Image editing pipeline 分类；
- 在项目总 roadmap 中增加链接；
- 记录所有已确定设计；
- 记录每个 Stacked PR 的 base；
- 不修改产品代码。

完成条件：

- 文档覆盖 Model、DTO、Node、Pass、workspace、ROI、RAW、Mask、DRT 和后端顺序；
- 文档明确默认三节点图；
- 文档明确不使用参数计数器；
- 文档明确 PipelineStage 最终删除；
- 文档中的分支全部使用 `feature/` 前缀。

## 34. Phase G1 — Model、DTO 和 PipelineGraph

Branch: `feature/gpu-dag-model-graph`

Base: `feature/gpu-pipeline-dag-redesign`

目标：

- 建立不依赖 GPU 的 Model、DTO 和简单 DAG；
- 建立默认三节点图；
- 建立 format version 2 JSON；
- 让新结构与旧执行路径并存，以保持该 PR 可构建。

工作：

- 增加 `OperatorTypeId`；
- 增加 `IOperatorModel`；
- 增加完整 DTO 和 dirty Patch；
- 增加 NodeId、PortId 和 GraphValueId；
- 增加 DevelopNodeModel、ColorGradeNodeModel 和 DrtNodeModel；
- 增加 AnalyticMaskNodeModel 和 RasterMaskNodeModel 数据类型；
- 增加 ImageGeometryModel；
- 增加 PipelineDocument；
- 增加 DAG 检查和拓扑排序；
- 增加默认三节点工厂；
- 增加新 JSON writer；
- 增加 LegacyPipelineImporter 的结构，不切换产品读取路径；
- 不增加 GPU 执行。

测试：

```text
DefaultPipelineHasDevelopGradeAndDrtNodes
DefaultPipelineConnectsDevelopThroughPrimaryGradeToDrt
DefaultPrimaryGradeContainsOrderedSceneAdjustments
DefaultPrimaryGradeUsesFullMixAndNoMask
PipelineGraphRejectsCycle
PipelineGraphRejectsDisplayImageConnectedToSceneInput
PipelineGraphRejectsMaskConnectedToImageInput
RepeatedExposureWritesCollapseIntoOneDirtyPatch
DirtyPatchTakenBeforeNewEditLeavesNewEditDirty
CancelledParameterTransferRestoresDirtyFields
PipelineDocumentRoundTripPreservesNodeIdsEdgesAndAdjustmentOrder
LegacyStageJsonMapsRawGradeGeometryAndDrtToNewDocument
```

完成条件：

- Model 头文件不包含 GPU 头文件或 ImageBuffer；
- 默认图严格有三个节点；
- 参数更新只使用 dirty；
- 图修改只使用 `topology_dirty`；
- JSON 中没有 stage 或 priority；
- 旧产品执行路径仍然构建。

## 35. Phase G2 — CUDA workspace 和参数传输

Branch: `feature/gpu-dag-cuda-workspace`

Base: `feature/gpu-dag-model-graph`

目标：

- 建立模板化 workspace；
- 建立第一份 CUDA Backend Traits；
- 建立单份 ParameterArena；
- 建立 dirty range 上传；
- 建立 workspace KV cache 和 texture pool。

工作：

- 增加 Backend Traits 所需类型；
- 增加 `BasicRenderWorkspace<Backend>`；
- 增加 CUDA buffer 和 texture 包装；
- 增加 grow-only ParameterArena；
- 增加 TransientBufferArena 和 scope；
- 增加 TexturePool；
- 增加 ResourceLease；
- 增加 NodeResultCache；
- 增加 dirty range 合并；
- 增加单 in-flight BeginRender/EndRender；
- 增加完整 DTO 初始化路径；
- 增加上传失败后的 dirty 恢复；
- 不迁移实际调色 kernel。

测试：

```text
ParameterArenaKeepsStableOffsetsAcrossRenders
ParameterArenaUploadsOnlyDirtyFieldRanges
AdjacentDirtyParameterRangesMergeBeforeCudaCopy
RepeatedDirtyWritesUseLatestValue
CancelledCudaParameterCopyRestoresDirtyFields
WorkspaceResetRewindsTransientMemoryWithoutFreeingCudaAllocation
WorkspaceCannotGrowWhileTransientPointersAreLive
NodeResultCacheReturnsValueByProducerNodeAndPort
TextureLeasePreventsEvictionUntilCudaSubmissionCompletes
SecondRenderUsesNoCudaAllocationAfterPeakReserve
```

完成条件：

- 稳定第二帧不调用 `cudaMalloc` 或 `cudaFree`；
- 未改变参数时不执行参数 H2D copy；
- 一个字段改变时不上传完整参数区；
- workspace 生命周期独立于 ExecutionPlan。

## 36. Phase G3 — RenderGeometryResolver

Branch: `feature/gpu-dag-render-geometry`

Base: `feature/gpu-dag-cuda-workspace`

目标：

- 统一 crop、rotation、view ROI 和动态分辨率；
- 生成 CUDA GeometryResamplePass；
- 为 LLF 和 mask 提供同一坐标映射。

工作：

- 增加坐标空间类型；
- 增加 SourceGeometry；
- 增加 ViewRequest 和 ResolutionRequest；
- 增加 ResolvedRenderGeometry；
- 增加 pixel-center 规则；
- 增加 left-closed/right-open integer rect；
- 增加非直角旋转 bounds；
- 增加 SamplingFootprint；
- 增加 RequiredInputRegion；
- 增加 TextureSamplingPlan；
- 增加 CUDA GeometryResamplePass；
- 把 crop、view crop 和 resize 合并到一个 CUDA kernel；
- 不增加用户 GeometryNode。

测试：

```text
RenderGeometryRoundTripsReferenceAndRenderPixelCenters
FullCropZeroRotationMapsReferenceCornersToRenderCorners
RotatedCropBoundsContainAllFourTransformedCorners
ViewportCropAndDynamicScaleProduceRequestedRenderExtent
DecodeScaleDoesNotChangeNormalizedReferenceCoordinates
RequiredInputRegionExpandsForBicubicFootprintAndClampsToDecodedBounds
RasterMaskSamplingMapsSameReferencePointAtQuarterAndFullPreview
CropRotateViewportAndScaleExecuteAsOneCudaResample
OddImageDimensionsUseOneRoundingResultAcrossImageMaskAndLlf
```

完成条件：

- 当前 ROI 字段不再进入新 Operator DTO；
- GeometryResamplePass 不成为用户节点；
- 默认图仍然只有三个节点；
- 图像、LLF 和 mask 使用相同的 render-to-reference 数据。

## 37. Phase G4 — CUDA Develop Endpoint

Branch: `feature/gpu-dag-cuda-develop`

Base: `feature/gpu-dag-render-geometry`

目标：

- 把 LibRaw unpack 和 RAW 降采样移到管线输入之前；
- 实现 CUDA Develop Endpoint；
- 输出 AP1 scene-linear GPU 图像。

工作：

- 增加 RawInputLoader；
- 增加 PreparedRawInput；
- 记录 active area、DecodeRes、CFA pattern 和 phase；
- 保留 CPU LibRaw unpack；
- 保留 CPU RAW 降采样；
- 从旧 RawDecodeOp 移出 GPU Develop 行为；
- 把高光恢复、解拜尔、AI 降噪、RAW white balance、镜头校正和 camera-to-AP1
  编译为 Develop Pass；
- 使用 workspace 管理所有 Develop 临时内存；
- 建立直接 RGB 输入；
- 建立 Develop output GraphValue。

测试：

```text
RawInputLoaderUnpacksBeforePipelineBuild
RawInputLoaderDownsampleUpdatesCfaPatternAndPhase
PreparedRawInputKeepsFullReferenceExtentAcrossDecodeRes
CudaDevelopProducesFiniteAp1SceneLinearRgbFromBayerInput
CudaDevelopProducesFiniteAp1SceneLinearRgbFromXTransInput
CudaDevelopUsesWorkspaceForAllTemporaryBuffers
CudaDevelopSecondRenderCreatesNoGpuAllocation
DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint
```

完成条件：

- ExecutionPlan 中没有 LibRaw Pass；
- GPU 不执行 RAW DecodeRes 降采样；
- Develop 不调用 CPU 图像算子；
- Develop 输出格式和色彩空间明确。

## 38. Phase G5 — CUDA Primary Color Grade

Branch: `feature/gpu-dag-cuda-grade`

Base: `feature/gpu-dag-cuda-develop`

目标：

- 实现默认 ColorGradeNode 的 CUDA 执行；
- 增加纯 Model，并让新 CUDA 路径只使用这些 Model；
- 旧 operator 类只供尚未移植的 OpenCL 和 Metal 路径临时使用；
- 把 LLF 内存和缓存迁入 workspace。

工作：

- 实现 CUDA adjustment runtime registry；
- 实现 pointwise fusion；
- 实现 CAT02 scene white balance；
- 迁移 Exposure、Contrast、White、Black；
- 迁移 Shadows、Highlights 和 LLF；
- 迁移 Curve、HLS、Saturation、Vibrance；
- 迁移 Color Wheel 和 LMT；
- 迁移 Clarity、Sharpen、Halation 和 Film Grain；
- 每个 Model 生成自己的 DTO；
- 每个 runtime behavior 生成自己的 GPU 参数；
- 新 Model 不提供 Apply、ApplyGPU、SetGlobalParams 或 stage metadata；
- 不向旧 operator 类增加新行为；G10 在三个后端完成后删除它们；
- 删除 LLF 内部 allocator；
- 删除 LLF 内部 mask/base KV cache；
- 让 LLF 使用 workspace 和 RenderGeometryResolver。

测试：

```text
CudaPrimaryGradeDefaultParametersPreserveDevelopOutput
CudaExposurePatchChangesOnlyExposureParameterRange
CudaCat02WhiteBalanceZeroOffsetPreservesAp1White
CudaCat02WhiteBalanceMaskedSampleMatchesFullAdjustmentAtMaskOne
CudaPointAdjustmentsExecuteInSerializedModelOrder
CudaLocalToneReferenceReusesAcrossViewportChanges
CudaLocalToneUsesWorkspaceInsteadOfPrivateAllocation
CudaColorGradeSecondRenderCreatesNoGpuAllocation
MovingAdjustmentChangesExecutionOrderWithoutChangingOtherParameters
```

完成条件：

- 默认调色模型都能在 CUDA 上执行；
- 默认参数产生 identity 或当前规定的默认结果；
- 普通 slider 修改不重新编译 DAG；
- 新 CUDA 路径不调用旧 operator 的 Apply 或 SetGlobalParams；
- 尚未移植的后端仍可通过旧代码构建；
- LLF 不再拥有 GPU 内存池或长期缓存。

## 39. Phase G6 — MaskStore、CUDA Mask 和 Mix

Branch: `feature/gpu-dag-cuda-mask-mix`

Base: `feature/gpu-dag-cuda-grade`

目标：

- 实现持久 R8 mask；
- 实现 CUDA analytic 和 raster mask；
- 实现 GPU feather；
- 实现 ColorGradeNode Normal Mix。

工作：

- 增加 MaskStore 模块；
- 增加可配置 root；
- 增加 MaskAssetKey；
- 增加 R8 文件格式；
- 增加原子式文件替换；
- 增加主存 R8 LRU；
- 增加 workspace MaskTextureCache；
- 增加 R8 CUDA texture；
- 增加 dirty rectangle texture upload；
- 增加 RadialMask；
- 增加 GraduatedNdMask；
- 增加 RasterMask；
- 增加 CUDA PBA+ exact signed distance feather；
- 增加 mask mip levels；
- 增加 TextureSamplingPlan；
- 增加 Mix 或安全融合；
- 增加序列化 key 读取和写入。

测试：

```text
MaskStoreRejectsRasterMaskLargerThan4096OnEitherAxis
MaskStoreRoundTripPreservesR8PixelsDescriptorAndKey
MaskStoreUsesConfiguredRoot
MaskStoreReplacesFileOnlyAfterCompleteWrite
MaskHostCacheEvictsByBytesWithoutDeletingMaskFile
CudaMaskTextureCacheReusesTextureForSameMaskAssetKey
CudaMaskTextureCacheDoesNotEvictTextureUsedByActiveSubmission
CudaRasterMaskUploadsOnlyUnionedDirtyRectangle
CudaRadialMaskMatchesReferenceSpaceEllipseAtPreviewScales
CudaGraduatedNdMaskFollowsReferenceSpaceNormal
CudaFeatherPreservesZeroAndOnePlateaus
CudaSignedDistanceFeatherMatchesExactEuclideanReferenceWithinTolerance
CudaFeatherPreservesAntialiasedSourceBoundary
CudaFeatherRadiusIsStableAcrossDynamicRenderScales
ChangingFeatherRadiusReusesSignedDistanceTexture
CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne
```

完成条件：

- MaskStore 不包含 GPU 类型；
- GPU mask LRU 只存在于 workspace；
- 持久 raster mask 使用 R8；
- mask 尺寸检查生效；
- feather 使用 exact signed Euclidean distance field；
- 改变 feather radius 不重新计算 signed distance；
- dynamic resolution 不重新创建持久 mask texture。

## 40. Phase G7 — CUDA DRT 和产品管线路径

Branch: `feature/gpu-dag-cuda-drt-product`

Base: `feature/gpu-dag-cuda-mask-mix`

目标：

- 实现 CUDA DRT Endpoint；
- 让 Windows/CUDA 产品路径使用新默认三节点图；
- 接入 PipelineMgmtService；
- 保持 OpenCL 和 Metal 旧路径可构建，等待后续子 PR。

工作：

- 实现 CUDA ACES 2.0 DRT；
- 实现 CUDA OpenDRT；
- 实现显示色域和 EOTF；
- 实现 DRT dirty Patch；
- 让 PipelineMgmtService 创建 PipelineDocument；
- 让 CUDA RenderDevice 执行 ExecutionPlan；
- 接入已有 render scheduler 和 frame sink；
- 接入 LegacyPipelineImporter；
- 新保存格式只写 format version 2；
- 增加运行时错误报告；
- CUDA 失败不使用 CPU 图像处理；
- 保留 OpenCL 和 Metal 编译适配器，后续 PR 删除。

测试：

```text
DefaultCudaPipelineBuildsThreeVisibleNodes
CudaDrtOpenDrtProducesFiniteDisplayReferredOutput
CudaDrtAces20ProducesFiniteDisplayReferredOutput
ChangingDrtPeakLuminanceKeepsDevelopAndGradeCacheValid
PipelineMgmtServiceBuildsDefaultGpuDagForNewImage
LegacyPipelineImportRendersSameCudaReferenceWithinTolerance
CudaBackendFailureDoesNotEnterCpuImageProcessing
CudaDefaultPipelineSecondRenderCreatesNoGpuAllocation
```

完成条件：

- Windows/CUDA 产品路径默认使用新 DAG；
- 默认图保存为三个节点；
- CUDA 从输入到 DRT 完整运行；
- UI 文件没有修改。

## 41. Phase G8 — OpenCL 移植

Branch: `feature/gpu-dag-opencl`

Base: `feature/gpu-dag-cuda-drt-product`

目标：

- 使用相同设计完成 OpenCL；
- 不复制 graph、workspace、mask service 或 ROI 系统。

工作：

- 实现 OpenClBackend Traits；
- 实例化 BasicRenderWorkspace；
- 实现 buffer、image 和 command context 包装；
- 实现 Develop Pass；
- 实现 GeometryResamplePass；
- 实现默认 ColorGrade runtime；
- 实现 CAT02、LLF、mask、feather 和 Mix；
- 实现 ACES 2.0 和 OpenDRT；
- 使用 OpenClProgramLibrary 和 program registry；
- 对没有 mipmap 的设备使用多个 image2d level；
- 删除 OpenCL 旧 PipelineStage 适配器。

测试：

```text
OpenClDefaultPipelineBuildsThreeVisibleNodes
OpenClDevelopMatchesCudaReferenceWithinTolerance
OpenClGeometryMatchesCudaReferenceAtRotatedViewportRoi
OpenClPrimaryGradeMatchesCudaReferenceWithinTolerance
OpenClRasterMaskSamplingMatchesCudaAtDynamicResolutions
OpenClDrtMatchesCudaReferenceWithinTolerance
OpenClWorkspaceSecondRenderCreatesNoBufferOrImage
OpenClBackendFailureDoesNotEnterCpuImageProcessing
```

完成条件：

- OpenCL 使用同一 Model、DTO、GraphCompiler 和 workspace 模板；
- OpenCL 稳定渲染不创建 GPU buffer 或 image；
- OpenCL 产品路径不再使用 PipelineStage。

## 42. Phase G9 — Metal 移植

Branch: `feature/gpu-dag-metal`

Base: `feature/gpu-dag-opencl`

目标：

- 使用相同设计完成 Metal；
- 保持 macOS 产品行为与 CUDA 参考一致。

工作：

- 实现 MetalBackend Traits；
- 实例化 BasicRenderWorkspace；
- 实现 Metal buffer、texture 和 command context 包装；
- 使用 ComputePipelineCache；
- 实现 Develop Pass；
- 实现 GeometryResamplePass；
- 实现默认 ColorGrade runtime；
- 实现 CAT02、LLF、mask、feather 和 Mix；
- 实现 ACES 2.0 和 OpenDRT；
- 删除 Metal 旧 PipelineStage 适配器。

测试：

```text
MetalDefaultPipelineBuildsThreeVisibleNodes
MetalDevelopMatchesCudaReferenceWithinTolerance
MetalGeometryMatchesCudaReferenceAtRotatedViewportRoi
MetalPrimaryGradeMatchesCudaReferenceWithinTolerance
MetalRasterMaskSamplingMatchesCudaAtDynamicResolutions
MetalDrtMatchesCudaReferenceWithinTolerance
MetalWorkspaceSecondRenderCreatesNoBufferOrTexture
MetalComputePipelineStatesAreReusedAcrossRenders
MetalBackendFailureDoesNotEnterCpuImageProcessing
```

完成条件：

- Metal 使用同一 Model、DTO、GraphCompiler 和 workspace 模板；
- Metal 稳定渲染不创建 buffer、texture 或 compute pipeline state；
- Metal 产品路径不再使用 PipelineStage。

## 43. Phase G10 — 最终删除与全平台验证

Branch: `feature/gpu-dag-final-removal`

Base: `feature/gpu-dag-metal`

目标：

- 删除所有旧执行结构；
- 确认三个后端只使用新 DAG；
- 完成性能、内存和序列化验证。

删除：

- PipelineStage；
- PipelineStageName；
- merged stage；
- stage cache；
- stage 邻接指针；
- OperatorParams；
- 后端总参数结构和总参数转换；
- IOperatorBase::Apply；
- IOperatorBase::ApplyGPU；
- SetGlobalParams；
- EnableGlobalParams；
- CPU pipeline image execution；
- LLF 私有 allocator 和 cache；
- 每个算子的 GPU 内存管理；
- 旧 OpenCL 和 Metal adapter；
- 已迁移后的重复 kernel 入口；
- 旧 stage JSON writer。

验证：

```text
DefaultPipelineContainsExactlyDevelopGradeAndDrt
AllBuiltInOperatorModelsHaveNoImageApplyEntryPoint
AllGpuBackendsUseBasicRenderWorkspace
NoPipelineStageTypeRemainsInFirstPartySource
NoOperatorParamsAggregateRemainsInFirstPartySource
NoCpuImageOperatorEntryPointRemainsInProductPipeline
DefaultPipelineRoundTripPreservesThreeNodeGraph
LegacyPipelineImporterPreservesSupportedUserAdjustments
RasterMaskRoundTripPreservesR8DataAndSamplingBounds
CropRotateViewportAndDynamicResolutionMatchAcrossBackends
SteadyStateRenderAllocatesNoGpuBufferOrTextureAcrossBackends
OnlyDirtyParameterRangesTransferAcrossBackends
```

完成条件：

- CUDA、OpenCL 和 Metal 产品路径全部使用新 DAG；
- 没有 CPU 图像处理接口；
- 没有 PipelineStage；
- 没有 OperatorParams 总结构；
- 默认图只有三个用户可见节点；
- 所有临时 GPU 资源由 workspace 管理；
- 所有持久蒙版通过 MaskStore；
- 三个后端通过共同参考测试。

## 44. 测试分层

### 44.1 Model 单元测试

- 默认值；
- 参数限制；
- JSON 读取和写入；
- dirty field mask；
- 连续修改合并；
- TakeDirtyPatch 与新修改并发；
- RestoreDirty；
- MakeFullDto。

### 44.2 Graph 单元测试

- 端口类型；
- cycle 检查；
- 拓扑顺序；
- 默认三节点；
- mask edge；
- topology dirty；
- 调整实例顺序。

### 44.3 Geometry property 测试

- forward/inverse round trip；
- pixel center；
- odd dimensions；
- arbitrary rotation；
- crop 和 viewport intersection；
- DecodeRes 不改变 ReferenceSpace；
- mask、LLF 和 image 映射一致。

### 44.4 Workspace 单元测试

- grow-only；
- live allocation 时不增长；
- scope rewind；
- stable parameter offsets；
- dirty range merge；
- resource lease；
- LRU byte budget；
- KV lookup；
- no allocation after reserve。

### 44.5 后端集成测试

- Develop；
- Geometry；
- Primary Grade；
- CAT02；
- LLF；
- masks；
- feather；
- Mix；
- DRT；
- dynamic resolution；
- serialization；
- legacy import。

### 44.6 后端一致性测试

使用同一输入、PipelineDocument、RenderRequest 和 MaskAsset：

```text
CUDA reference
OpenCL output
Metal output
```

测试需要为每类行为定义清楚的误差范围。离散 mask 边缘和随机 grain 需要固定 seed。

### 44.7 性能和分配测试

- 首帧创建成本单独报告；
- 第二帧开始不得分配 GPU buffer 或 texture；
- 无参数变化时参数上传字节数必须为 0；
- 单字段变化时只上传对应字段范围；
- viewport 变化不得重新上传不变的 R8 mask；
- dynamic resolution 变化不得重新生成不变的 feather resource；
- LLF reference 可以跨 viewport 变化复用；
- GPU 内存预算和 LRU 清理字节数可查询。

## 45. 构建与运行

Windows 配置和构建使用项目包装脚本：

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug --output-on-failure
```

macOS：

```text
cmake --preset macos_debug
cmake --build --preset macos_debug --target alcedo_main
ctest --test-dir build/macos-debug --output-on-failure
```

每个 Stacked PR 只运行它影响的集中测试和必要回归测试。G10 运行完整测试集。

## 46. 全局完成条件

### 46.1 架构

- [ ] 默认 PipelineDocument 有且只有 Develop、Primary Color Grade 和 DRT 三个节点。
- [ ] Geometry 是全局 Model 和内部 Pass，不是第四个用户节点。
- [ ] MaskNode 是独立节点。
- [ ] ColorGradeNode 有可选 mask 输入和单一 Normal Mix。
- [ ] PipelineStage 全部删除。
- [ ] OperatorParams 总结构全部删除。
- [ ] operators 只保存参数 Model、DTO 和序列化逻辑。
- [ ] GPU 执行代码不在 operators 参数目录。

### 46.2 参数

- [ ] 不使用参数计数器。
- [ ] 连续修改合并为一个 dirty Patch。
- [ ] 没有变化的参数不上传。
- [ ] 单字段变化不上传完整参数区。
- [ ] GPU workspace 重建时使用完整 DTO。

### 46.3 GPU 资源

- [ ] BasicRenderWorkspace 通过模板支持 CUDA、OpenCL 和 Metal。
- [ ] 每个 RenderDevice 只有一个 workspace。
- [ ] 每个 RenderDevice 只有一份 ParameterArena。
- [ ] 稳定渲染不创建或销毁 GPU buffer 和 texture。
- [ ] LLF 不管理自己的内存池。
- [ ] Mask GPU LRU 只存在于 workspace。

### 46.4 RAW 和颜色

- [ ] LibRaw unpack 在编辑管线之前完成。
- [ ] RAW DecodeRes 降采样在 CPU 输入准备中完成。
- [ ] Develop 输出 AP1 scene-linear RGB。
- [ ] CAT02 调整以 AP1 白点为参考。
- [ ] DRT 支持 ACES 2.0 和 OpenDRT。

### 46.5 Geometry 和 Mask

- [ ] crop、rotation、view ROI 和 dynamic resolution 只执行一次图像重采样。
- [ ] LLF 和 mask 使用相同的 RenderGeometry 数据。
- [ ] Rasterized Mask 使用 R8。
- [ ] Rasterized Mask 任意一条边不大于 4096。
- [ ] Rasterized Mask 作为 GPU texture 采样。
- [ ] Rasterized Mask feather 使用 GPU exact signed Euclidean distance field。
- [ ] 改变 feather radius 复用 signed distance texture。
- [ ] view 或 render scale 变化不重新创建不变的 mask texture。
- [ ] MaskStore root 可以由用户配置。

### 46.6 后端

- [ ] CUDA 完整通过。
- [ ] OpenCL 完整通过。
- [ ] Metal 完整通过。
- [ ] 三个后端使用同一 PipelineDocument、GraphCompiler、Model、DTO、Geometry 和
      workspace 模板。
- [ ] GPU 后端失败时不进入 CPU 图像处理。

### 46.7 数据

- [ ] 新 JSON 只保存节点、边、Model 和 MaskAssetKey。
- [ ] 新 JSON 不保存 GPU 或 viewport 短期状态。
- [ ] 旧 stage JSON 可以单向导入。
- [ ] 新保存不再写旧 stage JSON。

## 47. 风险与处理

### 47.1 旧管线删除范围大

处理：

- 使用 Stacked PR；
- 每个 PR 保持可构建；
- CUDA 先完成端到端；
- OpenCL 和 Metal 各自独立移植；
- 最后一个 PR 才删除全部过渡代码。

### 47.2 默认调色顺序变化

处理：

- 默认顺序写入明确列表；
- legacy importer 使用固定映射；
- 使用 golden 图像和参数 round-trip 测试；
- GraphCompiler 不自动改变 Model 顺序。

### 47.3 ROI 与 mask 错位

处理：

- 只允许 RenderGeometryResolver 计算坐标；
- image、LLF 和 mask 共享同一结果；
- 使用 pixel-center property 测试；
- 使用 crop、rotation、odd size 和 dynamic resolution 组合测试。

### 47.4 GPU 内存增长

处理：

- workspace 使用明确字节预算；
- texture 使用 LRU；
- active submission 使用 lease；
- transient memory 在每帧完成后 rewind；
- 测试记录稳定状态分配次数和峰值。

### 47.5 Stacked PR 评审困难

处理：

- 每个 PR 描述列出完整堆栈；
- 每个 PR 只处理一个系统切片；
- 测试名称说明具体行为；
- 每个 Phase 完成后在本文件记录主要调用路径、测试和未完成项；
- 父 PR 变化后立即更新子分支，避免长时间分离。

## 48. Phase 完成记录格式

每个 Phase 完成后，在对应章节末尾增加：

```text
Status: complete
Date:
Branch:
Commit:

Primary success call chain:

Primary failure call chain:

Files added:

Files removed:

Tests run:

Performance and allocation evidence:

Open work:
```

不得只写“完成”。记录必须说明主要调用路径和实际运行的测试。
