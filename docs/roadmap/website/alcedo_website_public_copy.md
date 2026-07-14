# Alcedo Studio 官网公开文案（Phase 1 内容冻结）

Date: 2026-07-13  
Status: **Approved for Phase 2**  
Primary roadmap owner: external `AlcedoStudio-site` repository  
Scope: 完整中英文公开文案；不含样式、HTML 实现或部署配置  
Source plan: [Alcedo Studio Website Redesign Plan](alcedo_website_redesign_plan.md)

## 使用说明

1. 本文件是 Phase 1 的唯一文案交付物。审核通过前，不进入 Phase 2 网页实现。
2. 实现时可调整换行与标点，**不得**自行增加宣传词、绝对化表述或内部技术名词。
3. 首页摘要与 Features 详述不互相复制长段落：首页一句，Features 两到三句。
4. 页面不展示版本号与发布日期；下载按钮指向当前安装包（实现阶段维护 URL）。
5. 审核通过后，将本文件 Status 改为 `Approved for Phase 2`。

---

## 1. 已冻结的产品事实

| 项 | 值 |
| --- | --- |
| 产品名 | Alcedo Studio |
| 类别（SEO 可见） | free open-source RAW photo editor and image manager |
| 定位 | modern photography workstation for RAW editing and image management |
| 价格 | Free |
| 许可证 | GPL-3.0-only |
| 平台（首页可见） | Windows 10/11 x64；macOS Apple Silicon |
| 当前参考版本（页面不显示） | v0.2.7（2026-07-06） |
| Windows 安装包（当前） | `AlcedoStudio-0.2.7-Windows-AMD64.exe` |
| macOS 安装包（当前） | `AlcedoStudio-0.2.7-Darwin-arm64.dmg` |
| 文档 | https://zidage.github.io/AlcedoStudio_docs/ |
| 源码 | https://github.com/zidage/AlcedoStudio |
| Issues | https://github.com/zidage/AlcedoStudio/issues |
| 全部发布 | https://github.com/zidage/AlcedoStudio/releases |
| 许可证文件 | https://github.com/zidage/AlcedoStudio/blob/main/LICENSE |
| 百度网盘（仅中文页） | https://pan.baidu.com/s/1fb8eDBrSaBkoxdrgXeawww?pwd=c2ai |
| GitHub Pages 规范根 | https://zidage.github.io/AlcedoStudio/ |
| 英文主页 | `/` 或 `/AlcedoStudio/` |
| 中文主页 | `/zh-cn/` 或 `/AlcedoStudio/zh-cn/` |
| 英文 Features | `/features/` 或 `/AlcedoStudio/features/` |
| 中文 Features | `/zh-cn/features/` 或 `/AlcedoStudio/zh-cn/features/` |

### 1.1 下载 URL 策略（实现约定，不进页面正文）

- 首页按钮文案只写动作：`Download for Windows` / `Windows 下载` 等。
- **当前阶段（GitHub Pages）**：Windows / macOS 按钮直接指向当前安装包的 GitHub Release 下载 URL；另提供 `All releases` / `全部版本` 文字链接。
- 当前具体 URL（发版时同步更新，页面不显示版本号）：
  - Windows: `https://github.com/zidage/AlcedoStudio/releases/download/v0.2.7/AlcedoStudio-0.2.7-Windows-AMD64.exe`
  - macOS: `https://github.com/zidage/AlcedoStudio/releases/download/v0.2.7/AlcedoStudio-0.2.7-Darwin-arm64.dmg`
- **后续（Cloudflare R2）**：改为固定 `releases/latest/` 对象；GitHub Releases 保留为后备。
- 中文页额外提供 `百度网盘`；英文页不显示。
- 不写“高速”“推荐线路”“国内加速”等修饰。

### 1.2 公开文案禁写清单

- 内部实现词：CUDA、OpenCL、Metal、DuckDB、CLIP、SigLIP、HNSW、sidecar、R2、Cloudflare 等。
- 包装词：privacy-first、core pillars、重新定义、为创作者而生、专业级体验、AI 驱动的下一代 等。
- 未经验证的绝对词：zero latency、all RAW formats、best-in-class 等。
- 认证与协议名（OpenAI OAuth、Anthropic-compatible 等）仅出现在教程，不出现在首页或 Features。

---

## 2. 共享界面文案

### 2.1 导航

| 用途 | English | 简体中文 |
| --- | --- | --- |
| 品牌 | Alcedo Studio | Alcedo Studio |
| Features | Features | 功能 |
| Download（锚点到本页下载区） | Download | 下载 |
| Documentation | Documentation | 使用教程 |
| GitHub | GitHub | GitHub |
| 切到中文 | 中文 | — |
| 切到英文 | English | English |

### 2.2 下载区共用标签

| 用途 | English | 简体中文 |
| --- | --- | --- |
| 区块标题 | Download | 下载 |
| Windows 主按钮 | Download for Windows | Windows 下载 |
| Windows 说明 | Windows 10/11, x64 | Windows 10/11，x64 |
| macOS 按钮 | Download for macOS | macOS 下载 |
| macOS 说明 | Apple Silicon | Apple Silicon |
| 源码 | View source | 查看源码 |
| 全部版本 | All releases | 全部版本 |
| 百度网盘（仅中文） | — | 百度网盘 |

### 2.3 文档与开源区

| 用途 | English | 简体中文 |
| --- | --- | --- |
| 区块标题 | Documentation and source | 教程与源码 |
| 教程说明 | Installation and user guide for import, editing, and export. | 安装、导入、编辑与导出说明。 |
| 教程链接 | Read the documentation | 阅读使用教程 |
| 仓库 | GitHub repository | GitHub 仓库 |
| Issues | Issue tracker | 问题反馈 |
| 许可证链接文字 | GPL-3.0 | GPL-3.0 |

### 2.4 页脚

| 用途 | English | 简体中文 |
| --- | --- | --- |
| 产品名 | Alcedo Studio | Alcedo Studio |
| 链接 | Documentation · GitHub · GPL-3.0 | 使用教程 · GitHub · GPL-3.0 |
| 语言 | English · 中文 | 中文 · English |
| 版权 | © 2026 Alcedo Studio | © 2026 Alcedo Studio |

### 2.5 404 页面

**English**

- Title: `Page not found — Alcedo Studio`
- H1: `Page not found`
- Body: `This page does not exist.`
- Links: `Home` · `Features` · `Documentation` · `GitHub`

**简体中文**

- Title: `页面不存在 — Alcedo Studio`
- H1: `页面不存在`
- Body: `该页面不存在。`
- Links: `首页` · `功能` · `使用教程` · `GitHub`

---

## 3. 英文主页 `index.html`

### 3.1 SEO / 分享

| 字段 | 文案 |
| --- | --- |
| `<title>` | Alcedo Studio — Free open-source RAW photo editor and image manager |
| `meta description` | Alcedo Studio is a free, open-source photography workstation for RAW editing and image management. Available for Windows 10/11 x64 and Apple Silicon Mac. |
| `og:title` | 同 `<title>` |
| `og:description` | 同 `meta description` |
| `og:type` | website |
| `og:url` | https://zidage.github.io/AlcedoStudio/ |
| `og:image` | https://zidage.github.io/AlcedoStudio/assets/social-card.png |
| Twitter card | summary_large_image |
| canonical | https://zidage.github.io/AlcedoStudio/ |
| hreflang en | https://zidage.github.io/AlcedoStudio/ |
| hreflang zh-CN | https://zidage.github.io/AlcedoStudio/zh-cn/ |
| hreflang x-default | https://zidage.github.io/AlcedoStudio/ |

### 3.2 JSON-LD（SoftwareApplication，与可见正文一致）

```json
{
  "@context": "https://schema.org",
  "@type": "SoftwareApplication",
  "name": "Alcedo Studio",
  "description": "A free, open-source photography workstation for RAW editing and image management.",
  "applicationCategory": "MultimediaApplication",
  "operatingSystem": "Windows 10, Windows 11, macOS (Apple Silicon)",
  "license": "https://www.gnu.org/licenses/gpl-3.0.html",
  "downloadUrl": "https://github.com/zidage/AlcedoStudio/releases/latest",
  "offers": {
    "@type": "Offer",
    "price": "0",
    "priceCurrency": "USD"
  }
}
```

### 3.3 首屏

**H1:** Alcedo Studio

**Lead:**  
An open-source photography workstation for RAW editing and library management.

**Support line:**  
Local search and optional AI-assisted descriptions and ratings are included.

**Platform line:**  
Available for Windows 10/11 x64 and Apple Silicon Mac.

**Actions:**

1. `Download for Windows`（主按钮）
2. `Download for macOS`
3. `Documentation`
4. `View source`（可选，可放在首屏次级或下载区）

### 3.4 主视觉

- 合成图文件名（实现阶段产出）：`assets/alcedo-library.webp` / AVIF 变体（库 + 编辑器叠放）。
- **Alt:** Alcedo Studio library browser with the photo editor open behind it
- 不写版本号；不套仿窗口框。

### 3.5 What it does（功能摘要）

**Section heading:** What it does

1. **RAW editing**  
   Adjust exposure, color, tone, and crop, then export photos.

2. **Photo library**  
   Import, browse, filter, rate, and organize photos.

3. **Library search**  
   Search by image content, camera model, shooting date, or combined criteria. Exact filenames are not required.

4. **AI descriptions and ratings**  
   Connect an AI service to generate descriptions, tags, ratings, and reasons. Rating strictness is adjustable, and ratings can be read by other photo software.

5. **Performance**  
   Imports and library previews are fast, with batch tools for large photo sets. Adjustment previews update in about 10 ms, and exports are processed quickly. Memory is allocated on demand to limit resource use.

**Link:** `View all features` → `features/`

### 3.6 Download

**Heading:** Download

| Platform | Note | Action |
| --- | --- | --- |
| Windows | Windows 10/11, x64 | Download for Windows |
| macOS | Apple Silicon | Download for macOS |
| Source | Source code | View source |

**Secondary link:** All releases

### 3.7 Documentation and source

**Heading:** Documentation and source

Installation and user guide for import, editing, and export.

- Read the documentation
- GitHub repository
- Issue tracker
- GPL-3.0

---

## 4. 简体中文主页 `zh-cn/index.html`

### 4.1 SEO / 分享

| 字段 | 文案 |
| --- | --- |
| `<title>` | Alcedo Studio — 免费开源 RAW 照片编辑与图片管理软件 |
| `meta description` | Alcedo Studio 是免费、开源的现代摄影师工作站，支持 RAW 编辑与图片管理，适用于 Windows 10/11 x64 与 Apple Silicon Mac。 |
| `og:title` | 同 `<title>` |
| `og:description` | 同 `meta description` |
| `og:type` | website |
| `og:url` | https://zidage.github.io/AlcedoStudio/zh-cn/ |
| `og:image` | https://zidage.github.io/AlcedoStudio/assets/social-card.png |
| Twitter card | summary_large_image |
| canonical | https://zidage.github.io/AlcedoStudio/zh-cn/ |
| hreflang en | https://zidage.github.io/AlcedoStudio/ |
| hreflang zh-CN | https://zidage.github.io/AlcedoStudio/zh-cn/ |
| hreflang x-default | https://zidage.github.io/AlcedoStudio/ |

### 4.2 JSON-LD

```json
{
  "@context": "https://schema.org",
  "@type": "SoftwareApplication",
  "name": "Alcedo Studio",
  "description": "免费开源的 RAW 修图与筛图软件，支持图片管理。",
  "applicationCategory": "MultimediaApplication",
  "operatingSystem": "Windows 10, Windows 11, macOS (Apple Silicon)",
  "license": "https://www.gnu.org/licenses/gpl-3.0.html",
  "downloadUrl": "https://github.com/zidage/AlcedoStudio/releases/latest",
  "offers": {
    "@type": "Offer",
    "price": "0",
    "priceCurrency": "CNY"
  }
}
```

### 4.3 首屏

**H1:** Alcedo Studio

**Lead:**  
面向摄影师的现代工作站。

**Support line:**  
编辑 RAW 照片、管理图库、搜索照片，也可以连接 AI 服务生成描述和评分。

**Platform line:**  
免费开源。支持 Windows 10/11 x64 和 Apple Silicon Mac。

**Actions:**

1. `Windows 下载`（主按钮）
2. `macOS 下载`
3. `使用教程`

### 4.4 主视觉

- 与英文页同一张合成图。
- **Alt:** Alcedo Studio 图库界面，后方为照片编辑器

### 4.5 功能摘要

**Section heading:** 功能

1. **RAW 编辑**  
   调整曝光、色彩、影调和构图，然后导出照片。

2. **图库管理**  
   导入、浏览、筛选、评分和整理照片。

3. **搜索图库**  
   按图片内容、相机型号、拍摄日期或组合条件查找照片，不需要准确文件名。

4. **AI 描述与评分**  
   连接你使用的 AI 服务，为照片生成描述、标签、评分和理由。评分标准可以调整，结果可以被其他照片软件读取。

5. **性能**  
   快速导入和预览图库，并支持批量管理。调整预览约 10 ms 更新，导出速度快。内存按需分配，控制资源占用。

**Link:** `查看详细功能` → `features/`

### 4.6 下载

**Heading:** 下载

| 平台 | 说明 | 操作 |
| --- | --- | --- |
| Windows | Windows 10/11，x64 | Windows 下载 |
| macOS | Apple Silicon | macOS 下载 |
| 源码 | 源代码 | 查看源码 |

**Secondary:** `全部版本` · `百度网盘`

### 4.7 教程与源码

**Heading:** 教程与源码

安装、导入、编辑与导出说明。

- 阅读使用教程
- GitHub 仓库
- 问题反馈
- GPL-3.0

---

## 5. 英文 Features 页 `features/index.html`

### 5.1 SEO / 分享

| 字段 | 文案 |
| --- | --- |
| `<title>` | Features — Alcedo Studio RAW editor and photo library |
| `meta description` | Details on RAW editing with ACES and OpenDRT color pipelines, LUT support, tone and color tools, local library management, search, AI ratings, and performance in Alcedo Studio. |
| `og:title` | 同 `<title>` |
| `og:description` | 同 `meta description` |
| `og:url` | https://zidage.github.io/AlcedoStudio/features/ |
| canonical | https://zidage.github.io/AlcedoStudio/features/ |
| hreflang en | https://zidage.github.io/AlcedoStudio/features/ |
| hreflang zh-CN | https://zidage.github.io/AlcedoStudio/zh-cn/features/ |
| hreflang x-default | https://zidage.github.io/AlcedoStudio/features/ |

### 5.2 页眉与导语

**H1:** Features

**Intro:**  
Alcedo Studio is a free, open-source photography workstation. This page describes the main tools for RAW editing, library management, search, optional AI assistance, and everyday performance.

**Top links:** `Home` · `Download` · `Documentation`

### 5.3 RAW editing

**Heading:** RAW editing

Adjust exposure, white balance, color, tone, and crop on RAW photos. The editor includes ACES and OpenDRT color pipelines, with LUT support for applying custom color looks.

Use tone curves, HSL, channel mixing, geometry and lens correction, crop and rotate, plus grain and Halation controls. Previews update while you work so you can judge the result before export.

Edits stay in the project until you export; original files are not overwritten. Finished photos can be exported in common formats for sharing or further use.

**Screenshot (lazy):** `3-基础调整.png` → optimized asset  
**Alt:** Exposure, tone, and color controls in the Alcedo Studio editor

**Additional RAW screenshots (lazy):** `2-色彩科学.png`, `2-自然影调1.png`, `2-自然影调2.png`,
`5-几何调整.png`, `5-胶片颗粒与Halation模拟.png`, `Portra 400.png` → optimized AVIF/WebP assets.
These images show ACES 2.0 and OpenDRT, tone and RAW decoding controls, geometry, film grain/Halation,
and the LUT browser.

### 5.4 Photo library

**Heading:** Photo library

Import photos into a local library, browse folders and thumbnails, and rate or organize large sets without uploading them to a cloud service.

Filters help you review by rating and other attributes. The library is built for day-to-day culling and management, not only single-image editing.

**Screenshot (lazy):** `1-主界面.png` → optimized asset  
**Alt:** Alcedo Studio library browser with thumbnail grid and folders

### 5.5 Library search

**Heading:** Library search

Search by image content, camera model, shooting date, or combined criteria. Exact filenames are not required.

Local content understanding can run on your computer for basic library search and labels. No cloud account is required for that path.

**Screenshot (lazy):** `10-AI自然语言搜索.png` → optimized asset  
**Alt:** Searching a photo library by content and metadata in Alcedo Studio

### 5.6 AI descriptions and ratings

**Heading:** AI descriptions and ratings

Connect your preferred AI service to generate descriptions, tags, ratings, and short reasons for selected photos.

Adjust how strict the review is. Ratings are written to standard EXIF metadata so other photo software can read them. Alcedo Studio does not sell a cloud AI service of its own; you use a service you already choose.

**Screenshot (lazy):** `8-AI内容识别.png` → optimized asset（若后续有评分界面截图可替换）  
**Alt:** AI description and rating results shown for a photo in Alcedo Studio

### 5.7 Performance

**Heading:** Performance

Imports and library previews are built for large photo sets and batch work. Adjustment previews update in about 10 ms so sliders stay responsive while editing.

Exports are processed quickly for finished work. Memory is allocated on demand to limit resource use during everyday library and editing tasks.

**Screenshot (lazy):** `6-导出界面.png` → optimized asset  
**Alt:** Export settings for finished photos in Alcedo Studio

### 5.8 页尾操作

**Heading:** Next steps

- Download for Windows
- Download for macOS
- Read the documentation
- Back to home

---

## 6. 简体中文 Features 页 `zh-cn/features/index.html`

### 6.1 SEO / 分享

| 字段 | 文案 |
| --- | --- |
| `<title>` | 功能说明 — Alcedo Studio RAW 编辑与图库管理 |
| `meta description` | 了解 Alcedo Studio 的 RAW 编辑、ACES 与 OpenDRT 色彩管线、LUT 支持、调色工具、图库管理、搜索、AI 描述与评分，以及性能表现。 |
| `og:title` | 同 `<title>` |
| `og:description` | 同 `meta description` |
| `og:url` | https://zidage.github.io/AlcedoStudio/zh-cn/features/ |
| canonical | https://zidage.github.io/AlcedoStudio/zh-cn/features/ |
| hreflang en | https://zidage.github.io/AlcedoStudio/features/ |
| hreflang zh-CN | https://zidage.github.io/AlcedoStudio/zh-cn/features/ |
| hreflang x-default | https://zidage.github.io/AlcedoStudio/features/ |

### 6.2 页眉与导语

**H1:** 功能说明

**Intro:**  
Alcedo Studio 是免费、开源的摄影师工作站。本页说明 RAW 编辑、图库管理、搜索、可选 AI 辅助，以及日常使用中的性能表现。

**Top links:** `首页` · `下载` · `使用教程`

### 6.3 RAW 编辑

**Heading:** RAW 编辑

在 RAW 照片上调整曝光、白平衡、色彩、影调和构图。编辑器提供基于 ACES 与 OpenDRT 的色彩科学管线，并支持使用 LUT 应用自定义色彩效果。

可使用色调曲线、HSL、通道混合、几何与镜头校正、裁剪与旋转，以及胶片颗粒和 Halation 控制。预览会随调整更新，便于在导出前确认效果。

修改保存在项目中，不会覆盖原始文件。完成后可导出为常用格式，用于分享或后续使用。

**Screenshot (lazy):** 同英文页 RAW 编辑图  
**Alt:** Alcedo Studio 编辑器中的曝光、影调与色彩调整

**Additional RAW screenshots (lazy):** 同英文页 RAW 编辑辅助图库
**Alt:** Alcedo Studio 中的色彩科学、RAW 解码、几何、胶片效果与 LUT 工具

### 6.4 图库管理

**Heading:** 图库管理

将照片导入本地图库，浏览文件夹与缩略图，并对大量照片评分和整理，无需上传到云端服务。

可用评分等条件筛选，便于日常选片与管理，而不只是单张精修。

**Screenshot (lazy):** 同英文页图库图  
**Alt:** Alcedo Studio 图库浏览器中的缩略图与文件夹

### 6.5 搜索图库

**Heading:** 搜索图库

按图片内容、相机型号、拍摄日期或组合条件查找照片，不需要准确文件名。

基础的内容理解与标签可在本机完成，用于图库搜索；此路径不需要云端账号。

**Screenshot (lazy):** 同英文页搜索图  
**Alt:** 在 Alcedo Studio 中按内容与元数据搜索图库

### 6.6 AI 描述与评分

**Heading:** AI 描述与评分

连接你使用的 AI 服务，为选定照片生成描述、标签、评分和简短理由。

评分严格程度可以调整。评分写入标准 EXIF 元数据，其他照片软件也能读取。Alcedo Studio 本身不提供自营云端 AI 服务，你使用自己选择的服务。

**Screenshot (lazy):** 同英文页 AI 图  
**Alt:** Alcedo Studio 中显示的 AI 描述与评分结果

### 6.7 性能

**Heading:** 性能

导入和图库预览面向大量照片与批量操作。调整预览约 10 ms 更新，编辑时滑块保持跟手。

导出处理速度快。内存按需分配，控制日常图库与编辑时的资源占用。

**Screenshot (lazy):** 同英文页导出图  
**Alt:** Alcedo Studio 的导出设置界面

### 6.8 页尾操作

**Heading:** 下一步

- Windows 下载
- macOS 下载
- 阅读使用教程
- 返回首页

---

## 7. 图片与资源对照（实现备忘，非用户可见文案）

| 用途 | 源截图 | 生产资源 | loading |
| --- | --- | --- | --- |
| 首页主视觉 | `1-主界面.png` + `3-基础调整.png` 叠放合成 | `assets/alcedo-hero-desktop.avif/webp`、`assets/alcedo-hero-mobile.avif/webp` | high / LCP |
| Features: RAW | `3-基础调整.png`, `2-色彩科学.png`, `2-自然影调1.png`, `2-自然影调2.png`, `5-几何调整.png`, `5-胶片颗粒与Halation模拟.png`, `Portra 400.png` | RAW editing optimized AVIF/WebP set | lazy |
| Features: Library | `1-主界面.png` | features 优化图 | lazy |
| Features: Search | `10-AI自然语言搜索.png` | features 优化图 | lazy |
| Features: AI | `8-AI内容识别.png`（可换评分界面） | features 优化图 | lazy |
| Features: Performance | `6-导出界面.png` | features 优化图 | lazy |
| 社交卡片 | 基于主视觉或品牌构图 | `assets/social-card.png` 1200×630 | n/a |
| Favicon | 现有品牌图标 | `assets/favicon.svg` | n/a |

`8-AI内容识别.png`、`9-AI内容过滤.png` 和旧版 `header.jpg` 暂不放入首版网页。RAW 编辑相关截图
已经作为 RAW editing 区域的辅助图库发布，不单独拆成胶片或 LUT 营销页面。

---

## 8. robots / sitemap 文案位（结构，无营销句）

**robots.txt**

```text
User-agent: *
Allow: /

Sitemap: https://zidage.github.io/AlcedoStudio/sitemap.xml
```

**sitemap.xml 收录 URL（仅规范页）**

1. https://zidage.github.io/AlcedoStudio/
2. https://zidage.github.io/AlcedoStudio/features/
3. https://zidage.github.io/AlcedoStudio/zh-cn/
4. https://zidage.github.io/AlcedoStudio/zh-cn/features/

---

## 9. 审核清单（请逐项确认）

请回复时标明 **通过 / 修改意见**。重点检查：

1. **英文首屏**三句是否准确、是否过短或过长。
2. **中文首屏**“面向摄影师的现代工作站”是否保留，或改回“集 RAW 编辑与图片管理于一体…”。
3. **首页五项摘要**措辞是否与发布版一致（尤其“约 10 ms”“EXIF 可被其他软件读取”）。
4. **Features 五节**是否事实正确；是否要增删胶片模拟、Nikon HE、HDR 等单独章节。
5. **AI 表述**：本地搜索与“连接你使用的 AI 服务”分层是否清楚；是否禁止“不上传照片”类绝对句（本稿未写绝对隐私保证）。
6. **下载**：当前直链 v0.2.7 安装包 + All releases + 中文百度网盘是否接受。
7. **SEO 标题与 description** 长度与关键词是否可接受。
8. **Features 截图选型**是否要换成更新的评分 / 搜索界面图。

### 9.1 已知刻意省略（默认不写）

- 版本号、发布日期、更新日志
- CUDA / Metal / OpenCL 等加速后端名称
- 安装包大小、代码签名状态
- 新闻、捐赠、社区、贡献指南、隐私专题
- 用户评价、统计数字、轮播

---

## 10. Phase 1 完成定义

- [x] 访谈结论已汇总到 redesign plan §14–§16
- [x] 本文件产出完整中英文主页 + Features + SEO + 共享标签 + 404
- [x] **用户审核通过**（2026-07-13；中文 JSON-LD 已按修图/筛图表述修订）
- [x] Phase 2：建立 `docs/alcedo-website/site/` 并写入 HTML/CSS（2026-07-13）
- [x] Phase 3：robots/sitemap、子路径可移植链接、合并 Pages workflow 并部署 `site/`（2026-07-13）
