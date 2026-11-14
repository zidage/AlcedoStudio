# Tone Curve Preset Study

Date: 2026-11-13

Issue: [#891](https://github.com/zidage/AlcedoStudio/issues/891) (TC6 of
[tone_curve_editor_plan.md](tone_curve_editor_plan.md), tracking issue
[#885](https://github.com/zidage/AlcedoStudio/issues/885)). Highlight shoulder of Film-like and
Filmic: [#899](https://github.com/zidage/AlcedoStudio/issues/899) (2026-11-13, section 4.3).

Script: [`scripts/fit_tone_curve_presets.py`](../../../../scripts/fit_tone_curve_presets.py).
Figures: [`tone_curve_preset_study/`](tone_curve_preset_study/).

## 1. Question

The Tone Curve section gets a preset menu: Linear, three contrast shapes, Film-like, Filmic, and
the tone of Kodak 2383, Velvia 50, and Ektachrome E100. A preset is a list of at most 16 control
points of the product spline (TC1) on the curve axis. This study gives the points of each preset,
the source of each shape, and the error of each fit.

Film-like is also the RGB curve of the RAW import look (user decision of 2026-11-13, section 6).

## 2. Terms

- **Curve axis.** The curve reads and writes ACEScc values of the grade stage. The ACEScc value
  `c` of a scene value `v` is `(log2(v) + 9.72) / 17.52`. 18% grey is at `g = 0.41359`, and one
  stop is `s = 1 / 17.52 = 0.0571` on the axis. The axis [0, 1] covers -7.2 EV to +10.3 EV from
  18% grey.
- **Neutral response `N`.** The display light (relative to the 100 nit peak) that the default
  display transform, ACES 2.0 SDR, gives for a neutral of scene value `v`. The study uses the
  ACES 2.0 tone scale of `survey_dcp_tone_curves.py`; `ToneCurvePresetTest` checks that it equals
  the product response `ProfileToneNeutralResponse` (ACES 2.0) within 0.0001 display codes.
  `N(0.18) = 0.100` (10 nits), and `N` reaches the peak at scene value 128 (ACEScc 0.955).
- **Source tone `S(v)`.** The display light (white 1) that a source rendering gives for a neutral
  of scene value `v`, on the exposure scale of that source.
- **Display code.** The 8-bit code of display light `d` on a gamma 2.2 display,
  `255 * d^(1 / 2.2)`. The fit error is the largest difference of this code between the source
  tone and the neutral axis that the render gives with the preset.

## 3. Sources

### 3.1 Film-like: the Adobe default tone curve

The ACR3 default curve of the DNG SDK (`dng_tone_curve_acr3_default`): a table of 1025 values on
[0, 1], linear between entries, applied to linear ProPhoto values after the exposure ramp. Adobe
Camera Raw and Lightroom apply it to a profile that lacks a `ProfileToneCurve` (Adobe Standard), and
RawTherapee uses the same table for an Adobe profile that lacks a curve
(`rtengine/dcp.cc`, `adobe_camera_raw_default_curve`, commit
`64c2d90a3248c7ad41842d9162368858b61aac6a`). The script reads that table.

Exposure scale: Alcedo +1.5 EV equals ACR 0 EV
([dcp_profile_tone_curve_study.md](../raw-processing/dcp_profile_tone_curve_study.md), section
3.3), so the source of a scene value `v` at the curve input is `S(v) = acr3(v * 2^-1.5)`. The ramp
clips at raw white, which is +3.97 EV from 18% grey at the curve input of the Default document.

### 3.2 Filmic: darktable filmic rgb

darktable filmic rgb (`src/iop/filmicrgb.c`, commit
`f30f439fd801eef58047bc417644e66939b3f3b7`) with the default parameters of the module: middle grey
18.45%, black relative exposure -8 EV, white relative exposure +4 EV, contrast 1.0, latitude
0.01%, shadows and highlights balance 0, hardness 4, target black 0.01517634%, target middle grey
18.45%, target white 100%, fourth-order toe and shoulder, spline version 3. The script ports
`dt_iop_filmic_rgb_compute_spline` and the neutral path of the pixel function: the log encoding
`(log2(v / 0.1845) + 8) / 12` on [0, 1], the spline, and the power 4.

### 3.3 Contrast shapes

S curves around 18% grey on the curve axis:

`y(x) = x + (k - 1) * A * tanh((x - g) / A)`

`k` is the slope at 18% grey on the curve axis, and `A` is the width of the S, 3 stops
(`3 * s`). The slope returns to 1 away from 18% grey, so the deep shadows and the highlights keep
the slope of the display transform. Soft Contrast has `k = 1.15`, Medium Contrast `k = 1.3`,
Strong Contrast `k = 1.5`. The end points are where `y` reaches 0 and 1; outside them the curve
continues along its end slope. For comparison, contrast +15 of the Contrast slider has slope
`2^0.15 = 1.11` at mid grey.

The source tone of a contrast shape is `N(y(x))`: the shape through the display transform.

### 3.4 Kodak 2383, Velvia 50, Ektachrome E100

The neutral tone of the film print render: the point stages of the product
(`FilmPrintPointReference` of `film_print_reference.hpp`, the same math as the kernels) on an AP1
neutral ramp, scene light `reference` (the grey anchor of each film), Rec. 709 luminance of the
decoded display RGB. Kodak 2383 is the print of the reference negative Kodak Vision3 250D 5207;
Velvia 50 and Ektachrome E100 are colour reversal films on the light table. The resources are the
film print resources of schema 11. `ToneCurvePresetFilmSourceTest` prints the ramp (0.18 * 2^ev
for ev = -12 to +12 in steps of 1/16), and the script reads it. The same test checks that the
resources still give the tone of the study.

## 4. Fit

### 4.1 Target

18% grey stays at (0.41359, 0.41359). A source renders 18% grey at its own display level (Film-like
0.107, Filmic 0.1845, Kodak 2383 0.145, Velvia 50 0.227, Ektachrome E100 0.230), and `N` renders
it at 0.100. The grey exposure `e_g` is the exposure shift at which the source renders 18% grey
at `N(0.18)`: `S(0.18 * 2^e_g) = N(0.18)`. The target display light at curve input `x` is

`T(x) = S(2^e_g * v(x))`, with `v(x)` the scene value of the ACEScc value `x`,

and the target curve on the axis is `y_t(x) = ACEScc(N^-1(T(x)))`: the source tone divided by
the neutral response of the default display transform. Display light at or above 1 maps to the
scene value of the peak. So a preset keeps the mid-grey brightness of the image and gives the
contrast, the toe, and the shoulder of its source; the source white reaches the display peak.

### 4.2 Points

The script fits the product spline (TC1: monotone Hermite with weighted harmonic mean slopes and
end slopes from the end segments), in float like the kernels:

1. Start with the end points of the target (x = 0 and x = 1, or the x where `y_t` reaches 0 or
   1) and the grey point.
2. Move the inner points other than grey (Nelder-Mead) to lower the largest display code error
   over 4097 samples of the axis, with rising x at least 0.02 (1/3 stop) apart and rising y.
3. While the error is above 0.5 codes and the curve has fewer than 16 points, add a point at the
   largest error and move the points again.
4. Round each coordinate to 5 decimals and report the error of the rounded points.

### 4.3 Highlight shoulder of Film-like and Filmic (#899)

Finding (#899): the sources of Film-like and Filmic reach display white 4 to 5 stops above 18%
grey and stay there. Their first fits followed that: the curve climbed steeply to the peak and
then ran flat (Film-like: (0.64790, 0.92653) to (0.94031, 0.94154), a slope of 0.05), so every
highlight above about +4 EV rendered as the same code.

The two presets now fit a target with a highlight shoulder. With `x_w` the curve input at which
the source reaches display white and `y_t` the target of section 4.1:

- Knee: `x_k = x_w - s`, one stop below the source white. The DCP profile tone stage puts its
  shoulder at the same place, one stop below raw white
  ([dcp_profile_tone_curve_study.md](../raw-processing/dcp_profile_tone_curve_study.md), section
  6.6).
- Up to the knee the target stays `y_t`.
- Above the knee the target is the rational roll-off

  `y(x) = y_k + D * t / (1 + t)`, with `t = m_k * (x - x_k) / D`,

  where `y_k = y_t(x_k)` and `m_k` is the slope of `y_t` at the knee. Value and slope agree at the
  knee, the slope falls as `m_k / (1 + t)^2`, and `D` is chosen so that `y(1) = 0.95434`, the
  curve output at which `N` reaches the display peak:
  `D = R * m_k * (1 - x_k) / (m_k * (1 - x_k) - R)`, `R = 0.95434 - y_k`. Each stop above the knee
  maps to its own output value up to x = 1 (+10.3 EV).

The fit keeps the rounded points of the fit without the shoulder up to the point before the knee
point, and fits the points above them as in section 4.2, with the end point (1, 0.95434). The
point next to the knee moves with the shoulder points: its spline slope follows its right
neighbour, so with the shoulder in place of the steep top a fixed point there leaves an error of
0.93 codes (Film-like) and 0.88 codes (Filmic) in the stop below the knee. With that one point
free, the curve below the knee stays within 0.32 codes (Film-like) and 0.49 codes (Filmic) of the
curve without the shoulder.

| Preset | Source white (EV) | Knee `x_k` | Knee (EV) | `m_k` | Target slope at x = 1 | Points kept up to x | Change below the knee (codes) | Smallest slope of the fit above the knee |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Film-like | +4.05 | 0.58741 | +3.05 | 2.282 | 0.197 | 0.50517 | 0.32 | 0.324 |
| Filmic | +4.84 | 0.63295 | +3.84 | 2.694 | 0.170 | 0.41359 | 0.49 | 0.232 |

EV values are stops from 18% grey at the curve input. For Film-like the target with the shoulder
lies within 0.54 codes of the ACR3 curve after the DCP stage shoulder `H` (knee 0.5 of raw white):
the two forms give the same roll-off.

Display codes of a neutral at the curve input (8 bit, gamma 2.2):

| Preset | Form | +2 | +3 | +3.5 | +4 | +4.5 | +5 | +6 | +8 | +10 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Film-like | source tone | 187.4 | 229.5 | 244.8 | 254.5 | 255.0 | 255.0 | 255.0 | 255.0 | 255.0 |
| Film-like | points of #891 | 187.4 | 229.9 | 244.5 | 254.3 | 254.7 | 254.7 | 254.7 | 254.8 | 255.0 |
| Film-like | points with the shoulder | 187.6 | 230.0 | 242.2 | 247.5 | 249.8 | 251.6 | 253.1 | 254.3 | 254.9 |
| Filmic | source tone | 163.8 | 206.2 | 225.9 | 242.2 | 252.6 | 255.0 | 255.0 | 255.0 | 255.0 |
| Filmic | points of #891 | 163.7 | 206.4 | 225.4 | 242.7 | 253.1 | 254.8 | 254.8 | 254.8 | 254.9 |
| Filmic | points with the shoulder | 163.8 | 206.3 | 225.8 | 241.7 | 248.3 | 251.2 | 253.3 | 254.5 | 254.9 |

The shoulder spreads the highlights over 7.4 codes from +4 EV to +10 EV (Film-like) and 6.6 codes
from +4.5 EV to +10 EV (Filmic). In exchange the top stop below the source white renders darker:
7.0 codes at +4 EV for Film-like, 4.3 codes at +4.5 EV for Filmic. Velvia 50 and Ektachrome E100
keep their tops as fitted: a slide reaches its minimum density about 4 stops above 18% grey, and
that clip belongs to the look of the film.

## 5. Results

| Preset | Points | Fit error (codes) | Grey exposure `e_g` (EV) |
| --- | ---: | ---: | ---: |
| Linear | 2 | 0 | 0 |
| Soft Contrast | 6 | 0.15 | 0 |
| Medium Contrast | 6 | 0.22 | 0 |
| Strong Contrast | 6 | 0.26 | 0 |
| Film-like | 13 | 0.56 | -0.071 |
| Filmic | 15 | 0.49 | -0.808 |
| Kodak 2383 | 12 | 0.46 | -0.344 |
| Velvia 50 | 15 | 0.51 | -0.897 |
| Ektachrome E100 | 15 | 0.41 | -0.925 |

The points are in `alcedo_studio/src/edit/operators/models/tone_curve_preset_points.inc`, which
the script writes. `ToneCurvePresetTest` evaluates the product spline and the product neutral
response at 257 axis samples and checks each preset against the source tone within its fit error
plus 0.05 codes; the largest difference of each preset is below its recorded error. For Film-like
and Filmic the source tone of the test is the source tone with the shoulder (section 4.3), and
the test also checks the shoulder: from the knee to x = 1 the slope of the curve stays above the
target slope at x = 1, falls to below a quarter of its value at the knee, and the end point is
(1, 0.95434).

Film-like and Filmic points (section 4.3):

| Preset | Points |
| --- | --- |
| Film-like | (0, 0.14493), (0.24811, 0.26545), (0.28192, 0.28731), (0.32285, 0.32092), (0.36954, 0.36613), (0.41359, 0.41359), (0.50517, 0.53603), (0.57179, 0.64589), (0.59180, 0.68831), (0.63221, 0.76256), (0.66469, 0.79612), (0.71636, 0.84267), (1, 0.95434) |
| Filmic | (0, 0.11720), (0.05139, 0.12594), (0.14117, 0.17422), (0.17031, 0.20220), (0.24522, 0.26164), (0.36624, 0.37069), (0.41359, 0.41359), (0.52741, 0.52984), (0.57421, 0.59107), (0.61226, 0.65677), (0.65470, 0.75574), (0.70435, 0.82835), (0.72436, 0.84677), (0.80094, 0.89869), (1, 0.95434) |

Figures: for each preset, the curve on the axis (target, fitted spline, points) and the neutral
axis through ACES 2.0 (source tone, the render with the preset, and Linear). The Film-like and
Filmic figures also draw the source without the shoulder.

| Preset | Curve axis | Neutral axis |
| --- | --- | --- |
| Soft Contrast | ![](tone_curve_preset_study/soft_contrast_curve.svg) | ![](tone_curve_preset_study/soft_contrast_neutral.svg) |
| Medium Contrast | ![](tone_curve_preset_study/medium_contrast_curve.svg) | ![](tone_curve_preset_study/medium_contrast_neutral.svg) |
| Strong Contrast | ![](tone_curve_preset_study/strong_contrast_curve.svg) | ![](tone_curve_preset_study/strong_contrast_neutral.svg) |
| Film-like | ![](tone_curve_preset_study/film_like_curve.svg) | ![](tone_curve_preset_study/film_like_neutral.svg) |
| Filmic | ![](tone_curve_preset_study/filmic_curve.svg) | ![](tone_curve_preset_study/filmic_neutral.svg) |
| Kodak 2383 | ![](tone_curve_preset_study/kodak_2383_curve.svg) | ![](tone_curve_preset_study/kodak_2383_neutral.svg) |
| Velvia 50 | ![](tone_curve_preset_study/fuji_velvia_50_curve.svg) | ![](tone_curve_preset_study/fuji_velvia_50_neutral.svg) |
| Ektachrome E100 | ![](tone_curve_preset_study/kodak_ektachrome_e100_curve.svg) | ![](tone_curve_preset_study/kodak_ektachrome_e100_neutral.svg) |

Observations:

- Film-like lifts the shadows over the toe of ACES 2.0 (the curve starts at y = 0.145). Its
  source reaches the display peak at raw white (+3.97 EV at the curve input of the Default
  document), where ACES 2.0 alone keeps 72% of the peak, and the ACR exposure ramp clips there.
  The preset rolls off from one stop below raw white toward the peak at x = 1 (section 4.3).
- The three films hold a display black above 0 (the maximum density of the print or the slide):
  their curves start at y = 0.19 and stay flat over the deepest 3 stops.
- Velvia 50 and Ektachrome E100 have the steepest mid-tones and reach white about +4 EV above
  18% grey.

## 6. Film-like as the RAW import look

User decision of 2026-11-13: the default look is Film-like. `CreateDefaultPipelineDocument` sets
the RGB curve of Color Grade 1 to the points of `film_like`, with exposure +1.5 EV and saturation
1.3 as before. Contrast stays 0, because the curve carries the contrast of the look: the curve
and ACES 2.0 together give the Adobe tone at Alcedo +1.5 EV = ACR 0 EV, and contrast +15 on top
would add a second S. Documents imported before keep their stored values (contrast 15, the
identity curve), and their bytes and root ids stay the same.

The entry resets of the import look (dcp_profile_tone_curve_study.md, section 6.5) follow. The
Film-like points are the current points or those of the first fit (section 7):

- A profile tone entry writes the identity RGB curve while the RGB curve still has the Film-like
  points: the `ProfileToneCurve` of the camera profile replaces the Adobe default curve, as in
  ACR. Contrast 15 of an older document still resets to 0.
- A Film Print entry writes the identity RGB curve while it still has the Film-like points: the
  negative and the print give the tone of that display transform, and Film-like is defined on
  ACES 2.0.

## 7. Documents with the Film-like points of #891

The shoulder of #899 changes the Film-like points. A document stores its curve points, so a
document imported between #891 and #899 keeps the 12 points of the first fit and renders as
before; new imports get the points of section 5.

The entry resets of section 6 recognize both point lists as the import look
(`IsFilmLikeToneCurve` in `tone_curve_presets.hpp`): the first fit stays in
`tone_curve_presets.cpp` as `FilmLikeToneCurvePointsBeforeShoulder()`, by hand, since the script
writes only the current points:

(0, 0.14493), (0.24811, 0.26545), (0.28192, 0.28731), (0.32285, 0.32092), (0.36954, 0.36613),
(0.41359, 0.41359), (0.50517, 0.53603), (0.58131, 0.66565), (0.62018, 0.77121),
(0.64790, 0.92653), (0.94031, 0.94154), (1, 0.95434)

The preset menu matches exact points, so it shows the first fit as a custom curve; choosing
Film-like gives the points with the shoulder.

## 8. Parameters

| Symbol | Meaning | Unit | Value | Source |
| --- | --- | --- | --- | --- |
| `g` | 18% grey on the curve axis | ACEScc | 0.41359 | data (ACEScc of 0.18) |
| `s` | One stop on the curve axis | ACEScc | 0.0571 | data (1 / 17.52) |
| `N` | Neutral response of ACES 2.0 SDR | display light | `N(0.18) = 0.100` | data (product) |
| ACR anchor | Alcedo exposure of ACR 0 EV | EV | +1.5 | data (DCP study 3.3) |
| `k` | Slope of a contrast shape at 18% grey | | 1.15, 1.3, 1.5 | creative |
| `A` | Width of a contrast shape | stops | 3 | creative |
| `e_g` | Grey exposure of a source | EV | section 5 | fit |
| Spacing | Smallest x step between fitted points | ACEScc | 0.02 | creative |
| Goal | Error at which the fit stops adding points | display codes | 0.5 | creative |
| Points | Largest number of points of a preset | | 16 | data (TC1 limit) |
| Allowance | Test allowance over the fit error | display codes | 0.05 | inferred |
| `x_w` | Curve input at which a source reaches display white | ACEScc | Film-like 0.64449, Filmic 0.69003 | fit (section 4.3) |
| Knee distance | Stops from the knee `x_k` up to `x_w` | stops | 1 | creative (as the DCP stage shoulder) |
| `y_k`, `m_k` | Target value and slope at the knee | ACEScc | section 4.3 | fit |
| `D` | Scale of the rational roll-off | ACEScc | from `y(1) = 0.95434` | inferred |
| End point | Curve output at which `N` reaches the display peak | ACEScc | 0.95434 | data (ACEScc of scene 128) |
| Refit points | Points of the first fit next to the knee that move with the shoulder | | 1 | creative |
| Slope step | Central difference step of `m_k` on the curve axis | ACEScc | 0.002 | inferred |
| Filmic parameters | Defaults of filmic rgb | | section 3.2 | data (darktable) |
| Film resources | Film print resources | | schema 11 | data (alcedo-film-lab) |
