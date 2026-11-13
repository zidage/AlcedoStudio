#!/usr/bin/env python3
# Copyright 2026 Yurun Zi
# SPDX-License-Identifier: GPL-3.0-only
# Additional permission under GPLv3 section 7 applies; see the LICENSE file.
"""Survey the ProfileToneCurve tags of a DCP folder against the ACES 2.0 SDR tone scale.

Evidence script of docs/roadmap/alcedo_studio/raw-processing/dcp_profile_tone_curve_study.md.
It reads the folder read-only and prints statistics. It writes no file and copies no profile data.

Usage: python -I scripts/survey_dcp_tone_curves.py <DCP folder>
"""
import math
import os
import struct
import sys
from collections import Counter

TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}
TAG_TONE_CURVE = 50940
TAG_BASELINE_EXPOSURE_OFFSET = 51109
TAG_DEFAULT_BLACK_RENDER = 51110
# Product Default Color Grade exposure (kDefaultPipelineExposureEv in pipeline_document.hpp).
DEFAULT_EXPOSURE_EV = 1.5
SAMPLE_PROFILES = [
    "Sony ILCE-7M4/Sony ILCE-7M4 Camera ST.dcp",
    "Canon EOS R6/Canon EOS R6 Camera Standard.dcp",
    "Nikon Z 7/Nikon Z 7 Camera Standard v2.dcp",
    "Panasonic DC-S5/Panasonic DC-S5 Camera Natural.dcp",
]


def read_tags(path):
    """Return {tag: (type, count, value)} of the first IFD of a TIFF-shaped file."""
    with open(path, "rb") as handle:
        data = handle.read()
    endian = "<" if data[:2] == b"II" else ">"
    offset = struct.unpack(endian + "I", data[4:8])[0]
    count = struct.unpack(endian + "H", data[offset:offset + 2])[0]
    tags = {}
    for index in range(count):
        entry = data[offset + 2 + 12 * index: offset + 14 + 12 * index]
        tag, typ, n = struct.unpack(endian + "HHI", entry[:8])
        size = TYPE_SIZE.get(typ, 1) * n
        if size <= 4:
            raw = entry[8:8 + size]
        else:
            start = struct.unpack(endian + "I", entry[8:12])[0]
            raw = data[start:start + size]
        if typ == 11:
            value = list(struct.unpack(endian + "%df" % n, raw))
        elif typ == 10:
            pairs = struct.unpack(endian + "%di" % (2 * n), raw)
            value = [pairs[2 * k] / pairs[2 * k + 1] for k in range(n)]
        elif typ == 3:
            value = list(struct.unpack(endian + "%dH" % n, raw))
        elif typ == 4:
            value = list(struct.unpack(endian + "%dI" % n, raw))
        else:
            value = None
        tags[tag] = (typ, n, value)
    return tags


def tone_points(tags):
    values = tags[TAG_TONE_CURVE][2]
    return [(values[2 * i], values[2 * i + 1]) for i in range(len(values) // 2)]


def baseline_exposure_offset(tags):
    return tags.get(TAG_BASELINE_EXPOSURE_OFFSET, (0, 0, [0.0]))[2][0]


def curve(points, x):
    """Piecewise linear evaluation; the ends hold outside the point range."""
    if x <= points[0][0]:
        return points[0][1]
    if x >= points[-1][0]:
        return points[-1][1]
    lo, hi = 0, len(points) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if points[mid][0] <= x:
            lo = mid
        else:
            hi = mid
    (x0, y0), (x1, y1) = points[lo], points[hi]
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0)


def aces2_params(peak_nits=100.0):
    """ACES 2.0 tone scale constants (OpenColorIO ToneScaleParams) for one peak luminance."""
    n, n_r, g, c, c_d, w_g, t_1 = peak_nits, 100.0, 1.15, 0.18, 10.013, 0.14, 0.04
    r_hit = 128 + (896 - 128) * (math.log(n / n_r) / math.log(10000 / 100))
    m_0 = n / n_r
    m_1 = 0.5 * (m_0 + math.sqrt(m_0 * (m_0 + 4 * t_1)))
    u = ((r_hit / m_1) / ((r_hit / m_1) + 1)) ** g
    m = m_1 / u
    w_i = math.log(n / 100) / math.log(2)
    c_t = c_d / n_r * (1 + w_i * w_g)
    g_ip = 0.5 * (c_t + math.sqrt(c_t * (c_t + 4 * t_1)))
    g_ipp2 = -(m_1 * (g_ip / m) ** (1 / g)) / ((g_ip / m) ** (1 / g) - 1)
    w_2 = c / g_ipp2
    s_2 = w_2 * m_1
    u_2 = ((r_hit / m_1) / ((r_hit / m_1) + w_2)) ** g
    return {"g": g, "t_1": t_1, "s_2": s_2, "m_2": m_1 / u_2, "peak": peak_nits, "r_hit": r_hit}


def aces2_forward(x, p):
    """Scene luminance to display luminance relative to the display peak (neutral axis)."""
    x = max(x, 0.0)
    f = p["m_2"] * (x / (x + p["s_2"])) ** p["g"]
    return max(0.0, f * f / (f + p["t_1"])) * 100.0 / p["peak"]


def aces2_inverse(y, p):
    h = y * p["peak"] / 100.0
    f = 0.5 * (h + math.sqrt(h * h + 4 * p["t_1"] * h))
    r = (f / p["m_2"]) ** (1 / p["g"])
    return float("inf") if r >= 1.0 else p["s_2"] * r / (1 - r)


def log_slope(function, x, half_width_stops=0.05):
    low = function(x * 2 ** -half_width_stops)
    high = function(x * 2 ** half_width_stops)
    return (math.log2(high) - math.log2(low)) / (2 * half_width_stops)


def survey(folder, aces):
    files = []
    for root, _, names in os.walk(folder):
        files += [os.path.join(root, name) for name in names if name.lower().endswith(".dcp")]
    counts = Counter()
    distinct = {}
    largest_drops = []
    for path in files:
        tags = read_tags(path)
        if TAG_TONE_CURVE not in tags:
            counts["without curve"] += 1
            continue
        points = tone_points(tags)
        counts["with curve"] += 1
        drops = [points[i][1] - points[i + 1][1] for i in range(len(points) - 1)
                 if points[i + 1][1] < points[i][1]]
        if drops:
            largest_drops.append((max(drops), os.path.basename(path)))
        if points[0] != (0.0, 0.0) or points[-1] != (1.0, 1.0):
            counts["ends other than (0,0) and (1,1)"] += 1
        if tags.get(TAG_DEFAULT_BLACK_RENDER, (0, 0, [None]))[2][0] == 1:
            counts["DefaultBlackRender = 1"] += 1
        distinct.setdefault(tuple(round(v, 6) for v in tags[TAG_TONE_CURVE][2]), points)
    print("files %d, distinct curves %d" % (len(files), len(distinct)))
    for key, value in sorted(counts.items()):
        print("  %s: %d" % (key, value))
    largest_drops.sort(reverse=True)
    print("  curves with a falling segment: %d, largest fall %.6f (%s)" % (
        len(largest_drops), largest_drops[0][0] if largest_drops else 0.0,
        largest_drops[0][1] if largest_drops else "-"))

    rows = []
    for points in distinct.values():
        def function(v, pts=points):
            return curve(pts, v)
        rows.append((function(0.18), log_slope(function, 0.18)))
    rows.sort()
    pick = [rows[int(q * (len(rows) - 1))] for q in (0.1, 0.5, 0.9)]
    print("\ncurve value at scene 0.18: p10 %.3f, median %.3f, p90 %.3f" % tuple(r[0] for r in pick))
    slopes = sorted(r[1] for r in rows)
    print("log-log slope at scene 0.18: median %.3f" % slopes[len(slopes) // 2])
    print("ACES 2.0 SDR at scene 0.18: %.3f, log-log slope %.3f, scene value at the peak %.0f" % (
        aces2_forward(0.18, aces), log_slope(lambda v: aces2_forward(v, aces), 0.18), aces["r_hit"]))


def compare_default(folder, aces):
    stops = [-8, -6, -5, -4, -3.5, -3, -2, -1, -0.5, 0]
    print("\nraw value, stops below raw white:      " + " ".join("%6.1f" % e for e in stops))
    default = [aces2_forward(2 ** (e + DEFAULT_EXPOSURE_EV), aces) for e in stops]
    print("Alcedo default, ACES 2.0 at +1.5 EV:    " + " ".join("%6.3f" % v for v in default))
    for rel in SAMPLE_PROFILES:
        path = os.path.join(folder, rel)
        if not os.path.exists(path):
            continue
        tags = read_tags(path)
        points, offset = tone_points(tags), baseline_exposure_offset(tags)
        values = [curve(points, 2 ** (e + offset)) for e in stops]
        name = os.path.basename(rel)[:-4]
        print("%-39s " % (name[:38]) + " ".join("%6.3f" % v for v in values))
        print("%-39s " % "  difference, stops" + " ".join(
            "%+6.2f" % math.log2(max(v, 1e-6) / d) for v, d in zip(values, default)))


def code_value(display):
    """8-bit code of display light on a gamma 2.2 display."""
    return 255.0 * min(max(display, 0.0), 1.0) ** (1 / 2.2)


def shoulder_rational(x, knee):
    """Highlight shoulder of the stage (study section 6.6): slope 1 at the knee, limit 1."""
    if x <= knee:
        return max(x, 0.0)
    return 1 - (1 - knee) ** 2 / (x + 1 - 2 * knee)


def shoulder_exponential(x, knee):
    """Candidate shoulder with the same knee and slope that reaches 1 faster."""
    if x <= knee:
        return max(x, 0.0)
    return knee + (1 - knee) * (1 - math.exp(-(x - knee) / (1 - knee)))


def percentile(values, q):
    values = sorted(values)
    return values[int(q * (len(values) - 1))]


def compare_shoulder(folder):
    """Section 6.6: the clip at raw white against the highlight shoulder, in 8-bit codes."""
    stops = [-1, -0.5, 0, 1, 2, 3, 4]
    print("\nhighlight shoulder, stops from raw white: " + " ".join("%6.1f" % e for e in stops))
    for rel in SAMPLE_PROFILES:
        path = os.path.join(folder, rel)
        if not os.path.exists(path):
            continue
        points = tone_points(read_tags(path))
        name = os.path.basename(rel)[:-4]
        print("%-41s " % name[:40] + " ".join(
            "%6.1f" % code_value(curve(points, min(2 ** e, 1.0))) for e in stops))
        print("%-41s " % "  shoulder, knee 0.5" + " ".join(
            "%6.1f" % code_value(curve(points, shoulder_rational(2 ** e, 0.5))) for e in stops))

    distinct = {}
    for root, _, names in os.walk(folder):
        for name in names:
            if name.lower().endswith(".dcp"):
                tags = read_tags(os.path.join(root, name))
                if TAG_TONE_CURVE in tags:
                    distinct.setdefault(tuple(tags[TAG_TONE_CURVE][2]), tone_points(tags))
    curves = list(distinct.values())
    top = [code_value(curve(p, 1.0)) - code_value(curve(p, 0.5)) for p in curves]
    print("\ncodes in the top stop of the profile curves: p10 %.1f, median %.1f, p90 %.1f" % (
        percentile(top, 0.1), percentile(top, 0.5), percentile(top, 0.9)))
    print("shoulder candidates over %d distinct curves (codes; loss = largest drop at or below "
          "raw white, gain = codes from raw white to 2 stops above it):" % len(curves))
    for label, function in (("rational", shoulder_rational), ("exponential", shoulder_exponential)):
        for knee in (0.35, 0.5, 0.6):
            loss, gain = [], []
            for points in curves:
                worst = 0.0
                for i in range(1, 401):
                    x = knee + (1 - knee) * i / 400
                    worst = max(worst, code_value(curve(points, x)) -
                                code_value(curve(points, function(x, knee))))
                loss.append(worst)
                white = code_value(curve(points, function(1.0, knee)))
                gain.append(code_value(curve(points, function(4.0, knee))) - white)
            print("  %-11s knee %.2f: loss median %.1f, p90 %.1f, max %.1f; gain median %.1f, "
                  "p10 %.1f" % (label, knee, percentile(loss, 0.5), percentile(loss, 0.9),
                                max(loss), percentile(gain, 0.5), percentile(gain, 0.1)))


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    aces = aces2_params(100.0)
    survey(sys.argv[1], aces)
    compare_default(sys.argv[1], aces)
    compare_shoulder(sys.argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
