# DCP Profile Tone Curve Study

Date: 2026-11-12

Status: **Study complete; design approved** (2026-11-12, with the entry reset of section 6.5 from
the user). Phase T1 #862 is complete (2026-11-12, section 8). Phase T2 #863 is complete on CUDA,
OpenCL, and Metal (2026-11-12, section 8; Metal with C7). Phase T3 #864 is implemented
(2026-11-12, section 8); its user check with ACES 2.0 is open (the C7 review covered OpenDRT on
three camera makes). The highlight shoulder of section 6.6 replaces the clip at raw white
(2026-11-12, after the user saw clipped highlights at a high exposure). Tracking issue #851.

Parent: [DCP Camera Profile Plan](dcp_camera_profile_plan.md). This study replaces decision P-5 of
that plan ("`ProfileToneCurve` is read past and not applied") with the profile tone stage of
section 6.

Source revision: `ea3c9d91a` (`feature/dcp-profile-dng-source`, phase C6 of the parent plan).

Evidence script: `scripts/survey_dcp_tone_curves.py`. It reads a DCP folder read-only and prints
the numbers of sections 3 and 5.

Unless a path starts with `alcedo_studio/`, `docs/`, or `scripts/`, it is relative to
`alcedo_studio/src/`. Section 10 defines each symbol and parameter.

---

## 1. Question

The parent plan keeps the matrices, the HueSatMap, and the LookTable of a DCP profile and drops
its `ProfileToneCurve` (decision P-5). The reason in P-5: the curve is display-referred, and the
ACES output transform maps tone a second time.

The user's position: scene-referred pipelines with an output transform accept camera log footage,
and camera vendors deliver their own tone rendering with that footage. The input transform does
little more than linearize and change primaries. The vendor tone then travels as a look. So the
DCP tone curve belongs in the product too.

This study checks the position against the DNG SDK, the user's DCP folder, and Alcedo's current
render, and it proposes how the curve enters the product.

## 2. The tag and its reference behavior

`ProfileToneCurve` (tag 50940) is a list of `(x, y)` FLOAT pairs on `[0, 1]`. The DNG SDK applies
it in this order ([dng_render.cpp](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_render.cpp),
[dng_reference.cpp](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_reference.cpp),
[dng_tone_curve.cpp](https://android.googlesource.com/platform/external/dng_sdk/+/refs/heads/android14-prebuilt-test/source/dng_tone_curve.cpp)):

1. Camera RGB to linear ProPhoto RGB (D50), with the HueSatMap.
2. Exposure ramp: user exposure plus total baseline exposure, with white at `2^-E` for a positive
   exposure `E`. The ramp clips at white.
3. LookTable.
4. Baseline RGB tone (`RefBaselineRGBTone`): the curve, solved as a cubic spline
   (`dng_spline_solver`) and sampled into a `dng_1d_table` of 4096 entries, applies to the largest
   and the smallest ProPhoto channel. The middle channel keeps its relative position between them:
   `mid' = min' + (max' - min') * (mid - min) / (max - min)`. This keeps hue in display terms.
5. ProPhoto RGB to the output space with its encoding.

The curve output is linear display light relative to display white. Steps 4 and 5 together form a
complete SDR display rendering. A negative exposure beyond the baseline darkens the curve input
(`dng_function_exposure_tone`). A profile that lacks the tag uses the SDK's built-in ACR3 default
curve.

## 3. Evidence

### 3.1 User folder

`D:\Adobe\Adobe Lightroom Classic\Resources\CameraProfiles\Camera`, 2026-11-12:

| Fact | Value |
| --- | --- |
| Files with `ProfileToneCurve` | 2445 of 2445 |
| Distinct curves | 1390 |
| Points per curve | 32 to 256; 128 in 924 files |
| Curves that end at `(0, 0)` and `(1, 1)` | 2392; 53 Panasonic files reach `y = 1` at `x = 0.961` to `0.992` |
| Curves with a falling segment | 17 (Panasonic L Classic Neo, Canon EOS R100 Portrait); largest fall 0.0059 |
| `DefaultBlackRender = 1` (no automatic black) | 2028 files; the other 417 files leave it at the default 0 (automatic black) |
| Curve value at scene 0.18 | 0.314 (p10), 0.435 (median), 0.520 (p90) |
| Log-log slope at scene 0.18 | 0.908 (median) |

For comparison, the ACES 2.0 SDR tone scale (100 nits) renders scene 0.18 at 0.100 with a
log-log slope of 1.187, and reaches the display peak at scene 128.

### 3.2 DNG files

Both CI DNG files (`tests/resources/sample_images/ci_rawfiles/*.dng`) embed **Adobe Standard**
with a HueSatMap and a LookTable and `BaselineExposure = 0.15`. Their profile leaves out
`ProfileToneCurve`, so Adobe renders them with the ACR3 default curve. Phone DNG files carry their
own curve; the CI set holds only the two Adobe Standard files.

### 3.3 Exposure scale of the two renderings

Alcedo's RAW scene scale puts raw white at 1.0. The camera pass applies `BaselineExposure` and
`BaselineExposureOffset` (DNG path and DCP path). The product Default document adds
`kDefaultPipelineExposureEv = 1.5` in the primary Color Grade, then ACES 2.0 or OpenDRT renders.

A DCP curve expects raw white at 1.0 after the baseline exposure: the same scale before Alcedo's
default exposure. The table compares the two renderings on one raw ramp (display light relative
to white, ACES 2.0 at 100 nits):

| Raw value, stops below raw white | -8 | -6 | -5 | -4 | -3.5 | -3 | -2 | -1 | -0.5 | 0 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Alcedo Default, ACES 2.0 at +1.5 EV | 0.001 | 0.013 | 0.039 | 0.098 | 0.145 | 0.206 | 0.365 | 0.551 | 0.640 | 0.719 |
| Sony ILCE-7M4 Camera ST at ACR 0 EV | 0.001 | 0.011 | 0.032 | 0.095 | 0.159 | 0.251 | 0.503 | 0.783 | 0.892 | 0.970 |
| Difference, stops | +0.68 | -0.27 | -0.28 | -0.04 | +0.13 | +0.29 | +0.46 | +0.51 | +0.48 | +0.43 |
| Canon EOS R6 Camera Standard, difference | -0.11 | -1.10 | -0.50 | +0.04 | +0.30 | +0.48 | +0.60 | +0.51 | +0.46 | +0.40 |
| Nikon Z 7 Camera Standard v2, difference | +0.56 | -0.73 | -0.54 | -0.08 | +0.14 | +0.34 | +0.60 | +0.61 | +0.54 | +0.45 |
| Panasonic DC-S5 Camera Natural, difference | +1.62 | +0.13 | +0.11 | +0.24 | +0.33 | +0.39 | +0.41 | +0.35 | +0.37 | +0.43 |

Result: **Alcedo +1.5 EV and ACR 0 EV are the same exposure.** Around mid grey (-4 to -3.5
stops) the two renderings agree within 0.33 stops. The camera curves are 0.3 to 0.6 stops
brighter from -3 stops up and reach display white at raw white, where ACES 2.0 keeps 0.72 and
renders the rest as highlight roll-off. That difference is the camera look.

### 3.4 Placements that fail

Sony ILCE-7M4 Camera ST, raw mid grey (-3.5 stops). Reference: the curve at ACR 0 EV, 0.159.

| Placement | Display value | Error |
| --- | ---: | ---: |
| Curve after the +1.5 EV default, shown as display light | 0.503 | +1.67 stops |
| Curve in the Develop camera pass, +1.5 EV, then ACES 2.0 | 0.255 | +0.69 stops; raw white at 0.713, mid-grey slope 1.27 for 1.46 |
| Curve in the Develop camera pass, 0 EV, then ACES 2.0 | 0.086 | -0.89 stops |

The concern of P-5 is real for these placements: a display curve that feeds the output transform
as scene light renders tone twice, and the curve placed before the exposure anchor overexposes.

## 4. Answer to the question

The user's position holds. Vendor looks enter scene-referred pipelines as a look in front of the
output transform, built through the inverse of the output transform, so that look plus output
transform reproduces the vendor rendering. Camera vendor LMTs for ACES are made this way. The
DCP maps onto that structure:

| Pipeline part | Log footage in ACES | DCP in Alcedo |
| --- | --- | --- |
| Input transform | Log decode and primaries | Matrices, HueSatMap, baseline exposure (Develop, phases C1 to C6) |
| Look | Vendor LMT | LookTable (Develop) and **ProfileToneCurve** (profile tone stage, section 6) |
| Output transform | ACES output transform | Active DRT: ACES 2.0 or OpenDRT |

The tone curve conflicts with the DRT only when it feeds the DRT as scene light (section 3.4).
Expressed through the DRT's inverse at the right exposure, it reproduces the profile's neutral tone
exactly and keeps the DRT for gamut mapping, HDR output, and its look controls.

## 5. Constraints

| ID | Constraint | Source |
| --- | --- | --- |
| K-1 | The curve applies after every user exposure and grade, as in step 2 to 4 of section 2. | DNG SDK order |
| K-2 | Alcedo +1.5 EV equals ACR 0 EV. | Section 3.3 |
| K-3 | CUDA, OpenCL, and Metal compute the stage; the host builds tables only. | `AGENTS.md` |
| K-4 | Old projects keep their bytes and their render: the profile fingerprint and the embedded DNG path stay unchanged. | Root id rule |
| K-5 | Alcedo ships no Adobe data, so the ACR3 default curve stays outside the product. | Parent plan, user goal |
| K-6 | Film Print renders a negative from scene exposure and takes its tone from the negative and the print stock. The profile curve is a second display rendering there. | Film Print DRT plan |

---

## 6. Design: the profile tone stage

### 6.1 Definition

Symbols (section 10 lists each with its value):

- `x`: AP1 scene-linear RGB at the DRT entry, after every Color Grade node and the scene-referred
  DRT stages.
- `M`: AP1 to linear ProPhoto RGB (D50), the matrix of the camera pass.
- `E_a`: the exposure anchor, `kDefaultPipelineExposureEv` (1.5 EV, constraint K-2).
- `C`: the profile tone curve: the DNG SDK cubic spline through the tag points, sampled into 4097
  entries on `[0, 1]`.
- `H`: the highlight shoulder at the curve input (section 6.6): 0 below 0, identity up to the knee
  `k` = 0.5, and `1 - (1 - k)^2 / (v + 1 - 2k)` above it.
- `R_C`: baseline RGB tone with `C` (section 2, step 4).
- `N_m`: neutral response of DRT method `m` with its product default parameters at the SDR
  reference display: scene luminance to display light relative to the peak.
- `N_m^-1`: its inverse, held at the scene value of the peak for display light 1.

Stage:

```
p      = H(M * x * 2^-E_a)                  ProPhoto, ACR exposure scale, highlight shoulder
d      = R_C(p)                              display light, profile rendering
s      = N_m^-1(d)                           per channel, back to scene light of method m
x_out  = M^-1 * s                            AP1 into the DRT
```

At the default DRT parameters and the SDR display, a neutral input below the knee renders exactly
as the profile curve: `N_m(N_m^-1(C(v))) = C(v)`. Above the knee it renders as `C(H(v))`. Colored
input takes the profile's display values through the DRT's own chroma model.

### 6.2 Rules

| Case | Behavior |
| --- | --- |
| DRT ACES 2.0 | `N` = ACES 2.0 forward on a neutral at 100 nits, from `aces2_reference_math.h`. |
| DRT OpenDRT | `N` = OpenDRT achromatic path with the `DrtPayload` default parameters at 100 nits. A changed tone scale control acts relative to the profile tone: `N_user(N_default^-1(C))`. |
| DRT Film Print | The stage stays out of the render (K-6). The stored setting stays. |
| HDR display | `N` stays at the SDR reference; the HDR DRT renders `x_out`. ACES 2.0 at 1000 nits renders the profile white (scene 128) at 962 nits. |
| Scene light above the curve domain | The highlight shoulder `H` rolls it toward display white with a step for each stop; it reaches white only in the limit (section 6.6). The SDK exposure ramp clips there. |
| Negative ProPhoto channels | Clip to 0, as in the SDK. |
| Curve with a falling segment | Applies as stored, as the SDK does. `N^-1` needs a monotone `N` only. |
| Profile without the tag | The stage is unavailable for that profile (K-5). |
| `DefaultBlackRender` | Alcedo renders every profile as the value 1 (black stays at 0). |
| Embedded DNG profile | Unchanged in T1 to T3 (K-4). A later phase may add phone DNG curves with an explicit opt-in. |

### 6.3 Data and storage

- The DCP reader keeps the curve as an immutable `DngToneCurve` value (points, sampled table,
  own FNV-1a fingerprint). The profile fingerprint excludes it (K-4), so C1 to C6 references keep
  their fingerprints.
- The DCP reference `camera_profile_file` gains `profile_tone`. The stage runs by default, as in
  ACR; the key is written only as `false`. The camera-pass cache identity excludes it, so a switch
  recomputes the DRT result only.
- The DRT compile reads the effective profile of the document's Develop node. The DRT result
  identity gains the curve fingerprint and the method `m` while the stage runs.
- GPU data: the `C` table (4097 floats) and the `N_m^-1` table (4097 floats sampled on
  `d^(1/2.4)`). The host builds `N_m^-1` once per method and process.

### 6.4 Editor

The Camera Profile section of the RAW panel gains a switch **Tone Curve** under the profile combo.
The switch is visible when the selected DCP profile has a curve, and disabled while the DRT method
is Film Print. A change is one settled edit of the Develop field `camera_profile_file`; Undo, Redo,
and version checkout restore it.

### 6.5 Entry reset of the RAW import look

The profile curve and LookTable carry the camera's contrast and color. The RAW import look of the
Default document (contrast `kImportLookContrastBeforeFilmLike`, saturation `kDefaultPipelineSaturation`)
adds a second contrast and color on top. The stage takes the rule of the Film Print entry
(`FilmPrintEntrySaturationTarget`, user decision 2026-11-12):

- **Profile tone entry**: a settled edit of `camera_profile_file` after which the stage runs and
  before which it stayed out of the render. The stage runs when the reference is bound, its profile
  has a curve, `profile_tone` is true, and the DRT method is ACES 2.0 or OpenDRT. A DCP selection
  from the default entry and the Tone Curve switch turned on are both entries.
- On an entry, when **Color Grade 1** (`grade.primary`, `PipelineDocument::PrimaryGrade`) exists,
  the same history batch writes its contrast to `kProfileToneEntryContrast` while it still equals
  `kImportLookContrastBeforeFilmLike`, and its saturation to `kProfileToneEntrySaturation` while it still
  equals `kDefaultPipelineSaturation`. A value that the user selected stays. A document that lacks
  `grade.primary` keeps every Grade value.
- An input sequence that is open on contrast or saturation keeps its value, as for Film Print.
- One Undo restores the profile reference and both values; Redo writes them again.
- Since #891 the RAW import look has the Film-like RGB curve with contrast 0
  ([tone_curve_preset_study.md](../edit/tone_curve_preset_study.md), section 6). The entry also
  writes the identity RGB curve while the RGB curve still has the Film-like points; contrast 15
  (`kImportLookContrastBeforeFilmLike`) of a document imported before still resets to 0.
- Exposure stays. The anchor `E_a` makes the stored +1.5 EV the profile's own exposure
  (section 3.3), so a user exposure keeps its meaning with the stage on or off.
- A leave (stage on to stage off) writes no Grade value, as Film Print does.

The function `ProfileToneEntryResetTargets(document, develop_before, develop_after)` in
`app/editor_pipeline_command_service.cpp` returns the targets; `EditorHistoryMutation::
CommitAdjustment` appends them to the batch of a `camera_profile_file` commit.

### 6.6 Highlight shoulder

Finding (user, 2026-11-12, after T3): with the stage on, a high exposure clips the highlights. The
first form of the stage clamped the curve input to `[0, 1]`, as the SDK exposure ramp does. Every
scene value above raw white (ACR scale 1, Alcedo scene `2^E_a`) rendered as display white, where
the DRT alone keeps about five stops of roll-off above raw white. A curve with a steep top, such as
Canon EOS R6 Camera Standard (slope 0.91 at raw white), also showed a kink where the clip starts.

Cause: the camera curves spend little display range on highlights. The top stop below raw white
holds 18.4 codes (median, 8-bit on gamma 2.2; p10 11.7, p90 33.9; `scripts/survey_dcp_tone_curves.py`).
A rendering that keeps the curve exact up to raw white uses the whole display range at raw white.
So any highlight range above raw white takes codes from the top of the curve.

Rule: the curve input passes through `H` per ProPhoto channel in place of the clamp:

```
H(v) = 0                                   v < 0
H(v) = v                                   0 <= v <= k
H(v) = 1 - (1 - k)^2 / (v + 1 - 2k)        v > k,   k = 0.5
```

- `H` is the identity up to `k`, one stop below raw white, so mid grey and the shadows keep the
  profile curve exactly. At `k`, value and slope (1) agree, so the shoulder starts smoothly.
- With `k` = 0.5, `H(v) = 1 - 0.25 / v` above the knee: raw white maps to 0.75, and each further
  stop halves the distance to the curve's white. `H` stays below 1 for every finite scene value,
  and the curve's own top renders the roll-off.
- `H` acts per channel, as the clamp did, so a bright saturated color approaches white smoothly in
  place of a flat patch at the clip.
- `H` is the same for every profile and DRT method; one constant (`kProfileToneHighlightKnee`,
  `PROFILE_TONE_HIGHLIGHT_KNEE`) sets it on the host and on the three backends.

Effect on the sample profiles (8-bit codes, display light on gamma 2.2, stops from raw white at
the ACR scale):

| Profile | Form | -1 | -0.5 | 0 | +1 | +2 | +3 | +4 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Sony ILCE-7M4 Camera ST | clip | 238.2 | 249.1 | 255.0 | 255.0 | 255.0 | 255.0 | 255.0 |
| Sony ILCE-7M4 Camera ST | shoulder | 238.2 | 246.7 | 250.5 | 253.6 | 254.7 | 254.9 | 255.0 |
| Canon EOS R6 Camera Standard | clip | 237.3 | 246.4 | 255.0 | 255.0 | 255.0 | 255.0 | 255.0 |
| Canon EOS R6 Camera Standard | shoulder | 237.3 | 244.4 | 247.7 | 250.9 | 252.3 | 252.9 | 253.3 |
| Nikon Z 7 Camera Standard v2 | clip | 244.2 | 251.5 | 255.0 | 255.0 | 255.0 | 255.0 | 255.0 |
| Nikon Z 7 Camera Standard v2 | shoulder | 244.2 | 250.0 | 252.3 | 254.2 | 254.6 | 254.8 | 254.9 |
| Panasonic DC-S5 Camera Natural | clip | 222.3 | 238.9 | 255.0 | 255.0 | 255.0 | 255.0 | 255.0 |
| Panasonic DC-S5 Camera Natural | shoulder | 222.3 | 234.4 | 241.7 | 249.4 | 253.0 | 254.6 | 254.8 |

Over the 1390 distinct curves of the user folder, the shoulder lowers the top stop below raw white
by at most 6.0 codes (median; p90 11.3, max 26.4) and gives the two stops above raw white 4.8
codes (median; p10 1.9). The candidates of the evidence script:

| Shoulder | Knee | Loss at or below raw white: median, p90, max | Gain from raw white to +2 stops: median, p10 |
| --- | ---: | --- | --- |
| Rational (chosen form) | 0.35 | 8.9, 16.5, 37.4 | 6.7, 3.3 |
| Rational (chosen form) | **0.50** | **6.0, 11.3, 26.4** | **4.8, 1.9** |
| Rational (chosen form) | 0.60 | 4.5, 8.6, 20.1 | 3.7, 0.8 |
| Exponential, `k + (1 - k)(1 - e^(-(v - k)/(1 - k)))` | 0.50 | 4.0, 7.9, 18.5 | 3.8, 0.8 |

The knee 0.5 keeps the profile exact over all but its top stop and still gives the steep-top
curves several codes per stop above raw white. The exponential form costs fewer codes but reaches
white within about one stop above raw white, which leaves the clip visible one stop later.

---

## 7. Alternatives

| Alternative | Result | Decision |
| --- | --- | --- |
| A1. Curve in the Develop camera pass, next to the LookTable | Grade acts on display light; the DRT maps tone twice (section 3.4). | Rejected |
| A2. Stage without the exposure anchor | Default document overexposes by 1.67 stops at mid grey. | Rejected |
| A3. New DRT method "Camera Profile" that renders steps 4 and 5 of the SDK | Exact ACR SDR rendering. Loses HDR output, gamut mapping, and DRT look controls; adds a fourth method on three backends. | Rejected |
| A4. Full ACES 2.0 inverse in place of the per-channel neutral inverse | Exact ACR color on ACES 2.0 SDR. ACES 2.0 only; its forward chroma compression cancels. | Measured in T1; a later mode at most |
| A5. `N` from the active DRT parameters | Cancels every DRT tone control on neutrals. | Rejected |
| A6. ACR3 default curve for profiles that lack the tag | Ships Adobe data (K-5). | Rejected |
| A7. Entry reset of exposure to 0 EV with `E_a` = 0 | Equal to the anchor for an untouched document. A user exposure, such as +2.0 EV, renders 1.5 stops brighter after a DCP selection. | Rejected; the anchor keeps every exposure |
| A8. Clip the curve input at raw white, as the SDK exposure ramp | Exact profile up to raw white; every scene value above it renders as display white, with a kink on a steep-top curve (section 6.6). | Replaced by the shoulder `H` (2026-11-12) |
| A9. Shoulder sized by the primary grade exposure above the anchor (ACR-style exposure) | Exact at the anchor exposure. At +1 EV the pushed range lands on the flat top of the curve and gains 1 to 3 codes over the clip. The DRT stage would read a Grade value, and a second Grade or a Mask exposure would break the rule. | Rejected |
| A10. Profile curve up to a display knee, then the DRT's own shoulder | About twice the highlight range of `H`, at about twice the loss below raw white (Sony ILCE-7M4 Camera ST: raw white 243.7 for a knee at display 0.7). Needs a curve table for each DRT method. | Rejected; `H` keeps more of the profile look |

## 8. Phases

Each phase stays below 2000 lines. C7 of the parent plan follows T3 and verifies the stage with the
rest of the DCP path.

| Phase | Result | Main modules | Dependency |
| --- | --- | --- | --- |
| T1 | Curve value, host scalar reference of the stage, `N_m` and `N_m^-1` for both methods, measurement record | `image/`, `edit/runtime/` (host) | C6 |
| T2 | `profile_tone` key, DRT compile data, cache identity, stage at the DRT entry on CUDA, OpenCL, Metal, Film Print rule | `edit/graph/`, `edit/runtime/`, backends | T1 |
| T3 | Tone Curve switch in the Camera Profile section, history, documents | `app/`, `ui/alcedo_main/`, `docs/` | T2 |

### T1. Curve value and scalar reference

- `ReadDcpCameraProfile` keeps `ProfileToneCurve` as `DngToneCurve`. Validation: at least two
  points, finite values, `x` strictly rising on `[0, 1]`, `y` on `[0, 1]`; a failure names the tag.
- Spline and table match `dng_spline_solver` and `dng_1d_table` within 1e-6.
- Host neutral responses: ACES 2.0 from `aces2_reference_math.h`; OpenDRT from a port of the
  achromatic path of `edit/runtime/cuda/drt/open_drt_funcs.cuh` with default parameters.
- Measurement record in this document: (a) neutral error of `N_m(stage(v))` against `C` for both
  methods, target below 1e-3 relative; (b) CIEDE2000 of the ACES 2.0 host forward of the stage
  against the SDK-style reference (steps 4 and 5, Rec.709, sRGB) on a synthetic patch set, for the
  per-channel inverse and for A4; (c) the same CIEDE2000 for the Default document after the entry
  reset of section 6.5, which records how far contrast 0 and saturation 1.0 stand from the SDK
  rendering.
- Tests use synthetic DCP files. `ProfileToneCurveIsReadPastAndDoesNotChangeTheProfile` becomes
  a test that the curve keeps the profile fingerprint.

#### T1 completion (2026-11-12)

Status: **complete**. Branch `feature/dcp-profile-tone-reference`, stacked on the study PR #861.

Implementation:

- `image/dng_tone_curve.hpp`: `DngToneCurve` (points, knot slopes, 4097-entry float table,
  fingerprint), `MakeDngToneCurve`, and `ApplyDngBaselineRgbTone`. The slopes follow
  `dng_spline_solver::Solve` in its order of operations. The table samples the spline at
  `j / 4096`, as `dng_render` builds its tone table (`dng_1d_table::Initialize` with its default,
  direct sampling). The fingerprint is FNV-1a of the point count and the float bit patterns.
- `DcpCameraProfile::profile_tone_curve` holds the curve, or null when the file lacks tag 50940.
  `DcpCameraProfileToJson` leaves it out, so the profile fingerprint stays as in C1 to C6.
- The reader rejects an odd value count, a type other than FLOAT, and more than 4096 points
  (`kMaxDngToneCurvePoints`) in addition to the conditions above. Each message starts with
  `tag 50940 (ProfileToneCurve)` and names the point index.
- `edit/runtime/profile_tone_reference.hpp`: `ProfileToneNeutralResponse` (`N_m`, `N_m^-1`, scene
  values at the peak and at black) and `ApplyProfileToneStageReference` (section 6.1).
  `ProfileToneNeutralResponse::Resolve` builds each method once per process.
- `N_ACES` is the Rec.709 luminance of `A2rAp0ToDisplay` on the AP0 neutral, Rec.709 primaries,
  100 nits. `N_OpenDRT` is the achromatic path of `OpenDRTTransform_fwd` with the parameters that
  `DrtOutputResolver` resolves from a default `DrtPayload` with method OpenDRT. On a neutral, the
  opponent distance is 0, so the tone scale on the norm remains. `Resolve` checks that the defaults
  keep local and high contrast off, creative white D65, and a Rec.709 display.
- `N_m^-1` is a table of 4097 scene values at display light `(i / 4096)^2.4`, solved by bisection
  of `N_m` (64 steps) between the scene value at black and the scene value at the peak.
- `M` is `ProPhotoToAp1(XyzD50ToAp1Matrix())`, the matrix pair of the camera pass.
  `XyzD50ToAp1Matrix` (develop_color_transform) and `ProPhotoToAp1` (dng_profile_gpu_data) now
  hold the arithmetic that `ResolveDevelopColorTransform` and `PackDngProfileGpuData` used, so the
  camera pass keeps its bytes.

Details chosen during T1:

- OpenDRT renders scene 0.18 at 0.124 (12.4 nits) at its defaults. The default encoding,
  Rec.709 gamma 2.2, uses the average surround, which divides the contrast 1.66 by 1.1. The
  response follows the kernel, so the stage reproduces the profile curve through that rendering.
- The scene value at the peak is 128.00 for ACES 2.0 and 117.62 for OpenDRT. The scene value at
  black is 0 for ACES 2.0 and -0.005 (the OpenDRT offset `tn_off`) for OpenDRT, so profile black
  renders as display black on both.
- The OpenDRT kernel converts AP1 to P3-D65 through XYZ, so the AP1 white (near D60) enters the
  kernel as a slightly warm P3 color with a small opponent distance. The achromatic port uses
  distance 0. The neutral ramp of T2 on the GPU measures the difference against the host
  `N_m(stage)`.
- The host measurement starts the ACES 2.0 forward at the stage output. The product DRT entry also
  holds the ACES reference gamut compression (`AcesReferenceGamutCompress`); T2 places the stage
  relative to it.

Measurement record (`ProfileToneReferenceTest`, Debug build, synthetic camera-like curve of 128
points with value 0.444 at scene 0.18):

| Item | Case | Result |
| --- | --- | --- |
| (a) | Neutral error of `N_m(stage(v))` against `C(v)`, 12 stops below raw white to raw white | ACES 2.0 3.1e-5, OpenDRT 2.4e-5 relative (target 1e-3) |
| (b) | CIEDE2000 against the SDK-style rendering, per-channel inverse, ACES 2.0 | mean 2.17, p95 6.69, max 16.08 |
| (b) | CIEDE2000 against the SDK-style rendering, full ACES 2.0 inverse (A4) | mean 0.00, p95 0.00, max 0.00 |
| (c) | Default document after the entry reset (exposure +1.5 EV, contrast 0, saturation 1.0) | equal to the per-channel row: contrast 0 and saturation 1.0 are identity |
| (c) | Default document before the entry reset (contrast 15, saturation 1.3) | mean 4.85, p95 15.25, max 26.01 |

The patch set holds 175 patches in linear ProPhoto at the ACR scale: a neutral and 12 hues at
purity 0.3 and 0.6 (`L * (1 + purity * cos(hue - 120 * c))` for channel c), at 7 levels from 7
stops to 0.75 stops below raw white. The SDK-style rendering clips the exposure ramp, applies the
baseline RGB tone, converts ProPhoto to sRGB (Bradford D50 to D65), and clips to [0, 1]. The Alcedo
rendering applies the anchor, the stage, AP1 to AP0, and the ACES 2.0 forward at 100 nits, and
clips to [0, 1]. CIEDE2000 compares the two in CIELAB D65.

Reading of the record:

- The neutral patches agree within CIEDE2000 0.1. The per-channel error grows with level: the mean
  by level is 0.48, 0.79, 1.37, 1.58, 2.03, 2.80, and 6.16 from the darkest level to the brightest.
  The largest error is at ProPhoto (0.595, 0.286, 0.904), a bright saturated violet. There the
  ACES 2.0 chroma compression and gamut mapping act; the SDK keeps the hue of the RGB tone and
  clips to sRGB.
- A4 reproduces the SDK display values, as section 7 expects; it stays a later mode at most.
- The entry reset halves the mean error of the Default document (4.85 to 2.17) and lowers the p95
  from 15.25 to 6.69, which supports the reset of section 6.5.

Primary call chains:

- Read: `ReadDcpCameraProfile` -> `ReadToneCurve` -> `MakeDngToneCurve` -> `SolveSlopes`, table
  sampling -> `DcpCameraProfile::profile_tone_curve`.
- Stage reference: `ApplyProfileToneStageReference` -> `ProfileToneStageMatrices` (AP1 to
  ProPhoto, anchor, clamp) -> `ApplyDngBaselineRgbTone` -> `ProfileToneNeutralResponse::Inverse`
  per channel -> ProPhoto to AP1.
- Responses: `ProfileToneNeutralResponse::Resolve` -> `ResolveAces2ReferenceRuntime` (ACES 2.0) or
  `DrtOutputResolver::Resolve` (OpenDRT) -> `BuildInverseTable` -> `Forward`.

Tests: `DngToneCurveTest` (9, against a port of the SDK classes), `DcpFileReaderTest` (8 curve
tests; `ProfileToneCurveIsReadAndKeepsTheProfileFingerprint` replaces the C1 test),
`ProfileToneReferenceTest` (9). `DcpFileReaderFolderTest` on the user folder: 2445 files, 2445
curves, 0 rejected. Caller suites `DngColorProfileTest`, `DcpCameraProfileRenderTest`, and the
color transform and camera profile tests of `GpuDagModelGraphTest` pass.

### T2. Render

- `camera_profile_file.profile_tone` parse and write (`false` only), history comparison, binding.
- DRT compile data and result identity (section 6.3); the Film Print rule (section 6.2).
- The stage as the first operation of the display transform on CUDA, OpenCL, and Metal.
- Tests: a neutral ramp per backend against the host `N_m(stage)`; GPU against the scalar
  reference on color patches; a DCP that lacks the curve and every old document render with their
  current bytes; a switch reuses the camera-pass result.

#### T2 completion (2026-11-12)

Status: **complete** on CUDA, OpenCL, and Metal (Metal run with C7; see the Metal
item below). Branch `feature/dcp-profile-tone-render`, stacked on the T1 PR #865.

Implementation:

- `DevelopCameraProfileFile::profile_tone` (default true). `DevelopCameraProfileFileToJson` writes
  `"profile_tone": false` only; `DevelopCameraProfileFileFromJson` reads the key and rejects a value
  that is not a boolean. Equality compares the four stored values; `SameProfile` and
  `SameCameraProfile` compare the profile identity without the switch.
- `DevelopDirty::ProfileTone` marks a change of the switch alone. `ApplyCameraProfileFileUpdate` and
  `ReplaceParams` mark `WhiteBalance` only when the profile, its binding, or the as-shot values
  change. `LoadJson` keeps the bound profile when only the switch changes.
  `RuntimeInvalidationState::CollectDevelopChanges` turns a `ProfileTone` revision into an
  invalidation of the display result only.
- `DevelopCameraProfileFileUpdate::profile_tone` carries the switch. The field write accepts the
  optional boolean `profile_tone` next to `camera_model` and `profile_name` (default true). The
  history value form, `ResolveStoredDcpCameraProfileFile`, and `BindDcpCameraProfile` keep the
  stored value. The expected-side history check compares the switch with the identity (only the
  fingerprint is left out).
- `edit/runtime/profile_tone_stage.hpp`: `ResolveProfileToneStage(document)` returns the curve and
  the method when the reference is set with `profile_tone` true, the profile has the curve, and the
  DRT method is ACES 2.0 or OpenDRT; otherwise nullopt (Film Print, switch off, no curve, no
  reference). An unbound reference whose stage would run throws with its identity.
  `PackProfileToneGpuData` packs `M`, `M^-1`, the anchor scale, the inverse table exponent, the `C`
  table, and the `N_m^-1` table (8214 floats). `UploadProfileToneGpuData` keeps the buffer in the
  workspace values at `{drt, "profile_tone"}`.
- `edit/runtime/profile_tone_gpu_math.h`: one implementation of the stage for the host, CUDA,
  OpenCL C, and Metal (`ProfileToneApplyStage`), in the order of `ApplyProfileToneStageReference`.
- `DrtPostExecutor::Execute` resolves the stage, uploads its buffer before any GPU start, and
  passes it to `Ops::DispatchDisplayTransform` (null without the stage). The display kernels
  (`DrtKernel`, `drt_display_scene_rgba32f`, `drt_display`) run the stage before
  `DrtDisplayPixel`, so before the ACES reference gamut compression. Negative Scan
  (`film_print_scan`) calls `DrtDisplayPixel` and stays without the stage.
- `BuildFrameResultContentKeys` mixes `"profile_tone"`, the curve fingerprint, and the method into
  the DRT display key while the stage runs. The camera key (`MixCameraColorParams`) leaves the
  switch out. A document without the stage mixes nothing, so its keys stay.

Details chosen during T2:

- The stage runs before the ACES reference gamut compression. The compression leaves neutrals
  unchanged, so the neutral rendering stays exact, and it compresses the AP1 values outside the AP1
  gamut that `M^-1` can give for a saturated ProPhoto blue.
- Without the stage, the OpenCL and Metal kernels receive the parameter buffer in the stage buffer
  argument and a flag 0; the CUDA kernel receives a null pointer.
- The T1 open item on the OpenDRT entry: the kernel converts AP1 to P3-D65, but the GPU neutral ramp
  keeps the three channels within 3e-6 of a code value of each other. The achromatic port holds.

Measurement record (Debug build, RTX 3080 Laptop, camera-like curve of T1; neutral ramp 12 stops
below raw white to raw white in quarter stops; 128 color patches of
`profile_tone_gpu_test_support.hpp`; display Rec.709 gamma 2.2, 100 nits):

| Item | Backend | ACES 2.0 | OpenDRT |
| --- | --- | ---: | ---: |
| Neutral ramp, GPU display luminance against `C`, largest relative error | OpenCL | 2.5e-6 | 2.7e-6 |
| Neutral ramp, GPU display luminance against `C`, largest relative error | CUDA | 2.5e-6 | 2.7e-6 |
| Neutral ramp, against the host `N_m(stage)`, largest relative error | OpenCL | 7.5e-6 | 6.4e-6 |
| Neutral ramp, against the host `N_m(stage)`, largest relative error | CUDA | 8.0e-6 | 6.5e-6 |
| Color patches, stage on against the scalar reference, largest difference in 8-bit codes | OpenCL | 0.0008 | 0.0002 |
| Color patches, stage on against the scalar reference, largest difference in 8-bit codes | CUDA | 0.0003 | 0.0001 |
| Shared header on the host against the scalar reference, largest relative error | host | below 1e-5 | below 1e-5 |

Tests: `DcpCameraProfileRenderTest` (5 new: JSON form, dirty fields, stage rules, keys,
invalidation), `ProfileToneReferenceTest` (1 new: the shared header on the packed buffer),
`PipelineDcpProfileBindingTest` (1 new: write, Undo, Redo, replay, rejected value),
`OpenClProfileToneFixture`, `CudaProfileToneFixture`, and `MetalProfileToneFixture` (3 each: neutral
ramp, color patches, documents without the stage render the bytes of a document without a DCP
reference).

Metal: `drt.metal`, `metal_drt_pass.mm`, and `MetalProfileToneFixture` are written but not compiled
on this date; the Mac build host did not resolve on the network. The Metal run of the three fixture
tests stays open for C7.

Metal run (2026-11-12, with C7, branch `feature/dcp-profile-backend-verification`): the macOS
Debug build of `GpuDagMetalDrtTest` and `GpuDagMetalDevelopTest` compiles `drt.metal` and
`metal_drt_pass.mm` (exit 0). `MetalProfileToneFixture`: 3 discovered, 3 passed (neutral ramp,
color patches, documents without the stage).

Primary call chains:

- Render: `DrtPostExecutor::Execute` -> `ResolveProfileToneStage` -> `PackProfileToneGpuData`
  (`ProfileToneNeutralResponse::Resolve`, `ProfileToneStageMatrices`) -> `UploadProfileToneGpuData`
  -> `Ops::DispatchDisplayTransform` -> display kernel -> `ProfileToneApplyStage` ->
  `DrtDisplayPixel`.
- Identity: `BuildFrameResultContentKeys` -> `ResolveProfileToneStage` -> DRT display key;
  `CollectDevelopChanges` -> `DevelopDirty::ProfileTone` revision -> display result.
- Field: `ParseCameraProfileFileUpdate` -> `DevelopCameraProfileFileUpdate::profile_tone` ->
  `ApplyCameraProfileFileUpdate`; load: `BindDcpCameraProfile` -> stored `profile_tone`.

### T3. Editor and documents

- Switch in `EditorCameraProfileSection.qml`, list model property for curve presence, settled edit.
- Entry reset of section 6.5: `ProfileToneEntryResetTargets` and its use in `CommitAdjustment`.
  Tests next to `FilmPrintEntryResetsImportSaturationInOneUndoStep`: a DCP selection resets both
  values in one Undo step; a user value stays; a document that lacks `grade.primary` keeps its
  values; Film Print as the DRT method writes no Grade value; turning the switch off writes no
  Grade value.
- `docs/dng_color_profiles.md` and the parent plan describe the stage.
- User check on two camera makes with ACES 2.0 and OpenDRT.

#### T3 completion (2026-11-12)

Status: **implemented; the user check on two camera makes is open.** Branch
`feature/dcp-profile-tone-editor`, stacked on the T2 PR #866.

Implementation:

- `ProfileToneEntryResetTargets(document, develop_before)` in
  `app/editor_pipeline_command_service.cpp` returns `EditorEntryResetWrite` values (field target
  and patch JSON): contrast `kProfileToneEntryContrast` (0) while Color Grade 1 holds
  `kImportLookContrastBeforeFilmLike`, saturation `kProfileToneEntrySaturation` (1.0) while it holds
  `kDefaultPipelineSaturation`. The rule applies when the live document runs the stage after the
  edit and the stored reference of `develop_before` did not run it. Color Grade 1 is
  `PipelineDocument::PrimaryGrade`.
- `EntryResetWrites(document, field_key, before, after)` dispatches the field: `odt` to
  `FilmPrintEntrySaturationTarget`, `camera_profile_file` to `ProfileToneEntryResetTargets`.
  `EditorHistoryMutation::CommitAdjustment` applies each write in the batch of the edit, and skips
  a field whose input sequence is open, as the Film Print rule did.
- `EditorPanelCameraProfileFileValue` gains `has_tone_curve` (bound profile with a curve) and
  `profile_tone`; the panel presentation maps them to the same keys.
- `EditorCameraProfileListModel` gains `toneCurveAvailable`, `profileTone`, `filmPrintMethod` (from
  the `odt` entry of the snapshot), and `submitProfileTone(on)`, which submits one settled write
  `{"camera_model", "profile_name", "profile_tone"}` through `ParseEditorParameterWrite`.
- `EditorCameraProfileSection.qml`: row **Tone Curve** with a `Switch` under the combo; visible
  for `toneCurveAvailable`, disabled while `filmPrintMethod`; it follows the echo of its write.
  zh_CN: 色调曲线.
- `docs/dng_color_profiles.md` gains the section "DCP camera profiles and the profile tone stage"
  and a parameter table; the parent plan gains the switch and the entry reset in section 2.2.

Details chosen during T3:

- The signature takes the live document for the after side, where the reference is bound by the
  write, and the Develop Model JSON of the history before-side. The before reference is resolved
  through `DcpProfileLibrary::Shared()` to read whether its profile had a curve (cached after the
  first read). An identity that the library does not hold had no stage, so the edit counts as an
  entry. The `develop_after` parameter of section 6.5 is left out: the live document holds it.
- A selection of another profile keeps the switch value of the stored reference (the write adds
  `"profile_tone": false` when the switch is off). A selection from the default entry writes the
  default, true.
- The switch is a plain `Switch` driven by `submitProfileTone`, in place of
  `EditorAdjustmentToggleModel`: its `paramsBuilder` falls back to `{}` for empty params, and `{}`
  clears the reference.
- The Default document renders with OpenDRT; the ACES 2.0 case selects the method first.
- A change between two profiles that both run the stage is no entry; a change from a profile
  without a curve to one with a curve is an entry.

Primary call chains:

```text
DCP selection or Tone Curve on (QML)
  -> EditorCameraProfileListModel::submitSelection / submitProfileTone
  -> ParseEditorParameterWrite (FindSelectableDcpCameraProfile) -> submitNow(settled)
  -> EditorHistoryMutation::CaptureAdjustmentBeforePreview (Develop before JSON)
  -> ApplyEditorParameterWrite -> DevelopParamsModel::ApplyCameraProfileFileUpdate
  -> EditorHistoryMutation::CommitAdjustment
  -> EntryResetWrites -> ProfileToneEntryResetTargets
     (live stage: method, bound reference, curve; before stage: ResolveStoredDcpCameraProfileFile)
  -> ApplyEditorParameterPatch per Grade write -> one PipelineEditBatch -> PublishAppliedTypedBatch
  -> panel projection echo -> loadFromSnapshot (switch state)
```

```text
Failure: malformed before reference, a rejected Grade write, or a rejected batch
  -> restore_before (RestoreDocumentFields of the edit and each applied reset)
  -> CommitAdjustment returns false with the error; the history keeps its head
Failure: no stored reference, equal value, parse error, or a session that rejects edits
  -> submitProfileTone logs "[Camera Profile] Tone Curve ... is not applied" and returns false
  -> the Switch returns to profileTone
```

Tests (Debug build):

| Criterion | Test | Result |
| --- | --- | --- |
| A DCP selection resets both values in one Undo step (OpenDRT, then ACES 2.0) | `EditorProfileToneEntryHistoryTest.DcpSelectionResetsContrastAndSaturationInOneUndoStep`, `DcpSelectionOnAces20ResetsContrastAndSaturation` | PASS |
| A user value stays; exposure stays | `ProfileToneEntryKeepsValuesThatTheUserSelected`, `ProfileToneEntryKeepsExposure` | PASS |
| A document without Color Grade 1 keeps its values | `DocumentWithoutColorGradeOneKeepsItsValues` | PASS |
| Film Print as the DRT method writes no Grade value | `FilmPrintMethodWritesNoGradeValueOnDcpSelection` | PASS |
| Turning the switch off writes no Grade value; turning it on is an entry | `ToneCurveSwitchOffWritesNoGradeValue`, `ToneCurveSwitchOnIsAnEntry` | PASS |
| Entry only when the stage starts; a leave writes no Grade value | `ChangeBetweenProfilesThatRunTheStageIsNoEntry`, `ProfileWithoutCurveWritesNoGradeValueAndLaterCurveDoes`, `LeaveToDefaultEntryWritesNoGradeValue` | PASS |
| Projection of the curve and the switch | `EditorPanelProjectionTest.CameraProfileFileProjectsTheToneCurveAndItsSwitch` | PASS |
| List model state and the settled write | `EditorCameraProfileListModelTest.ToneCurveSwitch*` (3), `SelectionKeepsTheToneCurveSwitchOfTheStoredReference` | PASS |
| Switch in the section: visible, commit, Film Print disables, flat profile hides | `EditorRawDecodePanelQmlTest.CameraProfileToneCurveSwitchCommitsAndFollowsTheDrtMethod` | PASS |

Suites: `EditorSessionHistoryPortTest` 129/130 (`CheckoutAndPasteHashesAreUnchanged` fails before
this change, #555), `EditorPipelineCommandServiceTest` 24/24, `EditorPanelProjectionTest` 14/14,
`EditorCameraProfileListModelTest` 15/15, `EditorRawDecodePanelQmlTest` 10/10,
`PipelineDcpProfileBindingTest` 16/16. `lrelease` on `alcedo_main_zh_CN.ts`: exit 0.

LOC: `editor_history_mutation.cpp` holds 1002 lines before T3 and 1009 after; the dispatch of the
reset rules moved to `EntryResetWrites` to keep the mutation a router. The split is #867.

Open: the user check on two camera makes with ACES 2.0 and OpenDRT. The Metal run of T2 passed with C7.

#### Highlight shoulder (2026-11-12)

Status: **implemented on CUDA and OpenCL; Metal compiles the same header, run pending with the T2
Metal item.** Branch `feature/dcp-profile-tone-editor` (T3 PR #868). Design: section 6.6.

Implementation:

- `profile_tone_gpu_math.h`: `PROFILE_TONE_HIGHLIGHT_KNEE` and `ProfileToneHighlight`;
  `ProfileToneApplyStage` applies it per ProPhoto channel in place of `ProfileToneClamp01`. The
  buffer layout stays (8214 floats).
- `profile_tone_reference.hpp`: `kProfileToneHighlightKnee` and `ProfileToneHighlightShoulder`,
  in the float order of the GPU form; `ApplyProfileToneStageReference` uses it.
  `profile_tone_stage.cpp` checks that the two knees agree.
- `kProfileToneImplementationVersion` = 2 (`content_key.hpp`): the DRT display key mixes it only
  while the stage runs, so every other document keeps its keys.
- `scripts/survey_dcp_tone_curves.py` prints the tables of section 6.6.

Measurement record (Debug build, RTX 3080 Laptop, camera-like curve of T1; neutral ramp now 12
stops below raw white to 4 stops above it):

| Item | ACES 2.0 | OpenDRT |
| --- | ---: | ---: |
| Host: neutral error of `N_m(stage(v))` against `C(H(v))`, relative | 3.1e-5 | 2.4e-5 |
| OpenCL: neutral ramp against `C(H(v))` / against the host `N_m(stage)`, relative | 6.0e-6 / 7.5e-6 | 6.6e-6 / 6.4e-6 |
| CUDA: neutral ramp against `C(H(v))` / against the host `N_m(stage)`, relative | 6.4e-6 / 8.0e-6 | 6.9e-6 / 6.5e-6 |
| OpenCL / CUDA: color patches against the scalar reference, 8-bit codes | 0.0008 / 0.0003 | 0.0002 / 0.0000 |

CIEDE2000 against the SDK-style rendering (T1 patch set, ACES 2.0): mean 1.73, p95 4.47, max 6.72
after the entry reset (T1 with the clip: 2.17, 6.69, 16.08); 3.49, 7.59, 9.28 before the reset
(T1: 4.85, 15.25, 26.01). The neutral patches below the knee stay below 0.1; the neutral patch
0.75 stops below raw white measures 0.25, the shoulder's deviation from the SDK curve there. The
largest error moves from the bright violet to ProPhoto (0.904, 0.595, 0.286), a bright orange.

Tests: `ProfileToneReferenceTest` (11; new: `HighlightShoulderKeepsTheCurveBelowTheKneeAndStaysBelowWhite`;
`SceneLightAboveRawWhiteRendersAtThePeak` becomes `SceneLightAboveRawWhiteRollsOffTowardThePeak`),
`DcpCameraProfileRenderTest` 24/24, `CudaProfileToneFixture` 3/3, `OpenClProfileToneFixture` 3/3.

Primary call chain:

```text
DrtPostExecutor::Execute -> ResolveProfileToneStage -> PackProfileToneGpuData
  -> display kernel -> ProfileToneApplyStage
     -> ProfileToneMatrix (AP1 to ProPhoto) -> ProfileToneHighlight per channel (anchor, H)
     -> ProfileToneBaselineRgb (C) -> ProfileToneInverse per channel -> ProfileToneMatrix (to AP1)
  -> DrtDisplayPixel
```

## 9. Changes to the parent plan

- P-5 reads: "`ProfileToneCurve` applies through the profile tone stage of the
  [DCP Profile Tone Curve Study](dcp_profile_tone_curve_study.md)."
- Section 3.2 lists the remaining exclusions: the ACR3 default curve, automatic black
  (`DefaultBlackRender` absent), and the curve of an embedded DNG profile.
- C7 depends on T3.

## 10. Terms and parameters

| Symbol or parameter | Meaning | Unit | Value | Source |
| --- | --- | --- | --- | --- |
| `ProfileToneCurve` | DNG tag 50940, `(x, y)` FLOAT pairs | display light over scene value | per profile | DNG specification |
| `x` | AP1 scene-linear RGB at the DRT entry | relative scene light | per pixel | Alcedo |
| `M` | AP1 to linear ProPhoto RGB (D50) | matrix | camera-pass matrix | Alcedo |
| `E_a` | Exposure anchor between Alcedo and ACR | EV | 1.5 (`kDefaultPipelineExposureEv`) | Data, section 3.3 |
| `C` | Profile tone curve as a spline table | display light | 4097 entries on `[0, 1]` | DNG SDK |
| `H` | Highlight shoulder at the curve input, per ProPhoto channel | ACR scale (raw white 1) | identity to `k`, then `1 - (1 - k)^2 / (v + 1 - 2k)`, limit 1 | Section 6.6 |
| `kProfileToneHighlightKnee`, `PROFILE_TONE_HIGHLIGHT_KNEE` | Knee `k` of `H` | ACR scale | 0.5, one stop below raw white | Section 6.6 |
| `kProfileToneImplementationVersion` | Version of the stage pixel rules in the DRT display key, mixed only while the stage runs | integer | 2 (the shoulder) | Section 8 |
| `R_C` | Baseline RGB tone with `C` | display light | per pixel | DNG SDK `RefBaselineRGBTone` |
| `N_m` | Neutral response of DRT method `m`, default parameters, SDR reference | display light relative to peak | per method | Alcedo DRT |
| `N_m^-1` | Inverse of `N_m`, held at the peak | relative scene light | 4097 entries on `d^(1/2.4)` | Inferred |
| SDR reference display | Display for `N_m` | nits | 100 | ACES 2.0 SDR |
| ACES 2.0 scene value at the SDR peak | `r_hit` at 100 nits | relative scene light | 128 | OpenColorIO ToneScaleParams |
| ACES 2.0 scene 0.18 at 100 nits | Mid grey rendering | display light | 0.100 | OpenColorIO ToneScaleParams |
| `profile_tone` | Key of `camera_profile_file`; written only as `false` | boolean | default true | Section 6.3 |
| `kProfileToneEntryContrast` | Contrast of Color Grade 1 after a profile tone entry | slider value | 0 (catalog identity) | User decision, section 6.5 |
| `kProfileToneEntrySaturation` | Saturation of Color Grade 1 after a profile tone entry | multiplier | 1.0, as `kFilmPrintEntrySaturation` | User decision, section 6.5 |
| `kImportLookContrastBeforeFilmLike` | Contrast of the RAW import look | slider value | 15 | `pipeline_document.hpp` |
| `kDefaultPipelineSaturation` | Saturation of the RAW import look | multiplier | 1.3 | `pipeline_document.hpp` |
| `DngToneCurve` | Immutable curve value with its own fingerprint | value type | per profile | Section 6.3 |
| `kMaxDngToneCurvePoints` | Largest point count of a curve that the reader accepts | points | 4096 | T1 choice; user folder maximum 256 |
| `kProfileToneInverseTableEntries` | Entries of the `N_m^-1` table | entries | 4097 | Section 6.3 |
| `PROFILE_TONE_GPU_FLOATS` | Size of the stage buffer: two matrices, anchor scale, inverse exponent, `C` table, `N_m^-1` table | floats | 8214 | T2, `profile_tone_gpu_math.h` |
| `DevelopDirty::ProfileTone` | Develop dirty field of a change of `profile_tone` alone; invalidates the display result only | bit | `1 << 5` | T2 |
| `tn_off` | OpenDRT scene offset; `-tn_off` is the OpenDRT scene value at black | relative scene light | 0.005 | OpenDRT Standard look |
| Surround divisor | OpenDRT divides the contrast by it for the average surround of Rec.709 gamma 2.2 | factor | 1.1 | `open_drt_runtime.cpp` |
| OpenDRT scene 0.18 at 100 nits | Mid grey rendering of the OpenDRT defaults | display light | 0.124 | T1 measurement |
| OpenDRT scene value at the SDR peak | Scene value at which `N_OpenDRT` reaches display light 1 | relative scene light | 117.62 | T1 measurement |
| Raw white | Sensor saturation after black subtraction | relative scene light | 1.0 before the default exposure | Alcedo RAW scale |
