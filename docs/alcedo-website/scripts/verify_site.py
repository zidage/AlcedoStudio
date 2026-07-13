#!/usr/bin/env python3
"""Validate the static site before GitHub Pages deploy.

Checks:
- required files exist
- no root-absolute asset URLs (so /AlcedoStudio/ and future domain root both work)
- internal relative links resolve to files under site/
- each HTML page has title, description, canonical, hreflang, and Open Graph basics
- robots.txt and sitemap.xml list the public canonical URLs
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote

SITE = Path(__file__).resolve().parents[1] / "site"
PUBLIC_ORIGIN = "https://zidage.github.io/AlcedoStudio"
CANONICAL_PAGES = (
    f"{PUBLIC_ORIGIN}/",
    f"{PUBLIC_ORIGIN}/features/",
    f"{PUBLIC_ORIGIN}/zh-cn/",
    f"{PUBLIC_ORIGIN}/zh-cn/features/",
)

REQUIRED_FILES = (
    "index.html",
    "features/index.html",
    "zh-cn/index.html",
    "zh-cn/features/index.html",
    "404.html",
    "robots.txt",
    "sitemap.xml",
    ".nojekyll",
    "assets/site.css",
    "assets/favicon.svg",
    "assets/social-card.png",
    "assets/hero-workstation-640.webp",
    "assets/hero-workstation-960.webp",
    "assets/hero-workstation-1440.webp",
    "assets/hero-workstation-640.avif",
    "assets/hero-workstation-960.avif",
    "assets/hero-workstation-1440.avif",
)

errors: list[str] = []
warnings: list[str] = []


def err(msg: str) -> None:
    errors.append(msg)


def warn(msg: str) -> None:
    warnings.append(msg)


def check_required_files() -> None:
    for rel in REQUIRED_FILES:
        if not (SITE / rel).is_file():
            err(f"missing required file: {rel}")


def check_robots_and_sitemap() -> None:
    robots = (SITE / "robots.txt").read_text(encoding="utf-8")
    if f"Sitemap: {PUBLIC_ORIGIN}/sitemap.xml" not in robots:
        err("robots.txt must point to the public sitemap URL")
    if "Allow: /" not in robots:
        err("robots.txt must allow crawling")

    sitemap = (SITE / "sitemap.xml").read_text(encoding="utf-8")
    for url in CANONICAL_PAGES:
        if f"<loc>{url}</loc>" not in sitemap:
            err(f"sitemap.xml missing canonical URL: {url}")


def strip_query_fragment(href: str) -> str:
    return href.split("#", 1)[0].split("?", 1)[0]


def resolve_local(page: Path, href: str) -> Path | None:
    """Return expected filesystem path for a relative in-site link, or None if external/skip."""
    href = strip_query_fragment(href.strip())
    if not href or href.startswith(("http://", "https://", "mailto:", "data:", "javascript:")):
        return None
    if href.startswith("//"):
        return None
    if href.startswith("/"):
        err(f"{page.relative_to(SITE)}: root-absolute path is not portable: {href}")
        return None

    target = (page.parent / unquote(href)).resolve()
    try:
        target.relative_to(SITE.resolve())
    except ValueError:
        err(f"{page.relative_to(SITE)}: link escapes site root: {href}")
        return None
    return target


def path_exists_for_url(target: Path) -> bool:
    if target.is_file():
        return True
    # Directory URL → index.html
    if target.is_dir() and (target / "index.html").is_file():
        return True
    # href without trailing slash that maps to a directory
    if not target.suffix and (target / "index.html").is_file():
        return True
    if not target.suffix and target.with_suffix(".html").is_file():
        return True
    return False


HREF_SRC_RE = re.compile(
    r"""(?:href|src)\s*=\s*["']([^"']+)["']""",
    re.IGNORECASE,
)
SRCSET_RE = re.compile(r"""srcset\s*=\s*["']([^"']+)["']""", re.IGNORECASE)
CSS_URL_RE = re.compile(r"""url\(\s*['"]?([^'")]+)['"]?\s*\)""", re.IGNORECASE)


def check_html_page(page: Path) -> None:
    rel = page.relative_to(SITE).as_posix()
    text = page.read_text(encoding="utf-8")

    if re.search(r"""(?:href|src)\s*=\s*["']/(?!/)""", text):
        err(f"{rel}: contains root-absolute href/src (breaks /AlcedoStudio/ subpath)")

    # SEO requirements for content pages (not 404)
    if page.name != "404.html":
        if "<title>" not in text:
            err(f"{rel}: missing <title>")
        if 'name="description"' not in text:
            err(f"{rel}: missing meta description")
        if 'rel="canonical"' not in text:
            err(f"{rel}: missing canonical")
        for hreflang in ("en", "zh-CN", "x-default"):
            if f'hreflang="{hreflang}"' not in text:
                err(f"{rel}: missing hreflang {hreflang}")
        for prop in ("og:title", "og:description", "og:type", "og:url", "og:image"):
            if f'property="{prop}"' not in text and f"property='{prop}'" not in text:
                err(f"{rel}: missing Open Graph {prop}")
        if 'name="twitter:card"' not in text:
            err(f"{rel}: missing twitter:card")
        # Canonical must use public origin
        m = re.search(r'rel="canonical"\s+href="([^"]+)"', text)
        if m and not m.group(1).startswith(PUBLIC_ORIGIN):
            err(f"{rel}: canonical not under public origin: {m.group(1)}")

    # Home pages need SoftwareApplication JSON-LD
    if rel in ("index.html", "zh-cn/index.html"):
        if "SoftwareApplication" not in text:
            err(f"{rel}: missing SoftwareApplication JSON-LD")

    for m in HREF_SRC_RE.finditer(text):
        href = m.group(1)
        target = resolve_local(page, href)
        if target is None:
            continue
        if not path_exists_for_url(target):
            # allow pure fragment already stripped; empty after strip means same page
            if strip_query_fragment(href) == "":
                continue
            err(f"{rel}: broken local link -> {href}")

    for m in SRCSET_RE.finditer(text):
        for part in m.group(1).split(","):
            file_part = part.strip().split()[0] if part.strip() else ""
            if not file_part:
                continue
            target = resolve_local(page, file_part)
            if target is not None and not target.is_file():
                err(f"{rel}: broken srcset asset -> {file_part}")


def check_css() -> None:
    css_path = SITE / "assets" / "site.css"
    if not css_path.is_file():
        return
    text = css_path.read_text(encoding="utf-8")
    for m in CSS_URL_RE.finditer(text):
        url = m.group(1).strip()
        if url.startswith(("data:", "http://", "https://")):
            continue
        if url.startswith("/"):
            err(f"site.css: root-absolute url() is not portable: {url}")


def main() -> int:
    if not SITE.is_dir():
        print(f"ERROR: site directory missing: {SITE}", file=sys.stderr)
        return 1

    check_required_files()
    check_robots_and_sitemap()
    check_css()

    for page in sorted(SITE.rglob("*.html")):
        check_html_page(page)

    for w in warnings:
        print(f"WARNING: {w}")
    if errors:
        print(f"verify_site: {len(errors)} error(s)")
        for e in errors:
            print(f"  - {e}")
        return 1

    print(f"verify_site: OK ({len(list(SITE.rglob('*.html')))} HTML pages)")
    print(f"  public origin: {PUBLIC_ORIGIN}")
    print(f"  deploy root:   {SITE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
