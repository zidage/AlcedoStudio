<p align="center">
  <img src="docs/header.png" alt="Alcedo Studio" width="25%"/>
</p>

[项目网站](https://aoraw.org/zh-cn/) | [Project website](https://aoraw.org/)

<p align="right"><a href="./README.md">English</a> | <a href="./README.zh-CN.md"><strong>简体中文</strong></a></p>

![License](https://img.shields.io/badge/License-GPLv3-blue)
![CUDA](https://img.shields.io/badge/CUDA-12.8-76B900)
![C++](https://img.shields.io/badge/C++-20-blue)

**Alcedo Studio** 是一款免费、开源的摄影工作站，为拍摄完成后的工作而生。

导入存储卡中的 RAW，图库随即就能浏览。通过 GPU 上的 32 位浮点管线，为高达 1.5 亿像素的照片调色。

每个项目都是一个带有额外元数据的 [DuckDB](https://duckdb.org/) 文件。相册结构和完整编辑历史都保存在其中。只需移动和保存一个文件，整洁，没有散落的数据。

更进一步，调色遵循电影工业的成像流程。相机 RAW 首先成为场景参照图像。你在对数空间中工作，就像 DI 调色系统处理 ACEScc 一样。胶片模拟 LUT 带来不同风格，最后由显示渲染变换为显示器形成画面。

支持 Windows 10/11 x64 和 Apple Silicon。当前版本：[v0.2.9](https://github.com/zidage/AlcedoStudio/releases/tag/v0.2.9)。

---

https://github.com/user-attachments/assets/d70cd10d-2045-42f3-a67d-97ab3ef9874b

https://github.com/user-attachments/assets/ae0d9773-220e-4901-90f6-1989f58b0462

## 功能

**先看相机支持，再谈 RAW 质量。** 已测试 Canon、Nikon、Sony、Fujifilm、Panasonic、OM System、Leica、Hasselblad、Phase One（包括 IQ4 150MP）、Pentax、Sigma，以及手机和无人机生成的 DNG。完整列表：[支持的格式](docs/supported_raw_formats.md)和[支持的相机](docs/supported_cameras.md)。来自 Z 8、Z 9、Z 6 III 和 Z 50 II 的 Nikon HE 与 HE★ NEF 文件通过项目的 [LibRaw 分支](https://github.com/zidage/LibRaw)解码，并针对性能进行了特别优化。

你可以选择去马赛克方法：Default、RCD 或 Neural Engine。Neural Engine 是在 GPU 上运行的精简版 [DemosaicNet](https://groups.csail.mit.edu/graphics/demosaicnet/)，支持 Bayer 和 X-Trans。高光重建使用改进的 inpaint-opposed 方法，其原始实现来自 darktable 和 RawTherapee。

**2.5K@60，保持流畅。** 拖动滑块时，预览以 2.5K 分辨率保持 60 FPS。停止调整后，预览最高可达 4K，让高像素相机的表现完整呈现。RGB 直方图和波形图随画面同步更新，为下一步调整提供参考。NVIDIA 使用 CUDA，其他 Windows GPU 使用 OpenCL，macOS 使用 Metal。缓存机制在保持性能的同时降低内存占用。缩略图磁盘缓存的位置、大小、质量以及是否启用，都由你决定，不会出现来源不明的磁盘占用。

**显示渲染变换决定画面如何呈现。** 场景线性 RAW 包含显示器无法直接呈现的色彩和动态范围。DRT，也就是画面形成，是把这些内容映射到显示设备时所做的视觉决策。关于 DRT 的更多信息，请参阅 [Chris Brejon 对画面形成的介绍](https://chrisbrejon.com/articles/what-makes-a-good-picture-formation/)。你可以使用 ACES 2.0 或 OpenDRT，面向 sRGB、广色域或 HDR 进行调色，并选择编码色彩空间、EOTF 和 HDR 峰值亮度。在 macOS 上，编辑时还能直接预览 HDR 效果。

**胶片模拟 LUT。** [CUBE LUT 根据真实胶片的光谱响应生成](https://github.com/JanLohse/spectral_film_lut)，并搭配依据物理特性设计的颗粒和光晕效果。

**保留、复制，也能随时回退的 Look。** 命名版本可以为同一张照片保存不同 Look，方便直接比较。你可以把 Look 复制到另一张照片，也可以把一个 Look 中的字段合并到另一个 Look。每一次调整都是可以撤销的步骤。完整历史保存在项目文件中，即使下个月重新打开项目，撤销路径依然存在。

**使用高度可定制的配置导出。** 在检查器中设置格式、尺寸、命名、元数据、ICC 和 Alpha 通道。支持 JPEG、PNG、TIFF 和最高 32 位的 EXR。尺寸可以使用原始像素、最长边、像素边界，或带 DPI 的打印尺寸。文件名可以包含源文件名、拍摄日期、相机、镜头、曝光、评分和序号。

**评片与评分。** 接入你已经使用的 LLM，支持兼容 OpenAI 的服务、Anthropic 和 Volcengine Ark。它可以把描述、1–5 星评分和简短理由写入 EXIF。你可以设置评片的严格程度。

**打上标签，马上找到。** 本地多语言 CLIP 模型为图库生成标签。输入一个场景、一台相机、一个日期或一句描述即可搜索。检查器也会按拍摄日期、相机、镜头、标签和评分整理同一个图库。你可以自行管理 CLIP 模型：下载新模型、为当前图库启用已有模型，或删除已下载的模型以释放空间。

**大型拍摄项目也能保持快速。** DuckDB 可以让整张存储卡中的照片同时参与查询。筛选某个星期六、35mm 镜头和五星照片，网格会立即更新。检查器还能告诉你，那一周每台机身分别拍摄了多少张照片。

FTS 让描述或标签中的文字直接成为查询条件。搜索“红色雨伞”时，它匹配的是画面描述，而不只是文件名。HNSW 则让画面相似的照片在向量空间中彼此接近。即使没有人把那句话写进文件名，一段描述也能按含义找到对应画面。

把项目文件放在你需要的位置。移动它，备份它。

## 系统要求

- **Windows**：Windows 10/11 x64。NVIDIA GPU（计算能力 6.0 或更高）使用 CUDA；其他 GPU 使用 OpenCL。
- **macOS**：Apple Silicon（M1 或更新），macOS 13.3 或更新，使用 Metal。
- 最少 8GB RAM，大型图库建议使用 16GB 或更多内存。
- 正式版本可以在“设置 → 更新”中安装经过签名的更新。

## 文档

用户指南和构建说明：[文档网站](https://zidage.github.io/AlcedoStudio_docs/docs/intro)。源码构建：[docs/build_from_source.md](docs/build_from_source.md)。由 AI 智能体制定的开发计划：[docs/roadmap/roadmap.md](docs/roadmap/roadmap.md)。变更日志：[docs/changelog/](docs/changelog/)。

## 致谢

- 胶片 LUT 来自 [JanLohse/spectral_film_lut](https://github.com/JanLohse/spectral_film_lut)。
- 相机色彩矩阵来自 [rawtoaces-data](https://github.com/AcademySoftwareFoundation/rawtoaces-data)。
- Neural demosaic student models 蒸馏自 [mgharbi/demosaicnet](https://github.com/mgharbi/demosaicnet)（[Gharbi 等，2016](https://groups.csail.mit.edu/graphics/demosaicnet/)）。
- Inpaint-opposed 高光重建改编自 [darktable opposed.c](https://github.com/darktable-org/darktable/blob/master/src/iop/hlreconstruct/opposed.c) 和 [RawTherapee](https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc)。
- RCD 去马赛克来自 [LuisSR/RCD-Demosaicing](https://github.com/LuisSR/RCD-Demosaicing)。
- OpenDRT 移植自 [jedypod/open-display-transform](https://github.com/jedypod/open-display-transform)。
- ACES 2.0 来自 [aces-aswf/aces-core](https://github.com/aces-aswf/aces-core)。
- 胶片颗粒基于 [Realistic Film Grain Rendering](https://doi.org/10.5201/ipol.2017.192)（IPOL 2017）。

## 许可证

Alcedo Studio 使用 GPL-3.0-only 许可证。请参阅 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。
