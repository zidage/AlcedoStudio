# Alcedo Studio 官网重制方案

Date: 2026-07-13

Primary roadmap owner: external `AlcedoStudio-site` repository

Status: Phase 5 in progress — the deployed website now lives in the standalone
[`AlcedoStudio-site`](https://github.com/zidage/AlcedoStudio-site) repository and is
served from `https://aoraw.org` through Cloudflare Workers.

> Historical-path note: the `docs/alcedo-website/` paths below describe the original
> GitHub Pages implementation. That deployment has been retired; project README
> screenshots now live under `docs/screenshots/`.

Public copy (Phase 1 deliverable): [`alcedo_website_public_copy.md`](alcedo_website_public_copy.md)

## 1. 目标

将 `docs/alcedo-website/` 重制为一个简单、快速、可直接阅读的纯静态软件官网。
页面不承担品牌秀或交互动效展示，而是让访问者和搜索引擎立即理解：

- 产品名称是 **Alcedo Studio**。
- 它是一款免费、开源的 RAW 照片编辑器和图片管理软件。
- 它支持 Windows x64 和 Apple Silicon Mac。
- 用户可以立即下载、阅读教程或查看源代码。

首屏必须在不滚动、不播放动画、不等待 JavaScript 的情况下提供上述信息。

## 2. 非目标

- 不做暗色科技风、玻璃拟态、渐变、发光、漂浮背景或滚动显现。
- 不做营销式长文案，不堆叠“专业级”“重新定义”“为创作者而生”等空泛表述。
- 不把 AI 当作视觉主题；AI 只作为一项有具体用途的产品能力。
- 不做轮播图、灯箱、复杂卡片、统计数字、用户评价或没有真实内容的新闻区。
- 不为了关键词密度重复同义句。SEO 依赖清晰主题、真实内容和正确页面结构，而不是关键词堆砌。
- 不在本轮方案阶段直接改写现有网页；实现应在访谈确认内容后进行。

## 3. 当前网站审计

### 3.1 技术现状

- 当前站点使用 Vite 构建，但主体仍是原生 HTML、CSS 和 JavaScript。
- `index.html` 约 26 KB，`style.css` 约 30 KB，`main.js` 约 24 KB。
- 中英文内容放在同一个页面，由客户端 JavaScript 替换文本，没有独立且稳定的语言 URL。
- GitHub Pages 部署已合并为唯一 workflow：`.github/workflows/website.yml`，直接上传
  `docs/alcedo-website/site/`（无 Node/Vite 构建）。
- 资源与语言链接使用相对路径；`scripts/verify_site.py` 禁止根绝对路径，以便同时支持
  `/AlcedoStudio/` 子路径与后续域名根路径。

### 3.2 信息问题

- 首屏描述过长，同时强调 AI、GPU、胶片模拟、编辑和管理，缺少唯一清楚的产品定义。
- “下载”和“教程”不是并列的第一层入口；教程链接目前没有进入主导航和首屏操作。
- Windows 和 macOS 按钮都指向同一个 GitHub Releases 页面，没有明确说明版本、文件名、大小或系统要求。
- 功能按 AI、工具、胶片效果分成大量长卡片，用户需要滚动很久才能建立整体认识。
- 页面语言切换依赖 JavaScript；搜索引擎难以把中文和英文作为两个明确页面分别理解。

### 3.3 视觉问题

- 暗色背景、玻璃导航、渐变按钮、光晕、圆形背景、阴影、胶囊标签和滚动动画同时出现。
- 软件截图外又包一层仿窗口边框，增加装饰但没有增加信息。
- 标题和区块都使用较强的视觉强调，导致没有真正的主次关系。
- 过多卡片和标签把连续说明切碎，不符合成熟开源软件官网的文档式阅读体验。

### 3.4 SEO 缺口

当前页面只有基础 `title` 和 `description`，尚缺：

- canonical URL；
- 中英文 `hreflang`；
- Open Graph 和社交分享信息；
- `SoftwareApplication` 结构化数据；
- `robots.txt` 和 `sitemap.xml`；
- 独立、可索引的英文页面；
- 明确包含产品类别、平台、价格和许可证的可见正文；
- 教程、下载、源代码之间的清楚链接关系。

`meta keywords` 不列入方案。现代搜索引擎不会因为该标签中的关键词而提高排名。

## 4. 参考项目的取舍

| 参考站点 | 借鉴 | 不照搬 |
| --- | --- | --- |
| [Git](https://git-scm.com/) | 用一句话定义产品；把 Install、Learn、Reference、Community 变成直接入口；版本信息具体 | 不复制其插画和品牌造型 |
| [RawTherapee](https://www.rawtherapee.com/) | 首屏直接出现“free, cross-platform raw image processing program”；紧接版本和各平台下载；功能说明具体 | 不使用轮播，也不照搬旧式页面装饰 |
| [Ubuntu](https://ubuntu.com/) | 清楚的模块分隔线、标题、正文和文本链接；下载始终是一等入口 | 不复制其企业产品规模、巨型导航和促销内容 |

设计结论：采用 **Git 的信息密度、RawTherapee 的产品直白程度、Ubuntu 的排版秩序**。

## 5. 已确认定位

访谈已确认，Alcedo Studio 的主身份不是单独的编辑器或管理器，而是一个完整的
**现代摄影师工作站**。RAW 编辑和图片管理是两个并列的核心能力，本地及云端 AI
是建立在工作流之上的扩展能力。

推荐首屏英文表述：

> Alcedo Studio is a modern photography workstation for RAW editing and image
> management.
>
> Free and open-source, with local and cloud AI tools for photographers.

推荐首屏中文表述：

> Alcedo Studio 是一个集 RAW 编辑与图片管理于一体的现代摄影师工作站。
>
> 免费、开源，并支持面向摄影工作流的本地及云端 AI 功能。

“摄影师工作站”负责建立完整产品印象，但不能单独承担 SEO。页面标题、meta description、
功能标题和正文仍需自然包含 `free RAW editor`、`RAW photo editor`、`image manager`
等明确类别词，避免搜索引擎或第一次访问的用户误以为这是硬件工作站。

## 6. 页面与 URL 结构

### 6.1 推荐的第一版范围

```text
docs/alcedo-website/
  site/                      # 唯一可部署静态目录
    index.html               # 英文主页
    features/
      index.html             # 英文详细功能页
    zh-cn/
      index.html             # 简体中文主页
      features/
        index.html           # 简体中文详细功能页
    assets/
      site.css
      alcedo-library.webp
      alcedo-editor.webp
      social-card.png
      favicon.svg
    404.html
    robots.txt
    sitemap.xml
  wrangler.jsonc             # 后续 Cloudflare Workers Static Assets 部署
```

第一版不创建站内教程系统。导航中的 Documentation / 使用教程直接指向：

`https://zidage.github.io/AlcedoStudio_docs/`

已确认第一版创建独立 Features 页面，把具体功能和补充截图移出首页。第一版不创建 About
或 Download 页面：下载仍是首页的直接操作，About 暂时没有独立内容价值。

### 6.2 已确认的语言策略

已确认采用英文根页面 `/AlcedoStudio/`，中文页面 `/AlcedoStudio/zh-cn/`：

- 英文根页直接覆盖 `free raw editor`、`free image editor`、`RAW photo editor`、
  `image manager` 等主要检索意图。
- 中文页拥有自己的标题、描述、可见正文、canonical 和 `hreflang="zh-CN"`。
- 两页互相提供清楚的语言文本链接，不使用客户端翻译，也不自动跳转。
- `hreflang="x-default"` 指向英文根页。

切换到独立域名后，页面内容和语言层级不变，根页面改为 `/`，中文仍使用 `/zh-cn/`。

## 7. 首页信息架构

### 7.1 页头

- 左侧：`Alcedo Studio` 纯文字名称或小型标志。
- 右侧：Features、Download、Documentation、GitHub、中文 / English。
- 非悬浮、非玻璃、非吸顶；普通白底和一条细分隔线。
- 移动端允许导航自然换行，避免为了一个简单菜单引入 JavaScript 汉堡菜单。

### 7.2 首屏

按以下顺序呈现：

1. `h1`: Alcedo Studio
2. 一句话定义为现代摄影师工作站，并明确 RAW 编辑与图片管理能力。
3. 一句不超过约 22 个英文单词的补充说明，交代免费、开源以及本地和云端 AI。
4. 主链接：Download for Windows。
5. 次级文本链接：Download for macOS、Read the documentation、View source。
6. 不显示版本号和发布日期；下载按钮直接指向当前版本。

首屏推荐英文草稿：

> **Alcedo Studio**
>
> A modern photography workstation for RAW editing and image management.
>
> Free and open-source, with local and cloud AI tools for photographers.

中文草稿：

> **Alcedo Studio**
>
> 集 RAW 编辑与图片管理于一体的现代摄影师工作站。
>
> 免费、开源，并支持面向摄影工作流的本地及云端 AI 功能。

### 7.3 主截图

- 首页视觉由两张已有截图组成：主相册界面和编辑器界面前后叠放，同时表达管理与编辑能力。
- 推荐源图使用 `1-主界面.png` 和 `3-基础调整.png`；最终仍需确认两张图都代表当前版本。
- 为保证首屏速度，生产页面不分别请求两张原始 PNG。设计阶段将它们合成为一个响应式主视觉，
  导出桌面和移动尺寸的 AVIF/WebP；视觉上仍保持两张窗口叠放关系。
- 不套仿窗口框，不遮罩，不轮播，不自动切换。
- 图片提供准确 `alt`、固定宽高、响应式尺寸和 WebP/AVIF 优化版本。
- 其余截图只在独立 Features 页面中按需延迟加载。

### 7.4 What it does / 首页功能摘要

首页不展开功能细节。使用一个普通列表，每项只保留短标题和一句话，并提供一个统一的
`Explore features` / `查看详细功能` 链接。摘要使用用户能直接理解的名称：

1. **RAW editing / RAW 编辑**：调整曝光、色彩、影调和构图并导出照片。
2. **Photo library / 图库管理**：导入、浏览、筛选、评分和整理照片。
3. **Library search / 搜索图库**：按图片内容、相机型号、拍摄日期或组合条件查找照片，
   不需要准确文件名。
4. **AI descriptions and ratings / AI 描述与评分**：连接用户自己的 AI 服务，生成描述、
   标签、评分和理由；评分严格程度可调，其他软件也能读取评分。
5. **Performance / 性能**：快速导入和图库预览，支持批量管理；调整预览约 10 ms 更新，
   导出速度快；内存按需分配，控制资源占用。

首页每项不超过一句话、不配单独截图，也不把整行做成卡片。正文只写能由当前发布版验证的能力。

#### 详细 Features 页面

英文 URL：`/AlcedoStudio/features/`；中文 URL：`/AlcedoStudio/zh-cn/features/`。
迁移到独立域名后分别为 `/features/` 和 `/zh-cn/features/`。

该页面负责：

- 分别解释 RAW 编辑、图库管理、综合搜索、AI 描述与评分，以及导入、预览、导出和内存性能。
- 每项使用一个具体标题、2—3 句事实说明和最多一张相关截图。
- 提供返回首页、下载和教程的直接链接。
- 承担 `non-destructive RAW editing`、`local AI image search`、`EXIF-compatible AI rating`
  等更具体的长尾搜索内容，避免在首页堆词。
- 所有非首屏截图使用优化格式、`srcset`、固定尺寸和 `loading="lazy"`。

首页与 Features 页不复制长段落：主页只给摘要，详细页给完整事实。

#### AI 信息分层

首页只说明两类用户结果：

- 本地 AI：在本机理解和搜索图库，不需要连接云端模型。
- 云端 AI：连接用户选择的 AI 服务，对照片进行进一步分析。

首页和 Features 页面都不展开认证和协议名称。只有教程中的设置说明列出连接方式：

- OpenAI OAuth；
- OpenAI-compatible 消息接口；
- Anthropic-compatible 消息接口；
- CC Switch 路由；
- 用户自定义服务地址、模型和普通 API Key。

推荐的简短对外措辞是 `Connect your preferred AI service` / `连接你使用的 AI 服务`。
这比“支持多种云端 AI 能力”更直接，也不会把 Alcedo 错误描述为自营云服务。

已确认的云端 AI 正文草稿：

> 连接你使用的 AI 服务，为照片生成描述和标签，并按你设定的严格程度评分。
> 评分写入标准 EXIF 元数据，其他照片软件也能读取。

英文草稿：

> Connect your preferred AI service to describe, tag, and rate photos. Adjust how
> strict the review is; ratings are written to standard EXIF metadata for other
> photo software to read.

内部实现仍需确认远端请求实际发送的数据，但该内容属于设置教程或必要的授权提示，不进入首页
或 Features 页营销文案。

### 7.5 下载

用简单列表或表格代替大型 CTA 卡片：

| 平台 | 需要显示的信息 | 主动作 |
| --- | --- | --- |
| Windows | Windows 10/11、x64 | Download for Windows |
| macOS | Apple Silicon | Download for macOS |
| Source | Source code | View source |

建议下载链接直接指向当前安装包，并提供 `All releases` 作为稳定后备链接。如果发布资产文件名不稳定，
则先指向 GitHub `releases/latest`，不要伪装成平台直链。

中文页另提供一个文字为 `百度网盘` 的次级链接，不添加线路或速度说明。

### 7.6 文档与开源

- 一个短区块说明教程包含安装、导入、编辑和导出。
- 明确文本链接：`Read the documentation` / `阅读使用教程`。
- 同区提供 GitHub repository、issue tracker 和 GPL-3.0-only license。
- 如果没有活跃社区入口，不创建 Community 导航项。

### 7.7 页脚

只保留产品名、Documentation、GitHub、License、语言切换和版权信息。
不做多列链接农场，不重复整页导航。

## 8. 视觉规范

### 8.1 基础风格

- 页面底色：`#ffffff`；可选的次级区块底色：`#f7f6f3`。
- 主文字：`#1f2328`；次级文字：`#59636e`。
- 分隔线：`#d8dee4` 或更浅的同系灰色。
- 唯一强调色：从 Alcedo 现有蓝色中选一个低饱和深蓝，主要用于链接和主下载按钮。
- 不使用渐变、大面积深色背景、发光、玻璃、纹理或重阴影。

### 8.2 字体与阅读宽度

- 使用系统字体栈，不依赖 Google Fonts，减少请求和字体布局偏移。
- 英文建议：`system-ui`, `-apple-system`, `BlinkMacSystemFont`, `"Segoe UI"`, sans-serif。
- 中文补充：`"PingFang SC"`, `"Microsoft YaHei"`, sans-serif。
- 正文桌面端 17—18 px，行高 1.6；正文段落宽度不超过约 68 个英文字符。
- `h1` 桌面端约 48—56 px，移动端约 38—44 px；不使用超大展示字。
- 标题全部左对齐，使用 sentence case，不使用装饰性全大写标签。

### 8.3 布局与组件

- 主容器最大宽度约 1080 px；正文栏约 720 px。
- 区块依靠留白和水平分隔线建立秩序，不依靠卡片背景。
- 按钮接近矩形，圆角 4—6 px，无阴影；文本链接保持明显下划线或稳定的链接色。
- 不使用功能图标。对该站点而言，准确标题比通用图标更清楚。
- 所有功能在键盘操作和系统深色/高对比设置下保持可辨认。
- 尊重 `prefers-reduced-motion`；第一版建议完全没有页面动画。

### 8.4 首页性能预算

- 不加载第三方字体、分析脚本、图标库或客户端应用框架。
- 首页生产环境不加载 JavaScript。
- 首次打开只请求 HTML、一个 CSS 文件、favicon 和一张响应式主截图。
- HTML 与 CSS 的合计 Brotli/Gzip 传输体积以 35 KB 以下为目标。
- 主截图提供至少桌面和移动两个尺寸；桌面版本以 300 KB 以下、移动版本以 160 KB 以下为目标，
  在清晰度允许时优先 AVIF，并提供 WebP 后备。
- 主截图声明 `width`、`height`、`srcset`、`sizes` 和 `fetchpriority="high"`，避免布局跳动并优先完成 LCP。
- 首页不预加载 Features 页图片；详细页的截图全部懒加载。
- 在 GitHub Pages 和 Cloudflare Workers 两个目标环境分别检查请求数量、传输体积、LCP 和缓存头。

## 9. 文案规则

### 9.1 语气

- **已确认：所有措辞必须清晰、简单、一目了然。** 普通摄影用户第一次阅读就应知道
  这句话对应什么实际用途，不需要理解产品内部架构或营销概念。
- **已确认：实现前先单独提交完整的中英文公开文案，由用户审核通过后再写入网页。**
- 英文使用冷静的说明语气：优先使用客观陈述，减少命令式动词、第二人称和情绪性形容词。
- 先说类别和事实，再说差异。
- 每段只表达一个意思，首屏段落最多两句。
- 用 `free and open-source`、`RAW photo editor`、`image manager` 等用户实际搜索的词，
  但每个核心词在自然语境中出现即可。
- 避免“AI 驱动的下一代专业创作体验”之类无法帮助用户判断用途的句子。
- 不单独使用“摄影工作流”“智能能力”“AI 赋能”“云端能力”等抽象词；必须紧接具体动作，
  例如“用一句话搜索本地照片”或“调用云端模型检查失焦和构图问题”。
- 不使用 `privacy-first`、`core pillars`、`connected AI analysis` 或类似包装词。
- 不把内部实施决策写进公开文案。SEO、R2、Cloudflare、缓存、国内线路和镜像策略只留在
  工程方案中，除非用户完成某个动作时确实需要知道。
- 下载入口只写动作名称：`Download for Windows`、`Download for macOS`、
  `Windows 下载`、`macOS 下载`、`百度网盘`。不添加“高速”“推荐线路”“国内加速”等修饰。
- 用户点击按钮就能完成的事，不在按钮旁解释后台如何托管、路由或分发。
- 避免未经测试的绝对词，如 `zero latency`、`best-in-class`、`all RAW formats`。

### 9.2 建议的英文主题词

主页应自然覆盖：

- free RAW editor
- open-source RAW photo editor
- free image editor
- image manager / photo manager
- Windows RAW editor
- macOS RAW editor
- non-destructive photo editing
- local AI image search

不为每个词创建重复段落。可在首屏、功能标题、下载区和页面标题中分散使用。

### 9.3 内容真实性

所有公开陈述都要从当前稳定发布版核对，尤其是：

- 当前版本号和发布日期；
- Windows / macOS 最低系统要求；
- CUDA、OpenCL 和 Metal 支持范围；
- AI 功能是否默认可用、是否需要下载模型或配置 API；
- “完全本地”和“不上传照片”的准确适用范围；
- 支持的导出格式、RAW 格式和相机型号；
- 安装包是否签名、公证以及可能出现的系统警告。

## 10. SEO 与分享实现

每种语言页面应包含：

- 唯一的 `<title>` 和 `meta description`；
- 自引用 canonical；
- `en`、`zh-CN` 和 `x-default` 的 `hreflang`；
- `og:title`、`og:description`、`og:type`、`og:url`、`og:image`；
- Twitter/X summary card；
- 一个 `SoftwareApplication` JSON-LD 对象，包含：
  `name`、`description`、`applicationCategory`、`operatingSystem`、`license`、
  `downloadUrl` 和价格为 0 的 `offers`；
- 一个且仅一个 `h1`，后续标题层级连续；
- 语义化的 `header`、`nav`、`main`、`section` 和 `footer`；
- 站内语言链接和指向文档、下载、许可证、源代码的描述性锚文本。

全站应包含：

- `robots.txt`，允许抓取并指向 sitemap；
- `sitemap.xml`，只收录规范 URL；
- 品牌 favicon 和 1200 × 630 社交分享图；
- 有意义的图片文件名、alt、width、height、`srcset` 和懒加载；
- 正确的 404 页面；
- Google Search Console / Bing Webmaster Tools 的站点提交与索引检查步骤。

结构化数据中的版本、价格、平台和下载地址必须与可见页面一致。

## 11. 静态实现方案

### 11.1 推荐

- 两份直接可发布的 HTML 文件，共享一个手写 CSS 文件。
- 核心页面不加载 JavaScript。
- 不使用客户端语言字典，不通过 GitHub API 在浏览器中动态获取版本。
- 下载版本更新时，在发布流程中同步更新 HTML；如果无法保证同步，就只显示
  `Latest release` 链接，不展示可能过期的版本号。
- GitHub Pages workflow 直接上传静态目录，移除 Vite、Puppeteer 和 Node 安装步骤。
- 合并两份重复 workflow，只保留一个部署入口。

这仍然是“纯 HTML 网站”：HTML 和 CSS 是生产源码，不依赖框架或客户端渲染。

### 11.2 已确认的托管策略

同一份 `docs/alcedo-website/site/` 静态产物需要同时满足两个环境：

1. 当前发布到 GitHub Pages 项目地址 `https://zidage.github.io/AlcedoStudio/`。
2. 后续发布到带独立域名的 Cloudflare Workers。

为保证可迁移性：

- HTML 和 CSS 资源使用相对路径，不硬编码 `/AlcedoStudio/` 资源前缀，也不假设站点一定部署在域名根目录。
- canonical、hreflang、Open Graph URL 和 sitemap 使用绝对公开 URL；更换正式域名时集中更新这些 SEO URL。
- 不使用 GitHub Pages 专属模板、Jekyll 变量或运行时能力。
- GitHub Actions 只负责校验并上传 `site/`，不生成另一份结构不同的生产目录。
- Cloudflare 使用当前推荐的
  [Workers Static Assets](https://developers.cloudflare.com/workers/static-assets/)，不采用已经弃用的 Workers Sites。
- 第一版 Cloudflare 配置只声明 `assets.directory` 和 `not_found_handling: "404-page"`；
  没有动态逻辑时不添加 Worker `main` 脚本。
- 生产域名通过 Workers
  [Custom Domain](https://developers.cloudflare.com/workers/configuration/routing/custom-domains/)
  绑定；Cloudflare 负责对应 DNS 记录和证书。
- 根域名和 `www` 只能有一个 canonical，另一个必须永久跳转到它，不能同时提供两个可索引版本。

建议的初始 `wrangler.jsonc` 形态：

```jsonc
{
  "$schema": "./node_modules/wrangler/config-schema.json",
  "name": "alcedo-studio-site",
  "compatibility_date": "2026-07-13",
  "assets": {
    "directory": "./site",
    "not_found_handling": "404-page"
  }
}
```

域名确定前不把占位域名写进生产元数据。GitHub Pages 版本先使用当前公开地址作为 canonical。
购买域名后，按一次受控发布同时更新 canonical、hreflang、Open Graph、sitemap、Search Console
和 Bing Webmaster Tools，避免搜索引擎看到混合域名。

如果希望最大限度减少换站时的索引迁移，可以先把新域名绑定到 GitHub Pages，再在保持页面路径
不变的情况下把 DNS 和托管切换到 Workers。GitHub 建议在添加自定义域名前先完成域名验证，
以降低域名接管风险。

### 11.3 R2 图片与安装包存储

已确认未来可以使用 [Cloudflare R2](https://developers.cloudflare.com/r2/) 作为类似 OSS 的
公共对象存储，但按资源重要性分工：

- `www.<domain>`：Workers Static Assets 承载 HTML、CSS、favicon 和首页合成主视觉。
- `static.<domain>`：R2 公共 bucket 承载 Features 页补充截图、版本安装包、校验文件和签名。
- GitHub Releases：保留为版本记录和下载后备，不作为页面唯一可用来源。

首页合成主视觉继续跟随站点静态产物发布，因为它是 LCP 关键资源。把它放到另一个主机名会增加
首次 DNS/TLS 连接，并且不能显著节省当前很小的网站体积。非首屏图片和安装包更适合 R2。

生产 R2 访问必须使用自有子域名。Cloudflare 明确把 `r2.dev` 定位为会限流的开发地址；
R2 自定义域名才能使用 Cloudflare Cache、WAF 和访问控制。

推荐使用一个 bucket 和稳定前缀：

```text
images/features/<content-hash>/...
releases/v<version>/AlcedoStudio-<version>-windows-x64.exe
releases/v<version>/AlcedoStudio-<version>-macos-arm64.dmg
releases/v<version>/SHA256SUMS.txt
```

- 文件名必须包含版本和平台，发布后不覆盖同一个 key。
- 版本化图片和安装包使用长期不可变缓存；更新内容时产生新 URL。
- 安装包设置正确的 `Content-Type`、`Content-Disposition: attachment`、`Cache-Control` 和文件名。
- 每个安装包提供 SHA-256；如项目已有代码签名或 detached signature，也在同一版本目录发布。
- 主页下载链接指向固定的 `releases/latest/` 对象，同时提供 `All releases on GitHub` 后备入口；
  每个版本仍保留不可变的归档 URL。
- 不在浏览器中调用 R2 API，也不在网站源码中存放 R2 凭据；bucket 只暴露明确的公共下载对象。

#### GitHub Release 自动同步

安装包不手工上传 R2。新增一个独立 GitHub Actions workflow，在稳定版 Release 发布时自动同步：

1. 监听 `release` 的 `published` 事件，并保留 `workflow_dispatch` 手动重跑入口。
2. 读取 `github.event.release.tag_name`，使用 GitHub CLI 下载该 Release 的附件。
3. 只接受明确的安装包和校验文件扩展名；不上传源码归档或未知附件。
4. 在上传前确认 Windows x64 和 macOS Apple Silicon 安装包都存在，否则整次同步失败。
5. 生成 `SHA256SUMS.txt`，先上传到不可变的 `releases/<tag>/` 目录。
6. 版本目录全部成功后，再把两个安装包和校验文件复制到固定的 `releases/latest/` key。
7. `releases/<tag>/` 使用长期 immutable 缓存；`releases/latest/` 使用短缓存或重新验证，
   防止下载按钮继续拿到旧版本。
8. 预发布版本可以存入版本目录，但默认不更新 `latest`。

同步使用 R2 的 S3-compatible API 和 AWS CLI/rclone。Wrangler 只允许一次上传一个对象，且单文件
上传限制较低，不适合作为安装包批量同步工具。

所需 GitHub Secrets 只授予目标 bucket 的对象读写权限：

- `R2_ACCOUNT_ID`
- `R2_ACCESS_KEY_ID`
- `R2_SECRET_ACCESS_KEY`
- `R2_BUCKET`

网站部署和 Release 同步保持两个独立 workflow。普通网站文案或样式更新不会重复下载、上传安装包；
发布稳定版时也不需要手工部署网站，固定的 `latest` URL 会继续供下载按钮使用。

截至 2026-07，R2 Standard 免费额度包含每月 10 GB-month 存储、100 万次 Class A、
1000 万次 Class B 操作，并且直接从 R2 到公网的 egress 不收费。该项目当前体量预计可落在免费额度内，
但上线前和每次大版本发布前仍应复核最新价格。

#### 中国大陆访问边界

普通 Cloudflare/R2 地址可能从中国大陆访问，但不能据此承诺“国内高速”或稳定 SLA。
Cloudflare 官方的中国大陆节点网络是 Enterprise 的独立订阅，并要求域名具备有效 ICP 备案或许可证。
已确认不建立中国大陆多地区、多运营商测速流程。中文页采用低维护成本的固定后备方案：

- 主下载仍指向正式版本下载源，并保留 GitHub Releases。
- 中文页长期提供现有百度网盘链接作为次级分流。
- 百度网盘不出现在英文页，也不作为首屏最醒目的主按钮。
- 官网只写客观的下载来源，不写未经持续监测证明的“中国高速下载”。

### 11.4 可选保留

如果项目希望保留 Vite 作为本地预览工具，可以继续使用，但生产页面不能依赖 Vite 特有的
JavaScript 行为，并需要显式配置多页面构建和 GitHub Pages 子路径。该方案复杂度更高，
不作为首选。

## 12. 实施顺序

### Phase 1 — 内容冻结

- [x] 完成 `grill-me` 访谈。
- [x] 确认主定位、默认语言、功能范围、下载来源、版本信息和系统要求（见 §14–§16）。
- [x] 产出完整的中英文公开文案，不先做样式：[`alcedo_website_public_copy.md`](alcedo_website_public_copy.md)。
- [x] 用户审核通过公开文案（2026-07-13）；可进入 Phase 2 网页实现。

### Phase 2 — 结构重建

- [x] 建立英文和中文主页及对应的 Features 页面（`docs/alcedo-website/site/`）。
- [x] 首页实现页头、首屏、双截图叠放合成主视觉、功能摘要、下载、文档和页脚。
- [x] Features 页面实现五项具体能力及对应跳转。
- [x] 使用相册与编辑器截图制作首页合成主视觉，并优化 Features 页面需要的补充截图（`scripts/build_assets.py`）。
- [x] 删除旧的装饰性结构和客户端翻译逻辑（Vite / `main.js` 中英切换已移除）。

### Phase 3 — SEO 与部署

- [x] 增加 canonical、hreflang、Open Graph、JSON-LD、robots 和 sitemap（`site/robots.txt`、`site/sitemap.xml`；各页 head 已含元数据）。
- [x] 修正 `/AlcedoStudio/` 子路径下的所有资源和语言链接（相对路径 + `verify_site.py` 禁止根绝对路径；404 使用公开绝对 URL）。
- [x] 合并重复 GitHub Pages workflow，并按最终方案简化构建（仅保留 `.github/workflows/website.yml`；无 Node/Vite）。
- [x] 将唯一的 `site/` 静态目录作为 Pages artifact 发布（推送 `main` 后由 Actions 校验并部署；线上结果在合并后确认）。

### Phase 4 — 验证

- 桌面和手机尺寸下检查首屏、导航、长文本和下载区。
- 禁用 JavaScript后验证全部核心内容与链接。
- 检查 HTML 语义、键盘焦点、颜色对比和替代文本。
- 验证 Windows、macOS、教程、GitHub、许可证和语言链接。
- 运行 Lighthouse，并人工检查生成后的 HTML 和 GitHub Pages 部署结果。
- 提交 sitemap 后检查搜索引擎是否分别识别中英文规范页面。

### Phase 5 — Cloudflare Workers 与独立域名

- 购买并验证域名，确定根域名或 `www` 中哪个是唯一规范主机名。
- 将域名加入 Cloudflare zone，通过 Workers Custom Domain 绑定站点。
- 用 Wrangler 把同一个 `site/` 目录部署为 Workers Static Assets。
- 建立 R2 公共资源 bucket，通过自有静态资源子域名提供详细截图、安装包和校验文件。
- 为版本化 R2 对象设置内容类型、下载文件名、长期缓存和 SHA-256 校验。
- 添加 Release `published` → R2 的自动同步 workflow，并验证缺少任一平台安装包时不会更新 `latest`。
- 配置非规范主机名到规范主机名的永久跳转。
- 更新全部绝对 SEO URL，并验证证书、404、缓存头、robots 和 sitemap。
- 保持 `/` 与 `/zh-cn/` 路径不变，在搜索站长工具中提交新域名并观察索引迁移。

## 13. 验收标准

- 用户打开页面后无需滚动即可看到产品名称、产品类别、平台、下载和教程入口。
- 首页只加载一张由相册界面与编辑器界面合成的响应式主视觉；具体功能说明和补充截图位于
  独立 Features 页面。
- 页面在 JavaScript 禁用时完整可用。
- 首屏没有渐变、光晕、玻璃、轮播、动效、胶囊标签或仿窗口装饰。
- 正文没有超过三句的营销段落；所有功能描述可由发布版验证。
- 英文和中文拥有独立 URL、独立元数据、canonical 和互相对应的 hreflang。
- 下载和文档链接没有中间滚动步骤或模糊按钮文字。
- 移动端 360 px 宽度无横向滚动，导航和下载链接可直接操作。
- Lighthouse Performance、Accessibility、Best Practices 和 SEO 以 95+ 为目标；
  如未达到，记录具体的第三方或托管限制。
- 首页 HTML 与 CSS 压缩传输合计目标低于 35 KB，桌面主截图目标低于 300 KB，
  移动主截图目标低于 160 KB；超出时必须记录清晰度或格式兼容性原因。
- HTML 校验无严重错误，所有生产链接通过自动检查。
- 同一份 `site/` 产物可在 GitHub Pages `/AlcedoStudio/` 子路径和 Cloudflare Workers
  独立域名根路径下工作，不维护两套 HTML。

## 14. 待访谈确认

以下问题会按影响顺序逐项确认，并将答案更新到本文档：

1. **已确认：** Alcedo 是一个现代摄影师工作站，RAW 编辑和图片管理并重，支持本地及云端 AI。
   页面措辞必须清晰、简单、一目了然。
2. **已确认：** 默认根页面使用英文，简体中文使用独立 `/zh-cn/` 路径，不自动跳转。
3. AI 搜索是核心卖点、普通功能，还是暂时不应在首屏出现？
   **已确认的连接范围：** OpenAI OAuth、OpenAI-compatible、Anthropic-compatible、
   CC Switch 路由，以及使用普通 API Key 的自定义配置。
   **已确认的用户结果：** 生成照片描述和标签；按可调严格程度生成评分与理由；
   评分写入标准 EXIF 元数据，可被其他照片软件读取。
   **已确认的首页优先级：** AI 作为普通功能摘要出现，不使用 AI 徽章或单独占据首屏。
4. 第一版必须展示哪些已稳定功能？哪些仍属实验性或不应宣传？
   **已确认的信息架构：** 首页只提供五项能力的一句话总结和一个 Features 跳转；
   具体说明及补充截图放到独立的中英文 Features 页面。
5. Windows 和 macOS 的准确最低要求、安装包格式、大小和签名状态是什么？
   **已确认的首页范围：** 只显示 Windows 10/11 x64 和 macOS Apple Silicon；
   其他硬件与加速细节不放在下载按钮旁。
6. 下载应直达安装包、GitHub Releases，还是同时保留百度网盘？
7. 首屏使用图库截图、编辑器截图，还是一张能兼顾两者的新截图？
   **已确认：** 使用现有主相册与编辑器截图前后叠放，并导出为一张优化后的响应式主视觉。
8. **已确认：** 首页不展示版本号、发布日期、更新日志或旧版本；下载按钮直达当前版本。
9. **已确认：** 第一版不做新闻、捐赠、社区、贡献指南、宣传式隐私区或致谢区。
   页脚只保留 Documentation、GitHub 和 License。
10. **已确认：** 用户本人审核并确认最终中英文公开文案。
11. **已确认：** 当前先部署 GitHub Pages，后续迁移至 Cloudflare Workers Static Assets
    并使用购买的独立域名。仍需在域名确定后选择根域名或 `www` 作为唯一 canonical。
12. **已确认：** 后续使用 Cloudflare R2 存放 Features 页图片、版本安装包和校验文件；
    首页关键合成图仍跟随站点静态产物。不做中国大陆多运营商实测；中文页长期保留百度网盘
    作为次级分流，英文页不显示。
13. **已确认：** 公开文案只说明用户能做什么，不出现内部技术和品牌包装词；实现前必须先提交
    完整中英文文案供用户审核。
14. **已确认：** R2 安装包由 GitHub Release `published` workflow 自动同步，不手工上传；
    稳定版成功同步后更新固定的 `latest` 下载路径，网站部署不重复搬运安装包。

## 15. 参考资料

- Git homepage: <https://git-scm.com/>
- Ubuntu homepage: <https://ubuntu.com/>
- RawTherapee homepage: <https://www.rawtherapee.com/>
- Alcedo Studio documentation: <https://zidage.github.io/AlcedoStudio_docs/>
- Alcedo Studio repository: <https://github.com/zidage/AlcedoStudio>
- Cloudflare Workers Static Assets: <https://developers.cloudflare.com/workers/static-assets/>
- Cloudflare Workers Custom Domains: <https://developers.cloudflare.com/workers/configuration/routing/custom-domains/>
- GitHub Pages custom domains: <https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/about-custom-domains-and-github-pages>
- Cloudflare R2 public buckets: <https://developers.cloudflare.com/r2/buckets/public-buckets/>
- Cloudflare R2 pricing: <https://developers.cloudflare.com/r2/pricing/>
- Cloudflare R2 upload metadata: <https://developers.cloudflare.com/r2/objects/upload-objects/>
- Cloudflare China Network: <https://developers.cloudflare.com/china-network/>

## 16. 已确认的首页文案

以下文案已通过访谈确认。实现时可以调整换行，但不能自行增加宣传词或技术说明。

### 16.1 中文

#### 首屏

**Alcedo Studio**

面向摄影师的现代工作站。

编辑 RAW 照片、管理图库、搜索照片，也可以连接 AI 服务生成描述和评分。

免费开源。支持 Windows 10/11 x64 和 Apple Silicon Mac。

操作：`Windows 下载`、`macOS 下载`、`使用教程`

#### 功能摘要

**RAW 编辑**

调整曝光、色彩、影调和构图，然后导出照片。

**图库管理**

导入、浏览、筛选、评分和整理照片。

**搜索图库**

按图片内容、相机型号、拍摄日期或组合条件查找照片，不需要准确文件名。

**AI 描述与评分**

连接你使用的 AI 服务，为照片生成描述、标签、评分和理由。评分标准可以调整，结果可以被
其他照片软件读取。

**性能**

快速导入和预览图库，并支持批量管理。调整预览约 10 ms 更新，导出速度快。内存按需分配，
控制资源占用。

操作：`查看详细功能`

#### 下载与教程

下载：`Windows 下载`、`macOS 下载`、`百度网盘`

教程：安装和使用说明。`阅读使用教程`

页脚：`GitHub`、`使用教程`、`GPL-3.0`

### 16.2 English

#### Hero

**Alcedo Studio**

An open-source photography workstation for RAW editing and library management.

Local search and optional AI-assisted descriptions and ratings are included.

Available for Windows 10/11 x64 and Apple Silicon Mac.

Actions: `Download for Windows`, `Download for macOS`, `Documentation`

#### Feature summary

**RAW editing**

Adjust exposure, color, tone, and crop, then export photos.

**Photo library**

Import, browse, filter, rate, and organize photos.

**Library search**

Search by image content, camera model, shooting date, or combined criteria. Exact filenames are not
required.

**AI descriptions and ratings**

Connect an AI service to generate descriptions, tags, ratings, and reasons. Rating strictness is
adjustable, and ratings can be read by other photo software.

**Performance**

Imports and library previews are fast, with batch tools for large photo sets. Adjustment previews
update in about 10 ms, and exports are processed quickly. Memory is allocated on demand to limit
resource use.

Action: `View all features`

#### Download and documentation

Download: `Download for Windows`, `Download for macOS`

Documentation: Installation and user guide. `Read the documentation`

Footer: `GitHub`, `Documentation`, `GPL-3.0`

---

## 17. Phase 1 文案交付

完整可审核文案（主页、Features、SEO、导航、页脚、404、下载 URL 约定、截图对照）见：

**[`alcedo_website_public_copy.md`](alcedo_website_public_copy.md)**

- 首页中英文正文以 §16 为准，并已写入该文件。
- Features 详细中英文、各页 `<title>` / `meta description` / JSON-LD 与共享标签以该文件为准。
- 用户已审核通过。Phase 2–3 已完成：`docs/alcedo-website/site/` 为唯一部署根；SEO 与 Pages workflow 已就绪。下一步：Phase 4 验证。

---

## 18. 详细视觉风格规范

本节是实现阶段的视觉基准。如果前文的概括性描述与本节冲突，以本节为准。

### 18.1 整体气质

网站应像一个维护成熟的软件项目主页：直接、安静、容易读，打开后立即看到软件名称、用途、
下载入口和文档入口。它不是品牌发布会、设计作品集或 SaaS 营销页。

参考项目只取各自最适合 Alcedo Studio 的部分：

- 取 Git 官网的信息直接性和清晰下载入口；
- 取 RawTherapee 官网对软件、平台和文档的明确表达；
- 取 Ubuntu 官网稳定的内容层级，但不采用其大型营销模块和装饰性视觉；
- 页面最终更接近排版良好的软件说明页，而不是带大量卡片和动效的产品落地页。

### 18.2 必须遵守的视觉原则

- 全站使用白色或接近白色的背景，正文左对齐。
- 只使用一种低饱和蓝色作为链接和主要按钮颜色。
- 层级主要依靠字号、字重、留白和细分隔线建立。
- 首页不放功能图标；功能名称本身已经足够清楚。
- 不使用渐变、玻璃拟态、发光、噪点、纹理、插画、图库照片或 AI 生成图片。
- 不使用滚动显现、视差、漂浮、缩放、轮播、自动播放或截图灯箱。
- 不使用胶囊标签、版本徽章、悬浮卡片或伪造的浏览器窗口外壳。
- 不使用三个等宽卡片横排的常见 SaaS 布局；功能摘要使用纵向文本行。
- 截图保持软件原有界面颜色，不给截图套品牌色滤镜。

即使设计技能通常允许轻微渐变、纹理或淡入动画，本项目也明确关闭这些效果，因为用户要求
网页简单、读取快，并且不需要视觉表演。

### 18.3 色彩

| 用途 | 色值 | 使用规则 |
| --- | --- | --- |
| 页面背景 | `#FFFFFF` | 主页、Features 主体 |
| 次级背景 | `#F7F7F5` | 下载区、页脚或少量分区，不交替铺满每一节 |
| 主文字 | `#202124` | 标题和正文 |
| 次要文字 | `#5F6368` | 平台说明、辅助信息 |
| 更弱文字 | `#80868B` | 日期、图片说明等非关键内容 |
| 分隔线 | `#E2E4E7` | 导航底线、功能行和页脚分隔 |
| 品牌蓝 | `#426F8F` | 链接、主要下载按钮、`Alcedo` 字样 |
| 蓝色悬停 | `#315873` | 链接和主要按钮悬停态 |
| 按钮文字 | `#FFFFFF` | 只用于蓝色主要按钮 |
| 键盘焦点 | `#0B6EA8` | 清晰可见的焦点轮廓 |

`Alcedo` 使用品牌蓝，`Studio` 使用主文字色。不增加金色或第二强调色。正文中的普通链接使用
下划线，不能只靠颜色让用户识别。

### 18.4 字体与排版

不下载 Web Font，使用系统字体以减少请求并保证中英文都能快速、稳定显示：

```css
font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI",
  "PingFang SC", "Microsoft YaHei", sans-serif;
```

只使用 `400`、`500`、`600` 三种字重，不使用极细体或超粗体。

| 内容 | 桌面 | 手机 | 规则 |
| --- | --- | --- | --- |
| 首页 H1 | `56px / 1.05 / 600` | `40px / 1.08 / 600` | 最多 2–3 行，字距 `-0.025em` |
| Hero 说明 | `24px / 1.40 / 400` | `21px / 1.45 / 400` | 最宽 `32ch` |
| H2 | `34px / 1.20 / 600` | `28px / 1.25 / 600` | 使用普通句式，不全大写 |
| H3 | `21px / 1.35 / 600` | `19px / 1.35 / 600` | 功能名或平台名 |
| 正文 | `17px / 1.65 / 400` | `16px / 1.65 / 400` | 单段最宽 `66ch` |
| 导航、按钮 | `15px / 1.20 / 500` | 相同 | 不使用大写字母增加强调 |
| 图片说明 | `14px / 1.50 / 400` | `13px / 1.50 / 400` | 只在确实需要解释时出现 |

数字性能数据使用等宽数字特性 `font-variant-numeric: tabular-nums`。标题可使用
`text-wrap: balance`，正文可使用 `text-wrap: pretty`，但不依赖它们维持布局。

### 18.5 页面网格与留白

- 主内容最大宽度：`1180px`。
- 长正文最大宽度：`680px`。
- 桌面左右页边距：`32px`；平板：`24px`；手机：`20px`。
- 桌面使用 12 列网格，列间距 `24px`。
- 大区块上下间距：桌面 `96px`，手机 `64px`。
- 区块标题与正文间距：`20px`；正文段落间距：`12px`。
- 主要断点：`760px` 和 `1080px`。不为每种设备增加零散断点。
- 页面不强求满屏高度；内容有多高就显示多高。

### 18.6 页头

- 桌面高度 `64px`，白色背景，底部一条 `1px` 分隔线。
- 页头不吸顶、不透明模糊，不随滚动缩小。
- 左侧为文字标识 `Alcedo Studio`，`20px / 600`，不用单独制作大型 Logo 图。
- 右侧依次为 `Features`、`Download`、`Documentation`、`GitHub` 和语言切换。
- 导航项间距 `24px`。当前页面只用短下划线表示，不增加背景块。
- 手机端允许自然换行成两行：第一行为名称，第二行为主要链接；不用汉堡菜单和 JavaScript。
- 手机端页头总高度约 `96px`，仅保留 `Features`、`Download`、`Docs` 和语言切换。

### 18.7 首页 Hero

桌面端采用不对称两栏：左侧文字占 5 列，右侧截图占 7 列。Hero 顶部留白 `80px`，底部
留白 `96px`。软件名称直接作为 H1，不放“开源”“新版本”等前置徽章。

左栏固定顺序：

1. `Alcedo Studio`；
2. 一句产品定义；
3. 一句本地搜索与可选 AI 能力；
4. 支持平台；
5. 下载和文档操作。

操作区中，Windows 下载是蓝色主要按钮；macOS 下载是白底细边框按钮；Documentation 是普通
下划线文字链接。按钮高度 `44px`，左右内边距 `18px`，圆角 `4px`，无图标、无阴影。

手机端顺序不变：文字 → 两个下载按钮 → 文档链接 → 合成截图。两个下载按钮占整行，不横向
挤压。首屏不为了强行露出全部截图而缩小正文。

### 18.8 首页双截图构图

首页只加载一张已经合成好的响应式图片，而不是在浏览器里用两个大 PNG 实时叠放。源素材：

- 后层：`docs/alcedo-website/public/screenshots/1-主界面.png`；
- 前层：`docs/alcedo-website/public/screenshots/3-基础调整.png`。

桌面合成画布比例约 `1.52:1`。主相册界面从左上开始，占画布宽度约 `82%`；编辑器界面从
右下覆盖，占画布宽度约 `76%`；两张图的有效重叠约 `28%`。不旋转、不做透视、不增加彩色
背景板。边缘使用非常轻的内描边，整组合图最多使用一层
`0 12px 32px rgba(20, 28, 35, 0.10)` 阴影，圆角 `6px`。

手机端单独导出纵向构图：主相册在上、编辑器在下，只重叠约 `10%`，确保两张图的界面内容
仍能辨认。不能简单把桌面合成图缩到手机宽度。

### 18.9 首页功能摘要

标题使用 `What it does` / `主要功能`。下方不是卡片，而是五行纵向内容：RAW editing、Photo
library、Library search、AI descriptions and ratings、Performance。

每行桌面端使用 `3 + 9` 列：左侧为功能名，右侧为一小段说明；上下内边距 `28px`，行间用
细分隔线。手机端改成标题在上、正文在下。列表之后只放一个 `View all features` 文本链接。

### 18.10 下载、教程和页脚

下载区使用次级背景 `#F7F7F5`。每个平台是一行，高度约 `72px`，只包含平台名、系统要求和
下载链接，不做下载卡片。中文版的百度网盘是普通次要链接，不出现“国内高速”“镜像加速”或
存储服务说明。

教程区只写一句“Installation and user guide”及一个文档链接。页脚只保留 GitHub、
Documentation、GPL-3.0 和语言链接；不放口号、订阅框、社交媒体图标或大面积 Logo。

### 18.11 Features 页

Features 页与首页共用页头和排版。顶部只放 H1 和一段不超过两行的简介。下面每个功能区固定
使用“左侧 4 列文字、右侧 8 列截图”的布局，不交替左右，避免阅读路线来回跳动。

每个功能区上下内边距 `88px`，区块之间使用分隔线。文字内容固定为：功能标题、一段说明、
最多三个短要点。每个功能区最多一张主图。手机端变为文字在上、截图在下。页面末尾只保留
Download、Documentation、GitHub 三个下一步链接。

### 18.12 状态与可访问性

- 普通链接悬停只改变文字颜色；按钮悬停只改变背景或边框颜色。
- 元素不能在悬停时上浮、放大或产生新阴影。
- 按钮按下可以使用 `translateY(1px)`，持续时间不超过 `120ms`。
- 键盘焦点必须有清晰的 `2px` 外轮廓，并与元素保持 `2px` 间距。
- 正文和按钮对比度达到 WCAG AA；点击目标至少 `44 × 44px`。
- 提供跳到正文的隐藏链接；文档顺序与视觉顺序一致。
- 页面在禁用 CSS 或图片加载失败时，软件说明、下载和文档链接仍然可用。

### 18.13 页面线框

桌面首页的固定结构：

```text
┌ Alcedo Studio ───────── Features  Download  Documentation  GitHub  中文 ┐
├─────────────────────────────────────────────────────────────────────────┤
│ Alcedo Studio / 一句话用途             ┌ 主相册截图 ───────────────┐    │
│ 本地搜索与可选 AI / 支持平台           │                         │    │
│ [Windows] [macOS]  Documentation       │       ┌ 编辑器截图 ─────┐ │    │
│                                        └───────│                 │─┘    │
│                                                └─────────────────┘      │
├─────────────────────────────────────────────────────────────────────────┤
│ What it does                                                          │
│ RAW editing            简短说明                                        │
│ Photo library          简短说明                                        │
│ Library search         简短说明                                        │
│ AI descriptions...     简短说明                                        │
│ Performance            简短说明                                        │
│ View all features                                                     │
├─────────────────────────────────────────────────────────────────────────┤
│ Download：Windows / macOS / 百度网盘（仅中文页）                        │
├─────────────────────────────────────────────────────────────────────────┤
│ Documentation  ·  GitHub  ·  GPL-3.0                                  │
└─────────────────────────────────────────────────────────────────────────┘
```

手机端固定顺序为：两行页头 → 产品名称与说明 → 下载按钮 → 文档链接 → 纵向双截图 → 五项功能
摘要 → Features 链接 → 下载区 → 教程与页脚。

---

## 19. 图片与资源使用方案

### 19.1 图片使用原则

- 网站只使用真实的 Alcedo Studio 界面截图，不制作概念 UI，也不使用装饰性摄影图。
- 原始 PNG 只作为源文件保留，网页不直接加载数 MB 的原图。
- 发布时生成 AVIF 和 WebP，并用 `<picture>` 提供格式回退。
- 发布文件名使用小写 ASCII 和连字符，不把中文源文件名直接放进 URL。
- 所有图片都写明 `width`、`height`、`srcset`、`sizes` 和准确的 `alt`，避免布局跳动。
- 首页合成图使用高加载优先级；Features 页截图全部延迟加载。
- 不在截图上叠加宣传文案、箭头、圆圈或模拟窗口标题栏。
- 源图可以裁切掉无信息的黑边，但不能改变界面内容、伪造结果或隐藏影响理解的区域。

### 19.2 首页图片

首页只有一组视觉内容：主相册和编辑器的双截图合成图。

| 层级 | 源文件 | 保留内容 | 用途 |
| --- | --- | --- | --- |
| 后层 | `docs/alcedo-website/public/screenshots/1-主界面.png` | 完整相册界面，保留左侧导航、中央图库和右侧信息栏 | 说明图片管理能力 |
| 前层 | `docs/alcedo-website/public/screenshots/3-基础调整.png` | 保留照片预览和右侧基础调整面板，裁去多余外缘 | 说明 RAW 编辑能力 |

桌面和手机分别导出，不在 CSS 中临时叠放原始 PNG：

- `hero-workstation-640.avif/webp`：手机纵向构图；
- `hero-workstation-960.avif/webp`：平板构图；
- `hero-workstation-1440.avif/webp`：桌面横向构图。

英文 `alt`：`Alcedo Studio photo library and RAW editor interfaces.`

中文 `alt`：`Alcedo Studio 相册管理和 RAW 编辑界面。`

`alt` 只说明画面，不重复性能、AI 或下载关键词。

### 19.3 Features 页图片对应

| 功能区 | 源文件 | 处理方式 | 画面能够证明的内容 |
| --- | --- | --- | --- |
| RAW editing | `public/screenshots/4-高级色彩.png` | 裁掉多余黑边，保留照片和高级色彩面板 | RAW 照片调整和色彩控制 |
| RAW color science | `public/screenshots/2-色彩科学.png` | 转码后保留右侧显示变换设置 | ACES 2.0 与 OpenDRT |
| RAW tone and decoding | `public/screenshots/2-自然影调1.png`、`2-自然影调2.png` | 分别转码，保留调整、RAW 解码和镜头校正面板 | 影调、RAW 解码与镜头校正 |
| RAW geometry | `public/screenshots/5-几何调整.png` | 转码后保留裁剪网格、旋转角度和几何面板 | 裁剪、旋转和几何调整 |
| RAW film effects | `public/screenshots/5-胶片颗粒与Halation模拟.png` | 转码后保留胶片颗粒与 Halation 控件 | 胶片颗粒和 Halation |
| RAW LUT | `public/screenshots/Portra 400.png` | 转码后保留 LUT 浏览器和选中效果 | LUT 浏览和色彩效果 |
| Photo library | `public/screenshots/7-高级筛选.png` | 保留图库、日期和相机等筛选条件 | 图片浏览、元数据和组合筛选 |
| Library search | `public/screenshots/10-AI自然语言搜索.png` | 重点保留搜索输入和结果，减少无关背景 | 通过内容描述进行模糊搜索 |
| Performance and export | `public/screenshots/6-导出界面.png` | 以导出队列和批量操作为主体 | 批量管理和导出流程 |
| AI overview | `docs/social_media_pub/2026-07-06/10_opencode_analysis_result_inspector.png` | 保留完整图库和右侧分析结果，适度裁掉外部黑边 | 批量分析、AI 描述和评价理由 |
| AI rating result | `docs/social_media_pub/2026-07-06/rating.png` | 保留选中照片、EXIF 信息、描述、理由和星级 | AI 生成描述、评价理由和 4/5 评分 |
| Rating strictness | `docs/social_media_pub/2026-07-06/severity.png` | 保留任务选择和完整的苛刻程度滑杆 | 描述、评分、评分理由任务以及可调苛刻程度 |

表中以 `public/screenshots/` 开头的路径均相对于 `docs/alcedo-website/`。发布前，位于
`docs/social_media_pub/` 的三个 AI 源文件应复制并转换到网站自己的 `site/assets/`，网页不能
在运行时引用网站目录之外的文件。

AI 功能区桌面端可以使用一张主图加一张紧随正文的小图，但不能把三张图全部并排：

1. 主图使用 `10_opencode_analysis_result_inspector.png`，说明批量分析和结果查看；
2. `rating.png` 裁切成结果细节图，紧接“ratings and reasons”说明；
3. `severity.png` 裁切成设置细节图，紧接“adjustable strictness”说明。

Features 页仍保持“一节一个主要视觉”的节奏。后两张细节图是 AI 小节内部的补充，不扩展成
独立营销画廊。EXIF 可兼容读取属于产品行为，由正文清楚说明；截图没有直接显示写回格式时，
图片说明不能声称画面已经证明了 EXIF 写回。

### 19.4 首版暂不使用的现有图片

以下图片不是无效素材，但首版官网暂不使用：

- `8-AI内容识别.png`、`9-AI内容过滤.png`；
- `docs/alcedo-website/public/header.jpg`。

`2-自然影调1.png`、`2-自然影调2.png`、`2-色彩科学.png`、`5-几何调整.png`、
`5-胶片颗粒与Halation模拟.png` 和 `Portra 400.png` 已按用户要求加入 RAW editing 的辅助图库，
并生成对应的 AVIF/WebP 发布资源。`header.jpg` 不再作为首页横幅，因为它属于旧版品牌展示方式，
与新的“软件名称 + 用途 + 下载 + 真实界面”结构冲突。

### 19.5 发布文件结构

实现阶段建议整理为：

```text
docs/alcedo-website/public/
├─ media/
│  ├─ hero-workstation-640.avif
│  ├─ hero-workstation-640.webp
│  ├─ hero-workstation-960.avif
│  ├─ hero-workstation-960.webp
│  ├─ hero-workstation-1440.avif
│  ├─ hero-workstation-1440.webp
│  ├─ feature-raw-editor.avif
│  ├─ feature-raw-editor.webp
│  ├─ feature-raw-tone.avif
│  ├─ feature-raw-tone.webp
│  ├─ feature-raw-decoding.avif
│  ├─ feature-raw-decoding.webp
│  ├─ feature-raw-color-science.avif
│  ├─ feature-raw-color-science.webp
│  ├─ feature-raw-geometry.avif
│  ├─ feature-raw-geometry.webp
│  ├─ feature-raw-film-grain.avif
│  ├─ feature-raw-film-grain.webp
│  ├─ feature-raw-lut.avif
│  ├─ feature-raw-lut.webp
│  ├─ feature-library-filter.avif
│  ├─ feature-library-filter.webp
│  ├─ feature-library-search.avif
│  ├─ feature-library-search.webp
│  ├─ feature-export.avif
│  ├─ feature-export.webp
│  ├─ feature-ai-overview.avif
│  ├─ feature-ai-overview.webp
│  ├─ feature-ai-rating.avif
│  ├─ feature-ai-rating.webp
│  ├─ feature-ai-strictness.avif
│  └─ feature-ai-strictness.webp
├─ social-card.png
└─ favicon.svg
```

源 PNG 继续保留在原位置，转换后的文件是可重复生成的发布产物。生成脚本固定裁切区域、输出尺寸
和压缩参数，避免每次人工导出造成构图变化。

### 19.6 尺寸与体积目标

| 资源 | 像素宽度 | AVIF 目标上限 | WebP 目标上限 |
| --- | ---: | ---: | ---: |
| 手机 Hero | `640px` | `160 KB` | `250 KB` |
| 平板 Hero | `960px` | `230 KB` | `360 KB` |
| 桌面 Hero | `1440px` | `320 KB` | `510 KB` |
| Features 主图 | `1280–1600px` | 每张 `260 KB` | 每张 `420 KB` |
| AI 细节图 | `800–1100px` | 每张 `160 KB` | 每张 `260 KB` |
| 社交分享图 | `1200 × 630px` | 不使用 | PNG/JPEG `300 KB` |
| Favicon | SVG | `8 KB` | 不适用 |

体积是目标，不是牺牲可读性的硬上限。压缩后必须能看清菜单名、评分和苛刻程度刻度；如果文字
已经糊掉，应提高质量，而不是继续压缩。

### 19.7 社交分享图与 Favicon

Open Graph 分享图使用白色背景：左侧为 `Alcedo Studio` 和一句产品定义，右侧使用 Hero 合成图
的局部。图中不放版本号、下载按钮、平台列表或“free”徽章，避免发布后很快过时。

Favicon 使用简单的蓝色字母 `A` SVG。首版不新设计鸟、相机或光圈图形标志；文字名称已经足以
识别软件，新增图形 Logo 反而会扩大设计范围。

### 19.8 中英文图片策略

首版中英文网页共用现有截图，分别提供中英文 `alt` 和必要的图片说明。不能在截图上覆盖伪造的
英文界面文字。只有在 Alcedo Studio 的英文界面稳定且可以重新截取同一状态后，才替换英文页
截图；首版不维护两套内容不同的截图流水线。

### 19.9 图片验收清单

- 在 `360px`、`768px`、`1280px`、`1920px` 宽度检查构图和界面文字可读性。
- 验证 AVIF、WebP 及格式回退；同一视口只请求合适尺寸的 Hero。
- Hero 预留固有宽高，不产生明显布局偏移。
- Features 页首屏之外的截图使用 `loading="lazy"` 和 `decoding="async"`。
- 检查图片中没有 API Key、账号、私人路径、未授权照片或其他敏感内容。
- 不拉伸截图，不裁掉关键控件，不增加过重阴影。
- `alt` 描述画面本身，正文负责解释功能和 SEO 关键词。
