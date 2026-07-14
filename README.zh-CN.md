# Alcedo Studio

项目网站：[English](https://aoraw.org/) | [简体中文](https://aoraw.org/zh-cn/)

<p align="right"><a href="./README.md">English</a> | <a href="./README.zh-CN.md"><strong>简体中文</strong></a></p>

![License](https://img.shields.io/badge/License-GPLv3-blue)
![CUDA](https://img.shields.io/badge/CUDA-12.8-76B900)
![C++](https://img.shields.io/badge/C++-20-blue)
![AI](https://img.shields.io/badge/AI-CLIP%20%2B%20VLM-ff6f00)

**Alcedo Studio** 是一款专为追求极致速度、完全隐私和本地 AI 体验的摄影师打造的次世代 RAW 图像处理与影集管理系统。它不模仿任何传统软件的臃肿，而是以全新的 GPU 加速管线与本地端侧 AI，重新定义从导入、智能检索到创意调色的完整工作流。

---

## 早期演示与截图

视频演示1：[BiliBili](https://www.bilibili.com/video/BV1bPcxzzEeM)

视频演示2 (带解说)：[BiliBili](https://www.bilibili.com/video/BV1sFfjBeE3n)

<table>
  <colgroup>
    <col style="width: 76%" />
    <col style="width: 24%" />
  </colgroup>
  <tbody>
    <tr>
      <td><img src="docs/screenshots/1-主界面.png" alt="Alcedo Studio 图库浏览器" width="100%" /></td>
      <td><strong>图库影集浏览器</strong> —— 极速加载的缩略图网格、物理文件夹树、活跃搜索维度、星标过滤器与 AI 语义标签集成。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/7-高级筛选.png" alt="高级筛选与过滤" width="100%" /></td>
      <td><strong>高级筛选与过滤</strong> —— 结合 EXIF 元数据、星标和 AI 语义标签，在庞大的库中进行精准的组合搜索与筛选。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/8-AI内容识别.png" alt="本地 AI 视觉引擎" width="100%" /></td>
      <td><strong>本地 AI 视觉引擎</strong> —— 自由开启与管理本地 CLIP 或 SigLIP 端侧模型， 100% 数据隐私安全。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/9-AI内容过滤.png" alt="AI 语义标签过滤" width="100%" /></td>
      <td><strong>AI 语义标签过滤</strong> —— 本地 AI 自动生成的语义标签会作为一级属性融入过滤器面板，与其他参数无缝组合。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/10-AI自然语言搜索.png" alt="自然语言智能搜索" width="100%" /></td>
      <td><strong>自然语言智能搜索</strong> —— 像与人对话一样输入画面描述（如“海边日落人像”），在本地向量索引中实现即时的高匹配度排序。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/2-色彩科学.png" alt="电影级色彩科学" width="100%" /></td>
      <td><strong>电影级双色彩科学</strong> —— 提供 ACES 2.0 和 OpenDRT 规范的输出渲染，带显示色彩空间与峰值亮度调整。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/3-基础调整.png" alt="实时影调基础调整" width="100%" /></td>
      <td><strong>实时影调基础调整</strong> —— 支持曝光、对比度、白平衡、色调曲线和局部高光/阴影（LLF）微调，带实时直方图反馈。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/4-高级色彩.png" alt="创意调色板" width="100%" /></td>
      <td><strong>创意调色板</strong> —— 精准控制 HSL 调整、色彩轮（Lift, Gamma, Gain）创意分级与实时示波器。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/5-几何调整.png" alt="几何畸变与透视修复" width="100%" /></td>
      <td><strong>几何畸变与透视修复</strong> —— 包含裁剪、旋转、镜头畸变与透视修复等基本工具。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/Portra 400.png" alt="胶片模拟与配方" width="100%" /></td>
      <td><strong>胶片模拟与配方</strong> —— 原生支持针对 ACEScc/ACEScct 流程优化的 Kodak、Fuji 和 Agfa 预设（.cube 格式 3D LUT）。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/5-胶片颗粒与Halation模拟.png" alt="真实胶片质感模拟" width="100%" /></td>
      <td><strong>真实胶片质感模拟</strong> —— 基于物理模型的颗粒与光晕（Halation）仿真，在 CPU/GPU 混合管线上高效渲染。</td>
    </tr>
    <tr>
      <td><img src="docs/screenshots/6-导出界面.png" alt="高级批量导出" width="100%" /></td>
      <td><strong>高级批量导出</strong> —— 支持多格式并行队列，带精细位深控制、ICC 元数据选择和 UltraHDR 增益图写入。</td>
    </tr>
  </tbody>
</table>

> 项目使用的一部分演示 RAW 文件来自 [signatureedits](https://www.signatureedits.com/free-raw-photos/) 的 100% 免费 RAW 文件。

---

## 自 v0.2.3 以来的主要变化

自 v0.2.3 启用新品牌 Alcedo Studio 以来，项目逐步从原型探索过渡到实用的专业 RAW 编辑与资产管理系统。

| 版本周期 | 核心更新亮点 |
| --- | --- |
| **v0.2.4** | 迁移 GPU 后端至 OpenCL，新增 OpenCL 图像容器、镜头校正、透视修复、DNG 扭曲校正及示波器分析。Sleeve 虚拟文件系统引入了影集分组、分页、星级标记与全局索引搜索。 |
| **v0.2.5** | 基于 LLF（局部拉普拉斯滤波器）重构高光/阴影局部影调处理；优化 OKLCh 与 HLS 调色空间；增加批量复制/粘贴修图参数；新增交互式裁剪叠加层及 UltraHDR 增益图导出。 |
| **v0.2.6** | 全面落地 AI 原生工作流：内置本地语义搜索、本地 CLIP/SigLIP 影库扫描、HNSW 向量索引及异步模型下载。新增 CPU/GPU 胶片颗粒与光晕（Halation）模拟。修补 LibRaw 分支以原生解码尼康 HE/HE* 压缩格式。 |

---

## 五大核心支柱

### 1. 专业级 32 位浮点、双色彩科学与 HDR 工作流管线
编辑是 Alcedo Studio 的灵魂。为了保证极致的画质、精准的色彩与流畅度，我们绝不在计算精度和渲染性能上妥协。
* **32 位浮点与高保真解码**：从使用高质量 **RCD 算法** 执行 RAW 去马赛克（demosaic），到高光重建，全流程均在 32 位单通道浮点精度下运算，完整保留阴影与高光里的每一丝细节。
* **原生双色彩科学**：原生内置 ACES 2.0 与 OpenDRT 色彩科学渲染，完美匹配不同物理设备的色彩空间、EOTF 曲线和峰值亮度。
* **真 HDR 与 UltraHDR 工作流**：这绝非市面上那种靠算法强行拉曝、暴力提亮的假 HDR 滤镜，而是**对 RAW 传感器捕获到的宽广动态范围进行完整、忠实的还原与再现**。配合 ACES 2.0/OpenDRT 映射，将高动态光影无损呈现于 HDR 监视器（仅 macOS 支持应用内实时预览），并支持导出标准 **UltraHDR Gain-map 增益图 JPEG**，在现代 HDR 屏幕和社交媒体平台上完美再现拍摄现场的真实光强。
* **GPU 硬件级百帧级流畅**：Windows 下运行 **CUDA** 与 **OpenCL** 内核，macOS 下原生运行 **Metal** 渲染器，现代显卡下预览可达 **数百帧/秒**，拉动调整滑块完全没有延迟。

### 2. 独家原生支持尼康高效率压缩 (HE/HE*) RAW
尼康特有的高效率压缩（`HE`）和高效率压缩★（`HE*`）NEF 格式在 LibRaw 0.22 中至今未被支持。Alcedo Studio 为此独立编写了解码补丁，免除了将大批照片转为 DNG 的繁杂步骤。
* **告别格式转换瓶颈**：直接导入并极速编辑来自尼康 Z 8、Z 9、Z 6 III、Z f、以及 Z 50 II 的高压缩 RAW 照片。
* **高度优化的补丁分支**：内置针对多线程 and 硬件指令集深度优化的 LibRaw 解码器，实现流畅的无感导入。

### 3. 完全本地化的端侧 AI 语义搜索与打标
告别机械的文件名检索与繁琐的手动整理。Alcedo Studio 拥有完全运行于本机的向量搜索引擎，基于先进的端侧视觉模型（CLIP / SigLIP）。
* **自然语言检索**：只需在搜索框中输入“海边日落人像”或“森林中的越野车”，本地向量索引即可在毫秒级内给出语义匹配度最高的照片列表。
* **100% 数据隐私安全**：所有图像处理与向量化过程均在本地 GPU 运行，照片绝不上云，保护创作者的绝对隐私，且无需支付任何 API 密钥或订阅费用。
* **模型独立隔离**：支持灵活切换 MobileCLIP2、SigLIP2、Jina CLIP v2 或 macOS CoreML 模型，不同模型的语义标签包彼此隔离，防止标签混淆与污染。

### 4. 多模态大模型智能评片与选片
让 AI 成为你的专业“第二审核人”，帮你在海量废片中快速筛选出合格的照片。通过接入标准大模型接口（兼容 OpenAI 格式、Anthropic 或火山方舟豆包），可在后台开启智能助理。
* **精准揪出废片痛点**：AI 助理能自动识别画面中常见的技术与美学缺陷，例如**拍摄失焦（脱焦）、手抖模糊（运动抖动）以及构图失误**，在开始精修前直接过滤废片。
* **1-5 星智能打分**：基于定制的审美逻辑对候选片自动评级，**打分将直接写入图像标准的 EXIF 元数据**，这意味着你的评分在任意看图或管理软件中都能同步读取。
* **安全与个性化**：API Key 安全保存在操作系统密钥串（Keychain）中，绝不记录到日志或项目文件。可调的“严格度人设”（从宽容到挑剔）可满足不同的挑选要求。

### 5. 极简轻量、即开即走的单文件项目管理
彻底告别传统修图软件臃肿的集中式数据库，消除多机同步时数据库损坏或迁移困难的烦恼。
* **单文件 `.alcd` 设计**：一个项目，一个文件。所有的修图历史、多版本参数、AI 语义标签和虚拟目录结构，都保存在项目文件夹下一个小巧的 DuckDB 数据库文件（.alcd）中。
* **完美的便携性**：只需将包含照片和 `.alcd` 的文件夹整体复制、剪切或备份到外置硬盘/云盘，即可瞬间在另一台设备上无缝继续工作。
* **缩略图缓存解耦**：磁盘缩略图缓存与核心元数据完全独立，确保项目主体文件始终轻量小巧。

---

## 更多核心功能

### 无损版本管理（编辑历史）
* **多命名版本分支**：为同一张照片创建不同的创意分支（例如“高反差黑白” vs “暖调胶片模拟”），无需复制 RAW 源文件，轻松进行多版本比较。
* **独立的撤销/重做时间线**：每个版本分支拥有自己专属的修改日志。你可以随时在操作记录上挪动播放游标，或者一键将修图配方克隆到其他照片。
* **底层机制**：基于类似 Git 的内容寻址架构，采用 Merkle 树哈希根来识别编辑状态的唯一性。详情见 [底层架构细节说明](docs/technical/architecture_details.md)。

### RAW 处理与风格化
* **高保真去马赛克**：原生内置 **RCD demosaic 算法**，提供精细、清晰的 RAW 文件解码并有效抑制伪影。
* **胶片模拟与风格化**：支持针对 ACEScc/ACEScct 色彩空间优化的 CUBE 格式 LUT，内置基于 GPU 加速的真实物理颗粒与光晕（Halation）模拟渲染器。

### 批量导出队列
* **多格式输出**：支持批量并行将 RAW 渲染导出为 JPEG、PNG、TIFF 及 WebP 格式，支持 8/16/32 位深度。
* **UltraHDR 增益图与 HDR 格式**：完美支持 UltraHDR Gain-map 增益图 JPEG 导出，在现代 HDR 屏幕或移动设备上可呈现高亮度、高动态范围的绚丽视觉效果。
* **元数据及 ICC 配置文件**：可自由控制 EXIF/IPTC 信息的剥离或保留，并为每批导出任务嵌入专属的 ICC 颜色配置文件。

> [!TIP]
> 想要了解更硬核的 Sleeve 虚拟文件系统数据库设计、内存三级 LRU 缓存指标（如 786 张 42MP RAW 下 767MB DRAM 的测试数据）及 GPU 多后端渲染架构，请参阅专门维护的 [底层架构细节说明](docs/technical/architecture_details.md)。

---

## RAW 与相机支持

Alcedo Studio 通过补丁版 [LibRaw](https://github.com/zidage/LibRaw) 分支支持导入目前绝大多数主流相机的 RAW 格式：

- Canon CR2 / CR3
- Nikon NEF
- Sony ARW
- Fujifilm RAF
- Panasonic RW2
- Olympus / OM System ORF
- Leica、Hasselblad、Phase One、Pentax、Sigma、Samsung
- DNG，包括主流智能手机和航拍无人机生成的 DNG 文件

完整格式列表见 [docs/supported_raw_formats.md](docs/supported_raw_formats.md)，已验证支持相机列表见 [docs/supported_cameras.md](docs/supported_cameras.md)。

### 独家支持：尼康 HE / HE\* RAW 压缩格式
对于目前在 LibRaw 0.22 官方上游仍未提供支持的 Nikon High-Efficiency (`HE`) 与 High-Efficiency★ (`HE*`) NEF 文件，Alcedo Studio 自带的 Patched LibRaw 提供了原生的直接解码能力，无需转为 DNG。已通过实机照片测试的机型包括：
- Nikon Z 8
- Nikon Z 9
- Nikon Z 6 III
- Nikon Z f
- Nikon Z 50 II

解码补丁开源在：**https://github.com/zidage/LibRaw**

---

## 系统要求

- **Windows 10/11 x64**：建议搭配支持 CUDA 的 NVIDIA GPU（最低计算能力 6.0，即 10 系列或更高；推荐 7.0+，即 20 系列或更高），配备 6GB+ VRAM 以流畅处理 40MP+ 的高像素 RAW 文件。非 NVIDIA 显卡可使用 OpenCL 执行加速。
- **macOS**：支持 Apple Silicon 芯片（M1/M2/M3/M4 系列）的 Mac 硬件，原生运行 Metal 图像管线。
- **系统内存**：最少 8GB RAM（推荐 16GB+ 以保证大型图库浏览的顺畅度）。
- **硬盘空间**：至少 600MB 可用空间用于程序安装及工作缓存，安装包大小约为 130MB+。

---

## 源码构建

源码构建与依赖配置（中英文对照）请参考 [docs/build_from_source.md](docs/build_from_source.md)。

---

## 开发路线图

开发路线图与当前里程碑规划可以在 [docs/roadmap/roadmap.md](docs/roadmap/roadmap.md) 中查看。

---

## 致谢

Alcedo Studio 建立在开源生态与图像学领域众多杰出研究的基础之上：

- 胶片模拟 LUT 配方来源于 [JanLohse/spectral_film_lut](https://github.com/JanLohse/spectral_film_lut)。
- 相机色彩矩阵参数来源于 [AcademySoftwareFoundation/rawtoaces-data](https://github.com/AcademySoftwareFoundation/rawtoaces-data).
- 高光重建算法改编自 RawTherapee 中的 [hilite_recon.cc](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc).
- Demosaic RCD 算法来源于 [LuisSR/RCD-Demosaicing](https://github.com/LuisSR/RCD-Demosaicing).
- OpenDRT 色彩科学移植自 [OpenDRT.dctl](https://github.com/jedypod/open-display-transform/blob/main/display-transforms/opendrt/OpenDRT.dctl).
- ACES 2.0 色彩管理移植自 [aces-aswf/aces-core](https://github.com/aces-aswf/aces-core).
- 胶片物理颗粒算法基于 Alasdair Newson 等人的论文 *[Realistic Film Grain Rendering](https://doi.org/10.5201/ipol.2017.192)* 实现。

---

## 许可证

项目在 `v0.1.1` tag（含）以前的版本继续遵循 Apache-2.0 协议。
`v0.1.1` 以后的开发版本遵循 `GPL-3.0-only`，并在根目录 [LICENSE](LICENSE) 中根据 GPLv3 第 7 条附带一项针对合并和分发 NVIDIA CUDA 组件的补充许可说明。
详情请见 [LICENSE](LICENSE) 及 [NOTICE](NOTICE) 文件。
