# GPU DAG 编辑管线重构 Phase 计划

Date: 2026-08-22

Status: G1–G7 implementation landed；G7 产品验收撤回；G7R.1–G7R.3 complete；G7R.H complete（含 one-shot 缓存隔离修复与 canonical LLF ROI 采样）；G7R.4–G7R.5 remaining；G7R 仍阻塞 G8。

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
| GPU 缓存 | workspace 内的内容感知结果缓存和 texture LRU；分配复用不等于结果命中 |
| 持久蒙版 | app 之下的 MaskStore 模块 |
| 主存蒙版缓存 | MaskStore 可以按字节预算保存 R8 副本 |
| GPU 蒙版缓存 | workspace 按 GPU 字节预算管理 |
| Rasterized Mask | R8 UNORM；任意一条边不大于 4096 |
| Feather | GPU exact signed Euclidean distance field；持久数据仍为 R8 |
| RAW 输入 | LibRaw open、unpack、active area 和降采样在管线外 |
| Develop 逻辑输出 | `develop.image`，AP1 primaries / ACEScc encoded；CameraColorPass 完成后才可写入 |
| Develop 内部缓存 | `develop.sensor_linear` 保存传感器开发结果；不是用户可见端口 |
| Geometry 后缓存 | `geometry.scene_source` 保存重采样后的 camera scene-linear RGB；不是用户可见节点 |
| 中间调色 | AP1 primaries / ACEScc encoded 工作空间 |
| 局部色温 | 基于 CAT02，以 AP1 白点为参考 |
| DRT | ACES 2.0 或 OpenDRT |
| Geometry | 管线文档的全局 ImageGeometryModel，不是第四个用户节点 |
| ROI | RenderGeometryResolver 统一计算 |
| 动态分辨率 | RenderRequest 输入；不保存进持久参数 |
| 图像重采样 | GraphCompiler 在传感器开发缓存和 CameraColorPass 之间插入一个内部 GeometryResamplePass |
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

Develop 是用户可见的逻辑端点，但编译后必须保留以下内部值边界：

```text
PreparedRawInput
  -> SensorDevelopPass
  -> develop.sensor_linear       # camera scene-linear；可跨 CCT、调色和 DRT 编辑复用
  -> GeometryResamplePass
  -> geometry.scene_source       # geometry 后 camera scene-linear；可跨 CCT、调色和 DRT 编辑复用
  -> CameraColorPass
       - 解析 as-shot 或 custom CCT/tint
       - 按标定光源在 CameraMatrices 的双光源矩阵间插值
       - camera RGB -> XYZ D50 -> XYZ D60 -> ACES AP1
       - AP1 scene-linear -> ACEScc encode
  -> develop.image               # AP1/ACEScc；Develop 的唯一用户可见输出
```

G4 只生成 camera scene-linear，G5 再执行 CameraColorPass 的拆分是正确的缓存边界，
不得把两者重新合并。`develop.sensor_linear`、`geometry.scene_source` 和 `develop.image`
必须使用不同 `GraphValueId` 和不同内容 key。CCT 或 tint 改变时只让 CameraColorPass 及其
下游失效，不得重新 open/unpack RAW，不得重新上传 CFA，不得重新执行线性化、解拜尔、
高光恢复、镜头校正或 GeometryResamplePass。

CameraColorPass 的矩阵来源必须与旧管线 `ColorTempOp::ResolveRuntime` 一致：

- 优先使用 RAW/DNG metadata 或项目 CameraMatrices 数据库中的 `ColorMatrix1/2`；
- 使用 `CalibrationIlluminant1/2` 对应的 CCT，在 mired 空间对双光源矩阵插值；
- 有 `ForwardMatrix1/2` 时，同样插值 forward matrix，并按 DNG reference neutral 构造
  camera RGB 到 XYZ D50；
- 没有 forward matrix 时，反转插值后的 XYZ 到 camera matrix，再从所选白点 Bradford
  适配到 D50；
- 最后执行 D50 到 D60 的 Bradford 适配和 XYZ D60 到 AP1；
- `cam_mul`、`pre_mul` 只用于传感器白平衡、as-shot neutral 推导和 RAW 归一化，不能用作
  CameraColorPass 的对角 Camera→AP1 矩阵；
- LibRaw `rgb_cam`/`cam_xyz` 可以保留在 metadata snapshot 中用于诊断，但 G7R 的
  CameraColorPass 不使用它们替代 CameraMatrices/DNG profile；
- 需要 CameraColorPass 但找不到可用矩阵时返回明确错误，禁止静默使用 identity；直接 RGB
  输入只有在输入描述明确声明其源色域时，才允许通过已知色域矩阵进入 AP1。

Develop 输出固定为：

```text
float RGB
ACES AP1 primaries
ACEScc encoded
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

DRT 接收 AP1 primaries / ACEScc encoded 工作空间图像，在端点内部解码为 AP1
scene-linear 后执行所选 DRT，并输出 display-referred GPU texture。

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
| 输入 | camera/CFA 数据 | AP1 primaries / ACEScc encoded RGB |
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

GraphDirtyTracker 需要同时追踪用户节点和 Develop 内部 pass 值。`ResourceId` 相同只说明
GPU 分配被复用，不能说明图像结果仍有效。结果缓存项至少包含：

```cpp
struct CachedImageResult {
  GraphValueId value_id;
  ContentKey content_key;
  ImageExtent extent;
  TextureFormat format;
  ResourceLease texture;
  SubmissionId last_writer;
};
```

只有 `value_id`、`content_key`、尺寸、格式都匹配，并且 `last_writer` 已完成时才是结果命中。
仅在相同 `GraphValueId` 下找到同尺寸纹理属于 allocation reuse，仍必须执行 pass，不能记为
cache hit。

默认 CUDA 图的内部 dirty 传播必须是：

```text
RAW 文件或输入准备参数改变:
  PreparedRawInput -> SensorDevelop -> Geometry -> CameraColor -> Grade -> DRT

线性化、解拜尔、高光恢复、AI 降噪或镜头参数改变:
  SensorDevelop -> Geometry -> CameraColor -> Grade -> DRT

crop、rotation、viewport ROI、输出尺寸或 DecodeRes 几何映射改变:
  Geometry -> CameraColor -> Grade -> DRT

RAW as-shot/custom CCT、tint 或 CameraMatrices 内容改变:
  CameraColor -> Grade -> DRT

Primary Grade 参数或其 mask 改变:
  affected Grade -> downstream Grade -> DRT

DRT 参数改变:
  DRT only
```

MaskNode dirty 时，只标记读取该 mask 的 ColorGradeNode 及其下游。取消或失败的 submission
不能发布任何新 cache key；它在开始前已经命中的上游结果仍保持可用。

### 27.1 缓存身份

缓存使用实际输入值或内容 hash，不使用递增数字，也不使用纹理地址或 `ResourceId` 代替内容
身份。

| 资源 | 是否包含 viewport ROI | key 来源 |
| --- | ---: | --- |
| PreparedRawInput | 否 | 原始字节内容、输入种类、DecodeRes、LibRaw 输入准备版本 |
| `develop.sensor_linear` | 否 | PreparedRawInput key、线性化、解拜尔、高光恢复、AI 降噪、镜头参数和实现版本 |
| `geometry.scene_source` | 是 | sensor-linear key 和完整 ResolvedRenderGeometry |
| `develop.image` | 间接包含 | geometry key、WB mode、resolved CCT/tint、CameraMatrices 内容和颜色算法版本 |
| Primary Grade 输出 | 间接包含 | develop AP1 key、调整顺序、每个调整参数、mask sampling key 和 mix |
| DRT 输出 | 间接包含 | grade key、DRT method、显示色域、EOTF 和 DRT 参数 |
| 磁盘 R8 数据 | 否 | R8 descriptor 和 pixels |
| 主存 R8 数据 | 否 | MaskAssetKey |
| GPU base R8 texture | 否 | MaskAssetKey |
| feather texture | 否 | MaskAssetKey 和 feather 参数 |
| mask mip levels | 否 | MaskAssetKey |
| mask sampling plan | 是 | descriptor 和 ResolvedRenderGeometry |
| LLF reference | 否 | input content 和 LLF 参数 |
| LLF 当前输出 | 是 | reference resource 和当前 geometry |

`develop.image` 的 CameraMatrices 内容必须包含最终选中的 `ColorMatrix1/2`、
`ForwardMatrix1/2`、标定光源 CCT、as-shot neutral、矩阵来源和版本。`cam_mul` 与 `pre_mul`
可以进入 sensor-linear/as-shot neutral 的 key，但不能作为 Camera→AP1 的矩阵身份。

同一组实际输入产生相同 key。视图回到原位置时可以再次使用相同数据。

### 27.2 必须存在的缓存层级

每个打开的图像/版本至少持有以下可淘汰结果：

```text
PreparedSourceCache[source_key]
SensorDevelopCache[sensor_develop_key]        -> develop.sensor_linear
GeometryResultCache[geometry_key]             -> geometry.scene_source
DevelopAp1Cache[develop_ap1_key]              -> develop.image
NodeResultCache[grade_or_drt_key]              -> node output
```

这些缓存共享 workspace 的 GPU 字节预算和 submission 安全淘汰规则。PreparedSourceCache 使用
主存字节预算。切换到另一张图再切回时，只要条目未被淘汰且内容 key 匹配，就可以复用已准备
RAW 和相应 GPU 结果。任何缓存都不得依赖“这次仍使用同一纹理对象”来判定有效性。

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

### 28.1 旧数据处理

新产品路径完全废弃 legacy 参数和旧 stage JSON：

```text
format version 2 PipelineDocument -> load
旧 stage JSON                     -> explicit unsupported-format error
```

- 不在加载、编辑或每帧渲染时调用 `LegacyPipelineImporter`；
- 不把 legacy operator 参数镜像到新 Model；
- 不在新 JSON 中保存 nested legacy adapter；
- 不用新默认值静默替换旧参数；旧格式必须返回清楚的版本错误；
- `LegacyPipelineImporter`、legacy snapshot 和 CUDA 产品 legacy stage adapter 在 G7R 删除，
  不延后到 OpenCL/Metal 移植；
- UI 和 app service 直接读取、修改 `PipelineDocument` 中的 Model。

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
[x] Model and graph foundation
[x] CUDA workspace
[x] Render geometry
[x] CUDA Develop
[x] CUDA Color Grade
[x] CUDA Mask and Mix
[x] CUDA DRT and product path
[ ] CUDA default pipeline recovery    <- current
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
| G7R | `feature/gpu-dag-cuda-default-recovery` | G7 | CameraMatrices 色彩、内容缓存和默认管线性能恢复 |
| G8 | `feature/gpu-dag-opencl` | G7R | OpenCL 完整移植 |
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

##### Phase G1 completion record (2026-08-22)

**Status:** complete — GPU-free Model/DTO/PipelineGraph, default three-node document, format v2 JSON, and one-way legacy importer. Product execution still uses PipelineStage.

**Primary success call chain:**

```text
CreateDefaultPipelineDocument
  -> DevelopNodeModel + ColorGradeNodeModel::MakeDefault + DrtNodeModel
  -> PipelineGraph::AddNode / Connect
  -> Validate + TopologicalOrder
  -> PipelineDocument::ToJson  (format_version 2)
```

**Parameter dirty call chain:**

```text
ExposureModel::SetValue
  -> dirty field bit
  -> TakeDirtyPatch
  -> OperatorParamPatchDto (latest value; bits cleared)
```

**Primary failure call chain:**

```text
Connect(display or mask output, scene image input) or cyclic edge
  -> PipelineGraph::Validate
  -> PortTypeMismatch or Cycle
  -> TopologicalOrder throws; document graph left as constructed

Unknown legacy OperatorType
  -> LegacyPipelineImporter::Import
  -> error string, no PipelineDocument
```

**Cancelled parameter transfer:**

```text
TakePendingParameterPatch
  -> destructor without Commit
  -> RestoreDirty
  -> field remains dirty for retry
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DefaultPipelineHasDevelopGradeAndDrtNodes` | `GpuDagModelGraphTest` | PASS |
| `DefaultPipelineConnectsDevelopThroughPrimaryGradeToDrt` | `GpuDagModelGraphTest` | PASS |
| `DefaultPrimaryGradeContainsOrderedSceneAdjustments` | `GpuDagModelGraphTest` | PASS |
| `DefaultPrimaryGradeUsesFullMixAndNoMask` | `GpuDagModelGraphTest` | PASS |
| `PipelineGraphRejectsCycle` | `GpuDagModelGraphTest` | PASS |
| `PipelineGraphRejectsDisplayImageConnectedToSceneInput` | `GpuDagModelGraphTest` | PASS |
| `PipelineGraphRejectsMaskConnectedToImageInput` | `GpuDagModelGraphTest` | PASS |
| `RepeatedExposureWritesCollapseIntoOneDirtyPatch` | `GpuDagModelGraphTest` | PASS |
| `DirtyPatchTakenBeforeNewEditLeavesNewEditDirty` | `GpuDagModelGraphTest` | PASS |
| `CancelledParameterTransferRestoresDirtyFields` | `GpuDagModelGraphTest` | PASS |
| `PipelineDocumentRoundTripPreservesNodeIdsEdgesAndAdjustmentOrder` | `GpuDagModelGraphTest` | PASS |
| `LegacyStageJsonMapsRawGradeGeometryAndDrtToNewDocument` | `GpuDagModelGraphTest` | PASS |
| Header hygiene (no GPU / ImageBuffer includes) | `GpuDagModelGraphTest` | PASS |
| Unknown legacy operator fails import | `GpuDagModelGraphTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagModelGraphTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R GpuDagModelGraphTest
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target Operators EditPipeline --parallel 4
```

Suite totals: `17/17` GpuDagModelGraphTest PASS. Operators and EditPipeline still build.

**Checklist / exit condition:** all G1 exit conditions met. Product `PipelineMgmtService` is unchanged.

**LOC note (grill-code-review):** largest new file is `legacy_pipeline_importer.cpp` (~305 lines). No changed file exceeds 1000 LOC. `EditGraph` links JSON only.

**Remaining gaps:** GraphCompiler, ExecutionPlan, CUDA workspace (G2); product path still reads old stage JSON (G7).

**Files added:** `alcedo_studio/src/include/edit/graph/*`, `alcedo_studio/src/include/edit/operators/models/*`, matching `.cpp` under `edit/graph` and `edit/operators/models`, `tests/edit/graph/*`, CMake `EditGraph` + `GpuDagModelGraphTest`.

**Files removed:** none.

**Performance and allocation evidence:** not applicable; G1 has no GPU path.

**Open work:** G2 CUDA workspace.

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

##### Phase G2 completion record (2026-08-22)

**Status:** complete — templated CUDA render workspace, grow-only ParameterArena with dirty-range H2D, TransientBufferArena lifted from `cuda::nn::WorkspacePool`, texture LRU with leases, node-result KV cache. No grading kernels. No ExecutionPlan.

**Primary success call chain:**

```text
CudaRenderDevice::BeginRender
  -> CudaBackend::Wait (previous submission)
  -> TransientBufferArena::Reset
  -> TakePendingParameterPatch
  -> ParameterArena host-mirror field copy
  -> MergeAdjacentRanges
  -> CudaBackend::UploadBufferRange
  -> PendingParameterPatch::Commit
  -> EndRender (cudaEventRecord)
```

**Primary failure call chain:**

```text
CudaBackend::FailNextUpload / UploadBufferRange throws
  -> PendingParameterPatch destructor
  -> RestoreDirty
  -> no Commit; next TakePending retries the latest payload
```

**Transient grow failure:**

```text
Allocate while used_bytes() != 0 and capacity short
  -> TransientBufferArena throws
  -> existing slab and live pointers unchanged
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `ParameterArenaKeepsStableOffsetsAcrossRenders` | `GpuDagCudaWorkspaceTest` | PASS |
| `ParameterArenaUploadsOnlyDirtyFieldRanges` | `GpuDagCudaWorkspaceTest` | PASS |
| `AdjacentDirtyParameterRangesMergeBeforeCudaCopy` | `GpuDagCudaWorkspaceTest` | PASS |
| `RepeatedDirtyWritesUseLatestValue` | `GpuDagCudaWorkspaceTest` | PASS |
| `CancelledCudaParameterCopyRestoresDirtyFields` | `GpuDagCudaWorkspaceTest` | PASS |
| `WorkspaceResetRewindsTransientMemoryWithoutFreeingCudaAllocation` | `GpuDagCudaWorkspaceTest` | PASS |
| `WorkspaceCannotGrowWhileTransientPointersAreLive` | `GpuDagCudaWorkspaceTest` | PASS |
| `NodeResultCacheReturnsValueByProducerNodeAndPort` | `GpuDagCudaWorkspaceTest` | PASS |
| `TextureLeasePreventsEvictionUntilCudaSubmissionCompletes` | `GpuDagCudaWorkspaceTest` | PASS |
| `SecondRenderUsesNoCudaAllocationAfterPeakReserve` | `GpuDagCudaWorkspaceTest` | PASS |
| `UnchangedParametersIssueNoHostToDeviceCopy` | `GpuDagCudaWorkspaceTest` | PASS |
| Header hygiene (`include/gpu`, `include/edit/runtime` non-cuda) | `GpuDagCudaWorkspaceTest` | PASS |
| G1 `GpuDagModelGraphTest` regression | `GpuDagModelGraphTest` | PASS |
| Lifted `WorkspacePool` (`MlOpsWorkspaceTest`) | `MlOpsTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagCudaWorkspaceTest GpuDagModelGraphTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target MlOpsTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaWorkspaceTest|GpuDagModelGraphTest"
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaWorkspaceTest|MlOpsTest.MlOpsWorkspaceTest"
```

Suite totals: `12/12` GpuDagCudaWorkspaceTest PASS. `17/17` GpuDagModelGraphTest PASS. `12/12` MlOpsWorkspaceTest PASS.

**Checklist / exit condition:** all G2 exit conditions met. Product `PipelineMgmtService` and old `GPUPipelineWrapper` are unchanged. Workspace is owned by `CudaRenderDevice`, not by a plan.

**LOC note (grill-code-review):** largest new file is `texture_pool.hpp` (291 lines). `cuda_backend.cpp` is 251 lines. No changed file exceeds 1000 LOC.

**Remaining gaps:** GraphCompiler / ExecutionPlan / `IRenderDevice::Execute` (G4+). RenderGeometryResolver (G3). Develop/grade/DRT kernels (G4–G7). MaskStore and feather (G6). OpenCL `WorkspacePool` still a separate copy until G8. DemosaicNet still uses its own `WorkspacePool` instance, not `CudaRenderWorkspace`.

**Files added:** `alcedo_studio/src/include/gpu/transient_buffer_arena.hpp`, `transient_buffer_scope.hpp`, `include/cuda/cuda_check.hpp`, `cuda_slab_backend.hpp`, `include/edit/runtime/*`, `src/edit/runtime/*`, `tests/edit/runtime/*`. CMake `EditRuntime` + `EditRuntimeCuda` + `GpuDagCudaWorkspaceTest`.

**Files removed:** none. `cuda/nn/workspace.hpp` is now a facade over `TransientBufferArena<CudaSlabBackend>`.

**Performance and allocation evidence:** `SecondRenderUsesNoCudaAllocationAfterPeakReserve` asserts malloc/free counts stay 0 after peak reserve. `UnchangedParametersIssueNoHostToDeviceCopy` asserts H2D bytes 0. `ParameterArenaUploadsOnlyDirtyFieldRanges` asserts a 4-byte copy. `AdjacentDirtyParameterRangesMergeBeforeCudaCopy` asserts one 8-byte copy.

**Open work:** G3 RenderGeometryResolver.

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

##### Phase G3 completion record (2026-08-22)

**Status:** complete — GPU-free RenderGeometryResolver (pixel-center matrices, one rounding owner), TextureSamplingPlan for mask/LLF, CUDA GeometryResamplePass as one kernel. Not a user node. No GraphCompiler.

**Primary success call chain:**

```text
SourceGeometry + ImageGeometryParams + ViewRequest + ResolutionRequest + SamplingFootprint
  -> ResolveRenderGeometry
  -> ResolvedRenderGeometry (matrices, extents, required RectI, GpuRenderGeometry)
  -> MakeRasterMaskSamplingPlan / MakeLlfSamplingPlan
     or GeometryResamplePass::Encode
  -> one CUDA kernel, RGBA32F at render_extent
```

**Primary failure call chain:**

```text
zero or non-finite extent, or render_scale <= 0
  -> ResolveRenderGeometry throws
  -> no matrices, no kernel
```

```text
required AABB after footprint expansion
  -> clamp to decoded / reference RectI
  -> still inside source bounds
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RenderGeometryRoundTripsReferenceAndRenderPixelCenters` | `GpuDagGeometryTest` | PASS |
| `FullCropZeroRotationMapsReferenceCornersToRenderCorners` | `GpuDagGeometryTest` | PASS |
| `RotatedCropBoundsContainAllFourTransformedCorners` | `GpuDagGeometryTest` | PASS |
| `ViewportCropAndDynamicScaleProduceRequestedRenderExtent` | `GpuDagGeometryTest` | PASS |
| `DecodeScaleDoesNotChangeNormalizedReferenceCoordinates` | `GpuDagGeometryTest` | PASS |
| `RequiredInputRegionExpandsForBicubicFootprintAndClampsToDecodedBounds` | `GpuDagGeometryTest` | PASS |
| `RasterMaskSamplingMapsSameReferencePointAtQuarterAndFullPreview` | `GpuDagGeometryTest` | PASS |
| `OddImageDimensionsUseOneRoundingResultAcrossImageMaskAndLlf` | `GpuDagGeometryTest` | PASS |
| `CropRotateViewportAndScaleExecuteAsOneCudaResample` | `GpuDagCudaGeometryTest` | PASS |
| `ResolveRenderGeometryRejectsZeroExtent` | `GpuDagGeometryTest` | PASS |
| Default document still three nodes; JSON has no viewport/scale | `GpuDagGeometryTest` | PASS |
| Operator DTO headers contain no `roi_` | `GpuDagGeometryTest` | PASS |
| Geometry headers have no GPU / ImageBuffer includes | `GpuDagGeometryTest` | PASS |
| G1 `GpuDagModelGraphTest` regression | `GpuDagModelGraphTest` | PASS |
| G2 `GpuDagCudaWorkspaceTest` regression | `GpuDagCudaWorkspaceTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagGeometryTest GpuDagCudaGeometryTest GpuDagModelGraphTest GpuDagCudaWorkspaceTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagGeometryTest|GpuDagCudaGeometryTest|GpuDagModelGraphTest|GpuDagCudaWorkspaceTest"
```

Suite totals: `12/12` GpuDagGeometryTest PASS. `1/1` GpuDagCudaGeometryTest PASS. `17/17` GpuDagModelGraphTest PASS. `12/12` GpuDagCudaWorkspaceTest PASS. Combined `42/42` PASS.

**Checklist / exit condition:** all G3 exit conditions met. GeometryResamplePass is a CUDA encode unit, not an `INodeModel`. Product `PipelineMgmtService` and old `CropRotateOp` are unchanged.

**LOC note (grill-code-review):** largest new file is `render_geometry_resolver.cpp` (242 lines). `geometry_resample.cu` is 137 lines. No changed file exceeds 1000 LOC.

**Remaining gaps:** GraphCompiler / ExecutionPlan insert the pass (G4+). Develop/grade/DRT kernels (G4–G7). MaskStore GPU textures and feather (G6). OpenCL and Metal GeometryResamplePass (G8/G9).

**Files added:** `alcedo_studio/src/include/edit/geometry/*`, `alcedo_studio/src/edit/geometry/*`, `include/edit/runtime/cuda/geometry_resample_pass.hpp`, `src/edit/runtime/cuda/geometry_resample.cu`, `tests/edit/geometry/*`. CMake `EditGeometry` + `GpuDagGeometryTest` + `GpuDagCudaGeometryTest`.

**Files removed:** none. `NormalizedRect` moved from `image_geometry_model.hpp` into `edit/geometry/types.hpp`.

**Performance and allocation evidence:** `CropRotateViewportAndScaleExecuteAsOneCudaResample` asserts `LaunchCount() == 1` for the combined crop/rotate/view/scale kernel, GPU vs CPU pixel-center bilinear max error `< 1e-4`, and second encode malloc/free counts stay 0 after peak reserve.

**Open work:** G4 CUDA Develop Endpoint.

## 37. Phase G4 — CUDA Develop Endpoint

Branch: `feature/gpu-dag-cuda-develop`

Base: `feature/gpu-dag-render-geometry`

目标：

- 把 LibRaw unpack 和 RAW 降采样移到管线输入之前；
- 实现 CUDA Develop Endpoint 的传感器开发子阶段；
- 输出可缓存的 camera scene-linear GPU 图像；G5 的 CameraColorPass 再完成 Camera→AP1，
  并把 Develop 的图输出编码为 AP1 primaries / ACEScc 工作空间。

工作：

- 增加 RawInputLoader；
- 增加 PreparedRawInput；
- 记录 active area、DecodeRes、CFA pattern 和 phase；
- 保留 CPU LibRaw unpack；
- 保留 CPU RAW 降采样；
- 从旧 RawDecodeOp 移出 GPU Develop 行为；
- 把高光恢复、解拜尔、AI 降噪、RAW white balance 和镜头校正编译为 SensorDevelop Pass；
- camera-to-AP1 延后到 G5，使 CCT/tint 修改复用本阶段输出；
- 使用 workspace 管理所有 Develop 临时内存；
- 建立直接 RGB 输入；
- 建立内部 `develop.sensor_linear` GraphValue。

测试：

```text
RawInputLoaderUnpacksBeforePipelineBuild
RawInputLoaderDownsampleUpdatesCfaPatternAndPhase
PreparedRawInputKeepsFullReferenceExtentAcrossDecodeRes
CudaDevelopProducesFiniteCameraSceneLinearRgbFromBayerInput
CudaDevelopProducesFiniteCameraSceneLinearRgbFromXTransInput
CudaDevelopUsesWorkspaceForAllTemporaryBuffers
CudaDevelopSecondRenderCreatesNoGpuAllocation
DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint
```

完成条件：

- ExecutionPlan 中没有 LibRaw Pass；
- GPU 不执行 RAW DecodeRes 降采样；
- Develop 不调用 CPU 图像算子；
- `develop.sensor_linear` 的格式和 camera scene-linear 色彩空间明确。

##### Phase G4 completion record (2026-08-22)

**Status:** complete — CUDA Develop 的传感器开发部分位于
`feature/gpu-dag-cuda-develop`。输出是 **camera scene-linear RGBA32F**，Camera-to-AP1
延后到 G5 的独立、不可挂 mask 的内部 CameraColorPass，使 CCT 变化不让传感器开发结果失效，
也不重新执行解拜尔。这一拆分是保留项；完整 Develop 逻辑端点仍必须在 CameraColorPass 后输出
AP1。

**Highlight recovery order:** CUDA full-frame (`ProcessCudaFullFrame`) is Linearize → (optional CFA Clamp01 when HLR is off) → Demosaic → HighlightRecover on RGB. The CPU path (`ApplyLinearization` → `ApplyHighlightReconstruct` → `ApplyDebayer`) is not the G4 reference. `GraphCompiler` emits `Demosaic` before `HighlightRecover`. Encoder matches CUDA: Bayer + HLR uses planar RCD then `ApplyHighlightCorrectionAndPackRGBAOriented`; Bayer without HLR clamps CFA then packs with inverse cam_mul; non-neural X-Trans always clamps CFA then `XTransToRGB_Ref`.

**Primary success call chain:**

```text
RawInputLoader::FromUnpackedCfa | LoadEncoded | FromDirectRgb
  -> PreparedRawInput (CPU unpack + DecodeRes downsample, LibRaw recycled)
  -> GraphCompiler::Compile (Develop passes + GeometryResample; no LibRaw / DecodeRes / CameraToAp1)
  -> CudaRenderDevice.BeginRender
  -> ExecuteCudaDevelop (workspace transients + Images() RGBA32F)
  -> Linearize -> Demosaic -> HighlightRecover (RGB) | InverseCamMulPack
  -> current GraphImageCache[develop/image] camera scene-linear RGBA32F
     G7R migration: GraphImageCache[develop/sensor_linear]
  -> EndRender / WaitIdle
```

**Primary failure call chain:**

```text
CudaBackend::FailNextUpload
  -> ExecuteCudaDevelop throws
  -> PendingParameterPatch destructor RestoreDirty
  -> Develop params remain dirty; no CPU Apply fallback
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RawInputLoaderUnpacksBeforePipelineBuild` | `GpuDagRawInputTest` | PASS |
| `RawInputLoaderDownsampleUpdatesCfaPatternAndPhase` | `GpuDagRawInputTest` | PASS |
| `PreparedRawInputKeepsFullReferenceExtentAcrossDecodeRes` | `GpuDagRawInputTest` | PASS |
| `DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint` | `GpuDagRawInputTest` + `GpuDagCudaDevelopTest` | PASS |
| `GraphCompilerEmitsNoLibRawOrDecodeResPass` (also no CameraToAp1) | `GpuDagRawInputTest` | PASS |
| `GraphCompilerPlacesHighlightRecoverAfterDemosaic` | `GpuDagRawInputTest` | PASS |
| `CudaDevelopProducesFiniteCameraSceneLinearRgbFromBayerInput` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopProducesFiniteCameraSceneLinearRgbFromXTransInput` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopUsesWorkspaceForAllTemporaryBuffers` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopSecondRenderCreatesNoGpuAllocation` | `GpuDagCudaDevelopTest` | PASS |
| Upload failure restores dirty; no CPU Apply | `GpuDagCudaDevelopTest` | PASS |
| G1 `GpuDagModelGraphTest` regression | `GpuDagModelGraphTest` | PASS |
| G2 `GpuDagCudaWorkspaceTest` regression | `GpuDagCudaWorkspaceTest` | PASS |
| G3 `GpuDagGeometryTest` / `GpuDagCudaGeometryTest` regression | those binaries | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagRawInputTest GpuDagCudaDevelopTest GpuDagModelGraphTest GpuDagCudaWorkspaceTest GpuDagGeometryTest GpuDagCudaGeometryTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagRawInputTest|GpuDagCudaDevelopTest|GpuDagModelGraphTest|GpuDagCudaWorkspaceTest|GpuDagGeometryTest|GpuDagCudaGeometryTest"
```

Suite totals: `9/9` GpuDagRawInputTest PASS. `6/6` GpuDagCudaDevelopTest PASS. `17/17` GpuDagModelGraphTest PASS. `12/12` GpuDagCudaWorkspaceTest PASS. `12/12` GpuDagGeometryTest PASS. `1/1` GpuDagCudaGeometryTest PASS. Combined `57/57` PASS.

**Checklist / exit condition:** ExecutionPlan has no LibRaw or DecodeRes pass. GPU does not downsample RAW. Develop does not call CPU image operators. Output format is RGBA32F camera scene-linear (`SceneWorkingSpace::CameraRgb`). Product `PipelineMgmtService` and old `RawDecodeOp` / `RawProcessor` are unchanged.

**LOC note (grill-code-review):** `raw_input_loader.cpp` 396 lines. `cuda_develop_pass.cpp` 197 lines. `graph_compiler.cpp` 102 lines. No changed file exceeds 1000 LOC.

**Remaining gaps:** Camera-to-AP1 and CAT02 stay in G5。CameraColor 是 Develop 内部 pass，
不是第四个用户节点；用户图仍为 Develop → ColorGrade → DRT。NeuralEngine demosaic is not
executed in G4 (Legacy RCD / `XTransToRGB_Ref` only). Lensfun is a no-op unless DNG warp is
present; G4 tests do not cover DNG warp. Full-frame only (no 9000px tiled path). DemosaicNet
arena is not unified with workspace.

**Files added:** `include/edit/input/*`, `edit/input/raw_input_loader.cpp`, `include/edit/runtime/{pass_kind,execution_plan,graph_compiler,render_context,develop_compile_source,graph_image_cache}.hpp`, `edit/runtime/graph_compiler.cpp`, `include/edit/runtime/cuda/cuda_develop_pass.hpp`, `edit/runtime/cuda/cuda_develop_pass.cpp`, `include/decoders/processor/raw_linearization_params.hpp`, G4 tests under `tests/edit/input` and `tests/edit/runtime`.

**Open work:** G5 CUDA Primary Color Grade plus CameraColor node.

## 38. Phase G5 — CUDA Primary Color Grade

Branch: `feature/gpu-dag-cuda-grade`

Base: `feature/gpu-dag-cuda-develop`

目标：

- 实现默认 ColorGradeNode 的 CUDA 执行；
- 从可缓存的 G4 camera scene-linear 结果出发，在 Grade 前执行独立 CameraColorPass，并让
  `develop.image` 成为 AP1 primaries / ACEScc encoded 工作空间图像；
- 增加纯 Model，并让新 CUDA 路径只使用这些 Model；
- 旧 operator 类只供尚未移植的 OpenCL 和 Metal 路径临时使用；
- 把 LLF 内存和缓存迁入 workspace。

工作：

- 缓存 `develop.sensor_linear` 和 `geometry.scene_source`，CCT/tint 改变时只重跑
  CameraColorPass 及其下游；
- 按 CameraMatrices/DNG 双光源矩阵插值解析 Camera→AP1；
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

##### Phase G5 completion record (2026-08-22)

**Status:** complete — CUDA Primary Color Grade 使用纯 Model/DTO、序列化调整顺序、
Camera-to-AP1、CAT02、融合 point adjustment 和 workspace 局部色调资源。

**Primary success call chain:**

```text
PipelineDocument + PreparedRawInput + RenderRequest
  -> GraphCompiler::Compile
  -> ExecutionPlan(CameraToAp1, PrimaryColorGrade, serialized adjustment ids)
  -> CudaRenderDevice::BeginRender
  -> ExecuteCudaDevelop
  -> ExecuteCudaPrimaryGrade
  -> Model DTO / dirty Patch -> CUDA runtime POD -> ParameterArena dirty ranges
  -> fused PrimaryGradeKernel in Model order
  -> workspace GraphImageCache grade.primary:image
  -> CudaRenderDevice::EndRender
```

**Primary failure call chain:**

```text
parameter upload failure
  -> ExecuteCudaPrimaryGrade throws before patch commit
  -> PendingParameterPatch restores Model dirty bits
  -> caller receives GPU runtime failure; no legacy Apply or CPU pixel fallback

missing graph adjustment / missing Develop image / CUDA kernel launch failure
  -> ExecuteCudaPrimaryGrade throws
  -> caller receives GPU runtime failure; no legacy Apply or CPU pixel fallback
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CudaPrimaryGradeDefaultParametersPreserveDevelopOutput` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaExposurePatchChangesOnlyExposureParameterRange` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaCat02WhiteBalanceZeroOffsetPreservesAp1White` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaCat02WhiteBalanceMaskedSampleMatchesFullAdjustmentAtMaskOne` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaPointAdjustmentsExecuteInSerializedModelOrder` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaLocalToneReferenceReusesAcrossViewportChanges` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaLocalToneUsesWorkspaceInsteadOfPrivateAllocation` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaColorGradeSecondRenderCreatesNoGpuAllocation` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `MovingAdjustmentChangesExecutionOrderWithoutChangingOtherParameters` | `GpuDagCudaPrimaryGradeTest` | PASS |
| G1–G4 graph, workspace, geometry, input and Develop regression | six existing GPU DAG targets | PASS |
| CUDA default-grade memory access | `compute-sanitizer --tool memcheck` | PASS, 0 errors |
| Legacy operator compatibility | `Operators` | BUILD PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target Operators GpuDagCudaPrimaryGradeTest GpuDagRawInputTest GpuDagCudaDevelopTest GpuDagModelGraphTest GpuDagCudaWorkspaceTest GpuDagGeometryTest GpuDagCudaGeometryTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaPrimaryGradeTest|GpuDagRawInputTest|GpuDagCudaDevelopTest|GpuDagModelGraphTest|GpuDagCudaWorkspaceTest|GpuDagGeometryTest|GpuDagCudaGeometryTest"
compute-sanitizer --tool memcheck --print-limit 5 build\debug\alcedo_studio\tests\edit\GpuDagCudaPrimaryGradeTest_runtime\GpuDagCudaPrimaryGradeTest.exe --gtest_filter=CudaPrimaryGradeFixture.CudaPrimaryGradeDefaultParametersPreserveDevelopOutput
```

Suite totals: G5 `9/9` PASS; combined G1–G5 `66/66` PASS; memcheck `0` errors.

**Checklist / exit condition:** all G5 checks complete. Slider edits update stable ParameterArena
ranges without DAG recompilation. Adjustment moves change the compiled command order. The new CUDA
runtime does not call legacy operator `Apply`, `ApplyGPU`, or `SetGlobalParams`. Local-tone reference
and execution buffers live in `BasicRenderWorkspace`; the second render creates no CUDA allocation.
The legacy `Operators` target continues to build for the not-yet-migrated product/OpenCL/Metal path.

**LOC note (grill-code-review):** `cuda_primary_grade_pass.cu` 414 lines;
`cuda_primary_grade_test.cpp` 206 lines; runtime registry implementation/header 41/42 lines;
public pass header 32 lines; `graph_compiler.cpp` 126 lines. No changed file exceeds 1000 lines.

**Residual gaps:** Mask texture sampling and per-pixel Normal Mix are G6 scope. DRT and product
pipeline routing are G7 scope. OpenCL and Metal still use their legacy execution paths until G8/G9.

**G7R audit note:** G5 的缓存边界要求正确，但当前实现没有完成该要求：Camera→AP1 被融合进
`PrimaryGradeKernel`，读取的是不完整 `RawRuntimeColorContext`，且 `develop.image` 仍指向
camera RGB。现有 CAT02 测试使用 direct RGB identity 路径，只证明零 offset/mix 行为，未证明
CameraMatrices 双光源插值或实际 CAT02。第 41 节负责按原意修复，不回退 G4/G5 的拆分。

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

##### Phase G6 completion record (2026-08-22)

**Status:** complete — 持久 R8 MaskStore、可配置 root、主存 LRU、workspace CUDA mask
texture/mip LRU、dirty rectangle upload、analytic/raster mask、精确 signed Euclidean distance
feather 和 Color Grade Normal Mix 已接入同一 GPU DAG。

**Primary success call chain:**

```text
PipelineDocument mask edge + serialized MaskAssetKey + RenderRequest
  -> GraphCompiler::Compile
  -> ExecutionPlan(MaskEvaluate, MaskFeather, PrimaryColorGrade)
  -> ExecuteCudaMask
  -> MaskStore::Load -> host R8 byte-budget LRU
  -> BasicRenderWorkspace::MaskTextures -> keyed R8 mip chain + submission lease
  -> full or unioned dirty-rectangle upload
  -> analytic evaluation or raster TextureSamplingPlan
  -> parallel-band exact signed Euclidean distance -> cached distance buffer -> feather sample
  -> RenderSpace R8 mask
  -> ExecuteCudaPrimaryGrade -> per-pixel Normal Mix
  -> AP1/ACEScc working-space output
```

**Primary failure call chain:**

```text
empty key / axis outside [1, 4096] / wrong R8 byte count
  -> ValidateMaskAsset rejects before temporary-file creation
  -> existing persistent mask remains unchanged

incomplete or invalid R8 file / missing MaskStore / invalid dirty rectangle / CUDA failure
  -> MaskStore or ExecuteCudaMask throws
  -> active-submission leases prevent texture eviction
  -> caller receives GPU runtime failure; no CPU image-processing fallback
```

**Files added:**

- `alcedo_studio/src/include/edit/mask/mask_asset.hpp`
- `alcedo_studio/src/include/edit/mask/mask_store.hpp`
- `alcedo_studio/src/edit/mask/mask_store.cpp`
- `alcedo_studio/src/include/edit/runtime/mask_texture_cache.hpp`
- `alcedo_studio/src/include/edit/runtime/cuda/cuda_mask_pass.hpp`
- `alcedo_studio/src/edit/runtime/cuda/cuda_mask_pass.cu`
- `alcedo_studio/tests/edit/mask/mask_store_test.cpp`
- `alcedo_studio/tests/edit/runtime/cuda_mask_test.cpp`

**Files removed:** none.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskStoreRejectsRasterMaskLargerThan4096OnEitherAxis` | `GpuDagMaskStoreTest` | PASS |
| `MaskStoreRoundTripPreservesR8PixelsDescriptorAndKey` | `GpuDagMaskStoreTest` | PASS |
| `MaskStoreUsesConfiguredRoot` | `GpuDagMaskStoreTest` | PASS |
| `MaskStoreReplacesFileOnlyAfterCompleteWrite` | `GpuDagMaskStoreTest` | PASS |
| `MaskHostCacheEvictsByBytesWithoutDeletingMaskFile` | `GpuDagMaskStoreTest` | PASS |
| `CudaMaskTextureCacheReusesTextureForSameMaskAssetKey` | `GpuDagCudaMaskTest` | PASS |
| `CudaMaskTextureCacheDoesNotEvictTextureUsedByActiveSubmission` | `GpuDagCudaMaskTest` | PASS |
| `CudaRasterMaskUploadsOnlyUnionedDirtyRectangle` | `GpuDagCudaMaskTest` | PASS |
| `CudaRadialMaskMatchesReferenceSpaceEllipseAtPreviewScales` | `GpuDagCudaMaskTest` | PASS |
| `CudaGraduatedNdMaskFollowsReferenceSpaceNormal` | `GpuDagCudaMaskTest` | PASS |
| `CudaFeatherPreservesZeroAndOnePlateaus` | `GpuDagCudaMaskTest` | PASS |
| `CudaSignedDistanceFeatherMatchesExactEuclideanReferenceWithinTolerance` | `GpuDagCudaMaskTest` | PASS |
| `CudaFeatherPreservesAntialiasedSourceBoundary` | `GpuDagCudaMaskTest` | PASS |
| `CudaFeatherRadiusIsStableAcrossDynamicRenderScales` | `GpuDagCudaMaskTest` | PASS |
| `ChangingFeatherRadiusReusesSignedDistanceTexture` | `GpuDagCudaMaskTest` | PASS |
| `CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne` | `GpuDagCudaMaskTest` | PASS |
| serialized `MaskAssetKey` read/write | `GpuDagModelGraphTest` | PASS |
| G1–G5 graph, geometry, input, Develop, grade, and workspace regression | nine GPU DAG targets | PASS |
| exact-distance and Normal Mix CUDA memory access | `compute-sanitizer --tool memcheck` | PASS, 0 errors |
| not-yet-migrated backend compatibility | `Operators` | BUILD PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagMaskStoreTest GpuDagCudaMaskTest GpuDagCudaPrimaryGradeTest GpuDagCudaWorkspaceTest Operators --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest|GpuDagCudaPrimaryGradeTest|GpuDagMaskStoreTest|GpuDagModelGraphTest|GpuDagRawInputTest|GpuDagCudaDevelopTest|GpuDagGeometryTest|GpuDagCudaGeometryTest"
compute-sanitizer --tool memcheck --print-limit 10 build\debug\alcedo_studio\tests\edit\GpuDagCudaMaskTest_runtime\GpuDagCudaMaskTest.exe --gtest_filter=CudaMaskFixture.CudaSignedDistanceFeatherMatchesExactEuclideanReferenceWithinTolerance:CudaMaskFixture.CudaColorGradeMixUsesInputAtMaskZeroAndAdjustedAtMaskOne
```

Suite totals: G6 required tests `16/16` PASS; combined affected GPU DAG tests `83/83` PASS;
memcheck `2/2` PASS with `0` errors.

**Checklist / exit condition:** all G6 checks complete. MaskStore contains no GPU type. Persistent
raster data is R8 and rejects either axis above 4096. Host and GPU caches enforce byte budgets;
the GPU cache exists only in BasicRenderWorkspace and retains active-submission resources. Raster
textures build keyed mip chains, dirty edits upload one union rectangle, and viewport/dynamic scale
changes reuse the persistent texture. Feather uses an exact signed Euclidean distance transform;
radius-only changes reuse its workspace buffer. Color Grade applies one per-pixel Normal Mix.

**Performance and allocation evidence:** dynamic-scale renders retained the same persistent mask
texture resource id. Feather-radius changes retained the same signed-distance resource id. The mip
chain and exact-distance work buffers are allocated on first use and retained in the workspace;
active submissions block LRU eviction.

**LOC note (grill-code-review):** `cuda_mask_pass.cu` 423 lines; `cuda_mask_test.cpp` 324 lines;
`mask_texture_cache.hpp` 223 lines; `mask_store.cpp` 179 lines. No changed file exceeds 1000 lines.

**Residual gaps:** CUDA DRT and product pipeline routing are G7 scope. OpenCL and Metal mask
runtimes remain on their existing paths until G8 and G9.

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

##### Phase G7 completion record (2026-08-22)

**Status:** complete — CUDA ACES 2.0/OpenDRT、显示色域/EOTF、DRT dirty Patch、完整
`CudaRenderDevice::Execute`、PipelineMgmtService v2 文档持有/保存，以及现有 scheduler 到
CUDA DAG 和 frame sink 的产品路径均已接入。OpenCL/Metal 继续使用原有 stage 适配器。

**Primary product call chain:**

```text
PipelineScheduler -> CPUPipelineExecutor::Apply
  -> changed legacy editor controls -> LegacyPipelineImporter (default three-node transition only)
  -> CudaProductRenderer::Render
  -> RawInputLoader::LoadEncoded (CPU unpack boundary)
  -> GraphCompiler::Compile(RenderRequest with viewport ROI / target extent / max edge)
  -> CudaRenderDevice::Execute
  -> ExecuteCudaDevelop
  -> optional ExecuteCudaMask
  -> ExecuteCudaPrimaryGrade
  -> ExecuteCudaDrt
  -> persistent RGBA32F display texture
  -> CUDA device copy to IFrameSink mapping
  -> external semaphore signal -> NotifyFrameReady
```

Pure v2 documents do not mirror the legacy stage adapter. The transition import runs only for a
legacy/default document whose graph still has exactly the three visible nodes, so a document with
additional v2 nodes such as masks is not flattened by the old editor-control path.

**DRT parameter and execution call chain:**

```text
DrtParamsModel dirty fields
  -> TakePendingParameterPatch
  -> ODT_Op runtime resolution (ACES 2.0 tables or OpenDRT settings)
  -> GPUParamsConverter
  -> stable ParameterArena slot keyed by (drt, drt.output)
  -> dirty-range upload
  -> DrtKernel(AP1/ACEScc -> decode scene-linear AP1 -> DRT -> display gamut -> EOTF)
  -> GraphImageCache[(drt, display)]
```

The pending DRT patch commits only after the GPU upload succeeds. A DRT-only change reuses the
Develop and Primary Color Grade image resources. The second identical render performs no CUDA
allocation or free.

**Persistence and legacy-load call chain:**

```text
PipelineMgmtService::LoadPipeline
  -> ElementStore::GetPipelineJsonByElementId
  -> format v2 -> PipelineDocument::FromJson
     old stage JSON -> LegacyPipelineImporter::Import
  -> PipelineGuard::document_
  -> CPUPipelineExecutor::SetPipelineDocument

PipelineMgmtService::SavePipeline / SyncPipelineDocument
  -> PipelineDocument::ToJson(format_version = 2, three visible nodes)
  -> optional nested legacy_stage_adapter for unchanged OpenCL/Metal/editor controls
  -> ElementStore::UpdatePipelineJsonByElementId
  -> PipelineMapper raw JSON update
```

The top-level stored representation is always format version 2. New saves do not write the old
stage object as the pipeline root.

**Primary failure call chain:**

```text
CUDA allocation / upload / kernel failure
  -> CudaRenderDevice::Execute catches
  -> BasicRenderWorkspace::CancelRender
  -> synchronous runtime error reporter
  -> exception propagated through CudaProductRenderer and PipelineScheduler
  -> no legacy CPU image-processing Apply path

frame-sink map / copy / semaphore / host-download failure
  -> CudaProductRenderer reports synchronously
  -> exception propagated through PipelineScheduler
  -> no legacy CPU image-processing Apply path
```

**Files added:**

- `alcedo_studio/src/include/edit/runtime/cuda/cuda_drt_pass.hpp`
- `alcedo_studio/src/include/edit/runtime/cuda/cuda_product_renderer.hpp`
- `alcedo_studio/src/edit/runtime/cuda/cuda_drt_runtime_state.cuh`
- `alcedo_studio/src/edit/runtime/cuda/cuda_drt_pass.cu`
- `alcedo_studio/src/edit/runtime/cuda/cuda_plan_executor.cpp`
- `alcedo_studio/src/edit/runtime/cuda/cuda_product_renderer.cu`
- `alcedo_studio/tests/edit/runtime/cuda_drt_product_test.cpp`

**Files removed:** none.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DefaultCudaPipelineBuildsThreeVisibleNodes` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaDrtOpenDrtProducesFiniteDisplayReferredOutput` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaDrtAces20ProducesFiniteDisplayReferredOutput` | `GpuDagCudaDrtProductTest` | PASS |
| `ChangingDrtPeakLuminanceKeepsDevelopAndGradeCacheValid` | `GpuDagCudaDrtProductTest` | PASS |
| `PipelineMgmtServiceBuildsDefaultGpuDagForNewImage` | `PipelineMapperTest` | PASS |
| `LegacyPipelineImportRendersSameCudaReferenceWithinTolerance` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaBackendFailureDoesNotEnterCpuImageProcessing` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaDefaultPipelineSecondRenderCreatesNoGpuAllocation` | `GpuDagCudaDrtProductTest` | PASS |
| Pipeline/storage/history compatibility regression | `PipelineMapperTest` | PASS, `26/26` |
| G1–G7 graph, geometry, input, workspace, Develop, mask, grade, and DRT regression | ten GPU DAG targets | PASS, `90/90` |
| Windows/CUDA product executable | `alcedo_main` | BUILD PASS |
| UI source unchanged | `git diff --name-only -- alcedo_studio/src/ui` | PASS, empty |

Commands:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target alcedo_main PipelineMapperTest GpuDagCudaDrtProductTest --parallel 4
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagModelGraphTest GpuDagGeometryTest GpuDagMaskStoreTest GpuDagRawInputTest GpuDagCudaGeometryTest GpuDagCudaDevelopTest GpuDagCudaPrimaryGradeTest GpuDagCudaMaskTest GpuDagCudaWorkspaceTest GpuDagCudaDrtProductTest --parallel 4
GpuDagCudaDrtProductTest.exe --gtest_color=no --gtest_brief=1
PipelineMapperTest.exe --gtest_color=no --gtest_brief=1
```

Suite totals: G7 required tests `8/8` PASS; affected GPU DAG tests `90/90` PASS;
PipelineMgmtService/storage/history tests `26/26` PASS. The two disabled pre-existing
`PipelineMapperTest` cases were not executed.

**Checklist / exit condition:** all G7 checks complete. Windows/CUDA defaults to the v2 three-node
DAG, scheduler requests preserve viewport ROI and output-resolution intent, display output is
written directly to the existing frame sink, and host output is downloaded only for callers that
explicitly require it. CUDA errors cancel the incomplete workspace submission and propagate
without CPU image processing. The stored root is format version 2 with three visible nodes. No UI
source file changed.

**Performance and allocation evidence:** a DRT peak-luminance-only edit retained both upstream
texture resource ids. A second identical default render recorded `0` CUDA allocations and `0`
CUDA frees. The DRT output texture and resolved method resources remain owned by the per-pipeline
`CudaRenderDevice` workspace/runtime state.

**LOC note (grill-code-review):** new implementation files remain focused:
`cuda_product_renderer.cu` 149 lines, `cuda_drt_pass.cu` 139 lines,
`cuda_plan_executor.cpp` 42 lines, and `cuda_drt_product_test.cpp` 191 lines. Existing large
`pipeline_service.cpp` and `pipeline_cpu.cpp` received localized routing/persistence changes; no
new file exceeds 500 lines.

**Residual gaps:** G7R 审计确认当前 CUDA 产品路径仍缺少
PreparedRawInput/ExecutionPlan/节点结果的内容感知缓存、geometry 后缓存，以及正确的
CameraMatrices 双光源 Camera→AP1。G7 的资源 ID 与零 allocation 证据不能证明结果 cache hit
或没有重复计算。G7 接入的 legacy 参数镜像、importer 和 nested adapter 也不是目标架构，
G7R 必须从 CUDA 产品路径删除。OpenCL 和 Metal 仍通过旧 stage adapter 执行；它们的完整
DAG runtime 移植必须在 G7R 达标后进入 G8/G9。

## 41. Phase G7R — CUDA 默认管线行为、颜色与性能恢复

Branch: `feature/gpu-dag-cuda-default-recovery`

Base: `feature/gpu-dag-cuda-drt-product`

Status: G7R.1–G7R.3 complete；G7R.H complete；G7R.4–G7R.5 remaining；G7R 完成前禁止开始 G8 的 OpenCL 像素路径移植。

Requirement source: `codex://threads/01a0273a-48bd-7702-9503-127bb5e2ec1e`。本阶段恢复该
任务中已经明确的 Develop endpoint、GPU workspace KV cache 和动态分辨率要求，不重新定义
产品范围。

目标：

- 保留 G4/G5 把传感器开发与 CameraColorPass 分开的正确设计，使 Develop 的昂贵结果可缓存；
- 从 CUDA 产品路径删除 legacy 参数、importer、snapshot 和 stage adapter；
- 使用 CameraMatrices/DNG 双光源矩阵插值恢复正确的 RAW CCT/tint 和 Camera→AP1；
- 增加 geometry 后 camera scene-linear 结果缓存，并让所有结果缓存使用内容 key；
- 消除无变化渲染中的 LibRaw open/unpack、源图上传、GraphCompiler 编译和 GPU pass 重算；
- 用同机 A/B 数据证明默认 CUDA 管线达到重构前基线，不以“没有新 GPU 分配”代替性能证据。

### 41.1 审计结论与现有证据

本节区分用户观察到的运行时失败、已执行测试、结构检查和覆盖缺口。代码检查可以证明调用
发生或测试没有断言某项行为，但不能代替像素正确性测试。

| 优先级 | 证据类别 | 问题 | 当前证据 | 必须恢复的行为 |
| --- | --- | --- | --- | --- |
| P0 | Observed failure | 真实 RAW 输出明显偏绿，色温行为不正确 | 编辑器实际使用反馈；当前本地真实 RAW E2E 被环境开关跳过，尚无自动化像素复现 | 真实 RAW 在 as-shot 与 custom CCT 下匹配旧管线参考，不出现通道异常 |
| P0 | Structural evidence | `RawInputLoader::FillColorContext` 只写 `cam_mul`、`pre_mul`、make/model，未写 CameraMatrices、forward matrices、标定光源和 as-shot neutral | `raw_input_loader.cpp` 与 `metadata_extractor.cpp::PopulateMetadataRuntimeContext` 对比 | 输入准备使用完整、共享的 RAW 色彩 metadata 解析结果 |
| P0 | Structural evidence | 当前 `MakeCameraToAp1` 计算 `sRGB_to_AP1 * rgb_cam`；`rgb_cam` 全零时静默返回 identity | `cuda_primary_grade_pass.cu` | CameraColorPass 使用旧管线双光源矩阵插值；缺失矩阵不能静默 identity |
| P0 | Structural evidence | Develop 的 WB mode、as-shot/custom CCT 和 tint 已进入 Model/JSON，但 CUDA pass 未消费；Develop 只读取高光恢复开关 | `cuda_develop_pass.cpp` 与 Develop DTO/Model 对比 | CCT/tint 进入独立 CameraColorPass 内容 key 并真实改变像素 |
| P0 | Structural evidence | `develop.image` 在当前执行路径中实际保存 geometry 后 camera RGB，而 GraphCompiler 把该端口声明为 AP1 | Develop pass、ExecutionPlan 与 Primary Grade 输入对比 | `develop.image` 只能保存 CameraColorPass 后 AP1；内部缓存使用不同值 ID |
| P0 | Structural evidence | `CudaProductRenderer::Render` 每次都 `LoadEncoded` 和 `Compile`，executor 每次无条件运行 Develop、Grade、DRT | `cuda_product_renderer.cu`、`cuda_plan_executor.cpp` | 无变化帧不重复输入准备、编译和 GPU 节点执行 |
| P0 | Structural evidence | `GraphImageCache`/`NodeResultCache` 只按 `GraphValueId` 保存资源，没有内容 key 和有效性；当前所谓 cache valid 只比较纹理 `ResourceId` | cache headers 与 G7 测试断言 | 分配复用与结果命中成为两个独立概念，缓存命中由内容 key 证明 |
| P0 | Structural evidence | 当前没有 geometry 后结果缓存，CCT、Grade 或 DRT 编辑无法明确复用重采样结果 | Develop pass 与 workspace image map | `geometry.scene_source` 是内容感知的一级结果缓存 |
| P1 | Structural evidence | 当前 creative white balance 只做每通道指数缩放，不是计划要求的 AP1/CAT02 色适配 | CUDA adjustment runtime | 局部色温使用实际 CAT02；不与 RAW CameraColorPass 混用 |
| P1 | Coverage gap | `ChangingDrtPeakLuminanceKeepsDevelopAndGradeCacheValid` 只比较资源 ID；第二帧测试只检查 malloc/free | `cuda_drt_product_test.cpp` | 测试断言 LibRaw、H2D、compile、每类 pass execute/skip 和内容 key |
| P1 | Coverage gap | Primary Grade 色彩测试使用 direct RGB，并明确让 camera conversion 保持 identity | `cuda_primary_grade_test.cpp` | 加入真实 RAW、CameraMatrices、双光源插值和 AP1 reference 测试 |

审计时执行的集中测试：

```text
ctest --test-dir build/debug --output-on-failure \
  -R "GpuDagRawInputTest|GpuDagCudaPrimaryGradeTest|GpuDagCudaDrtProductTest|EditorRealRawGpuE2eTest.CudaSustainedImageSwitches"
```

结果为 `26/26` PASS，耗时 `5.53 s`。该结果只证明现有窄测试仍通过，不否定上表问题。
直接执行真实 RAW 编辑器用例时结果是 `0` PASS、`1` SKIPPED；它受
`ALCEDO_RUN_DEADLOCKING_RAW_GPU_E2E` 控制。因此目前没有一个默认执行的测试同时覆盖真实
RAW、CameraMatrices 色彩和重复渲染性能。

### 41.2 恢复后的产品调用链

默认三节点图不增加用户节点。产品执行链固定为：

```text
CPUPipelineExecutor::Apply
  -> translate existing executor inputs into immutable RenderRequest
       source content key / document snapshot
       viewport / target extent / max edge / DecodeRes / quality
  -> CudaProductPipelineSession::Render
  -> PreparedSourceCache lookup
       miss -> RawInputLoader open + unpack + downsample + complete color metadata
  -> Static ExecutionPlan lookup
       miss -> GraphCompiler::Compile
  -> ResolvedRenderGeometry
  -> ResultCache lookup and minimal pass schedule
       SensorDevelopPass -> develop.sensor_linear
       GeometryResamplePass -> geometry.scene_source
       CameraColorPass -> develop.image
       optional MaskPass
       PrimaryGradePass
       DrtPass
  -> completion fence
  -> publish successful cache entries
  -> existing bound FrameSubmission / IFrameSink path
  -> return through the existing CPUPipelineExecutor result path
```

`CudaProductPipelineSession` 表示一个打开图像和 PipelineDocument 的可复用运行实例。它拥有
PreparedSourceCache、静态 ExecutionPlan、CudaRenderDevice/workspace 和结果 cache metadata。
它不能在每个 `Apply` 调用中临时创建，也不能借助旧 merged stage 保存结果。

### 41.3 G7R.1 — 缓存 Prepared RAW 和静态 ExecutionPlan

Prepared source key 至少包含：

```text
encoded content hash
input kind and CFA description
DecodeRes/downsample policy
active area and orientation inputs
LibRaw/input-preparation implementation version
```

工作：

- [x] 同一 source key 的 preview/quality/detail 渲染共享 PreparedRawInput；
- [x] 未淘汰的 source 再次打开时复用主存结果，不重新执行 LibRaw open/unpack；
- [x] DecodeRes 或输入准备规则改变时生成新 key，不覆盖仍被 submission 使用的旧条目；
- [x] source host bytes 只在 `develop.sensor_linear` miss 时上传；
- [x] 静态 ExecutionPlan key 只包含 graph topology、节点/调整类型与顺序、source layout 和后端能力；
- [x] 参数值、viewport、CCT、Grade 和 DRT 编辑不触发静态 plan 重编译；
- [x] ResolvedRenderGeometry 和 pass dirty schedule 属于每帧只读数据，不通过重编译静态图表达；
- [x] 接入并测试 `GraphCompiler::NeedsRecompile`，或删除该未使用 API 并用单一 plan key 机制替代；
- [x] plan cache miss、hit 和 compile count 必须可查询。

##### Phase G7R.1 completion record (2026-08-22)

**Status:** complete — reusable CUDA product session caches PreparedRawInput and the static
ExecutionPlan by content key. Parameter, viewport, CCT, Grade, and DRT edits no longer
recompile. Source H2D skip on `develop.sensor_linear` miss is owned by G7R.2.

`CudaProductRenderer` is the session named in 41.2 (`CudaProductPipelineSession`). It is
created once per `CPUPipelineExecutor` and owns PreparedSourceCache, StaticExecutionPlanCache,
and one `CudaRenderDevice`.

**Primary success call chain:**

```text
CPUPipelineExecutor::Apply
  -> CudaProductRenderer::Render
  -> PreparedSourceCache::AcquireEncoded(encoded hash + DecodeRes + prep version)
       hit  -> lease existing PreparedRawInput
       miss -> unpack (LibRaw in production) + insert under a new key
  -> StaticExecutionPlanCache::GetOrCompile(StaticPlanKey)
       hit  -> copy cached pass list
       miss -> GraphCompiler::CompileStatic
  -> GraphCompiler::BindFrameGeometry (per-frame ResolvedRenderGeometry)
  -> CudaRenderDevice::Execute
  -> existing IFrameSink present or host download
```

**Primary failure call chain:**

```text
unpack / CompileStatic / Execute throw
  -> PreparedSourceCache lease released; prior host and plan entries stay
  -> CudaRenderDevice::CancelRender + ReportError
  -> exception returns through CPUPipelineExecutor (no CPU image processing)
```

DecodeRes change inserts a second prepared-source key and does not replace a leased FULL
entry. Adjustment-order or mask-topology change compiles a new static plan and leaves the
previous plan in the cache.

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `PreparedSourceKeyIncludesEncodedHashKindDecodeResAndPreparationVersion` | `GpuDagRawInputTest` | PASS |
| `PreparedSourceCacheReusesHostResultForSameEncodedKeyWithoutCallingUnpack` | `GpuDagRawInputTest` | PASS |
| `PreparedSourceCacheSharesHostResultAcrossPreviewAndExportQuality` | `GpuDagRawInputTest` | PASS |
| `PreparedSourceCacheUsesNewKeyForDecodeResChangeWithoutReplacingLeasedEntry` | `GpuDagRawInputTest` | PASS |
| `PreparedSourceCacheReusesMatchingSourceAfterSwitchingEncodedBuffers` | `GpuDagRawInputTest` | PASS |
| `PreparedSourceCacheDoesNotEvictLeasedEntryWhenHostBudgetIsExceeded` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerNeedsRecompileIsFalseForUnchangedTopologyAndSourceLayout` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerNeedsRecompileIsTrueWhenAdjustmentOrderChanges` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerNeedsRecompileIsTrueWhenSourceExtentChanges` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerCompileDoesNotBakeViewportIntoStaticPlanKey` | `GpuDagRawInputTest` | PASS |
| `GraphCompilerBindsFrameGeometryWithoutChangingStaticPlanKey` | `GpuDagRawInputTest` | PASS |
| `StaticExecutionPlanCacheCompilesOnceForRepeatedParameterAndViewportEdits` | `GpuDagRawInputTest` | PASS |
| `StaticExecutionPlanCacheRecompilesWhenMaskTopologyChanges` | `GpuDagRawInputTest` | PASS |
| `StaticExecutionPlanCacheRecompilesWhenSourceLayoutChanges` | `GpuDagRawInputTest` | PASS |
| `StaticExecutionPlanCacheExposesHitMissAndCompileCounts` | `GpuDagRawInputTest` | PASS |
| `ProductRendererCompilesStaticPlanOnlyForTopologyOrSourceLayoutChange` | `GpuDagCudaDrtProductTest` | PASS |
| `ProductRendererReusesPreparedSourceAfterSwitchingEncodedBuffers` | `GpuDagCudaDrtProductTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagRawInputTest GpuDagCudaDrtProductTest GpuDagCudaDevelopTest GpuDagCudaPrimaryGradeTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagRawInputTest|GpuDagCudaDrtProductTest|GpuDagCudaDevelopTest|GpuDagCudaPrimaryGradeTest|GpuDagCudaWorkspaceTest.GpuDagCudaWorkspace.GpuAndRuntimeHeadersDoNotIncludeCudaOrImageBuffer"
```

Suite totals: `49/49` PASS (`GpuDagRawInputTest` 24, CUDA Develop 6, CUDA Primary Grade 9,
CUDA DRT product 9, runtime header hygiene 1).

**Checklist / exit condition:** eight of nine G7R.1 work items checked. The remaining item is
source H2D only on `develop.sensor_linear` miss.

**LOC note (grill-code-review):** new files stay small: `prepared_source_cache.hpp` 99,
`prepared_source_cache.cpp` 117, `static_execution_plan_cache.hpp` 49,
`static_execution_plan_cache.cpp` 21, `cuda_product_plan_cache_test.cpp` 120,
`prepared_source_cache_test.cpp` 113, `static_execution_plan_cache_test.cpp` 141.
`cuda_product_renderer.cu` 157, `graph_compiler.cpp` 202, `raw_input_loader.cpp` 424.
No changed file exceeds 500 lines.

**Residual gaps:** source host-byte upload still runs on every `ExecuteCudaDevelop` of a Bayer
input. Skipping H2D requires the G7R.2 `develop.sensor_linear` result cache. GPU node pass
skip, geometry-after cache, CameraMatrices AP1, CAT02, and 41.9 A/B remain G7R.2–G7R.5.
Crop/viewport still force GeometryResample; the product Bayer path currently throws
`GeometryResamplePass::Encode: textures must be RGBA32F` for those frames, so the product
test binds viewport/crop geometry without executing that resample. G7R.2 should keep that
failure visible until the result cache owns geometry.

### 41.4 G7R.2 — 内容感知结果缓存和 geometry 后缓存

实现第 27 节定义的五层缓存，并保持以下值语义：

```text
develop.sensor_linear   = camera scene-linear，geometry 前
geometry.scene_source   = camera scene-linear，geometry 后
develop.image           = AP1 primaries / ACEScc encoded，CameraColorPass 后
grade.<id>.image        = AP1 primaries / ACEScc encoded
drt.display             = display-referred output
```

工作：

- [x] 从 `GraphImageCache` 中拆出 allocation slots 与 valid results，或让 API 强制调用者明确选择
  `AcquireTextureForWrite`、`FindValidResult`、`PublishResult`；
- [x] 缓存查找比较 value ID、内容 key、extent、format 和完成 submission；
- [x] pass 写入临时 unpublished 状态，只在 submission 成功后原子发布内容 key；
- [x] 取消、kernel 失败、frame-sink 失败不能把部分写入标成有效；
- [x] 同 key 已命中时跳过 pass，保留原 `last_writer` 和内容身份；
- [x] 同一物理纹理被新 key 覆写前，先移除旧结果身份；
- [x] geometry cache key 使用完整 ResolvedRenderGeometry，包括 crop、rotation、ROI、目标尺寸、
  DecodeRes 映射和采样规则；
- [x] CCT/tint、Grade、mask 或 DRT 改变不得使 `geometry.scene_source` 失效；
- [x] viewport/geometry 改变复用 `develop.sensor_linear`，只重跑 Geometry 和下游；
- [x] workspace LRU 按 GPU 字节预算淘汰已完成且没有 lease 的结果；
- [x] 结果缓存支持 source/document revision 隔离，防止切图后错误命中。

主要文件：

- `alcedo_studio/src/include/edit/runtime/graph_image_cache.hpp`
- `alcedo_studio/src/include/edit/runtime/node_result_cache.hpp`
- `alcedo_studio/src/include/edit/runtime/basic_render_workspace.hpp`
- `alcedo_studio/src/edit/runtime/cuda/cuda_develop_pass.cpp`
- `alcedo_studio/src/edit/runtime/cuda/cuda_plan_executor.cpp`

##### Phase G7R.2 completion record (2026-08-22)

**Status:** complete — content-keyed GPU result cache with independent
`develop.sensor_linear`, `geometry.scene_source`, and `develop.image` values.
Unchanged, Exposure, CCT, DRT, viewport, geometry, Develop, and image-switch
frames skip or execute the required pass set. Source H2D runs only on a
`develop.sensor_linear` miss.

`GraphImageCache` is now a content-keyed result cache. Allocation reuse is
`AcquireTextureForWrite` / TexturePool. A content hit is `BindValidResult` /
`FindValidResult` with matching value ID, content key, extent, format, and a
completed `last_writer`. Passes write unpublished slots; `PublishResults` runs
only after a successful submit and, on the product path, after present/download.

**Primary success call chain:**

```text
CPUPipelineExecutor::Apply
  -> CudaProductRenderer::Render
  -> PreparedSourceCache::AcquireEncoded
  -> StaticExecutionPlanCache::GetOrCompile
  -> GraphCompiler::BindFrameGeometry
  -> BuildFrameResultContentKeys
  -> CudaRenderDevice::Execute(publish_on_success=false)
       BeginRender waits previous submission and drops leftover unpublished writes
       BindValidResult(develop.sensor_linear)
         hit  -> skip SensorDevelop, no source H2D
         miss -> ExecuteCudaDevelop + RecordUnpublished
       BindValidResult(geometry.scene_source)
         hit  -> skip Geometry
         miss -> ExecuteCudaGeometryResample + RecordUnpublished
       BindValidResult(develop.image)
         hit  -> skip CameraColor
         miss -> ExecuteCudaCameraColor + RecordUnpublished
       BindValidResult(grade.primary.image) / (drt, display)
         hit  -> skip
         miss -> ExecuteCudaPrimaryGrade / ExecuteCudaDrt + RecordUnpublished
  -> present or host download
  -> CudaRenderDevice::PublishResults
```

**Primary failure call chain:**

```text
kernel / source H2D / frame-sink mapping failure
  -> CudaRenderDevice::CancelRender
  -> GraphImageCache::DiscardUnpublished
  -> previously published content keys remain FindValidResult hits
  -> new unpublished keys are not published
  -> exception returns through CPUPipelineExecutor (no CPU image processing)
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `SecondUnchangedProductRenderRunsNoLibRawNoSourceUploadAndNoGpuNodePass` | `GpuDagCudaDrtProductTest` | PASS |
| `ExposureEditRunsOnlyPrimaryGradeAndDrtPasses` | `GpuDagCudaDrtProductTest` | PASS |
| `DevelopCctEditReusesSensorAndGeometryAndRunsCameraColorGradeDrt` | `GpuDagCudaDrtProductTest` | PASS |
| `DrtEditRunsOnlyDrtPass` | `GpuDagCudaDrtProductTest` | PASS |
| `ViewportChangeReusesSensorDevelopAndRunsGeometryAndDownstream` | `GpuDagCudaDrtProductTest` | PASS |
| `GeometryEditReusesSensorDevelopAndInvalidatesPostGeometryResult` | `GpuDagCudaDrtProductTest` | PASS |
| `RawDevelopEditInvalidatesSensorDevelopAndAllDownstreamResults` | `GpuDagCudaDrtProductTest` | PASS |
| `ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults` | `GpuDagCudaDrtProductTest` | PASS |
| `ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit` | `GpuDagCudaWorkspaceTest` | PASS |
| `FailedSubmissionDoesNotPublishResultContentKey` | `GpuDagCudaWorkspaceTest` and `GpuDagCudaDrtProductTest` | PASS |
| `CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable` | `GpuDagCudaWorkspaceTest` and `GpuDagCudaDrtProductTest` | PASS |
| `SensorLinearKeyIgnoresCctTintGradeAndDrt` | `GpuDagRawInputTest` | PASS |
| `GeometryKeyIncludesViewportAndCropAndIgnoresGrade` | `GpuDagRawInputTest` | PASS |
| `HighlightRecoverChangesSensorLinearAndAllDownstreamKeys` | `GpuDagRawInputTest` | PASS |
| `ProductRendererCompilesStaticPlanOnlyForTopologyOrSourceLayoutChange` | `GpuDagCudaDrtProductTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagRawInputTest GpuDagCudaDrtProductTest GpuDagCudaDevelopTest GpuDagCudaPrimaryGradeTest GpuDagCudaWorkspaceTest GpuDagCudaMaskTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagRawInputTest|GpuDagCudaDrtProductTest|GpuDagCudaDevelopTest|GpuDagCudaPrimaryGradeTest|GpuDagCudaWorkspaceTest|GpuDagCudaMaskTest"
```

Suite totals: `92/92` PASS (`GpuDagRawInputTest` 29, CUDA Develop 6, CUDA Primary Grade 9,
CUDA DRT product 21, CUDA Mask 11, CUDA workspace 16). One mask mix test was updated
to run Geometry and CameraColor before Primary Grade after the value-ID split.

**Checklist / exit condition:** all 11 G7R.2 work items checked. G7R.1 remaining
source-H2D-on-sensor-miss item is also checked.

**LOC note (grill-code-review):** new files stay small: `content_key.hpp` 107,
`graph_image_cache.hpp` 329, `gpu_node_pass_stats.hpp` 37,
`result_content_key.hpp` 51, `result_content_key.cpp` 228,
`cuda_camera_color_pass.cu` 90, `cuda_plan_executor.cpp` 130,
`result_content_key_test.cpp` 129, `graph_image_cache_test.cpp` 122,
`cuda_result_cache_test.cpp` 320. `cuda_develop_pass.cpp` 230,
`cuda_product_renderer.cu` 184, `cuda_primary_grade_pass.cu` 400.
No changed file exceeds 500 lines.

**Residual gaps:** CameraColor still uses the current `rgb_cam` × sRGB→AP1 matrix
and may still fall back to identity; G7R.3 replaces that with CameraMatrices/DNG
dual-illuminant interpolation and rejects missing/singular matrices. Creative
CAT02 remains channel scaling until G7R.4. Full 41.9 A/B timing, GPU
allocation/free, and duration snapshots remain G7R.5. `develop.image` is a
separate AP1-bound GraphValue after CameraColor, but AP1 correctness is not
claimed until G7R.3 reference tests.

### 41.5 G7R.3 — 恢复 CameraMatrices 双光源插值和 Develop AP1 输出

颜色实现必须从旧 `ColorTempOp::ResolveRuntime` 提取一个不依赖 `OperatorParams` 的纯解析器，
例如：

```cpp
struct DevelopColorTransform {
  Matrix3x3 camera_to_xyz;
  Matrix3x3 camera_to_xyz_d50;
  Matrix3x3 xyz_d50_to_ap1;
  Matrix3x3 camera_to_ap1;
  float resolved_cct;
  float resolved_tint;
  ContentKey content_key;
};

Expected<DevelopColorTransform, ColorTransformError>
ResolveDevelopColorTransform(const DevelopPayload& develop,
                             const RawRuntimeColorContext& raw);
```

算法顺序固定为：

1. 从 DNG metadata 或 CameraMatrices 数据库选择 `ColorMatrix1/2`，并读取对应标定光源 CCT；
2. 缺少任一有效标定光源时按单光源 profile 处理，不虚构 2856K/6504K 双光源区间；
3. as-shot 模式从 `AsShotNeutral` 求解相机白点；只有 metadata 未提供 neutral 时才由
   `cam_mul` 推导 neutral；
4. custom 模式把 CCT/tint 转为目标 xy；
5. 在 mired 空间按目标 CCT 插值 `ColorMatrix1/2`；
6. 存在 `ForwardMatrix1/2` 时以相同权重插值 forward matrix，并用 reference neutral 缩放
   得到 camera→XYZ D50；
7. 不存在 forward matrix 时反转插值后的 XYZ→camera 矩阵，再从目标白点 Bradford 到 D50；
8. 执行 Bradford D50→D60 和 XYZ D60→ACES AP1；
9. 校验每个矩阵有限、可逆且输出有限，再生成 content key；
10. CameraColorPass 只消费解析后的 3×3 矩阵，并把结果写入 `develop.image`。

明确禁止：

- 用 `cam_mul` 或 `pre_mul` 构造 Camera→AP1 对角矩阵；
- 使用 LibRaw `rgb_cam`、`cam_xyz` 或 `pre_mul` 路径替代 CameraMatrices/DNG profile；
- 先假定 camera RGB 是 sRGB，再乘 `sRGB_to_AP1`；
- metadata 缺失、矩阵全零或不可逆时静默使用 identity；
- 在 Primary Grade kernel 内隐式完成 Camera→AP1；CameraColorPass 必须是可单独失效和计数的
  Develop 内部 pass；
- 把 RAW CCT/tint 与 Primary Grade 的 creative CAT02 参数合并成一个 dirty 区域。

输入准备必须复用 `MetadataExtractor::PopulateRuntimeContextFromOpenLibRaw` 及其 CameraMatrices/
DNG 解析能力，或提取共享 `RawColorMetadataResolver`。不得继续维护只有六个字段的
`FillColorContext` 副本。若 `LoadEncoded` 只有字节而没有路径，DNG tag 读取必须支持内存输入，
或由调用方把已经解析的不可变 metadata snapshot 一起传入；不能因此退回 LibRaw 对角缩放。

实施时 ColorMatrix/ForwardMatrix/AsShotNeutral 作为 Develop JSON 的可序列化参数存在，在导入时
由 MetadataExtractor 写入 `DevelopPayload::camera_profile`。`ResolveDevelopColorTransform`
只读该 payload，不接收 `RawRuntimeColorContext`，也不在 GPU execute 时打开 RAW 或查询
CameraMatrices 数据库。用户 CCT/tint 由 CPU 做 mired 双光源插值，把解析后的 3×3 写入
`CameraColorGpuParams`；kernel 只做 camera RGB × 该矩阵。`develop_color_transform.hpp`
包含 `develop_node_model.hpp`，`BindDevelopCameraProfile` 只接受 `DevelopPayload&`。
`PipelineDocument` 绑定发生在 `CPUPipelineExecutor` 内部。CUDA/executor 头文件包含
`pipeline_document.hpp`，不使用 `PipelineDocument` 前向声明。解析结果用
`ColorTransformResult`，不用 `std::expected`。

工作：

- [x] `DevelopCameraProfile` 进入 `DevelopPayload`，JSON `camera_profile` 可往返；
- [x] `ResolveDevelopColorTransform(const DevelopPayload&)` 执行第 1–9 步，不依赖
  `OperatorParams`、LibRaw、`rgb_cam`、`cam_xyz`、`pre_mul` 或 CameraMatrices 数据库；
- [x] `BindDevelopCameraProfile` 在 `InjectRawMetadata` 时从 MetadataExtractor 上下文复制矩阵，
  并求解 as-shot CCT/tint；`SetPipelineDocument` 与 CUDA legacy JSON mirror 之后重新绑定，
  避免 profile 被擦掉；
- [x] CameraColorPass 在 CPU 插值后 `BindSlot`/`ApplyPatch` 到 ParameterArena slot
  `{develop, "camera_color"}`，kernel 只乘 `camera_to_ap1`；
- [x] 缺失或奇异矩阵抛错，CUDA 路径不静默 identity；
- [x] `develop.image` 内容 key 哈希 camera_profile 与 WB/CCT；
  `kCameraColorImplementationVersion = 2`；
- [x] OpenCL/Metal 仍走的 `ColorTempOp` 改为调用同一解析器。

##### Phase G7R.3 completion record (2026-08-23)

**Status:** complete — serializable Develop camera profile, CPU mired dual-illuminant
interpolation, CUDA CameraColor multiplies the uploaded 3×3, import bind from
MetadataExtractor. Creative CAT02 remains G7R.4. 41.8.2 `bf6686fb` pixel goldens
and 41.8.4 editor E2E remain residual.

**Primary success call chain:**

```text
ImportService / PipelineController / ExportService
  -> MetadataExtractor::ExtractEXIF_ToImage
       PopulateRuntimeContextFromOpenLibRaw (ColorMatrix/ForwardMatrix/AsShotNeutral)
  -> CPUPipelineExecutor::InjectRawMetadata
  -> BindDevelopCameraProfile(DevelopPayload, RawRuntimeColorContext)
       copy matrices onto DevelopPayload.camera_profile
       as-shot resolve -> as_shot_cct / as_shot_tint
  -> ColorTempOp JSON {as_shot_cct, as_shot_tint, mode, custom_cct, custom_tint}
       EditorColorTempModel::loadFromOperatorParams
  -> user CCT/tint edit submits ColorTemp JSON
  -> CUDA legacy import copies mode/CCT/tint onto DevelopPayload
  -> ExecuteCudaCameraColor
       ResolveDevelopColorTransform(DevelopPayload)   // CPU, stored matrices only
       ParameterArena BindSlot/ApplyPatch {develop, "camera_color"}
       UploadDirty
        CameraColorKernel: ap1_linear = camera_to_ap1 * camera_rgb
                           working = ACESccEncode(ap1_linear)
   -> publish develop.image (AP1 primaries / ACEScc encoded)
```

**Primary failure call chain:**

```text
missing / singular / non-finite ColorMatrix on DevelopPayload
  -> ResolveDevelopColorTransform returns ColorTransformResult.ok = false
  -> ExecuteCudaCameraColor throws (no identity camera_to_ap1)
  -> CudaRenderDevice::CancelRender + DiscardUnpublished
  -> previously published geometry.scene_source remains valid
  -> exception returns through CPUPipelineExecutor (no CPU image processing)

legacy JSON mirror would drop camera_profile
  -> InjectRawMetadata stashes RawRuntimeColorContext
  -> ApplyImportedCameraProfile after SetPipelineDocument and after each mirror
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DevelopColorTransformInterpolatesDualIlluminantMatricesInMiredSpace` | `GpuDagModelGraphTest` | PASS |
| `DevelopColorTransformInterpolatesForwardMatricesWithTheSameWeight` | `GpuDagModelGraphTest` | PASS |
| `DevelopColorTransformUsesSingleProfileWhenCalibrationIlluminantsAreIncomplete` | `GpuDagModelGraphTest` | PASS |
| `DevelopColorTransformSolvesAsShotNeutralWithoutUsingCamMulAsColorMatrix` | `GpuDagModelGraphTest` | PASS |
| `DevelopColorTransformDoesNotUseLibRawRgbCamOrPreMulAsCameraMatrix` | `GpuDagModelGraphTest` | PASS |
| `DevelopColorTransformRejectsMissingOrSingularCameraMatrices` | `GpuDagModelGraphTest` | PASS |
| `DevelopCameraProfileJsonRoundTripPreservesMatricesAndAsShotNeutral` | `GpuDagModelGraphTest` | PASS |
| `BindDevelopCameraProfileWritesAsShotCctFromStoredNeutral` | `GpuDagModelGraphTest` | PASS |
| `CameraColorEncodesAp1AsAcesccGraphWorkingSpace` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `ExecuteCudaCameraColorRejectsMissingCameraMatrices` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `BindDevelopCameraProfileCopiesDngColorMatricesFromMetadataExtractor` | `GpuDagCudaDrtProductTest` | PASS |
| `BindDevelopCameraProfileCopiesNonDngCameraMatricesFromMetadataExtractor` | `GpuDagCudaDrtProductTest` | PASS |
| `CameraProfileChangeInvalidatesDevelopImageNotSensorLinear` | `GpuDagRawInputTest` | PASS |
| `DevelopCctEditReusesSensorAndGeometryAndRunsCameraColorGradeDrt` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaCat02WhiteBalanceZeroOffsetPreservesAp1White` | `GpuDagCudaPrimaryGradeTest` | PASS (channel-scale CAT02; G7R.4 replaces) |

`RawInputLoaderPopulatesCompleteColorContextFromRealRaw` is not added. EditInput stays
Image-free; CameraColor no longer reads `PreparedRawInput.color_context`. Import-time
MetadataExtractor bind is the source of the serializable profile. The two
`BindDevelopCameraProfileCopies*` tests cover DNG and ARW.

`CudaCat02ZeroOffsetIsExactIdentity` and `CudaCat02MapsSourceWhiteToRequestedWhiteInAp1`
belong to G7R.4.

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest GpuDagCudaMaskTest GpuDagModelGraphTest GpuDagRawInputTest GpuDagCudaWorkspaceTest GpuDagCudaDevelopTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagModelGraphTest|GpuDagRawInputTest|GpuDagCudaDevelopTest|GpuDagCudaPrimaryGradeTest|GpuDagCudaDrtProductTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest"
```

Suite totals: `123/123` PASS (`GpuDagModelGraphTest` 26, `GpuDagRawInputTest` 30,
CUDA Develop 6, CUDA Primary Grade 11, CUDA DRT product 23, CUDA Mask 11,
CUDA workspace 16).

**Checklist / exit condition:** seven of seven G7R.3 work items checked. Algorithm
steps 1–10 run from stored Develop params (step 1 at import bind, steps 2–9 in
`ResolveDevelopColorTransform`, step 10 in CameraColorPass).

**LOC note (grill-code-review):** new files: `develop_color_transform.cpp` 665,
`develop_color_transform.hpp` 74, `camera_color_gpu_params.hpp` 18,
`develop_color_transform_test.cpp` 251, `test_camera_profile.hpp` 39,
`develop_camera_profile_import_test.cpp` 83. `color_temp_op.cpp` 418 (was ~1087).
`cuda_camera_color_pass.cu` 100. `develop_node_model.hpp` 114,
`develop_node_model.cpp` 121. No new file exceeds 1000 lines.
`pipeline_cpu.cpp` remains 742; this phase only binds the camera profile on inject,
document set, and legacy mirror.

**Remaining gaps:** 41.8.2 `bf6686fb` CameraToAp1 pixel goldens are not generated.
41.8.4 editor real-RAW E2E still sits behind `ALCEDO_RUN_DEADLOCKING_RAW_GPU_E2E`.
`RawInputLoader::FillColorContext` still writes only cam_mul/pre_mul/make/model;
the CUDA product path does not use that context for CameraColor. OpenCL/Metal
`ColorTempOp` still has a `cam_xyz` fallback when ColorMatrix is missing; CUDA
does not. Creative CAT02 remains per-channel scaling until G7R.4. 41.9 A/B
timing remains G7R.5.

### 41.5b G7R.H — CUDA 默认管线产品回归修复

Branch: `feature/gpu-dag-cuda-default-recovery`

在 G7R.4 CAT02 之前修复三件产品回归：RAW demosaic/HLR 未接入 CUDA Develop、归还 pipeline 时 GPU 会话缓存不释放、DRT 入口硬裁 scene-linear。

工作：

- [x] SensorDevelop 读取 `demosaic_method`；default Bayer=Legacy RCD，default X-Trans=Neural Engine
- [x] Neural 失败抛错误字符串，不静默落到 Legacy
- [x] HLR on 时 CFA 不 Clamp01；HLR 在 Bayer 和 X-Trans demosaic 之后的 RGB 上运行
- [x] `CudaProductRenderer::ReleaseSessionCaches` 在 `ClearAllIntermediateBuffers` / `ReleaseAllGPUResources` 中调用
- [x] 产品 TexturePool 预算来自设备显存（下限 256 MiB），不再写死 64 MiB
- [x] 删除 `OutputTransform_fwd` 入口 `clamp_AP1`；`kDrtImplementationVersion = 2`；`kSensorDevelopImplementationVersion = 2`
- [x] `PipelineScheduler` 把异常 `what()` 传给 `on_complete_(success, message)`

##### Phase G7R.H completion record (2026-08-23)

**Status:** complete — CUDA Develop honors demosaic/HLR, session caches drop on pipeline return, DRT no longer hard-clips AP1 before tonescale.

**Primary success call chain:**

```text
EditorRawDecodePanel submit method / highlights_reconstruct
  -> CPUPipelineExecutor::Apply mirrors legacy JSON
  -> ExecuteCudaDevelop
       ToLinearRef
       Clamp01(CFA) only if HLR off
       ResolveDevelopDemosaicMethod
       RCD | XTrans interpolator | Neural student tiles
       optional HighlightCorrection on RGB
       pack develop.sensor_linear

editor switch image -> SavePipeline last pin
  -> ClearAllIntermediateBuffers
       -> CudaProductRenderer::ReleaseSessionCaches
            WaitIdle
            GraphImageCache::Clear + TexturePool::ReleaseUnleased
            PreparedSourceCache::Clear
            NeuralDemosaicWorkspace reset
```

**Primary failure call chain:**

```text
Neural preprocess / tile / EnsureLoaded failed
  -> throw runtime_error("Neural Engine ...")
  -> CudaRenderDevice::CancelRender + ReportError
  -> PipelineScheduler catch exception -> on_complete_(false, what())
  -> FinishJob(false, message) -> editor last_error_
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CudaDevelopDefaultBayerUsesLegacyRcdNotNeural` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopDefaultXTransUsesNeuralEngine` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopExplicitNeuralEngineChangesBayerPixelsVersusLegacy` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopHighlightReconstructOnSkipsCfaClamp01ForBayerAndXTrans` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopHighlightReconstructOffAppliesCfaClamp01` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopHighlightReconstructChangesXTransRgb` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopNeuralEngineFailureThrowsErrorStringAndDoesNotFallBackToLegacy` | `GpuDagCudaDevelopTest` | PASS |
| `EditorRenderFailureForwardsExceptionMessageInsteadOfEmptyResult` | `PipelineSchedulerRequestIdTest` | PASS |
| `ClearAllIntermediateBuffersReleasesCudaProductSessionGpuAndHostCaches` | `GpuDagCudaDrtProductTest` | PASS |
| `ThreeSequentialImagePinsDoNotRetainPreviousImageGpuTextures` | `GpuDagCudaDrtProductTest` | PASS |
| `TexturePoolBudgetNoLongerHardCodedTo64MiBOnProductPath` | `GpuDagCudaDrtProductTest` | PASS |
| `ImageSwitchBackAfterReleaseSessionCachesMissesAndReexecutes` | `GpuDagCudaDrtProductTest` | PASS |
| `ViewportChangeAfterSessionReleaseStillReusesSensorLinearOnTheLivePipeline` | `GpuDagCudaDrtProductTest` | PASS |
| `DrtInputIsNotHardClampedToForwardLimitBeforeTonescale` | `GpuDagCudaDrtProductTest` | PASS |
| `HighlightReconstructOnSurvivesIntoDrtInsteadOfBeingClippedToUnitCube` | `GpuDagCudaDrtProductTest` | PASS |
| `OpenDrtAndAcesStillLimitDisplayReferredOutput` | `GpuDagCudaDrtProductTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagCudaDevelopTest GpuDagCudaDrtProductTest GpuDagCudaWorkspaceTest PipelineSchedulerRequestIdTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "GpuDagCudaDevelopTest|GpuDagCudaDrtProductTest|GpuDagCudaWorkspaceTest|PipelineSchedulerRequestIdTest"
```

Suite totals: `67/67` PASS.

**Checklist / exit condition:** all G7R.H work items checked.

**LOC note (grill-code-review):** `cuda_sensor_demosaic.cpp` 252, `cuda_sensor_demosaic.hpp` 48, `cuda_develop_pass.cpp` 152, `cuda_render_device.cpp` 40, `cuda_product_renderer.cu` 212. No new file exceeds 1000 lines.

**Residual gaps:** `SavePipeline` / `HandleEviction` themselves were not driven through `PipelineMgmtService` in this binary (DuckDB + LibRaw winsock clash). They call `ClearAllIntermediateBuffers`, which calls `ReleaseSessionCaches` (proven). Real Fuji/Sony files in the editor still need a product pass. CAT02 remains G7R.4. 41.9 A/B remains G7R.5.

##### Phase G7R.H one-shot cache isolation correction (2026-08-23)

**Status:** complete — thumbnail/export 继续复用同一个 `CPUPipelineExecutor` 和
`PipelineDocument`，但不读取、发布或清空 editor session cache。

**Root cause:** CUDA DAG product path 没有读取 `CPUPipelineExecutor::enable_cache_`。
`THUMBNAIL` / `FULL_RES_EXPORT` 虽然调用 `SetEnableCache(false)`，仍写入
`CudaProductRenderer` 的 PreparedSource、static plan 和 GPU result cache。thumbnail 完成后
`ResetThumbnailRenderParams` 又调用 `ClearAllIntermediateBuffers`，进而执行
`ReleaseSessionCaches`。filmstrip 的后台 thumbnail 因此会清空同一 pipeline 的 editor cache，
下一次 ImageSwitch 偶发承担 LibRaw、H2D 和所有 CUDA pass 的冷启动成本。export 不执行这次
清理，反而会把 one-shot full-resolution 结果留在 editor cache。

**Primary success call chain:**

```text
filmstrip ImageSwitch editor preview
  -> PipelineTask::SetExecutorRenderParams -> SetEnableCache(true)
  -> CPUPipelineExecutor::Apply
  -> CudaProductRenderer::Render(UseSessionCache)
  -> PreparedSourceCache / StaticExecutionPlanCache / GraphImageCache hit
  -> no LibRaw, no H2D, no CUDA node pass on unchanged matching content

thumbnail or export
  -> PipelineTask::SetExecutorRenderParams -> SetEnableCache(false)
  -> CPUPipelineExecutor::Apply
  -> CudaProductRenderer::Render(BypassSessionCache)
  -> direct prepare + static compile + isolated one-shot CudaRenderDevice/workspace
  -> present or host download
  -> discard unpublished results + wait idle + release one-shot GPU result resources
  -> restore one-shot render params; editor session cache remains unchanged
```

**Primary failure call chain:**

```text
one-shot unpack / compile / CUDA execute / present fails
  -> CudaRenderDevice::CancelRender or renderer discards unpublished results
  -> exception reaches PipelineScheduler
  -> on_complete_(false, what()) / blocking promise exception
  -> editor session cache remains owned only by the session render lane
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OneShotRenderDoesNotReadWriteOrClearEditorSessionCaches` | `GpuDagCudaDrtProductTest` | PASS, repeated 3/3 |
| `ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults` | `GpuDagCudaDrtProductTest` | PASS |
| CUDA product cache/develop/DRT regression suite | `GpuDagCudaDrtProductTest` | 32/32 PASS |
| scheduler request identity, cache reuse and error propagation suite | `PipelineSchedulerRequestIdTest` | 7/7 PASS |
| one-shot pipeline/sink state suite | `PipelineFrameSinkTest` | 31/32 initial; isolated allocator-address assertion 3/3 PASS on rerun |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagCudaDrtProductTest PipelineSchedulerRequestIdTest PipelineFrameSinkTest --parallel 4
build\debug\alcedo_studio\tests\edit\GpuDagCudaDrtProductTest_runtime\GpuDagCudaDrtProductTest.exe
build\debug\alcedo_studio\tests\edit\PipelineSchedulerRequestIdTest_runtime\PipelineSchedulerRequestIdTest.exe
build\debug\alcedo_studio\tests\edit\PipelineFrameSinkTest_runtime\PipelineFrameSinkTest.exe
build\debug\alcedo_studio\tests\edit\PipelineFrameSinkTest_runtime\PipelineFrameSinkTest.exe --gtest_filter=PipelineFrameSinkTest.ReattachingFrameSinkPreservesMergedStage --gtest_repeat=3
```

**Checklist / exit condition:** one-shot work cannot observe or mutate editor session result caches；
thumbnail completion no longer clears those caches；matching ImageSwitch still proves zero LibRaw and
zero CUDA node execution。

**LOC note (grill-code-review):** `cuda_product_renderer.hpp` 126，
`cuda_product_renderer.cu` 258，`pipeline_cpu.cpp` 893，`pipeline_scheduler.cpp` 751，
`cuda_result_cache_test.cpp` 437。没有文件超过 1000 行；本修复没有增加新模块或新依赖。

**Residual gaps:** 尚未在当前自动化测试中重放用户的真实 filmstrip trace，因此 12.6 s 尖峰的
产品侧消失仍需真实 RAW 手动验证。首次完整 `PipelineFrameSinkTest` 中
`ReattachingFrameSinkPreservesMergedStage` 因 allocator 复用同一地址失败一次；单测连续复跑
3/3 通过。该断言用对象地址判断重建，属于独立的测试稳定性问题，本修复未修改它。

##### Phase G7R.H filmstrip 争用、请求几何与 scene-linear 高光修正（2026-08-23）

**Status:** complete — 更正上一条记录对 12.6 s 尖峰的解释；one-shot cache 隔离是必要的，
但不是全部根因。普通 thumbnail 和 export 仍曾复用 live editor executor，因此后台完整渲染
会占用同一把 `render_lock_`。该等待发生在 scheduler worker 内，计入 `[RENDER_E2E] pipeline`
而不是 `queue`；`queue≈50 ms, pipeline≈12.5 s` 与锁等待完全一致，不是冷启动结论。

同时修复两个独立产品回归：Quality Base 在 ROI 放大期间重新读取 live viewport，导致 ROI
结果按 full-frame 展示而拉伸；默认 Curve 对工作空间中超过控制点范围的数值返回末端控制点，
造成 ACES 和 OpenDRT 共同输入在 DRT 前硬裁。旧管线的 RGC 曲线没有执行该硬裁，因此默认
Curve 改为沿首尾线段外推。这里此前把 Primary Grade 记为 AP1 scene-linear 是错误的；
第 41.5.1 节记录工作空间边界和 LLF 修正。

**Primary success call chains:**

```text
filmstrip thumbnail / export
  -> PipelineMgmtService::LoadPipelineSnapshot
       -> briefly copy current params under live render_lock_
       -> build independent CPUPipelineExecutor + independent render_lock_
  -> PipelineScheduler renders on snapshot executor with BypassSessionCache
  -> ReleasePipelineSnapshot clears only snapshot resources
  -> live editor pin, dirty state, executor and session caches remain untouched

ROI zoom -> panel/demosaic change -> Quality Base
  -> request captures optional ViewportRenderRegion
  -> PipelineTask::SetExecutorRenderParams
       -> FAST/DETAIL stores the frozen ROI
       -> Quality Base stores explicit null/full-frame geometry
  -> CPUPipelineExecutor::Apply builds RenderRequest only from frozen task geometry
  -> full-frame result keeps source aspect ratio

scene-linear highlight > 1
  -> CameraColorPass converts camera RGB to AP1 and encodes ACEScc
  -> PrimaryGrade reads and writes AP1/ACEScc; default Curve extrapolates its identity end segment
  -> encoded highlight remains available at grade output
  -> selected DRT decodes ACEScc and owns highlight mapping
```

**Primary failure call chain:**

```text
snapshot capture / RAW metadata inject / thumbnail or export render fails
  -> explicit error or blocking promise exception
  -> ReleasePipelineSnapshot clears isolated resources
  -> live editor executor and caches remain valid
  -> no Legacy/backend/quality substitute
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `OrdinaryThumbnailRendersWithoutUsingLiveEditorExecutor` | `ThumbnailServiceTest` | PASS, real DNG |
| `AnalysisRenditionRendersWithoutSavePipelineOnLiveGuard` | `ThumbnailServiceTest` | PASS, real DNG |
| `LoadPipelineSnapshotClonesParamsAndDoesNotTouchLiveGuard` | `PipelineMapperTest` | PASS, independent executor and mutex |
| `QualityBaseAfterRoiClearsFrozenViewportGeometry` | `PipelineFrameSinkTest` | PASS |
| `DetailRoiPreviewUsesFrozenRequestRegionInsteadOfChangedSinkRegion` | `PipelineFrameSinkTest` | PASS |
| `DefaultCurvePreservesSceneLinearHighlightsAboveOne` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `DrtInputIsNotHardClampedToForwardLimitBeforeTonescale` | `GpuDagCudaDrtProductTest` | PASS |
| `CudaDevelopDefaultXTransUsesNeuralEngine` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopExplicitNeuralEngineChangesBayerPixelsVersusLegacy` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopHighlightReconstructOnSkipsCfaClamp01ForBayerAndXTrans` | `GpuDagCudaDevelopTest` | PASS |
| `CudaDevelopNeuralEngineFailureThrowsErrorStringAndDoesNotFallBackToLegacy` | `GpuDagCudaDevelopTest` | PASS |
| export snapshot path writes readable SDR and Ultra HDR files | `ExportServiceTest` | 2/2 PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target PipelineFrameSinkTest ThumbnailServiceTest ExportServiceTest PipelineMapperTest GpuDagCudaDevelopTest GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest EditorRawDecodePanelQmlTest EditorSessionRenderSchedulerPortTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "^(PipelineFrameSinkTest|GpuDagCudaPrimaryGradeTest|GpuDagCudaDrtProductTest|PipelineMapperTest|ExportServiceTest|EditorRawDecodePanelQmlTest|EditorSessionRenderSchedulerPortTest)\."
build\debug\alcedo_studio\tests\app\ThumbnailServiceTest_runtime\ThumbnailServiceTest.exe --gtest_filter=ThumbnailServiceTests.OrdinaryThumbnailRendersWithoutUsingLiveEditorExecutor:ThumbnailServiceTests.AnalysisRenditionRendersWithoutSavePipelineOnLiveGuard
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagCudaDevelopTest|PipelineMapperTest)\."
```

Suite totals: combined discovered set `123/123` PASS（4 disabled）；RAW Develop + latest pipeline
snapshot set `39/39` PASS（2 disabled）；direct real-DNG thumbnail set `2/2` PASS。

**Checklist / exit condition:** ordinary thumbnail and export no longer render on the live editor
executor；one-shot tasks still bypass session caches；Quality Base cannot inherit a live ROI；default
Primary Grade cannot clip AP1/ACEScc working values；RAW demosaic/HLR selection and failures remain
observable。

**LOC note (grill-code-review):** no new production module or dependency. The fix reuses
`PipelineSnapshot` and adds one request-scoped optional viewport value. Existing large files remain
`thumbnail_service.cpp` 1190 lines and `thumbnail_service_test.cpp` 3215 lines；this change does not
split them because the snapshot lifecycle already belongs to the existing service path。

**Residual gaps:** automated tests prove ownership, cache isolation, request geometry and pixel
range, but do not replay the user's exact multi-photo filmstrip timing trace. Product validation
should confirm that any remaining long thumbnail duration is visible only on the thumbnail worker，
not inside an InteractivePrimary request's `pipeline` duration。

##### 41.5.1 Phase G7R.H AP1/ACEScc 工作空间与 Shadows/Highlights LLF 修正（2026-08-23）

**Status:** complete — 此项修正撤销了上一条 completion record 中“Primary Grade 为 AP1
scene-linear”的错误语义。图级工作空间现在由 CameraColorPass 建立：CameraColorPass 在 Geometry
之后把 camera scene-linear 转换到 AP1 primaries，再编码为 ACEScc；所有 Grade 输入和输出继续
保持 AP1/ACEScc；DRT 端点负责解码 ACEScc，然后执行 ACES 2.0 或 OpenDRT，并转换到用户选择的
输出 encoding space、EOTF 和 limiting space。Primary Grade 不再自行执行 Camera→AP1，也不在
节点边界做 ACEScc 往返。

Shadows 和 Highlights 已从 pointwise adjustment kernel 移除，改为真正的 CUDA multi-level
Gaussian/Laplacian local Laplacian filter。LLF 从 AP1/ACEScc 解码亮度，构建 reference/remap
pyramids，按相邻 gamma samples 选择 Laplacian 层并 collapse，最后把局部亮度差重新应用到
AP1，做 lower-gamut fit 后编码回 ACEScc。所有 pyramid buffer 都由 `CudaRenderWorkspace`
持有；LLF 不调用私有 `cudaMalloc/cudaFree`，warm render 不产生 GPU allocation/free。

**Primary success call chains:**

```text
Develop sensor pass -> camera scene-linear
  -> GeometryResamplePass -> geometry.scene_source (camera scene-linear)
  -> CameraColorKernel -> camera_to_ap1 -> ACESccEncode
  -> publish develop.image (AP1 primaries / ACEScc encoded)
  -> PrimaryGrade commands before Shadows/Highlights (AP1/ACEScc)
  -> ExecuteCudaLocalTone
       -> Extract ACEScc log intensity
       -> Gaussian source pyramid
       -> sampled remap pyramids
       -> Laplacian select + collapse
       -> apply AP1 intensity delta + ACESccEncode
  -> PrimaryGrade commands after Shadows/Highlights (AP1/ACEScc)
  -> grade mix/mask in AP1/ACEScc
  -> publish grade.<id>.image (AP1 primaries / ACEScc encoded)
  -> DrtKernel -> ACESccDecode -> selected DRT -> selected display encoding
```

**Primary failure call chain:**

```text
missing workspace image / invalid CUDA allocation / kernel launch failure
  -> explicit exception from CameraColor, PrimaryGrade, LLF or DRT pass
  -> CudaRenderDevice cancels submission and discards unpublished results
  -> no CPU, legacy curve, alternate backend or lower-quality substitute
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `CameraColorEncodesAp1AsAcesccGraphWorkingSpace` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `DefaultCurvePreservesAcesccWorkingValuesWithoutClipping` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaLocalToneUsesWorkspaceInsteadOfPrivateAllocation` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaColorGradeSecondRenderCreatesNoGpuAllocation` | `GpuDagCudaPrimaryGradeTest` | PASS |
| Shadows/Highlights CUDA memcheck | `compute-sanitizer` | PASS, 0 errors |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest GpuDagCudaDevelopTest GpuDagRawInputTest GpuDagModelGraphTest GpuDagCudaMaskTest GpuDagCudaWorkspaceTest GpuDagGeometryTest GpuDagCudaGeometryTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagCudaPrimaryGradeTest|GpuDagCudaDrtProductTest|GpuDagCudaDevelopTest|GpuDagRawInputTest|GpuDagModelGraphTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest|GpuDagGeometryTest|GpuDagCudaGeometryTest)\."
compute-sanitizer --tool memcheck --print-limit 5 build\debug\alcedo_studio\tests\edit\GpuDagCudaPrimaryGradeTest_runtime\GpuDagCudaPrimaryGradeTest.exe --gtest_filter=GpuDagCudaPrimaryGrade.ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel:GpuDagCudaPrimaryGrade.HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel
```

Suite total: combined related set `155/155` PASS；post-format Primary Grade + DRT product set
`46/46` PASS；compute-sanitizer targeted set `2/2` PASS with `ERROR SUMMARY: 0 errors`。

**LOC note (grill-code-review):** `cuda_local_tone_pass.cu` 361 lines，
`cuda_primary_grade_pass.cu` 444 lines，`cuda_primary_grade_test.cpp` 364 lines。没有本次修改的
文件超过 1000 行；没有新增第三方依赖。

**Residual gap:** 本次修复使用当前 post-Geometry render extent 构建 LLF reference，并复用
workspace pyramid allocations；full-frame、跨 ROI 的 reference content cache 仍需沿
`MakeLlfSamplingPlan` 接入独立的 canonical reference 输入。该项不能用 pointwise curve 或固定
标量代替，且不影响本次已证明的 LLF 空间行为和 AP1/ACEScc 工作空间边界。

##### 41.5.1 默认 DAG 算法分派与 DNG WarpRectilinear 修正（2026-08-23）

**Status:** complete — 修正范围是默认文档经过 `GraphCompiler` 和 legacy/UI stage adapter 后的
实际算法选择，以及 encoded DNG 的 OpcodeList3 `WarpRectilinear` 进入 CUDA Develop 的路径。

默认文档虽然一直包含独立的 Shadows、Highlights 和 Curve type id，但旧的 compiled
adjustment 没有记录执行算法，CUDA Primary Grade 因而曾把 Shadows/Highlights 当作 pointwise
亮度曲线执行。`CompiledAdjustment` 现在显式记录 `Pointwise` 或 `LocalLaplacian`；compiler 只把
Shadows/Highlights 标记为 `LocalLaplacian`，Curve 继续保持 `Pointwise`。Primary Grade 对任何没有
被编译为 LLF 的 Shadows/Highlights 直接报错，不会静默执行 curve、CPU 或其他替代路径。

`RawInputLoader` 现在从 encoded DNG 解析 OpcodeList3 warp，保存到 `PreparedRawInput`，并把完整
系数与中心点 hash 纳入 prepared source identity。CUDA Develop 在 demosaic、HLR 和 RGBA pack
之后、GeometryResample 和 CameraColor/ACEScc 之前，把 warp 写入 workspace-owned 目标图像；
caller-owned warp API 不分配私有 GPU 图像。缺少图像、类型/尺寸不匹配或 CUDA kernel 失败均直接
抛错。

**Primary success call chains:**

```text
CreateDefaultPipelineDocument / LegacyPipelineImporter
  -> GraphCompiler::CompileStatic
  -> Shadows + Highlights => CompiledAdjustmentAlgorithm::LocalLaplacian
  -> Curve => CompiledAdjustmentAlgorithm::Pointwise
  -> ExecuteCudaPrimaryGrade
  -> ExecuteCudaLocalTone -> workspace Gaussian/Laplacian pyramids
  -> AP1/ACEScc grade output -> DRT
```

```text
encoded DNG bytes
  -> RawInputLoader::LoadEncoded -> ExtractMetadata(OpcodeList3 WarpRectilinear)
  -> PreparedRawInput + dng_warp_hash
  -> ExecuteCudaDevelop -> demosaic/HLR/pack
  -> CUDA::WarpDngRectilinear(unwarped, sensor_linear, coefficients)
  -> GeometryResample -> CameraColor -> AP1/ACEScc working space
```

**Primary failure call chains:**

```text
Shadows/Highlights compiled without LocalLaplacian
  -> ExecuteCudaPrimaryGrade throws
  -> CudaRenderDevice cancels the unpublished submission
  -> no curve or backend substitute

invalid/missing DNG warp image or CUDA launch failure
  -> WarpDngRectilinear throws
  -> Develop submission is cancelled
  -> no unwarped result is published as success
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `DefaultPipelineCompilesShadowsAndHighlightsToLocalLaplacianOnly` | `GpuDagRawInputTest` | PASS |
| `LegacyShadowControlExecutesLocalLaplacianWorkspacePath` | `GpuDagCudaDrtProductTest` | PASS |
| `ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `EncodedDngCarriesOpcodeList3WarpIntoPreparedInput` | `GpuDagRawInputTest` | PASS with real DNG |
| `CudaDevelopAppliesPreparedDngRectilinearWarpAfterDemosaic` | `GpuDagCudaDevelopTest` | PASS |
| LLF CUDA memcheck | `compute-sanitizer` | PASS, 0 errors |
| DNG warp CUDA memcheck | `compute-sanitizer` | PASS, 0 errors |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagCudaPrimaryGradeTest GpuDagCudaDrtProductTest GpuDagCudaDevelopTest GpuDagRawInputTest GpuDagModelGraphTest GpuDagCudaMaskTest GpuDagCudaWorkspaceTest GpuDagGeometryTest GpuDagCudaGeometryTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagCudaPrimaryGradeTest|GpuDagCudaDrtProductTest|GpuDagCudaDevelopTest|GpuDagRawInputTest|GpuDagModelGraphTest|GpuDagCudaMaskTest|GpuDagCudaWorkspaceTest|GpuDagGeometryTest|GpuDagCudaGeometryTest)\."
compute-sanitizer --tool memcheck --print-limit 5 build\debug\alcedo_studio\tests\edit\GpuDagCudaPrimaryGradeTest_runtime\GpuDagCudaPrimaryGradeTest.exe --gtest_filter=GpuDagCudaPrimaryGrade.ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel:GpuDagCudaPrimaryGrade.HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel
compute-sanitizer --tool memcheck --print-limit 5 build\debug\alcedo_studio\tests\edit\GpuDagCudaDevelopTest_runtime\GpuDagCudaDevelopTest.exe --gtest_filter=CudaDevelopFixture.CudaDevelopAppliesPreparedDngRectilinearWarpAfterDemosaic
```

Suite total: related discovered set `159/159` PASS；LLF memcheck `2/2` PASS；DNG warp memcheck
`1/1` PASS；两次 sanitizer 均为 `ERROR SUMMARY: 0 errors`。

**LOC note (grill-code-review):** `cuda_local_tone_pass.cu` 360 lines，
`cuda_primary_grade_pass.cu` 452 lines，`raw_input_loader.cpp` 495 lines，
`cuda_develop_pass.cpp` 169 lines，`cuda_dng_warp.cu` 229 lines。没有本次修改的文件超过 1000 行。

**Residual gaps:** full-frame、跨 ROI 的 canonical LLF reference 仍是上一条记录明确列出的独立
剩余项；本次没有用当前 ROI 缓存冒充 canonical reference。DNG 测试证明真实文件的 warp 元数据
进入 prepared input，并以合成非恒等参数证明 CUDA 像素结果实际改变；没有保存新的大型 golden
图像。

##### 41.5.1 Canonical LLF reference 与 ROI 坐标采样（2026-08-23）

**Status:** complete — CUDA Shadows/Highlights 不再从当前 viewport ROI 重建内部蒙版。
全 EditSpace 帧按 `full_reference_extent` 把 log-intensity 提取到 ReferenceSpace 的
canonical LLF 平面（长边不超过 `kReferenceMaskMaxLongEdge`），并写入 workspace pyramid。
后续 ROI 帧用 G3 `MakeLlfSamplingPlan` 把 render 像素中心映射到该平面，只应用已有
reference/adjusted 亮度，不重跑 Gaussian/Laplacian。`HashLlfReferenceKey` 含 source、
crop/rotation、CameraColor 和 Grade，不含 viewport。没有 canonical 缓存的 ROI 仍从
当前帧局部构建，且不会把局部结果标成 canonical。

**Primary success call chain:**

```text
full-EditSpace PrimaryGrade
  -> ExecuteCudaLocalTone
  -> ExtractReferenceKernel (reference_to_render)
  -> workspace local_tone.source/result pyramids
  -> ApplyKernel + MakeLlfSamplingPlan
viewport ROI PrimaryGrade (same HashLlfReferenceKey)
  -> ExecuteCudaLocalTone
  -> skip extract/remap/select/collapse
  -> ApplyKernel samples canonical planes by render_to_texture_uv
```

**Primary failure call chain:**

```text
missing input / zero geometry extent / CUDA launch failure
  -> ExecuteCudaLocalTone throws
  -> CudaRenderDevice cancels unpublished submission
  -> no CPU, curve, or other-backend substitute
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `RoiFrameSamplesCanonicalLlfReferenceInsteadOfRebuilding` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `CudaLocalTonePyramidBuffersReuseAcrossViewportChanges` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `FullViewCoversEditSpaceAndViewportRoiDoesNot` | `GpuDagGeometryTest` | PASS |
| `LlfReferenceKeyIgnoresViewportAndFollowsCropAndGrade` | `GpuDagRawInputTest` | PASS |
| `ShadowsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |
| `HighlightsLlfRespondsToNeighborhoodWithIdenticalCenterPixel` | `GpuDagCudaPrimaryGradeTest` | PASS |

Commands:

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target GpuDagCudaPrimaryGradeTest GpuDagGeometryTest GpuDagRawInputTest GpuDagCudaMaskTest GpuDagCudaDrtProductTest GpuDagCudaDevelopTest --parallel 4
ctest --test-dir build/debug --output-on-failure -R "^(GpuDagCudaPrimaryGradeTest|GpuDagGeometryTest|GpuDagRawInputTest|GpuDagCudaMaskTest|GpuDagCudaDrtProductTest|GpuDagCudaDevelopTest)\."
```

Suite total: `119/119` PASS。

**Checklist / exit condition:** G3 “图像、LLF 和 mask 使用相同的 render-to-reference 数据” 已接到 CUDA LLF apply。ROI 放大不再重算 canonical 蒙版。

**LOC note (grill-code-review):** `cuda_local_tone_pass.cu` 488 lines，`cuda_primary_grade_pass.cu` 454 lines，`cuda_primary_grade_test.cpp` 490 lines。没有本次修改的文件超过 1000 行。

**Residual gaps:** 没有先行全图帧时，孤立 ROI 仍走局部 extract，不会伪造 canonical 缓存。canonical 身份的主机侧槽以 workspace 指针为键，GPU 平面仍由 workspace `Values()` 持有；`ReleaseSessionResources` 清掉平面后下一次全图帧会重新播种。G7R.4 CAT02 与 G7R.5 统计仍未做。

### 41.6 G7R.4 — 正确实现独立的 creative CAT02

Primary Grade 的第一个 adjustment 是 creative white balance，与 RAW
CameraColorPass 是两个不同功能：

- 节点输入和输出都保持 AP1 primaries / ACEScc encoded；需要 scene-linear 运算时只在该
  adjustment 内部解码并重新编码，不能改变图的工作空间；
- temperature/tint offset 先解析成源/目标白点，不直接变成 RGB 通道指数倍率；
- 使用 CAT02 cone response 矩阵、白点 LMS 比例和逆 CAT02 完成色适配；
- mix 在适配后以原 AP1 像素和适配像素插值；
- zero offset 必须是严格 identity；
- 参数 dirty 只让当前 Grade 和 DRT 失效，不让 CameraColor、Geometry 或 SensorDevelop 失效。

### 41.7 G7R.5 — 可观测性与性能恢复

为测试和 profiling 增加每个 pipeline session 的只读统计快照：

```text
prepared source cache hit/miss
LibRaw open/unpack count
static plan cache hit/miss/compile count
source H2D copy count and bytes
parameter H2D ranges and bytes
per-pass execute/skip count
result cache hit/miss by GraphValueId
GPU allocation/free count and bytes
CPU prepare/compile/submit/present duration
GPU pass and total submission duration
renderer completed/aborted/failed count
```

统计默认关闭或使用低成本原子计数；测试构建可开启细粒度计时。性能验收不能依赖日志文本。

### 41.8 必须新增或加强的测试

#### 41.8.1 RAW metadata 与颜色单元测试

```text
RawInputLoaderPopulatesCompleteColorContextFromRealRaw
DevelopColorTransformInterpolatesDualIlluminantMatricesInMiredSpace
DevelopColorTransformInterpolatesForwardMatricesWithTheSameWeight
DevelopColorTransformUsesSingleProfileWhenCalibrationIlluminantsAreIncomplete
DevelopColorTransformSolvesAsShotNeutralWithoutUsingCamMulAsColorMatrix
DevelopColorTransformDoesNotUseLibRawRgbCamOrPreMulAsCameraMatrix
DevelopColorTransformRejectsMissingOrSingularCameraMatrices
CameraColorEncodesAp1AsAcesccGraphWorkingSpace
CudaCat02ZeroOffsetIsExactIdentity
CudaCat02MapsSourceWhiteToRequestedWhiteInAp1
```

`DevelopColorTransformRejectsMissingOrSingularCameraMatrices` 必须断言明确错误，不能接受 identity
输出。双光源测试使用非对角矩阵，使 `cam_mul` 对角实现、`rgb_cam` 固定矩阵实现和错误的
线性 Kelvin 插值都必然失败。

#### 41.8.2 真实 RAW reference 测试

使用 `alcedo_studio/tests/resources/sample_images/ci_rawfiles` 中至少一个 DNG 和一个非 DNG RAW。
reference 来自重构前提交 `bf6686fb` 的旧 CameraMatrices/ColorTemp 路径，在固定输入、
DecodeRes、geometry 和 CCT/tint 下生成。保存可审查的浮点 reference 或确定性统计，不以肉眼
截图作为唯一断言。

```text
CudaRealRawAsShotCameraToAp1MatchesPreRebuildReference
CudaRealRawCustomCctCameraToAp1MatchesPreRebuildReference
CudaRealRawCctEndpointsMatchCameraProfileMatrices
CudaRealRawColorTransformProducesFiniteAp1WithoutChannelCollapse
```

误差范围必须在 fixture 中按阶段说明。至少断言每通道有限值、非零动态范围、参考白点、色卡或
固定 patch 的 RGB/xy 误差；不能只断言“图像存在”。

#### 41.8.3 缓存和最小执行集测试

```text
SecondUnchangedProductRenderRunsNoLibRawNoSourceUploadAndNoGpuNodePass
ExposureEditRunsOnlyPrimaryGradeAndDrtPasses
DevelopCctEditReusesSensorAndGeometryAndRunsCameraColorGradeDrt
DrtEditRunsOnlyDrtPass
ViewportChangeReusesSensorDevelopAndRunsGeometryAndDownstream
GeometryEditReusesSensorDevelopAndInvalidatesPostGeometryResult
RawDevelopEditInvalidatesSensorDevelopAndAllDownstreamResults
ProductRendererCompilesStaticPlanOnlyForTopologyOrSourceLayoutChange
ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit
FailedSubmissionDoesNotPublishResultContentKey
CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable
ImageSwitchBackReusesMatchingPreparedSourceAndGpuResults
```

每个测试断言内容 key、execute/skip counter、LibRaw count、H2D bytes 和输出像素；禁止只比较
`ResourceId` 或 malloc/free。

#### 41.8.4 编辑器真实 RAW E2E

```text
RealRawEditorColorTempEditChangesPixelsWithoutRerunningDemosaic
RealRawEditorExposureEditKeepsPreparedSensorAndGeometryResults
```

这些测试必须进入默认 CUDA CI，不得依赖 `ALCEDO_RUN_DEADLOCKING_RAW_GPU_E2E` 才执行。现有
deadlock teardown 问题需要拆成单独回归并修复，不能用跳过整个真实 RAW 行为测试来规避。

### 41.9 重构前性能 A/B 验收

基线固定为重构前提交 `bf6686fb`。在同一台机器、同一 CUDA driver/GPU、同一 MSVC/CMake
配置、同一输入 RAW、同一 DecodeRes、同一 viewport/输出尺寸和同一编辑序列下比较。Debug
用于行为测试；性能结论使用同一份 Release 构建配置。

每个场景执行一次冷启动和至少 30 次 warm render，报告 median、p95、CPU 准备时间、GPU
时间、LibRaw 次数、H2D 字节和各 pass 次数。首帧单独报告，不能用首帧或缓存预热隐藏 warm
回归。

| 场景 | G7R 的强制执行集 | 强制为零的工作 |
| --- | --- | --- |
| 无变化重复渲染 | 查找并复用已完成结果，沿现有输出路径提交 | LibRaw、source H2D、plan compile、所有图像节点 pass、GPU alloc/free |
| Exposure 连续调整 | PrimaryGrade、DRT | LibRaw、source H2D、SensorDevelop、Geometry、CameraColor、GPU alloc/free |
| RAW CCT/tint 连续调整 | CameraColor、PrimaryGrade、DRT | LibRaw、source H2D、SensorDevelop、Geometry、GPU alloc/free |
| DRT 连续调整 | DRT | LibRaw、source H2D、SensorDevelop、Geometry、CameraColor、PrimaryGrade、GPU alloc/free |
| viewport/geometry 改变 | Geometry、CameraColor、PrimaryGrade、DRT | LibRaw、source H2D、SensorDevelop、GPU alloc/free |
| RAW Develop 参数改变 | SensorDevelop 及下游 | 重复 LibRaw open/unpack、无关 source preparation、GPU alloc/free |
| 切换图像后切回 | 命中仍在预算内的 source/GPU 结果，沿现有输出路径提交 | 对命中项的 LibRaw、source H2D 和图像节点 pass |

量化完成条件：

- 每个场景都满足上表的 pass/H2D/LibRaw 硬限制；任何一次多余重算都算失败；
- warm interactive median 不高于 `bf6686fb` 基线的 `1.05x`，p95 不高于 `1.10x`；
- QualityBase、DetailPatch 和切图返回场景的 median/p95 不比基线差 10%；
- 无变化、Exposure、CCT 和 DRT 连续编辑从第二帧开始 GPU allocation/free 均为 0；
- 若新路径优于基线，记录绝对时间与提升比例；若未达到阈值，G7R 不得标记 complete；
- 性能报告写入本 Phase completion record，包含 GPU、driver、CPU、构建类型、fixture 和命令。

### 41.10 完成条件

- 用户可见图仍然只有 Develop、Primary Color Grade 和 DRT；
- G4/G5 的缓存拆分保留，并存在独立 `develop.sensor_linear`、`geometry.scene_source`、
  `develop.image`；
- `develop.image` 被 reference 测试证明是 AP1 primaries / ACEScc encoded；
- Camera→AP1 使用 CameraMatrices/DNG 双光源插值，`cam_mul/pre_mul` 不作为颜色矩阵；
- as-shot/custom CCT/tint 和 creative CAT02 分别通过数学单元测试和真实 RAW reference 测试；
- CUDA 产品路径不再包含 legacy 参数镜像、stage adapter 或 `LegacyPipelineImporter`；
- 缓存命中由内容 key 和 pass counter 证明，不由资源 ID 推断；
- 第 41.8 节测试全部默认执行并通过；
- 第 41.9 节 A/B 达标，并把完整测量写回 completion record；
- G7 completion record 中“upstream cache valid”的结论，在 G7R 完成前只视为历史记录，
  不作为 G8 的验收依据；
- G8 必须以 G7R 分支为 base，不得从当前 G7 直接开始 OpenCL 移植。

## 42. Phase G8 — OpenCL 移植

Branch: `feature/gpu-dag-opencl`

Base: `feature/gpu-dag-cuda-default-recovery`

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

## 43. Phase G9 — Metal 移植

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

## 44. Phase G10 — 最终删除与全平台验证

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
- LegacyPipelineImporter、legacy parameter snapshot 和 nested stage adapter；
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
NoLegacyParameterImporterOrStageAdapterRemainsInProductPath
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

## 45. 测试分层

### 45.1 Model 单元测试

- 默认值；
- 参数限制；
- JSON 读取和写入；
- dirty field mask；
- 连续修改合并；
- TakeDirtyPatch 与新修改并发；
- RestoreDirty；
- MakeFullDto。

### 45.2 Graph 单元测试

- 端口类型；
- cycle 检查；
- 拓扑顺序；
- 默认三节点；
- mask edge；
- topology dirty；
- 调整实例顺序。

### 45.3 Geometry property 测试

- forward/inverse round trip；
- pixel center；
- odd dimensions；
- arbitrary rotation；
- crop 和 viewport intersection；
- DecodeRes 不改变 ReferenceSpace；
- mask、LLF 和 image 映射一致。

### 45.4 Workspace 单元测试

- grow-only；
- live allocation 时不增长；
- scope rewind；
- stable parameter offsets；
- dirty range merge；
- resource lease；
- LRU byte budget；
- KV lookup；
- no allocation after reserve。

### 45.5 后端集成测试

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
- unsupported old-format error。

### 45.6 后端一致性测试

使用同一输入、PipelineDocument、RenderRequest 和 MaskAsset：

```text
CUDA reference
OpenCL output
Metal output
```

测试需要为每类行为定义清楚的误差范围。离散 mask 边缘和随机 grain 需要固定 seed。

### 45.7 性能和分配测试

- 首帧创建成本单独报告；
- 第二帧开始不得分配 GPU buffer 或 texture；
- 无变化重复渲染不得重新 open/unpack RAW、上传 source、编译静态 plan 或执行图像节点 pass；
- Exposure、RAW CCT/tint、DRT、viewport/geometry 编辑分别断言第 41.9 节的最小执行集；
- cache hit 必须由内容 key 和 pass skip counter 证明，不能由相同 `ResourceId` 推断；
- 无参数变化时参数上传字节数必须为 0；
- 单字段变化时只上传对应字段范围；
- viewport 变化不得重新上传不变的 R8 mask；
- dynamic resolution 变化不得重新生成不变的 feather resource；
- LLF reference 可以跨 viewport 变化复用；
- GPU 内存预算和 LRU 清理字节数可查询；
- CUDA 产品路径必须与重构前提交 `bf6686fb` 做同机 Release A/B，并满足第 41.9 节阈值。

## 46. 构建与运行

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

## 47. 全局完成条件

### 47.1 架构

- [ ] 默认 PipelineDocument 有且只有 Develop、Primary Color Grade 和 DRT 三个节点。
- [ ] Geometry 是全局 Model 和内部 Pass，不是第四个用户节点。
- [ ] MaskNode 是独立节点。
- [x] ColorGradeNode 有可选 mask 输入和单一 Normal Mix。
- [ ] PipelineStage 全部删除。
- [ ] OperatorParams 总结构全部删除。
- [ ] operators 只保存参数 Model、DTO 和序列化逻辑。
- [ ] GPU 执行代码不在 operators 参数目录。

### 47.2 参数

- [ ] 不使用参数计数器。
- [ ] 连续修改合并为一个 dirty Patch。
- [ ] 没有变化的参数不上传。
- [ ] 单字段变化不上传完整参数区。
- [ ] GPU workspace 重建时使用完整 DTO。

### 47.3 GPU 资源

- [ ] BasicRenderWorkspace 通过模板支持 CUDA、OpenCL 和 Metal。
- [ ] 每个 RenderDevice 只有一个 workspace。
- [ ] 每个 RenderDevice 只有一份 ParameterArena。
- [ ] 稳定渲染不创建或销毁 GPU buffer 和 texture。
- [x] Prepared RAW、静态 ExecutionPlan 和节点结果都有可查询的内容感知 cache hit/miss。
- [x] `develop.sensor_linear` 和 `geometry.scene_source` 是独立、可淘汰、submission-safe 的结果缓存。
- [x] 无变化渲染不执行 LibRaw、source H2D、plan compile 或 GPU 图像节点 pass。
- [x] LLF 不管理自己的内存池。
- [x] Mask GPU LRU 只存在于 workspace。

### 47.4 RAW 和颜色

- [ ] LibRaw unpack 在编辑管线之前完成。
- [ ] RAW DecodeRes 降采样在 CPU 输入准备中完成。
- [ ] Geometry 和 CameraColor 后的 Develop 图输出 AP1 primaries / ACEScc encoded RGB。
- [ ] Camera→AP1 使用 CameraMatrices/DNG 双光源矩阵和标定光源 CCT 在 mired 空间插值。
- [ ] `cam_mul/pre_mul` 只用于 RAW 归一化和 neutral 推导，不作为 Camera→AP1 矩阵。
- [ ] metadata 缺失或矩阵无效时返回明确错误，不静默使用 identity。
- [ ] RAW CameraColorPass 与 Primary Grade creative CAT02 分别拥有 dirty key 和测试。
- [ ] CAT02 调整以 AP1 白点为参考。
- [ ] DRT 从 AP1/ACEScc 工作空间解码后支持 ACES 2.0 和 OpenDRT，并转换到用户选择的
  encoding space / EOTF / limiting space。

### 47.5 Geometry 和 Mask

- [ ] crop、rotation、view ROI 和 dynamic resolution 只执行一次图像重采样。
- [x] LLF 和 mask 使用相同的 RenderGeometry 数据。
- [x] Rasterized Mask 使用 R8。
- [x] Rasterized Mask 任意一条边不大于 4096。
- [x] Rasterized Mask 作为 GPU texture 采样。
- [x] Rasterized Mask feather 使用 GPU exact signed Euclidean distance field。
- [x] 改变 feather radius 复用 signed distance texture。
- [x] view 或 render scale 变化不重新创建不变的 mask texture。
- [x] MaskStore root 可以由用户配置。

### 47.6 后端

- [ ] CUDA 完整通过。
- [ ] OpenCL 完整通过。
- [ ] Metal 完整通过。
- [ ] 三个后端使用同一 PipelineDocument、GraphCompiler、Model、DTO、Geometry 和
      workspace 模板。
- [ ] GPU 后端失败时不进入 CPU 图像处理。

### 47.7 数据

- [ ] 新 JSON 只保存节点、边、Model 和 MaskAssetKey。
- [ ] 新 JSON 不保存 GPU 或 viewport 短期状态。
- [ ] 旧 stage JSON 返回明确 unsupported-format error，不进入新产品路径。
- [ ] 不存在 `LegacyPipelineImporter`、legacy parameter snapshot 或 nested stage adapter。
- [ ] 新保存只写 format version 2，不写旧 stage JSON。

### 47.8 性能

- [ ] 切图返回只命中相同 source/document revision 的缓存。
- [ ] 真实 RAW 编辑器颜色和缓存测试默认进入 CUDA CI，不依赖环境变量才执行。
- [ ] G7R 与重构前提交 `bf6686fb` 的同机 A/B 满足第 41.9 节阈值。
- [ ] G7R 完成后才允许以其为 base 开始 G8。

## 48. 风险与处理

### 48.1 旧管线删除范围大

处理：

- 使用 Stacked PR；
- 每个 PR 保持可构建；
- CUDA 先完成端到端；
- OpenCL 和 Metal 各自独立移植；
- G7R 删除 legacy 参数 importer、snapshot 和 CUDA 产品 adapter；
- G8/G9 分别删除尚未移植后端的执行 adapter；G10 只做最终验证和剩余清理。

### 48.2 默认调色顺序变化

处理：

- 默认顺序写入明确列表；
- format version 2 直接保存和读取明确 Model 顺序；旧 stage JSON 返回版本错误；
- 使用 golden 图像和参数 round-trip 测试；
- GraphCompiler 不自动改变 Model 顺序。

### 48.3 ROI 与 mask 错位

处理：

- 只允许 RenderGeometryResolver 计算坐标；
- image、LLF 和 mask 共享同一结果；
- 使用 pixel-center property 测试；
- 使用 crop、rotation、odd size 和 dynamic resolution 组合测试。

### 48.4 GPU 内存增长

处理：

- workspace 使用明确字节预算；
- texture 使用 LRU；
- active submission 使用 lease；
- transient memory 在每帧完成后 rewind；
- 测试记录稳定状态分配次数和峰值。

### 48.5 Stacked PR 评审困难

处理：

- 每个 PR 描述列出完整堆栈；
- 每个 PR 只处理一个系统切片；
- 测试名称说明具体行为；
- 每个 Phase 完成后在本文件记录主要调用路径、测试和未完成项；
- 父 PR 变化后立即更新子分支，避免长时间分离。

## 49. Phase 完成记录格式

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
