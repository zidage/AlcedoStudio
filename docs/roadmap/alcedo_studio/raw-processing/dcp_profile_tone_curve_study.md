# DCP Profile Tone Curve Study

Date: 2026-11-12

Status: **Study complete; design approved** (2026-11-12, with the entry reset of section 6.5 from
the user). Phases T1 to T3 (section 8) are planned: T1 #862, T2 #863, T3 #864. Tracking issue
#851.

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
- `R_C`: baseline RGB tone with `C` (section 2, step 4).
- `N_m`: neutral response of DRT method `m` with its product default parameters at the SDR
  reference display: scene luminance to display light relative to the peak.
- `N_m^-1`: its inverse, held at the scene value of the peak for display light 1.

Stage:

```
p      = clamp(M * x * 2^-E_a, 0, 1)        ProPhoto, ACR exposure scale, ramp clip
d      = R_C(p)                              display light, profile rendering
s      = N_m^-1(d)                           per channel, back to scene light of method m
x_out  = M^-1 * s                            AP1 into the DRT
```

At the default DRT parameters and the SDR display, a neutral input renders exactly as the profile
curve: `N_m(N_m^-1(C(v))) = C(v)`. Colored input takes the profile's display values through the
DRT's own chroma model.

### 6.2 Rules

| Case | Behavior |
| --- | --- |
| DRT ACES 2.0 | `N` = ACES 2.0 forward on a neutral at 100 nits, from `aces2_reference_math.h`. |
| DRT OpenDRT | `N` = OpenDRT achromatic path with the `DrtPayload` default parameters at 100 nits. A changed tone scale control acts relative to the profile tone: `N_user(N_default^-1(C))`. |
| DRT Film Print | The stage stays out of the render (K-6). The stored setting stays. |
| HDR display | `N` stays at the SDR reference; the HDR DRT renders `x_out`. ACES 2.0 at 1000 nits renders the profile white (scene 128) at 962 nits. |
| Scene light above the curve domain | Clips at display white, as the SDK exposure ramp does. Exposure and Highlights pull values back below raw white. |
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
Default document (contrast `kDefaultPipelineContrast`, saturation `kDefaultPipelineSaturation`)
adds a second contrast and color on top. The stage takes the rule of the Film Print entry
(`FilmPrintEntrySaturationTarget`, user decision 2026-11-12):

- **Profile tone entry**: a settled edit of `camera_profile_file` after which the stage runs and
  before which it stayed out of the render. The stage runs when the reference is bound, its profile
  has a curve, `profile_tone` is true, and the DRT method is ACES 2.0 or OpenDRT. A DCP selection
  from the default entry and the Tone Curve switch turned on are both entries.
- On an entry, when **Color Grade 1** (`grade.primary`, `PipelineDocument::PrimaryGrade`) exists,
  the same history batch writes its contrast to `kProfileToneEntryContrast` while it still equals
  `kDefaultPipelineContrast`, and its saturation to `kProfileToneEntrySaturation` while it still
  equals `kDefaultPipelineSaturation`. A value that the user selected stays. A document that lacks
  `grade.primary` keeps every Grade value.
- An input sequence that is open on contrast or saturation keeps its value, as for Film Print.
- One Undo restores the profile reference and both values; Redo writes them again.
- Exposure stays. The anchor `E_a` makes the stored +1.5 EV the profile's own exposure
  (section 3.3), so a user exposure keeps its meaning with the stage on or off.
- A leave (stage on to stage off) writes no Grade value, as Film Print does.

The function `ProfileToneEntryResetTargets(document, develop_before, develop_after)` in
`app/editor_pipeline_command_service.cpp` returns the targets; `EditorHistoryMutation::
CommitAdjustment` appends them to the batch of a `camera_profile_file` commit.

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

### T2. Render

- `camera_profile_file.profile_tone` parse and write (`false` only), history comparison, binding.
- DRT compile data and result identity (section 6.3); the Film Print rule (section 6.2).
- The stage as the first operation of the display transform on CUDA, OpenCL, and Metal.
- Tests: a neutral ramp per backend against the host `N_m(stage)`; GPU against the scalar
  reference on color patches; a DCP that lacks the curve and every old document render with their current bytes; a switch
  reuses the camera-pass result.

### T3. Editor and documents

- Switch in `EditorCameraProfileSection.qml`, list model property for curve presence, settled edit.
- Entry reset of section 6.5: `ProfileToneEntryResetTargets` and its use in `CommitAdjustment`.
  Tests next to `FilmPrintEntryResetsImportSaturationInOneUndoStep`: a DCP selection resets both
  values in one Undo step; a user value stays; a document that lacks `grade.primary` keeps its
  values; Film Print as the DRT method writes no Grade value; turning the switch off writes no
  Grade value.
- `docs/dng_color_profiles.md` and the parent plan describe the stage.
- User check on two camera makes with ACES 2.0 and OpenDRT.

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
| `R_C` | Baseline RGB tone with `C` | display light | per pixel | DNG SDK `RefBaselineRGBTone` |
| `N_m` | Neutral response of DRT method `m`, default parameters, SDR reference | display light relative to peak | per method | Alcedo DRT |
| `N_m^-1` | Inverse of `N_m`, held at the peak | relative scene light | 4097 entries on `d^(1/2.4)` | Inferred |
| SDR reference display | Display for `N_m` | nits | 100 | ACES 2.0 SDR |
| ACES 2.0 scene value at the SDR peak | `r_hit` at 100 nits | relative scene light | 128 | OpenColorIO ToneScaleParams |
| ACES 2.0 scene 0.18 at 100 nits | Mid grey rendering | display light | 0.100 | OpenColorIO ToneScaleParams |
| `profile_tone` | Key of `camera_profile_file`; written only as `false` | boolean | default true | Section 6.3 |
| `kProfileToneEntryContrast` | Contrast of Color Grade 1 after a profile tone entry | slider value | 0 (catalog identity) | User decision, section 6.5 |
| `kProfileToneEntrySaturation` | Saturation of Color Grade 1 after a profile tone entry | multiplier | 1.0, as `kFilmPrintEntrySaturation` | User decision, section 6.5 |
| `kDefaultPipelineContrast` | Contrast of the RAW import look | slider value | 15 | `pipeline_document.hpp` |
| `kDefaultPipelineSaturation` | Saturation of the RAW import look | multiplier | 1.3 | `pipeline_document.hpp` |
| `DngToneCurve` | Immutable curve value with its own fingerprint | value type | per profile | Section 6.3 |
| Raw white | Sensor saturation after black subtraction | relative scene light | 1.0 before the default exposure | Alcedo RAW scale |
