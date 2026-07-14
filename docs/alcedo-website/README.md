# Alcedo Studio website

Static production site for Alcedo Studio. **Deploy root: `site/` only.**

## Layout

```text
site/
  index.html              English home
  features/index.html     English features
  zh-cn/index.html        Chinese home
  zh-cn/features/         Chinese features
  404.html
  robots.txt
  sitemap.xml
  .nojekyll
  assets/
    site.css
    hero-workstation-*.{avif,webp}
    feature-*.{avif,webp}
    social-card.png
    favicon.svg
```

No JavaScript is required for core pages. Content pages use **relative** asset and language links so the same tree works under:

- GitHub Pages project URL: `https://zidage.github.io/AlcedoStudio/`
- Future domain root (Cloudflare Workers): `/`

Canonical, hreflang, Open Graph, Twitter cards, JSON-LD, robots, and sitemap use the current public origin:

`https://zidage.github.io/AlcedoStudio`

Update those absolute SEO URLs in one pass when the custom domain goes live (Phase 5).

## Cloudflare Workers deployment

This project uses Workers Static Assets. The Worker serves `site/` directly; it has no
application script, server, database, or build step.

`wrangler.jsonc` is the deployment configuration:

- `assets.directory` is `./site`;
- `not_found_handling` is `404-page`, because this is a multi-page static site rather
  than a client-side single-page application;
- the first deployment is available on a `workers.dev` test address until a custom
  domain is attached in Cloudflare.

### First deployment

```bash
# from docs/alcedo-website
npm install
npm run deploy
```

For continuous deployment, connect the GitHub repository in **Workers & Pages** with:

```text
Production branch: main
Root directory: docs/alcedo-website
Build command: (leave empty)
Deploy command: npm run deploy
```

After the Worker is deployed, attach the apex custom domain in **Worker → Settings →
Domains & Routes**. Keep `www` as a 301 redirect to the apex domain so only one hostname
is indexable.

Do not change the SEO origin until the custom domain returns the deployed site. At that
point, update the canonical, `hreflang`, Open Graph, JSON-LD image URLs, `robots.txt`,
`sitemap.xml`, `404.html`, and `scripts/verify_site.py` together. GitHub Pages remains a
fallback until this cutover is verified.

## Local preview

```bash
# from docs/alcedo-website
python -m http.server 8080 --directory site
```

Open `http://127.0.0.1:8080/`.

To mimic the GitHub Pages subpath locally:

```bash
# from docs/alcedo-website
mkdir -p /tmp/alcedo-pages/AlcedoStudio
# Windows PowerShell example:
# New-Item -ItemType Directory -Force -Path $env:TEMP\alcedo-pages\AlcedoStudio
# Copy-Item -Recurse site\* $env:TEMP\alcedo-pages\AlcedoStudio\
# python -m http.server 8080 --directory $env:TEMP\alcedo-pages
```

Then open `http://127.0.0.1:8080/AlcedoStudio/`.

## Validate before deploy

```bash
python scripts/verify_site.py
```

Checks required files, robots/sitemap, SEO tags, portable (non root-absolute) asset paths, and that internal relative links resolve.

## Regenerate images

Source screenshots: `public/screenshots/` and `docs/social_media_pub/2026-07-06/`.

```bash
python scripts/build_assets.py
```

Requires Pillow with WebP and AVIF support.

## GitHub Pages deploy

Workflow: `.github/workflows/website.yml` (single entry; the old Vite / Node build and duplicate workflow are gone).

- Trigger: push to `main` touching `docs/alcedo-website/site/**`, or `workflow_dispatch`
- Steps: checkout → `verify_site.py` → upload `site/` as Pages artifact → deploy
- Repo setting: Pages → Source = **GitHub Actions**

After merge to `main`, the public site should be:

`https://zidage.github.io/AlcedoStudio/`

## Copy and design sources

- Public copy: `docs/roadmap/alcedo_website_public_copy.md`
- Redesign plan: `docs/roadmap/alcedo_website_redesign_plan.md`
