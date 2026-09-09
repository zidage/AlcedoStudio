# NM7 — Parameterized Mask Commands, Regional Replay and Project Cache

Date: 2026-09-08

Status: NM7.2 parameterized Brush source and Grade owner operations landed; NM7.3 typed
stroke/placement history, WAL/Version/Paste remapping, and the Section 8.1 project/schema
version gate landed. NM7.4 host canonical rasterization, spatial index, and regional Mix
replay landed. Raster-only Brush JSON is rejected. NM7.5 shared ReferenceSpace mapping and
Brush placement landed. NM7.6 control-only retained QSG Mask overlay landed. NM7.7 Radial
and Linear creation plus existing-mask movement landed. Some former NM7.11 UI wiring was
brought forward for testing. New NM7.8 parameter-mask controls and drawer selection/deletion
remain planned. Brush UI, Interactive Mix and cache service are now NM7.9–NM7.11; remaining
full UI is NM7.12 and final qualification ends at NM7.15.

Parent: [NM7 execution plan](phase_nm7_viewer_mask_creation_plan.md).
This document defines NM7's revised algorithm, data ownership and storage behavior. It replaces
the earlier NM7 proposal to persist an immutable R8 asset for every completed stroke. Earlier
NM3/NM4 completion records remain historical implementation evidence, not the new product rule.

## 1. 用户目标与准确的可逆含义

用户要求已绘制 Brush、Radial、Linear Gradient 的移动实时改变照片，通过 Interactive preview
显示真实结果。QSG 负责控件，不用蒙版覆盖区域高亮代替照片预览。笔刷必须支持 paint/erase、
size/strength 修改、多次 stroke 累积；每个 Color Grade 用于 Mix 的当前 R8 结果只有一份。
蒙版缓存按项目设置路径和清理策略，删掉缓存不应破坏编辑、Undo/Redo 或 Version。

核心设计：

```text
project document + typed history
  Brush stroke commands / analytic parameters / placement
          ↓ deterministic evaluation
  one current Grade coverage R8 cache
          ↓ native Grade Mix
  actual Interactive / Quality photograph
```

“只缓存一次”解释为一个稳定的当前缓存槽，而不是终身只写一次文件。状态改变后要更新这个槽，
不能为每个 StrokeId、commit hash 或 Version 建立另一张长期 R8 图。

### 1.1 为什么不能直接对 R8 做无损逆运算

下面两种原始状态经过相同 paint 后得到同一个值：

```text
max(30, 200) = 200
max(90, 200) = 200
```

擦除至 0、clamp 和 8-bit 量化也都是多对一映射。从最终像素和一次操作无法判断原值是 30
还是 90。因此“擦除的 Undo 等于再画一次”“从结果减去该 stroke”“反向平移已经插值的 R8”
不能保证恢复原状态。乘法累积再做除法同样在 alpha=1、零值及量化处失败。

本方案使**文档命令可逆**：撤回笔画插入、恢复删除的笔画、恢复参数或 placement 的原值；
然后由恢复的参数重算受影响像素。这满足无每步栅格副本的 Undo/Redo，同时保留原有笔刷语义。
这不是承诺存在不可逆像素运算的代数逆函数。

## 2. 算法依据与方案选择

以下只引用实际查阅的一手资料；具体 Alcedo 算法是本节之后的设计推导，不声称照搬外部实现。

| 来源 | 查阅内容与用途 |
| --- | --- |
| [darktable 官方 drawn masks 文档](https://darktable-org.github.io/dtdocs/en/darkroom/masking-and-blending/masks/drawn/) | Brush 在抬笔后可表示为连接节点，并支持修改节点和笔画形状；证明参数化笔刷可用于照片局部调整。该软件的平滑/图形组合规则不作为 Alcedo 的像素定义。 |
| [Qt QUndoCommand](https://doc.qt.io/qt-6/qundocommand.html) | 文档操作的 redo/undo 和命令合并语义。只借鉴命令模式；继续扩展 NM4，不接入另一套 QUndoStack。 |
| [Felzenszwalb / Huttenlocher, Distance Transforms of Sampled Functions](https://cs.brown.edu/people/pfelzens/papers/dt-final.pdf) | 通过一维变换分解计算精确平方欧氏距离；用于理解和核对现有 Brush signed-distance feather 的计算边界，不替换为近似 blur。 |
| [Qt QSaveFile](https://doc.qt.io/qt-6/qsavefile.html) | 临时文件完成后替换目标、写失败保留旧文件；用于稳定缓存槽和项目设置保存。使用 Qt 6.9 可用 QString 路径重载。 |
| [Qt QLockFile](https://doc.qt.io/qt-6/qlockfile.html) | 写入者协作锁及长期持锁的 stale-time 注意事项；缓存更新、切换根路径和清理必须服从同一所有权。 |

不使用这些方案：

- 每次 stroke 保存一张完整 R8、按 hash 永久积累历史栅格。
- 每隔若干 stroke 自动产生长期 raster checkpoint 或 tile undo 图像。它们仍会随历史增长，
  且用户没有授权此类例外；现有文档参数 checkpoint 可保留。
- 为支持逆运算改变 paint/erase 混合规则，或对量化结果相减/相除。
- 只移动旧 coverage texture 来冒充准确重算，导致重复移动模糊或 Union 覆盖错误。
- 为追求“路径更短”自动用有损曲线拟合改变笔画。初版保留规范采样序列。

## 3. 参数化数据与单一所有者

以下类型/API 名是拟新增项；执行者先检查能否由现有 owner 表达，不增加另一份文档模型。

### 3.1 BrushMaskSource

将当前 `asset_key + descriptor` 的持久化 source 改为：

```text
BrushMaskSource (owned by ColorGradeNodeModel)
  source_format_version
  raster_algorithm_version
  reference metric / canonical raster descriptor policy
  placement_translation = (tx, ty)
  ordered strokes:
    StrokeId
    paint_or_erase
    immutable canonical samples:
      local_reference_x, local_reference_y
      radius_in_reference_pixels, strength, hardness
  existing source feather parameter
```

规范 samples 是原始创建数据，持续存储在项目 document/history 中，不是缓存。StrokeId 在
其生命周期内稳定；顺序是计算语义，不能按空间索引或 ID 重排。一次 stroke 只保留一份规范
sample body；owner 和待提交历史可按现有不可变共享表示持有它，不能每帧复制整个 strokes 列表。

UI 中仍是一个累积 Brush Mask；不为每个 stroke 添加 Mask 行。增加内部 StrokeId 不要求本期
再做逐笔画节点编辑器。NM7 必须支持整张 Brush 移动、按 stroke Undo/Redo、继续 paint/erase。

Radial/Linear 沿用解析参数。所有 Mask 仍有自己的 MaskId、enabled、opacity、invert、feather。
Color Grade 的最终 Union R8 包含所有启用 source 的有效 coverage，且在 grade_mix **之前**。
调节 grade_mix 不必重新栅格化，因其是 Mix 的独立标量输入。

### 3.2 执行与历史 API

| 操作 | Forward | Inverse / 恢复信息 |
| --- | --- | --- |
| 首个 Brush stroke | AddMask with first StrokeId and samples | RemoveMask；不先提交空 Mask |
| 后续笔画 | AppendBrushStroke(exact target, stroke) | RemoveBrushStroke(same StrokeId) |
| 删除笔画（底层历史能力） | RemoveBrushStroke | InsertBrushStroke(original order, original sample body) |
| 移动 Brush | SetBrushTranslation(before, after) | 恢复精确 before 标量；不是对当前值累加负 delta |
| 移动 Radial / Linear | Set source center / origin fields | 恢复原字段 |
| 改已有参数 | SetMaskField / focused source fields | 恢复被修改字段 |
| 删除整个 Mask / Grade | 现有 NM4 structural mutation | 保存该删除对象所必需的参数/笔画数据，复用原始 sample body |

一次拖动合并为一个最终命令，内部 preview 只更新未提交的字段。一次 stroke 合并为一个追加
命令，不能每个 sample 创建 history commit。多个已完成 stroke 仍逐笔可 Undo；history 增长
是新增路径参数的大小，不是累计所有之前路径或全幅图像的大小。

在 NM4 `PipelineEditBatch`、序列化、逆操作、WAL、materializer、checkpoint、Version/Paste
中一次性补齐这些操作。History 是唯一命令链；不要另外维护独立可移动 cursor 的笔画历史栈。
恢复一个 Version 得到该 Version 的参数化 Brush，再计算当前 R8；无需其旧缓存存在。

### 3.3 临时数据和生命周期

- 未提交 samples 是用户新增数据，由输入 owner 按顺序接收；已消费数据通过移动/不可变共享
  交给 model 和 history，不保留第二份完整 stroke 集合。
- 空间索引只存 StrokeId、sample/segment 范围、bounds、顺序号。它是 owner 派生索引，不能存
  另一份可写样本、R8 或每个历史节点的栅格。
- R8/float 工作缓冲是算法输出或 scratch，遵守独占 writer 和 GPU reader lease。不是历史状态。
- Dirty bounds、generation 和 required/completed revision 是派生有效性信息，不进入编辑历史。
- GUI 只接收最小的当前选择/控件数值投影；不获取整个笔刷路径容器用于回写。

## 4. 确定性笔刷生成

### 4.1 规范采样

继续使用弧长间隔 dab 的可执行算法，避免先实现曲线拟合带来的额外形状变化：

1. 通过现有 mapper 得到 ReferenceSpace 点。记录设备真实 press，不使用拖动阈值后的第一点。
2. 将点映射到 Brush 局部空间：`local = reference - placement_translation`。
3. 按 reference 像素度量弧长采样。每段间隔不超过局部 radius 的四分之一；跨输入批次保留
   余数。radius/strength/hardness 改变作为有序边界保留；不跨边界丢弃信息。
4. 开始 dab 一次、最终 stroke endpoint 一次；不在每个事件结尾额外加 dab。
5. 存储规范 samples 和算法版本。重放使用这些持久值，不重新读取鼠标速度/设备 pressure。
6. 规范坐标/半径编码、插值和量化顺序由第 4.4 节规定，拒绝 NaN/Inf、重复 StrokeId 和
   不支持的算法版本。不能通过静默加载另一个算法版本来重放旧历史。

初始值沿用执行计划：size/strength 控件影响后续样本，Mask opacity 影响整张 Brush。无自动
pressure 动态；不把用户调整过的 size/strength 在松手后丢失。

### 4.2 Dab、paint 和 erase

在像素中心求单位 dab：内半径 `hardness * radius` 内为 1，外半径处为 0，中间线性衰减，
乘 strength。Hardness=1 单独定义硬边，避免除零。量化与组合公式见第 4.4 节。

对一个 Brush 内所有规范 dab，按原始 stroke/sample 顺序：

```text
b0 = 0
paint: b_next = max(b_previous, a8)
erase: b_next = min(b_previous, 255 - a8)
```

这个规则保持此前计划的固定 coverage-strength 含义，不增加随停留时间积累的 flow。以后若要
flow，应显式改变算法版本和测试，不能用不同采样速度无意引入它。

先重放 Brush，再使用现有 source feather、invert、opacity；所有 Mask 按 NM3 Union 组合。
别把 erase 命令应用到整个 Grade Union，否则会错误擦除 Radial/Gradient 的覆盖。

### 4.3 精度与缩放

保留 4096 的规范 R8 每轴上限和现有 full-reference 定义，不降低 RAW decode/preview 质量。
Brush 的规范格网按第 4.4 节由 `full_reference_extent` 派生，不随 zoom、
DPR、拖动速度或 Interactive 输出尺寸变化。Brush 在固定格网上重放，再由原有 native sampling
映射到请求表示；analytic sources 在请求的 reference 坐标求值。固定运算/量化顺序，缓存命中
和无缓存路径必须满足同一数值标准。

整张 Brush 的移动只更改 translation；samples 永远不做逐次原地平移。每次从原始局部路径
计算目标位置，因此来回移动不累积插值损失。`before`/`after` 精确恢复，避免反复浮点加减漂移。

### 4.4 规范编码、羽化单位与输出采样

生产共享定义在 `edit/mask/brush_raster_encoding.hpp`。下列数值在 host 规范栅格化和
native Mask pass 之间必须一致。`source_format_version` 或 `raster_algorithm_version`
不等于 1 的输入在格式入口拒绝，文件保持原样，不做另一算法的静默重放。

| 量 | 编码 | 规则 |
| --- | --- | --- |
| `source_format_version` | `uint32` | 参数化 Brush JSON 为 `kBrushSourceFormatVersion = 1` |
| `raster_algorithm_version` | `uint32` | dab/paint/erase 为 `kBrushRasterAlgorithmVersion = 1` |
| Mix cache 容器 | `uint32` | `kProjectMaskCacheFormatVersion = 1`，与 `kMaskAssetFormatVersion`（旧 `.r8mask`）分开 |
| 样本坐标 / 半径 | IEEE-754 binary32 | 有限；局部坐标 = 参考像素坐标 − `placement_translation`；半径 > 0，单位是参考像素 |
| strength / hardness | IEEE-754 binary32 | 闭区间 `[0, 1]` |
| StrokeId | 非空字符串 | 生命周期内稳定；重复 ID 拒绝 |
| 笔画模式 | `uint8` | `0` paint，`1` erase |
| 弧长间隔 | 参考像素 | 不超过局部 radius 的 `kBrushDabSpacingRadiusFraction = 1/4`；跨事件批次保留余数 |
| 规范格网 | `Extent2D` | `CanonicalBrushRasterExtent(full_reference)`：长边 `min(long_edge, 4096)`，ceil 比例缩放，每轴 `[1, 4096]`。`reference_bounds = {0,0,1,1}`。不使用 LLF 的 2048 上限。格网由当前 `full_reference_extent` 派生，不随 zoom、DPR、Interactive 输出尺寸或 DetailPatch 改变 |
| 像素中心 | 参考像素 | 规范 texel `(i,j)` 的中心是 `(i+0.5)*full_w/raster_w`。连续指针位置不加半像素 |
| dab 覆盖 | float `[0, 1]` | hardness `>= 1` 时 `distance <= radius` 为 strength，否则为 0。否则内半径 `hardness*radius` 内为 strength，外半径处为 0，中间线性。distance 为参考像素欧氏距离 |
| R8 量化 | `uint8` | `clamp(coverage * 255 + 0.5, 0, 255)`（正方向 round-half-up）。反变换 `value / 255` |
| paint / erase | 逐 dab 顺序 | `b0 = 0`；paint `max(prev, a8)`；erase `min(prev, 255-a8)`。max/min 不可从结果反推 |
| 源羽化 | 现有 `BrushMaskSource.feather_radius` | 非负有限。单位是参考像素度量。native 转为 source texel：`radius_texels = feather_radius * 0.5 * (x_scale + y_scale)`，`x_scale = raster_w / (full_w * max(bounds.w, 1e-6))`。当规范格网等于 full reference 且 bounds 为全图时，该值等于 texel 半径 |
| 求值顺序 | 固定 | 规范 R8 → signed-distance 羽化（若半径 > 0）→ invert → opacity → clamp → 再量化到请求 R8。Union 是启用 Mask 的逐像素 max，发生在 Grade Mix 之前 |
| 输出采样 | 现有 `MakeRasterMaskSamplingPlan` | 渲染像素中心 `(x+0.5, y+0.5)` 经 `render_to_texture_uv` 得到归一化 UV。UV 在 `[0,1]` 外为 0。R8 为双线性，texel 中心 `u*width-0.5`。无羽化时按该 plan 的 mip；有羽化时对距离场做同样的双线性。`geometry.filter` 默认双线性。缓存命中要求 extent、geometry、算法版本和 producer 完全一致，禁止把不相符的槽 resize 后当作命中 |

JSON 中的样本数组按上述 binary32 规则读写。非法值、重复 StrokeId 或不支持的算法版本不得部分写入 owner。NM7.2 将规范笔画、算法版本和 `placement_translation` 写入 `BrushMaskSource`。NM7.3 起 JSON 只写参数化笔画，拒绝 `asset_key` / width / height / `reference_bounds` raster-only 编码；项目文件版本门只接受 8.1 表中切换后身份。

## 5. Undo/Redo 的局部重放算法

### 5.1 均匀分块空间索引

使用简单固定网格（初始实现建议 64×64 规范 raster texel 一块；性能实测可调整，不改像素语义）。
索引存与每块相交的 stroke/sample span，排序保留原计算顺序。长 stroke 按 segment/span bounds
索引，不能只用整条曲折路径的大包围盒把每个 tile 都标成相关。索引不持久化，可以从参数重建。

对平移的 Brush，在 local 空间保留索引，通过逆平移后的 tile bounds 查询。整张 Brush 移动
不用逐帧重建所有 stroke 索引；只更新 owner placement 和输出脏范围。

### 5.2 区域恢复与重合覆盖

设发生变化的 source 旧支持域为 `A`，新支持域为 `B`：

```text
dirty = outward_round(A union B)
for each affected output tile:
    query surviving ordered source contributors
    rebuild Brush coverage from b0 = 0 in the required region
    evaluate feather/invert/opacity and other relevant Mask sources
    recompute Grade Union from its defined empty/all-disabled base
    replace the current output tile only after successful evaluation
```

撤回添加取旧支持域；Redo 取新支持域；移动取两者并集。擦除 Undo 必须重放该区域内更早的
paint/erase；不能仅清零。删除或移动 Union 最大贡献者时要重新计算其他 source；不能从最大值
中减去被移除的贡献。对拥有全图背景值的 invert、Linear Gradient 或 enabled/opacity 变化，
必须使用其实际完整支持域，而不是误认为只有控件附近会变。

正确性原因：对一个输出像素，不与它的求值域相交的 dab 是恒等操作。保留相关 dab 原始顺序
从初始值重放，得到与完整重放相同的结果。该论证只覆盖逐像素 source 合成；邻域 feather 的
依赖域必须另算。

### 5.3 Feather 的完整依赖

现有 native Brush feather 涉及 signed distance。它的中间距离场可能全局变化；不要因为 dab
的 R8 写入范围小就断言 distance pass 也能只更新相同矩形。

初版保留完整 native distance 求值作为需要该依赖的正常算法路径，复用已有分离方向 pass；
只对已证明局部的无 feather 操作做 tile 重放。若实现有限 feather 半径的 halo 优化，必须证明
输出域、输入 halo、边界条件和全量算法一致后才能启用。不能更换成 Gaussian blur 或裁掉长距离
影响来满足 16 ms。Distance scratch 用完释放，不生成长期每-stroke 距离缓存。

### 5.4 增量追加与单 R8 的限制

只有当前完整源信息足够时才允许追加优化。最终 Grade R8 已混合了多个 Mask，并可能经过
invert/opacity，因此不能把它当作原始 Brush 直接 max/min。默认按上面的区域重放生成改变部分，
不要为省重算悄悄保留每个 Mask 一整张长期 source R8。

需要的 source/feather/Union 临时缓冲由现有 executor scratch 提供，顺序复用并在最后 reader
完成后释放。临时计算缓冲和一次原子文件替换的临时文件，不是“第二份长期缓存”；要分别测量。

### 5.5 复杂度和已做的小型算法验证

路径存储为 `O(S)`，S 是真实样本总数；追加一个 stroke 的 history 大小为该 stroke 的样本数，
不是所有 S 的累计副本。索引大小取决于相交 span 数。一次局部重放成本取决于 dirty tiles 内
相交样本数和像素数；重叠很多、全幅移动或全局 feather 的最坏情况仍可能需要完整重算。
不承诺无条件常数时间 Undo 或任意复杂 Brush 都在 16 ms 内完成。

2026-09-08 用临时纯算法脚本验证了 4,000 次有序添加/移除/移动：16×16 整数格网、4×4 tile、
paint/erase max/min、越界 clipping、另一个 analytic-like source 的 Union；每次局部重算结果
都与独立全量求值逐字节相等。脚本与输出在 `build/tmp/nm7_plan/check_replay.py` 和
`build/tmp/nm7_plan/replay_result.txt`，不提交。它证明此简化实例的重放逻辑，不证明真实 dab
插值、Qt 输入、feather、GPU、文件恢复或产品性能。产品测试仍须覆盖第 10 节。

## 6. 一个当前 R8 槽的准确范围

逻辑身份是 `(ProjectUUID, ImageId, NodeId)`。一个 Color Grade 的多 source 最终只产生一张
供 Mix 使用的当前有效 coverage。磁盘上这个身份最多一个已发布 R8 cache 文件；Version、
StrokeId、commit hash、content hash 不能作为新增文件的目录层级。

示意路径（ID 使用受验证的安全编码，不使用用户节点名称）：

```text
<chosen-root>/alcedo-mask-cache/<ProjectUUID>/<ImageId>/<NodeId>.r8cache
```

文件内部包含 schema/algorithm version、身份、source 规范格网与当前输出 extent/ReferenceSpace 描述、完整 recipe
fingerprint、checksum 和 R8 payload。Fingerprint 只用于验证内容，不作为多版本文件名。
必须含有原始采样、placement、全部 source 参数、enabled/invert/opacity、raster 算法和 geometry
表示依赖；还需记录 producer/backend 数值兼容标识，未经像素一致性证明不能跨 producer 复用。
不要错误包含不影响 coverage 的节点显示名称。

会话热路径用 NM6 required/completed revision 与表示条件，不每个鼠标事件序列化/hash 整个
笔刷集合。跨重开验证的 fingerprint 在 owner 的 settle/save 边界计算；request 和写入者持有
精确版本身份，不能给旧像素写上新 fingerprint。

### 6.1 内存表示与 NM6 相容性

- 每个 live workspace 为一个 Grade 保留一个当前 Interactive Mix coverage 结果；失效后在
  reader 安全边界重新写入。单一 R8 指有效内容身份，不否定异步提交所需的短期 unpublished
  输出缓冲；这些 lease 不得变成历史结果集合。
- 磁盘槽保存最近一次 settled 状态的 Interactive Mix coverage，直接来自该当前结果；
  不是再计算/保留一份不同分辨率的长期 full-reference Mix 图。source 的规范格网仍保持不变。
  缓存仅在请求 extent/geometry/算法/producer 表示完全相符时命中，不 resize 不相符的缓存
  来假定像素相等。新表示成功后替换同一个槽，不增加另一个表示文件。
- 若 release 前没有产生最终值的 Interactive coverage，标记磁盘槽 stale；Quality 仍正常由
  参数求值。没有合格结果时不贴错 fingerprint 写盘，也不为了落盘偷偷增加一个 Quality-like
  持久结果；下次实际 Interactive 请求产生合格结果时再写该槽。
- 落盘确需独立读回 buffer 时，仅为一个合格 settled 结果创建一次；说明其 owner、版本和
  I/O reader lifetime，用完释放。不能每次 pointer move 复制一张 CPU 图等着写盘。
- Interactive、Quality、Detail 的采样语义保持现有定义；QualityBase 在 RAW Develop 之后仍
  不读写持久结果缓存，直接由参数求值到 submission-local coverage，Mix 后释放。
- 不为每个表示/Version 各建磁盘 R8，也不新增长期 Brush/Radial/Gradient source 图。
- 暂时持有的 GUI 图元、CPU 输出、上传暂存、native scratch 和读者 lease 分项计量并有上限。

### 6.2 提交与缓存写回完全解耦

```text
pointer move -> focused provisional command -> region replay -> Interactive photograph
release -> NM4 durable parameter command -> Quality photograph
        -> mark stable cache slot dirty; coalesce pending cache write
project save / safe idle / clean close -> validate captured settled identity -> atomic replace slot
```

R8 写回不在 pointer handler，不是每个 stroke 的 durable history 前置条件。采用一个项目后台
writer，合并同槽尚未开始的写入；已有 reader/writer 完成后再复用缓冲，不排队保存所有版本。
只写 settled state，不把取消前的 preview 作为重开缓存发布。短时间内很多 stroke 可以只落盘
最新结果；即使每次都遇到 idle，文件槽数量也不随操作次数增加。

使用原子替换，一次活动写入最多一个临时文件；成功/失败/启动恢复清理它。使用已验证文件 owner
和作业身份删除临时文件，不按任意目录通配符清理。禁用 `QSaveFile::setDirectWriteFallback(true)`
这类直接写坏旧槽的模式。Qt 原子替换不等于两个独立 metadata/R8 文件的共同事务，因此 header
与 payload 写在同一容器内。项目持久历史仍遵循自己的 WAL 耐久规则。

缓存写失败要报告具体错误并保留 dirty 状态，不宣称写成功、不换存储路径、不撤销已经成功的
参数提交。后续重建来自完整参数的同一算法，这是本次明确指定的缓存模型，不是降低质量的替代
算法。缺失或 checksum 不匹配的缓存不是用户数据损坏；参数/历史损坏则是真实错误，不能靠
旧 R8 掩盖。持续 I/O 错误不忙循环重试。

## 7. 项目级存储设置与清理

### 7.1 所有权与界面

由 `ProjectService` 拥有 Mask cache 设置，并提供拟新增 `ProjectMaskCacheService`。通过
现有 project module 向设置界面投影；不能复用 thumbnail 全局配置键当成项目设置。
设置入口建议放入现有 Settings > Cache 中独立的“项目蒙版缓存”区域，显示当前项目名称/UUID、
路径、文件数、字节数与待写/失败状态。已注册项目可逐行管理，不遍历用户磁盘寻找项目。

每项目设置：

| 字段 / 操作 | 语义 |
| --- | --- |
| Cache directory | 用户选定 root；默认项目 metadata 所在持久目录下的专用 cache 子目录，不使用解包临时目录或全局 product_mask_store |
| Retention policy | `Keep`（默认）或 `DeleteOnProjectClose`；项目 A 的选择不改变 B |
| Clear this project's Mask cache | 删除选定项目的可重建 R8；保留参数、history、Version、RAW 和缩略图 |
| Project removal choice | 删除/移除项目时单独选择保留或清理该项目 R8；不把移出最近项目列表当成删除文件授权 |
| Move cache directory | 验证新路径并切换 owner；设置成功后清理旧的本项目缓存，失败残留可继续逐项目管理 |

`ProjectService::SaveProject` 当前重新构造 metadata JSON，必须在 owner 中保存并 round-trip
新增字段，不能只让 QML 写一个下次 Save 会消失的键。复用 project UUID。相对路径以持久项目
位置解析；外部绝对路径按原值保存，路径不可用时显示错误并让用户重新选择，不转到系统 temp。
设置不属于照片 edit history，修改设置不能新增 photo Version。

### 7.2 安全切换路径

1. UI 提交目标 project UUID、预期设置 revision、新 root。界面明确此操作将切换位置并清理原来的本项目缓存。
2. owner 验证/创建专用 namespace、权限与路径关系；新路径必须在 UI Apply 前可具体检查。
3. 停止该项目新 cache jobs，递增 storage generation，等待已有文件写入者/读取者到安全边界。
   Photo history/数据保存使用原有 project 生命周期，不用 GUI 阻塞等锁。
4. 原子保存新项目设置后才发布新 root。保存失败继续使用原 root，并保留原缓存。
5. 缓存可由参数重建；无需复制每张缓存。所有旧 generation jobs 拒绝向新 root 写数据。
6. 新设置成功后删除旧的本项目 cache namespace，完成“更改位置并清理旧缓存”操作。
   不提供让每次换路径都积累旧缓存的默认行为。清理失败则记录旧 root 的所有权位置和错误，
   供重试/逐项目清理；不能丢掉记录而留下无法管理的隐藏文件。

项目级 owner 的存储位置记录只记录路径/UUID/格式，不镜像 Mask 参数。它必须随设置事务更新。
如果 root 变更后的旧目录清理失败，报告残留路径；不把已经成功的新设置回滚成半迁移状态。

### 7.3 清理算法与并发保证

清理只针对带匹配 ProjectUUID/格式标记的专用 cache namespace。每个文件通过容器身份和
受验证的路径定位，不递归删除用户选择的整个 root；不跟随逃离 namespace 的符号链接。
不同项目可以共用上层 root，不能互删。历史不可变 `MaskAssetKey` 文件与新 cache namespace
必须明确区分，旧文件没有完整参数可重建时绝不能作为 cache 清理。

Clear 先建立维护边界，递增 cache generation、停止/合并写回，等读者/写者释放后删除，清除
内存有效标记，再解除边界。正在编辑的项目可在下一次真实渲染请求重建；Clear 操作本身不要
立即发一次重建写回而让用户看到“清理后立刻原样长回来”。已经排队的旧 writer 不能复活文件。

DeleteOnProjectClose 在参数/WAL 保存成功、项目 reader/writer 停止后执行。崩溃没有可靠 close
回调，下次恢复参数后再执行已保存策略的待清理工作。清理失败列出已删除/未删除字节及原因。
不能因为缓存清理失败就删除项目参数或伪造历史保存失败。

进程内所有 reader/writer/cleaner 共用 owner 维护锁；需要跨进程共享时复用项目独占机制，
不足才用 QLockFile namespace 锁。长作业禁用 30 秒时间过期判定并处理真实锁错误。缓存配置
不是绕过已存在项目多进程写保护的入口。

## 8. 保存、Version、Paste、项目打包及格式切换

Parameter document 和 typed history 必須包含全部 stroke samples、算法版本和 placement。
新项目 package 即使不带任何 R8，也能完整恢复每个 Version 和 Undo/Redo。Portable Paste
复制/复用必要的参数化 stroke body，重映射 NodeId/MaskId/StrokeId；不复制源机器绝对 cache
路径或复制一组按历史版本堆积的 R8。目标项目使用自己的存储策略。

这修改了 NM3/NM4 的 source/history schema，必须作为 NM7 内部**先行前置工作**完成，不能只
改 viewer。更新项目版本 gate、document schema、typed operation 编码、canonical serialization、
WAL recovery、package manifest 和 transfer validation。同一发布版本里不能用 R8 key 作为新
Brush 的唯一恢复依据。NM3/NM4 旧测试通过不等于参数化格式通过。

只有栅格而没有笔画参数的旧格式不能无损反推原始路径。遵守仓库“不自动迁移旧项目”的现有
规则，在格式入口报告不支持并保持文件不变；不新增自动向量化、隐藏背景 bitmap 或“缓存就是
原始数据”的兼容路径。修改文件版本前在测试 fixtures 和发布说明明确覆盖的格式范围。
不执行旧项目转换或删除已有 `.r8mask` 文件。

### 8.1 切换后接受的唯一格式身份

当前生产加载器只接受下表「切换后唯一接受值」（NM7.3 已切换）。旧身份立即不支持；不读取、不改写、不删除被拒绝的文件。`asset_key` 不再是 Brush 的持久来源。

| 身份 | 切换后唯一接受值 | 当前已发布值 |
| --- | --- | --- |
| Project metadata `kProjectFileVersion` | `0.6.0` | `0.5.0` |
| Packed `.alcd` `kPackedProjectFormatVersion` | `6` | `5` |
| Pipeline document `kPipelineDocumentFormatVersion` | `6` | `5` |
| Image edit schema `kImageEditSchemaVersion` | `4` | `3` |
| Commit hash input `kCommitFormatVersion` | `4` | `3` |
| Chain-fold `kChainFormatVersion` | `4` | `3` |
| Typed batch `kPipelineEditBatchFormatVersion` | `3` | `2` |
| Root serialized pipeline state `kRootStateFormatVersion` | `4` | `3` |
| Checkpoint serialized pipeline state `kCheckpointStateFormatVersion` | `4` | `3` |
| Mini-Git WAL `kMiniGitJournalRecordFormatVersion` | `5` | `4` |
| Transfer package `kAdjustmentTransferSchema` | `alcedo.adjustment_transfer.v4` | `alcedo.adjustment_transfer.v3` |
| Brush `source_format_version` | `1` | 不存在；当前 JSON 无此键 |
| Brush `raster_algorithm_version` | `1` | 不存在 |
| Project Mix cache `kProjectMaskCacheFormatVersion` | `1` | 不存在 |
| 旧 Mask 资产文件 `ALCR8MSK` / `kMaskAssetFormatVersion` | 新 Brush 不再写入或要求 | `1`（`.r8mask`） |

拒绝条件（均保持原文件）：document/history/package 仍带 Brush `asset_key` 且没有完整规范
strokes；`source_format_version`/`raster_algorithm_version` 缺失或不为 1；未知 source kind；
非有限样本；重复 StrokeId。Radial/Linear 字段集合不变。运行时 `kMaskImplementationVersion`
（现为 3）在 Brush 内容身份从 asset key 改为 stroke recipe 时再升到 4，它不是项目文件身份。

### 8.2 `MaskAssetKey` 产品依赖与替换

| 现有依赖 | 当前作用 | 参数化路径下的替换 |
| --- | --- | --- |
| `BrushMaskSource::asset_key` + descriptor | 持久 Brush 来源 | 有序 strokes、StrokeId、algorithm version、`placement_translation`；保留 `feather_radius` |
| `MaskStore::Put` / `Load` / `DefaultProductMaskStoreRoot` | 内容寻址 `.r8mask` | 新 Brush 提交不再 `Put`。当前 Grade Mix 使用项目 cache 槽；旧 store 不得当 disposable cache 清理 |
| `CollectPersistentMaskAssetKeys` / `VerifyPersistentMaskAssets` | 文档引用的 R8 必须存在 | 校验规范笔画；Mix cache 缺失时按同一算法重建，不把缺 cache 当成参数损坏 |
| `CollectMaskAssetKeysFromBatch` / reachability / `DeleteUnreachableMaskAssetFiles` | 按 key 做 GC | 新 history 不含 per-stroke R8 key。GC 不得删除无法从参数重建的旧 `.r8mask` |
| `ReplaceMaskAssetChange` 与 `EditorHistoryMutation::ReplaceMaskAsset` | Undo 换 key | `Append/Remove/InsertBrushStroke`、`SetBrushTranslation`；首笔是一次 `AddMask` |
| `AddMaskChange` / `RemoveMaskChange` / `ReplaceMaskSourceChange` / `SetMaskFieldChange` | 结构与标量 | 保留；`AddMask` 的 JSON 改为笔画而不是 asset key |
| `AdjustmentTransferPackage::mask_assets_` | Paste 复制 key+descriptor | 复制笔画 JSON；目标项目用自己的 cache 策略；不再要求 listed R8 key 集合 |
| `result_content_key` 混合 `asset_key` | GPU 结果身份 | 混合 strokes / placement / algorithm version |
| `MaskTextureCache` keyed by `MaskAssetKey` | 不可变持久纹理 | 每 Grade 一个当前 Mix coverage；source/feather 为 executor scratch |
| `ActiveRasterMaskInput` / `ActiveRasterTextureCache` | 请求持有的预览 R8 | 仍是请求所有的不可变像素；内容改为规范重放输出，不与 `MaskAssetKey` 共用 key 空间 |
| `Renderer::MaskAssets()` 临时目录 store | 产品默认根 | `ProjectService` + `ProjectMaskCacheService` 的每项目根 |
| `EditorPendingInputQueue` 按 field 保留最新绝对写入 | 普通调整 | Brush 需要有序 append，不能把样本当成同一 field 的最新值替换 |
| `DescribeEditorParameterTargetError` 拒绝 `ColorGradeMask` | 文案仍写 “until NM3” | 增加显式 Mask 命令路由；不能只删掉守卫 |
| 测试夹具 `grade_mask_test::AddBrushMask(..., MaskAssetKey)` | 现行运行时/历史测试 | 保留作为旧格式证据；新测试走笔画 builder |

## 9. 移动已有蒙版的完整预览链

```text
press move handle -> capture exact target + before translation/center/origin
move -> normalized new fields -> enqueue (coalesce latest absolute placement)
     -> QSG control position update only
owner safe boundary -> apply fields -> invalidate old/new source domains
                    -> regional/full-required replay -> Grade Union R8 -> native Mix
                    -> Interactive photograph
release -> one typed placement/parameter commit -> Quality photograph
Escape -> restore before fields -> recompute correct previous result -> no commit
```

Brush 移动整个累积笔画集合，包括已记录 erase；在移后位置继续 paint，要用当前 placement
的逆映射存新 samples。Radial 修改中心；Linear 修改 origin，保持其 normal/transition 不变。
相邻 mouse moves 合并绝对 translation，不能丢失结束值；不是把最后一张照片临时平移。

QSG 已有蒙版编辑只显示可交互 handles、方向/连线等必要控件，不显示整片填充、羽化区域高亮、
已完成笔刷轨迹或 coverage heatmap。控件随输入定位；照片由 Interactive 更新。初次创建是否
保留暂时轮廓/轨迹的细节在执行计划决策表中记录；任何模式都不以高亮代替真实照片求值。

## 10. 必须完成的测试与证据

| 测试组 | 具体断言 |
| --- | --- |
| 可逆命令 | append/remove/move/field forward+inverse 恢复原参数；no-op 不提交；多 stroke 同 MaskId |
| 像素反例 | 证明 max/min 不可直接逆；擦除 Undo 与重放旧参数相等 |
| 局部重放 | 随机 paint/erase/overlap/move/delete/undo/redo，与独立完整重放逐字节比较；halo 单独验证 |
| 移动 | Brush/Radial/Linear 每次 owner consume 都有正确 Interactive；release 后才 Quality；旧域清除、新域正确、siblings 保留 |
| 精度 | 同 translation A→B→A 后参数精确恢复，R8 与初始相同；重复移动无重采样模糊 |
| 缓存数量 | 同节点 1,000 stroke + 1,000 Undo/Redo + Version 切换，稳定已发布 R8 文件数 ≤ 1 |
| 磁盘上限 | 4,096² R8 = 16 MiB；一个槽加 bounded active replacement，不出现 1,000×16 MiB；history 大小随样本数记录 |
| 全清理恢复 | 删除全部新 cache，重开及每个 Version/Undo/Redo 仍恢复相同参数和 coverage |
| 写回失败 | 中断写/磁盘满/旧 generation/job：无半文件、无假成功、无每步残留 tmp、参数不丢 |
| 项目隔离 | A/B 共用 root；A 清理/换路径/关闭，不影响 B；符号链接不能逃逸；原始 assets 不删除 |
| 设置 | 重开、项目切换、保存/打包后设置正确；路径不可用报错；关闭清理和保留策略互不混用 |
| 历史 | 新 payload 不含每-stroke R8 key 或整幅像素；节点删除恢复必要 samples；WAL/Version/Paste 完整 |
| 性能 | event→Qt 控件、queue→owner、重放、feather、Union/Mix、Interactive completion、缓存 I/O 分项测量 |

所有 native 测试在 CUDA/OpenCL/Metal 分别记录，保留现有数字容差和 NM6 串行/质量策略。
不要因为复杂图超过 16 ms 就保留隐藏历史栅格、降低分辨率、丢样本或换 backend。
