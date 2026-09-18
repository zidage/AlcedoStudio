# Phase NM9 — 蒙版组孪生面板与传统摄影工作流

Date: 2026-09-16

Status: **NM9.1 complete 2026-09-16**；**NM9.2 complete 2026-09-16**；**NM9.3 partial
2026-09-16 on `feature/mask-groups-workspace`**；NM9.4–NM9.6 仍为 planned。本文件记录产品
语义和实施拆分；NM9.1 的共享组投影、顶部插入、桥接删除与草稿
边界已在 `feature/nm91-mask-group-projection` 实现并通过验证，见 NM9.1 完成记录。
NM9.2 的删除保护、默认 Grade 身份与格式读写已在 `feature/nm92-deletion-protection`
实现并通过验证，见 NM9.2 完成记录；NM9.3 见其完成记录，组顺序调整（每次移动即一次
主链重连 + 一条 typed history commit + 一次渲染请求）已在 `feature/mask-groups-panel`
实现并通过验证，见 2026-09-17 ordering increment 记录。

Design update 2026-09-16：用户已选择方案 C（缩略图优先）。当前 Mask 合成在 UI 中只称
`Add`，内部保持 Union；每个 Mask 和 Group 均有独立删除按钮。缩略图采用项目级内存
KV/LRU service，固定 128×128 灰度、默认 1000 项、跨图复用、仅成功 commit 后更新、
逐项异步显示且不阻塞删除；完整规格见第 5 节与 NM9.4。NM9.3 的约 2–3 阶段拆分留待
单独整理，本次保留阶段编号和依赖，先写清已确定的行为。

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
第 2.6 节采用已选定的方案 C；A、B、D 仅保留为设计比较记录。

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

当前 Mask 合成只开放 **Add**，这是内部 Union 的用户可见名称。Union 保持逐像素
`max`，不改为数值相加、饱和加法或普通 alpha 叠加；两个 50% coverage 的相交区域
仍为 50%。此名称描述组内 Mask 合成，不新增 Grade 之间的照片混合操作。每个 Mask
row 与每个 Group 组头都有独立删除按钮，沿 NM9.2 的删除保护与原子校验执行。

输出固定为 128×128 的 R8 灰度图；照片比例保留在黑色方形画布内，取样定义见第 5.3 节。
UI 按 AppTheme 的紧凑尺寸显示，DPR 不改变缓存尺寸。裁剪、旋转和纵横比
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

### 2.6 ASCII 布局方案（已选择 C）

2026-09-16：用户在 NM9.2 完成后选择 **C — 缩略图优先**。A、B、D 保留为比较记录，
不再是待选择的生产布局。方案 C 保留多组同时展开；缩略图采用第 5 节的跨图内存缓存、
固定采样与 commit 后逐项加载。以下均遵守第 2.1–2.5 节的数据和操作语义，不改变顶部插入
顺序、Mask 所有权或删除锁规则。

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

#### 方案 C — 缩略图优先（已选定）

把组的合成蒙版放在标题左侧，给位置/形状更高的视觉优先级；子 row 也使用稍大的预览。
适合凭“蒙版画在哪里”寻找调整，代价是同屏组数更少。显示尺寸由 AppTheme 控制，
输出仍为第 2.2 节的 128×128，不增加照片渲染尺寸，也不等待全部小图完成再显示面板。

```text
+--------------------------------------------------+
| Mask Groups                      [+ Mask Group]  |
|--------------------------------------------------|
| v +------+  Color Grade 1             [L]  [--]  |
|   |..##..|                                       |
|   |.####.|                                       |
|   +------+                                       |
|=    +------+  Radial                  [L]  [--] =|
|=    |..##..|  Add       100%                    =|
|=    +------+                                    =|
|     +------+  Gradient                [L]  [--]  |
|     |..::##|  Add        45%                      |
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

## 5. 缩略图 service、跨图缓存与异步显示规格

**当前设计（2026-09-16）：** 采用项目级常驻 `MaskThumbnailService`，为当前 Radial /
Linear Gradient 生成 CPU 灰度小图。内存 KV 缓存默认最多 **1000 项**，严格 LRU 淘汰，
跨图片、跨编辑会话和 Version 复用；不读写磁盘。编辑过程中保持最近一次已提交结果，
只在成功 commit 后检查新的内容键；首次显示、缓存未命中和历史恢复按下文请求。
本节取代原先的原生后端小图限定、拖动定时刷新和切图清空缓存建议。照片的原生渲染
后端、Interactive/Quality/export 的质量策略保持既有定义。

### 5.1 service 的所有者、值与容量

- service 由项目级服务容器持有，生命周期覆盖该项目内的全部照片编辑会话；不由
  panel、delegate 或单张图片的 EditorSession 创建/销毁。切 A→B→A、关闭重开面板、
  关闭单图编辑会话和切 Version 都保留 LRU。项目关闭时释放；另一个项目使用独立
  service 实例，element 数字 ID 不跨项目混用。本阶段不做跨项目或进程重启后的缓存。
- 一个缓存 Value 是不可变的 **128×128、R8 灰度输出**，占 16,384 bytes（16 KiB）。
  Qt 边界可用 `QImage::Format_Grayscale8` 包装；不保存 RGB32/RGBA32F 的长期副本。
  单 Mask 图和 Group 图共用 1000 项额度，历史参数对应的旧图也占额度。
- 成功查询和新结果插入提升为 MRU；第 1001 项插入前淘汰 LRU，使 ready 项数始终
  不超过配置值。容量是可配置的正整数，默认 1000；本阶段不增加用户设置页。
  不因 pin、命中率、面板数量或临时内存压力而自动增加容量。
- 1000 项的灰度像素为 16,384,000 bytes（15.625 MiB）。键、LRU 节点、在途计算、
  当前可见项的共享引用和 Qt 上传纹理另外计量，不能宣称整个功能只占 16 MB。
- 淘汰只移除 KV 的强引用，不使正在显示或上传的图像失效。接收方以共享只读句柄
  持有输出，行隐藏、删除、替换 source 或 Loader 销毁时释放；不为离屏行保留句柄。
  service 是唯一长期缓存，provider 不再建立一份永久保留所有历史 URL 的图像表。
- 不创建文件、SQLite/DuckDB 表或 `ThumbnailDiskCacheService`；不调用 Library 的
  磁盘缩略图读写、解码/照片渲染 scheduler 或 JPEG/WebP 编码。缩略图不进 history、
  checkpoint、项目包、Version 或 Paste 数据，错误也不触发写回文档。

### 5.2 内容键：element ID 加实际求值输入

项目由 service 实例隔离。实例内的键定义如下，`input` 同时作为异步求值所需的不可变
最小输入；不得另外复制整份 MaskModel、Grade、PipelineDocument 或照片像素：

```text
Key = (element_id, input)
input = (sampling_version = 1, kind = Single | Group, geometry, mask_parameters)
```

| 字段 | 必须包含的内容 | 必须排除的内容 |
| --- | --- | --- |
| `element_id` | 当前项目内的实际 element ID | 列表下标、文件名或缩略图 URL |
| `geometry` | full reference 尺寸、决定 reference 坐标基准的源方向值、已提交照片 crop/rotation/expand_to_fit；复用现有值类型的必要字段。worker 从这些字段推导完整照片取样映射与有效区域，不在键中重复保存派生矩阵 | 当前照片 decode/render 分辨率、Viewer 临时 pan/zoom/ROI、窗口大小、DPR、geometry revision 计数 |
| 单 Mask 参数 | source kind；Radial 的 center_x/y、major/minor_radius、rotation、inner/outer_feather，或 Gradient 的 origin_x/y、normal_x/y、transition_distance、start/end_value；两者的 enabled、invert、opacity | NodeId、MaskId、display_name、删除保护、选中/展开状态、Grade enabled/Mix/曝光等调整、会话 ID、Version ID、history/document revision |
| Group 参数 | 当前启用成员的单 Mask 参数编码，按编码排序后组成数组；复用同一 geometry；Group 类型标签使全关闭的非空组能生成全黑图 | 成员 ID、显示顺序、名称、锁；无 Mask 的组根本不请求图 |

Group 参数只取启用成员，因此添加/删除已关闭成员而仍保持非空组时可复用相同 Group
图。单 Mask 行关闭时显示有效全黑 coverage，并由行的文字/控件表示关闭状态。无 Mask
与全关闭必须区分：前者无缩略图，后者有全黑结果。Group 图取 Grade Mix 之前的 coverage。

键对语义输入做逐字段稳定编码。浮点按实际存储精度保持值，`-0` 规范为 `+0`，拒绝
非有限/非法输入；不读 struct padding、不按地址编码、不把参数四舍五入成显示文本。
哈希仅用于查表，命中还必须比较完整规范键，不能把哈希碰撞当成同图。序列化整个
`MaskModelToJson` 不符合此定义，因为它包含身份和无关元数据。

同一 element 的相同参数在 A→B→A、Undo、Redo、Version 恢复或新 Mask 身份下应命中
同一内容；不同 element 不共用键。普通编辑产生新键，旧项留待 LRU 淘汰，不执行
`InvalidateElement` 或清空整个项目。固定 128 规格不因 DPR 变化生成第二套缓存。

**最小独立输入的必要性：** 异步生成必须能在 live Mask 已被删除后安全完成，不能
持有 document 引用或在整个计算期间阻塞 owner。缓存键本身已经需要这些参数，故从
成功提交后的 owner scoped read 一次构造该不可变键，并直接用它求值；不再增加平行
状态对象。复用既有 Radial/Gradient 参数和 geometry 值类型，去掉名称、ID、锁和全部
无关字段。请求、KV 与共享任务按需持有键，取消/淘汰/结束时释放；绝不写回 live 数据。

### 5.3 固定取样与生成职责

1. 每个输出画布固定 128×128，先填黑。使用完整 EditSpace 的 ViewRequest，分辨率
   请求固定 render_scale=1、max_edge=128，经现有 geometry resolver 取得不超过 128
   的内容尺寸与映射，小于上限的原图不扩大求值。保持照片比例，内容居中；整数尺寸
   使用 resolver 的舍入结果，不按 UI 宽高再算一遍，左/上黑边取差值的一半向下取整。
2. 只使用完整照片视图；临时 Viewer 缩放和平移不改变缩略图。内容区像素中心经
   thumbnail-to-reference 映射求值；照片有效区域外保持黑色，反相也不得把黑边变白。
   裁剪、旋转和源方向使用与照片相同的 geometry 规则。无效尺寸/映射报告失败。
3. 当前 Radial/Gradient 复用既有解析公式，顺序为 source → invert → opacity → clamp
   → R8 round-half-up。Group 逐像素取 max，UI 仍称 Add；不使用 alpha 叠加或数值相加。
   不应用照片 DRT、LUT、曝光或 gamma 转换。当前 range 字段只能是已支持的 identity。
4. 一个后台 CPU worker 即可。Group 工作按成员键查同一 LRU，未命中时逐个生成并
   插入，再取 max；每次只需当前成员和组累积输出，不一次持有全部成员像素副本。
   Group 作业不递归提交子任务后等待同一个 worker，避免单线程自锁。成员图即使不
   在展开行中显示也可因 Group 求值进入同一 1000 项缓存。
5. 删除、关闭、降低 opacity 或成员移动后必须从当前成员集合重新合成 Group；不得
   仅在旧组图上继续 max。首次载入和 geometry 提交需要的成员可逐步生成，均不阻塞 UI。
6. CPU 小图是本功能选定的生成方式，失败显示真实错误，不再尝试其他后端。它不读取
   全尺寸 GPU Mask、不另起照片 executor、不改变现有照片渲染资源或计算质量。

### 5.4 请求时机与逐项显示

“只在 commit 更新”约束的是编辑内容变化；打开面板或缓存缺失时仍必须能加载当前
已提交内容。成功入队、pointer release、通用参数通知或 render completion 都不能代替
history/document owner 的成功提交通知。`pipeline_document()` 是现有发布接口，执行者
须核对其 committed/preview 边界，不能直接监听每次发布就发请求。

| 事件 | 规定行为 |
| --- | --- |
| 打开面板、展开组、行进入可见区域 | 立即显示组名、Mask 行与操作按钮；请求各可见目标当前已提交键。命中也走统一的异步完成通知，不等其他行 |
| 创建中的未提交 Mask | 允许显示既有创建状态；不生成缩略图、不把临时参数写入 KV。确认提交成功才请求 |
| 拖动、滑条输入及其 preview | 不变更缩略图 source，不构造新内容键、不调 service、不定时刷新；已有图代表最近一次提交 |
| Mask 创建/参数/enabled/invert/opacity 成功 commit | 为受影响可见 Mask 和所属 Group 求键；键未变则 no-op；变了只更新相应目标 |
| 编辑取消、no-op commit、提交失败 | 保留上次提交的小图；不为未成功提交的试改参数求值或留下新缓存项 |
| Mask/Group 删除 | 按第 5.6 节即时撤销订阅；文档删除成功后移除行、更新仍存在的 Group；不删除旧 KV 项 |
| 裁剪、旋转等 geometry 成功 commit | 为受影响可见目标请求新键；旧 geometry 键保留，Undo 时可复用 |
| Undo/Redo、Version checkout、切图/重新打开单图 | 当前已提交文档就绪后按内容请求；新订阅使用新请求身份，KV 可命中历史值 |
| 曝光、Saturation、Grade Mix、名称、锁、row 顺序、选择 | 不调用重新生成；Group 的参数排序使纯显示重排保持同键 |
| 折叠/离屏/关闭面板/切图 | 撤销相应 UI 订阅，释放显示句柄；移除没有订阅的排队工作，保留 ready KV |

每个可见目标独立拥有 `Empty / Loading / Ready / Error / PendingDelete` 显示状态。
commit 后键变化时先清除旧 source，显示固定尺寸占位；Ready 单项到达就更新该项。
拖动中原图不是过期图，因为目标仍是上次提交的键。新键失败则显示准确错误，不能拿
旧图标成新键的 Ready。纯取消不显示错误。无 Mask 的 Group 使用 Empty，不发请求。

QML 不以“所有 thumbnail Ready”作为列表 visible、Loader active、行创建或操作可用的
条件。生成完成只通知对应项的 source/status/error，不 reset model、不重建全列表、
不改变 contentY/选择。缓存命中或单项失败都不影响其余行显示和按钮操作。

### 5.5 请求身份与回调接收规则

**缓存键表示像素内容，请求身份表示谁仍在等待。两者不可混用。** controller 为每次
订阅分配单调递增的 `request_id`，并保存以下接收条件；它们不进入 KV 键：

```text
receiver = weak controller
binding = 当前项目 service 实例、图片/Version/面板绑定代次
target = NodeId + optional MaskId          // 不使用 row index 或 delegate 地址
expected = request_id + Key
```

- 同一 Key 的未完成请求合并为一次生成，各订阅各有 request_id。取消一个订阅不影响
  其他目标。service 只保存弱接收方/取消标记，worker 不持有 QML Item 或 model index。
  作业真正开始时再查一次 KV，以复用先前 Group 作业顺带生成的成员图；命中后直接完成。
- 同一目标同一键已有 Ready/Loading 时不重复请求；目标更换键、销毁或进入删除流程时
  立即撤销旧 request_id。Undo 恢复同 NodeId/MaskId 也分配新 request_id，不能复活旧订阅。
- service 在短临界区查 KV / 注册任务 / 插入完成值；像素生成、Qt 转换、回调派发及
  删除命令都不在缓存锁内运行。队列只保留仍有可见订阅的不同 Key，不对全部离屏图预取。
  每个可见目标最多一个当前订阅；取消后移除无订阅的未启动任务和回调记录，防止滚动
  或连续 commit 堆积旧工作。在途只有一个作业；取消不等待该作业完成。
- 成功输出可以先进入项目 LRU，再将每个仍有效订阅的完成通知投递 GUI 线程。即使
  最后一个订阅已取消，已运行作业的有效结果仍可缓存，以便切回照片或 Undo；项目已
  关闭的结果丢弃。Ready cache fill 不具有创建 UI 行或恢复文档对象的权限。
- **GUI 队列实际执行回调时再次检查**：弱 controller 仍存在；binding 仍相同；target
  在当前投影存在且未 PendingDelete；当前 request_id 与 Key 均匹配。全部满足才更新
  对应行。失败/错误通知也必须做相同检查。不能仅在 worker 发出回调前检查一次。
- Qt provider 只提供已完成输出，用本次接收句柄精确寻址；旧 URL 不得返回“这个
  element 最新的一张图”。从取结果到 provider/QSG 读取完毕，共享句柄保证像素寿命。
  QML 使用异步 Image 加载，并关闭历史 URL 的默认长期缓存，由 service 控制保留；
  provider 的临时句柄随可见消费者/实际 reader 结束释放。provider 不访问 live document，
  不在图片查找中生成 Mask，也不让任意 Image 请求重新激活已取消的 UI 订阅。
  若 Image 加载完成/失败还需回写 model，必须带同一 request_id/Key 再校验；不能把
  旧 source 的 Qt 加载完成信号用到已复用的 delegate 当前目标。

### 5.6 删除的异步顺序：撤销订阅，不等待像素销毁

删除入口包括 Mask 行按钮、Group 按钮、快捷键以及 Nodes 发起的同一对象删除。共享
controller 必须统一处理；不能只在新面板按钮里加保护。明确区分“删除文档对象”和
“释放该行显示句柄”：前者由既有 owner/history 操作完成，后者无权阻止前者。

1. **本地删除意图通过基本校验后**，GUI controller 先将目标置 PendingDelete，立即
   使其 request_id 失效并撤销接收资格，再向 owner 异步提交既有删除命令。Group 删除
   同时撤销组及子行订阅；Mask 删除同时撤销受影响 Group 的旧合成图订阅。已经 ready
   的旧图可留在 PendingDelete 行内直到领域结果返回，期间不得接收新的缩略图回调。
2. `CancelRequest` 只标记订阅失效并安排队列清理，立即返回。删除路径不得调用
   future.get/wait、join、GPU fence，或等待 thumbnail cancel acknowledgment、LRU
   erase、provider release、QSG texture 销毁。CPU worker 不占文档锁、session admission
   或 photo render lock；即使它被测试闩锁暂停，删除仍必须能提交并收到领域完成结果。
3. 入队仅表示提交请求，不表示删除成功。领域成功前保留行、原选择与对象身份，禁用
   该目标重复删除；不乐观修改文档。普通删除锁、草稿、history/WAL 规则仍由 owner 检查。
   删除成功的同一次投影更新才真正移除行/更新选择；缩略图状态不能延后这次更新。
4. 删除被拒绝、同步入队失败或领域提交失败时，撤销 PendingDelete，保留原选择与行；
   若图/Version 已切换则不向新绑定恢复旧行。仍在原绑定时，从 owner 的当前已提交
   内容重新订阅，使用新 request_id；通常命中原有缓存，不复用被撤销的旧回调。
5. 删除成功后不遍历或清除这个 Mask 的历史参数 KV 项。正在计算的旧内容可以完成并
   进入 LRU，但不回填已删除行、不 upsert target、不发出新选择或参数写入。Undo
   恢复对象后通过新订阅命中旧内容属于正常复用，旧订阅本身始终无效。
6. 若外部 owner/历史操作直接移除目标，处理新的投影时先撤销消失目标的订阅再移除行。
   已排入 GUI 队列的回调仍执行第 5.5 节检查。panel 销毁、delegate 复用、切图、checkout
   和项目关闭遵循同样原则；项目关闭标记 service 停止接收并异步释放在途资源，不在 GUI
   析构中 join 工作线程，也不以无所有者的 detached thread 延长资源寿命。

```text
开始生成 K1，订阅 R1
  → 用户 Delete：R1 立即失效，行 PendingDelete，领域删除异步入队
  → 任意先后：K1 生成完成可入 LRU；R1 回调在 GUI 检查失败，直接丢弃
  → 删除成功：投影移除行                     // 不等待 K1
  → 若 Undo：新行/新订阅 R2 请求 K1，可命中  // R1 仍不能更新任何行
```

### 5.7 最小接口与失败边界

以下为行为接口建议，实施时用实际名称替换，不因此增加一套文档或通用任务框架：

| 操作 | 必须保证的行为 |
| --- | --- |
| `Request(Key, subscription)` | 非阻塞；命中和生成统一投递完成事件，同 Key 合并工作；成功事件携带 Key、request_id、只读输出句柄 |
| `CancelRequest(request_id)` | 仅撤销该订阅，幂等、立即返回；不删 ready KV、不影响其他订阅、不等待作业 |
| `SetCapacity(count)` | 正整数配置；缩小时按 LRU 去除多余 cache 引用，reader 安全，默认 1000，不自动扩容 |
| 项目关闭 | 拒绝新请求，撤销订阅，清空项目 KV；在后台安全收尾 worker，禁止旧结果写新项目 |

求值/分配失败不缓存空图，清除对应 pending 记录，对仍有效订阅报告一次 Error。
之后显式重试、离屏再进入或新内容请求可再次生成；不在每个 paint 自动重试。无接收方的
错误仅走既有诊断，不弹出已删除对象的错误。缓存插入失败也按真实失败完成，不能卡住
订阅或删除。取消和 Key 过期不是领域错误，不更改 history 或照片渲染。

### 5.8 已核对源码与测量的使用边界

- [ThumbnailManager](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/thumbnail_manager.cpp)
  的活动标记、弱 QObject 接收方和 GUI queued callback 可作参考；不照搬每图 detached
  thread、磁盘行为、pin 扩容或释放时删除缓存的策略。
- [ThumbnailImageStore](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/thumbnail_image_provider.cpp)
  目前忽略旧 URL revision 并返回 element/size 的当前值。Mask 不能使用该规则；须以
  精确输出句柄读取，也不能在图切换时调用其 `Clear` 代替跨图 LRU。
- [LRUCache](../../../../../alcedo_studio/src/include/utils/cache/lru_cache.hpp) 的 `Evict`
  含自动 Resize 逻辑。执行者须明确关闭该行为后才能复用；若现有接口不支持，service
  内用标准 unordered_map + list 做固定容量 LRU 即可，不重构 Library 缓存。
- [GradeMaskCoverage](../../../../../alcedo_studio/src/edit/mask/grade_mask_coverage.cpp)
  已提供 R8 解析求值，但当前缺少 render-to-reference 映射，不能原样宣称满足裁剪/旋转。
  映射复用 [RenderGeometryResolver](../../../../../alcedo_studio/src/include/edit/geometry/render_geometry_resolver.hpp)。
- 前次 HEAD `88a034c2` 的独立测量：i7-12700H、MSVC 19.44 Release `/O2 /arch:AVX2`、
  Qt 6.9.3；预热后 9 批平均耗时的中位数，128×128 单 Radial 约 0.299 ms、8 Mask
  全部求值并合成约 2.005 ms、8 张已缓存 R8 图取 max 约 0.069 ms。未含 owner 排队、
  geometry、Qt 上传和真实窗口；不是本 service 的实现或性能验收。复现资料在
  `build/tmp/nm93_thumbnail_research/`，不提交临时文件。

## 6. 子阶段与完成条件

各阶段按“目标 → 前置/文件 → 实施步骤 → 成功/失败链 → 验证与交付证据”执行。
步骤是待实现要求，不是完成记录；新增 API/文件建议名在真正落地后替换为实际名称。
后续阶段的集成验证不能代替前一阶段的基本正确性测试。布局已选择 C，缩略图以第 5 节
的明确规格执行；NM9.3 的细分另行整理，本表暂保留现有编号和依赖。

| 阶段 | 工作 | 依赖 | 状态 |
| --- | --- | --- | --- |
| NM9.1 | 共享组投影、顶部插入/桥接删除的 app 操作、草稿边界 | NM8 收口 | complete 2026-09-16 on `feature/nm91-mask-group-projection` |
| NM9.2 | 删除锁、默认保护、typed history 和格式规则 | NM9.1 | complete 2026-09-16 on `feature/nm92-deletion-protection` |
| NM9.3 | Mask Groups 面板、空抽屉、创建入口与双向选择 | NM9.1–NM9.2 | partial — implementation present; acceptance failures under investigation |
| NM9.4 | 项目级内存 LRU、跨图复用、commit 后逐项小图、非阻塞删除与回调校验 | NM9.3 | planned |
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

##### Phase NM9.2 completion record (2026-09-16)

**Status:** complete — owner 强制删除保护、可撤销锁切换、显式默认 Grade 身份、
完整格式读写规则已交付;布局与真实缩略图仍属 NM9.3/NM9.4。

**实现事实:**

- **领域字段。** `ColorGradeNodeModel::SetDeletionProtected/DeletionProtected` 与
  `MaskModel::deletion_protected`(`mask_model.{hpp,cpp}`)是持久领域数据;canonical
  mask JSON 强制 bool。默认值只在创建时应用:`CreateDefaultPipelineDocument` 给
  默认 Grade 上锁,新建 Mask 默认上锁(Clean Grade 不锁);读取/投影/重建不重套默认值
  (`default_pipeline_test.cpp` 断言读取不改锁)。
- **默认身份。** 新增 `PipelineDocument::DefaultGradeId/SetDefaultGradeId`(类型与
  唯一性校验,删除后为空),随 document JSON/checkpoint/transfer 持久化;Paste 由
  identity source 重映射。不按显示名或行号判断,删除后不自动升级其他组。
- **typed metadata change。** `SetNodeDeletionProtectionChange`(NodeId+before/after)与
  `SetMaskFieldChange`("deletion_protected")走 JSON/validate/apply/inverse/hash;
  同值写入 no-op 不提交。锁 change 元数据生效:`RenderReasonForBatch` 不返回 pixel
  原因,mixed batch 仍产生 render intent。
- **统一删除 admission。** `PipelineDocument::ValidateUserDeletion` 检查节点自身与
  全部将被移除 Mask 的锁,一次预检;草稿删除、topology batch、组删除、Mask 行、
  service 入口共用。失败返回含 stable ID 的可本地化原因,不部分删除。
- **历史与新命令边界。** 用户删除按当前锁校验;已验证历史 forward/inverse 回放不再
  当新用户删除。`NodeGraphTopologyChange` 新增 `removed_default_grade_id`:forward
  清空默认身份,inverse 精确恢复;JSON 要求显式字段并校验指向被移除 Grade。
  拓扑删除默认 Grade 若未记录身份则拒绝。
- **格式。** `kPipelineDocumentFormatVersion` 6→7、`kPipelineEditBatchFormatVersion`
  3→4、root/checkpoint 5、WAL 5、transfer schema v5;缺锁字段/非 bool/非法默认身份
  在 decode 边界真实失败,无旧格式迁移路径。

**Primary success call chain:**

```text
锁动作(UI/service) -> EditorSessionController::SubmitSetColorGradeDeletionProtected
  -> EditorSessionService(会话/交互校验) -> EditorSessionHistoryPort::SetColorGradeDeletionProtected
  -> EditorHistoryMutation(SetColorGradeDeletionEnabled 等) -> MakeSetNodeDeletionProtectionBatch
  -> PublishAppliedTypedBatch(apply + WAL append + live mirror)
  -> document 字段与投影更新 -> 两视图锁与删除可用性刷新;无 render intent
```

**删除成功链:**

```text
removeMaskGroup/deleteColorGrade -> draft/topology capture(NodeGraphTopologyChange,
  removed_default_grade_id 由 draft 基准身份记录)
  -> ApplyNodeGraphTopologyChange(默认身份清空/恢复) -> history commit -> 投影重建
```

**Primary failure call chain:**

```text
受保护删除请求 -> ValidateUserDeletion / topology 默认身份预检
  -> 返回 "Unlock Color Grade before deletion: <NodeId>" 或
     "NodeGraphTopologyChange must record removal of the default Grade identity"
  -> 图、history head、commit 计数、选择全部不变
```

**What was proven (executed tests, debug build):**

| 覆盖点 | Target / 测试 | 结果 |
| --- | --- | --- |
| 拓扑删除默认身份清空、JSON roundtrip、Undo 恢复身份与原文档 | `EditorSessionHistoryPortTest.NodeGraphTopologyHistory.DefaultGradeRemovalClearsIdentityAndUndoRestoresIt` | PASS(修复前失败:身份未清空,decode 抛 invalid_argument) |
| 显式节点+Mask 解锁经 checkpoint/WAL 真实重开、Undo/Redo 头与锁值精确恢复 | `NodeGraphTopologyHistory.ProductionPortRecoversExplicitNodeAndMaskUnlockFromCheckpointAndWal` | PASS |
| 新建默认保护 Mask Undo/Redo 恢复原 ID+锁 | `EditorDocumentHistoryTest.ProtectedMaskCreationUndoRedoRestoresExactIdentityAndLock` | PASS |
| mixed lock+pixel batch apply/Undo/Redo 均有 render intent | `EditorDocumentHistoryTest.MixedLockAndPixelBatchRequestsRenderThroughUndoRedo` | PASS |
| 多对象移除 batch 预检拒绝,含未尝试解锁,文档/head/计数不变 | `EditorDocumentHistoryTest.MultipleMaskRemovalRejectsBeforeUnlockOrPartialMutation` | PASS |
| 锁 no-op/有效切换/Undo/Redo、无 render intent、MaskContentRevision 稳定 | `NodeDeletionLockHistoryRoundTripHasNoRenderIntent`、`MaskDeletionLockPreservesCoverageAcrossNoOpUndoAndRedo`、`SameValueNodeLockPreservesHistoryAndClearsStaleRenderReason` | PASS |
| service 双视图计数与非交互拒绝 | `DeletionLockPublishesOnlyEffectiveChangesWithoutRender`、`EditorSessionNodeCommandTest` 锁用例 | PASS |
| 四种父/子锁组合删除拒绝、草稿保持 | `EditorNodeGraphDraft.LiveDeletionProtectionPreservesDraftAndPriorReversal` | PASS |
| 参数/形状在锁下可编辑 | `ProtectedDeletionPreservesOpenEditAndAllowsParameterChanges` 等 | PASS |
| 默认值按身份而非名称/父锁 | `AnalyticMaskCreationTest.CreationProtectionUsesDefaultIdentityNotNameOrParentLock` 等 | PASS |
| checkpoint/root/WAL 默认身份+独立锁、缺字段/非法身份拒绝 | `PipelineDocumentCheckpointTest`(RoundTrip/Rejects* 等) | PASS |
| typed batch 锁 change JSON/validate/hash、兼容性 | `PipelineEditBatchTest`(DeletionProtection* 等) | PASS |
| transfer 缺失/非 bool 锁字段与非法默认身份 import 拒绝、Paste 重映射 | `DocumentTransferTest.ImportRejectsMissingOrInvalidProtectionAndDefaultIdentity`、`PasteRemapsEveryNodeAdjustmentAndMaskId`、`PasteWithoutDefaultIdentityDoesNotInheritTargetDefault` | PASS |
| 受保护 batch 拒绝原子性 | `EditorDocumentHistoryTest.ProtectedGradeBatchRemovalKeepsDocumentAndHistoryUnchanged` | PASS |
| 全套件回归 | 260(历史/会话/投影/draft/transfer/batch/checkpoint/commit)+ 61(EditorNodeSelectionLayoutTest)+ 62(GpuDagModelGraphTest)+ 36(batch/checkpoint) | 全部 PASS |

Commands: `cmd /c scripts\msvc_env.cmd --build --preset win_debug --target ... --parallel 4`;
`ctest --test-dir build/debug --output-on-failure -R <suite>`(具体清单见上)。

**格式兼容表:**

| 格式 | 新版本 | 旧行为 |
| --- | --- | --- |
| pipeline document JSON | 7 | 拒绝,不转换 |
| typed batch payload | 4 | 拒绝 |
| root state / checkpoint | 5 | 拒绝 |
| mini-Git WAL record | 5 | 拒绝 |
| adjustment transfer schema | `alcedo.adjustment_transfer.v5` | 拒绝 |
| commit/chain hash | 5 | 拒绝 |

**Checklist / exit condition:** NM9.2.4 表全部覆盖;所有删除入口共用 owner 校验;
格式边界真实失败；序列化期望数据使用 expected_serialized，测试使用 Expected* 名称。

**LOC note (grill-code-review):** 62 files, +1535/-194(含 13 个期望数据文件目录迁移)。
`analytic_mask_creation_test.cpp` 已超 1000 LOC(1035),拆分点:controller 创建/
编辑/删除历史 vs overlay geometry/coverage;留待 NM9.3 触碰时执行。

**Residual gaps:** 实际 Viewer 键盘/adapter 删除入口的 e2e 由 NM9.3 面板接线后验证
(当前 analytic 测试直接调用 app controller);mixed batch 的 GPU 侧渲染证据同 NM9.5。

**删除入口清单:** 节点草稿删除(`EditorNodeController::deleteColorGrade`/
`EditorNodeGraphDraft::RemoveColorGrade`)、完整 topology batch、组删除
(`RemoveColorGradeAndBridge`)、Mask 行/抽屉删除(`EditorHistoryMutation::RemoveMask`)、
Viewer Delete(`EditorNodeController` adapter)、直接 service
(`EditorSessionService`/`PipelineDocument::ValidateUserDeletion`)。

### NM9.3 — 面板、创建与选择

**目标与交付物：** 交付可操作的 Mask Groups 页面，复用节点/Mask 选择与创建流程，
完成空态、锁、键盘、Loader 生命周期和 monochrome 表达。NM9.4 接入真实缩略图结果。

#### NM9.3.1 布局输入与文件入口

第 2.6 节已经选择 C，Mask 合成仅显示 Add，Mask 与 Group 均有删除按钮。缩略图行为
以第 5 节为准；本阶段的细分另行整理。此处保留原步骤作为拆分依据，不把其他候选
实现为可切换 UI。面板结构不得以 NM9.4 的全部图像完成作为可见/可操作的前提。

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
   子行显示 Mask 类型、单 Mask 预览区域、Add、opacity 和锁/删除。按方案 C 多组展开；
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
   和有效对象路由，文本输入中不删除组或 Mask。为第 5.6 节的 PendingDelete 与领域
   完成通知保留明确入口；删除入队不等于成功，不得等待 thumbnail 生成/取消/释放。
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


##### Phase NM9.3 completion record (2026-09-16)

**Status:** partial — implementation present on `feature/mask-groups-workspace`;
required workspace and visual acceptance are not yet complete.

**Primary success call chain:**

```text
Mask Groups [+]/row/delete (EditorMaskGroupsPanel.qml / EditorMaskGroupDelegate.qml)
  -> EditorNodeController::insertMaskGroupAtTop / removeMaskGroup / selectNode
  -> EditorSessionController::SubmitInsertColorGradeAtTop / SubmitRemoveColorGradeAndBridge
  -> EditorSessionService::InsertColorGradeAtTop / RemoveColorGradeAndBridge (owner atomicity)
  -> PipelineGraphCommands AddCleanColorGrade / RemoveColorGradeAndBridge + typed history commit
  -> projection refresh -> both panel and Nodes update; selection restored per successor rule
```

**Primary failure call chain:**

```text
stale generation / draft / endpoint / backend rejection
  -> EditorNodeController SetLastError, command generation guard, no partial mutation
  -> row and selection retained; exact reason surfaced via panel statusMessage
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MaskGroupsPageCreatesThenRemovesGroupWithoutOpeningNodes` | `EditorNodesPanelQmlTest` | PASS |
| `MaskGroupDeleteFailureKeepsRowAndSelectionUntilSuccessfulRetry` | `EditorNodesPanelQmlTest` | PASS |
| `MaskGroupsRestoreScrollAndDrawersAcrossImagesVersionsAndLoader` | `EditorNodesPanelQmlTest` | PASS |
| `MaskGroupsKeepSelectedTextAndActionsVisibleAtAllPanelWidths` | `EditorNodesPanelQmlTest` | PASS |
| Full panel regression | `EditorNodesPanelQmlTest` | 45/45 PASS |
| Layout/controller/delegate/mask suites | `EditorNodeSelectionLayoutTest`, `EditorNodeDelegateQmlTest`, `AnalyticMaskCreationTest` | 112/112 PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build --preset win_debug --target EditorNodesPanelQmlTest --parallel 4`
then gtest filter on the four new tests, then full binary run;
`ctest --test-dir build/debug -R "^(EditorNodeSelectionLayoutTest|EditorNodeDelegateQmlTest|AnalyticMaskCreationTest)\." -C Debug`
(100% tests passed, 0 failed out of 112).

**Checklist / exit condition:** not yet satisfied. The required `WorkspaceShellTest` run
reported 48 passed, 6 failed and 1 skipped out of 55. Serial failure isolation is in progress.
Thumbnail evaluation remains NM9.4 scope; empty preview areas do not prove coverage rendering.

**LOC note (grill-code-review):** the earlier claim that all changed files were below 1000
lines was incorrect. In particular, `workspace_shell_test.cpp` has 2844 lines. A complete
changed-file count and responsibility assessment remain outstanding.

**Residual gaps:** DPR 1.5/2 visual verification and the complete NM9.3 interaction matrix
remain unproven. These are NM9.3 acceptance requirements, not work transferred to NM9.5.

##### Phase NM9.3 ordering increment record (2026-09-17)

**Status:** Mask Group ordering implemented and revised to reuse the Nodes topology-edit
path. One drop produces one `NodeGraphTopologyChange`, one typed history commit, and one
`GraphTopologyChanged` render request; the wider NM9.3 acceptance gaps above are unchanged
by this increment.

**Primary success call chain (drag-and-drop reorder, revised 2026-09-17):**

```text
whole-header / drawer-body MouseArea drag (EditorMaskGroupDelegate dragBody,
  Y axis; action buttons and mask rows keep their own handling)
  -> reorderDragStarted/Moved/Dropped(contentY)
  -> EditorMaskGroupsPanel groupDropSlot: nearest insertion boundary from
     sibling midpoints -> accent hairline indicator clamped inside the
     content rect so the top/bottom slots stay visible during drag
  -> finishGroupDrag maps boundary -> final downstream-first index
  -> EditorNodeController::moveMaskGroupToIndex(nodeId, targetIndex)
     maps the downstream-first row order back to Develop-to-DRT identity order
     (index 0 = nearest DRT; last = nearest Develop; out-of-range clamps;
      same index = accepted no-op with no command)
  -> short-lived EditorNodeGraphDraft connects each adjacent identity in final order
     and materializes the minimal NodeGraphTopologyChange (no node copies inserted/removed)
  -> EditorSessionController::SubmitNodeGraphTopologyEdit
  -> EditorSessionService::EditNodeGraph (queued off-owner; session/generation checks)
  -> IEditorHistoryPort::EditNodeGraph
  -> EditorSessionHistoryPort -> EditorHistoryMutation::EditNodeGraph
  -> MakeEditNodeGraphBatch -> typed NodeGraphTopologyChange commit
  -> RenderReasonForBatch -> EditorRenderReason::GraphTopologyChanged
  -> projection refresh -> Nodes and Mask Groups rows agree; selection preserved

Keyboard parity: Ctrl+Up / Ctrl+Down on the focused header calls the same
moveMaskGroupToIndex with index -1/+1.
```

**Primary failure call chain:**

```text
endpoint, unknown id, non-grade, draft, stale generation
  -> rejected before submission, no backend call
drop on the same slot / clamped to current index -> accepted no-op:
  no command, no commit, no render; card snaps back
invalid generated topology -> local draft rejects before submission
journal append failure -> batch rollback restores document, head, and render
  reason; the card snaps back to its row and the exact error surfaces
```

**What was proven (executed tests):**

| Required name / criterion | Target / binary | Result |
| --- | --- | --- |
| `MoveMaskGroupToIndexMovesAcrossMultiplePositionsAndKeepsSelection` (multi-slot drop = one topology command; delta has no inserted/removed nodes and exactly the changed edges), `MoveMaskGroupToIndexClampsOutOfRangeIndices`, `MoveMaskGroupToIndexSameSlotIsAnAcceptedNoOp` (including a single-group backbone), `MoveMaskGroupToIndexRejectsEndpointsUnknownDraftStaleAndFailure` | `EditorNodeSelectionLayoutTest` filtered run | 4/4 PASS |
| `MaskGroupDragReordersCardAndRewiresBackbone`, `MaskGroupDragToBottomSlotRewiresBackboneTowardDevelop`, `MaskGroupDragFromCardEdgeShowsEdgeSlotIndicators`, `MaskGroupDragWithinOwnSlotDoesNotSubmitCommand`, `MaskGroupDragDoesNotStartWhileStructureLocked`, `MaskGroupCtrlArrowMovesGroupOneStep`, `MaskGroupDragFailureKeepsRowUntilRetry` | `EditorNodesPanelQmlTest` filtered run | 7/7 PASS |

Commands:
`cmd /c scripts\msvc_env.cmd --build build\debug --target EditorNodeSelectionLayoutTest EditorNodesPanelQmlTest --parallel 4`, followed by the two filtered runs above.

**UI notes (revised 2026-09-17 — replaces the earlier chevron-pair design):**
reordering is drag-and-drop on the card itself. The drag surface is the
whole header minus the lock/delete buttons plus the drawer body around the
mask rows — grabbing the fold-arrow zone, preview, name, or empty card space
all lift the same `dragBody` (card + open drawer) on the Y axis while the
delegate slot stays fixed, so sibling midpoints — and thus the drop-slot
resolution — stay stable. A 2 px `appTheme.accentColor` hairline inside the
ListView content item marks the live insertion boundary (`groupDragSlot`);
its y is clamped into the content rect so the first/top and last/bottom
slots stay visible instead of clipping outside the list bounds. The dragged
card dims to 94% and the list flick is disabled while a drag is active. A
drop is mapped boundary→index accounting for the source-row shift
(`slot > source ? slot - 1 : slot`), then goes through the same
`structureEditable` gate as every other structure command; a rejected or
same-slot drop animates the card back via the `y` Behavior and submits
nothing. Keyboard parity is Ctrl+Up / Ctrl+Down on the focused header
(reorderEnabled rows also advertise it in `Accessible.description`). The
compact-button exception was removed with the chevrons; DESIGN.md documents
the drag affordance instead.

**Residual gaps:** same as the 2026-09-16 record (DPR/visual matrix, WorkspaceShellTest
serial isolation). Ordering adds no new snapshot or parallel model: rows keep
reading the shared `mask_group_snapshot` projection after each committed move.

### NM9.4 — 跨图内存 LRU 与提交后异步缩略图

**目标与交付物：** 完整实现第 5 节：项目级 CPU 小图 service、精确内容键、1000 项
固定容量 LRU、commit 后更新、面板逐项加载，以及不等待生成任务的删除与安全回调。
第 5 节为行为规格；不得沿用旧计划的 GPU 小图依赖、拖动刷新或切图清缓存实现。

#### NM9.4.1 前置检查与文件入口

1. NM9.3 已提供方案 C、稳定 NodeId/MaskId 与共享选择。确认创建、删除、Undo/Redo、
   checkout 的领域完成通知；记录哪个通知表示成功 commit，哪些仅代表 preview/入队。
   现有 `EditorMaskCreationAdapter::removeMask` 已注明 Enqueue 不是 deletion admission，
   接线必须尊重这条边界；Group controller 的返回值也须追踪到真实完成语义。
2. 核对 [ProjectHandler](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/project_handler.cpp)
   的项目服务创建/替换/关闭位置，将 service 生命周期放在同一层。执行前验证是否已有
   合适的后台执行器可复用，但不得与 photo render/session 串行任务共用阻塞工作队列。
3. 核对第 5.8 节的 Library manager/provider/LRU 和解析求值文件，以及
   [EditorSessionService](../../../../../alcedo_studio/src/include/app/editor_session_service.hpp)、
   [Mask adapter](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_mask_creation_adapter.cpp)、
   [Node controller](../../../../../alcedo_studio/src/ui/alcedo_main/album_backend/editor_node_controller.cpp)。
   UI 经 app service/controller 请求，不直接访问 live document 或在 QML 中计算键。
4. 拟新增文件为 `include/app/mask_thumbnail_service.hpp`、
   `app/mask_thumbnail_service.cpp` 及必要的 Qt 显示适配器；名称在实施后换成实际入口。
   只为明确职责拆分；不引入通用后台任务框架、第二个文档 owner 或照片 pipeline。

#### NM9.4.2 实施顺序与必要结果

1. **内容键和求值。** 按 5.2/5.3 逐字段实现 Single/Group 键、完整相等比较和 128×128
   R8 输出。补齐 committed geometry 的取样映射；在键定义处说明最小不可变输入的
   必要性、字段、owner、捕获时机、释放点和不写回规则。
2. **项目级 KV/LRU。** ready map 与 LRU 同步修改，默认严格 1000 项；读写短锁保护。
   成功命中提升 MRU，旧参数、旧图片、旧 Version 的图自然保留。构造 service 不依赖
   disk cache、Storage 或照片解码；项目切换创建隔离实例，单图切换不 Clear。
3. **有界异步请求。** 一个独立 worker，按 Key 合并 pending；队列限于仍有可见订阅的
   请求，同一目标最多一个。取消的未启动任务及时移除；在途任务不持文档/渲染锁，
   可完成并缓存。Group 按成员键串行查/算/合成，不递归排队等待自身。
4. **提交驱动的协调。** 只在明确成功 commit 或已提交文档恢复后计算受影响键；初次
   进入可见区域加载当前键。preview 不触发任何 service 请求。隐藏后保留 KV，重新
   显示时再请求；非 Mask 元数据和 Grade 调整不触发无关请求。
5. **逐项展示。** 面板结构与按钮先呈现；每行独立 Empty/Loading/Ready/Error。Image
   异步读取精确输出句柄；收到一张显示一张，不设置全列表完成门槛。仅通知目标角色，
   不 reset model、重置 contentY 或借完成通知重复提交编辑。
6. **两次回调资格核对。** 工作完成时过滤取消订阅，GUI 真正执行时再核对弱接收方、
   binding、target 存在性、PendingDelete、request_id、Key。成功与错误都做相同校验；
   cache fill 与 UI 发布分开，旧内容允许缓存，旧订阅不能更新新行。
7. **非阻塞删除。** 按 5.6 的顺序接入所有删除入口。提交前撤销受影响订阅，成功前
   保留行和选择，失败后新建订阅；成功后通过领域投影移除。使用已有异步命令/完成
   通道，必要时补齐适配器的完成接线，不把排队成功当删除成功。不能让 thumbnail
   cancel、像素释放或 GPU/QSG 生命周期成为删除操作的等待条件。
8. **资源与失败收口。** provider 暂时持有的输出与 reader 共用只读存储；KV 淘汰后
   reader 仍安全，最后一个引用释放才回收。失败不缓存伪造空图、不自动切生成方式。
   项目关闭异步收尾旧 worker，旧完成不能进入新项目或复活面板。
9. **诊断与记录。** 记录 ready 数量/字节、hit/miss/evict、pending/running 数、取消、
   过期 UI 回调、失败和单项发布数。分别统计 cache、可见句柄、worker scratch 和 Qt
   上传内存；不逐像素/paint 打日志，不把 1000 项额度误写成所有内存的总上限。

#### NM9.4.3 必须实现的受控验收

下面的暂停点使用可控 executor、promise/latch 或事件队列推进；不得用随机 sleep
制造先后顺序。每项记录生成次数、请求/发布次数及 owner 提交结果，不能只验证无崩溃。

| 用例 | 明确断言 |
| --- | --- |
| 固定输入 Radial/Gradient/反相/羽化/opacity | 128×128 R8、线性灰度、独立解析期望；黑边反相后仍黑，记录边界点与量化容差 |
| Group 相离、相交、重复与半强度 | max 正确；两个 50% 的交集仍为 50%；关闭/删除成员后旧 coverage 消失 |
| 无 Mask / 全关闭 | 前者没有请求与图片，后者 Ready 全黑且关闭 rows 保留 |
| 横/竖/方/奇数尺寸、裁剪旋转、边缘外 Mask | 统一 resolver 映射、保持比例；Viewer pan/zoom 与 DPR 改变不换键、不生成 |
| element 与全部内容参数的键测试 | 每个有效求值字段变化造成正确 miss；名称、锁、ID、会话/Version/revision 不改变同内容键；强制哈希碰撞不返回错图 |
| 单图相同参数不同 MaskId、Group 显示重排 | 相同内容复用，纯成员显示顺序变化命中；NodeId/MaskId 仍用于 UI 精确路由 |
| A→B→A、离开重开单图编辑器 | A 的条目仍在，回到 A 的生成计数不增加；容量内的其他图片不被清空 |
| 参数 P1→commit P2→Undo→Redo | P1/P2 各生成一次；之后命中；历史恢复使用新 request_id，旧请求不重新有效 |
| 连续 preview、取消、提交失败、no-op commit | preview/取消/失败新增请求和生成均为零；已缓存图不变；成功且键变化才提交请求 |
| 容量=3，插 A/B/C、读 A、插 D；默认容量=1000 | 首例只淘汰 B；1001 项后仍为 1000，键/像素一致；不因 pin 或淘汰频繁自动扩容 |
| LRU 淘汰时 provider/QSG 仍读旧图 | KV 项数遵守容量，已获取句柄的像素仍有效；reader 释放后内存回收 |
| service 完成后暂停 Qt 图片加载，再删除/复用行 | Qt 晚到的 ready/error 也不能更新新目标；已取到的旧像素句柄安全释放 |
| 多个目标同时请求相同 Key | 一个生成任务、各有效订阅分别完成；取消其中一个不影响其余目标 |
| 暂停全部生成，打开面板 | 组/Mask 行与删除等按钮已可用；不等待任何 thumbnail Ready |
| 分别完成行 C、A、B | 每次只更新对应 source/status；无全列表 reset、选择变化或滚动跳动 |
| 暂停正在生成的 Mask，提交删除 | 生成闩锁仍未释放时，领域删除已完成、行已移除；随后完成仅可入 KV，UI 发布为零 |
| callback 已排入 GUI 队列后再 Delete，领域尚未完成 | PendingDelete 已使 request_id 无效；推进旧 callback 不更新仍存在的行 |
| 同上但 owner 因锁/history 失败拒绝删除 | 原行/选择保留，PendingDelete 清除；新订阅可加载，旧 callback 仍不能发布 |
| 删除 Group，多个子行请求未完成 | Group 与所有子行订阅均失效，删除不等待任何请求；每个晚到结果都不能恢复 UI |
| 删除→Undo 恢复同 ID/同参数，旧回调最后执行 | 新订阅可命中缓存或加入仍在运行的同 Key 作业；旧 request_id 即使 Key 相同也不能发布 |
| K1 慢完成、K2 先绑定；旧失败最后到达 | 只更新当前 K2；旧成功/失败均不能覆盖当前图或错误状态 |
| 切图/checkout/Loader 销毁/delegate 复用/离屏 | 撤销旧接收资格，队列清理，缓存保留；回调不按 row index 写到其他对象 |
| 项目关闭后旧结果到达，新项目使用相同 element 数字 ID | 不串项目，不重建旧 UI；旧 service 安全释放且 GUI 未 join worker |
| 注入分配/求值/缓存插入失败，再显式重试 | 正确 Error、pending 清理；不缓存空图、不修改文档，之后合法请求可成功 |
| 大量切图/滚动/commit 与面板关闭 | ready 严格有界；pending 不留取消目标，running≤1；显示句柄随可见项释放，无离屏永久表 |
| 禁止磁盘 I/O 的 service fixture | 请求、命中、淘汰、关闭均不访问文件/DB/编码器；应用重启后从空内存缓存开始 |

#### NM9.4.4 成功链、失败链与交付证据

**成功链：** committed owner 数据 → 可见目标求 Key → service 命中/合并/排队 →
后台解析求值或 Group max → LRU 插入 → GUI 核对当前订阅 → 单项角色更新 → Image
读取共享输出 → reader 结束释放。每个环节均不要求其他缩略图先完成。

**删除链：** 同一共享 controller 撤销订阅 → 领域异步删除 → 成功投影移除/失败新订阅；
thumbnail worker 可以独立完成。取消结果回调不能反向触发删除、复原或选择命令。

**失败链：** 原因返回有效订阅的 Error；已撤销订阅不发布。cache miss 是正常请求，
取消是无显示结果，均不修改 history。项目关闭时旧任务仅释放自己持有的资源。

交付实际 service/key/provider/controller 调用链、上述矩阵、生成/发布计数和内存上限。
拟新增 `MaskThumbnailServiceTest` 与面板 thumbnail QML fixture（实施后填写实际目标），
并沿已有 CUDA/OpenCL/Metal Mask 测试做解析像素对照；独立解析期望必须存在，不能只
与生产函数自身比较。只修改 CPU/Qt 小图不要求重写三套 GPU 生成器；涉及的原生路径
分别验证，Windows/macOS 的 QML 图像消费与生命周期分别记录实测或缺口。

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
   格式错误和 thumbnail service 求值/分配失败。断言 document/head/selection 的前后状态及
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
| CPU 小图求值 | 删除 Mask → Undo | 删除不等待求值；旧结果可进 LRU，新订阅可命中，旧回调不得发布 |
| 小图回调/Qt 上传 | checkout / 图像切换 / Loader 销毁 | KV 保留，旧订阅失效；无越界访问、无串图、无旧回调重新创建 UI |
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
   仅在验证工具中隔离这一变量；记录照片 input-to-present、producer、独立 CPU 小图
   求值/排队/Qt 上传时间、命中率、取消/丢弃数和分类内存。持续 preview 的小图请求数
   必须为零，另测 commit 后显示延迟。不要在产品中增加降低质量的“快速模式”。
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
| NM9.4 | 原生像素对照可用 `GpuDagCudaMaskTest`、`GpuDagOpenClGradeTest`、`GpuDagMetalGradeTest`；新增 service/QML 目标实施后登记 | 第 NM9.4.3 节完整矩阵、固定容量跨图 LRU、无磁盘、commit 触发、逐项显示与删除先于生成完成 |
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
| `StaleMaskThumbnailCannotReplaceCurrentImagePreview` | 旧订阅不能发布到新绑定；同内容缓存可由新订阅复用 |
| `MaskThumbnailCacheRetainsOtherImagesUntilLruEviction` | A→B→A 命中，固定容量淘汰，不因切图清空 |
| `MaskEditsRequestThumbnailsOnlyAfterSuccessfulCommit` | 编辑 preview/取消/失败请求数为零，成功且键变化才更新；首次可见加载单独验证 |
| `MaskDeletionCompletesWhileThumbnailWorkerIsPaused` | 暂停生成时删除照常完成，晚到回调不能恢复行 |
| `MaskPanelShowsRowsBeforeAnyThumbnailCompletes` | 全部生成暂停时行和操作已显示，完成逐项更新 |
| `PanelSwitchPreservesIncompleteNodeConnections` | 未连接节点可定位，无隐式提交或丢弃 |
| `EquivalentEditsFromBothViewsProduceMatchingPixels` | 同一任务从两入口执行后 DAG、参数和最终像素一致 |

## 8. 完成记录

NM9.1/NM9.2 已完成，记录见对应阶段；NM9.3–NM9.6 仍为 planned。本次完善规格不代表
service 或新面板已实现。
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
| NM9.4 | Key 精确包含哪些输入？如何跨图命中并严格限制 1000 项？哪些事件触发请求？暂停 worker 时删除是否完成？旧回调、Undo 同 ID、逐项展示与 QSG reader 如何验证？有无磁盘 I/O？ |
| NM9.5 | 哪些磁盘 reopen、WAL、Version/Paste 和异步交错真正执行？故障发生后文档、head、选择和资源分别是什么状态？ |
| NM9.6 | 六项任务最终图/像素是否一致？每轮时间和操作数是多少？缩略图成本是多少？有哪些仍未解决的交互或平台问题？ |

临时日志、截图和原始测量表放在 `build/tmp/mask_group_panel/nm9_N/`，不提交到仓库。
文档保留必要的结果表、命令、输入定义和路径，避免结论只能依赖后来可能被清理的临时文件。
NM9 完成时同步总方案第 21.10 节与第 26 节的实际状态；NM10 不因这次验收增加内容。

下一阶段：[NM10 — Adjustment Transfer](phase_nm10_adjustment_transfer_plan.md)，仅保留占位。
