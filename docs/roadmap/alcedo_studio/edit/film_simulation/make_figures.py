"""Figures of film_simulation_explained.md.

Inputs: a checkout of alcedo-film-lab (profiles/, results/, and the CIE tables that
scripts/poc/p0_data_and_reference/fetch_sources.py downloads to build/tmp/p0/sources/).

    python -I make_figures.py --lab D:/Projects/alcedo-film-lab

Needs numpy and matplotlib. The figures are data plots and illustrations.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

OUT = Path(__file__).resolve().parent / "figures"
WL = np.arange(360.0, 781.0, 1.0)
LAYER_COLOURS = {"red": "#c0392b", "green": "#27ae60", "blue": "#2e6fd8"}
DYE_COLOURS = {"cyan_dye": "#16a5b8", "magenta_dye": "#c2338f", "yellow_dye": "#d9a400"}


def style() -> None:
    plt.rcParams.update({
        "figure.dpi": 110, "savefig.dpi": 110, "font.size": 9, "axes.titlesize": 10,
        "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
        "grid.alpha": 0.25, "legend.frameon": False, "legend.fontsize": 8,
    })


def profile(lab: Path, name: str) -> dict:
    return json.loads((lab / "profiles" / f"{name}.json").read_text(encoding="utf-8"))


def curve(chart: dict, key: str, x: np.ndarray | None = None, fill: float = np.nan) -> tuple[np.ndarray, np.ndarray]:
    c = chart["curves"][key]
    cx, cy = np.asarray(c["x"], float), np.asarray(c["y"], float)
    if x is None:
        return cx, cy
    return x, np.interp(x, cx, cy, left=fill, right=fill)


def cie(lab: Path, name: str) -> np.ndarray:
    return np.loadtxt(lab / "build/tmp/p0/sources" / name, delimiter=",")


def planck(wl_nm: np.ndarray, kelvin: float) -> np.ndarray:
    c2 = 1.4388e7  # nm K
    s = wl_nm ** -5 / np.expm1(c2 / (wl_nm * kelvin))
    return s / np.interp(560.0, wl_nm, s)


def layer_key(layer: str) -> str:
    return {"red": "cyan_forming_layer", "green": "magenta_forming_layer", "blue": "yellow_forming_layer"}[layer]


def sensitivities(p: dict) -> dict[str, np.ndarray]:
    chart = p["charts"]["spectral_sensitivity"]
    out = {}
    for layer in ("red", "green", "blue"):
        _, y = curve(chart, layer_key(layer), WL)
        lin = np.where(np.isfinite(y), 10.0 ** y, 0.0)
        out[layer] = lin / lin.max()
    return out


def fig_sensitivity_vs_cmf(lab: Path) -> None:
    cmf = cie(lab, "CIE_xyz_1931_2deg.csv")
    fig, axes = plt.subplots(1, 2, figsize=(9.5, 3.4), sharey=True)
    for ax, (name, title) in zip(axes, [("kodak_5207", "Kodak Vision3 250D 5207"), ("kodak_portra_400", "Kodak Portra 400")]):
        for k, label, ls in ((1, "x\u0304 (CIE 1931)", ":"), (2, "y\u0304", "--"), (3, "z\u0304", "-.")):
            y = np.interp(WL, cmf[:, 0], cmf[:, k], left=0, right=0)
            ax.plot(WL, y / y.max(), color="0.45", ls=ls, lw=1.0, label=label)
        for layer, s in sensitivities(profile(lab, name)).items():
            ax.plot(WL, s, color=LAYER_COLOURS[layer], lw=1.8, label=f"{layer} layer")
        ax.axvspan(650, 700, color=LAYER_COLOURS["red"], alpha=0.07)
        ax.set_title(title)
        ax.set_xlabel("wavelength (nm)")
        ax.set_xlim(370, 740)
    axes[0].set_ylabel("relative sensitivity (peak = 1)")
    axes[1].legend(loc="upper left", bbox_to_anchor=(1.01, 1.0))
    fig.suptitle("Film layer sensitivities against the CIE 1931 colour matching functions", y=1.02)
    fig.tight_layout()
    fig.savefig(OUT / "layer_sensitivity_vs_cmf.png", bbox_inches="tight")
    plt.close(fig)


def mtf_model(f_cyc_mm: np.ndarray, s_um: float, sigma_um: float, boost: float) -> np.ndarray:
    s, sd = s_um * 1e-3, sigma_um * 1e-3
    return 1.0 / (1.0 + (2 * np.pi * s * f_cyc_mm) ** 2) * boost / (1.0 + (boost - 1.0) * np.exp(-2 * np.pi ** 2 * sd ** 2 * f_cyc_mm ** 2))


def fig_stock_data(lab: Path) -> None:
    p = profile(lab, "kodak_5207")
    fit = json.loads((lab / "results/p4_physical_film_response/mtf_fit.json").read_text())["5207"]
    fig, axes = plt.subplots(1, 3, figsize=(12, 3.5))
    ch = p["charts"]["characteristic"]
    for layer in ("red", "green", "blue"):
        x, y = curve(ch, layer)
        axes[0].plot(x, y, color=LAYER_COLOURS[layer], lw=1.6, label=layer)
    axes[0].set_xlabel("log10 exposure (lux s)")
    axes[0].set_ylabel("Status M density")
    axes[0].set_title("Characteristic curves")
    axes[0].legend()

    dyes = p["charts"]["spectral_dye_density"]
    wl = np.arange(400.0, 751.0)
    for key, col in DYE_COLOURS.items():
        _, y = curve(dyes, key, wl)
        axes[1].plot(wl, y, color=col, lw=1.6, label=key.replace("_", " "))
    _, dmin = curve(dyes, "minimum_density", wl)
    _, mid = curve(dyes, "midscale_neutral", wl)
    axes[1].plot(wl, dmin, color="#e67e22", lw=2.0, label="minimum density (orange base)")
    axes[1].plot(wl, mid, color="0.25", lw=1.2, ls="--", label="midscale neutral")
    axes[1].set_xlabel("wavelength (nm)")
    axes[1].set_ylabel("spectral density")
    axes[1].set_title("Dyes and base")
    axes[1].legend(fontsize=7)

    mt = p["charts"]["modulation_transfer"]
    f = np.linspace(1, 100, 300)
    for layer in ("red", "green", "blue"):
        x, y = curve(mt, layer)
        axes[2].plot(x, y, "o", ms=2.2, color=LAYER_COLOURS[layer], alpha=0.6)
        v = fit[layer]
        axes[2].plot(f, mtf_model(f, v["spread_um"], v["sigma_inhibitor_um"], v["boost"]), color=LAYER_COLOURS[layer], lw=1.4,
                     label=f"{layer}: s {v['spread_um']:.1f} um, B {v['boost']:.2f}")
    axes[2].set_xscale("log")
    axes[2].set_xlabel("spatial frequency (cycles/mm)")
    axes[2].set_ylabel("response")
    axes[2].set_title("Modulation transfer: chart (dots) and fit")
    axes[2].legend(fontsize=7)
    fig.suptitle("Published data of Kodak Vision3 250D 5207 (digitized by spectral_film_lut, MIT)", y=1.02)
    fig.tight_layout()
    fig.savefig(OUT / "stock_data_5207.png", bbox_inches="tight")
    plt.close(fig)


def fig_inhibition_edge() -> None:
    """One layer, 1D: a step of exposure, with inhibitor strength K = 0 and K = 0.2."""
    log_h = np.linspace(-4.0, 2.0, 2001)

    def published(x: np.ndarray) -> np.ndarray:  # an illustrative S curve of dye amount
        return 2.2 / (1.0 + np.exp(-2.0 * (x + 1.0))) + 0.05

    k = 0.20
    y = log_h - k * published(log_h)  # flat field: a = F(logH - K a) must equal published(logH)
    def f_comp(v: np.ndarray) -> np.ndarray:
        return np.interp(v, y, published(log_h))

    um = np.arange(-300.0, 300.0, 1.0)
    edge = np.where(um < 0, -2.0, -0.6)
    sigma = 35.0
    g = np.exp(-0.5 * (np.arange(-150, 151) / sigma) ** 2)
    g /= g.sum()
    a = published(edge)
    for _ in range(16):
        blurred = np.convolve(np.pad(a, 150, mode="edge"), g, mode="valid")
        a = f_comp(edge - k * blurred)

    fig, axes = plt.subplots(1, 2, figsize=(10, 3.3))
    axes[0].plot(log_h, published(log_h), color="0.2", lw=1.6, label="published curve (neutral, flat field)")
    axes[0].plot(log_h, f_comp(log_h), color="#8e44ad", lw=1.4, ls="--", label="F: the curve before inhibition")
    axes[0].set_xlabel("log10 exposure")
    axes[0].set_ylabel("dye amount")
    axes[0].set_title("F is solved so that a flat neutral returns the published curve")
    axes[0].legend(fontsize=7)
    axes[1].plot(um, published(edge), color="0.55", lw=1.4, label="K = 0: the published curve")
    axes[1].plot(um, a, color="#8e44ad", lw=1.8, label="with inhibitor, G: 35 um Gaussian")
    axes[1].set_xlabel("position on the film (um)")
    axes[1].set_ylabel("dye amount")
    axes[1].set_title("At an edge the inhibitor of the dense side spreads: edge effect")
    axes[1].legend(fontsize=7, loc="center right")
    fig.suptitle("Illustration of rule development_inhibition, a = F(log H - K G*a), one layer, K = 0.2 (illustrative values)", y=1.03)
    fig.tight_layout()
    fig.savefig(OUT / "inhibition_edge.png", bbox_inches="tight")
    plt.close(fig)


def fig_printer_light(lab: Path) -> None:
    neg = profile(lab, "kodak_5207")
    dyes = neg["charts"]["spectral_dye_density"]
    _, mid = curve(dyes, "midscale_neutral", WL, fill=np.nan)
    _, dmin = curve(dyes, "minimum_density", WL, fill=np.nan)
    lamp_a = np.interp(WL, *cie(lab, "CIE_std_illum_A_1nm.csv").T)
    lamp_a /= np.interp(560, WL, lamp_a)
    filt = json.loads((lab / "profiles/printer_filters.json").read_text())["filters"]["wratten_2b"]
    d2b = np.interp(WL, filt["x"], filt["y"], left=np.nan, right=0.0)
    t2b = np.where(np.isfinite(d2b), 10.0 ** -np.nan_to_num(d2b, nan=0.0), 0.0)
    t2b[WL < filt["x"][0]] = 0.0
    neg_t = 10.0 ** -np.where(np.isfinite(mid), mid, np.nan)

    fig, axes = plt.subplots(1, 2, figsize=(10.5, 3.5))
    axes[0].plot(WL, lamp_a, color="#e67e22", lw=1.4, label="lamp A (2856 K)")
    axes[0].plot(WL, lamp_a * t2b, color="#7f4f24", lw=1.6, label="lamp A + Wratten 2B")
    ax2 = axes[0].twinx()
    ax2.plot(WL, neg_t, color="0.3", lw=1.3, ls="--", label="5207 midscale neutral, transmittance")
    ax2.set_ylabel("negative transmittance")
    ax2.grid(False)
    axes[0].set_xlabel("wavelength (nm)")
    axes[0].set_ylabel("relative power")
    axes[0].set_title("Printer light and the negative")
    h1, l1 = axes[0].get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    axes[0].legend(h1 + h2, l1 + l2, fontsize=7, loc="upper left")
    axes[0].set_xlim(360, 760)

    for name, ls in (("kodak_2383", "-"), ("kodak_endura_premier", "--")):
        for layer, s in sensitivities(profile(lab, name)).items():
            axes[1].plot(WL, s, color=LAYER_COLOURS[layer], lw=1.5, ls=ls,
                         label=f"{'2383' if name == 'kodak_2383' else 'Endura Premier'} {layer}")
    axes[1].axvspan(360, 400, color="#8e44ad", alpha=0.10)
    axes[1].text(363, 0.9, "UV band:\nflat\nexposure", fontsize=7, color="#6c3483")
    axes[1].set_xlabel("wavelength (nm)")
    axes[1].set_ylabel("relative sensitivity (peak = 1)")
    axes[1].set_title("Print material layer sensitivities")
    axes[1].legend(fontsize=6.5, ncol=2)
    axes[1].set_xlim(360, 760)
    fig.tight_layout()
    fig.savefig(OUT / "printer_light_and_print_sensitivity.png", bbox_inches="tight")
    plt.close(fig)


def fig_scanner(lab: Path) -> None:
    neg = profile(lab, "kodak_portra_400")
    cine = profile(lab, "kodak_5207")["charts"]["spectral_dye_density"]
    lamp = planck(WL, 3200.0)
    fig, ax = plt.subplots(figsize=(7.5, 3.6))
    for key, col in DYE_COLOURS.items():
        _, y = curve(cine, key, WL)
        ax.plot(WL, y, color=col, lw=1.4, label=f"5207 {key.replace('_', ' ')}")
    _, dmin = curve(cine, "minimum_density", WL)
    ax.plot(WL, dmin, color="#e67e22", lw=1.8, label="5207 orange base")
    ax2 = ax.twinx()
    for centre, layer in ((650, "red"), (545, "green"), (450, "blue")):
        sigma = 40.0 / 2.3548
        band = np.exp(-0.5 * ((WL - centre) / sigma) ** 2)
        band[band < 1e-4] = 0.0
        w = band * lamp
        ax2.fill_between(WL, 0, w / w.max(), color=LAYER_COLOURS[layer], alpha=0.18)
        ax2.plot(WL, w / w.max(), color=LAYER_COLOURS[layer], lw=1.0)
    ax2.set_ylabel("scanner channel weight L S_i (peak = 1)")
    ax2.grid(False)
    ax.set_xlabel("wavelength (nm)")
    ax.set_ylabel("spectral density")
    ax.set_xlim(380, 760)
    ax.set_title("Virtual scanner of Negative Scan: Gaussian bands 650/545/450 nm, 40 nm FWHM, halogen 3200 K")
    ax.legend(fontsize=7, loc="upper center", bbox_to_anchor=(0.5, -0.18), ncol=4)
    fig.tight_layout()
    fig.savefig(OUT / "negative_scan_scanner_bands.png", bbox_inches="tight")
    plt.close(fig)
    del neg


def fig_inversion(lab: Path) -> None:
    p = profile(lab, "kodak_5207")
    ch = p["charts"]["characteristic"]
    gamma = {"red": 0.478, "green": 0.546, "blue": 0.528}  # P14: cine medium
    grey = np.log10(12.5 / 250.0)  # 12.5 / ISO lux s for a 250 speed stock
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 3.5))
    stops = np.linspace(-6, 6, 241)
    for layer in ("red", "green", "blue"):
        x, d = curve(ch, layer)
        d_base = d.min()
        d_aim = np.interp(grey, x, d)
        axes[0].plot(x - grey, d - d_base, color=LAYER_COLOURS[layer], lw=1.6, label=f"{layer}: density above base")
        axes[0].plot(x - grey, d_aim - d_base + gamma[layer] * (x - grey), color=LAYER_COLOURS[layer], lw=0.9, ls=":")
        dd = np.interp(grey + stops * np.log10(2), x, d)
        e = 0.18 * 10 ** ((dd - d_aim) / gamma[layer])
        axes[1].plot(stops, np.log2(e / 0.18), color=LAYER_COLOURS[layer], lw=1.6, label=layer)
    axes[0].set_xlabel("log10 exposure relative to the 18 % grey")
    axes[0].set_ylabel("Status M density above base")
    axes[0].set_title("The negative and the straight inversion line (dotted, one gamma per medium)")
    axes[0].legend(fontsize=7)
    axes[1].plot(stops, stops, color="0.6", lw=1.0, ls="--", label="identity")
    axes[1].set_xlabel("scene stops relative to grey")
    axes[1].set_ylabel("positive, stops relative to 0.18")
    axes[1].set_title("Positive after inversion: toe and shoulder of the stock stay")
    axes[1].legend(fontsize=7)
    fig.suptitle("Illustration of rule generic_density_inversion on the published 5207 Status M curves (illustration)", y=1.03)
    fig.tight_layout()
    fig.savefig(OUT / "negative_scan_inversion.png", bbox_inches="tight")
    plt.close(fig)


def daylight(components: np.ndarray, kelvin: float) -> np.ndarray:
    """CIE daylight from the S0, S1, S2 table (CIE 15)."""
    t = float(kelvin)
    if t <= 7000:
        xd = -4.6070e9 / t ** 3 + 2.9678e6 / t ** 2 + 0.09911e3 / t + 0.244063
    else:
        xd = -2.0064e9 / t ** 3 + 1.9018e6 / t ** 2 + 0.24748e3 / t + 0.237040
    yd = -3.000 * xd ** 2 + 2.870 * xd - 0.275
    m = 0.0241 + 0.2562 * xd - 0.7341 * yd
    m1 = (-1.3515 - 1.7703 * xd + 5.9114 * yd) / m
    m2 = (0.0300 - 31.4424 * xd + 30.0717 * yd) / m
    s = components[:, 1] + m1 * components[:, 2] + m2 * components[:, 3]
    return np.interp(WL, components[:, 0], s)


def fig_scene_lights(lab: Path) -> None:
    d = cie(lab, "CIE_illum_Dxx_comp.csv")
    fig, axes = plt.subplots(1, 2, figsize=(10.5, 3.4))
    for k, c in ((2000, "#b03a2e"), (2500, "#d35400"), (3200, "#e67e22"), (3800, "#f0b27a")):
        axes[0].plot(WL, planck(WL, k), color=c, lw=1.5, label=f"Planck {k} K")
    for kelvin, c in ((5000, "#7f8c8d"), (6500, "#5dade2"), (10000, "#1f4e9c")):
        s = daylight(d, kelvin)
        axes[0].plot(WL, s / np.interp(560, WL, s), color=c, lw=1.5, label=f"CIE daylight {kelvin} K")
    axes[0].set_ylim(0, 3.2)
    axes[0].set_xlabel("wavelength (nm)")
    axes[0].set_ylabel("relative power (560 nm = 1)")
    axes[0].set_title("Scene light spectra")
    axes[0].legend(fontsize=7, ncol=2)
    mired = np.append(np.arange(500.0, 69.9, -5.0), 1e6 / 15000.0)  # 5 mired from 500 to 70, and 15000 K
    axes[1].plot(1e6 / mired, np.zeros_like(mired), "|", ms=14, color="#2e6fd8")
    axes[1].set_xscale("log")
    axes[1].set_yticks([])
    axes[1].set_xlabel("colour temperature (K), log axis")
    axes[1].set_title(f"As Shot grid: {mired.size} lights, 5 mired apart; Planckian to 4000 K, daylight from 5000 K")
    axes[1].grid(False)
    fig.tight_layout()
    fig.savefig(OUT / "scene_lights_and_as_shot_grid.png", bbox_inches="tight")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lab", type=Path, required=True)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    style()
    fig_sensitivity_vs_cmf(args.lab)
    fig_stock_data(args.lab)
    fig_inhibition_edge()
    fig_printer_light(args.lab)
    fig_scanner(args.lab)
    fig_inversion(args.lab)
    fig_scene_lights(args.lab)
    for f in sorted(OUT.glob("*.png")):
        print(f.name)


if __name__ == "__main__":
    main()
