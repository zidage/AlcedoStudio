<p align="center">
  <img src="docs/header.png" alt="Alcedo Studio" width="25%"/>
</p>

<p align="center">
  <a href="https://aoraw.org/zh-cn/">项目网站</a> · <a href="https://aoraw.org/">Project website</a> · <a href="https://zidage.github.io/AlcedoStudio_docs/docs/intro">文档</a> · <a href="https://github.com/zidage/AlcedoStudio/releases/tag/v0.3.5">下载 v0.3.5</a>
</p>

<p align="right"><a href="./README.md">English</a> | <a href="./README.zh-CN.md"><strong>简体中文</strong></a></p>

![License](https://img.shields.io/badge/License-GPLv3-blue)
![C++](https://img.shields.io/badge/C++-20-blue)
![Platforms](https://img.shields.io/badge/Windows%20%7C%20macOS%20(Apple%20Silicon)-lightgrey)

**Alcedo Studio** 是一款免费、开源的 RAW 编辑器与照片图库。

将照片导入后，你可以在统一的项目中浏览、评分、检索，并通过全链路 32 位浮点 GPU 管线冲洗 RAW 图像，即使面对高像素中画幅文件也能流畅编辑。相册结构和每张照片的完整编辑历史都保存在同一个项目文件里，移动或备份它就像处理任何普通文件一样。

支持 Windows 10/11（x64，CUDA/OpenCL）和搭载 Apple Silicon 的 Mac（Metal）。

<p align="center">
  <img src="docs/images/editor.jpg" alt="Alcedo Studio 编辑器：打开一张 RAW 照片，右侧为调整面板，底部为胶片条" width="100%"/>
</p>

<details>
<summary><strong>演示视频</strong>（两段短视频）</summary>

编辑器

https://github.com/user-attachments/assets/d70cd10d-2045-42f3-a67d-97ab3ef9874b

图库

https://github.com/user-attachments/assets/ae0d9773-220e-4901-90f6-1989f58b0462

</details>

## 核心特性

- **场景参考的节点式色彩管线**  
  全链路基于 32 位浮点场景线性空间。调色过程组织为直观的节点图：`Develop` 节点完成 RAW 解码与色彩空间转换，后续可串联任意数量带独立蒙版的 `Color Grade` 节点。最终画面由内置的 ACES 2.0 或 OpenDRT 显示渲染变换形成，也可以交给光谱胶片模拟 Film Print（胶片印片）。

- **基于 DAG 的 GPU 交互管线**  
  调色处理采用有向无环图（DAG）架构，全链路基于原生 CUDA / OpenCL（Windows）与 Metal（macOS）的 32 位浮点计算。每个节点与蒙版独立缓存中间结果，调整滑块时只重算后面的变化，而不需要从头重跑整张照片。拖动时即时响应，松开后自动呈现完整画质；放大查看时也仅对当前屏幕区域渲染 1:1 细节，处理高像素大文件完全不需要等待漫长的离线代理。

- **风格化工具箱：光谱胶片模拟、胶片 LUT 与颗粒**  
  胶片印片以光谱方式模拟 30 款彩色负片和 5 款彩色反转片：场景光让各感光层曝光，显影形成染料，负片再印放到摄影相纸或电影印片胶片上，或经扫描反相后输出 SDR 或 HDR；反转片以灯箱观看方式呈现。颗粒随 16mm、35mm、70mm 片幅和各片种的颗粒度数据变化。另有 113 款胶片 LUT，并支持任意色彩空间的自定义 3D CUBE LUT。

- **AI 蒙版**  
  一键选择主体、背景、天空或人物，也可以通过点选、绘制路径或框选来选择任意物体。支持添加、减去选区，调整羽化和扩展，并可按明度和色相范围限制任意蒙版。模型在本机运行；已保存的蒙版存放在项目中，渲染时无需重新运行模型。

- **轻量本地图库（DuckDB + 本地 CLIP）**  
  相册数据直接由内嵌的 DuckDB 驱动，在海量图库下依然支持复杂的毫秒级条件筛选；结合本地运行的多语言 CLIP 模型，导入即可自动为照片生成语义标签，支持自然语言检索，全程零云端依赖。层级影集、星级和编辑标记帮助整理图库。

- **类似 Git 的分支化历史**  
  每次修改都记录在非线性的版本树中，可随时从任意历史步骤切出新版本进行比对，所有操作均可永久撤销，并支持将 Look 局部或完整批量应用至其他照片。

- **1:1 的命令行与智能体协同**  
  图形界面中的每个交互（从筛片、调滑块到分支管理）在 `alcedo-cli` 中均有对应命令。配合 [alcedo-skills](https://github.com/zidage/alcedo-skills)，Claude Code 或 Codex 等智能体可以在后台批量处理，或在当前窗口中协同调色，将静态预设转变为能理解画面内容的动态脚本。

## 功能深入

### RAW 解码

Alcedo 可以读取 Canon、Nikon、Sony、Fujifilm、Panasonic、OM System、Leica、Hasselblad、Phase One（包括 1.5 亿像素的 IQ4）、Pentax、Sigma 的 RAW 文件，以及手机和无人机生成的 DNG，详见[支持的格式](docs/supported_raw_formats.md)和[支持的相机](docs/supported_cameras.md)。借助项目维护的 [LibRaw 分支](https://github.com/zidage/LibRaw)，它也能打开 Z 8、Z 9、Z 6 III 和 Z 50 II 拍摄的 Nikon 高效率（HE）NEF 文件。

它提供两种去马赛克方法。Bayer 传感器默认使用 RCD；Fujifilm X-Trans 默认使用 Neural Engine，这是一个从 [DemosaicNet](https://groups.csail.mit.edu/graphics/demosaicnet/) 蒸馏而来、在 GPU 上运行的精简神经网络，同样适用于 Bayer 文件。Alcedo 还支持基于 darktable 与 RawTherapee 的 inpaint-opposed 方法改进而来的高光重建、基于 Lensfun 配置文件和 Lanczos-3 重采样的镜头校正，以及 DNG 文件内嵌的色彩配置文件。

### 场景参考的色彩管线

Alcedo 在整个编辑过程中保留 RAW 文件的全部动态范围，只在最后一步为你的显示器形成画面：

```mermaid
flowchart LR
    A[RAW 文件] --> B[解码与去马赛克]
    B --> C[场景线性图像]
    C --> D[在 ACEScc 中调色]
    D --> E[显示渲染变换：<br/>ACES 2.0、OpenDRT 或胶片印片]
    E --> F[SDR、广色域或 HDR 显示器]
```

显示渲染变换可以选择 ACES 2.0 输出变换、OpenDRT 或胶片印片。使用 ACES 2.0 和 OpenDRT 时，可以设置目标色彩空间、传递函数和峰值亮度。在配备 HDR 显示屏的 Mac 上，编辑器可以直接预览 HDR 效果。

胶片印片是一种光谱胶片模拟。它把调色后的场景线性图像当作照到胶片上的光：为每个像素重建光谱，按各感光层的光谱灵敏度计算曝光，模拟乳剂中的光扩散和光晕，再按特性曲线和层间抑制完成显影。显影后的胶片进入三种输出之一：

```mermaid
flowchart LR
    A[场景线性图像] --> B[光谱重建]
    B --> C[各感光层曝光]
    C --> D[光扩散与光晕]
    D --> E[显影与<br/>层间抑制]
    E --> F[负片密度]
    F --> G[印放到相纸<br/>或电影印片胶片]
    F --> H[负片扫描]
    E --> I[反转片<br/>灯箱观看]
    G --> J[Rec. 709 SDR]
    I --> J
    H --> K[OpenDRT：SDR 或 HDR]
```

- **印片**：30 款负片可以搭配任一摄影相纸或电影印片胶片，包括 Cine Look（Kodak Vision 2383）。
- **负片扫描**：虚拟扫描器读取并反转负片，再由 OpenDRT 形成 SDR 或 HDR 画面。可以调整扫描曝光、对比度、青、品、黄和黑位。“中性”自动校色在缩放和平移时保持全画面的色彩平衡。
- **反转片**：5 款彩色反转片以灯箱观看方式呈现。

“场景光”默认使用“按拍摄参数”，读取 RAW 白平衡的色温与色调。片幅（16mm、35mm 或 70mm）决定颗粒相对画面的大小，较小片幅也会放大光扩散和光晕。颗粒在印片或扫描之前形成于胶片层中，并使用所选片种的颗粒度数据。胶片浏览器可以按工艺和品牌筛选，并显示每款胶片的色卡。胶片印片支持 CUDA、OpenCL 和 Metal，所需胶片数据随安装包提供。物理原理、数据来源和 GPU 实现请参阅[胶片模拟说明](docs/roadmap/alcedo_studio/edit/film_simulation/film_simulation_explained.md)。

### 基于节点的调色

每张照片的编辑都是一张节点图。Develop 节点把 RAW 转换为场景参考图像，之后可以依次串联任意多个 Color Grade 节点，最后由 Display Transform 节点形成最终画面。每个 Color Grade 节点都有自己的调整和蒙版，因此一次调色既可以作用于整个画面，也可以只作用于你选定的区域。

```mermaid
flowchart LR
    D[Develop] --> G1[Color Grade：基础]
    G1 --> G2[Color Grade：天空<br/>渐变蒙版]
    G2 --> G3[Color Grade：主体<br/>径向蒙版]
    G3 --> T[Display Transform]
```

每个节点内都有常用的影调和色彩控制、色调曲线、Lift/Gamma/Gain 与 Offset 色轮、可选颜色和细节工具，阴影与高光由局部色调映射处理。Alcedo 还内置了[根据真实胶片光谱响应生成的 CUBE LUT](https://github.com/JanLohse/spectral_film_lut)，以及基于物理模型的胶片颗粒和光晕。颗粒强度范围为 0 至 200%，缩放时颗粒尺度保持一致。

### AI 蒙版与范围蒙版

Color Grade 节点可以使用渐变、径向、画笔和 AI 蒙版。AI 蒙版可以选择主体、背景、天空或人物，也可以选择你通过点选、绘制路径或框选标出的任意物体。支持添加、减去选区，单独加入或移除人物，以及调整羽化和扩展；红色叠层显示当前选区。工具内有独立的撤销与重做，点击“确认”后将结果保存为一条历史记录。已保存的 AI 蒙版存放在项目中，渲染时无需重新运行模型。

任意蒙版都可以用明度/色相范围进一步限制，提供双滑块范围条、数值输入、柔和度和饱和度门槛。“显示选区”可查看实际覆盖范围。

AI 蒙版模型在本机运行，可在“设置 > AI 模型”中下载，每张模型卡片显示许可证、大小和下载进度。

### 任意色彩空间的 LUT

除了官方胶片 LUT 包，你也可以使用自己的 3D CUBE LUT：把它们放进 LUT 浏览器提示的用户 LUT 文件夹，它们就会出现在“User”分类下。你可以为每个 LUT 选择它期望的输入编码和它产生的输出编码。场景编码包括 ACES、ARRI LogC3 和 LogC4、Fujifilm F-Log、Panasonic V-Log、Nikon N-Log、Apple Log 和 DaVinci Intermediate；显示编码包括 sRGB、BT.1886、PQ 和 HLG。当 LUT 输出的是显示编码时，Alcedo 会用 ACES 2.0 逆输出变换把结果带回场景空间，让这个 LUT 像其他调整一样在场景参考管线中工作。Alcedo 可以记住某个 LUT 的编码组合，每次使用时自动应用；把调整复制到其他照片时，也会一并带上这些编码。LUT 浏览器可以按分类、来源、品牌和印片筛选。

### 版本与编辑历史

每张照片可以保存多个命名版本，你也可以从历史中的任意一步分出新版本。每一次调整都会记录在类似 Git 的历史中，并保存在项目文件里，因此可以永远撤销。你还可以把完整的 Look，或只把其中你选中的部分，复制到其他照片上。实现原理请参阅[编辑历史设计说明](https://zidage.github.io/AlcedoStudio_docs/en/docs/developer/edit-history-architecture)。

### 图库与搜索

图库存储在 [DuckDB](https://duckdb.org/) 数据库中，照片越来越多时，浏览、筛选和搜索依然流畅。相册检查器按拍摄日期、相机、镜头、标签和评分对照片分组，选中某个分组即可筛选相册。层级影集以紧凑的树形显示，支持就地新建和重命名、拖动移动，并显示照片数量。照片卡片显示编辑标记和星级，较大的卡片支持点击星星评分。

Alcedo 支持按相机、镜头、日期、ISO、焦距和光圈进行 EXIF 搜索，以及使用在本地运行的多语言 CLIP 模型进行自然语言搜索，这些模型也可以在导入后自动为照片打标签。CLIP 模型在“设置 > AI 模型”中下载和激活。搜索面板把最近搜索、命令、筛选项和照片预览集中在一个窗口，按 Enter 即可将搜索应用到图库。

### 导出

Alcedo 可以导出 JPEG、PNG、TIFF、OpenEXR 和 Ultra HDR JPEG，并根据格式支持 8、16 或 32 位。它支持按长边、像素尺寸或打印尺寸设置大小，嵌入 ICC 配置文件，并用源文件名、拍摄日期、相机、镜头、曝光参数、评分和序号组合文件名。导出预设（Standard、Web、Social Media、Email、Maximum Quality JPEG、16-bit TIFF 和 EXR Archive）会设置除目标位置以外的所有选项，你也可以保存自己的预设。每次导出都可以选择是否保留源照片的 EXIF 元数据、ICC 配置文件和 GPS 定位。相机与镜头序列号以及编辑历史永远不会写入导出的文件。

### AI 智能体与 alcedo-cli

alcedo-cli 安装在应用程序旁边（macOS 上位于应用包内）。无论是智能体、脚本，还是在终端里操作的你，都能通过它做到界面能做的事：

- **项目与图库**：创建、打开和保存项目；导入文件夹；列出、评分、选择和删除照片；读取拍摄元数据和缩略图；并按搜索对话框的规则搜索，包括自然语言搜索。
- **编辑**：打开照片，读取和设置编辑器中的任意调整，一步设置多个字段，操作节点和 LUT，列出胶片印片可用的胶片，撤销与重做，读取编辑历史。
- **版本**：创建、分支、切换、重命名和删除版本，在照片之间复制和粘贴调整。
- **输出**：渲染预览供智能体查看，使用与导出面板相同的选项导出，并跟踪后台任务直到完成。

智能体有两种工作方式。无窗口会话在后台运行，适合批量任务。智能体也可以接管你正在使用的窗口：在它控制期间，你的编辑控件会被锁定，但仍然可以平移、缩放和查看示波器。智能体会话固定在任务岛和后台任务对话框的顶部，并列出它执行的操作；点击 Stop 会立即结束它的控制。智能体的编辑会在历史中标出，并显示它留下的说明，方便你逐条检查或撤销。

```sh
alcedo-cli session start --headless --project <项目>.alcd
alcedo-cli call search.query '{"text": "海边的日落"}'
alcedo-cli schema          # 每个命令的参数和结果
alcedo-cli session stop
```

配合 [alcedo-skills](https://github.com/zidage/alcedo-skills) 仓库中的 `alcedo` 技能，智能体可以形成「渲染预览 → 观察画面 → 调整参数」的闭环，根据实际画面内容动态调整，而非机械套用固定预设。安装时，直接对智能体说“Install the skill zidage/alcedo-skills”即可；在 Claude Code 中也可以运行 `/plugin marketplace add zidage/alcedo-skills` 和 `/plugin install alcedo@alcedo-skills`。新功能也不需要编译进二进制插件，编写轻量 Python 脚本即可直接与管线交互。例如，`contact_sheet.py` 通过一次批量调用为照片生成联系表用于挑选评分；`analyze_preview.py` 则从预览中测量影调、色偏与地平线倾斜，结果直接写回项目。完整的[命令参考](https://github.com/zidage/alcedo-skills/blob/main/skills/alcedo/references/README.md)（英文）在同一个仓库中。

## 截图

<table>
  <tr>
    <td width="50%"><img src="docs/images/library.jpg" alt="图库网格，相册检查器按相机和镜头分组"/></td>
    <td width="50%"><img src="docs/images/display-transform.jpg" alt="显示变换面板，显示 OpenDRT 和 HDR 输出设置"/></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/nodes.jpg" alt="节点编辑器中有多个 Color Grade 节点，画面上显示蒙版"/></td>
    <td width="50%"><img src="docs/images/masks.jpg" alt="蒙版编辑器，画面上显示蒙版"/></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/images/search.jpg" alt="图库中的自然语言搜索结果"/></td>
    <td width="50%"><img src="docs/images/export.jpg" alt="导出面板，显示格式、尺寸和文件命名选项"/></td>
  </tr>
</table>

## 系统要求

- **Windows** 10 或 11，x64。计算能力 6.0 及以上的 NVIDIA GPU 使用 CUDA，其他 GPU 使用 OpenCL。
- **macOS** 13.3 或更高版本，Apple Silicon（M1 或更新），使用 Metal。
- 内存至少 8 GB，大型图库建议 16 GB 或以上。

## 文档

用户指南和开发者说明请见[文档网站](https://zidage.github.io/AlcedoStudio_docs/docs/intro)。从源码构建请参阅 [docs/build_from_source.md](docs/build_from_source.md)，各版本的更新说明位于 [docs/changelog/](docs/changelog/)。如需从终端、脚本或 AI 智能体控制 Alcedo Studio，请参阅 [alcedo-cli 文档](https://github.com/zidage/alcedo-skills/blob/main/skills/alcedo/references/README.md)（英文）。该文档属于 [alcedo-skills](https://github.com/zidage/alcedo-skills) 仓库中的智能体技能。

## 致谢

Alcedo Studio 基于许多开源项目及其作者的工作。

- 胶片模拟 LUT 来自 [JanLohse/spectral_film_lut](https://github.com/JanLohse/spectral_film_lut) 和 [andreavolpato/spektrafilm](https://github.com/andreavolpato/spektrafilm)。
- 胶片印片的光谱模拟受 [andreavolpato/spektrafilm](https://github.com/andreavolpato/spektrafilm) 和 [DhaliwalX/fotufilm-engine](https://github.com/DhaliwalX/fotufilm-engine) 启发。胶片数据使用 [spectral_film_lut](https://github.com/JanLohse/spectral_film_lut) 数字化的数据手册，以及 fotufilm 的 Astia 100F 配置文件（CC BY-SA 4.0）。
- AI 蒙版使用 [SAM 2.1](https://github.com/facebookresearch/sam2)（Hiera tiny）完成点选、路径和框选，使用 [S3OD](https://github.com/KupynOrest/s3od)（[s3od-synth 权重](https://huggingface.co/okupyn/s3od-synth)，图像编码器为 [DINOv3](https://github.com/facebookresearch/dinov3)）完成主体抠图（matting）。
- 部分相机色彩矩阵来自 [rawtoaces-data](https://github.com/AcademySoftwareFoundation/rawtoaces-data)。
- 神经网络去马赛克模型蒸馏自 [mgharbi/demosaicnet](https://github.com/mgharbi/demosaicnet)（[Gharbi 等，2016](https://groups.csail.mit.edu/graphics/demosaicnet/)）。
- Inpaint-opposed 高光重建改编自 [darktable](https://github.com/darktable-org/darktable/blob/master/src/iop/hlreconstruct/opposed.c) 和 [RawTherapee](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc)。
- RCD 去马赛克改编自 [LuisSR/RCD-Demosaicing](https://github.com/LuisSR/RCD-Demosaicing)。
- OpenDRT 移植自 Jed Smith 的 [open-display-transform](https://github.com/jedypod/open-display-transform)。
- ACES 2.0 输出变换依据 [aces-aswf/aces-core](https://github.com/aces-aswf/aces-core) 实现。
- 胶片颗粒基于 [Realistic Film Grain Rendering](https://doi.org/10.5201/ipol.2017.192)（IPOL 2017）。
- RAW 解码使用 [LibRaw](https://www.libraw.org/)，镜头校正使用 [Lensfun](https://lensfun.github.io/)，元数据使用 [Exiv2](https://exiv2.org/)，图像读写使用 [OpenImageIO](https://github.com/AcademySoftwareFoundation/OpenImageIO) 和 [OpenCV](https://opencv.org/)，色彩管理使用 [OpenColorIO](https://opencolorio.org/)，图库使用 [DuckDB](https://duckdb.org/)，界面使用 [Qt](https://www.qt.io/) 和 [QuickQanava](https://github.com/cneben/QuickQanava)。

完整的第三方组件及其许可证列表请参阅 [THIRD_PARTY_NOTICE.txt](THIRD_PARTY_NOTICE.txt) 和 [third_party_licenses/](third_party_licenses/)。

ACES 是美国电影艺术与科学学院（A.M.P.A.S.）的商标。Alcedo Studio 是独立项目，与 A.M.P.A.S.、Academy Software Foundation、Jed Smith 以及上文提到的其他上游作者均无关联，也未获得其认证或背书。文中提及这些名称仅用于标明相应的技术。Kodak、Fujifilm 等胶片名称是其所有者的商标，在此仅用于标明胶片印片所模拟的胶片。

## 许可证

Alcedo Studio 使用 GNU 通用公共许可证 v3.0（GPL-3.0-only）。请参阅 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
