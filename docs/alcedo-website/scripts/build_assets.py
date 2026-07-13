#!/usr/bin/env python3
"""Generate optimized website images from source screenshots.

Outputs AVIF/WebP (and social-card.png, favicon.svg) into site/assets/.
Source PNGs stay in public/screenshots/ and docs/social_media_pub/.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]  # docs/alcedo-website
SHOTS = ROOT / "public" / "screenshots"
SOCIAL = ROOT.parent / "social_media_pub" / "2026-07-06"
OUT = ROOT / "site" / "assets"

BRAND_BLUE = (66, 111, 143)  # #426F8F
TEXT = (32, 33, 36)  # #202124
MUTED = (95, 99, 104)  # #5F6368
BG = (255, 255, 255)
HERO_CANVAS = (247, 247, 245)  # #F7F7F5


def load_rgb(path: Path) -> Image.Image:
    im = Image.open(path)
    if im.mode in ("RGBA", "LA"):
        base = Image.new("RGB", im.size, HERO_CANVAS)
        base.paste(im, mask=im.split()[-1])
        return base
    return im.convert("RGB")


def fit_width(im: Image.Image, width: int) -> Image.Image:
    if im.width == width:
        return im
    height = max(1, round(im.height * (width / im.width)))
    return im.resize((width, height), Image.Resampling.LANCZOS)


def round_corners(im: Image.Image, radius: int) -> Image.Image:
    if radius <= 0:
        return im.convert("RGBA")
    rgba = im.convert("RGBA")
    mask = Image.new("L", rgba.size, 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((0, 0, rgba.width - 1, rgba.height - 1), radius=radius, fill=255)
    rgba.putalpha(mask)
    return rgba


def add_edge(im: Image.Image, color=(226, 228, 231, 255)) -> Image.Image:
    """Very light 1px edge on the alpha-aware image."""
    out = im.copy()
    draw = ImageDraw.Draw(out)
    draw.rounded_rectangle(
        (0, 0, out.width - 1, out.height - 1),
        radius=6,
        outline=color,
        width=1,
    )
    return out


def drop_shadow(
    layer: Image.Image,
    canvas_size: tuple[int, int],
    xy: tuple[int, int],
    blur: int = 18,
    offset: tuple[int, int] = (0, 10),
    opacity: int = 26,
) -> Image.Image:
    """Compose a soft shadow under layer onto a new transparent canvas."""
    shadow = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    alpha = layer.split()[-1] if layer.mode == "RGBA" else Image.new("L", layer.size, 255)
    sh = Image.new("RGBA", layer.size, (20, 28, 35, opacity))
    sh.putalpha(alpha.point(lambda a: min(255, int(a * opacity / 255))))
    ox, oy = xy[0] + offset[0], xy[1] + offset[1]
    shadow.paste(sh, (ox, oy), sh)
    shadow = shadow.filter(ImageFilter.GaussianBlur(blur))
    result = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    result = Image.alpha_composite(result, shadow)
    result.paste(layer, xy, layer)
    return result


def compose_desktop_hero(library: Image.Image, editor: Image.Image, out_w: int) -> Image.Image:
    """Overlapped dual-window composition (~1.52:1)."""
    out_h = max(1, round(out_w / 1.52))
    pad = max(8, round(out_w * 0.028))
    radius = max(4, round(out_w * 0.006))

    lib_w = round(out_w * 0.82)
    lib = fit_width(library, lib_w)
    # Crop height if needed so both fit the canvas with overlap
    max_lib_h = out_h - pad * 2
    if lib.height > max_lib_h:
        top = 0
        lib = lib.crop((0, top, lib.width, top + max_lib_h))

    ed_w = round(out_w * 0.76)
    ed = fit_width(editor, ed_w)
    max_ed_h = out_h - pad * 2
    if ed.height > max_ed_h:
        # Prefer keeping the right adjustment panel; crop from left if very tall
        top = 0
        ed = ed.crop((0, top, ed.width, top + max_ed_h))

    lib_r = add_edge(round_corners(lib, radius))
    ed_r = add_edge(round_corners(ed, radius))

    canvas = Image.new("RGBA", (out_w, out_h), (*HERO_CANVAS, 255))

    # Library top-left
    lib_xy = (pad, pad)
    with_lib = drop_shadow(lib_r, (out_w, out_h), lib_xy, blur=max(10, out_w // 90), opacity=28)
    canvas = Image.alpha_composite(canvas, with_lib)

    # Editor bottom-right — ~28% overlap with library
    ed_x = out_w - ed_r.width - pad
    ed_y = out_h - ed_r.height - pad
    with_ed = drop_shadow(ed_r, (out_w, out_h), (ed_x, ed_y), blur=max(10, out_w // 90), opacity=32)
    canvas = Image.alpha_composite(canvas, with_ed)

    return canvas.convert("RGB")


def compose_mobile_hero(library: Image.Image, editor: Image.Image, out_w: int) -> Image.Image:
    """Vertical stack: library above editor with ~10% overlap."""
    pad = max(8, round(out_w * 0.04))
    radius = max(4, round(out_w * 0.01))
    content_w = out_w - pad * 2

    lib = fit_width(library, content_w)
    ed = fit_width(editor, content_w)

    # Cap each pane height so the stack stays usable on phones
    max_pane_h = round(out_w * 0.72)
    if lib.height > max_pane_h:
        lib = lib.crop((0, 0, lib.width, max_pane_h))
    if ed.height > max_pane_h:
        ed = ed.crop((0, 0, ed.width, max_pane_h))

    lib_r = add_edge(round_corners(lib, radius))
    ed_r = add_edge(round_corners(ed, radius))

    overlap = round(min(lib_r.height, ed_r.height) * 0.10)
    out_h = pad + lib_r.height + ed_r.height - overlap + pad
    canvas = Image.new("RGBA", (out_w, out_h), (*HERO_CANVAS, 255))

    lib_xy = (pad, pad)
    with_lib = drop_shadow(lib_r, (out_w, out_h), lib_xy, blur=12, opacity=26)
    canvas = Image.alpha_composite(canvas, with_lib)

    ed_y = pad + lib_r.height - overlap
    with_ed = drop_shadow(ed_r, (out_w, out_h), (pad, ed_y), blur=12, opacity=30)
    canvas = Image.alpha_composite(canvas, with_ed)

    return canvas.convert("RGB")


def save_pair(im: Image.Image, stem: str, quality_avif: int = 55, quality_webp: int = 78) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    avif_path = OUT / f"{stem}.avif"
    webp_path = OUT / f"{stem}.webp"
    im.save(avif_path, format="AVIF", quality=quality_avif)
    im.save(webp_path, format="WEBP", quality=quality_webp, method=6)
    print(f"  {stem}: avif={avif_path.stat().st_size // 1024}KB webp={webp_path.stat().st_size // 1024}KB size={im.size}")


def trim_dark_margins(im: Image.Image, threshold: int = 18) -> Image.Image:
    """Crop near-black letterbox margins when present."""
    gray = im.convert("L")
    w, h = gray.size
    px = gray.load()

    def row_dark(y: int) -> bool:
        dark = 0
        step = max(1, w // 200)
        for x in range(0, w, step):
            if px[x, y] < threshold:
                dark += 1
        return dark > (w // step) * 0.92

    def col_dark(x: int) -> bool:
        dark = 0
        step = max(1, h // 200)
        for y in range(0, h, step):
            if px[x, y] < threshold:
                dark += 1
        return dark > (h // step) * 0.92

    top = 0
    while top < h - 1 and row_dark(top):
        top += 1
    bottom = h - 1
    while bottom > top and row_dark(bottom):
        bottom -= 1
    left = 0
    while left < w - 1 and col_dark(left):
        left += 1
    right = w - 1
    while right > left and col_dark(right):
        right -= 1

    # Only crop if margins are meaningful
    if top < 4 and left < 4 and (h - 1 - bottom) < 4 and (w - 1 - right) < 4:
        return im
    return im.crop((left, top, right + 1, bottom + 1))


def feature_export(src: Path, stem: str, max_w: int = 1440, quality_avif: int = 52) -> None:
    im = trim_dark_margins(load_rgb(src))
    if im.width > max_w:
        im = fit_width(im, max_w)
    save_pair(im, stem, quality_avif=quality_avif, quality_webp=76)


def feature_detail(src: Path, stem: str, max_w: int = 960, crop: tuple[float, float, float, float] | None = None) -> None:
    """Optional relative crop box (l,t,r,b) as fractions 0–1."""
    im = load_rgb(src)
    if crop:
        l, t, r, b = crop
        im = im.crop(
            (
                int(im.width * l),
                int(im.height * t),
                int(im.width * r),
                int(im.height * b),
            )
        )
    im = trim_dark_margins(im)
    if im.width > max_w:
        im = fit_width(im, max_w)
    save_pair(im, stem, quality_avif=50, quality_webp=74)


def make_social_card(hero: Image.Image) -> None:
    w, h = 1200, 630
    card = Image.new("RGB", (w, h), BG)
    draw = ImageDraw.Draw(card)

    # Left text block
    try:
        font_title = ImageFont.truetype("segoeui.ttf", 48)
        font_body = ImageFont.truetype("segoeui.ttf", 26)
        font_alcedo = ImageFont.truetype("segoeuib.ttf", 48)
    except OSError:
        font_title = ImageFont.load_default()
        font_body = font_title
        font_alcedo = font_title

    x = 56
    y = 180
    draw.text((x, y), "Alcedo", font=font_alcedo, fill=BRAND_BLUE)
    # measure Alcedo width for Studio placement
    alcedo_w = draw.textlength("Alcedo ", font=font_alcedo)
    draw.text((x + alcedo_w, y), "Studio", font=font_title, fill=TEXT)

    body = "Open-source photography workstation\nfor RAW editing and image management."
    draw.multiline_text((x, y + 72), body, font=font_body, fill=MUTED, spacing=8)

    # Right: cropped hero
    hero_w = 560
    hero_h = 500
    hero_fit = hero.copy()
    # Cover-crop center of hero into the right panel
    scale = max(hero_w / hero_fit.width, hero_h / hero_fit.height)
    nw, nh = int(hero_fit.width * scale), int(hero_fit.height * scale)
    hero_fit = hero_fit.resize((nw, nh), Image.Resampling.LANCZOS)
    left = (nw - hero_w) // 2
    top = (nh - hero_h) // 2
    hero_crop = hero_fit.crop((left, top, left + hero_w, top + hero_h))
    hero_r = round_corners(hero_crop, 8)
    # light edge
    draw_r = ImageDraw.Draw(hero_r)
    draw_r.rounded_rectangle((0, 0, hero_r.width - 1, hero_r.height - 1), radius=8, outline=(226, 228, 231, 255), width=1)

    paste_x = w - hero_w - 48
    paste_y = (h - hero_h) // 2
    card_rgba = card.convert("RGBA")
    card_rgba.paste(hero_r, (paste_x, paste_y), hero_r)
    card = card_rgba.convert("RGB")

    path = OUT / "social-card.png"
    card.save(path, format="PNG", optimize=True)
    print(f"  social-card.png: {path.stat().st_size // 1024}KB")


def make_favicon() -> None:
    svg = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" role="img" aria-label="Alcedo Studio">
  <rect width="32" height="32" rx="6" fill="#426F8F"/>
  <text x="16" y="22" text-anchor="middle" font-family="system-ui,Segoe UI,sans-serif" font-size="18" font-weight="600" fill="#ffffff">A</text>
</svg>
"""
    path = OUT / "favicon.svg"
    path.write_text(svg, encoding="utf-8")
    print(f"  favicon.svg: {path.stat().st_size}B")


def main() -> None:
    print("Building Alcedo website assets…")
    library = load_rgb(SHOTS / "1-主界面.png")
    editor = load_rgb(SHOTS / "3-基础调整.png")

    # Desktop / tablet heroes (landscape overlap)
    for w, q in ((1440, 52), (960, 50)):
        hero = compose_desktop_hero(library, editor, w)
        save_pair(hero, f"hero-workstation-{w}", quality_avif=q, quality_webp=74)

    # Mobile hero (vertical stack)
    mobile = compose_mobile_hero(library, editor, 640)
    save_pair(mobile, "hero-workstation-640", quality_avif=48, quality_webp=72)

    # Features images (plan §19.3)
    feature_export(SHOTS / "4-高级色彩.png", "feature-raw-editor")
    feature_export(SHOTS / "7-高级筛选.png", "feature-library-filter")
    feature_export(SHOTS / "10-AI自然语言搜索.png", "feature-library-search")
    feature_export(SHOTS / "6-导出界面.png", "feature-export")

    feature_export(SOCIAL / "10_opencode_analysis_result_inspector.png", "feature-ai-overview", max_w=1440)
    # Rating: focus on inspector panel region (right half often denser)
    feature_detail(
        SOCIAL / "rating.png",
        "feature-ai-rating",
        max_w=1000,
        crop=(0.35, 0.08, 0.98, 0.92),
    )
    feature_detail(
        SOCIAL / "severity.png",
        "feature-ai-strictness",
        max_w=1000,
        crop=(0.28, 0.10, 0.98, 0.90),
    )

    desktop_for_social = compose_desktop_hero(library, editor, 1200)
    make_social_card(desktop_for_social)
    make_favicon()
    print("Done →", OUT)


if __name__ == "__main__":
    main()
