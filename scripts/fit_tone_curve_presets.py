#!/usr/bin/env python3
# Copyright 2026 Yurun Zi
# SPDX-License-Identifier: GPL-3.0-only
# Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Fit the control points of the Tone Curve presets (issue #891).

Evidence script of docs/roadmap/alcedo_studio/edit/tone_curve_preset_study.md. For each preset
it builds the source tone, the fit target on the curve axis, and at most 16 control points of the
product spline, and it reports the fit error.

Sources:
  - Film-like: the ACR3 default tone curve of the DNG SDK, the 1025-entry table that RawTherapee
    uses for an Adobe profile without a tone curve (rtengine/dcp.cc). Pass the file with
    --acr-source; without it the script downloads the pinned commit below.
  - Filmic: darktable filmic rgb (src/iop/filmicrgb.c, pinned commit below) with its default
    parameters, ported here (spline version 3, fourth-order toe and shoulder).
  - Kodak 2383, Velvia 50, Ektachrome E100: the neutral tone of the film print point stages. The
    script runs ToneCurvePresetFilmSourceTest (--film-test) and reads its lines.
  - Soft, Medium, and Strong contrast: S curves around 18% grey on the curve axis.

Film-like and Filmic get a highlight shoulder (issue #899): their sources reach display white
about 4 to 5 stops above 18% grey and stay there. Above a knee one stop below that white the fit
target rolls off to the display peak at x = 1; the points of the fit without the shoulder stay up
to the point next to the knee.

Film-like and Filmic also get a shadow toe that ends at (0, 0) (issue #910): their sources keep a
display black well above 0 at x = 0, which lifts the black point of the default look. Below a knee
three stops under 18% grey the fit target falls to (0, 0) along a parabola; the points of the fit
with the shoulder stay from the point after the knee point.

Usage:
  python -I scripts/fit_tone_curve_presets.py \
      --film-test build/debug/.../ToneCurvePresetFilmSourceTest.exe [--acr-source dcp.cc] [--write]

With --write it writes the preset points (alcedo_studio/src/edit/operators/models/
tone_curve_preset_points.inc), the study data of the tests (alcedo_studio/tests/edit/runtime/
tone_curve_preset_study_data.hpp), and the figures of the study document. Needs numpy and scipy.
"""
from __future__ import annotations

import argparse
import math
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

import numpy as np
from scipy.optimize import minimize

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import survey_dcp_tone_curves as aces  # noqa: E402

RAWTHERAPEE_COMMIT = "64c2d90a3248c7ad41842d9162368858b61aac6a"
RAWTHERAPEE_DCP_URL = (
    f"https://raw.githubusercontent.com/RawTherapee/RawTherapee/{RAWTHERAPEE_COMMIT}/rtengine/dcp.cc")
DARKTABLE_COMMIT = "f30f439fd801eef58047bc417644e66939b3f3b7"

# Product Default Color Grade exposure (kDefaultPipelineExposureEv): Alcedo +1.5 EV is ACR 0 EV
# (dcp_profile_tone_curve_study.md, section 3.3).
ACR_EXPOSURE_ANCHOR_EV = 1.5
# The curve axis is ACEScc. 18% grey and one stop on it (curve::kCurveAxisGrey, kCurveAxisStop).
AXIS_GREY = (math.log2(0.18) + 9.72) / 17.52
AXIS_STOP = 1.0 / 17.52
MAX_POINTS = 16
MIN_SPACING = 0.02           # about 1/3 stop between handles
FIT_GOAL_CODES = 0.5         # stop adding points below this error
DIGITS = 5                   # decimals of the stored points
DENSE = 4097                 # fit grid on the curve axis
STUDY_SAMPLES = 257          # samples of the study data in the tests
FILM_FIRST_EV, FILM_STEP_EV, FILM_COUNT = -12.0, 0.0625, 385
# Contrast shapes: slope at 18% grey on the curve axis, and the width of the S in stops.
CONTRAST_SHAPES = {"soft_contrast": 1.15, "medium_contrast": 1.3, "strong_contrast": 1.5}
CONTRAST_WIDTH_STOPS = 3.0
# Highlight shoulder (#899): presets whose source reaches display white inside the curve axis and
# stays there. Above a knee one stop below that white the target rolls off toward the display peak
# at x = 1, as the shoulder of the DCP profile tone stage (knee one stop below raw white).
SHOULDER_PRESETS = ("film_like", "filmic")
SHOULDER_KNEE_STOPS = 1.0
SHOULDER_SLOPE_STEP = 2e-3   # central difference of the knee slope on the curve axis
SHOULDER_REFIT_BELOW_KNEE = 1  # source points next to the knee that move with the shoulder
SHOULDER_TABLE_EVS = (2.0, 3.0, 3.5, 4.0, 4.5, 5.0, 6.0, 8.0, 10.0)
# Shadow toe (#910): presets whose curve keeps a raised output at x = 0. Below a knee three stops
# under 18% grey the target falls to (0, 0) along a parabola with value and slope continuous at
# the knee.
TOE_PRESETS = ("film_like", "filmic")
TOE_KNEE_STOPS = 3.0
TOE_REFIT_ABOVE_KNEE = 1     # points of the earlier fit next to the knee that move with the toe
TOE_TABLE_EVS = (-7.0, -6.0, -5.0, -4.0, -3.0, -2.0)

PRESETS = [
    # id, display name, group, source kind
    ("linear", "Linear", "", "identity"),
    ("soft_contrast", "Soft Contrast", "Contrast", "contrast"),
    ("medium_contrast", "Medium Contrast", "Contrast", "contrast"),
    ("strong_contrast", "Strong Contrast", "Contrast", "contrast"),
    ("film_like", "Film-like", "Rendering", "acr"),
    ("filmic", "Filmic", "Rendering", "filmic"),
    ("kodak_2383", "Kodak 2383", "Film", "film"),
    ("fuji_velvia_50", "Velvia 50", "Film", "film"),
    ("kodak_ektachrome_e100", "Ektachrome E100", "Film", "film"),
]

ACES_PARAMS = aces.aces2_params(100.0)


# ---------------------------------------------------------------------------------------------
# Axis, display transform, and the product spline


def acescc_from_linear(v):
    v = np.asarray(v, dtype=float)
    tiny = 2.0 ** -16
    out = np.where(v <= 0.0, (math.log2(tiny) + 9.72) / 17.52,
                   np.where(v < 2.0 ** -15, (np.log2(tiny + np.maximum(v, 0) * 0.5) + 9.72) / 17.52,
                            (np.log2(np.maximum(v, 1e-30)) + 9.72) / 17.52))
    return out


def linear_from_acescc(c):
    c = np.asarray(c, dtype=float)
    low = (9.72 - 15.0) / 17.52
    high = (math.log2(65504.0) + 9.72) / 17.52
    return np.where(c < low, (2.0 ** (c * 17.52 - 9.72) - 2.0 ** -16) * 2.0,
                    np.where(c < high, 2.0 ** (c * 17.52 - 9.72), 65504.0))


def neutral_response(scene):
    """N: ACES 2.0 SDR (100 nits) display light of an AP1 neutral, relative to the peak
    (survey_dcp_tone_curves.aces2_forward on arrays)."""
    p = ACES_PARAMS
    x = np.maximum(np.asarray(scene, dtype=float), 0.0)
    f = p["m_2"] * (x / (x + p["s_2"])) ** p["g"]
    return np.maximum(0.0, f * f / (f + p["t_1"])) * 100.0 / p["peak"]


SCENE_AT_PEAK = aces.aces2_inverse(1.0 - 1e-12, ACES_PARAMS)


def neutral_inverse(display):
    """N^-1 (survey_dcp_tone_curves.aces2_inverse on arrays); display light 1 and above give the
    scene value at the peak."""
    p = ACES_PARAMS
    d = np.clip(np.asarray(display, dtype=float), 0.0, 1.0 - 1e-12)
    h = d * p["peak"] / 100.0
    f = 0.5 * (h + np.sqrt(h * h + 4.0 * p["t_1"] * h))
    r = (f / p["m_2"]) ** (1.0 / p["g"])
    scene = p["s_2"] * r / (1.0 - r)
    return np.where(np.asarray(display) >= 1.0, SCENE_AT_PEAK, scene)


def solve_spline(points):
    """tone_curve::SolveToneCurveSpline in float32."""
    x = np.array([p[0] for p in points], dtype=np.float32)
    y = np.array([p[1] for p in points], dtype=np.float32)
    n = len(x)
    width = x[1:] - x[:-1]
    secant = (y[1:] - y[:-1]) / width
    slope = np.zeros(n, dtype=np.float32)
    slope[0] = secant[0]
    slope[-1] = secant[-1]
    for i in range(1, n - 1):
        if secant[i - 1] * secant[i] <= 0:
            slope[i] = 0
            continue
        w1 = np.float32(2) * width[i] + width[i - 1]
        w2 = width[i] + np.float32(2) * width[i - 1]
        slope[i] = (w1 + w2) / (w1 / secant[i - 1] + w2 / secant[i])
    return x, y, slope


def evaluate_spline(spline, xs):
    """tone_curve::EvaluateToneCurveSpline (float32 operations)."""
    x, y, m = spline
    xs = np.asarray(xs, dtype=np.float32)
    n = len(x)
    out = np.empty_like(xs)
    low = xs <= x[0]
    high = xs >= x[-1]
    out[low] = y[0] + m[0] * (xs[low] - x[0])
    out[high] = y[-1] + m[-1] * (xs[high] - x[-1])
    mid = ~(low | high)
    idx = np.clip(np.searchsorted(x, xs[mid], side="right") - 1, 0, n - 2)
    h = x[idx + 1] - x[idx]
    t = (xs[mid] - x[idx]) / h
    dy = y[idx + 1] - y[idx]
    d = dy / h
    u = np.float32(1) - t
    out[mid] = y[idx] + t * dy + h * t * u * (u * (m[idx] - d) - t * (m[idx + 1] - d))
    return out


def code(display):
    """8-bit code of display light on a gamma 2.2 display."""
    return 255.0 * np.clip(display, 0.0, 1.0) ** (1.0 / 2.2)


# ---------------------------------------------------------------------------------------------
# Sources: display light (white 1) of a scene value on the scale of each source


def load_acr_table(path: Path | None) -> np.ndarray:
    if path is None:
        cache = ROOT / "build" / "tmp" / "tone_curve_presets" / "dcp.cc"
        if not cache.exists():
            cache.parent.mkdir(parents=True, exist_ok=True)
            with urllib.request.urlopen(RAWTHERAPEE_DCP_URL) as response:
                cache.write_bytes(response.read())
        path = cache
    text = path.read_text(encoding="utf-8")
    start = text.index("adobe_camera_raw_default_curve[] = {")
    body = text[start:text.index("};", start)]
    values = np.array([float(v) for v in re.findall(r"([0-9.]+)f", body)])
    if len(values) != 1025:
        raise SystemExit(f"ACR3 table has {len(values)} entries, expected 1025")
    return values


def acr_source(table):
    grid = np.linspace(0.0, 1.0, len(table))

    def tone(scene):
        # dng_tone_curve_acr3_default: the table on [0, 1], linear between entries; the exposure
        # ramp clips at white.
        raw = np.clip(np.asarray(scene) * 2.0 ** -ACR_EXPOSURE_ANCHOR_EV, 0.0, 1.0)
        return np.interp(raw, grid, table)
    return tone


def filmic_source():
    """darktable filmic rgb v6 defaults: grey 18.45 %, black -8 EV, white +4 EV, contrast 1,
    latitude 0.01 %, balance 0, hardness 4, target black 0.01517634 %, white 100 %,
    fourth-order toe and shoulder, spline version 3."""
    grey_source, black_ev, white_ev = 0.1845, -8.0, 4.0
    hardness, contrast_param, latitude = 4.0, 1.0, 0.01 / 100.0
    safety = 0.01
    grey_display = 0.1845 ** (1.0 / hardness)
    dynamic_range = white_ev - black_ev
    black_log, grey_log, white_log = 0.0, abs(black_ev) / dynamic_range, 1.0
    black_display = (0.01517634 / 100.0) ** (1.0 / hardness)
    white_display = (100.0 / 100.0) ** (1.0 / hardness)
    slope = contrast_param * dynamic_range / 8.0
    min_contrast = max(1.0, (white_display - grey_display) / (white_log - grey_log),
                       (grey_display - black_display) / (grey_log - black_log)) + safety
    contrast = slope / (hardness * grey_display ** (hardness - 1.0))
    contrast = min(max(contrast, min_contrast), 100.0)
    intercept = grey_display - contrast * grey_log
    xmin = (black_display + safety * (white_display - black_display) - intercept) / contrast
    xmax = (white_display - safety * (white_display - black_display) - intercept) / contrast
    toe_log = max((1.0 - latitude) * grey_log + latitude * xmin, xmin)
    shoulder_log = min((1.0 - latitude) * grey_log + latitude * xmax, xmax)
    toe_display = toe_log * contrast + intercept
    shoulder_display = shoulder_log * contrast + intercept
    tl, sl = toe_log, shoulder_log
    toe = np.linalg.solve(np.array([
        [0, 0, 0, 0, 1], [0, 0, 0, 1, 0], [tl ** 4, tl ** 3, tl ** 2, tl, 1],
        [4 * tl ** 3, 3 * tl ** 2, 2 * tl, 1, 0], [12 * tl ** 2, 6 * tl, 2, 0, 0]]),
        np.array([black_display, 0, toe_display, contrast, 0]))
    shoulder = np.linalg.solve(np.array([
        [1, 1, 1, 1, 1], [4, 3, 2, 1, 0], [sl ** 4, sl ** 3, sl ** 2, sl, 1],
        [4 * sl ** 3, 3 * sl ** 2, 2 * sl, 1, 0], [12 * sl ** 2, 6 * sl, 2, 0, 0]]),
        np.array([white_display, 0, shoulder_display, contrast, 0]))

    def tone(scene):
        scene = np.maximum(np.asarray(scene, dtype=float), 1e-30)
        x = np.clip((np.log2(scene / grey_source) - black_ev) / dynamic_range, 0.0, 1.0)
        y = np.where(x < toe_log, np.polyval(toe, x),
                     np.where(x > shoulder_log, np.polyval(shoulder, x),
                              intercept + contrast * x))
        return np.clip(y, 0.0, 1.0) ** hardness
    return tone


def read_film_tones(test: Path) -> dict[str, np.ndarray]:
    result = subprocess.run([str(test), "--gtest_filter=ToneCurvePresetFilmSourceTest.*"],
                            capture_output=True, text=True, check=False)
    tones: dict[str, list[tuple[float, float]]] = {}
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[0] == "tone_curve_film_tone":
            tones.setdefault(parts[1], []).append((float(parts[2]), float(parts[3])))
    if not tones:
        raise SystemExit("ToneCurvePresetFilmSourceTest printed no film tone; are the film print "
                         "resources configured?\n" + result.stdout[-2000:])
    out = {}
    for preset_id, samples in tones.items():
        samples.sort()
        if len(samples) != FILM_COUNT:
            raise SystemExit(f"{preset_id}: {len(samples)} film samples, expected {FILM_COUNT}")
        out[preset_id] = np.array([s[1] for s in samples])
    return out


def film_source(samples: np.ndarray):
    evs = FILM_FIRST_EV + FILM_STEP_EV * np.arange(FILM_COUNT)
    log_tone = np.log2(np.maximum(samples, 1e-12))

    def tone(scene):
        ev = np.log2(np.maximum(np.asarray(scene, dtype=float), 1e-30) / 0.18)
        return 2.0 ** np.interp(ev, evs, log_tone)
    return tone


def grey_exposure(tone) -> float:
    """k: the exposure scale at which the source renders 18% grey as N does (18% grey fixed)."""
    target = float(neutral_response(0.18))
    low, high = -12.0, 12.0
    for _ in range(200):
        mid = 0.5 * (low + high)
        if float(tone(0.18 * 2.0 ** mid)) < target:
            low = mid
        else:
            high = mid
    return 0.5 * (low + high)


def contrast_shape(slope: float):
    """S around 18% grey on the curve axis: y = x + (s - 1) A tanh((x - g) / A)."""
    width = CONTRAST_WIDTH_STOPS * AXIS_STOP

    def curve(x):
        u = np.asarray(x, dtype=float) - AXIS_GREY
        return AXIS_GREY + u + (slope - 1.0) * width * np.tanh(u / width)
    return curve


# ---------------------------------------------------------------------------------------------
# Fit


class Target:
    """Fit target of one preset: the curve y_t(x) on the axis and the display light T(x)."""

    def __init__(self, axis_curve, display):
        self.axis_curve = axis_curve      # x -> y_t on the curve axis (may leave [0, 1])
        self.display = display            # x -> display light that the source gives


def target_from_source(tone, ev_shift: float) -> Target:
    scale = 2.0 ** ev_shift

    def display(x):
        return np.clip(tone(scale * linear_from_acescc(x)), 0.0, 1.0)

    def axis_curve(x):
        return acescc_from_linear(neutral_inverse(display(x)))
    return Target(axis_curve, display)


def target_from_axis(curve) -> Target:
    def display(x):
        return neutral_response(np.maximum(linear_from_acescc(curve(x)), 0.0))
    return Target(curve, display)


# Curve output at which N reaches the display peak: the end point of a shoulder.
AXIS_AT_PEAK = float(acescc_from_linear(np.array([SCENE_AT_PEAK]))[0])


class Shoulder:
    """Highlight shoulder of a target on the curve axis.

    Up to the knee x_k the target stays. Above it the curve is the rational roll-off
    y = y_k + D t / (1 + t), t = m_k (x - x_k) / D: value and slope m_k agree at the knee, the
    slope falls as m_k / (1 + t)^2, and D is chosen so that x = 1 maps to the display peak."""

    def __init__(self, target: Target):
        grid = np.linspace(0.0, 1.0, DENSE)
        white = np.nonzero(target.display(grid) >= 1.0)[0]
        if len(white) == 0 or white[0] == 0:
            raise SystemExit("shoulder: the source does not reach display white on the axis")
        low, high = float(grid[white[0] - 1]), float(grid[white[0]])
        for _ in range(80):
            mid = 0.5 * (low + high)
            if float(target.display(np.array([mid]))[0]) >= 1.0:
                high = mid
            else:
                low = mid
        self.white_x = high
        self.knee_x = self.white_x - SHOULDER_KNEE_STOPS * AXIS_STOP
        step = SHOULDER_SLOPE_STEP
        self.knee_y = float(target.axis_curve(np.array([self.knee_x]))[0])
        self.knee_slope = float((target.axis_curve(np.array([self.knee_x + step]))[0] -
                                 target.axis_curve(np.array([self.knee_x - step]))[0]) / (2 * step))
        width, rise = 1.0 - self.knee_x, AXIS_AT_PEAK - self.knee_y
        if self.knee_slope * width <= rise:
            raise SystemExit("shoulder: the knee slope cannot roll off to the peak")
        self.scale = rise * self.knee_slope * width / (self.knee_slope * width - rise)
        self.end_slope = self.knee_slope / (1.0 + self.knee_slope * width / self.scale) ** 2
        self.source = target

    def axis_curve(self, x):
        x = np.asarray(x, dtype=float)
        t = self.knee_slope * np.maximum(x - self.knee_x, 0.0) / self.scale
        return np.where(x <= self.knee_x, self.source.axis_curve(x),
                        self.knee_y + self.scale * t / (1.0 + t))

    def display(self, x):
        x = np.asarray(x, dtype=float)
        rolled = neutral_response(np.maximum(linear_from_acescc(self.axis_curve(x)), 0.0))
        return np.where(x <= self.knee_x, self.source.display(x), rolled)

    def target(self) -> Target:
        return Target(self.axis_curve, self.display)


class Toe:
    """Shadow toe of a target on the curve axis (#910).

    From the knee x_k down the curve is the parabola y = y_k - m_k u - c u^2, u = x_k - x, with
    c = (y_k - m_k x_k) / x_k^2: value and slope m_k agree at the knee and x = 0 maps to 0. The
    slope at x = 0 is m_k + 2 c x_k; the toe is monotone while it is not negative."""

    def __init__(self, target: Target):
        self.knee_x = AXIS_GREY - TOE_KNEE_STOPS * AXIS_STOP
        step = SHOULDER_SLOPE_STEP
        self.knee_y = float(target.axis_curve(np.array([self.knee_x]))[0])
        self.knee_slope = float((target.axis_curve(np.array([self.knee_x + step]))[0] -
                                 target.axis_curve(np.array([self.knee_x - step]))[0]) / (2 * step))
        self.curvature = (self.knee_y - self.knee_slope * self.knee_x) / self.knee_x ** 2
        self.start_slope = self.knee_slope + 2.0 * self.curvature * self.knee_x
        if self.knee_y <= 0.0 or self.start_slope < 0.0:
            raise SystemExit("toe: the knee cannot fall monotonically to (0, 0)")
        self.source = target

    def axis_curve(self, x):
        x = np.asarray(x, dtype=float)
        u = np.maximum(self.knee_x - x, 0.0)
        return np.where(x >= self.knee_x, self.source.axis_curve(x),
                        self.knee_y - self.knee_slope * u - self.curvature * u * u)

    def display(self, x):
        x = np.asarray(x, dtype=float)
        toe = neutral_response(np.maximum(linear_from_acescc(self.axis_curve(x)), 0.0))
        return np.where(x >= self.knee_x, self.source.display(x), toe)

    def target(self) -> Target:
        return Target(self.axis_curve, self.display)


class Fit:
    def __init__(self, target: Target, fixed_below: float = -1.0, fixed_above: float = 2.0):
        self.target = target
        # Points at or below fixed_below and at or above fixed_above stay where they are; new
        # points go between them.
        self.fixed_below = fixed_below
        self.fixed_above = fixed_above
        self.grid = np.linspace(0.0, 1.0, DENSE)
        self.target_code = code(target.display(self.grid))
        self.target_curve = target.axis_curve(self.grid)
        # End points: (0, y_t(0)) and (1, y_t(1)), or where y_t leaves [0, 1].
        self.x_low = self.crossing(0.0) if self.target_curve[0] < 0.0 else 0.0
        self.x_high = self.crossing(1.0) if self.target_curve[-1] > 1.0 else 1.0

    def crossing(self, level: float) -> float:
        low, high = 0.0, 1.0
        rising = self.target.axis_curve(np.array([1.0]))[0] > self.target.axis_curve(np.array([0.0]))[0]
        for _ in range(80):
            mid = 0.5 * (low + high)
            above = self.target.axis_curve(np.array([mid]))[0] > level
            if above == rising:
                high = mid
            else:
                low = mid
        return 0.5 * (low + high)

    def y_at(self, x):
        return np.clip(self.target.axis_curve(np.asarray(x, dtype=float)), 0.0, 1.0)

    def rendered(self, points, xs):
        values = evaluate_spline(solve_spline(points), xs).astype(float)
        return neutral_response(np.maximum(linear_from_acescc(values), 0.0))

    def error(self, points) -> float:
        return float(np.max(np.abs(code(self.rendered(points, self.grid)) - self.target_code)))

    def start(self):
        xs = [self.x_low, AXIS_GREY, self.x_high]
        return [(x, AXIS_GREY if x == AXIS_GREY else float(self.y_at(x))) for x in xs]

    def insert_worst(self, points):
        errors = np.abs(code(self.rendered(points, self.grid)) - self.target_code)
        for index in np.argsort(-errors):
            x = float(self.grid[index])
            if all(abs(x - p[0]) >= MIN_SPACING for p in points) and \
                    max(self.x_low, self.fixed_below) < x < min(self.x_high, self.fixed_above):
                points = sorted(points + [(x, float(self.y_at(x)))])
                return points
        return points

    def refine(self, points):
        """Move the inner points (not grey) to lower the largest code error."""
        grey = next(i for i, p in enumerate(points) if abs(p[0] - AXIS_GREY) < 1e-5)
        fixed = {0, len(points) - 1, grey}
        fixed |= {i for i, p in enumerate(points)
                  if p[0] <= self.fixed_below or p[0] >= self.fixed_above}
        free = [i for i in range(len(points)) if i not in fixed]
        if not free:
            return points

        def unpack(vector):
            out = list(points)
            for k, i in enumerate(free):
                out[i] = (vector[2 * k], vector[2 * k + 1])
            return out

        def objective(vector):
            candidate = unpack(vector)
            xs = [p[0] for p in candidate]
            ys = [p[1] for p in candidate]
            penalty = 0.0
            for a, b in zip(xs, xs[1:]):
                penalty += max(0.0, MIN_SPACING - (b - a)) * 1e4
            for a, b in zip(ys, ys[1:]):
                penalty += max(0.0, a - b) * 1e4
            penalty += sum(max(0.0, -y) + max(0.0, y - 1.0) for y in ys) * 1e4
            if penalty > 0.0:
                return 1e3 + penalty
            errors = np.abs(code(self.rendered(candidate, self.grid)) - self.target_code)
            # A smooth form of the largest error.
            return float(np.max(errors) + 0.05 * np.sqrt(np.mean(errors ** 2)))

        start = np.array([v for i in free for v in points[i]])
        best = minimize(objective, start, method="Nelder-Mead",
                        options={"maxiter": 4000 * len(start), "xatol": 1e-6, "fatol": 1e-5})
        candidate = unpack(best.x)
        return candidate if self.error(candidate) <= self.error(points) else points


def rounded(points):
    out = []
    for x, y in points:
        if x == AXIS_GREY:
            out.append((round(AXIS_GREY, DIGITS), round(AXIS_GREY, DIGITS)))
        else:
            out.append((round(x, DIGITS), round(y, DIGITS)))
    return out


def fit_points(fit: Fit, points):
    best = None
    while True:
        refined = rounded(fit.refine(points))
        error = fit.error(refined)
        if best is None or error < best[1] - 1e-9:
            best = (refined, error)
        if error <= FIT_GOAL_CODES or len(points) >= MAX_POINTS:
            break
        grown = fit.insert_worst(fit.refine(points))
        if len(grown) == len(points):
            break
        points = grown
    return best


def fit_preset(target: Target):
    fit = Fit(target)
    points, error = fit_points(fit, fit.start())
    return fit, points, error


def fit_shoulder(shoulder: Shoulder, source_points):
    """Keep the points of the source fit below the knee and fit the shoulder above it.

    The point next to the knee moves with the shoulder points: its spline slope follows its right
    neighbour, so with the shoulder in place of the steep top it would leave an error of about
    0.9 codes in the stop below it."""
    kept = [p for p in source_points if p[0] <= shoulder.knee_x]
    fixed_below = kept[-1 - SHOULDER_REFIT_BELOW_KNEE][0]
    fit = Fit(shoulder.target(), fixed_below=fixed_below)
    points, error = fit_points(fit, kept + [(1.0, float(fit.y_at(1.0)))])
    return fit, points, error, fixed_below


def fit_toe(toe: Toe, source_points):
    """Keep the points of the source fit above the knee and fit the toe below it, from (0, 0).

    As with the shoulder, the point next to the knee moves with the toe points."""
    above = [p for p in source_points if p[0] >= toe.knee_x]
    fixed_above = above[TOE_REFIT_ABOVE_KNEE][0]
    fit = Fit(toe.target(), fixed_above=fixed_above)
    points, error = fit_points(fit, [(0.0, 0.0)] + above)
    return fit, points, error, fixed_above


def smallest_slope_above(points, x_from: float) -> float:
    """Smallest slope of the product spline on [x_from, 1], from steps of 1/4096."""
    xs = np.linspace(x_from, 1.0, max(2, int(round((1.0 - x_from) * 4096)) + 1))
    ys = evaluate_spline(solve_spline(points), xs).astype(float)
    return float(np.min(np.diff(ys) / np.diff(xs)))


# ---------------------------------------------------------------------------------------------
# Output


def svg_plot(path: Path, title: str, x_label: str, y_label: str, series, x_range, y_range,
             points=None, x_ticks=(), y_ticks=()):
    width, height, left, right, top, bottom = 520, 360, 58, 16, 30, 44
    plot_w, plot_h = width - left - right, height - top - bottom

    def px(x):
        return left + (x - x_range[0]) / (x_range[1] - x_range[0]) * plot_w

    def py(y):
        return top + plot_h - (y - y_range[0]) / (y_range[1] - y_range[0]) * plot_h

    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
             f'viewBox="0 0 {width} {height}" font-family="Helvetica, Arial, sans-serif" '
             f'font-size="11">',
             f'<rect width="{width}" height="{height}" fill="white"/>',
             f'<text x="{left}" y="18" font-size="13" font-weight="bold">{title}</text>',
             f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="none" '
             f'stroke="#999"/>']
    for value, label in x_ticks:
        lines.append(f'<line x1="{px(value):.1f}" y1="{top}" x2="{px(value):.1f}" '
                     f'y2="{top + plot_h}" stroke="#e4e4e4"/>')
        lines.append(f'<text x="{px(value):.1f}" y="{top + plot_h + 14}" '
                     f'text-anchor="middle">{label}</text>')
    for value, label in y_ticks:
        lines.append(f'<line x1="{left}" y1="{py(value):.1f}" x2="{left + plot_w}" '
                     f'y2="{py(value):.1f}" stroke="#e4e4e4"/>')
        lines.append(f'<text x="{left - 6}" y="{py(value) + 4:.1f}" text-anchor="end">{label}</text>')
    lines.append(f'<text x="{left + plot_w / 2}" y="{height - 8}" text-anchor="middle">{x_label}</text>')
    lines.append(f'<text x="14" y="{top + plot_h / 2}" text-anchor="middle" '
                 f'transform="rotate(-90 14 {top + plot_h / 2})">{y_label}</text>')
    legend_y = top + 14
    for xs, ys, color, dash, label in series:
        keep = (xs >= x_range[0]) & (xs <= x_range[1])
        coords = " ".join(f"{px(x):.1f},{py(min(max(y, y_range[0]), y_range[1])):.1f}"
                          for x, y in zip(xs[keep], ys[keep]))
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        lines.append(f'<polyline points="{coords}" fill="none" stroke="{color}" '
                     f'stroke-width="1.6"{dash_attr}/>')
        lines.append(f'<line x1="{left + 10}" y1="{legend_y}" x2="{left + 30}" y2="{legend_y}" '
                     f'stroke="{color}" stroke-width="1.6"{dash_attr}/>')
        lines.append(f'<text x="{left + 36}" y="{legend_y + 4}">{label}</text>')
        legend_y += 15
    for x, y in points or []:
        lines.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="3" fill="white" '
                     f'stroke="#222" stroke-width="1.2"/>')
    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_figures(out: Path, preset_id: str, name: str, fit: Fit, points,
                  shoulder: Shoulder | None = None):
    out.mkdir(parents=True, exist_ok=True)
    grid = fit.grid
    ev = (grid - AXIS_GREY) / AXIS_STOP
    fitted = evaluate_spline(solve_spline(points), grid).astype(float)
    ev_ticks = [(AXIS_GREY + e * AXIS_STOP, f"{e:+d}" if e else "0") for e in range(-6, 11, 2)]
    axis_ticks = [(v / 4, f"{v / 4:.2f}") for v in range(0, 5)]
    curve_series = [(grid, fit.target_curve, "#888", "4 3",
                     "fit target with the toe and the shoulder" if shoulder else "fit target")]
    if shoulder:
        curve_series.append((grid, np.clip(shoulder.source.axis_curve(grid), 0.0, 1.0), "#d9a441",
                             "2 2", "source target (raised black, clips)"))
    curve_series += [(grid, fitted, "#1f5fbf", "", "fitted curve"),
                     (grid, grid, "#cccccc", "2 3", "identity")]
    svg_plot(out / f"{preset_id}_curve.svg", f"{name}: curve axis (ACEScc)",
             "curve input, EV from 18% grey", "curve output", curve_series,
             (0.0, 1.0), (0.0, 1.0), points=points, x_ticks=ev_ticks, y_ticks=axis_ticks)
    source = code(fit.target.display(grid))
    final = code(fit.rendered(points, grid))
    linear = code(neutral_response(np.maximum(linear_from_acescc(grid), 0.0)))
    code_ticks = [(v, f"{v}") for v in range(0, 256, 51)]
    neutral_series = [(ev, source, "#888", "4 3",
                       "source tone with the toe and the shoulder" if shoulder else "source tone")]
    if shoulder:
        neutral_series.append((ev, code(shoulder.source.display(grid)), "#d9a441", "2 2",
                               "source tone (raised black, clips)"))
    neutral_series += [(ev, final, "#c0392b", "", "with the preset"),
                       (ev, linear, "#bbbbbb", "2 3", "Linear (ACES 2.0 only)")]
    svg_plot(out / f"{preset_id}_neutral.svg", f"{name}: neutral axis through ACES 2.0",
             "scene exposure at the curve input, EV from 18% grey",
             "display code (8 bit, gamma 2.2)", neutral_series,
             (-7.5, 10.5), (0, 255), x_ticks=[(e, f"{e:+d}" if e else "0") for e in range(-6, 11, 2)],
             y_ticks=code_ticks)


def cpp_name(preset_id: str) -> str:
    return "k" + "".join(part.capitalize() for part in preset_id.split("_")) + "Points"


def write_points_inc(path: Path, results):
    lines = ["//  Copyright 2026 Yurun Zi",
             "//  SPDX-License-Identifier: GPL-3.0-only",
             "//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.",
             "",
             "// Control points of the fitted Tone Curve presets, written by",
             "// scripts/fit_tone_curve_presets.py (tone_curve_preset_study.md). Do not edit.",
             "// clang-format off",
             ""]
    for preset_id, _name, _group, points, error, _shift in results:
        if preset_id == "linear":
            continue
        body = ", ".join(f"{{{x:.{DIGITS}f}f, {y:.{DIGITS}f}f}}" for x, y in points)
        lines.append(f"// Fit error {error:.2f} codes.")
        lines.append(f"inline constexpr std::array<CurvePoint, {len(points)}> "
                     f"{cpp_name(preset_id)}{{{{{body}}}}};")
    lines.append("// clang-format on")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def write_study_header(path: Path, results, fits, film_tones, shoulders, toes):
    grid = np.linspace(0.0, 1.0, STUDY_SAMPLES)
    lines = ["//  Copyright 2026 Yurun Zi",
             "//  SPDX-License-Identifier: GPL-3.0-only",
             "//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.",
             "",
             "// Study data of the Tone Curve presets, written by scripts/fit_tone_curve_presets.py",
             "// (tone_curve_preset_study.md). Do not edit.",
             "// clang-format off",
             "",
             "#pragma once",
             "",
             "#include <array>",
             "#include <string_view>",
             "",
             "namespace alcedo::tone_curve_study {",
             "",
             "/// Film tone samples: display light (white 1) of the AP1 neutral 0.18 * 2^ev, for",
             "/// ev = kFilmSampleFirstEv + i * kFilmSampleStepEv.",
             f"inline constexpr double kFilmSampleFirstEv = {FILM_FIRST_EV};",
             f"inline constexpr double kFilmSampleStepEv  = {FILM_STEP_EV};",
             f"inline constexpr int    kFilmSampleCount   = {FILM_COUNT};",
             "",
             "struct FilmSourceTone {",
             "  std::string_view                     preset_id;",
             "  std::array<double, kFilmSampleCount> tone;",
             "};",
             "",
             f"inline constexpr std::array<FilmSourceTone, {len(film_tones)}> kFilmSourceTones{{{{"]
    for preset_id, tones in film_tones.items():
        values = ", ".join(f"{v:.9g}" for v in tones)
        lines.append(f'    {{"{preset_id}", {{{{{values}}}}}}},')
    lines += ["}};",
              "",
              "inline constexpr auto FindFilmSource(std::string_view preset_id) -> const FilmSourceTone* {",
              "  for (const auto& source : kFilmSourceTones) {",
              "    if (source.preset_id == preset_id) return &source;",
              "  }",
              "  return nullptr;",
              "}",
              "",
              "/// Samples of the curve axis: x = i / (kAxisSampleCount - 1).",
              f"inline constexpr int kAxisSampleCount = {STUDY_SAMPLES};",
              "",
              "/// The study of one preset: the largest error of its fit in 8-bit display codes",
              "/// (gamma 2.2), and the display light (relative to the 100 nit peak) of the source",
              "/// tone at each axis sample through ACES 2.0. A preset with a highlight shoulder has",
              "/// the source tone with the shoulder.",
              "struct PresetStudy {",
              "  std::string_view                     preset_id;",
              "  double                               fit_error_codes;",
              "  std::array<double, kAxisSampleCount> source_display;",
              "};",
              "",
              f"inline constexpr std::array<PresetStudy, {len(fits)}> kPresetStudies{{{{"]
    for preset_id, _name, _group, _points, error, _shift in results:
        if preset_id not in fits:
            continue
        values = ", ".join(f"{v:.9g}" for v in fits[preset_id].target.display(grid))
        lines.append(f'    {{"{preset_id}", {error:.4f}, {{{{{values}}}}}}},')
    neutral = ", ".join(f"{v:.9g}" for v in neutral_response(np.maximum(linear_from_acescc(grid), 0.0)))
    lines += ["}};",
              "",
              "/// Highlight shoulder of a preset (#899): the knee on the curve axis (one stop below",
              "/// the stop at which the source reaches display white), the slope of the target on",
              "/// the curve axis at the knee and at x = 1, and the largest x up to which the points",
              "/// of the fit without the shoulder stay. Above the knee the target rolls off to the",
              "/// display peak at x = 1.",
              "struct HighlightShoulder {",
              "  std::string_view preset_id;",
              "  double           knee_x;",
              "  double           knee_slope;",
              "  double           end_slope;",
              "  double           kept_points_up_to_x;",
              "};",
              "",
              f"inline constexpr std::array<HighlightShoulder, {len(shoulders)}> kHighlightShoulders{{{{"]
    for preset_id, shoulder in shoulders.items():
        lines.append(f'    {{"{preset_id}", {shoulder.knee_x:.9g}, {shoulder.knee_slope:.9g}, '
                     f'{shoulder.end_slope:.9g}, {shoulder.fixed_below:.9g}}},')
    lines += ["}};",
              "",
              "/// Shadow toe of a preset (#910): the knee on the curve axis (three stops below 18%",
              "/// grey), the target value and slope there, the slope of the target at x = 0, and",
              "/// the smallest x from which the points of the fit without the toe stay. Below the",
              "/// knee the target falls to (0, 0) along a parabola.",
              "struct ShadowToe {",
              "  std::string_view preset_id;",
              "  double           knee_x;",
              "  double           knee_y;",
              "  double           knee_slope;",
              "  double           start_slope;",
              "  double           kept_points_from_x;",
              "};",
              "",
              f"inline constexpr std::array<ShadowToe, {len(toes)}> kShadowToes{{{{"]
    for preset_id, toe in toes.items():
        lines.append(f'    {{"{preset_id}", {toe.knee_x:.9g}, {toe.knee_y:.9g}, '
                     f'{toe.knee_slope:.9g}, {toe.start_slope:.9g}, {toe.fixed_above:.9g}}},')
    lines += ["}};",
              "",
              "/// N of the study (the ACES 2.0 SDR tone scale) at each axis sample, as display light.",
              f"inline constexpr std::array<double, kAxisSampleCount> kStudyNeutralResponse{{{{{neutral}}}}};",
              "",
              "}  // namespace alcedo::tone_curve_study",
              "// clang-format on"]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--film-test", type=Path, required=True,
                        help="ToneCurvePresetFilmSourceTest executable (film print resources)")
    parser.add_argument("--acr-source", type=Path, help="RawTherapee rtengine/dcp.cc")
    parser.add_argument("--write", action="store_true", help="write the data and the figures")
    args = parser.parse_args()

    film_tones = read_film_tones(args.film_test)
    sources = {"acr": acr_source(load_acr_table(args.acr_source)), "filmic": filmic_source()}
    results, fits, shoulders, toes = [], {}, {}, {}
    for preset_id, name, group, kind in PRESETS:
        if kind == "identity":
            results.append((preset_id, name, group, [(0.0, 0.0), (1.0, 1.0)], 0.0, 0.0))
            continue
        shift = 0.0
        if kind == "contrast":
            target = target_from_axis(contrast_shape(CONTRAST_SHAPES[preset_id]))
        else:
            tone = sources[kind] if kind != "film" else film_source(film_tones[preset_id])
            shift = grey_exposure(tone)
            target = target_from_source(tone, shift)
        fit, points, error = fit_preset(target)
        print(f"{preset_id:24s} {len(points):2d} points  error {error:5.2f} codes  "
              f"grey exposure {shift:+.3f} EV", flush=True)
        if preset_id in SHOULDER_PRESETS:
            shoulder = Shoulder(target)
            shoulders[preset_id] = shoulder
            source_fit, source_points = fit, points
            fit, points, error, shoulder.fixed_below = fit_shoulder(shoulder, points)
            grid = fit.grid
            below = grid <= shoulder.knee_x
            shoulder.change_below_knee = float(np.max(np.abs(
                code(fit.rendered(points, grid)) -
                code(source_fit.rendered(source_points, grid)))[below]))
            shoulder.code_rows = [
                (ev, float(code(source_fit.target.display(np.array([x])))[0]),
                 float(code(source_fit.rendered(source_points, np.array([x])))[0]),
                 float(code(fit.rendered(points, np.array([x])))[0]))
                for ev in SHOULDER_TABLE_EVS
                for x in [AXIS_GREY + ev * AXIS_STOP]]
            print(f"{'  with the shoulder':24s} {len(points):2d} points  error {error:5.2f} codes  "
                  f"points kept up to x {shoulder.fixed_below:.5f}, change below the knee "
                  f"{shoulder.change_below_knee:.2f} codes  "
                  f"knee x {shoulder.knee_x:.5f} "
                  f"({(shoulder.knee_x - AXIS_GREY) / AXIS_STOP:+.2f} EV), "
                  f"slope {shoulder.knee_slope:.3f} -> {shoulder.end_slope:.3f}, "
                  f"smallest fitted slope above the knee "
                  f"{smallest_slope_above(points, shoulder.knee_x):.3f}", flush=True)
        if preset_id in TOE_PRESETS:
            toe = Toe(fit.target)
            toes[preset_id] = toe
            source_fit, source_points = fit, points
            fit, points, error, toe.fixed_above = fit_toe(toe, points)
            grid = fit.grid
            above = grid >= toe.knee_x
            toe.change_above_knee = float(np.max(np.abs(
                code(fit.rendered(points, grid)) -
                code(source_fit.rendered(source_points, grid)))[above]))
            toe.code_rows = [
                (ev, float(code(source_fit.target.display(np.array([x])))[0]),
                 float(code(source_fit.rendered(source_points, np.array([x])))[0]),
                 float(code(fit.rendered(points, np.array([x])))[0]))
                for ev in TOE_TABLE_EVS
                for x in [AXIS_GREY + ev * AXIS_STOP]]
            print(f"{'  with the toe':24s} {len(points):2d} points  error {error:5.2f} codes  "
                  f"points kept from x {toe.fixed_above:.5f}, change above the knee "
                  f"{toe.change_above_knee:.2f} codes  knee x {toe.knee_x:.5f} "
                  f"y {toe.knee_y:.5f}, slope {toe.knee_slope:.3f} at the knee, "
                  f"{toe.start_slope:.3f} at x = 0", flush=True)
        fits[preset_id] = fit
        results.append((preset_id, name, group, points, error, shift))

    print("\n| Preset | Points | Fit error (codes) | Grey exposure (EV) |")
    print("| --- | ---: | ---: | ---: |")
    for preset_id, name, _group, points, error, shift in results:
        print(f"| {name} | {len(points)} | {error:.2f} | {shift:+.3f} |")
    print("\n| Preset | Source white (EV) | Knee x | Knee (EV) | Knee slope | End slope | "
          "Points kept up to x | Change below the knee (codes) | "
          "Smallest fitted slope above the knee |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for preset_id, name, _group, points, _error, _shift in results:
        if preset_id in shoulders:
            s = shoulders[preset_id]
            print(f"| {name} | {(s.white_x - AXIS_GREY) / AXIS_STOP:+.2f} | {s.knee_x:.5f} | "
                  f"{(s.knee_x - AXIS_GREY) / AXIS_STOP:+.2f} | {s.knee_slope:.3f} | "
                  f"{s.end_slope:.3f} | {s.fixed_below:.5f} | {s.change_below_knee:.2f} | "
                  f"{smallest_slope_above(points, s.knee_x):.3f} |")
    print("\nDisplay codes of a neutral at the curve input, EV from 18% grey:\n")
    print("| Preset | Form | " + " | ".join(f"{ev:+g}" for ev in SHOULDER_TABLE_EVS) + " |")
    print("| --- | --- | " + " | ".join("---:" for _ in SHOULDER_TABLE_EVS) + " |")
    for preset_id, s in shoulders.items():
        name = next(r[1] for r in results if r[0] == preset_id)
        for column, form in ((1, "source tone"), (2, "points without the shoulder"),
                             (3, "points with the shoulder")):
            print(f"| {name} | {form} | " +
                  " | ".join(f"{row[column]:.1f}" for row in s.code_rows) + " |")
    print("\n| Preset | Knee x | Knee (EV) | Knee y | Knee slope | Slope at x = 0 | "
          "Points kept from x | Change above the knee (codes) |")
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
    for preset_id, t in toes.items():
        name = next(r[1] for r in results if r[0] == preset_id)
        print(f"| {name} | {t.knee_x:.5f} | {(t.knee_x - AXIS_GREY) / AXIS_STOP:+.2f} | "
              f"{t.knee_y:.5f} | {t.knee_slope:.3f} | {t.start_slope:.3f} | "
              f"{t.fixed_above:.5f} | {t.change_above_knee:.2f} |")
    print("\nDisplay codes of a neutral at the curve input, EV from 18% grey:\n")
    print("| Preset | Form | " + " | ".join(f"{ev:+g}" for ev in TOE_TABLE_EVS) + " |")
    print("| --- | --- | " + " | ".join("---:" for _ in TOE_TABLE_EVS) + " |")
    for preset_id, t in toes.items():
        name = next(r[1] for r in results if r[0] == preset_id)
        for column, form in ((1, "target without the toe"), (2, "points without the toe"),
                             (3, "points with the toe")):
            print(f"| {name} | {form} | " +
                  " | ".join(f"{row[column]:.1f}" for row in t.code_rows) + " |")
    print()
    for preset_id, name, _group, points, _error, _shift in results:
        print(f"{preset_id}: " + ", ".join(f"({x:.{DIGITS}f}, {y:.{DIGITS}f})" for x, y in points))

    if args.write:
        figures = ROOT / "docs" / "roadmap" / "alcedo_studio" / "edit" / "tone_curve_preset_study"
        for preset_id, name, _group, points, _error, _shift in results:
            if preset_id in fits:
                write_figures(figures, preset_id, name, fits[preset_id], points,
                              shoulders.get(preset_id))
        write_points_inc(ROOT / "alcedo_studio" / "src" / "edit" / "operators" / "models" /
                         "tone_curve_preset_points.inc", results)
        write_study_header(ROOT / "alcedo_studio" / "tests" / "edit" / "runtime" /
                           "tone_curve_preset_study_data.hpp", results, fits, film_tones,
                           shoulders, toes)
        print("written")


if __name__ == "__main__":
    main()
