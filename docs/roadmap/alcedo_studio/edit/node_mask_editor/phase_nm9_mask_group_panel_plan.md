# Phase NM9 — 蒙版组孪生面板与传统摄影工作流

Date: 2026-09-16

Status: **NM9.1 complete 2026-09-16**；NM9.2–NM9.6 仍为 planned。本文件记录产品语义和
实施拆分；NM9.1 的共享组投影、顶部插入、桥接删除与草稿边界已在
`feature/nm91-mask-group-projection` 实现并通过验证，见 NM9.1 完成记录。

Parent: [Node-aware Pipeline Editing and Mask Creation](../node_mask_editor_master_plan.md)，
第 7.1、16、18.1、21.10、26 节。

Prerequisite: [NM8](phase_nm8_product_qualification_plan.md) 已按用户 2026-09-16 的决定在
NM8.4 收口。原 NM8.5/NM8.6 不作为 NM9 前置工作；已有平台证据缺口保留在 NM8。
当前 Mask 类型继续为 Radial / Linear Gradient，`ALCEDO_ENABLE_BRUSH_MASK=OFF`。

## 1. 目标与数据边界

节点数量在 NM8.3 已测场景下没有带来用户认为显著的计算时间增长，但节点图的选择、
连线和定位会增加传统摄影调整的操作成本。NM9 保留现有 QuickQanava 节点编辑器，增加
一个 **Mask Groups（蒙版组）** 面板，让用户按组创建和管理局部调整。

两种面板是同一 DAG 的不同投影：节点图强调连接，蒙版组强调按处理顺序排列的调整及其
蒙版。它们共用 `PipelineDocument`、NodeId、MaskId、参数、编辑历史、Version 和渲染路径。
“图层”可用于解释交互，但不引入独立图层文档、第二套调整模型或像素层存储。

用户确认的删除锁只保护删除：节点参数、Mask 形状和强度仍可编辑；显式解锁后才允许
删除。默认 Color Grade 1 及其蒙版默认开启删除保护。

## 2. 面板和编辑语义

本阶段遵守 [DESIGN.md 的 monochrome 选择规则](../../../../../alcedo_studio/src/ui/alcedo_main/DESIGN.md#monochrome-selection-and-restrained-theme-blue)：
禁止蓝色相框式选中态，组、Mask row 和缩略图都不使用蓝框、蓝底或蓝色侧边条表达选择。
第 2.6 节保留多个 ASCII 布局候选，目前不选定任何一个，也不将候选写成已批准的 UI。

### 2.1 DAG 与 stack 的对应

```text
节点图：Develop → Color Grade 1 → Color Grade 2 → Color Grade 3 → DRT/Post

Mask Groups（从上到下）：
  Color Grade 1    [删除锁] [合成蒙版缩略图（有蒙版时）]
    Masks 抽屉
      Radial      [位置/强度示意] [删除锁] [删除]
      Gradient    [位置/强度示意] [删除锁] [删除]
  Color Grade 2
    空的 Masks 抽屉
  Color Grade 3    [合成蒙版缩略图]
    Masks 抽屉
      Radial      [位置/强度示意] [删除锁] [删除]
```

- 每个 Color Grade 都有一个组，包括没有 Mask 的节点；组的身份就是 NodeId。
- 从 Develop 沿 scene-image edge 到 DRT/Post 的执行顺序，就是 stack 从上到下的顺序。
  不按创建时间、显示名称、节点画布坐标或字符串排序。
- 名称与节点图完全相同。重排不重命名；`Color Grade 1` 是默认节点的初始名称，不是
  “当前第一行”的别名。新建仍使用既有创建计数器，插到顶部也不重新编号已有组。
- Develop 与 DRT/Post 保留现有入口，不显示成可添加 Mask 的普通组。
- 抽屉展开状态、滚动位置和面板宽度是 UI 状态；不进入照片历史，不触发渲染。

### 2.2 空抽屉、蒙版 rows 和缩略图

无 Mask 时保留组头和空抽屉，不虚构一个全黑蒙版。没有 Mask 的 Grade 仍遵守原有全图
调整语义；“空”只表示未附加 Mask，不表示透明、没有输入或不执行该节点。

有 Mask 时，在组头显示一个小的黑底白蒙版缩略图，用灰度表达过渡和强度。展开后显示
所有 Mask rows，包含类型、位置/强度示意、删除锁和删除动作。Mask 参数仍在现有右侧
Mask 页编辑；组列表承担查找和管理，不复制完整参数表。

组头显示该 Grade 的有效合成 Mask coverage：复用既有 enabled、invert、opacity 和 Union
顺序，取应用 Grade Mix 之前的 coverage。单行显示该 Mask 经现有规则求值后的 coverage，
并显示其 opacity；关闭的 Mask 有明确不可用状态。现有求值规则是：空 Mask 集合不限制
Grade 的全图作用；非空集合但全部关闭时 coverage 为零。组头对前者不显示虚构缩略图，
对后者显示真实全黑 coverage 并保留各关闭 row，不能把这两种状态合并成“没有 Mask”。

采样图约为 256×256 级别，最长边不超过 256；保持照片比例，可置于黑色方形底内。
UI 按 AppTheme 的紧凑尺寸显示，不把整个 256 px 图直接占满组头。裁剪、旋转和纵横比
使用与 Viewer 一致的坐标映射；不能把椭圆拉圆或把照片外区域算进 Mask。

### 2.3 两个创建入口

| 操作 | 目标和结果 |
| --- | --- |
| 新建 Mask Group | 在 stack 顶部创建一个 Clean Color Grade，原子完成插入和连线，选中新组；初始无 Mask、画面不变 |
| 已选中组后使用现有 Radial / Gradient 按钮 | 在该 NodeId 下进入原有 Mask 创建流程，不另建 Grade |
| 没有选中可添加 Mask 的组时使用现有 Mask 按钮 | 默认选中 stack 顶部的可编辑 Color Grade，再开始创建；无此组时给出明确不可用原因，引导使用新建组 |
| 从节点编辑器新建、改名、连接或删除节点/Mask | 同一次 owner 更新刷新 Mask Groups；不另造一次用户编辑或 history commit |
| 从 Mask Groups 新建或删除 | 节点图在同一次提交后更新；不要求先打开 Nodes，也不依赖 Qan delegate 存活 |

有效选择优先于“默认顶部”。不能在用户已选中 Color Grade 3 时把新 Mask 写到 Color
Grade 1。用户显式选中 Develop、DRT/Post 或未来的混合组时，Mask 创建动作显示禁用原因，
不能偷偷改写目标。创建中的确认、取消、离开面板规则复用原有状态机。

### 2.4 顶部插入与“复制下面图层”的准确含义

按用户要求的顶部新建和第 2.1 节从上到下的执行顺序，当前计划采用：

```text
新建前：Develop → Color Grade 1 → Color Grade 2 → Color Grade 3 → DRT/Post
新建后：Develop → Color Grade 4 → Color Grade 1 → Color Grade 2 → Color Grade 3 → DRT/Post
stack： Color Grade 4、Color Grade 1、Color Grade 2、Color Grade 3
```

这是对“顶部插入”的顺序解释；没有倒转第 2.1 节的列表。插入点依据真实 edge 确定，
不以 `Color Grade 1` 的字符串或固定数组下标寻址。

这里的“复制下面图层”表达的是**继续处理已有图像结果**：新组不是透明像素层，其无调整
状态等价于把输入原样传递，添加 Mask 后只改变本组调整的作用范围。实际输入始终来自
DAG 的上游；不读取列表下方的未来输出，不复制相邻 Grade 的 Exposure、Saturation、LUT
或 Mask，不重复应用默认曝光/饱和度，也不分配一份图层 RGBA 副本。

新组采用既有 Clean Grade：Exposure 0 EV、Saturation 无变化值、其他调整无视觉变化、
enabled=true、mix=1、Mask 为空。这和默认 Color Grade 1 的产品初始调整严格区分。

### 2.5 未来并行混合的表示

未来 DAG 若增加并行分支与混合，一个完整混合步骤在 stack 中显示为一个整体组，身份
指向其真实图操作，并能定位到节点编辑器。该组不允许添加 Mask；不把分支简单排序成
串行 Grade，也不把未支持的拓扑悄悄展开成不同像素语义。

NM9 只固定这条表示和能力规则，不实现新的并行混合执行器。当前单主链继续由既有图
验证器保证；未来新增混合类型时必须补充明确的分组边界、输入/输出和测试后才能开放。

### 2.6 ASCII 布局候选（未选定）

2026-09-16：用户要求先保留多个候选，**现在不决定布局**。A–D 不设推荐顺序，不代表
实施承诺；NM9.3 开始布局实施前再确定采用哪一种。以下只比较信息组织，均遵守第 2.1–2.5
节的数据和操作语义，不通过线稿改变顶部插入顺序、Mask 所有权或删除锁规则。

线稿使用英文短标签保证等宽对齐；产品使用本地化文案。字符宽度不代表实际像素，最终
使用现有 260–460 px 侧栏范围与 AppTheme 尺寸，不因线稿紧凑而缩小按钮点击区域。

**共同约定：**

- `[+ Mask Group]` 是唯一新建组动作。现有右侧节点标题旁的 Radial / Gradient 按钮继续
  创建 Mask，目标跟随所选组；本轮候选不把同一工具栏再复制一份到每个组。
- `v` / `>` 表示展开 / 折叠；`[L]` / `[U]` 代表关闭 / 打开的删除锁图标。`[X]` 是删除
  动作，`[--]` 是受锁保护的禁用删除动作；这些不是状态标签或新增徽章。
- `[.##.]` 等小框表示蒙版缩略图的占位区域，不是蓝色边框。`.` 是黑底，`#` 是白色
  coverage，`:` 是过渡灰度；真实 UI 使用第 2.2 节的采样结果。
- 行首和行尾的 `=` 仅标注**该整行使用浅色底和深色文字/图标**，不是实际显示的字符
  或描边。当前 owner 组用文字强调或安静的中性底识别，不再套一层高亮框。
- 图中百分比都是 Mask opacity 的只读摘要，不是新增内联滑条。参数仍在右侧 Mask 页
  编辑。组头缩略图表示合成 Mask，子行缩略图表示单个 Mask。

四个候选共用的工作区关系：

```text
+---------------------+--------------------+--------------------------+
| Mask Groups         |                    | Color Grade 1            |
| [+ Mask Group]      |                    | [Radial] [Gradient]      |
|                     |       Viewer       +--------------------------+
|   A / B / C / D     |                    | Mask parameters          |
|                     |   selected Mask    |                          |
|   selected row      |   editing controls | Shape / strength / ...   |
+---------------------+--------------------+--------------------------+
```

#### 候选 A — 独立组抽屉

每组保持熟悉的标题加 Masks 抽屉，多个组可以同时展开。组之间有间距，组内是平整的
蒙版 rows，容易对应现有节点抽屉；代价是标题、抽屉头和组间距占用更多垂直空间。

```text
+--------------------------------------------------+
| Mask Groups                      [+ Mask Group]  |
|                                                  |
| +----------------------------------------------+ |
| | Color Grade 1            [.##.]  [L]  [--]   | |
| | v Masks                                      | |
| |=  [.##.] Radial     100%          [L]  [--] =| |
| |   [..:#] Gradient    45%          [L]  [--]  | |
| +----------------------------------------------+ |
|                                                  |
| +----------------------------------------------+ |
| | Color Grade 2                    [U]  [X]    | |
| | v Masks                                      | |
| |   No masks                                   | |
| +----------------------------------------------+ |
|                                                  |
| +----------------------------------------------+ |
| | Color Grade 3            [##..]  [U]  [X]    | |
| | v Masks                                      | |
| |   [##..] Radial      70%          [U]  [X]   | |
| +----------------------------------------------+ |
+--------------------------------------------------+
```

外框仅表示已有中性组结构，不随选择变蓝；选中子 row 时只反相该行。空抽屉始终属于
Color Grade 2，不增加虚构的黑色缩略图或默认 Mask。

#### 候选 B — 扁平缩进列表

取消逐组卡片，只用一条共享列表和细分隔线。组头带折叠控制，Mask rows 缩进；相同高度
能看到更多组，适合频繁跨组选择。需要用对齐和行距清楚表达归属，避免长列表读串行。

```text
+--------------------------------------------------+
| Mask Groups                      [+ Mask Group]  |
|--------------------------------------------------|
| v Color Grade 1            [.##.]  [L]  [--]     |
|=    [.##.] Radial     100%          [L]  [--]   =|
|     [..:#] Gradient    45%          [L]  [--]    |
|                                                  |
|--------------------------------------------------|
| v Color Grade 2                    [U]  [X]      |
|     No masks                                     |
|                                                  |
|--------------------------------------------------|
| v Color Grade 3            [##..]  [U]  [X]      |
|     [##..] Radial      70%          [U]  [X]     |
|                                                  |
+--------------------------------------------------+
```

组头缩略图和操作列对齐，子 row 的类型、强度与操作列保持稳定。折叠只隐藏该组子项，
不移动组的主链顺序；删除和锁点击不传播成组选择。

#### 候选 C — 缩略图优先

把组的合成蒙版放在标题左侧，给位置/形状更高的视觉优先级；子 row 也使用稍大的预览。
适合凭“蒙版画在哪里”寻找调整，代价是同屏组数更少。预览只是更大的显示窗口，采样
上限仍为第 2.2 节约 256×256 级别，不增加照片渲染尺寸。

```text
+--------------------------------------------------+
| Mask Groups                      [+ Mask Group]  |
|--------------------------------------------------|
| v +------+  Color Grade 1             [L]  [--]  |
|   |..##..|                                       |
|   |.####.|                                       |
|   +------+                                       |
|=    +------+  Radial                  [L]  [--] =|
|=    |..##..|  100%                              =|
|=    +------+                                    =|
|     +------+  Gradient                [L]  [--]  |
|     |..::##|  45%                                |
|     +------+                                     |
|--------------------------------------------------|
| v            Color Grade 2             [U]  [X]  |
|              No masks                            |
|--------------------------------------------------|
| > +------+  Color Grade 3              [U]  [X]  |
|   |##....|                                       |
|   |###...|                                       |
|   +------+                                       |
+--------------------------------------------------+
```

无蒙版组留空预览列并显示空抽屉，不拿照片缩略图代替 Mask。图内预览边界只是 ASCII
示意；实现时可直接将黑底图嵌入行内，不要求每张预览另加一圈描边。

#### 候选 D — 单组展开的手风琴

组头保持紧凑，只展开当前正在处理的一组，把该组的 Mask rows 留在原有顺序位置。
适合一次完成一个局部调整，减少长抽屉挤占空间；跨组比较需要更多展开操作。选择其他
组或其 Mask 时，先按现有状态机完成当前编辑，再展开新 owner，不能取消尚未提交的输入。

```text
+--------------------------------------------------+
| Mask Groups                      [+ Mask Group]  |
|--------------------------------------------------|
| v Color Grade 1            [.##.]  [L]  [--]     |
|                                                  |
|     Masks                                        |
|=    [.##.] Radial     100%          [L]  [--]   =|
|     [..:#] Gradient    45%          [L]  [--]    |
|                                                  |
|--------------------------------------------------|
| > Color Grade 2                    [U]  [X]      |
|--------------------------------------------------|
| > Color Grade 3            [##..]  [U]  [X]      |
|                                                  |
|                                                  |
+--------------------------------------------------+
```

展开空的 Color Grade 2 时，原位显示 `No masks`；已有组不会因为折叠而从列表中消失。
从节点编辑器选中另一组的 Mask，同样自动展开其 owner。该候选的“单组展开”是待选布局
行为，不覆盖 A–C 的多组展开方式。

#### 比较维度与共同行为

| 候选 | 查找依据 | 同屏密度 | 跨组比较 | 主要取舍 |
| --- | --- | --- | --- | --- |
| A 独立抽屉 | 组边界、标题和子 rows | 中 | 多组同时展开 | 熟悉、边界明确；垂直结构较多 |
| B 扁平列表 | 缩进、名称、固定操作列 | 高 | 多组同时展开 | 扫描快；需要控制缩进和组间距 |
| C 缩略图优先 | 合成与单 Mask 的位置/形状 | 低 | 能比较可见预览 | 视觉查找直接；滚动距离更长 |
| D 单组手风琴 | 当前组和紧凑的其他组头 | 随展开组变化 | 需要切换展开组 | 聚焦当前任务；跨组比较操作更多 |

这些是布局预期，尚无用户测试数据，不据此宣布某个候选更快。后续比较使用 NM9.6 的
相同任务与数据，尤其检查 260 px 宽度、较长节点名、多个 Mask 和键盘操作。

所有候选还必须保留以下状态，不只实现上图的正常态：

- 组或 Mask 上锁时删除禁用并可解释；锁仍可点击解除。锁使用中性图标，不使用蓝色。
- 加载或生成预览时保留稳定行高；失败用准确文字和现有错误语义，不显示旧图冒充新图。
- 未连接草稿按第 4 节显示状态和定位入口，不把候选列表当作新的草稿 owner。
- 未来混合组保持一个组头，Mask 创建禁用；不画虚构 Mask row。
- 面板折叠、选择和滚动沿用既有 UI 状态 owner；用户选择可见 row 不导致滚动跳动。
- 选中态、hover 与键盘焦点在两个主题下都能区分；任何候选都没有蓝色相框、蓝色
  选中底、蓝色侧边条或无意义的彩色装饰。

## 3. 双向选择与删除保护

### 3.1 一份选择状态

选择组等价于选择其 NodeId，右侧 adjustment stack 显示该节点参数。选择任一 Mask row
通过 `(NodeId, MaskId)` 同时确定 owner 节点和 Mask，打开现有 Mask 参数页及 Viewer
控件；两种视图高亮相同对象。节点视图可见时将目标滚动/平移到可见范围，不重排整张图。
从蒙版组选择不强制切换离开当前面板；再打开 Nodes 时立即显示同一目标。

节点图选择反向更新组选择；选择节点内的 Mask 还会展开对应组并显示该 Mask。跨面板
选择只定位一次；用户在已经可见的 row 上点击，不反复强制滚动，不触发渲染或新历史。
单纯选择组退出旧 Mask 的选中态并按原状态机结束编辑，避免右侧参数仍指向旧 owner。
如果选择导致进行中的编辑收尾，该编辑原本需要的 history/Quality 请求仍须完成；选择
本身不额外增加一次提交或渲染。验证时分别统计编辑收尾和定位操作。

继续使用现有 Node 与 Mask selection owner。新面板绑定其 stable IDs，不保存可独立
写回的第二份 selectedNode/selectedMask。异步结果必须匹配会话、图片、Version 和 revision。

### 3.2 删除锁的默认值和传播

| 对象 | 初始删除保护 | 行为 |
| --- | --- | --- |
| 默认 Color Grade 1 | 开启 | 仍可调参数、添加 Mask 和重命名；显式解锁后才可删除 |
| 默认 Color Grade 1 所属 Mask | 创建时默认开启 | 仍可改形状、强度、invert 和 enabled；每个 Mask 可单独解锁 |
| 其他新建 Color Grade 与其 Mask | 关闭 | 用户可独立上锁，开启后两种视图均执行保护 |
| Develop / DRT/Post | 固有不可删除 | 保留端点规则，不开放解除端点保护 |

默认节点的身份由 document owner 的默认节点语义识别；不得按显示名称、当前行号或
节点坐标判断。重命名、移动、Version 和 Paste 的 ID 重映射不能让保护错误转移到其他组。
用户已经解锁的值必须保存，不能在打开面板或重新加载投影时又自动上锁。

父节点上锁不锁定参数，也不自动改写全部 Mask 的独立锁。删除一个节点同时会删除它的
Mask，所以即使父节点已解锁，只要存在上锁的 Mask，整个删除操作也必须被拒绝，并准确
说明需要先解除哪一项保护。所有校验先完成再修改，不能删除了一半才遇到受保护 Mask。

节点与 Mask 的锁属于现有领域 owner 的持久数据；锁切换是可 Undo/Redo 的元数据编辑，
不改变像素，不触发 GPU render 或 Mask 缩略图重算。按钮、菜单、Delete 快捷键、节点草稿
删除和应用 service 必须执行同一校验，不能仅通过 QML 隐藏垃圾桶图标来实现保护。

历史回放要精确恢复当时的对象和锁状态：Undo 新建可撤销该次创建，Undo 删除恢复原 ID
及锁，锁切换本身可撤销。受信任的历史回放与新的用户删除请求在 owner 中明确区分；
不能把新上的锁变成无法 Undo 创建的障碍，也不能向普通 UI 暴露绕过保护的删除入口。

## 4. 当前源码事实与改动位置

以下来自 2026-09-16 工作树 HEAD `80d99e45` 的只读检查，不是 NM9 实施结果。

| 当前 owner / 入口 | 已有行为 | NM9 工作 |
| --- | --- | --- |
| [PipelineDocument](../../../../../alcedo_studio/src/include/edit/graph/pipeline_document.hpp) / [图命令](../../../../../alcedo_studio/src/edit/graph/pipeline_graph_commands.cpp) | 真实 DAG；已有 `AddCleanColorGrade(before_node_id)` 和 `RemoveColorGradeAndBridge` | 经现有 app/history 路径开放顶部原子插入和删除桥接；统一锁校验 |
| [ColorGradeNodeModel](../../../../../alcedo_studio/src/include/edit/graph/color_grade_node_model.hpp) / [MaskModel](../../../../../alcedo_studio/src/include/edit/mask/mask_model.hpp) | 调整、Mask 和针对字段的 owner 操作；当前无删除保护字段 | 最小增加节点/Mask 删除保护、默认值、序列化和字段操作 |
| [EditorNodeGraphProjection](../../../../../alcedo_studio/src/include/app/editor_node_graph_projection.hpp) | 已按主链发布节点和 Mask 身份；不包含完整参数 | 同一投影 owner 提供组顺序和必要显示字段；不新增整图或参数镜像 |
| [EditorNodeController](../../../../../alcedo_studio/src/include/ui/alcedo_main/album_backend/editor_node_controller.hpp) | 持有节点选择和现有投影；Add 创建未连接节点，Delete 不桥接；支持增量草稿 | 保留节点编辑器行为，为组面板增加意图明确的 app 命令；不把草稿按钮串调用作原子组操作 |
| [EditorMaskCreationAdapter](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_mask_creation_adapter.cpp) / [Mask controller](../../../../../alcedo_studio/src/app/editor_mask_creation_controller.cpp) | 已有 `selectMask(node_id, mask_id)` 和创建/编辑/删除状态机 | 复用选择和创建，统一 deletion guard 与错误反馈 |
| [节点抽屉](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskDrawer.qml) / [Mask row](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorNodeMaskTypeRow.qml) | 节点内部的类型 row、选择与删除入口 | 保持节点编辑器，复用 row 行为并加入删除锁；组面板增加位置/强度示意 |
| [Mask 参数页](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorMasksContextPanel.qml) | 所选 Mask 的参数控件 | 继续作为唯一完整参数页，接收两种视图的同一目标 |
| [typed changes](../../../../../alcedo_studio/src/include/edit/history/pipeline_edit_change.hpp) / [document history](../../../../../alcedo_studio/src/app/pipeline_document_history.cpp) | 文档操作的历史与恢复 | 覆盖锁切换、组插入/删除、失败回滚及 Version/Paste/reopen |

UI 只调用 app service/controller，不能直接调用 `PipelineDocument` 或领域容器。模型读取
通过已有 owner 的 const view 或已发布表示，在有效生命周期内使用；不复制整图后排序、
修改再覆盖回去。需要跨线程增加字段时，只扩展既有发布边界的最小字段，明确 owner、
revision 和生存期，禁止新建 parallel layer state。

节点编辑器可能有尚未连完整的草稿。组面板不能把它伪装成完整可渲染顺序，也不能通过
切换面板提交或丢弃草稿。显示明确的未完成连接状态及未接入节点的定位入口，禁止依赖
完整主链的组插入/删除和 Mask 创建；完成连接后，随同一个有效提交恢复 stack。复用
现有草稿的读取接口，不新增一份草稿。

## 5. 缩略图资源、失效与持久化

缩略图是 UI 展示所需的独立算法输出，由现有 Mask/render 资源 owner 生成，经过 app
边界交给 UI；不持有或复制 live document，也不保留 Grade 的 RGBA 结果。复用实际 Mask
求值/Union 语义，不能另外写一个看起来相近的 QML 椭圆或渐变代替实际 coverage。

- owner 以会话、NodeId、可选 MaskId、Mask 内容 revision 和 geometry revision 标识结果。
  只允许匹配当前身份与版本的结果发布；取消/失败不发布旧图冒充新图。
- 仅更新受影响的 Mask 和所属组；改 Exposure 等非 Mask 参数、选择、折叠、移动节点、
  切换删除锁不触发 coverage 重算。纯 Mask row 重排不改变 Union 结果。
- 生成和上传异步完成，不能每次 QML paint 都求值、同步等待 GPU 或读回全尺寸 Mask。
  使用所选原生后端已有结果或同语义的原生求值；失败显示真实状态，不改用其他后端。
- 256 级采样仅服务缩略图，不改变 Interactive/Quality/export 的分辨率、算法或数值精度。
  不另起一条照片渲染管线，不恢复 NM8 已取消的逐 Grade RGBA 缓存。
- 由 owner 限定缩略图数量和字节；面板不可见时停止新请求，切图/Version/关闭会话时释放
  过期资源。排队结果在 UI 销毁后仍可安全丢弃。
- 锁进入 document、typed history、checkpoint 和 Version/Paste 数据；缩略图不进入照片
  历史或项目序列化，展开/滚动等只进入既有 UI 状态 owner。

新增持久字段必须审计项目格式版本与现有解码规则，并在实施时写明缺字段的处理方式。
旧数据若需要转换，只能沿已批准的格式策略显式处理；不能偷偷补字段改变历史重放，也
不能在没有版本规则时宣称可以直接打开所有旧项目。NM9 验收必须记录实际版本及支持边界。

## 6. 子阶段与完成条件

各阶段按“目标 → 前置/文件 → 实施步骤 → 成功/失败链 → 验证与交付证据”执行。
步骤是待实现要求，不是完成记录；新增 API/文件建议名在真正落地后替换为实际名称。
后续阶段的集成验证不能代替前一阶段的基本正确性测试。用户尚未选择的布局保持未定，
其他已确定语义与不依赖布局的工作可继续按各阶段推进。

| 阶段 | 工作 | 依赖 | 状态 |
| --- | --- | --- | --- |
| NM9.1 | 共享组投影、顶部插入/桥接删除的 app 操作、草稿边界 | NM8 收口 | complete 2026-09-16 on `feature/nm91-mask-group-projection` |
| NM9.2 | 删除锁、默认保护、typed history 和格式规则 | NM9.1 | planned |
| NM9.3 | Mask Groups 面板、空抽屉、创建入口与双向选择 | NM9.1–NM9.2 | planned |
| NM9.4 | 组和单 Mask 缩略图、位置/强度显示、资源失效 | NM9.3 | planned |
| NM9.5 | Undo/Redo、Version、Paste、reopen 和失败恢复验证 | NM9.2–NM9.4 | planned |
| NM9.6 | 真实摄影任务的 UI/UX、像素一致性与性能验收 | NM9.1–NM9.5 | planned |

### NM9.1 — 一份 DAG 的两种投影

**目标与交付物：** 为两种视图建立共同的读取与命令入口，交付可验证的组顺序、顶部
插入、桥接删除和草稿状态。此阶段不决定 A–D 的布局，不增加独立可写的图层集合。

#### NM9.1.1 前置检查与文件入口

1. 记录实际 HEAD、工作树变更、现有项目格式和 Brush OFF 配置。重新核对第 4 节的源码
   表；发生重命名时更新链接，不能直接按计划中的旧签名接线。
2. 沿 [EditorNodeController](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp)
   的 `ActiveNodes` / `ActiveEdges`、`PublishDocument`、`SubmitNodeGraphTopologyEdit` 调用
   检查 committed 数据、未完成草稿和 UI 发布的所有者与线程。
3. 核对 [EditorNodeGraphProjection](../../../../../alcedo_studio/src/app/editor_node_graph_projection.cpp)、
   [EditorNodeGraphDraft](../../../../../alcedo_studio/src/app/editor_node_graph_draft.cpp)、
   [EditorSessionService](../../../../../alcedo_studio/src/app/editor_session_service.cpp)、
   [EditorHistoryMutation](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_mutation.cpp)
   和第 4 节图命令之间的顺序，确认谁完成校验、局部恢复、WAL 写入与成功通知。
4. 先运行本阶段对应的现有测试，记录修改前失败。以真实 default document、三个 Grade
   和含多个 Mask 的 document 为测试输入，不用只含字符串数组的假模型证明 DAG 顺序。

#### NM9.1.2 实施步骤

1. **定义只读组访问。** 从已验证的 Develop→DRT/Post 主链取全部 Color Grade；每行使用
   NodeId，子行使用 `(NodeId, MaskId)`。显示顺序来自主链和现有 Mask display order。
   显示字段只包含名称、类型、可操作性及后续阶段需要的最小状态，不复制 adjustment
   参数表或图像。若 Qt model 必须保存行映射，只保存指向同一发布版本的身份/索引，
   不保留跨发布失效的 document 指针。
2. **复用发布边界。** 两个面板读同一个 owner 的当前 revision。名称变更只通知对应
   组；Mask 列表变更只更新所属组；拓扑变更更新受影响的行/连接。检查现有投影相等
   判定，避免后续新增锁字段被 `SameProjectionContent` 忽略。普通参数滑动不重建组列表。
3. **新增顶部插入意图。** 应用入口名称明确表达“顶部插入蒙版组”，与节点图原有
   `addCleanColorGrade()` 的未连接节点操作区分。执行时重新读取 Develop 的真实后继：
   有 Grade 时插在首个 Grade 前，无 Grade 的合法主链则插在 DRT/Post 前。创建一个
   Clean Grade，复用名称计数器和图命令，原子替换一条旧 edge 为两条新 edge。
4. **复用现有 typed history。** 优先使用已有 `AddColorGradeChange` /
   `RemoveColorGradeChange` 和相应 batch 工厂表达完整操作。不要为了组面板再造平行
   history 格式；也不能先执行领域修改，再把相同 batch 当作未应用数据重放第二遍。
   提交记录准确包含 NodeId、插入点、边和名称计数器变化，失败不消耗名称或留下空组。
5. **新增组删除意图。** 与节点图的断开式草稿删除区分，组操作直接验证目标并调用
   桥接删除。保留被删节点/Mask 和边所需的既有可逆记录；这是历史恢复所需的数据，
   不另存整份 document。锁校验由 NM9.2 接入同一 owner 删除入口。
6. **规定通知顺序。** 完整文档和 history head 更新可见后，再发布组/图变化与新选择。
   每次结构修改只产生一个用户操作和既有 Quality intent；纯读取、改名显示、选择、
   折叠及定位不重复产生 render intent。单个 Grade 的原始内容不可因组序刷新被覆盖。
7. **处理完整性和过期请求。** UI 可提前禁用动作，owner 仍须再次检查会话 generation、
   图片/Version、命令状态及拓扑 revision。前序命令已改变插入点时，拒绝过期意图并
   刷新可用状态；不能按旧 row index 删除现在占据该行的其他节点。
8. **保留节点草稿。** 未完成连接时提供状态、未接入节点身份和“定位 Nodes”动作。
   组插入/删除及 Mask 创建按第 4 节禁用；切换面板不发布、不丢弃草稿、不私自桥接。
   只有既有草稿 owner 确认真正完整并提交后，才恢复有效 stack。

#### NM9.1.3 成功链与失败链

**成功链：** 组意图 → session admission → history/document owner scoped read → 验证目标
和主链 → 最小 typed change 与局部修改 → WAL/history 成功 → 发布同一 revision → 两视图
更新 → 选择新组或删除后的合法目标 → 结构修改的 Quality render。

**失败链：** 旧身份、重复 NodeId、计数器耗尽、无效 edge 或 history 失败 → 按既有 owner
恢复受影响节点/边/计数器 → 不发布成功状态、不产生新 head → 显示原始错误。UI 投影失败
应明确报告并按同一已提交 revision 重建视图，不能因此重复提交已成功的领域操作。

#### NM9.1.4 验证与完成条件

| 用例 | 必须观察的结果 |
| --- | --- |
| 1/3/8 Grade，名称乱序或重名，画布位置移动 | 组序仍等于真实主链，NodeId 不变，无 Mask 节点不丢失 |
| 顶部插入；合法无 Grade 主链插入 | 恰好新增一个 Clean Grade、两条连接和一次提交；初始像素不变 |
| 删除首/中/末 Grade 和最后一个可删除 Grade | 前后继准确桥接，端点保留；按图验证器处理合法零 Grade 主链 |
| 插入/删除失败及旧 revision 命令 | 图、名称计数器和 head 未发生部分变化，选择不跳到无效对象 |
| Nodes 未创建或其 Loader 已销毁 | app 操作正常提交，重新打开 Nodes 显示同一结果 |
| 草稿增加未连接节点后切换面板 | 草稿仍在、可定位，禁止把不完整图显示成已生效 stack |

优先扩展 `EditorNodeGraphProjectionTest`、`EditorNodeGraphDraftTest`、
`EditorSessionNodeCommandTest`、`PipelineDocumentDefaultNameTest` 和现有 topology history
用例。记录主链、序列化变化、提交次数及新增测试数；结构正确和失败不留半状态均通过后
才标记 NM9.1 complete。真实像素比较可复用现有 native fixture，不能用组数相等代替。

##### Phase NM9.1 completion record (2026-09-16)

**Status:** complete on branch `feature/nm91-mask-group-projection`（Windows debug build，
MSVC + CUDA 12.8，Qt 6.9.3，Brush OFF）。NM9.2 删除保护未实现，按范围要求保留给后续阶段。

**Implementation.**

- **共享只读组投影。** `EditorMaskGroupRow` / `EditorMaskGroupMaskRow` /
  `EditorMaskGroupSnapshot` 与 `EditorNodeGraphProjection::BuildMaskGroups` 在
  `editor_node_graph_projection.{hpp,cpp}`：沿 `ImageBackboneNodeIds()` 的真实
  Develop→DRT/Post 主链取全部 Color Grade；无 Mask 的 Grade 保留为空抽屉。组身份为
  `NodeId`，Mask 子行按 `(NodeId, MaskId)` 键控；名称/enabled 直接来自节点模型。
  脱离主链或未连接的 Grade 被省略，无效主链抛出错误而不是编造顺序。投影不携带
  adjustment 参数负载或任何 UI-only 状态，参数滑动不重建组行。
- **顶部插入。** `CaptureAddColorGradeAtTopChange`
  （`pipeline_document_history.{hpp,cpp}`）在 owner 线程上重新解析 Develop 的真实
  后继；`expected_successor_id` 与当前图不符则拒绝，不消耗名称计数器。新名复用既有
  color-grade 计数器，不重编号既有节点；一个 `AddColorGradeChange` 原子完成
  “新增节点 + 一条旧 edge 换两条新 edge”。
- **桥接删除。** 复用 `CaptureRemoveColorGradeChange` / `MakeRemoveColorGradeBatch`：
  只删 Color Grade，端点不可删；删除关联 scene-image 边并把前驱直接连到后继；
  删除最后一个 Grade 留下合法的 `Develop → DRT` 文档。
- **命令链路。** 新增 `EditorSessionCommandKind::{InsertColorGradeAtTop,
  RemoveColorGradeAndBridge}` 与 `expected_successor_id` 字段；命令经
  `IEditorSessionBackend` 排队，owner 线程在 `EditorSessionService` 中复查会话
  element/image 身份与 interactive 状态；`IEditorHistoryPort` →
  `EditorSessionHistoryPort` → `EditorHistoryMutation` 在 live render lock 内
  构造 typed batch 并交给 `PublishAppliedTypedBatch`。action policy 将两个新 kind
  归入 `CommitAdjustment`。
- **失败原子性。** `PublishAppliedTypedBatch` 现在在 apply 前克隆整份
  `PipelineDocument`；prepare、WAL append、live mirror 或 committed refresh 任一失败时
  按克隆字节级恢复（节点/边容器顺序、名称计数器、history head 均不部分变化），并还原
  原 panel 投影节点。`document_already_at_after` 的 preview 收尾路径不受影响。
- **零 Grade 边界。** `ProjectPanelFieldsForState` 在所选投影节点已被删除时清除该
  节点并回退 current-panel 路由；grade-owned panel adapter 带元数据标记，document
  无 Color Grade 时被跳过，Develop/DRT/Post 与 geometry 字段继续正常投影。
- **草稿边界。** `EditorNodeGraphDraft::DetachedNodeIds()` 暴露主链外节点；
  `EditorNodeController` 新增 `maskGroups`、`detachedDraftNodeIds`、
  `can_edit_mask_group_structure`、`insertMaskGroupAtTop`、`removeMaskGroup`、
  `locateNodeInGraph`；`EditorMaskCreationAdapter::CanAuthorMasksFor` 在草稿存活期间
  返回 false。切换面板不丢弃草稿；草稿不完整时组插入/删除与 Mask 创建均被拒绝。

**序列化变化：** 无新格式版本——两个命令复用既有 `AddColorGradeChange` /
`RemoveColorGradeChange` 的 JSON/validate/apply/inverse/hash 路径；名称计数器变化
随 add change 记录。

**Primary call chains.**

- 插入：`insertMaskGroupAtTop`（draft/generation 检查，anchor = `snapshot_.nodes[1]`，
  新 id `grade.<uuid>`）→ `EditorSessionController::SubmitInsertColorGradeAtTop` →
  `IEditorSessionBackend::InsertColorGradeAtTop` → 排队 `EditorSessionCommand` →
  `EditorSessionService::InsertColorGradeAtTop` → `IEditorHistoryPort::
  InsertColorGradeAtTop` → `EditorHistoryMutation::InsertColorGradeAtTop` →
  `CaptureAddColorGradeAtTopChange` → `MakeAddColorGradeBatch` →
  `PublishAppliedTypedBatch` → history 提交 + live mirror + committed refresh →
  `PublishDocument` / `AdoptCommittedDocument` 重建两个投影 → `MaskGroupsChanged` →
  `selectNode(new_id)`。
- 删除：`removeMaskGroup` → `SubmitRemoveColorGradeAndBridge` → 同层
  `RemoveColorGradeAndBridge` → `CaptureRemoveColorGradeChange` →
  `MakeRemoveColorGradeBatch` → `PublishAppliedTypedBatch` → 投影重建 → 后继选择。

**What was proven (executed tests, debug build):**

| 覆盖点 | Target / 测试 | 结果 |
| --- | --- | --- |
| 组序=真实主链、空抽屉、NodeId/名称/enabled 身份、`(NodeId, MaskId)` 键控、detached 省略、参数编辑不重建、generation 检查、无效主链拒绝 | `EditorNodeGraphProjectionTest`（`MaskGroups*` 9 个新用例） | 16/16 PASS |
| 草稿 DetachedNodeIds 与边界 | `EditorNodeGraphDraftTest`（`DetachedNodeIds*` 2 个新用例） | 16/16 PASS |
| service 层 admission、一次 history change、journal 失败不留状态、非交互拒绝 | `EditorSessionNodeCommandTest`（4 个新用例） | 8/8 PASS |
| 顶部插入+桥接删除一次提交、undo/redo 回放、journal 失败文档/head/计数器字节级恢复 | `EditorSessionHistoryPortTest`（`MaskGroupTopInsertAndBridgeRemoveCommitOnceAndReplayThroughUndo`、`MaskGroupJournalFailureLeavesDocumentHeadAndCounterUntouched`） | 79/79 PASS |
| 名称计数器消耗/不重编号、stale successor 拒绝不耗计数器 | `PipelineDocumentDefaultNameTest`（`TopInsertUsesCounterWithoutRenumberingExistingGrades`、`StaleTopInsertSuccessorRejectsWithoutConsumingCounter`） | 8/8 PASS |
| 控制器组发布、插入/删除提交与选择、草稿与端点拒绝、stale generation 拒绝、切面板保留草稿 | `EditorNodeSelectionLayoutTest`（6 个 `MaskGroup*`/`PanelSwitch*` 用例） | 61/61 PASS |
| 最后 Grade 删除后 panel 投影仍有效（grade-owned 字段跳过） | `EditorPanelProjectionTest` | 5/5 PASS |
| 干净顶部插入像素不变（真实 RAW，CUDA debug） | `PipelineDocumentRenderTest.CleanTopInsertedMaskGroupLeavesPixelsUnchanged` | 11/11 PASS（约 135 s） |

每次被接受的插入/删除产生恰好一个 typed batch 与一个 history commit；测试中通过
commit 计数与 head hash 断言。渲染复用既有 `RenderRouted`/Quality intent 路径；
组投影发布与 UI 状态不产生 render intent。

### NM9.2 — 可恢复的删除保护

**目标与交付物：** 交付 owner 强制执行的删除保护、可撤销的锁切换、准确的默认保护身份
和完整的格式读写规则。锁不是 QML 临时属性，也不改变调整参数的可编辑性。

#### NM9.2.1 前置检查与文件入口

1. NM9.1 的插入/删除 owner 路径已具备可验证的原子行为。列出所有删除入口，包括
   节点草稿、完整 `NodeGraphTopologyChange`、组删除、Mask 行、Viewer Delete 和 service。
2. 检查第 4 节 `ColorGradeNodeModel` / `MaskModel` 与默认 document 工厂，确认默认节点
   身份在 root、Version 和 Paste 中如何保存；当前选择中出现 `grade.primary` 并不能
   证明所有新历史或重映射后的文档仍具有相同字符串。
3. 读取 [typed batch 定义](../../../../../alcedo_studio/src/include/edit/history/pipeline_edit_batch.hpp)、
   [格式版本表](../../../../../alcedo_studio/src/include/edit/history/pipeline_history_format.hpp)、
   [root/checkpoint 编解码](../../../../../alcedo_studio/src/edit/history/pipeline_document_checkpoint.cpp)
   和 `pipeline_edit_change_json.cpp` / `pipeline_edit_change_validate.cpp`，列出必须同步
   更新的序列化、验证、apply、inverse、hash、显示文本和 transfer 分支。

#### NM9.2.2 实施步骤

1. **定义领域字段。** 在现有节点和 Mask owner 中各增加独立删除保护值与针对字段的
   操作；名称清楚表达 deletion protection，不复用 enabled、mix、read-only 或 adjustment
   锁。UI 只拿到显示值及允许执行的动作，不保留独立的锁状态字典。
2. **固定默认身份。** 优先沿用现有可持久、可重映射的默认节点标识。如果审计确认
   不存在，给 document 增加一个最小的默认 Grade 身份字段，并定义目标类型/唯一性
   验证、删除后的空值、Undo 恢复和 Paste 映射；不能以显示名猜测，也不能新增一份
   默认节点数据副本。用户删除默认节点后，不把其他组自动升级为默认节点。
3. **只在创建时应用默认值。** 默认 Grade 上锁；向该默认 Grade 创建的新 Mask 默认
   上锁，其他新建对象默认未锁。父 Grade 已解锁不自动改写其默认身份，新 Mask 仍按
   默认身份初始化；既有 Mask 的锁独立保存。读文档、发布投影和面板重建不重新套默认值。
4. **增加最小 typed metadata change。** 节点锁记录 NodeId 和 before/after bool；Mask
   锁记录 NodeId、MaskId 和 before/after bool。复用能准确表达该字段的既有 change，
   否则增加职责明确的新 kind。相同值写入是 no-op，不产生无意义的 history commit。
5. **统一删除判断。** 在 document owner 的用户命令 admission 检查端点、节点自身锁
   和所属 Mask 锁；检查整个变更实际移除的对象，不能仅检查 QML 声称选中的对象。
   含多个移除对象的 batch 要全部预检后再执行。父节点解锁不能连带删除上锁 Mask。
6. **保留失败解释。** 返回被保护对象的 stable ID 与可本地化原因，UI 可显示对应名称。
   失败不自动解锁、吞掉错误、移除部分子项，或绕到另一删除入口重试。
7. **明确历史与新命令边界。** 用户删除按当前锁校验；已验证历史的 forward/inverse
   恢复准确记录，不再当成新用户删除。Undo 创建默认上锁的 Mask 可以撤销创建，Redo
   恢复原 ID 和锁。不能向 QML 暴露一个 `ignore_lock` 参数来复用受信任回放入口。
8. **分离元数据和像素失效。** 锁更新需要 dirty/save/history 和两视图通知，但不改变
   scene 内容、Mask coverage、静态执行计划或 GPU 参数。审计通用 Undo/Redo 调度：只
   撤销锁时同样不请求像素渲染；混合批次含真正像素修改时仍执行所需 render intent。
9. **一次完成格式修改。** 按当前显式版本切换规则，列出本次实际变更的 project、
   document、typed batch、root、checkpoint、WAL、hash 和 transfer 格式版本及兼容表，
   同时更新对应常量、解码器与期望 JSON。缺少必需锁字段、类型错误、无效默认身份和
   不支持版本真实失败；未获授权不新增旧项目迁移或静默补默认值路径。
10. **贯穿保存与复制。** 保存 bool 实值而非“未设值则跟随父节点”；Version 恢复其自身
    记录，Paste 同时重映射默认身份、节点/Mask 引用与已有锁值。不得在目标图中多出两个
    默认身份；用既有完整替换/保留规则决定归属，实施时写清实际规则和测试结果。

#### NM9.2.3 成功链与失败链

**成功链：** 锁动作 → owner 验证身份与当前值 → typed metadata change → history/WAL →
文档字段和投影可见 → 两视图更新锁及删除可用性；photo render 和缩略图请求计数不增加。

**删除失败链：** 用户请求 → 检查节点与所有将被移除的 Mask → 发现保护 → 返回具体原因
和目标 → 图、history、选择不变。历史解码或回放失败沿既有恢复 owner 报告，不借解锁
绕过不合法数据；恢复本身失败时不得继续宣称文档可编辑。

#### NM9.2.4 验证与完成条件

| 用例 | 必须观察的结果 |
| --- | --- |
| 默认/普通 Grade 与新建 Mask | 默认值符合第 3.2 节；读取、改名和重排不改变锁或默认身份 |
| 节点锁开/关 × 子 Mask 锁开/关 | 四种组合逐一验证，任一将被删除对象上锁都拒绝整个删除 |
| 锁定后曝光、opacity、invert、enabled、控制点修改 | 参数与形状可编辑，保护只限制删除 |
| 锁相同值写入、锁 Undo/Redo | no-op 不提交；有效切换一次提交；无 GPU/coverage/plan 失效 |
| 键盘、row、组、Nodes 草稿和直接 service 删除 | 都由同一 owner 拒绝受保护目标，不能从另一入口绕过 |
| Undo 新建、Undo 删除、Redo、Version/Paste/reopen | 原 ID 与锁精确恢复，显式解锁不会在重开后丢失 |
| 非法 bool、缺字段、旧格式、错误默认身份 | 在规定解码边界失败；不会得到看似有效但丢失保护的数据 |

扩展 `GpuDagModelGraphTest` 中的模型用例、`PipelineEditBatchTest`、
`PipelineDocumentCheckpointTest` 以及 `EditorSessionHistoryPortTest` 的文档/Version/Paste
用例。提交阶段记录必须包含格式兼容表、所有删除入口清单和元数据无渲染证据；不能把
持久化正确性全部推给 NM9.5，后者负责跨模块组合验证。

### NM9.3 — 面板、创建与选择

**目标与交付物：** 交付可操作的 Mask Groups 页面，复用节点/Mask 选择与创建流程，
完成空态、锁、键盘、Loader 生命周期和 monochrome 表达。NM9.4 接入真实缩略图结果。

#### NM9.3.1 布局输入与文件入口

第 2.6 节 A–D 仍未选定。模型、命令和选择接线可依据已锁定语义推进；把某个候选实现
为生产布局前，需要用户确定采用哪一种，不能在本次细化计划时代选。最终文件记录所选
候选及获准改动，不把所有候选实现为四套可切换产品 UI。

主要入口为 [EditorWorkspaceRail.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspaceRail.qml)、
[EditorWorkspace.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorWorkspace.qml)、
[EditorAdjustmentHeader.qml](../../../../../alcedo_studio/src/ui/alcedo_main/qml/EditorAdjustmentHeader.qml)、
第 4 节 Node controller、Mask adapter、抽屉和参数页。拟新增 `EditorMaskGroupsPanel.qml`
及必要的组/row delegate；这些是计划文件名，不是已存在文件。共享 token 仍由 AppTheme
拥有，QML 注册由 [主应用 CMake](../../../../../alcedo_studio/src/ui/alcedo_main/CMakeLists.txt) 管理。

#### NM9.3.2 实施步骤

1. **接入同一侧栏。** 增加独立 Mask Groups 工具页，Nodes 入口保留。更新 tool-panel
   page 的允许值、rail action、active body、Loader 和状态保存位置；它不是右侧第八个
   adjustment 参数页，不要把工具页 key 注册到错误的 `NormalizeAdjustmentPanel`。
2. **复用常驻 controller。** Nodes 与新面板使用同一个 Node controller、Mask adapter
   和 session。控制器生命周期不得依赖某个面板 Loader；新组操作在 Nodes 从未打开时
   也能执行。新列表适配器只能转发同一 owner 的读取和命令，不能拥有另一份 DAG。
3. **实现组与 row。** 组头显示名称、合成预览区域、独立锁/删除操作；空组保留抽屉。
   子行显示 Mask 类型、单 Mask 预览区域、opacity 和锁/删除。折叠按选定候选执行；
   长名称省略并提供完整 accessible name/tooltip。NM9.4 尚未完成时不伪造 coverage 图。
4. **接入两个创建入口。** 新建组调用 NM9.1 的完整插入命令，成功后选中新组。现有
   Radial/Gradient 工具优先使用明确选择的可编辑 Grade；仅真正没有目标时按第 2.3 节
   默认定位顶部 Grade。选择端点、混合组或存在不完整草稿时显示准确禁用原因。
5. **统一节点和 Mask 选择。** row 一次传递 `(NodeId, MaskId)`；按原状态机完成上一个
   编辑后，协调节点高亮、Mask selection、参数页与 Viewer。不能先开放新节点参数而
   仍保留旧 Mask target；owner 切换期间禁用会产生错误目标的写操作，完成后统一通知。
6. **处理反向选择与移除。** 节点图选择同步组头；节点内 Mask 选择展开并显示该组。
   删除后使用既有合法后继/前驱选择规则并清除失效 MaskId；Undo/Version 从当前有效
   身份恢复，不依据旧 row index。隐藏的 Nodes 在重新创建时读取当前选择，不覆盖它。
7. **避免信号回环。** UI 绑定 owner 选择作为显示输入，只有真实用户操作提交选择意图。
   相同 ID 重复选中为 no-op。参数页载入、投影重建或 QML delegate 初始化不能再回写
   一次选择/参数命令；controller 不把两个面板的镜像通知当成两个用户操作。
8. **隔离按钮事件。** 锁、删除、展开与 row 主选择拥有明确 hit area；点击删除不再
   触发选中或父组删除。禁用删除仍能从可访问描述得知原因；Delete 快捷键按当前焦点
   和有效对象路由，文本输入中不删除组或 Mask。
9. **保持滚动和 UI 状态。** 在已有 UI owner 中按图片/Version/NodeId 保存必要的展开
   与滚动状态，Loader 销毁后可恢复。只在目标不在可见区域时做最小定位；不在每次
   SelectionChanged 后重置 model 或强制 `ListView.Contain`。
10. **落实 monochrome 和布局约束。** 浅底深字使用既有 selected fill/ink；owner 组
    只作次级强调。锁、组头、预览、侧栏激活均不使用蓝框/蓝底/蓝侧条。保持 Basic、
    IconActionButton、260–460 px 侧栏范围、Viewer 最小空间和 `reduceMotion` 行为；新增
    数值先加入 AppTheme 与 DESIGN.md，同次提交注册全部新 QML 和图标资源。
11. **补齐状态与可访问性。** 无图、加载、命令执行、未完成连接、失败、无组、空 Mask
    抽屉都有明确表现。为组名、展开、创建、锁、删除及 Mask 类型提供本地化文字、Tab
    次序和键盘焦点。颜色不作为唯一线索；缩略图是补充信息，不能替代文字辨认与操作。

#### NM9.3.3 成功链与失败链

**选择成功链：** 组/Mask row → 既有选择 owner → 必要的上一编辑收尾 → 原子确定
NodeId/MaskId → 节点高亮、参数页、Viewer 控件 → 两视图显示，定位本身无额外渲染。
**创建成功链：** 既有 Mask 工具 → 当前组目标 → 原创建状态机 → 确认提交 → 两视图新增
同一 Mask；取消按原状态机还原，不留下空 row 或额外 Grade。

**失败链：** 目标已删除、会话切换、锁拒绝、草稿不完整或创建失败 → owner 返回原因 →
不显示操作成功、不把参数页指向失效对象；恢复与当前文档一致的可操作状态。纯 UI
重建失败与领域提交失败分开报告，不能用再次提交弥补 delegate 未刷新。

#### NM9.3.4 验证与完成条件

- 真实 Loader 装载生产 QML，验证从未打开 Nodes 时新建组/Mask、两视图轮流操作和
  右侧参数路由；不以 mock 列表单独显示正常代替真实 controller 接线。
- 覆盖组选择、Mask 选择、重复选择、修改中切换 owner、删除选中目标、Undo 后定位、
  端点/不完整草稿禁用、创建确认/取消及失败；分别统计编辑收尾与选择产生的命令数。
- 在 A/B 两张图、两个 Version 和多组展开之间往返，验证 selection 与 contentY。
  点击可见中间 row 不跳动；展开、锁和删除按钮不冒泡触发其他动作。
- 覆盖两个主题、260/320/460 px 面板宽度、960×640 最小窗口、长名称、DPR 1/1.5/2、
  键盘和 `reduceMotion`。通过属性断言及必要截图检查无蓝色选中装饰、文字图标反相、
  hit area、裁切和焦点；不能只断言颜色来自某个 token，因为该 token 也可能是蓝色。
- 扩展 `EditorNodeSelectionLayoutTest`、`EditorNodesPanelQmlTest`、
  `EditorNodeDelegateQmlTest`、`AnalyticMaskCreationTest` 和 `WorkspaceShellTest`；为新面板
  增加职责明确的 QML fixture 并记录真实 CTest 名称。布局和共享交互全部通过后，本阶段
  才 complete；缩略图求值的剩余工作明确归 NM9.4，不把空框称为已完成预览。

### NM9.4 — 实际 coverage 的微型预览

**目标与交付物：** 为组头和各 Mask row 提供实际 coverage 的小图，准确表达位置、
形状与强度；交付原生求值/缩采样、异步发布、缓存上限、失效规则和像素比较证据。

#### NM9.4.1 前置检查与文件入口

1. NM9.3 已确定显示位置和生命周期；确认组图与单 Mask 图均取 Grade Mix 之前的
   coverage，`opacity` 是 Mask 参数，不能把它混成 Grade Mix 或卡片透明度。
2. 读取 [CompiledMaskStack](../../../../../alcedo_studio/src/include/edit/runtime/compiled_mask_stack.hpp)
   的 source/effective/union 输出及 [GradeMaskCoverage](../../../../../alcedo_studio/src/include/edit/mask/grade_mask_coverage.hpp)
   的空集合、全关闭和 Union 定义。后者可用于理解现有语义，不授权把产品缩略图改走
   CPU 求值；测试期望也不能只调用被测生产求值器再与自身比较。
3. 跟踪 [CUDA Mask pass](../../../../../alcedo_studio/src/edit/runtime/cuda/cuda_mask_pass.cu)、
   [OpenCL Mask pass](../../../../../alcedo_studio/src/edit/runtime/opencl/opencl_mask_pass.cpp)、
   [Metal Mask pass](../../../../../alcedo_studio/src/edit/runtime/metal/metal_mask_pass.mm)
   和已有 geometry/reference-space 映射，确认纹理格式、边界采样、completion 和最后
   reader 的资源释放位置。只改 Mask 展示所需路径，不重构 RAW 解码或 Grade 算法。

#### NM9.4.2 实施步骤

1. **固定采样定义。** 写明缩略图宽高、最长边上限 256、照片比例、黑色留边、像素中心
   与 reference-space 的映射、旋转/裁剪顺序及灰度编码。缩略图表示线性 coverage，
   不对它应用照片的 DRT/LUT，也不随所选 Grade 的曝光变化。小照片不得被无谓放大求值。
2. **选择原生生成入口。** 有匹配内容与 geometry revision 的现成 effective/union
   结果时，经 Mask owner 的安全读取产生小图；需要新求值时，使用同一选定后端和既有
   Mask 语义，在 session 允许的串行访问边界执行。分别写明复用与新求值的条件，
   不能读取已经复用给下一帧的工作纹理，不能创建独立 document/executor。
3. **定义结果身份。** 单 Mask 图包含会话/图片/Version、NodeId、MaskId、该 Mask
   内容 revision、geometry revision 和采样规格；组图使用能覆盖成员增删和所有有效
   Mask 参数的组合 revision。不要用每次都变化的整份 document revision 作为唯一
   缓存键，否则曝光、锁和改名也会错误触发 coverage 重算。
4. **明确独立输出的用途。** 小图是 UI 所需的派生输出，可以拥有独立像素存储；在其
   定义处写明由谁创建、是否只读、有效尺寸、输入版本和释放点。它不携带 document
   镜像、不写回 Mask 参数，也不构成逐 Grade RGBA 跨帧缓存。
5. **接入有界调度。** 根据可见组头和展开的可见 rows 请求图像，同一会话只在既有串行
   owner 边界启动一个求值批次；相同目标的待处理请求保留最新 revision，已运行批次
   按真实 completion 安全结束。列表快速滚动或连续拖动不能积累无界任务。
6. **实现精准失效。** 按下表处理 source、opacity、invert、enabled、成员关系和 geometry；
   失效只改变对应显示资源。没有像素变化的 UI 操作不触发照片渲染，也不重新编译图。
   显示订单与求值集合分开，Mask row 重排不使 Union 内容失效。
7. **安全送到 Qt。** 复用现有支持的图像/texture provider 边界，GUI/渲染线程只消费已
   完成的小图。所有权覆盖 QSG 的读取期；替换图后，旧资源等实际 reader 释放再回收。
   若传 CPU 展示图，只读回缩略图尺寸，不能同步下载全尺寸 Mask 再缩小。
8. **防止过期发布。** completion 回来后再次检查目标、revision 和面板/会话状态。
   切图、checkout、删除、关闭及新编辑都可能让结果过期；过期图不覆盖新目标、不通过
   相同 row index 发布给别的 Mask。删除后 Undo 恢复相同 ID 时仍须检查新的有效版本。
9. **定义 UI 终态。** 区分无 Mask、请求中、ready、过期和失败。没有 Mask 不请求合成图；
   全部关闭显示有效全黑图并保留关闭状态。请求失败时清除“当前有效”标记并显示真实
   原因，不用空图或先前版本伪装 ready，也不自动改后端或降低照片质量。
10. **界定内存与释放。** 在实现记录中给出实际缓存项数/字节、在途小图数及对应上限，
    至少区分 owner 像素、上传/下载暂存和 QSG reader。隐藏面板停止新请求并取消未启动
    工作；切图/Version/关闭按安全边界清除过期项。大列表不能为所有离屏 Mask 永久留图。
11. **补充低开销观测。** 记录请求、合并、命中、求值、过期丢弃、失败、发布、读回字节
    和当前/峰值内存，沿现有 diagnostics 汇总。不要在每个 paint 或 GPU pass 同步打印。

#### NM9.4.3 失效与重用表

| 变化 | 单 Mask 小图 | 组的合成小图 | 照片渲染 |
| --- | --- | --- | --- |
| Mask 形状、位置、feather、opacity、invert | 该 Mask 更新 | 所属组更新 | 原编辑流程决定 |
| Mask enabled | 该 row 有效状态更新，按既定关闭表示处理 | 所属组更新 | 原编辑流程决定 |
| 增加/删除 Mask | 新建/释放对应项，其他 Mask 可复用 | 所属组更新 | 结构编辑原有请求 |
| 改名、锁、row display order、组折叠或选择 | 内容复用；必要时按可见性请求尚无的小图 | 内容复用 | 无额外请求 |
| Exposure、Saturation、Grade Mix | 内容复用 | 内容复用，因为取 Mix 前 coverage | 原参数编辑流程决定 |
| 裁剪、旋转、参考尺寸/采样规格改变 | 所有受影响可见项更新 | 受影响组更新 | 原 geometry 流程决定 |
| 切图/Version、session 重建 | 拒绝旧结果，按新身份读取 | 同左 | 原会话流程决定 |

此表仅描述当前 Radial/Gradient。未来内容相关 Mask 不自动沿用“曝光不失效”的规则，
必须由那项功能定义真实输入依赖；NM9 不开放未实现的 Mask 类型。

#### NM9.4.4 成功链与失败链

**成功链：** 可见目标/内容变化 → 读取 owner 的身份与 revision → 合并排队 → 安全读取
已有原生结果或原生求值 → 小尺寸输出完成 → 再验身份/revision → Qt 发布 → reader 完成
后回收旧输出。请求中不阻塞 GUI，不增加逐 pass host wait。

**失败链：** 分配、原生求值、缩采样、传输或 Qt 发布失败 → 标记该目标失败并归还资源 →
输出真实错误；过期 completion 仅安全丢弃。展示资源失败不伪造一次文档编辑，仍需证明
主照片资源、下一次合法请求及会话关闭可继续遵守既有生命周期规则。

#### NM9.4.5 验证与完成条件

- 用独立解析期望验证黑/白内部点、外部点、边缘、feather 和反相；Union 覆盖相离、
  相交、完全重叠及不同 opacity。空集合不显示假图，全关闭集合明确为零 coverage。
- 覆盖横图、竖图、方图、奇数尺寸、裁剪、旋转、边缘外 Mask、极小有效 Mask 和多个
  强度值。先固定采样规则，再根据量化/过滤推导绝对容差；报告最大/平均误差、失败
  坐标和输入，不能在失败后单纯扩大容差。相同采样定义下与真实原生 coverage 比较。
- 用受控 completion 顺序测试旧图后返回、删除后 Undo、切图/checkout 后返回、Loader
  已销毁、关闭会话和失败后重试。不靠任意 sleep 制造竞争窗口。
- 在多组、多 Mask、持续拖动和滚动中验证排队与内存上限；停止输入后完成最新 revision，
  关闭/重开后没有重复连接、无界缓存或仍占用会话的任务。记录真实 request/byte 计数。
- 修改涉及的 CUDA/OpenCL/Metal 路径分别使用对应原生测试和真设备，不能以某后端通过
  代替其他后端。现有测试入口为 `GpuDagCudaMaskTest`、`GpuDagOpenClGradeTest` 与
  `GpuDagMetalGradeTest`；增加小图发布/失效用例并记录实际 fixture 和平台结果。
- 完成记录必须包含取样定义、资源 owner 图、失效表、原生像素结果及受控竞争测试。
  未验证的平台明确列为未验证，不能把未完成的小图能力混入 NM8 已关闭的历史范围。

### NM9.5 — 持久化与失败行为

**目标与交付物：** 验证 NM9.1–NM9.4 组合后的完整编辑生命周期，并修复发现的问题。
本阶段不能只重复模型单测；必须使用真实 history/session owner、项目保存与重新打开。
现有 Paste 的兼容性属于回归范围，NM10 的新 transfer 产品能力仍未定义。

#### NM9.5.1 输入与文件入口

准备一个真实可保存的项目，包含 A/B 两张不同尺寸或方向的照片、每图至少两个 Version，
以及默认 Grade、普通组、空组、Radial、Gradient、混合锁值和用户重命名。记录可重建的
参数与操作顺序；不能依赖开发机器残留的 UI 设置或已有缓存才让场景通过。

主要入口：NM9.2 格式与 owner 文件、[document transfer](../../../../../alcedo_studio/src/app/document_transfer.cpp)、
[editor history transfer](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_transfer.cpp)、
[checkpoint store](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_checkpoint_store.cpp)、
现有 session 生命周期/恢复路径，以及 WorkspaceRail 和 NM9.4 completion 发布边界。
优先复用已有项目/history fixture，不新增另一个简化存储实现来代替 DuckDB/WAL 路径。

#### NM9.5.2 实施与验证步骤

1. **建立独立期望。** 按已知输入列出图顺序、NodeId/MaskId、名称计数器、参数、默认
   身份、锁、Mask 顺序与最终像素的期望；用保存的操作描述重现。序列化比较排除 UI
   滚动和缩略图缓存，避免把派生显示数据误当成照片编辑状态。
2. **跨面板连续编辑。** 在组面板建组，在 Nodes 改名/连接，再在组面板添加 Mask、
   调整、上锁、解锁、删除。逐步核对两侧目标、一次操作一次提交、正确 render intent
   与图合法性；不能只在最终状态做一次 `ToJson()` 比较。
3. **逐步 Undo/Redo。** 沿整个序列退回 root，再前进到最终 head。检查锁元数据步骤
   不渲染，像素操作正常渲染，删除恢复原 ID/边，名称计数器可逆，Mask 创建的撤销不被
   默认锁挡住。Undo 后另作编辑时，沿原有分支规则处理，不错误覆盖其他 Version。
4. **Version checkout。** 两个 Version 使用不同组顺序、默认节点存留状态和锁值。
   反复 checkout 后核对真实 document 和 UI 选择；目标缺失时依既有规则选合法节点，
   不能仅清空高亮而让参数继续写旧 NodeId。纯 checkout 不重置已保存的显式解锁值。
5. **现有 Paste 回归。** A 的当前内容通过正式 transfer 路径传给 B，确认按既有语义
   创建目标 Version、保留 B 的 RAW/Develop 身份并映射节点/Mask/默认身份/锁。源图
   继续编辑不改变已粘贴内容。失败或取消不留下半个 Version、半套边或新默认保护对象。
6. **真实保存和重开。** 保存、关闭 session/项目并释放内存，再从磁盘重新打开。分别
   检查正常保存、已有 checkpoint、checkpoint 后存在 WAL 以及既有恢复流程支持的
   中断位置。不能用同一内存对象再次显示面板冒充 reopen；锁和值必须来自持久记录。
7. **控制异步交错。** 在 pending Mask 编辑、结构命令或小图请求前后切换 A/B、切换
   Version、关闭面板并打开 Nodes。使用测试端 completion latch/显式推进队列安排
   先后顺序，验证旧 target 不写入新图，原图应提交的收尾仍归属于原图。
8. **注入真实失败边界。** 至少覆盖写入/提交失败、无效 target、保护拒绝、失效 revision、
   格式错误和 native thumbnail failure。断言 document/head/selection 的前后状态及
   资源释放；沿既有恢复机制执行，不能新增 catch-and-continue 或替代算法来让测试通过。
9. **检查完整草稿生命周期。** 同一图片内只切换 Nodes/Groups 不改变草稿；离开图片或
   checkout 时按已落地的草稿生命周期规则处理，记录具体结果。不得把“切面板保留”
   错套成“草稿带到另一张图”。失败后回到原图时也不把错版本草稿提交给当前 document。
10. **审计序列化内容。** root、checkpoint、history 和 transfer 都包含必须保存的锁与
    默认身份；UI 滚动、可见行、临时预览像素、GPU 资源句柄和 stale 请求均不在其中。
    不引入面板关掉后才保存，或依赖打开 Nodes 才补齐字段的路径。

#### NM9.5.3 必须完成的交错矩阵

| 在途工作 | 同时发生的操作 | 期望 |
| --- | --- | --- |
| 修改 Mask 形状 | 选择另一组 / 切换面板 | 旧编辑按状态机收尾，新目标准确，无重复提交 |
| 插入或删除组 | 第二次结构意图 / 切图 | admission 有序；旧意图不按新图下标执行 |
| 原生小图求值 | 删除 Mask → Undo | 只允许匹配恢复后有效版本的输出发布 |
| 小图传输/上传 | checkout / 图像切换 / Loader 销毁 | 无越界访问、无串图、无旧回调重新创建 UI |
| 锁切换 | 保存 / Undo / Redo | 持久值与当前 head 一致，无额外 GPU 工作 |
| Paste | 失败 / 取消 / 后续源图修改 | 原目标完整、无半成品 Version；成功内容独立 |
| WAL 中存在新编辑 | 关闭并重新打开项目 | 沿现有恢复规则重建相同图、参数和锁 |

#### NM9.5.4 成功链、失败链与完成条件

**成功链：** 两视图意图 → 单一 session/document/history → 持久 commit/checkpoint/WAL →
销毁会话 → 正式项目 reopen/Version/Paste → 恢复文档 → 发布新会话的两视图和有效预览。
**失败链：** 提交、解码、恢复或在途资源失败 → 原 owner 的恢复/终止逻辑 → 明确结果及
错误 → 不发布半状态、不继续使用失效 target、不吞掉第二次恢复失败。

使用 `EditorSessionHistoryPortTest` 中的 document/topology/Version/Paste 测试文件、
`PipelineDocumentCheckpointTest`、现有 journal/recovery 测试和真实 QML 工作流 fixture。
完成记录逐项列出矩阵结果、实际存储路径、重启/重开的方式与非零用例数。无串图、无
部分提交、无错误 ID 映射且锁完整恢复才可 complete；不能仅以 JSON round-trip 代替
session、磁盘和异步生命周期的验收。

### NM9.6 — 摄影任务验收

**目标与交付物：** 用完整产品操作证明两种入口产生等价编辑结果，并量化蒙版组面板
对任务时间和操作成本的影响。交付可复现的任务脚本、像素对照、UI 证据、性能/资源表及
最终 NM9 完成记录；不得把只显示出面板或高 FPS 当作摄影工作流完成。

#### NM9.6.1 固定验收环境

1. 使用含所有 NM9 改动的优化构建，记录 commit、真实 build 配置、Qt/OS、GPU/驱动、
   所选 backend、Brush OFF、窗口/viewport/DPR、主题、侧栏宽度和照片实际渲染尺寸。
2. 选固定横向 Bayer RAW、竖向 RAW，并加入奇数尺寸/裁剪旋转场景；若已有 X-Trans
   fixture 可覆盖同一产品路径则一并记录。固定源文件身份与调整参数，避免把不同图像
   或不同质量等级的结果拿来计算收益。
3. 准备 1/3/8 Grade，含空组、单 Mask、多 Mask、部分关闭和不同锁值。每轮从相同
   document 状态开始；初次加载与模型/原生资源初始化单列，连续编辑性能在预热后测量。
4. 保留 NM8 的 input/producer/native pass/present 口径。缩略图的求值和 QML 开销单列，
   不把缩略图完成时间冒充照片呈现时间，不通过改变 viewport 尺寸制造性能收益。

#### NM9.6.2 配对任务脚本

每个任务从同一已知起点分别通过 Nodes 和 Mask Groups 完成。时间从用户开始该任务
到最终目标状态及所需照片结果可见；同时记录操作结束时间，以区分找控件时间与渲染等待。

| 任务 | 操作目标 | 成功条件 |
| --- | --- | --- |
| T1 新建局部调整 | 新建一个组，Exposure 改为 +0.5 EV，再创建预设位置的 Radial | 新组位置正确，参数与 Mask 归同一 NodeId；Groups 无需手工连线 |
| T2 增加第二种 Mask | 在现有选中组创建 Gradient，opacity 设为 45% | 不意外新建 Grade；两 Mask 的 Union、row 和右侧参数一致 |
| T3 跨组寻找并修改 | 在 8 Grade/多 Mask 场景找到指定位置的 Mask，移动中心或渐变位置并调强度 | 选中 Mask 即定位 owner；不再手动寻找节点，不修改其他 Mask |
| T4 管理默认保护 | 尝试删除默认 Grade/Mask，再显式解锁目标后执行允许的删除 | 受保护删除为零；原因明确，父子锁规则可理解，解锁不改像素 |
| T5 删除与撤销 | 删除一个未锁的中间组，然后 Undo/Redo，最后回到指定 head | 图连接、Mask、锁、选择和像素按历史精确恢复 |
| T6 跨视图恢复 | Groups 编辑 → Nodes 定位/改名 → 关闭并重开面板 → Version/reopen | 两侧状态一致，无重复操作、选择丢失或旧缩略图 |

Radial/Gradient 的几何参数在执行记录中用实际归一化值列出，不能只写“画一个差不多的
蒙版”。独立运行时随机 NodeId 可不同，但对照必须建立完整的一一映射并验证所有引用；
比较时仅映射不影响含义的身份值，不忽略顺序、参数、锁、名称计数器或默认身份差异。

#### NM9.6.3 采集与对照步骤

1. **先验证结果再比较速度。** 每个任务断言预期节点、边、Mask、参数和锁；同一后端
   在相同输入/质量下比较两路径最终像素。记录现有容差、最大/平均误差及差异图。Mask
   预览另按 NM9.4 采样规则比较，不把照片像素与小图采样混成一项。
2. **记录完整交互成本。** 分别统计点击、按键、滚动、展开、手动连线、面板切换、
   选错目标后的纠正和误删次数；记录最终完成时间与等待渲染的部分。只记录 UI 动作
   所需的数据，不导出照片内容或额外用户信息。
3. **采用配对重复。** 工程自测每种入口至少三次有效重复，轮换先用 Nodes/Groups 的
   顺序，练习轮不混入结果。报告每轮值、样本数、中位数和范围；小样本不虚报可靠
   P95/P99。若仅开发者执行，明确标为工程自测，不写成用户研究。
4. **比较缩略图开销。** 在固定照片、viewport 和相同输入序列下控制展示请求的有无，
   仅在验证工具中隔离这一变量；记录照片 input-to-present、producer、GPU 求值时间、
   dispatch、读回字节、请求合并数和内存。不要在产品中增加降低质量的“快速模式”。
5. **做资源稳定性验证。** 连续多轮切组、滚动、改 Mask、切图、checkout、关闭/重开面板，
   观察资源回到 owner 规定的空闲/保留范围。报告轮数、当前/峰值字节、在途请求及回收
   后状态，不能只凭一次内存截图声称无增长。
6. **验证完整 UI。** 检查真实应用中的创建、删除锁、快捷键、focus、长名、空态、错误
   态与 Loader 重建；两个主题都不出现蓝色选择框/底/侧条。按 NM9.3 的宽度/DPR 矩阵
   留必要截图。安装/打包路径还要确认新增 QML 与资源可加载，不能只验证源码树导入。
7. **逐平台如实记录。** 共享逻辑通过不替代原生后端和平台 UI 结果；涉及 NM9 新路径
   的 Windows CUDA/OpenCL、macOS Metal 分别记录可执行结果与缺口。不重跑与本次改动
   无关的整个 NM8 清单，也不把 NM8 未实测事项改写为 NM9 已通过。
8. **处理结果。** 所有误删、串写、错误像素、无法定位和资源无界增长必须修复并重跑
   受影响场景。若任务更慢，具体记录额外步骤/定位/等待原因，在已选布局内修正可修复
   的摩擦并复测；不删除不利样本，也不在没有数据时宣布“效率显著提升”。

#### NM9.6.4 成功链与失败链

**成功链：** 同一初始 document → 两入口完成同一任务 → 验证图/参数/锁/像素 → 配对
时间与操作统计 → 原生资源及 UI 回归 → 写入阶段证据 → 更新总方案 NM9 状态。
**失败链：** 结果不一致或任务/资源回归 → 定位到具体 owner/UI 步骤 → 修正 → 重跑对应
任务及受影响回归；不得降低 render/decode 质量、改换后端或隐藏真实错误换取通过。

#### NM9.6.5 最终完成条件

- [ ] 六个任务两入口都能完成，最终图与参数按身份映射等价，原生照片像素通过既定容差。
- [ ] Groups 新建无需手动连线，选择 Mask 无需再手动查找 owner；受保护对象未被误删。
- [ ] 有每轮任务时间、操作次数、纠正次数、样本信息和真实收益/退步说明。
- [ ] 缩略图和照片延迟分别记录，资源有明确上限且生命周期验证通过。
- [ ] 两主题、窄面板、长名、键盘焦点及实际打包 QML/资源可用，符合 monochrome 规范。
- [ ] NM9.1–NM9.5 均有完成记录，新增路径的真实平台结果和仍缺证项目明确列出。
- [ ] 更新总方案及本文件状态；未完成项不能仅因计划细化或测试目标存在而勾选。

### 6.7 执行时的构建、测试与证据规则

以下为未来实施时的命令模板，**本次计划细化没有运行这些构建或测试**。先检查当前
`CMakePresets.json` 和对应 tests/CMakeLists，再选择实际目标；本文件中的拟新增 fixture
在注册前不能写成可运行目标。测试发现为零、整组 skipped 或只运行旧二进制均不算通过。

Windows 从仓库根目录使用 MSVC wrapper。需要配置时保留调用者的 cache 选项和实际 Qt
路径，显式保持 Brush OFF；新增 QML/CMake 注册后重新配置。示例：

```powershell
cmd /c scripts\msvc_env.cmd --preset win_debug -DALCEDO_ENABLE_BRUSH_MASK=OFF -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorNodeGraphProjectionTest EditorNodeGraphDraftTest EditorSessionNodeCommandTest PipelineDocumentDefaultNameTest --parallel 4
ctest --test-dir build/debug -N -R "^(EditorNodeGraphProjectionTest|EditorNodeGraphDraftTest|EditorSessionNodeCommandTest|PipelineDocumentDefaultNameTest)\."
ctest --test-dir build/debug --output-on-failure -R "^(EditorNodeGraphProjectionTest|EditorNodeGraphDraftTest|EditorSessionNodeCommandTest|PipelineDocumentDefaultNameTest)\."
```

上例对应 NM9.1 的现有目标。其余阶段按下表构建相关目标并更新 CTest 筛选；QML 和
集成测试必须确认实际 test prefix，源文件名不等于独立可执行目标名称。

| 阶段 | 已核对的主要测试入口 | 额外要求 |
| --- | --- | --- |
| NM9.1 | `EditorNodeGraphProjectionTest`、`EditorNodeGraphDraftTest`、`EditorSessionNodeCommandTest`、`PipelineDocumentDefaultNameTest` | 结构、计数器与 history 提交次数；补对应 native 像素不变验证 |
| NM9.2 | `GpuDagModelGraphTest`、`PipelineEditBatchTest`、`PipelineDocumentCheckpointTest`、`EditorSessionHistoryPortTest` | 全删除入口、格式、回放和 metadata 无渲染 |
| NM9.3 | `EditorNodeSelectionLayoutTest`、`EditorNodesPanelQmlTest`、`EditorNodeDelegateQmlTest`、`AnalyticMaskCreationTest`、`WorkspaceShellTest` | 新面板生产 QML fixture、应用构建与真实交互 |
| NM9.4 | `GpuDagCudaMaskTest`、`GpuDagOpenClGradeTest`、`GpuDagMetalGradeTest` | 新小图/失效测试、真实设备和 QSG reader 生命周期 |
| NM9.5 | `EditorSessionHistoryPortTest`、`PipelineDocumentCheckpointTest` 及实际发现的 journal/recovery 用例 | 真实项目保存/reopen、受控异步交错和失败注入 |
| NM9.6 | 相关回归集合与 `alcedo_main` 产品路径 | 优化构建、任务脚本、像素、UI、资源及打包加载 |

macOS 使用实际可用的 `macos_debug_tests` / `macos_release` 预设和对应 build 目录；Metal
测试必须在原生设备运行。性能测量使用优化构建，不能把 CUDA debug 的慢速结果解释为
应降低产品质量。具体命令、编译开关和 driver/device 信息写入执行记录。

Windows configure/build/link 每次至少预留 10 分钟，CUDA 或较多目标优先预留 20 分钟，
健康进程持续有进展时继续等待；短轮询与总预算分开，不因一次短等待无输出重启构建。
所有临时日志、测试发现清单、失败截图和测量表放到
`build/tmp/mask_group_panel/nm9_N/`，不在仓库根目录创建 dump，不提交 build 输出。

每阶段先运行影响范围内的测试，修复后重跑失败与受影响用例；通过后仅在新改动或未解
疑点需要时扩大范围。完成前检查命名、roadmap 全树文件名/禁用术语、新增复制/快照、
第一方头文件前向声明、链接及 `git diff --check`。记录实际检查结果，不能只有清单。

## 7. 关键验收用例

下列名称描述预期行为，实施时接入现有合适的 model/service/QML/product fixtures。

| 建议测试名 | 断言 |
| --- | --- |
| `MaskGroupsFollowSceneImageExecutionOrder` | 改名和画布移动不改组序；重连后两视图顺序一致 |
| `EmptyMaskGroupKeepsGradeAndFullImageAdjustment` | 无 Mask 仍保留组，像素遵守无 Mask 调整规则 |
| `CreateTopMaskGroupInsertsOneCleanGradeWithoutChangingPixels` | 顶部插入、正确 edge、单次提交、初始像素无变化 |
| `SelectedGroupReceivesNewMaskFromExistingTool` | 明确选择优先于默认顶部，目标 NodeId/MaskId 正确 |
| `SelectingMaskUpdatesOwnerNodeAndBothViews` | 两侧高亮、参数和 Viewer 对准同一对象，无额外渲染 |
| `DefaultGradeAndItsMasksStartWithDeletionProtection` | 默认身份不依赖名称/位置，新增 Mask 默认保护 |
| `DeletionProtectionAllowsParameterAndShapeEdits` | 锁定后曝光、强度和控制点仍可编辑 |
| `LockedMaskPreventsOwningGradeDeletion` | 父节点解锁不能绕过子 Mask 保护，失败保持完整状态 |
| `UndoCreationRestoresPriorDocumentDespiteDefaultDeletionProtection` | 受信任历史撤销按记录恢复，普通 Delete 仍受保护 |
| `ExplicitUnlockSurvivesUndoVersionPasteAndReopen` | 锁值、默认身份和重映射稳定，不被投影重建覆盖 |
| `MaskThumbnailMatchesEffectiveCoverageWithinTolerance` | 单 Mask/Union 图与真实 coverage 的采样值比较，记录容差 |
| `StaleMaskThumbnailCannotReplaceCurrentImagePreview` | 过期会话、图片、Version 或 revision 的结果不发布 |
| `PanelSwitchPreservesIncompleteNodeConnections` | 未连接节点可定位，无隐式提交或丢弃 |
| `EquivalentEditsFromBothViewsProduceMatchingPixels` | 同一任务从两入口执行后 DAG、参数和最终像素一致 |

## 8. 完成记录

尚无实施记录；NM9.1–NM9.6 均为 planned。本次增加执行指示不改变阶段状态。
在各阶段对应小节后追加有日期的记录，并在本节维护简短索引，不删掉早期失败或平台
缺证记录来使完成结果看起来更完整。

### 8.1 每阶段必须填写的记录

```text
Phase / date / status:
Actual commit and working-tree scope:
Implemented behavior and explicitly unimplemented items:
Actual changed files and owner APIs:
Primary success call chain:
Primary failure / restore call chain:
Model, UI, history, render and thumbnail ownership:
Actual build preset, Qt/OS, backend/device and Brush OFF evidence:
Actual configure/build/test commands and exit codes:
Discovered / passed / failed / skipped test counts:
Independent expected results, pixel/sample tolerance and observed error:
History commits, render requests and thumbnail resource measurements:
UI layout choice, themes, widths, DPR, keyboard and accessibility results:
Evidence directory and reproducible inputs:
Remaining defects or unverified platforms, and effect on completion:
```

按阶段填写相关项，不适用的项目说明原因；不能把没有测量写成零，也不能把 skipped
计为通过。单个平台实现/编译完成与该平台真设备通过分别记录；必需验收仍未通过时
保持 partial/in progress，并指出哪条完成条件缺证，不把它自动转给下一阶段。

### 8.2 各阶段特有交付证据

| 阶段 | 完成记录必须能回答的问题 |
| --- | --- |
| NM9.1 | 谁拥有图和组序？顶部插入/删除从哪个 app 入口到哪个领域操作？一次用户操作如何只提交一次？不完整草稿如何保留？ |
| NM9.2 | 锁与默认身份存在哪里？哪些删除入口执行同一校验？格式如何变化？Undo 元数据如何不渲染？ |
| NM9.3 | 最终选了哪个布局？真实 QML 如何取得同一选择？新面板从未打开 Nodes 时能否工作？窄面板、键盘和 monochrome 有何证据？ |
| NM9.4 | 小图表示哪一层 coverage？采样/容差如何定义？谁拥有原生输出和 QSG reader？请求与内存上限是多少？哪些平台真正运行？ |
| NM9.5 | 哪些磁盘 reopen、WAL、Version/Paste 和异步交错真正执行？故障发生后文档、head、选择和资源分别是什么状态？ |
| NM9.6 | 六项任务最终图/像素是否一致？每轮时间和操作数是多少？缩略图成本是多少？有哪些仍未解决的交互或平台问题？ |

临时日志、截图和原始测量表放在 `build/tmp/mask_group_panel/nm9_N/`，不提交到仓库。
文档保留必要的结果表、命令、输入定义和路径，避免结论只能依赖后来可能被清理的临时文件。
NM9 完成时同步总方案第 21.10 节与第 26 节的实际状态；NM10 不因这次验收增加内容。

下一阶段：[NM10 — Adjustment Transfer](phase_nm10_adjustment_transfer_plan.md)，仅保留占位。
