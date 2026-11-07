# Film Grain Format and Stock Granularity Plan

Date: 2026-11-05

Status: **In progress.** G0 is complete in the lab ([alcedo-film-lab #2](https://github.com/zidage/alcedo-film-lab/pull/2)). G1 is complete
(2026-11-05): the host plan, the shared grain math, and the bundle schema 11. G2 is complete (2026-11-05): the field
`film_format`, the Film Grain range 0 to 200 %, the Format selector of both panels, and the compatibility tests. G3 is complete
(2026-11-05): the CUDA post grain with the size and the strength of the plan, and decision U5 (the #660 colour). G5 is complete
(2026-11-06): on CUDA the film print adds the grain to the dye amounts of its negative and skips the post Film Grain. G7 is complete
(2026-11-06): the film format sets the frame of the film print image plane on all backends. The user approved the plan and closed decisions U1 to U4 on
2026-11-05 (section 2.3). Section 2.4 states the compatibility requirement of the user. The G0 results changed some designs of
this plan; the sections below hold the changed form, and the G0 completion record lists each change.

Parent: [Roadmap](../../roadmap.md), section "Still Planned".

Related plans:

- [Film Print DRT Plan](film_print_drt_plan.md): this plan extends it. The film print gets grain in the negative, and the film format
  sets the frame of its image plane (decision U2).
- [Film Simulation Explained](film_simulation/film_simulation_explained.md): the model and the flow of the film print. Phase G8
  adds a grain section to it.

Research source: [alcedo-film-lab](https://github.com/zidage/alcedo-film-lab). Phase G0 adds the grain data and the reference
evaluator there.

Source audit revision: `f28ca394` on `main` (2026-11-05). All source paths are relative to `alcedo_studio/src/` unless the path
starts with `alcedo_studio/` or `docs/`.

Before each phase, read `AGENTS.md` and each applicable skill again. These rules directly affect this work:

- No fallback without user authorization. A failure gives the real error.
- No stored expected pixels. Grain tests are statistical tests.
- A change of the `ToJson` bytes of a Model breaks the root id of old projects. Unedited old JSON stays byte-identical.
- New GPU work is for CUDA, OpenCL, and Metal only.
- Run only the targeted test binaries. Only the user starts a full test suite.
- No phase id in a pull request title. No prohibited terminology.

---

## 1. Summary

The current Film Grain has a fixed size of 0.8 source pixels and one fixed strength curve, the Kodak 5207 datasheet. The grain
therefore has the same size in pixels for a 12 MP and a 100 MP image, and a different size relative to the frame. The grain is
the same for every film.

This plan gives the grain a physical size on the film. The user selects the film format: 16mm, 35mm, or 70mm. The format sets
the frame size, and the frame size and the image size set the pixel pitch on the film. Each film stock brings its own datasheet
granularity, so the strength follows the film. With the film print, the grain goes into the negative, before the print or the
scan, as in a real film.

Main rules of the model:

1. The grain field has a fixed size on the film, in micrometres. A smaller frame shows the same grain larger and stronger.
2. The strength comes from the RMS granularity of the stock datasheet, through the 48 um aperture. The Selwyn relation and
   an exact pixel integral convert it to the pitch of each render.
3. The grain is a Gaussian random field. Its statistics are the same at each zoom level, and its pattern is the same in a
   region of interest (ROI) and in the full frame.
4. Each frame uses one hash per pixel and two separable blur passes. No pass draws one random value per tap.

---

## 2. Decision record

### 2.1 User request (2026-11-05)

| Item | Request |
| --- | --- |
| Format | Add three grain sizes: 16mm, 35mm, and 70mm. Each format has its own grain size. |
| Realism | Use the model that is closest to real film. Keep the algorithm fast. |
| Stock | Adjust the grain strength automatically from the film stock. |

### 2.2 Design decisions of this plan

| Item | Decision | Reason |
| --- | --- | --- |
| Grain size | A fixed size on the film in micrometres, from the dye-cloud size of the stock (section 3.3). | Grain is a property of the film. The frame size sets its size in the image. |
| Pixel pitch | `pitch_um = frame_long_side_mm * 1000 / reference_long_side_px`. | The same rule as the image plane of the film print (`film_print_plan.cpp`). A crop enlarges the grain, as an enlargement of a part of a negative does. |
| Strength | The datasheet RMS granularity, converted to the render pitch by an exact pixel integral (section 3.2). | Selwyn's law holds only for an aperture much larger than the cloud. At 16mm the pixel is near the cloud size. |
| Strength control | Film Grain stays one slider. 100 % is the granularity of the film on its datasheet. | Section 2.3, decision U3. |
| Grain with the film print | The grain is added to the dye amounts of the negative before the print table, the scanner table, or the light table. The DRT/Post Film Grain pass is then skipped. | Section 2.3, decision U4. The print contrast and the dye spectra then give the tone and the colour of the grain. |
| Grain with other methods | ACES 2.0 and OpenDRT keep the display-referred grain of #660, with the size and strength rules of this plan and the 5207 data. | These methods have no negative. The colour rules of #660 come from measured 5207 scans. |
| Random field | A Gaussian field from a 32-bit hash per pixel and a separable Gaussian blur. | Section 3.4: about 300 dye clouds overlap at each point, so the density is Gaussian. A 32-bit hash is fast on Metal. |
| Zoom | The hash reads the reference pixel. A render below full size sums k x k reference hashes (k up to 4). | The fit view then shows the grain of the downscaled full image. Section 5.1 records the current defect. |

Rejected work that must not return:

- A grain size in source pixels.
- A Bernoulli draw for each blur tap.
- A stored grain texture with a fixed pixel size.
- A per-stock grain look fitted by eye in place of the datasheet value. The user review in G8 can change the slider, not
  the data.

### 2.3 User decisions (closed 2026-11-05)

The user accepted each recommendation. Each row is now an approved decision.

| Id | Question | Decision |
| --- | --- | --- |
| U1 | Frame long side of each format. | 16mm: 12.52 mm (Super 16 camera aperture). 35mm: 36 mm (the 135 frame and the current film print frame `common/frame_mm`). 70mm: 70 mm (the 6x7 frame of 120 film, 69.5 mm, and the 15/70 frame, 70.4 mm). A Super 35 cine frame (24.89 mm) is between 16mm and 35mm. |
| U2 | The format also sets the frame of the film print image plane: light spread, halation, and the inhibitor. | Yes. A real 16mm frame is softer and has a larger halation relative to the frame. With the 36 mm frame alone, 16mm grain has 35mm sharpness. Phase G7. |
| U3 | The range and the meaning of the slider. | The stored key stays `strength`. The range becomes 0 to 2, shown as 0 to 200 %. The value 1 is the datasheet granularity of the film. Old documents keep their stored value and render with the new model. By the estimate of section 3.5, the old value 1 and the new value 1 give about the same grain at 6000 px on 35mm. |
| U4 | With the film print, the grain goes into the negative and the post Film Grain pass is skipped. | Yes. This follows the rule of Halation: the film model makes the effect from the film layers, and the stored value of the post operator stays. Here the film print reads the same slider. |
| U5 | The colour of the post grain (closed 2026-11-05, in G3). G0 changed the post mix to the display correlation of 5207 on Endura Premier, but section 2.2 keeps the #660 colour, and G3 requires the #660 colour statistics. With the Endura correlation the lightness-warmth correlation of #660 is negative (grain that darkens is warm). | The #660 scan colour: the channel correlation rg 0.661, gb 0.41, rb 0.246 of the 5207 scans, and the ratio r : g : b = 1.2 : 1 : 1.4 at the 48 um aperture. The green level is the G0 target, `grain_post_display_sigma_48` / `grain_post_display_value` of green. The Endura display correlation stays in the bundle for the film print, which makes its colour from its layers (G5). |

### 2.4 Compatibility requirement (user, 2026-11-05)

Every project made before this plan opens with no error. The rendered look of its grain can change (U3). These items are required:

| Item | Rule | Check |
| --- | --- | --- |
| Document bytes | An unedited document serializes to the same bytes. `film_format` is written only when it is not `35mm`. `strength` keeps its key and its value. | G2: `ExistingFilmGrainRootDocumentsSerializeByteIdenticalAfterFormatChange` |
| Root id | `ComputeRootId` of an old root gives the same id, so `TryDecodeRootState` accepts it. | Same test |
| History | An old `SetParameter` entry of Film Grain or of the DRT node replays: its `before_value` equals the current Model JSON byte for byte. Undo and redo of an old entry work. | G2: `ExistingFilmGrainHistoryReplaysAfterFormatChange` |
| Values | An old `strength` in 0 to 1 is in the new range 0 to 2. No value is clamped or rewritten on load. | G2: catalog and parser tests |
| Film print documents | An old document with method `film_print` reads `35mm`, so G7 gives its image plane the same values as before. | G7: the `35mm` plan equals the current plan exactly |
| Resources | Schema 11 resources ship with the application. A project stores no resource data, so no project depends on the schema. | G1: `FilmPrintTablesTest` |

The pre-change documents for these tests come from the build of `main` before G2: one root and one checkpoint with method `film_print`
and Film Grain 0.6, and one with OpenDRT and Film Grain 0.3, each with one Film Grain history entry. G2 stores them in
`alcedo_studio/tests/edit/history/expected_serialized/` as `drt_film_grain_root_before_film_format.json`,
`drt_film_grain_checkpoint_before_film_format.json`, and `drt_film_grain_history_before_film_format.json` (the stored entries), as the
existing `raw_root_before_raster_input.json` pattern does. These files hold serialized documents, not pixels.

---

## 3. Physical model

This section defines each parameter before it uses it. Section 11 lists all parameters.

### 3.1 The grain of a colour negative

A colour negative has three layers. Each layer has two or three sublayers of different speed. In development, each developed
silver halide crystal makes a dye cloud. The clouds of one layer overlap. The density of a layer at a point is the sum of the
clouds there. The three layers develop independently, so their grain is spatially independent. The dyes absorb also outside
their main band, so a densitometer channel reads some grain of the other layers.

The datasheet gives the RMS granularity `sigma_48`: the standard deviation of the diffuse density, read with a
microdensitometer through a circular aperture of 48 um diameter, on a uniform exposure. Kodak gives `sigma_48` as a curve over
the density for each layer (for example `Kodak_Vision3_250D.pdf`). A Kodak still film gives the Print Grain Index (PGI) in
place of `sigma_48` (section 4.3).

### 3.2 From the datasheet to a pixel

Parameters:

- `A_48 = pi * (24 um)^2`: the area of the datasheet aperture.
- `G = sigma_48 * sqrt(A_48)`: the Selwyn granularity, in density times micrometres. It is a property of the film.
- `p`: the pixel pitch on the film, in micrometres.
- `sigma_c`: the size of the dye cloud as the sigma of an equivalent Gaussian, in micrometres (section 3.3).

The density field of one layer is white noise with the spectral density `G^2`, blurred by the cloud. A pixel integrates the field
over a square of side `p`. The standard deviation of one pixel is:

```text
sigma_px = G * I(sigma_c, p)
I(sigma_c, p) = integral over f of exp(-4 pi^2 sigma_c^2 f^2) * sinc^2(p f) df      (one axis, f in cycles per um)
```

For a large pixel, `I` goes to `1 / p`, and `sigma_px = G / p`: Selwyn's law. For a pixel near the cloud size, `I` is smaller,
and the pixel reads the cloud itself. The host computes `I` once per render geometry.

The renderer makes the field from a unit white noise per pixel, blurred by a discrete Gaussian of `sigma_b` pixels with the
weights `w` (radius `ceil(3 sigma_b)`, at most 24). Rule `grain_kernel`: `sigma_b` is the value that gives the discrete field the
lag-1 correlation of the continuous pixel field, `rho1 = C(p) / C(0)`, with
`C(d) = integral of tri_p(u) N(d - u; 0, 2 sigma_c^2) du` (closed form in the lab evaluator; the host finds `sigma_b` by
bisection). The blur gives the variance `(sum w^2)^2`. The host multiplies the field by `sigma_px / (sum w^2)`, so the pixel
standard deviation is exact at every pitch, and the 2 x 2 box ratio `(1 + rho1) / 2` equals `I(2p) / I(p)`. The first form,
`sigma_b = sigma_c / p`, omits the pixel box: below 1 pixel it gives a lag-1 correlation up to 0.28 too low, and the zoom check of
G3 fails by 21 % (lab P15 report, section 5).

Values for the 5207 green layer near mid density (`sigma_48 = 0.0070`, script in G0):

| Format | 2560 px | 6000 px (24 MP) | 9504 px (61 MP) |
| --- | --- | --- | --- |
| 16mm (12.52 mm) | p 4.89 um, sigma_px 0.034 | p 2.09 um, sigma_px 0.040, blur 0.96 px | p 1.32 um, sigma_px 0.041, blur 1.52 px |
| 35mm (36 mm) | p 14.06 um, sigma_px 0.018 | p 6.00 um, sigma_px 0.031, blur 0.33 px | p 3.79 um, sigma_px 0.037, blur 0.53 px |
| 70mm (70 mm) | p 27.34 um, sigma_px 0.010 | p 11.67 um, sigma_px 0.021, blur 0.17 px | p 7.37 um, sigma_px 0.028, blur 0.27 px |

The table uses `sigma_c = 2.0 um`, and its blur column is `sigma_c / p`. Rule `grain_kernel` gives `sigma_b` 1.00 px (16mm,
6000 px), 0.51 px (35mm, 6000 px), and 0.42 px (70mm, 6000 px); `grain__expected.bin` of G0 holds the values of the tests. At the
same view size (2560 px), 16mm has 1.9 times and 70mm has 0.56 times the grain of 35mm.
With `sigma_c = 3.0 um` the ratios are 1.57 and 0.59. The ratio of the frame sizes, 2.88, applies only when the pixel is much
larger than the cloud.

### 3.3 The size of the dye cloud

Sources:

| Source | Value | Gaussian sigma |
| --- | --- | --- |
| Wiener spectrum model of colour negative film (J. Photogr. Sci., 1990s; two diameters fit measured spectra) | One cloud 6.5 um, one cluster 15 um | About 1.6 um and 3.8 um (sigma = diameter / 4) |
| Jarvis, coupler-starved C-41 layer (J. Photogr. Sci. 40:105, 1992; 43:136, 1995), exponential cloud `exp(-r / k)` | k = 1.45 um | 2.05 um (same noise area: sigma = sqrt(2) k) |
| fotufilm profiles (CC BY-SA 4.0) | Clump radius 4 to 5 um | 2.0 to 2.5 um |
| spektrafilm `GrainParams` | Blue particle area 2 times red and green | Blue about 1.4 times red and green |

Values: `sigma_c` = 2.0 um for red and green and 2.8 um for blue. G0 result: the 5207 scans give a total width of 5.9 um (R),
7.7 um (G), and 7.8 um (B), the cloud and the scanner blur together. The scans have no sharp edge for the scanner MTF, so the width
is an upper limit only. The product uses the literature values and no cluster term.
A Gaussian cloud profile fits photomicrographs better than a sharp disc (J. Photogr. Sci., 1985).

### 3.4 Why a Gaussian field

For overlapping clouds of area `a` and density `d0` with `n` clouds per square micrometre, the mean density is `D = n a d0` and
`G^2 = n a^2 d0^2`. Then `n = D^2 / G^2`, with no dependence on the cloud size. For the 5207 green layer (`D = 1`, `G = 0.30`), `n`
is about 11 clouds per square micrometre, and with `sigma_c = 2 um` about 500 clouds overlap at each point. The skew of the
density is then near 0, so a Gaussian field is correct. A Gaussian field also needs no per-tap random draw.

### 3.5 Calibration of the post path against the old grain

The old post grain at slider value `s` gives a relative display error of about `0.05 s` per pixel at mid grey, at the source
resolution (unit field standard deviation 0.354 after the 7-tap blur of sigma 0.8, times the channel terms of
`film_grain_math.h`). The 5207 field of section 3.2 at 6000 px on 35mm gives 0.031 negative density. Through a print slope of
about 2.7 and the display encoding, this is about 0.07 relative display error. The new value 1 is therefore near the old value 1
at 24 MP. G0 computes the exact target of the post path: at 35mm and mid grey, the post path gives the display standard
deviation of the film print path with 5207 on Endura Premier.

G0 result (linear display, 48 um aperture, 18 % grey, reference light): display value 0.188 / 0.191 / 0.172 (R, G, B), standard
deviation 0.0107 / 0.0100 / 0.0152, display correlation rg 0.535, gb 0.509, rb 0.087. The post path takes the green level of
this target, and the colour of the 5207 scans of #660 (decision U5): the scan correlation and the ratio 1.2 : 1 : 1.4.

---

## 4. Stock granularity data

### 4.1 Data in the stock bundle

G0 adds a group `grain/<stock>` for each film stock to `film_stock_bundle.bin`, resource schema 11:

| Field | Shape | Meaning |
| --- | --- | --- |
| `layer_amount`, `layer_sigma_amount` | (3, 16) | Standard deviation of the dye amount of each layer at 48 um against the dye amount, solved from `sigma_48` (section 4.4). A reversal film reads `dye_max - record`. |
| `channel_density`, `channel_sigma_48` | (3, 16) | `sigma_48` of each channel against the gross density of the read condition of the source (Status M; Status A for a reversal film) |
| `layer_node_count`, `channel_node_count` | (3), int32 | The counted nodes of each curve. The nodes after the count repeat the last node. |

Values for all stocks:

| Field | Shape | Meaning |
| --- | --- | --- |
| `common/grain_cloud_sigma_um` | (3) | `sigma_c` of each layer: 2.0, 2.0, 2.8 |
| `common/grain_layer_correlation` | (3) | rg, gb, rb of the layer correlation R (section 4.4) |
| `common/grain_aperture_area_um2` | () | `A_48` |
| `common/grain_post_display_value`, `common/grain_post_display_sigma_48`, `common/grain_post_display_correlation` | (3) | Target of the post path (section 3.5) |

The bundle metadata `grain` holds the tier, the source, and the rule of each stock. `n` is at most 16 nodes. The host packs the curves into the kernel parameters as literal segments, as `FilmGrainLayerSigma` does
today, so no backend indexes an array per pixel.

### 4.2 Source tiers

| Tier | Data | Stocks | Rule |
| --- | --- | --- | --- |
| `curve` | Datasheet curves per layer | `kodak_5207` (in `film_grain_math.h`), `kodak_5203`, `kodak_5213`, `kodak_5219` (`Kodak_Vision3_*.pdf`), `kodak_exr_5248` (`EXR_5248_100T.pdf`), `kodak_5247` (`ECN_5247.pdf`), `kodak_5293` (`Kennel_Sehlin_etalNeg5293_1982.pdf`) | Digitize each curve. Check the read density against the characteristic curve of the same sheet. |
| `number` | One published value | `fuji_*` negatives, `kodak_5206`, `kodak_5277`, `kodak_5250`, `kodak_5248`, `kodak_5247_II`, `kodak_vericolor_iii`, `kodak_aerocolor`, `agfa_vista_100`, and the five reversal films | Scale the 5207 curve shape of the same layer to the value at the read density of the source. Reversal films read at gross density 1.0. |
| `pgi` | Print Grain Index only | `kodak_portra_160`, `kodak_portra_400`, `kodak_portra_800`, `kodak_ektar_100`, `kodak_gold_200`, `kodak_ultramax_400` | Section 4.3. |

G0 result: 8 `curve` stocks (5203, 5206, 5207, 5213, 5219, 5277, 5293, EXR 5248: vector charts), 9 `number` stocks, 6 `family`
stocks (the value of a named related stock: 5247 II and 5250 from 5247, Natura 1600 from Superia 1600, both Eterna 500 from F-250,
Kodak 5248 from EXR 5248), 7 `pgi` stocks (with Vericolor III), and 5 `reversal` stocks (rule `reversal_nutting`,
`sigma = v sqrt(D_gross)`). The Fuji values are about half of the Kodak values of a similar speed; the Fuji sheets state no aperture,
and the values are used as published.

The datasheets in `D:\Projects\spectral_film_lut\datasheets` cover the `curve` tier and Ektar 100, Portra 400, and Gold 200
(`E-7022`). G0 collects the other Kodak, Fuji, and Agfa sheets and records the source of each value. The fotufilm packs
(`grainStrength`, CC BY-SA 4.0) are a check value; the Portra 400 value is the PGI anchor (section 4.3).

### 4.3 Print Grain Index

Kodak E-58 (July 2000) defines the PGI:

- PGI rises with the logarithm of the visual granularity of the print. Two units are a 50 % just-noticeable difference.
- A change of 6 % of the granularity gives 2 units (Cookingham and Kane, US 5629769). So
  `PGI = c + 34.3 ln(sigma_visual)`, with `34.3 = 2 / ln(1.06)`.
- The print granularity is linear in the negative granularity for most colour negative films.
- The common viewing distance is 14 inches for all print sizes.

At the same magnification, the ratio of two films is therefore `G_1 / G_2 = exp((PGI_1 - PGI_2) / 34.3)`. Compare only values
above 25, at the same magnification on the 135 frame:

| Film | PGI on 135 | G relative to Portra 400 |
| --- | --- | --- |
| Portra 400 (`Kodak_Portra_400.pdf`) | 37 at 4.4X, 59 at 8.8X, 89 at 17.8X | 1.00 |
| Ektar 100 (`Kodak_Ektar_100.pdf`) | 38 at 8.8X, 66 at 17.8X | 0.54 at 8.8X, 0.51 at 17.8X |
| Gold 200 (`E-7022`) | 44 at 4.4X | 1.23 at 4.4X |

The two Ektar ratios differ by 6 %, which supports the rule.

The PGI scale has an unknown constant `c`, so it gives no absolute value. G0 finds the anchor in this order:

1. A Kodak colour negative still film with a published `sigma_48` and a published PGI (older E-series sheets, for example
   Portra 160NC and 400NC of 1998 to 2000, or Kodak E-143).
2. If none exists: the Portra 400 value 0.009 of fotufilm, recorded as an external value with no primary source, and checked in the
   user review of G8.

G0 result: no collected sheet gives both values, so the anchor is the fotufilm value (rule `pgi_anchor_external`; attribution in the
lab THIRD_PARTY_NOTICES). Green `sigma_48` at net density 1.0: Portra 160 0.0069, Portra 400 0.0090, Portra 800 0.0123, Ektar 100
0.0047, Gold 200 0.0110, Ultramax 400 0.0117, Vericolor III 0.0095.

### 4.4 From channel granularity to layer noise

The grain of the film print goes into the dye amount `a_k` of layer `k`. Parameters:

- `s_jk`: the density in densitometer channel `j` of one unit of dye `k`, from the dye spectra of the bundle and the read
  condition of the source (Status M for Kodak cine sheets).
- `sigma_a_k`: the standard deviation of the dye amount of layer `k` for the 48 um aperture.

The datasheet curves read a neutral exposure. With independent layers, `sigma_48_j^2 = sum over k of s_jk^2 sigma_a_k^2`. G0 solves
this 3x3 linear system in the squares at each density node, with the condition `sigma_a_k^2 >= 0`. The product then reproduces the
datasheet value in each channel, and the dye side absorption gives the channel correlation.

The 5207 scans give the channel correlation rg 0.661, gb 0.41, rb 0.246 (study of #660). G0 rendered the independent layers through
Negative Scan of 5207: rg 0.255, gb 0.178, rb 0.044. The first form of this plan, one shared field
(`N_k = sqrt(1 - rho) N_k,own + sqrt(rho) N_shared`), cannot give the target: `rho = 0.466` matches rg and gives gb 0.62 and rb 0.53.
G0 fitted a layer correlation matrix R (unit diagonal; rg 0.489, gb 0.2349, rb 0.1743; eigenvalues 0.51, 0.87, 1.62). R gives the
three targets exactly. Rule `grain_layer_mix`: the renderer mixes the three unit fields after the blur and the division,
`N = L u`, with L the lower Cholesky factor of R, so the pixel correlation of the layers is R exactly. The noise image has three
channels. R of 5207 holds for all stocks (no other stock has a measured scan). The post path mixes with the channel correlation of
the 5207 scans (rg 0.661, gb 0.41, rb 0.246) in place of R (decision U5).

---

## 5. Current source audit

### 5.1 Source facts

| Area | Current owner and path | Current behavior | Required change |
| --- | --- | --- | --- |
| Grain parameters | `edit/runtime/adjustment_runtime.cpp`, `MakeNeighborhoodParams` | `amount = strength / 3`. Blur `sigma = 0.8 * render_scale` render pixels, at most 3 taps. Fixed seed. | The plan of section 6.2 sets the blur and the amplitude. |
| Grain math | `include/edit/runtime/film/film_grain_math.h` | Dye clouds of #660. 5207 `sigma_D` curves as literal segments on a linear map from display value to density. | Keep the colour mix. Replace the size and amplitude terms. Add the per-stock curve evaluation. |
| CUDA pass | `edit/runtime/cuda/cuda_neighbor_grade.cuh`, `include/edit/runtime/cuda/cuda_film_grain_math.cuh` | The horizontal pass draws a Bernoulli value for each tap and channel with a 64-bit hash (`cuda::PixelStream2D`, `UniformFloat01`), at the nearest reference pixel. | A noise pass with a 32-bit hash and a halo, then the separable blur. |
| OpenCL pass | `edit/runtime/opencl/shader/primary_grade_neighbor.cl` | Same structure as CUDA. | Same change. |
| Metal pass | `edit/runtime/metal/shader/drt_neighbor.metal` | Same structure. 56.5 ms at 24 MP; the horizontal pass dominates (#660 record). | Same change. The sliding-window blur of the film print. |
| Post schedule | `edit/runtime/drt_post_schedule.cpp`, `DrtPostNeighborhoodRuns` | Skips Halation when the method is the film print. | Also skips Film Grain when the method is the film print (U4). |
| Model | `include/edit/operators/models/scalar_operator_model.hpp`, `FilmGrainTraits` | `ScalarFloatPayload`, key `strength`, range 0 to 1. | Range 0 to 2 (U3). |
| Format | none | The film print image plane uses `common/frame_mm` = 36 for all stocks (`film_print_plan.cpp`, `MakeFilmPrintImagePlan`). | A field `film_format` of the DRT parameters (section 6.1). |
| Film print display | `include/edit/runtime/film/film_print_math.h`, `FilmKernelReadRecordTable`; kernels in `cuda_film_print_pass.cu`, `film_print.cl`, `film_print.metal`, `film_print_scan.*` | FP6 reads the print table, the scanner table, or the light table with the dye amounts of the pixel. | Add the grain to the dye amounts before the read and before the Enlarger flare amounts. |
| Panel | `ui/alcedo_main/qml/EditorPostProcessPanel.qml`, `editorAdjustmentPanel_post` | One Film Grain slider, 0 to 100 %, `Conversion::Percent` (`app/editor_parameter_catalog.cpp`). | Slider 0 to 200 %, a Format selector, and the name of the film whose data apply. |
| Tests | `alcedo_studio/tests/edit/runtime/film_grain_math_test.cpp`, `film_grain_colour_statistics.hpp`, `cuda_drt_product_test.cpp`, `adjustment_runtime_test.cpp` | Host and backend statistical colour tests. | Add the size, zoom, ROI, and stock statistics of section 9. |

### 5.2 Defect found in the audit

The fit view shows more grain than the full image. At a render scale below 1, the blur sigma is `0.8 * render_scale`, for example
0.2 render pixels at 25 %. That blur has a variance factor of 1.0, against 0.354 at full size. Each render pixel reads one
reference pixel (`FilmReferenceCoordinate`, nearest pixel). So the fit view shows the per-pixel grain of the full image at about
2.8 times its amplitude, and about 4 times the amplitude of the full image downscaled to the same size. Phase G3 removes this.
Issue [#674](https://github.com/zidage/AlcedoStudio/issues/674) records it.

ROI behavior, confirmed in G1: `use_reference_coordinates` is `CoversFullEditSpace(geometry)`. A render that does not cover the full
edit space hashes its render coordinates, so a zoomed view has another grain pattern than the same region of the full frame, and the
pattern stays on the screen on a pan. Issue [#687](https://github.com/zidage/AlcedoStudio/issues/687) records it; G3 removes it.

---

## 6. Target design

### 6.1 Data and ownership

| Data | Owner | Serialization | Default | Compatibility |
| --- | --- | --- | --- | --- |
| `film_format` (`16mm`, `35mm`, `70mm`) | `DrtParamsModel`, `DrtPayload` (`include/edit/graph/drt_node_model.hpp`) | Key `film_format` in `params` of the DRT node. Written only when the value is not `35mm`. | `35mm` | Old documents read `35mm`. Their bytes and root id stay. |
| Grain strength | `FilmGrainModel`, key `strength` | Unchanged | 0 | Same bytes. Range 0 to 2 (U3). |
| Stock grain data | `FilmStockBundle` (`include/edit/runtime/film/film_stock_bundle.hpp`), read-only after `Open` | `grain/<stock>` in the bundle, schema 11 | 5207 data for methods with no stock | A bundle of schema 10 fails to open with the real version error, as today. |

`film_format` belongs to the DRT node because it is a property of the film frame. The grain of every method and the film print
image plane read it (U2). It is not part of `params.film_print`, so the remembered film print settings of #672 keep their form.

`FilmFormat` is a proposed scoped enum in the proposed header `include/edit/graph/film_format.hpp`, with
`FilmFrameLongSideMm(FilmFormat) -> double` (U1 values) and the identifier conversion.

### 6.2 Host grain plan

Proposed: `include/edit/runtime/film_grain_plan.hpp`, `edit/runtime/film_grain_plan.cpp`.

```text
MakeFilmGrainPlan(const ResolvedRenderGeometry& geometry, FilmFormat format,
                  const FilmGrainStockView& stock, FilmGrainTarget target,
                  float strength) -> FilmGrainPlan
```

- Input: the render geometry, the format, the grain data of the stock (or of 5207), the target (`Post` or `Negative`), and the
  slider value. The target selects the curves (channel or layer) and the correlation (display or layer) of the plan.
- Output, a value type that the compile step copies into the kernel parameters:
  - `reference_pitch_um` and `render_pitch_um`;
  - `aggregate` k: `clamp(round(1 / render_scale), 1, 4)`; 1 for a render scale of 1 or more;
  - for each layer: the blur weights (`sigma_b` of rule `grain_kernel`, radius `ceil(3 sigma_b)`, at most 24 taps) and the
    amplitude factor `I / (sum w^2)`;
  - the Cholesky factor L of the layer correlation (film print) or of the display correlation (post path), and the piecewise curve
    segments of the stock;
  - `enabled`: 0 when the strength is 0.
- The plan is a pure function. It changes no state. A bad input (a format outside the enum, an empty stock curve) throws
  `std::invalid_argument` with the name of the field.

`FilmGrainStockView` is a view into the bundle; its spans are valid for the life of the bundle. The post path reads the 5207 view.
The film print path reads the view of the selected film.

### 6.3 GPU passes

Shared math: proposed `include/edit/runtime/film/film_grain_field.h`, included like `film_grain_math.h` (OpenCL lists it in the
program, Metal includes it by relative path, CUDA through a `.cuh` wrapper):

- `FilmGrainHash3(uint x, uint y, uint seed) -> uint3`: pcg3d, the 32-bit hash of Jarzynski and Olano (JCGT 9(3), 2020). One call
  gives three channels.
- `FilmGrainGaussian(uint bits) -> float`: the inverse normal CDF of Giles (2010), single precision.
- `FilmGrainCurveSigma(FilmGrainCurve curve, float value) -> float`: the per-stock curve. `FilmGrainCurve` holds the first sigma
  and 15 segments (start, inverse width, change); a segment past the counted nodes adds 0.

Passes, for each render:

```text
G-noise   one thread per pixel of the render plus a halo of the blur radius:
          sum of k x k reference-pixel Gaussians / k, at the true reference coordinates; 3 channels
          (one per layer); writes the noise image (float4, the 4th channel unused; half4 on Metal)
G-blur-x  separable Gaussian, per-channel weights (the FP2 blur of the film print, with its
          Metal sliding window)
G-blur-y  same; divides each channel by its sum w^2 and mixes with L (rule grain_layer_mix);
          writes the unit grain image
apply     post path:   FilmGrainDyeClouds with the unit grain image in place of the Bernoulli field
          film print:  in FP6, a' = a + strength * sigma_a(a) * amplitude * N, before the flare
                       amounts and the table read
```

The halo makes an ROI render equal to the same region of a full-frame render. The noise image and the blur buffers come from the
texture pool of the pass and are released at the end of the pass.

### 6.4 Primary success chain

```text
User moves Film Grain or Format
  -> EditorParameterWrite (strength or film_format) -> DRT node history entry
  -> GraphCompiler::CompileDrt -> MakeFilmGrainPlan(geometry, format, stock, strength)
  -> film print:  DrtPostSchedule skips Film Grain; RecordCudaFilmPrint (and OpenCL, Metal)
                  runs G-noise, G-blur-x, G-blur-y, then FP6 with the grain image
  -> other method: DrtPostExecutor runs G-noise, G-blur-x, G-blur-y, apply
  -> display image published
```

### 6.5 Primary failure chain

```text
Bundle of schema 10, or a stock with no grain group
  -> FilmStockBundle::Open throws with the file name and the found version
  -> the render fails with that error; no partial grain image is published; the pass releases
     its pool textures
Allocation of the noise image fails
  -> the backend reports the allocation error of the texture pool; the frame fails; the
     previous published frame stays on screen
```

No fallback grain path exists. A stock with no grain data is a resource defect, and G0 checks that every stock has a group.

---

## 7. Product design specification

User change (2026-11-06): with the film print, Film Grain moves to the Display Transform panel under Format, the Post Processing
page does not show it, and no page shows a caption of the granularity. A selection of the film print turns on Film Grain.

### 7.1 Post Processing page, Texture section (a method other than the film print)

```text
Film Grain                                   [  48 % ]
  ------------------------------o-------------------------  0 % to 200 %
Format            ( 16mm | 35mm | 70mm )
Halation
```

- The slider keeps its current position in the page. 100 % is the datasheet granularity.
- Format is a segmented selector with three buttons. One write per click, one history entry.
- The section has the same enabled state as the other post controls (`controlsEnabled`).
- With the film print the page has no Texture section: the film print takes its halation from the film layers and its grain in
  the negative (section 7.2). The stored values stay, and the section returns with another method.

### 7.2 Display Transform panel, Film Print

```text
Film              [ Kodak Vision3 250D 5207 ]
Format            ( 16mm | 35mm | 70mm )
Film Grain                                   [ 100 % ]
Print             [ Kodak 2383 ]
```

- Format and Film Grain write `film_format` and `film_grain`, the same fields as the Post Processing page, so both pages always
  show the same values.
- A selection of method Film Print on another method sets Film Grain to 100 % when its stored strength is 0. A stored strength
  stays. Film Grain is a field of its own, so this is a second settled write, and a second history entry after the method. An
  open document with the film print and a strength of 0 keeps it: only the selection writes.
- An old document has a strength and no `film_format`; it reads `35mm` (section 2.4).

### 7.3 Other surfaces

- `EditorParameterCatalog`: `film_grain` gets the range 0 to 200 %; a new entry `film_format` (enum).
- `alcedo-cli` parameter catalog and the adjustment transfer catalog (copy and paste of settings) include `film_format`.
- Reset of the DRT node writes `35mm`.

---

## 8. Phases

Tracking: [#684](https://github.com/zidage/AlcedoStudio/issues/684). Related bug: [#674](https://github.com/zidage/AlcedoStudio/issues/674),
closed by G3.

| Phase | Issue | Result | Main modules | Dependency | Expected diff | Status |
| --- | --- | --- | --- | --- | ---: | --- |
| G0 | [#675](https://github.com/zidage/AlcedoStudio/issues/675) | Grain data, fits, and reference evaluator in the lab | alcedo-film-lab | none | 1500 (lab) | Complete ([alcedo-film-lab #2](https://github.com/zidage/alcedo-film-lab/pull/2)) |
| G1 | [#676](https://github.com/zidage/AlcedoStudio/issues/676) | Host plan, shared math, bundle schema 11 | `edit/runtime`, `edit/runtime/film` | G0 | 1100 | Complete (1605 lines) |
| G2 | [#677](https://github.com/zidage/AlcedoStudio/issues/677) | `film_format`, slider range, panels, catalogs, compatibility tests | `edit/graph`, `app`, `ui` | G1 | 1300 | Complete (991 lines) |
| G3 | [#678](https://github.com/zidage/AlcedoStudio/issues/678) | Post grain on CUDA with the new passes | `edit/runtime/cuda` | G1, G2 | 900 | Complete (1492 lines) |
| G4 | [#679](https://github.com/zidage/AlcedoStudio/issues/679) | Post grain on OpenCL and Metal | `edit/runtime/opencl`, `edit/runtime/metal` | G3 | 1200 | Complete (Metal 16mm timing: #696) |
| G5 | [#680](https://github.com/zidage/AlcedoStudio/issues/680) | Grain in the negative on CUDA, post skip | `edit/runtime/cuda`, `drt_post_schedule.cpp` | G3 | 1000 | Complete (1703 lines) |
| G6 | [#681](https://github.com/zidage/AlcedoStudio/issues/681) | Grain in the negative on OpenCL and Metal | `edit/runtime/opencl`, `edit/runtime/metal` | G4, G5 | 1100 | Planned |
| G7 | [#682](https://github.com/zidage/AlcedoStudio/issues/682) | Format sets the film print image plane | `film_print_plan.cpp` | G2 | 400 | Complete |
| G8 | [#683](https://github.com/zidage/AlcedoStudio/issues/683) | Measurements, user review, and documents | docs, lab | G6, G7 | 400 | Planned |

Each estimate is at or below 2000 lines. One backend group per phase keeps each review small.

Commands for the product phases, from the repository root, in PowerShell:

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4
ctest --test-dir build/debug -R "<test name>" --output-on-failure
```

Timing values come from a Release build only (`win_release`, `macos_release`). Metal phases build and run on the Mac.

### G0: grain data and reference evaluator (lab)

**Objective and deliverables.** The lab produces the grain group of every stock, the cloud sizes, the layer correlation (first form: the shared share), and a
reference evaluator for the product tests.

**Inputs.** The datasheets of section 4.2. The 5207 scans `D:\素材\照片\胶片翻拍\2023 5207\2\` (Flextight, 3200 dpi, 7.94 um per
pixel, FlexColor USM -60, Gamma 2.0). The lab bundle builder `scripts/poc/p7_spectral_acceleration/build_product_resources.py`.

**Work, in order.**

1. Digitize the `curve` tier. Record each sheet, page, and read condition. Check: the 5207 digitization reproduces the segments
   of `film_grain_math.h` within 3 %.
2. Collect the `number` and `pgi` sources. Compute the PGI ratios of section 4.3 and find the anchor in the stated order.
3. Solve `sigma_a_k` (section 4.4) for every stock. A negative square is a data error: record it and stop for that stock.
4. Fit `sigma_c` on the 5207 scans: compute the radial noise power spectrum of the density residual on flat blocks (method of #660,
   scanner white noise removed by the lag-1 and lag-2 cross-covariance). Measure the scanner MTF from a sharp frame edge or a
   sprocket edge of the same scan. Fit one Gaussian per layer. If the fit residual in the band 5 to 40 cycles per mm is above 10 %,
   fit the cluster term of section 3.3 and record its design and its added cost before G1.
5. Fit the correlation of the layer noise (section 4.4; the first form fitted `rho`).
6. Compute the post-path target of section 3.5.
7. Write the reference evaluator `grain_reference.py`: per-pixel standard deviation, lag-1 correlation, and channel correlation for
   a format, a pitch, a stock, and a density. Write the expected values for the product tests to the directory that
   `ALCEDO_FILM_PRINT_EXPECTED_DIR` names.
8. Add `grain/<stock>` to the bundle builder. Schema 11.

**Exit criteria.** Every stock of `kFilmNegativeIds` and `kFilmReversalIds` has a grain group with a recorded source and tier. The
lab report records the fits, the residuals, and the anchor. The lab ROADMAP records the decisions.

**Completion record.** Complete 2026-11-05: [alcedo-film-lab #2](https://github.com/zidage/alcedo-film-lab/pull/2), lab commit `d98712f`, branch `feature/p15-film-grain-data` (stacked on
`feature/p7-product-resources`). Report: [P15 report](https://github.com/zidage/alcedo-film-lab/blob/feature/p15-film-grain-data/docs/exploration/p15_film_grain.md). Lab ROADMAP decision 12.

Results:

- All 35 stocks have a grain group (8 `curve`, 9 `number`, 6 `family`, 7 `pgi`, 5 `reversal`). The layer solve has no negative
  square; the 16-node curves are within 4.3 % of the full curves.
- Step 1: the vector digitization passes its axis limits on all 8 charts. The 5207 segments of `film_grain_math.h` differ from it by a
  median of 6 % (R), 12 % (G), and 15 % (B), not 3 %; the product puts the green and blue peaks at higher density (0.94 against 0.77,
  1.32 against 1.12). The product segments are the older reading. G1 replaces them with `grain/kodak_5207/channel_*`.
- Step 2: no collected sheet anchors the PGI; the anchor is the fotufilm value (section 4.3).
- Step 4: the scanner MTF is not measurable on the scans; `sigma_c` keeps the literature values (section 3.3).
- Step 5: the layer correlation R replaces `rho` (section 4.4).
- Step 6: the post target and the display correlation (section 3.5).
- Step 7: `grain_reference.py` reproduces the table of section 3.2 and writes `grain__expected.bin` (pitch, `sigma_b`, radius,
  `sum w^2`, pixel factor, lag-1 before and after the mix, the zoom factors and pattern correlation, the aggregate k, R and L, the
  film print sigma for 5207, 5219, Portra 400, and Velvia 50, and the post sigma). With rule `grain_kernel` the 2 x 2 zoom statistic
  is exact and the zoom pattern correlation is at least 0.943 (G3 limits: 5 % and 0.9).
- Step 8: `build_product_resources.py` writes schema 11 (section 4.1).

Changes to this plan from G0: rule `grain_kernel` in place of `sigma_b = sigma_c / p` (section 3.2); R and rule `grain_layer_mix`
in place of the shared field, three noise channels (sections 4.4, 6.3); the post path mixes with the display correlation (section
3.5); the bundle fields of section 4.1; the Selwyn test of G1 at 500 um; the fotufilm licence is CC BY-SA 4.0 for the profiles.

Primary call chain (lab):

```text
fetch_sources.py -> build/tmp/p15_grain/sources/ (38 documents, SHA-256)
digitize_granularity_charts.py -> granularity_curves.json
stock_granularity.py (tiers, PGI anchor, layer solve, nodes) -> stock_grain.json
fit_scan_correlation_width.py -> scan_correlation_width.json
fit_layer_correlation.py -> layer_correlation.json
post_path_target.py -> post_path_target.json
build_product_resources.py: collect() -> collect_grain() -> film_stock_bundle.bin, schema 11
                            main() -> grain_reference.expected_arrays() -> expected/grain__expected.bin
```

### G1: host plan, shared math, and bundle schema 11

**Objective and deliverables.** `FilmGrainPlan`, `film_grain_field.h`, the bundle reader for `grain/*`, and their host tests. No
GPU change.

**Files.** Proposed: `include/edit/runtime/film_grain_plan.hpp`, `edit/runtime/film_grain_plan.cpp`,
`include/edit/runtime/film/film_grain_field.h`, `include/edit/graph/film_format.hpp`. Changed:
`include/edit/runtime/film/film_stock_bundle.hpp`, `edit/runtime/film/film_stock_bundle.cpp` (schema 11, `FilmGrainStockView`),
the CMake lists of the runtime and the tests.

**Data rules.** The amplitude of the plan equals `G * I(sigma_c, p) / (sum w^2)`. The integral uses a fixed quadrature of at least
2000 points per axis. The weights sum to 1.

**Steps.**

1. Add `FilmFormat` and the frame sizes of U1.
2. Read `grain/<stock>` in `FilmStockBundle::Open`; check shapes and the tier string.
3. Write `MakeFilmGrainPlan` as in section 6.2.
4. Write the hash and the inverse CDF in `film_grain_field.h`, compiled also on the host.
5. Write a test of the ROI hash coordinates of the current post pass (section 5.1, inference to check). Record the result in this
   section.

**Tests.** New suites in the `GpuDagRawInputTest` executable, next to `film_grain_math_test.cpp`:

- `FilmGrainPlan`: the amplitude for the table of section 3.2 within 1 %; Selwyn's law at `p = 500 um` within 1 % (at 50 um, `I p`
  is 0.955 with `sigma_c` 2.0); `sigma_b`, the weights, the pixel factor, and lag-1 against `grain__expected.bin` within 1e-6; the
  aggregate k for render scales 1, 0.5, 0.3, and 0.1 (1, 2, 3, 4).
- `FilmGrainField`: the Gaussian of 10^6 hashes has mean 0 within 0.005, standard deviation 1 within 0.005, and kurtosis 3 within
  0.05; neighbour hashes have a correlation below 0.005.

In `FilmPrintTablesTest`: every stock has a grain view; a schema 10 bundle fails with the version in the message.

**Exit criteria.** The tests pass in the debug build. No GPU file changed.

**Completion record.** Complete 2026-11-05, branch `feature/film-grain-host-plan`, stacked on the plan branch
`docs/film-grain-format-plan`. No GPU file changed.

Files: `include/edit/graph/film_format.hpp` (`FilmFormat`, `FilmFormatId`, `TryParseFilmFormat`, `FilmFrameLongSideMm`),
`include/edit/runtime/film/film_grain_field.h` (`FilmGrainHash3`, `FilmGrainGaussian`, `FilmGrainCurve`, `FilmGrainCurveSigma`),
`include/edit/runtime/film_grain_plan.hpp` and `edit/runtime/film_grain_plan.cpp` (`MakeFilmGrainPlan` and its rules),
`film_stock_bundle.hpp/.cpp` (`FilmGrainStockView`, `FilmGrainConstants`, `FilmStockBundle::Grain`, `GrainConstants`, the checks of
`ReadGrain`), `film_array_file.hpp` (`kFilmResourceSchemaVersion` 11).

Details that the plan did not give, chosen in G1:

- Pixel integral: the closed form of the lab evaluator, which is exact. The data rule "a fixed quadrature of at least 2000 points"
  is the check: a composite Simpson rule of 20000 intervals of the frequency form agrees within 1e-6 for sigma_c 1 to 4 um and p 1
  to 60 um.
- `MakeFilmGrainPlan` has the parameter `FilmGrainTarget` (section 6.2): `Post` reads the channel curves and the display
  correlation, `Negative` the layer curves and the layer correlation. The plan holds the values in double precision; the compile
  step of G3 and G5 converts them to the kernel parameters.
- The render scale is the mean of the two axis scales, as `NeighborhoodRenderScale` of the current post grain. The aggregate k
  rounds half to even, as the lab (Python `round`).
- `FilmGrainGaussian` reads the 23 high bits of the hash: u = (k + 0.5) / 2^23 is exact in single precision, so 1 - u is exact, and
  the tails end at about 5.3.
- A curve is literal segments of start, inverse width, and change, so a padded node adds 0 and no division happens per pixel.
- `FilmStockBundle::Open` checks the grain data: each negative and reversal film has a group and a known tier; each counted curve
  has 2 to 16 nodes, a rising x, and a sigma above 0; the cloud sizes, the aperture, and the post target are above 0; each
  correlation is inside (-1, 1). `MakeFilmGrainPlan` checks that each correlation matrix has a Cholesky factor.

Step 5 result: the inference of section 5.1 is correct. Test `FilmGrainOfAViewportRoiHashesRenderCoordinates` shows the flag
`use_reference_coordinates` 1 for the full frame and the fit view, and 0 for a ROI whose render pixel (0, 0) is reference pixel
(64, 64). `FilmReferenceCoordinate` returns `(x, y)` for flag 0 (`cuda_neighbor_grade.cuh:87`, `primary_grade_neighbor.cl:109`,
`drt_neighbor.metal:123`). Issue [#687](https://github.com/zidage/AlcedoStudio/issues/687).

Lab outputs: G0 rebuilt the resources and `grain__expected.bin` at schema 11, but `expected/display/` and `expected/image/` stayed at
schema 10. G1 rebuilt both at lab commit `a3356a8` (`build_display_expected.py`, 1 min; `build_image_expected.py
_DSC8148.ARW`, 8.5 min). All 105 expected files now state schema 11. Their values do not depend on the grain; only the schema version in the header changes.

Primary success call chain:

```text
FilmStockBundle::Open(directory)
  -> ReadManifest (schema 11) -> ReadResource x 3 (size, SHA-256)
  -> FilmStockBundle ctor -> ReadLists -> ReadCctGrid -> ReadCommonValues -> ReadReflectance
  -> ReadGrain: common/grain_* -> per film: metadata grain/<id>/tier, ReadGrainCurves, CheckGrainCurves
  -> CheckShapes -> read-only bundle
FilmStockBundle::Grain(id) -> FilmGrainStockView (spans into the bundle, tier, reversal flag)
MakeFilmGrainPlan(geometry, format, view, target, strength)
  -> FilmFrameLongSideMm(format) -> reference_pitch_um = frame * 1000 / long side of the full reference
  -> render_scale (RenderAxisScale, two axes) -> render_pitch_um, Aggregate (grain_aggregate)
  -> per layer MakeFilmGrainLayerKernel: MatchedSigmaB (grain_kernel bisection of FilmGrainPixelLag1),
     GaussianKernel, weight_square_sum, FilmGrainPixelIntegral -> pixel_factor, amplitude
  -> CorrelationCholesky (layer or display correlation) -> mix
  -> MakeFilmGrainCurve x 3 (layer or channel curves) -> FilmGrainPlan (value type)
```

Primary failure call chain:

```text
Bundle of schema 10
  -> ReadManifest: "Film print resource '.../manifest.json': schema version 10 is not version 11"
  -> Open throws std::runtime_error; no partial bundle stays in memory
Film with no grain group, an unknown tier, or a damaged curve
  -> ReadGrain throws std::runtime_error that names the film and the reason ("'rev_a' has no grain group",
     "does not rise", "has 1 nodes", "tier 'guess'")
Format outside the enum, strength outside [0, 2], empty reference extent, curve outside 2..16 nodes,
cloud size not above 0, correlation with no Cholesky factor
  -> MakeFilmGrainPlan throws std::invalid_argument that names the field; it changes no state
```

What was proven (executed tests, debug build):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Amplitude for the table of section 3.2 within 1 % | `FilmGrainPlan.PixelSigmaOfTheGreenLayerMatchesTheTableOfThePlan`, `SmallerFrameGivesLargerGrainAtTheSameViewSize` | `GpuDagRawInputTest` | PASS |
| Selwyn's law at 500 um within 1 %; `I p` 0.955 at 50 um | `FilmGrainPlan.SelwynLawHoldsForALargePixel` | `GpuDagRawInputTest` | PASS |
| Quadrature of at least 2000 points | `FilmGrainPlan.PixelIntegralEqualsTheQuadratureOfTheFrequencyForm` | `GpuDagRawInputTest` | PASS |
| `sigma_b`, weights, pixel factor, lag-1 against `grain__expected.bin` within 1e-6 | `FilmGrainPlanLabTest.KernelAndPixelFactorMatchTheLabEvaluator`, `FilmGrainPlan.KernelWeightsSumToOneAndGiveTheLag1OfThePixelField` | `GpuDagRawInputTest` | PASS |
| Aggregate k 1, 2, 3, 4 for render scales 1, 0.5, 0.3, 0.1 | `FilmGrainPlan.AggregateFollowsTheRenderScale`, `FilmGrainPlanLabTest.AggregateAndMixMatchTheLabEvaluator` | `GpuDagRawInputTest` | PASS |
| L of the layer and the display correlation | `FilmGrainPlanLabTest.AggregateAndMixMatchTheLabEvaluator`, `FilmGrainPlan.TargetSelectsTheCurvesAndTheCorrelation` | `GpuDagRawInputTest` | PASS |
| Stock curves of 5207, 5219, Portra 400, Velvia 50 give the lab sigma at grey | `FilmGrainPlanLabTest.PlanOfEachPrintStockGivesTheLabPixelSigmaAtGrey` | `GpuDagRawInputTest` | PASS |
| Curve segments, strength 0, field-named errors | `FilmGrainPlan.CurveSegmentsReproduceTheNodesAndHoldTheEndValues`, `StrengthZeroDisablesThePlanAndKeepsItsValues`, `InvalidInputThrowsWithTheNameOfTheField`, `FilmFormatTest.IdentifiersAndFrameSizesFollowDecisionU1` | `GpuDagRawInputTest` | PASS |
| Gaussian of 10^6 hashes: mean, standard deviation, kurtosis | `FilmGrainField.GaussianOfAMillionHashesHasUnitMomentsAndNormalKurtosis` | `GpuDagRawInputTest` | PASS |
| Neighbour hashes: correlation below 0.005 | `FilmGrainField.NeighbourHashesAndChannelsAreUncorrelated` | `GpuDagRawInputTest` | PASS |
| Inverse normal and hash identity | `FilmGrainField.GaussianIsTheInverseNormalDistribution`, `HashIsAFunctionOfTheCoordinatesAndTheSeed` | `GpuDagRawInputTest` | PASS |
| Step 5, ROI hash coordinates | `GpuDagAdjustmentRuntime.FilmGrainOfAViewportRoiHashesRenderCoordinates` | `GpuDagRawInputTest` | PASS |
| Every stock has a grain view | `FilmPrintTablesTest.EveryFilmHasAGrainViewWithItsTierAndCurves`, `FilmPrintTablesLabTest.EveryShippedFilmHasAGrainViewOfItsTier` (35 films: 8 curve, 9 number, 6 family, 7 pgi, 5 reversal) | `FilmPrintTablesTest` | PASS |
| A schema 10 bundle fails with the version in the message | `FilmPrintTablesTest.BundleOfSchema10FailsWithTheFoundVersionInTheMessage` | `FilmPrintTablesTest` | PASS |
| Damaged grain data fails with the film and the reason | `FilmPrintTablesTest.FilmWithNoGrainGroupThrowsWithItsName`, `DamagedGrainDataThrowsWithTheFilmAndTheReason` | `FilmPrintTablesTest` | PASS |

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --preset win_debug -DCMAKE_PREFIX_PATH="D:/Qt/6.9.3/msvc2022_64/lib/cmake"
cmd /c scripts\msvc_env.cmd --build build\debug --target GpuDagRawInputTest FilmPrintTablesTest --parallel 6
build\debug\alcedo_studio\tests\edit\GpuDagRawInputTest_runtime\GpuDagRawInputTest.exe
  --gtest_filter="FilmGrainPlan*:FilmGrainField*:FilmFormatTest*:FilmGrainMath*:GpuDagAdjustmentRuntime*:FilmPrintImagePlan*"
build\debug\alcedo_studio\tests\edit\FilmPrintTablesTest_runtime\FilmPrintTablesTest.exe
```

Suite totals: `GpuDagRawInputTest` with the filter above 51 / 51 (the existing `FilmGrainMath`, `GpuDagAdjustmentRuntime`, and
`FilmPrintImagePlan` suites are included); `FilmPrintTablesTest` 72 / 72, with the lab values configured (no skip).

Checklist / exit condition: steps 1 to 5 done; the tests of the section pass in the debug build; no GPU file changed.

LOC note: 12 files, 1605 added lines. `film_grain_plan.cpp` 245, `film_grain_plan_test.cpp` 599, `film_stock_bundle.cpp` 846. The
test file `film_print_tables_test.cpp` was 2506 lines before G1 and is 2678 now; its grain tests use the fixture of the file, so G1
kept them there.

Remaining gaps: none for G1. G2 adds `film_format` to the document; G3 to G6 read the plan in the kernels.

### G2: format field, slider range, panels, and catalogs

**Objective and deliverables.** The user can set the format and the slider range of section 7. The render does not use the format
yet.

**Files.** `include/edit/graph/drt_node_model.hpp` and its source (`film_format` in `DrtPayload`, JSON only when not `35mm`),
`include/edit/operators/models/scalar_operator_model.hpp` (`FilmGrainTraits::kMax = 2`), `app/editor_parameter_catalog.cpp`,
`app/editor_parameter_write.cpp`, `app/editor_parameter_write_parse.cpp`, `app/editor_panel_projection.cpp`,
`include/app/adjustment_transfer_catalog.hpp`, the `alcedo-cli` parameter catalog, `EditorPostProcessPanel.qml`,
`EditorDisplayTransformPanel.qml`, the translation sources (no `lupdate` run on a partial scan).

**Steps.**

0. Before any code change, build `main` and write the pre-change documents of section 2.4. Commit them first, in their own commit.
1. Add the field and its JSON rule. Add a dirty flag `FilmFormat` that rebuilds the grain plan and, with G7, the film print canvases.
2. Add the catalog entries and the write parser.
3. Add the selector and the caption to both panels.

**Tests.** `GpuDagModelGraphTest` (`json_roundtrip_test.cpp`): the expected serialized documents of `alcedo_studio/tests/edit/history/expected_serialized/` keep
their bytes; a document with `film_format: "16mm"` round-trips. `PipelineDocumentCheckpointTest`: the root id of an old document
stays. `EditorParameterCatalogTest`: the range and the enum. `EditorDisplayTransformSnapshotQmlTest` and a new
`EditorPostProcessPanelQmlTest`:
one click writes one history entry; both selectors show the value; a QTRY timeout fails the test (#526 rule).

In `PipelineDocumentCheckpointTest`, from the pre-change documents of section 2.4:
`ExistingFilmGrainRootDocumentsSerializeByteIdenticalAfterFormatChange` (bytes and root id) and
`ExistingFilmGrainHistoryReplaysAfterFormatChange` (replay, undo, and redo of the stored Film Grain entry).

**Exit criteria.** The listed tests pass. Every row of section 2.4 that G2 owns has a passing test.

**Completion record.** Complete 2026-11-05, branch `feature/film-grain-format-field`, stacked on the G1 branch
`feature/film-grain-host-plan`. No GPU file changed. The render does not read the format yet.

Step 0: commit `test(film-grain): Store the Film Grain documents written before the film format` holds only the three documents. The
document code of `6b8eebaf6` (G1) wrote them. That code is the same as `main`: G1 adds only `film_format.hpp`, which no document read.

Files: `drt_node_model.hpp/.cpp` (`DrtPayload::film_format`, `DrtDirty::FilmFormat`, `DrtFilmFormatUpdate`,
`DrtParamsModel::FilmFormat`, `ApplyFilmFormat`, `ReadFilmFormat`), `scalar_operator_model.hpp` (`FilmGrainTraits::kMax` 2),
`adjustment_runtime.cpp` (the clamp of the current grain), `editor_adjustment_pipeline.hpp/.cpp` (`EditorAdjustmentField::FilmFormat`),
`editor_adjustment_context.cpp`, `editor_parameter_write.hpp/.cpp`, `editor_parameter_write_parse.cpp` (`ParseFilmFormatUpdate`),
`editor_pipeline_command_service.cpp`, `editor_panel_projection.hpp/.cpp` (`EditorPanelFilmFormatValue`, `ReadFilmFormat`),
`editor_parameter_catalog.cpp` (entry `film_format`, `kFilmGrainRange`), `document_transfer_planner.cpp`,
`editor_panel_presentation.cpp`, `editor_history_commit_presentation.cpp`, `EditorPostProcessPanel.qml`,
`EditorDisplayTransformPanel.qml`, `alcedo_main_zh_CN.ts`.

Details that the plan did not give, chosen in G2:

- The editor field `film_format` has its own history entry, as the field `diffusion` has. Its value is `{"film_format": "16mm"}`, also
  for `35mm`. The field `odt` (`OutputTransformJson`) does not hold the format, so an undo of a Display Transform entry does not change
  the format, and an old `odt` entry keeps its before value. `film_format` is not in `params.film_print`, so the remembered film
  print settings of #672 keep their form.
- The write is the typed operation `DrtFilmFormatUpdate`. The parser accepts `{"film_format": id}` and the enum control form
  `{"index": n, "value": id}`. An unknown identifier fails with "film_format must be one of 16mm, 35mm, 70mm".
- `DrtParamsModel::LoadJson` reads a missing key as `35mm`. A key that is not a string or not an identifier stops the load with a
  `std::runtime_error` that names `film_format`; no field changes. `ToJson` writes the key only when the format is not `35mm`.
- `DrtDirty::FilmFormat` is bit 9 and is part of `All`. In G2 no runtime pass reads it: a change increments the revision of the DRT
  params, so the display output renders again. G3 reads the flag to make the grain plan again, and G7 to make the film print canvases
  again.
- Range: until G3, the current post grain reads the full range 0 to 2 in place of a clamp to 1. Its amplitude is linear in the
  amount, so 200 % gives twice the grain of 100 %. A clamp to 1 would show a 200 % slider with the grain of 100 %.
- Transfer: the package holds `DrtParamsModel::ToJson`, so it holds `film_format` when the format is not `35mm`. The paste planner
  writes a third change, `film_format`, next to `odt` and `diffusion`. A package of a `35mm` document, or one made before the field,
  sets `35mm`, as the source renders.
- `alcedo-cli` reads `EditorParameterCatalog`, so the catalog command lists `film_format` and the Film Grain range 0 to 200 with no
  CLI code change. The generated reference of the alcedo-skills repository (`editor_fields.md`, `editor_catalog.json`) is made again
  in that repository.
- The product has no reset of the complete DRT node. The default of the catalog entry and the reset of the Format model write
  `35mm`.
- The Format selector is a `SegmentedCardSwitcher` with an `EditorAdjustmentEnumModel`. Its `paramsBuilder` converts the selection
  through `EditorParameterCatalog`. The Display Transform page shows it under the film picker of the film print. The history row is
  "Film Format", with the summary "35mm → 16mm". The caption reads the browser label of the film print negative, else
  `kodak_5207`.
- The plan named a new `EditorPostProcessPanelQmlTest`. The tests of the Post Processing page are in
  `EditorDisplayTransformSnapshotQmlTest`, whose harness loads the adjustment stack and already tests that page
  (`PostHalationSliderHidesForFilmPrintAndReturnsWithItsValue`). A new executable would copy the harness. Each wait is
  `ASSERT_TRUE(WaitFor(...))`, so a timeout fails the test (#526).
- The model tests are in the new file `drt_film_format_model_test.cpp` of `GpuDagModelGraphTest`, next to the diffusion filter tests,
  in place of `json_roundtrip_test.cpp`. The checks of the stored documents are in `PipelineDocumentCheckpointTest`, which has the
  directory of the stored documents.
- The stored documents are three files. The third holds the history entries, because a checkpoint holds only the hash of its head.
  The `film_print` document has a second entry, Halation 0.6 to 0.4 of the field `odt`, for the DRT node row of section 2.4.
- The zh_CN translations ("Format", "Granularity of %1") are edited in the `.ts` file by hand, with no `lupdate` run.

Primary success call chain:

```text
SegmentedCardSwitcher.selected (postFilmFormatControl or displayFilmFormatControl)
  -> EditorAdjustmentEnumModel::selectIndex -> paramsBuilder -> EditorParameterCatalogAdapter::modelParamsJson
     -> EditorParameterCatalog::ToModelJson("film_format") -> {"film_format": "16mm"}
  -> submitPatch (settled) -> ParseEditorParameterWrite -> ParseFilmFormatUpdate -> DrtFilmFormatUpdate
  -> ApplyEditorParameterWrite (DrtPost) -> DrtParamsModel::ApplyFilmFormat -> DrtDirty::FilmFormat
  -> history: ReadEditorParameterJson before and after -> MakeSetParameterBatch (field film_format)
  -> projection: ReadFilmFormat -> EditorPanelFilmFormatValue -> PanelProjectionToVariantMap
  -> loadFilmFormat of both pages
Old document
  -> DrtNodeModel::FromJson -> DrtParamsModel::LoadJson -> ReadFilmFormat (no key: 35mm)
  -> ToJson writes no key -> same bytes -> same ComputeRootId -> TryDecodeRootState accepts it
```

Primary failure call chain:

```text
Write of an unknown format ("8mm")
  -> ParseFilmFormatUpdate throws std::invalid_argument "film_format must be one of 16mm, 35mm, 70mm"
  -> ParseEditorParameterWrite returns no write, with the error; no Model changes
Stored document with "film_format": "8mm" or a number
  -> ReadFilmFormat throws std::runtime_error "DRT params: film_format has the unknown identifier '8mm'..."
  -> the load fails before Mutate; no field changes
```

What was proven (executed tests, debug build):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Section 2.4, bytes and root id of the old documents | `PipelineDocumentCheckpointFormat.ExistingFilmGrainRootDocumentsSerializeByteIdenticalAfterFormatChange` | `PipelineDocumentCheckpointTest` | PASS |
| Section 2.4, replay, undo, and redo of the old entries | `PipelineDocumentCheckpointFormat.ExistingFilmGrainHistoryReplaysAfterFormatChange` | `PipelineDocumentCheckpointTest` | PASS |
| `film_format` only when not `35mm`; each format round-trips | `DrtFilmFormatModel.DefaultDocumentReads35mmAndWritesNoFilmFormatKey`, `EveryFormatRoundTripsThroughDocumentJson`, `Return35mmRemovesTheKeyAndGivesTheDefaultBytes`, `OutputTransformJsonExcludesFilmFormat` | `GpuDagModelGraphTest` | PASS |
| Dirty flag, revision, and field-named errors | `DrtFilmFormatModel.FormatChangeMarksOnlyTheFilmFormatField`, `ValueOutsideTheEnumThrowsWithTheFieldNameAndChangesNothing`, `LoadRejectsAnInvalidStoredFormatAndNamesTheKey`, `LoadWithoutTheKeyReads35mm` | `GpuDagModelGraphTest` | PASS |
| Range 0 to 200 %; old values in range, not changed | `EditorParameterCatalogTest.FilmGrainRangeIsTwiceTheDatasheetGranularity`, `ScalarRangesMatchPreviousPanelSliders`, `OutOfRangeValueIsRejectedWithRange` | `EditorParameterCatalogTest` | PASS |
| The enum `film_format` | `EditorParameterCatalogTest.FilmFormatOptionsAreTheThreeFormatsWithDefault35mm`, `ObjectFieldsAreKnownFieldKeysWithPanels`, `EveryCatalogFieldCoversEveryFieldKey` | `EditorParameterCatalogTest` | PASS |
| Target, projection, and write of the field | `EditorAdjustmentContextTest.FilmFormatFieldTargetsDocumentDrtAndProjectsTheFormat` | `EditorAdjustmentContextTest` | PASS |
| History round trip; `odt` does not hold the format | `PipelineHistoryApplierTest.FilmFormatRoundTripsWithoutEnteringOdtHistoryJson` | `PipelineHistoryApplierTest` | PASS |
| Transfer of the format | `DocumentTransferTest.DrtPasteAppliesSourceFilmFormat`, `DrtPasteFromSourceWith35mmSetsTargetFormatTo35mm` | `DocumentTransferTest` | PASS |
| Both selectors show the value | `EditorDisplayTransformSnapshotQmlTest.FilmFormatSelectorsOfBothPagesShowTheStoredFormat` | `EditorDisplayTransformSnapshotQmlTest` | PASS |
| One click, one settled write (one history entry) | `EditorDisplayTransformSnapshotQmlTest.FilmFormatSelectionWritesOneSettledFilmFormatWrite` | `EditorDisplayTransformSnapshotQmlTest` | PASS |
| Caption and slider maximum of the Post Processing page | `EditorDisplayTransformSnapshotQmlTest.FilmGrainCaptionNamesTheFilmOfItsGranularity`, `EditorAdjustmentModelTest` (maximum 200) | `EditorDisplayTransformSnapshotQmlTest`, `EditorAdjustmentModelTest` | PASS |

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 4 --target GpuDagModelGraphTest EditorParameterCatalogTest
  EditorAdjustmentContextTest PipelineHistoryApplierTest DocumentTransferTest PipelineDocumentCheckpointTest
  EditorPanelProjectionTest EditorDisplayTransformSnapshotQmlTest EditorAdjustmentModelTest AdjustmentTransferCatalogTest
  GpuDagRawInputTest alcedo_main
<Target>_runtime\<Target>.exe for each target; GpuDagRawInputTest with the filter of G1
```

Suite totals: `GpuDagModelGraphTest` 182 / 182, `EditorParameterCatalogTest` 40 / 40, `EditorAdjustmentContextTest` 19 / 19,
`PipelineHistoryApplierTest` 14 / 14, `EditorPanelProjectionTest` 12 / 12, `EditorAdjustmentModelTest` 19 / 19,
`AdjustmentTransferCatalogTest` 9 / 9, `EditorDisplayTransformSnapshotQmlTest` 30 / 30, `GpuDagRawInputTest` with the filter of G1
51 / 51. `PipelineDocumentCheckpointTest` 20 / 22 and `DocumentTransferTest` 36 / 37: the failures are older than G2 and have issues.
`ExistingRawRootDocumentsSerializeByteIdenticalAfterRasterChange` and `ExistingLmtRootDocumentsSerializeByteIdenticalAfterEncodingChange`
fail on the HLS defaults ([#553](https://github.com/zidage/AlcedoStudio/issues/553)).
`TransferPackageCarriesOnlyOwnedDevelopKeysAndNoCaches` fails on the color wheel key `version`
([#484](https://github.com/zidage/AlcedoStudio/issues/484), [#554](https://github.com/zidage/AlcedoStudio/issues/554)).

Checklist / exit condition: steps 0 to 3 done; each row of section 2.4 that G2 owns has a passing test; no GPU file changed.

LOC note: 33 files, 991 added lines in the code and the tests, and the plan record. Product C++ 208, QML 154, translations 12, tests
614, stored documents 3 (one line each). `pipeline_document_checkpoint_test.cpp` 204, `drt_film_format_model_test.cpp` 123,
`editor_display_transform_snapshot_qml_test.cpp` 123.

Remaining gaps: none for G2. G3 reads the format and the plan in the CUDA post grain; G7 reads the format in the film print image
plane.

### G3: post grain on CUDA

**Objective and deliverables.** The CUDA post grain uses the passes of section 6.3. Its size and strength follow the plan.

**Files.** `edit/runtime/cuda/cuda_neighbor_grade.cuh` and `.cu`, `include/edit/runtime/cuda/cuda_film_grain_math.cuh`,
`edit/runtime/adjustment_runtime.cpp` (the grain no longer uses `kFilmGrainDyeCloudSigma`), `film_grain_math.h` (the unit field
comes from the grain image).

**Steps.**

1. Add the G-noise kernel with the halo and the aggregate k.
2. Blur with per-channel weights.
3. Apply `FilmGrainDyeClouds` with the unit grain image and the amplitude of the plan.
4. Remove the Bernoulli draw and the 64-bit per-tap hash of the grain.

**Tests** (`GpuDagCudaDrtProductTest`, statistical, expected values from G0):

- per-pixel standard deviation on a flat grey at 16mm, 35mm, 70mm and at 2560 and 6000 px: within 5 % of the evaluator;
- lag-1 correlation within 0.03;
- zoom: a render at scale 0.5 against the full render box-downscaled by 2: standard deviation within 5 %, pattern correlation above
  0.9;
- ROI: a 512 x 512 ROI equals the same region of the full render within 1e-5;
- the colour statistics of `film_grain_colour_statistics.hpp` still pass;
- strength 0 runs no grain pass.

**Timing.** Release, RTX 3080 Laptop, 6000 px: at most 4 ms for the grain at 35mm, at most 6 ms at 16mm. Record the old value on the
same machine first.

**Exit criteria.** The tests pass. The fit-view defect of section 5.2 is closed by the zoom test.

**Completion record.** Complete 2026-11-05, branch `feature/film-grain-cuda-post-grain`, stacked on the G2 branch
`feature/film-grain-format-field`. OpenCL and Metal keep the Bernoulli grain until G4.

Files: `include/edit/runtime/film/film_grain_field.h` (`FilmGrainNoiseParams`, `FilmGrainFieldParams`, `FilmGrainAggregateNoise`,
`FilmGrainMix`), the new `include/edit/runtime/film/film_grain_post.h` (`FilmGrainPostParams`, `FilmGrainPostPixel`),
`film_grain_plan.hpp/.cpp` (`MakeFilmGrainFieldParams`, `MakeFilmGrainPostParams`, the post target, the curve ranges, the scan
correlation), the new `include/edit/runtime/drt_post_film_grain.hpp` (`MakeDrtPostFilmGrainPlan`, `MakeDrtPostFilmGrainEncoding`),
`drt_post_executor.hpp` (`Ops::kImplementsFilmGrainField`, `Ops::DispatchFilmGrain`), `drt_output_resolver.hpp/.cpp`
(`ResolveNodeEotf`), the new `edit/runtime/cuda/cuda_film_grain_pass.cu` and `include/edit/runtime/cuda/cuda_film_grain_pass.hpp`
(`RecordCudaFilmGrainPost`), `cuda_drt_pass.cu`, `cuda_neighbor_grade.cu/.cuh` (the Bernoulli grain removed),
`opencl_drt_pass.cpp` and `metal_drt_pass.mm` (`kImplementsFilmGrainField = false`). `cuda_film_grain_math.cuh` moved to
`alcedo_studio/deprecated/legacy_pipeline/` with no change.

Details that the plan did not give, or changes to the plan, chosen in G3:

- Decision U5 (user, 2026-11-05): the post grain has the #660 colour of the 5207 scans, not the Endura display correlation of G0.
  The plan mixes the post fields with the Cholesky factor of rg 0.661, gb 0.41, rb 0.246 (`kFilmGrainPostScanCorrelation`), and the
  relative standard deviation of the linear display light at the grey is the green G0 target 0.0100 / 0.191 times 1.2 : 1 : 1.4
  (`kFilmGrainPostChannelRatio`), at the 48 um aperture. `FilmGrainPlanLabTest.AggregateAndMixMatchTheLabEvaluator` no longer
  compares the post mix with `post_cholesky` of the lab.
- One kernel in place of three passes. Each 16 x 16 tile makes its noise and halo in shared memory, then the horizontal blur, the
  vertical blur, the unit field, the mix, and the apply step. The first form, with a pool noise image of (w + 2 halo) x (h + 2 halo)
  and a horizontal blur image of w x (h + 2 halo), failed `CudaNeighborStagesReuseWorkspaceTexturesAfterFirstRender`: the texture
  pool frees an idle image whose size no live lease holds, so the two images were made again on each frame. The kernel uses no pool
  image and no global intermediate image (risk "The halo adds memory" of section 10 is gone). Shared memory is 3 (16 + 2 halo)^2
  floats, at most 48 KB at the radius 24. The noise of a halo is made again by each tile that reads it: about 1.6 hashes per pixel
  at 35mm and 6000 px, and 1.9 at 16mm.
- The apply step (`film_grain_post.h`) scales the linear display light, y' = y exp(-e), so no grain makes negative light. It decodes
  the code value with the transfer function of the output (`ResolveNodeEotf`, which runs no precompute) and encodes it again; for
  ST 2084 the light relative to the reference white is the decoded light times 10000 / `ColorUtils::ref_lum`. The tone form is the
  #660 form, sqrt(q (1 - q)) times the channel curve of the stock at the gross density lerp(first node, last node, q), with q the
  sRGB code of the relative light. The grain is 0 at black and at or above the reference white, as on a print paper. Near white the
  brightening half gets smaller, as in #660. HLG decodes with the inverse OETF only, so its relative grain differs by the OOTF gain
  (about 17 % on a neutral). A value with no grain keeps its code value exactly.
- The plan is made for each render from the 5207 data of `SharedFilmPrintTables`, the format of the DRT node, and the strength. It
  costs a few microseconds, so no state is kept between frames and the render does not read `DrtDirty::FilmFormat`.
- The post grain reads the film stock bundle for every DRT method. A missing or damaged bundle fails the render with its real error
  (section 6.5). `GpuDagCudaPrimaryGradeTest` now copies the resources next to the executable, as `GpuDagCudaDrtProductTest` does.
- The noise reads the reference pixels for every render, so a viewport region has the grain of the same region of the full frame
  (issue [#687](https://github.com/zidage/AlcedoStudio/issues/687)), on CUDA. `GradeNeighborParams` keeps
  `use_reference_coordinates` for OpenCL and Metal until G4; `adjustment_runtime.cpp` keeps the old weights for them too.
- The Bernoulli kernels of CUDA throw `std::invalid_argument` when they get Film Grain, so no path can run the old grain on CUDA.

Primary success call chain:

```text
ExecuteCudaDrt -> DrtPostExecutor<CudaDrtOps>::Execute
  -> MakeGradeNeighborParams (enabled when strength > 0) -> DrtPostNeighborhoodRuns
  -> MakeDrtPostFilmGrainPlan(drt, model, plan.geometry)
     -> SharedFilmPrintTables().Bundle().Grain("kodak_5207") -> MakeFilmGrainPlan(..., Post, strength)
  -> display transform write -> CudaDrtOps::DispatchFilmGrain
     (MakeDrtPostFilmGrainEncoding -> DrtOutputResolver::ResolveNodeEotf)
  -> RecordCudaFilmGrainPost -> MakeFilmGrainFieldParams, MakeFilmGrainPostParams
  -> FilmGrainPostKernel: FilmGrainAggregateNoise (tile + halo, shared) -> blur x -> blur y
     -> unit field -> FilmGrainMix -> FilmGrainPostPixel -> display output
```

Primary failure call chain:

```text
Missing or damaged film stock bundle
  -> SharedFilmPrintTables throws std::runtime_error with the file name -> the render fails; no partial grain is published
Strength 0
  -> DrtPostNeighborhoodRuns false -> no grain kernel runs (RecordCudaFilmGrainPost throws for a disabled plan)
Unknown display EOTF
  -> ResolveNodeEotf throws std::runtime_error "ExecuteCudaDrt: ... unsupported DRT encoding EOTF"
Film Grain parameters in the CUDA neighborhood kernels
  -> LaunchBlurHorizontal / LaunchApplyVertical throw std::invalid_argument
```

What was proven (executed tests, debug build, RTX 3080 Laptop):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Per-pixel standard deviation at a flat grey, 3 formats x 2560 and 6000 px, within 5 % of the evaluator | `CudaFilmGrainPostTest.PixelSigmaAtGreyMatchesTheEvaluatorForEachFormatAndSize` (green: `post_pixel_sigma_linear`; red and blue: the U5 ratio at the lab pixel factor) | `GpuDagCudaDrtProductTest` | PASS |
| Lag-1 correlation within 0.03 | same test (sum of L^2 times `lag1_kernel`) | `GpuDagCudaDrtProductTest` | PASS |
| Zoom: scale 0.5 against the full render box-downscaled, standard deviation within 5 %, pattern correlation above 0.9 (issue 674) | `CudaFilmGrainPostTest.HalfScaleRenderHasTheGrainOfTheFullRenderBoxDownscaled` (16mm and 35mm, 6000 px) | `GpuDagCudaDrtProductTest` | PASS |
| ROI: a 512 x 512 region equals the full render within 1e-5 (issue 687) | `CudaFilmGrainPostTest.ViewportRegionEqualsTheSameRegionOfTheFullRender` (origin 250, 317: the tiles do not line up) | `GpuDagCudaDrtProductTest` | PASS |
| The kernel equals the host form of its steps, partial tiles, aggregate 2 | `CudaFilmGrainPostTest.KernelEqualsTheHostFormOfItsStepsAtTheTileEdges` | `GpuDagCudaDrtProductTest` | PASS |
| Colour statistics of `film_grain_colour_statistics.hpp` | `CudaPrimaryGradeFixture.CudaFilmGrainColourIsWarmCoolWithTheQuietestGreen`, `CudaFilmGrainPostTest.ChannelCorrelationAtGreyIsTheScanCorrelationOf660` | `GpuDagCudaPrimaryGradeTest`, `GpuDagCudaDrtProductTest` | PASS |
| Strength 0 runs no grain pass | `CudaPrimaryGradeFixture.CudaFilmGrainAtStrengthZeroRunsNoPostPass`, `CudaFilmGrainPostTest.DisabledPlanIsRejectedBeforeAnyKernel` | `GpuDagCudaPrimaryGradeTest`, `GpuDagCudaDrtProductTest` | PASS |
| The same change of light for sRGB and ST 2084 output | `CudaFilmGrainPostTest.St2084OutputGetsTheSameChangeOfLinearLightAsSrgbOutput` | `GpuDagCudaDrtProductTest` | PASS |
| No allocation on the second render | `CudaPrimaryGradeFixture.CudaNeighborStagesReuseWorkspaceTexturesAfterFirstRender` | `GpuDagCudaPrimaryGradeTest` | PASS |
| Post target, scan mix, kernel parameters, aggregate noise, apply step | `FilmGrainPostPlan.*` (4), `FilmGrainPostField.*` (2), `FilmGrainPostApply.*` (5) | `GpuDagRawInputTest` | PASS |

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target GpuDagRawInputTest GpuDagCudaDrtProductTest
  GpuDagCudaPrimaryGradeTest EditRuntimeOpenCl
GpuDagRawInputTest.exe --gtest_filter="FilmGrainPlan*:FilmGrainField*:FilmGrainPost*:FilmFormatTest*:FilmGrainMath*:
  GpuDagAdjustmentRuntime*:FilmPrintImagePlan*:DrtPost*"
GpuDagCudaDrtProductTest.exe
GpuDagCudaPrimaryGradeTest.exe --gtest_filter="*Grain*:*Neighbor*:*DrtPost*:*Halation*"
cmd /c scripts\msvc_env.cmd --preset win_release -DALCEDO_BUILD_TESTS=ON   (timing only, then OFF again)
cmd /c scripts\msvc_env.cmd --build --preset win_release --target GpuDagCudaDrtProductTest
GpuDagCudaDrtProductTest.exe --gtest_filter=CudaFilmPrintFixture.DISABLED_DrtCostRecordAtEditorSizes
  --gtest_also_run_disabled_tests   (on this branch and on 839abddbb)
```

Suite totals: `GpuDagRawInputTest` with the filter 66 / 66; `GpuDagCudaDrtProductTest` 119 / 119 (the cost record is disabled); `GpuDagCudaPrimaryGradeTest` with the
filter 8 / 10. The two failures are older than G3 and do not use Film Grain: `CudaNeighborGradeDetailPixelsAreUnchanged` and
`CudaSceneWorkPairFixture.NeighborhoodApplyReadsScratchBeforeInPlaceWrite` fail the same way on `839abddbb` (the G2 head), issue
[#690](https://github.com/zidage/AlcedoStudio/issues/690). `EditRuntimeOpenCl` builds with the shared executor change. Metal was not
built (no Mac in this phase); its change is the constant `kImplementsFilmGrainField = false`.

Timing (Release, RTX 3080 Laptop, `CudaFilmPrintFixture.DISABLED_DrtCostRecordAtEditorSizes`, median of 9 GPU times of the DRT
pass, the grain is the difference against OpenDRT alone):

| Render | OpenDRT alone | Old grain (`839abddbb`) | G3, 35mm | G3, 16mm |
| --- | ---: | ---: | ---: | ---: |
| 2560 x 1709 | 1.36 ms | +1.42 ms | +0.92 ms | +1.05 ms |
| 4096 x 2735 | 3.33 ms | +3.57 ms | +1.51 ms | +1.66 ms |
| 6000 x 4006 | 7.13 ms | +9.98 ms | +3.26 ms | +4.31 ms |

The limits of the plan at 6000 px are 4 ms (35mm) and 6 ms (16mm): both pass. The old grain has the same cost for every format, so
G3 is 3.1 times faster at 35mm and 2.3 times faster at 16mm. Each grain value holds the Post write itself (one more pass over the
display image). The record now adds the DRT/Post adjustments of the document to its plan and allocates the scene work images; before
G3 it measured no Post write. Release built with `-DALCEDO_BUILD_TESTS=ON` for this record only; `build/release` is back at `OFF`.

Checklist / exit condition: steps 1 to 4 done; the tests of the section pass; the fit-view defect of section 5.2 is closed by the
zoom test (issue [#674](https://github.com/zidage/AlcedoStudio/issues/674)).

LOC note: 24 files, 1492 added and 85 removed lines against `839abddbb`, without this record. Product code about 650, tests 754 (`cuda_film_grain_test.cpp` 438, `film_grain_post_test.cpp` 246, the rest in existing test files), the archived header unchanged. `cuda_film_grain_pass.cu` 151, `film_grain_post.h` 141, `film_grain_plan.cpp` 322. `cuda_film_print_test.cpp` is 1692 lines and `cuda_primary_grade_test.cpp` 1147; G3 adds 33 and 21 lines to them, next to their tests of the same subject.

Remaining gaps: OpenCL and Metal still have the old grain, its size, and the ROI pattern of #687 (G4). The post grain still runs
under the film print until G5 skips it (U4).

### G4: post grain on OpenCL and Metal

**Objective and deliverables.** OpenCL and Metal give the G3 result.

**Files.** `edit/runtime/opencl/shader/primary_grade_neighbor.cl`, `edit/runtime/opencl/opencl_gpu_dag_programs.cpp` (list
`film_grain_field.h`), `edit/runtime/metal/shader/drt_neighbor.metal`, the Metal pass source.

**Steps.** Port G3. On Metal, use `half4` for the noise and blur images and the sliding-window blur of the film print. On OpenCL,
pass the per-channel weights in the per-render parameter upload (the 1024-byte argument limit of OpenCL 1.2).

**Tests.** `GpuDagOpenClGradeTest` and `GpuDagMetalDrtTest`: the G3 statistics; the standard deviation against CUDA within 3 %.

**Timing.** Metal, Apple M4, 6000 px: at most 15 ms (the old grain is 56.5 ms at 24 MP). OpenCL: at most 6 ms on the RTX 3080 Laptop.

**Exit criteria.** The tests pass on Windows (OpenCL) and on the Mac (Metal). Check the free space of the Mac first.

**Completion record.** Complete 2026-11-06, branch `feature/film-grain-opencl-metal-post-grain`, stacked on the G3 branch
`feature/film-grain-cuda-post-grain`. OpenCL is verified on Windows and Metal on the Apple M4. The tests pass on both. Metal is above
the timing limit at 16mm (23 ms against 15 ms); issue [#696](https://github.com/zidage/AlcedoStudio/issues/696) records the gap
and its measurements.

Files: the new `edit/runtime/opencl/shader/film_grain_post.cl`, `edit/runtime/opencl/opencl_film_grain_pass.cpp`, and
`include/edit/runtime/opencl/opencl_film_grain_pass.hpp` (`RecordOpenClFilmGrainPost`, program `opencl_dag_film_grain`); the new
`edit/runtime/metal/shader/film_grain_post.metal` (its own AIR file, which the DRT library links; `src/metal/CMakeLists.txt`),
`color_encoding_math.h` (`CE_OPENCL_POW_EXP2_LOG2`, `CE_METAL_POW_EXP2_LOG2`), `edit/runtime/metal/metal_film_grain_pass.mm`, and
`include/edit/runtime/metal/metal_film_grain_pass.hpp` (`RecordMetalFilmGrainPost`); `film_grain_field.h` and `film_grain_post.h`
(pointer parameters, `FILM_GRAIN_FIELD_PRIVATE`, `FILM_GRAIN_FIELD_PARAMS`); `cuda_film_grain_pass.cu` (the band layout);
`opencl_drt_pass.cpp`, `metal_drt_pass.mm`, `drt_post_executor.hpp` (`DispatchFilmGrain` on every backend, `kImplementsFilmGrainField`
removed); `primary_grade_neighbor.cl`, `drt_neighbor.metal`, `opencl_neighbor_dispatch.hpp` (the Bernoulli grain removed; the
neighborhood dispatches throw `std::invalid_argument` for Film Grain); `adjustment_runtime.hpp/.cpp` (the grain fields of
`GradeNeighborParams` removed, 344 to 300 bytes; Film Grain carries only its strength there). `film_grain_math.h` and
`film_grain_math_test.cpp` moved to `alcedo_studio/deprecated/legacy_pipeline/` with no change. The tests of the three backends share
the new `tests/edit/runtime/film_grain_post_checks.hpp`.

Details that the plan did not give, or changes to the plan, chosen in G4:

- One kernel per backend, as in G3, in place of the half4 noise and blur images and the sliding-window blur of the plan. The kernel
  has no image between its steps, so it moves only the source and the output pixels; the half4 images and the sliding window reduce
  the bytes of images that the kernel does not have.
- The band layout. A 16 x 16 tile makes its noise one band of 16 rows at a time and blurs each band across. Local or threadgroup
  memory holds the band and the horizontal blur: 2 x 3 x 16 x (16 + 2 halo) floats, 24 KB at the radius 24. The G3 layout (the
  whole noise tile, 48 KB at the radius 24) is above the 32 KB of local memory that OpenCL 1.2 guarantees and above the 32 KB of
  threadgroup memory of Apple GPUs. CUDA has the same band layout now: the G3 layout used exactly 48 KB at the radius 24, and the
  612 bytes of the apply parameters in shared memory (next item) made that launch fail with `invalid argument`
  (`CudaFilmGrainPostTest.LargestBlurRadiusEqualsTheHostForm`). The number of noise texels per pixel is the same as in G3.
- The apply step and the curve read their parameters through pointers in the parameter memory of the kernel
  (`FILM_GRAIN_FIELD_PARAMS`: `__local const` on OpenCL, `constant` on Metal, `const` on CUDA and the host). With the parameters by
  value the OpenCL compiler copied the 612 bytes of `FilmGrainPostParams` for each pixel, and the OpenCL grain took 19.0 ms at 35mm
  and 6000 px. The OpenCL work-group copies the field and the apply parameters (1000 bytes) once into local memory; the CUDA block
  copies the apply parameters once into shared memory (a pointer to a kernel parameter makes a copy for each thread); Metal reads
  them from the constant buffer of `setBytes`. `FilmGrainPostPixel` decodes each channel once.
- The code value of the scaled light (`FilmGrainPostScaledCode`, issue
  [#693](https://github.com/zidage/AlcedoStudio/issues/693)): a pure power encoding has
  Encode(L f) = Encode(L) f^(1/g), so linear, gamma 2.2, 2.6, 1.8, and BT.1886 take code * exp(-error / g), and sRGB takes
  (code + 0.055) exp(-error / 2.4) - 0.055 in its power segment. Other encodings encode the light. Gamma 2.2 is the display encoding
  of a new document.
- The OpenCL grain program builds with `-DCE_OPENCL_POW_EXP2_LOG2`: `CE_POW` of `color_encoding_math.h` is exp2(y log2 x) there.
  The pow of the NVIDIA OpenCL driver cost about 0.16 ms per call and pixel at 24 MP; the relative error of the form is about 2e-6
  on the gamma and sRGB curves and up to about 2e-5 near the black of the ST 2084 decode. The other OpenCL programs keep pow.
- The Metal grain shader has the same form: `film_grain_post.metal` is its own AIR file with `-DCE_METAL_POW_EXP2_LOG2`, so
  `CE_POW` is `fast::exp2(y * fast::log2(x))` there, and the DRT library links that AIR file as it links the film print. The other
  DRT shaders keep `precise::pow`. With `precise::pow` the kernel took 28.3 ms (35mm) and 35.0 ms (16mm) at 6000 px on the M4; the
  decode and the sRGB encode of the tone coordinate were 15 ms of it. With the form it takes 16.2 ms and 22.5 ms, and the
  `MetalFilmGrainPostTest` checks against the host form pass.
- The curve loop keeps its fixed count of 15 segments: a loop to the counted segments was slower (the compiler does not unroll it).
- `FilmGrainMix` takes a pointer in the private memory of the thread (`FILM_GRAIN_FIELD_PRIVATE`), because Metal needs an address
  space on every pointer parameter.
- The fixed values of OpenCL are one upload in the transient arena of the render (`FilmGrainPostDeviceData`, 1000 bytes), because
  they are above the 1024 bytes of by-value arguments of OpenCL 1.2. The kernel reads and writes images and scene work buffers.
- The reference of the "standard deviation against CUDA" criterion: on Windows, `OpenClFilmGrainPostTest` renders the same input
  with the CUDA pass. The Mac has no CUDA, so `MetalFilmGrainPostTest` compares with the host form of the steps
  (`film_grain_post_checks.hpp`, `HostForm`), which the CUDA kernel equals within 2e-5.
- The blur radius 24 needs a reference pitch of about 0.2 um: the render scale of the plan is at most 1, so a zoom does not make the
  radius larger. At 6000 px the halo is 2 (35mm) and 3 (16mm). The test of the radius 24 uses 16mm at the smallest long side
  6000 x 2^n with the radius 24.
- The Metal grade test copies the film stock bundle next to its executable, because the post grain reads the 5207 data on every
  backend (G3 did the same for `GpuDagCudaPrimaryGradeTest`).

Primary success call chain:

```text
DrtPostExecutor<Ops>::Execute -> MakeDrtPostFilmGrainPlan(drt, model, plan.geometry)
  -> Ops::DispatchFilmGrain (MakeDrtPostFilmGrainEncoding)
  OpenCL: RecordOpenClFilmGrainPost -> MakeFilmGrainFieldParams, MakeFilmGrainPostParams
          -> transient upload of FilmGrainPostDeviceData -> film_grain_post_rgba32f
  Metal:  RecordMetalFilmGrainPost -> setBytes (field, post, size), threadgroup memory -> film_grain_post
  kernel: copy of the parameters -> for each band: FilmGrainAggregateNoise -> blur x
          -> blur y -> unit field -> FilmGrainMix -> FilmGrainPostPixel -> display output
```

Primary failure call chain:

```text
Strength 0                    -> no grain dispatch; RecordOpenClFilmGrainPost / RecordMetalFilmGrainPost throw for a disabled plan
Film Grain in a neighborhood  -> RequireOpenClNeighborhoodKernelBehavior / RequireNeighborhoodKernelBehavior throw
Missing film stock bundle     -> SharedFilmPrintTables throws with the file name; the render fails
Metal pipeline below 256 threads to a threadgroup -> RecordMetalFilmGrainPost throws std::runtime_error
```

What was proven (debug build, RTX 3080 Laptop, Windows):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| G3 statistics on OpenCL: pixel sigma (3 formats x 2 sizes, 5 %), lag-1 (0.03), half scale (5 %, correlation 0.9), ROI (1e-5), channel correlation, ST 2084 | `OpenClFilmGrainPostTest.*` (6 tests) | `GpuDagOpenClGradeTest` | PASS |
| Standard deviation against CUDA within 3 % | `OpenClFilmGrainPostTest.StandardDeviationIsWithin3PercentOfCuda` | `GpuDagOpenClGradeTest` | PASS |
| Kernel equals the host form at partial tiles and at the radius 24 | `OpenClFilmGrainPostTest.KernelEquals...`, `.LargestBlurRadius...`; `CudaFilmGrainPostTest` the same | `GpuDagOpenClGradeTest`, `GpuDagCudaDrtProductTest` | PASS |
| Scene work buffers give the result of images | `OpenClFilmGrainPostTest.SceneWorkBuffersGiveTheResultOfImages` | `GpuDagOpenClGradeTest` | PASS |
| Strength 0 runs no grain pass; colour of the product path | `OpenClGradeFixture.OpenClFilmGrainAtStrengthZeroRunsNoPostPass`, `.OpenClFilmGrainColourIsWarmCool...`, `.OpenClFilmGrainStrengthScales...` | `GpuDagOpenClGradeTest` | PASS |
| CUDA after the band layout and the pointer parameters | `CudaFilmGrainPostTest.*` (8), `CudaPrimaryGradeFixture` grain cases | `GpuDagCudaDrtProductTest`, `GpuDagCudaPrimaryGradeTest` | PASS |
| Host plan, apply step, neighborhood strength | `FilmGrainPlan*`, `FilmGrainPost*`, `GpuDagAdjustmentRuntime.FilmGrainNeighborhoodCarriesOnlyTheStrength` | `GpuDagRawInputTest` | PASS (58 / 58) |

What was proven on the Mac (`macos-debug`, Apple M4):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| G3 statistics on Metal: pixel sigma, half scale, ROI, channel correlation, ST 2084 | `MetalFilmGrainPostTest.*` (5 tests) | `GpuDagMetalDrtTest` | PASS |
| Standard deviation within 3 % of the form of CUDA (the host form) | `MetalFilmGrainPostTest.StandardDeviationIsWithin3PercentOfTheFormOfCuda` | `GpuDagMetalDrtTest` | PASS |
| Kernel equals the host form at partial tiles and at the radius 24 (24 KB of threadgroup memory) | `MetalFilmGrainPostTest.KernelEquals...`, `.LargestBlurRadiusFitsTheThreadgroupMemory...` | `GpuDagMetalDrtTest` | PASS |
| Disabled plan rejected | `MetalFilmGrainPostTest.DisabledPlanIsRejectedBeforeAnyKernel` | `GpuDagMetalDrtTest` | PASS |
| Strength 0 runs no grain pass; colour and strength of the product path | `MetalGradeFixture.MetalFilmGrain*` (3 tests) | `GpuDagMetalGradeTest` | PASS |

Mac suite totals: `GpuDagMetalDrtTest` 46 passed, 5 skipped, 0 failed; the skipped tests are the lab comparisons of
`MetalFilmPrintFixture`, which read expected files that the G4 expected folder (`grain__expected.bin` only) does not have.
`GpuDagMetalGradeTest` 75 / 86; the 11 failures are those of issues [#497](https://github.com/zidage/AlcedoStudio/issues/497)
(LLF, Mask Mix, and `MetalPointwiseAdjustmentsUseOneDispatchPerLlfSegment`) and
[#496](https://github.com/zidage/AlcedoStudio/issues/496) (the six multi-grade tests).

Suite totals: `GpuDagOpenClGradeTest` 127 / 133; the 6 failures are `OpenClMultiGradeFixture.*` of issue
[#496](https://github.com/zidage/AlcedoStudio/issues/496). `GpuDagCudaDrtProductTest` 120 / 120. `GpuDagCudaPrimaryGradeTest` with the
grain filter 8 / 10; the 2 failures are those of issue [#690](https://github.com/zidage/AlcedoStudio/issues/690).
`GpuDagOpenClDrtProductTest` 31 / 32; the failure is the stale source count of issue
[#692](https://github.com/zidage/AlcedoStudio/issues/692).

Timing, OpenCL (debug host build, the driver compiles the kernel; `OpenClGradeFixture.DISABLED_FilmGrainCostRecordAt24Megapixels`,
6000 x 4000, Film Grain 100 %, median of 9, the grain is the difference against the DRT pass alone):

| Form | 35mm | 16mm |
| --- | ---: | ---: |
| Parameters by value | +19.0 ms | +20.3 ms |
| Parameters through a `__global` pointer | +7.9 ms | +8.4 ms |
| Parameters in local memory | +5.3 to 5.7 ms | +7.0 to 7.7 ms |
| Also the scaled code value and exp2(y log2 x) (this branch, 3 runs) | +4.5 to 5.2 ms | +5.7 to 5.8 ms |

The limit of 6 ms holds at both formats. The time without grain varies between runs (6.8 to 8.1 ms); the values above use the
runs at 6.8 ms. `OpenClFilmGrainPostTest.DISABLED_KernelCostRecordAt24Megapixels` times the kernel alone on scene work buffers in
gamma 2.2: 4.8 ms at 35mm and 6.1 ms at 16mm, with the enqueue and the wait. In that record the noise is about 1.6 ms at 16mm, the
apply step about 2 ms, and the read and the write of the two 24 MP float4 images about 2 ms.

Timing, CUDA after the band layout and before the scaled code value (Release,
`CudaFilmPrintFixture.DISABLED_DrtCostRecordAtEditorSizes`, median of 9 GPU times). The scaled code value removes work from the
apply step of CUDA too; this record is its upper limit:

| Render | OpenDRT alone | G3, 35mm | G4, 35mm | G3, 16mm | G4, 16mm |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2560 x 1709 | 1.36 ms | +0.92 ms | +1.03 ms | +1.05 ms | +1.11 ms |
| 4096 x 2735 | 3.33 ms | +1.51 ms | +1.74 ms | +1.66 ms | +1.85 ms |
| 6000 x 4006 | 7.16 ms | +3.26 ms | +3.74 ms | +4.31 ms | +4.72 ms |

The band layout adds two barriers for each band and costs about 0.4 ms at 6000 px. The limits of G3 (4 ms at 35mm, 6 ms at 16mm)
hold. Release built with `-DALCEDO_BUILD_TESTS=ON` for this record only; `build/release` is back at `OFF`.

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target GpuDagRawInputTest GpuDagCudaDrtProductTest
  GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest GpuDagOpenClDrtProductTest
GpuDagOpenClGradeTest.exe; GpuDagCudaDrtProductTest.exe; GpuDagOpenClDrtProductTest.exe
GpuDagCudaPrimaryGradeTest.exe --gtest_filter="*Grain*:*Neighbor*:*DrtPost*:*Halation*"
GpuDagRawInputTest.exe --gtest_filter="FilmGrainPlan*:FilmGrainField*:FilmGrainPost*:FilmFormatTest*:GpuDagAdjustmentRuntime*:
  FilmPrintImagePlan*:DrtPost*"
GpuDagOpenClGradeTest.exe --gtest_filter=OpenClGradeFixture.DISABLED_FilmGrainCostRecordAt24Megapixels --gtest_also_run_disabled_tests
GpuDagOpenClGradeTest.exe --gtest_filter=*KernelCostRecordAt24Megapixels --gtest_also_run_disabled_tests
```

Timing, Metal (Apple M4, `macos-debug`: the Metal shaders of a debug build differ from Release only by `-gline-tables-only`;
6000 x 4000, Film Grain 100 %, median of 9):

| Record | 35mm (halo 2) | 16mm (halo 5) |
| --- | ---: | ---: |
| Kernel alone, `precise::pow` (first build) | 28.3 ms | 35.0 ms |
| Kernel alone, exp2(y log2 x) (this branch; `MetalFilmGrainPostTest.DISABLED_KernelCostRecordAt24Megapixels`) | 16.2 ms | 22.5 ms |
| Frame difference, `precise::pow` (`MetalGradeFixture.DISABLED_FilmGrainCostRecordAt24Megapixels`, 3 runs) | +29.1 to 30.0 ms | +36.4 to 36.6 ms |
| Frame difference, this branch (3 runs) | +15.4 to 17.0 ms | +22.7 to 23.8 ms |

The limit of 15 ms holds about at 35mm and does not hold at 16mm. In the kernel record, the read and the write of the two 24 MP
float4 images are about 6.4 ms (768 MB at about 120 GB/s), the granularity curve about 4 ms, and the noise and the blur about 2 ms
at 35mm and 8 ms at 16mm (1.56 and 2.64 noise texels per pixel). Issue [#696](https://github.com/zidage/AlcedoStudio/issues/696)
has the ablation and the possible next steps. The old grain took 56.5 ms at 24 MP.

Commands (Mac, repository root; the schema 11 resources and `grain__expected.bin` of the lab are in `build/tmp/g4`):

```text
cmake -S . -B build/macos-debug -DALCEDO_FILM_PRINT_RESOURCE_DIR=$PWD/build/tmp/g4/resources
  -DALCEDO_FILM_PRINT_EXPECTED_DIR=$PWD/build/tmp/g4/expected
cmake --build build/macos-debug --target GpuDagMetalDrtTest GpuDagMetalGradeTest -j 8
GpuDagMetalDrtTest; GpuDagMetalGradeTest
GpuDagMetalDrtTest --gtest_filter='*KernelCostRecord*' --gtest_also_run_disabled_tests
GpuDagMetalGradeTest --gtest_filter='*FilmGrainCostRecordAt24Megapixels' --gtest_also_run_disabled_tests
```

Remaining gaps: the Metal timing at 16mm (#696).

### G5: grain in the negative on CUDA

**Objective and deliverables.** With the film print, CUDA adds the grain to the dye amounts in FP6. The post Film Grain pass is
skipped.

**Files.** `edit/runtime/drt_post_schedule.cpp`, `edit/runtime/cuda/cuda_film_print_pass.cu`, `film_print_math.h`
(`FilmKernelGrainAmounts`), `include/edit/runtime/film/film_print_kernel_tables.hpp` (the grain segments of the stock),
`film_print_plan.cpp` (the grain plan joins the image plan).

**Data rules.** The grain is added after the inhibitor step and before `FilmKernelFlareAmounts` and the table read, in the print,
Negative Scan, and light table forms. For a reversal film, the grain is added to the dye amount `dye_max - record`. The noise image
uses the halo rule, so an ROI equals the full frame.

**Tests** (`GpuDagCudaDrtProductTest`, `FilmPrintImagePlan`, statistical):

- the standard deviation of the dye amounts on a flat field, box-averaged to 48 um equivalent area, equals `sigma_a` of the stock
  within 5 % at three densities, for 5207, 5219, Portra 400, and Velvia 50;
- through Negative Scan of 5207, the channel correlation rg is within 0.05 of 0.661;
- with strength 0, the output equals the film print without grain (no grain pass runs);
- the post schedule skips Film Grain under the film print and keeps the stored value.

**Timing.** Release, RTX 3080 Laptop, 6000 px: the film print with grain adds at most 3 ms.

**Exit criteria.** The tests pass. The film print tests of F4 and F10 still pass with strength 0.

**Completion record.** Complete 2026-11-06, branch `feature/film-grain-cuda-negative-grain`, stacked on the G4 branch
`feature/film-grain-opencl-metal-post-grain`. OpenCL and Metal keep the post grain under the film print until G6.

Files: the new `include/edit/runtime/film/film_grain_negative.h` (`FilmGrainNodeCurve`, `FilmGrainNodeCurveSigma`,
`FilmGrainNegativeParams`, `FilmGrainNegativeRecord`); the new `include/edit/runtime/cuda/cuda_film_grain_tile.cuh`
(`FilmGrainTileKernel`, `FieldParamsFor`, `Launch`: the tile kernel of G3 and G4, moved out of `cuda_film_grain_pass.cu` so the
film print instantiates it); `film_grain_plan.hpp/.cpp` (`FilmGrainPlan::reversal`, the counted nodes of each curve,
`MakeFilmGrainNegativeParams`); `drt_post_film_grain.hpp` (`DrtFilmGrainStrength`, `MakeDrtFilmPrintGrainPlan`);
`drt_post_schedule.hpp/.cpp` and `drt_post_executor.hpp` (`Ops::kFilmPrintGrainInNegative`, the strength for
`Ops::DispatchFilmPrint`); `cuda_drt_pass.cu`, `opencl_drt_pass.cpp`, `metal_drt_pass.mm` (the constant: true on CUDA, false on
OpenCL and Metal); `cuda_film_print_pass.cu/.hpp` (the parameter `grain_strength`, `DisplayArgs`, `ScanDisplayArgs`,
`GrainDisplayApply`, `GrainScanDisplayApply`); `cuda_film_grain_pass.cu/.hpp` (`RecordCudaFilmGrainNegative`);
`gpu_node_pass_stats.hpp` (`film_grain_negative`). Tests: the new `film_grain_negative_test.cpp` (host),
`cuda_film_grain_negative_test.cpp`, and `film_grain_negative_checks.hpp` (the kernel checks that G6 shares);
`film_grain_post_checks.hpp` (`HostMixedGrain`, the field steps of the host form, shared by both apply steps).

Details that the plan did not give, or changes to the plan, chosen in G5:

- Place of the grain. The FP6 kernel adds the grain to the record of each pixel, in the print, light table, and Negative Scan
  forms, before `FilmKernelFlareAmounts`, the negative grade, and the table read. The FP6 kernel is the grain tile kernel with the
  display step as its apply step, so the amounts with grain are never written. The blurred amounts of the negative grade, the flare
  canvas, the loupe flare canvas, and the prescan read the amounts with no grain: they are means over many pixels, where the grain
  has a mean of 0, so their canonical results do not depend on the Film Grain and need no new invalidation rule. The first form, a
  separate in-place kernel on the amounts before FP6, added 3.57 ms (35mm) and 4.91 ms (16mm) at 6000 px; it read and wrote the
  whole amounts image once more. The plan files `film_print_math.h` (`FilmKernelGrainAmounts`), `film_print_kernel_tables.hpp`,
  and `film_print_plan.cpp` did not change: the grain plan of G1 holds the curves, and `film_grain_negative.h` holds the step.
- The node form of the layer curve (`FilmGrainNodeCurve`). The segment form of `FilmGrainCurveSigma` adds all 15 segments for each
  value; for the three layers of the FP6 kernel that was about 1.2 ms at 6000 x 4000. The node form finds the segment in 4 reads
  of the 16 nodes and reads one segment; it equals the segment form within 1e-6 relative
  (`FilmGrainNegativePlan.NodeCurveEqualsTheSegmentCurveOfThePlan`). The post grain keeps the segment form of G4, so OpenCL and
  Metal did not change.
- The tile of the film print has 16 columns and 32 rows: each thread of the 16 x 16 block applies the grain to two pixels, so the
  noise of the halo and the horizontal blur serve twice as many pixels (`Apply::kTileRows`). A band holds the whole tile with its
  halo up to 48 rows (one barrier pair). The post grain keeps 16 rows: with 32 rows its apply step took twice as long (+6.9 ms at
  35mm). The single band also takes about 0.2 ms from the post grain at 6000 px.
- Backends. The DRT/Post schedule skips Film Grain under the film print only on a backend with
  `Ops::kFilmPrintGrainInNegative` (CUDA). OpenCL and Metal keep the post grain under the film print until G6, as G3 kept the old
  grain on them until G4, so no backend loses the grain. Until G6 `DrtPostDecisionTrace` of a film print with grain differs
  between CUDA and the other backends by the Film Grain write.
- The strength. The executor passes the stored value of the enabled Film Grain to `DispatchFilmPrint` (0 when it is disabled), and
  `RecordCudaFilmPrint` makes the plan with `MakeDrtFilmPrintGrainPlan` before any GPU work, so a strength outside [0, 2] stops
  the render first. The Film Grain Model keeps its value (U4).
- Reversal films. The record is `dye_max - dye amount` (lab rule 'removed_dye_record'), so the step reads the curve at
  `dye_max - record` and subtracts the change from the record (`record_sign` -1).
- The 48 um check. A box of 48 um area is near the size of the dye cloud, where Selwyn's law does not hold (the pixel integral at
  42.5 um is about 5 % below 1 / B for sigma_c 2 um), so the box would be about 5 % below the datasheet value by the model itself.
  The test averages boxes of 32 px at 6 um (192 um) and takes the box value to 48 um by Selwyn's law,
  sigma_48 = sigma_box * B / sqrt(A_48).
- The Negative Scan correlation is the mean over the AP1 greys 0.03, 0.09, 0.25, and 0.7, as the lab fit of R averaged net green
  densities 0.1 to 0.7, of the correlation of ln of the linear AP1 of the scan.

Mean shift (risk of section 10), 5207 on 2383, 100 %, 6000 px, the change of the mean display light of a flat grey, in stops
(R, G, B; `CudaFilmPrintGrainTest.GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows`):

| AP1 grey | 35mm | 16mm |
| ---: | --- | --- |
| 0.005 | +0.048, +0.043, +0.054 | +0.085, +0.071, +0.075 |
| 0.02 | +0.120, +0.072, +0.148 | +0.201, +0.113, +0.199 |
| 0.06 | +0.078, +0.049, +0.125 | +0.133, +0.077, +0.166 |
| 0.18 | +0.038, +0.012, +0.035 | +0.058, +0.016, +0.047 |
| 0.5 | +0.001, -0.001, +0.002 | -0.000, -0.004, +0.005 |
| 1.5 | -0.001, -0.002, -0.004 | -0.002, -0.005, -0.005 |
| 4.0 | -0.001, -0.002, -0.003 | -0.002, -0.003, -0.004 |

The print makes light from density through a convex curve, so the grain makes the mean light of a print shadow brighter, as on a
real print. The test bounds are the measured values with a margin (35mm: 0.2 stop below the grey 0.18, 0.06 at and above; 16mm:
0.35 and 0.1).

Primary success call chain:

```text
DrtPostExecutor<CudaDrtOps>::Execute
  -> MakeGradeNeighborParams -> DrtPostNeighborhoodRuns(FilmPrint, Film Grain, true) is false
     -> DrtFilmGrainStrength(model)                       (the stored value stays)
  -> CudaDrtOps::DispatchFilmPrint(..., strength) -> RecordCudaFilmPrint(..., grain_strength)
     -> MakeDrtFilmPrintGrainPlan(drt, strength, geometry)
        -> bundle.Grain(film.negative) -> MakeFilmGrainPlan(..., Negative, strength)
     -> FP1 to FP5 -> blurred amounts, flare canvas, prescan (no grain)
     -> cuda_film_grain::FieldParamsFor, MakeFilmGrainNegativeParams (dye_max of a reversal film)
     -> cuda_film_grain::Launch(GrainDisplayApply or GrainScanDisplayApply)
        -> FilmGrainTileKernel: noise, blur x, blur y, unit field, FilmGrainMix
        -> FilmGrainNegativeRecord (FilmGrainNodeCurveSigma) -> DisplayPixel or ScanDisplayPixel
```

Primary failure call chain:

```text
Strength outside [0, 2]        -> MakeFilmGrainPlan throws std::invalid_argument before any GPU work; the render fails
Strength 0                     -> no plan; DisplayKernel or ScanDisplayKernel as before G5 (film_grain_negative stays 0)
Missing bundle or unknown film -> SharedFilmPrintTables / FilmStockBundle::Grain throws with the file or the film name
Reversal dye_max missing       -> MakeFilmGrainNegativeParams throws std::invalid_argument
Disabled or Post plan          -> RecordCudaFilmGrainNegative throws std::invalid_argument before any kernel
```

What was proven (executed tests, debug build, RTX 3080 Laptop):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| Dye amounts on a flat field, box-averaged and taken to 48 um, equal `sigma_a` within 5 % at three amounts, for 5207, 5219, Portra 400, Velvia 50 | `CudaFilmGrainNegativeTest.DyeAmountSigmaMatchesTheDatasheetForEachPrintStock` | `GpuDagCudaDrtProductTest` | PASS |
| Pixel sigma of each layer equals the plan within 5 % at 16mm, 35mm, 70mm | `CudaFilmGrainNegativeTest.PixelSigmaFollowsThePlanForEachFormat` | `GpuDagCudaDrtProductTest` | PASS |
| Negative Scan of 5207: rg within 0.05 of 0.661 (also gb of 0.41, rb of 0.246) | `CudaFilmPrintGrainTest.NegativeScanChannelCorrelationIsTheScanCorrelationOf660` (rg 0.661 to 0.673, gb 0.405 to 0.411, rb 0.248 to 0.267) | `GpuDagCudaDrtProductTest` | PASS |
| Strength 0 equals the film print without grain, no grain pass, in the three forms | `CudaFilmPrintGrainTest.StrengthZeroEqualsTheFilmPrintWithNoGrainKernel` | `GpuDagCudaDrtProductTest` | PASS |
| The schedule skips Film Grain under the film print and keeps the stored value; the grain follows the slider | `CudaFilmPrintGrainTest.FilmPrintSkipsThePostFilmGrainAndKeepsItsStoredValue`, `DrtPostSchedule.FilmPrintWithGrainInTheNegativeHasNoFilmGrainWrite` | `GpuDagCudaDrtProductTest`, `GpuDagRawInputTest` | PASS |
| An ROI equals the full frame (halo rule) | `CudaFilmGrainNegativeTest.ViewportRegionEqualsTheSameRegionOfTheFullRender` (1e-6) | `GpuDagCudaDrtProductTest` | PASS |
| The kernel equals the host form at partial tiles, aggregate 2, negative and reversal | `CudaFilmGrainNegativeTest.KernelEqualsTheHostFormOfItsStepsAtTheTileEdges` (1e-5) | `GpuDagCudaDrtProductTest` | PASS |
| Mean shift on a grey ramp (risk of section 10) | `CudaFilmPrintGrainTest.GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows` | `GpuDagCudaDrtProductTest` | PASS |
| Apply parameters, node curve, reversal record, rejected inputs | `FilmGrainNegativePlan.*` (5), `FilmGrainNegativeApply.*` (2) | `GpuDagRawInputTest` | PASS |
| F4 and F10 film print tests with strength 0; the post grain after the tile change | `GpuDagCudaDrtProductTest` (all) | `GpuDagCudaDrtProductTest` | PASS (129 / 129) |

Suite totals: `GpuDagCudaDrtProductTest` 129 / 129 (the two cost records are disabled). `GpuDagRawInputTest` with the filter of the
commands 71 / 71. `GpuDagCudaPrimaryGradeTest` with the grain filter 8 / 10; the 2 failures are those of issue
[#690](https://github.com/zidage/AlcedoStudio/issues/690). `GpuDagOpenClGradeTest` 127 / 133; the 6 failures are
`OpenClMultiGradeFixture.*` of issue [#496](https://github.com/zidage/AlcedoStudio/issues/496). Metal was not built (no Mac in this
phase); its change is the constant `kFilmPrintGrainInNegative = false`.

Timing (Release, RTX 3080 Laptop, `CudaFilmPrintFixture.DISABLED_DrtCostRecordAtEditorSizes`, median of 9 GPU times of the DRT
pass; the grain is the difference against the film print with Film Grain 0 % and the post adjustments compiled):

| Render | Film print, 0 % | Grain, 35mm | Grain, 16mm |
| --- | ---: | ---: | ---: |
| 2560 x 1709 | 3.78 ms | +0.44 ms | +0.60 ms |
| 4096 x 2735 | 11.30 ms | +0.77 ms | +1.08 ms |
| 6000 x 4006 | 16.61 ms | +1.90 ms | +2.92 ms |

The limit of 3 ms at 6000 px holds at both formats. Steps of the tuning at 6000 px (35mm / 16mm): the separate kernel +3.57 /
+4.91 ms; FP6 as the apply step +2.82 / +4.10; one band (the kernel alone from 3.41 / 4.67 to 3.09 / 4.21 ms); the node curve +1.51 / +3.20;
the tile of 32 rows +1.90 / +2.92 (between runs the values vary by about 0.25 ms). `__logf` in the Gaussian gave 0.2 ms at 16mm
and nothing at 35mm, and blurs with a loop to the radius of each channel were 0.3 ms slower (the compiler does not unroll them);
neither is in the product. `CudaFilmGrainNegativeTest.DISABLED_KernelCostRecordAt24Megapixels` times the grain step alone on
6000 x 4000 amounts: 1.76 ms (35mm) and 2.75 ms (16mm). The post grain at 6000 px after this phase: +3.52 ms (35mm) and +4.56 ms
(16mm), against +3.74 and +4.72 in G4. Release built with `-DALCEDO_BUILD_TESTS=ON` for these records only; `build/release` is back
at `OFF`.

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --build --preset win_debug --parallel 6 --target GpuDagRawInputTest GpuDagCudaDrtProductTest
  GpuDagCudaPrimaryGradeTest GpuDagOpenClGradeTest
GpuDagRawInputTest.exe --gtest_filter="FilmGrainPlan*:FilmGrainField*:FilmGrainPost*:FilmGrainNegative*:FilmFormatTest*:
  GpuDagAdjustmentRuntime*:FilmPrintImagePlan*:DrtPost*:DrtWriteSequence*:NeighborExecutor*"
GpuDagCudaDrtProductTest.exe; GpuDagOpenClGradeTest.exe
GpuDagCudaPrimaryGradeTest.exe --gtest_filter="*Grain*:*Neighbor*:*DrtPost*:*Halation*"
cmd /c scripts\msvc_env.cmd --preset win_release -DALCEDO_BUILD_TESTS=ON   (timing only, then OFF again)
cmd /c scripts\msvc_env.cmd --build --preset win_release --target GpuDagCudaDrtProductTest
GpuDagCudaDrtProductTest.exe --gtest_filter="*KernelCostRecordAt24Megapixels:CudaFilmPrintFixture.DISABLED_DrtCostRecordAtEditorSizes"
  --gtest_also_run_disabled_tests
```

Checklist / exit condition: the tests of the section pass; the film print tests of F4 and F10 pass with strength 0 (the full
`GpuDagCudaDrtProductTest`); the timing limit holds.

LOC note: 24 files, 1703 added and 232 removed lines against `f1c39fad7`, without this record. Product code 765 added (the tile
kernel moved from `cuda_film_grain_pass.cu` to `cuda_film_grain_tile.cuh`, 191 lines), tests 934 (`cuda_film_grain_negative_test.cpp`
398, `film_grain_negative_checks.hpp` 254, `film_grain_negative_test.cpp` 200). `cuda_film_print_pass.cu` is 1245 lines; the FP6
kernels are split into their pixel functions for the apply steps.

Remaining gaps: OpenCL and Metal add the post grain under the film print until G6. G6 sets `kFilmPrintGrainInNegative` on them,
ports the FP6 apply step with `film_grain_negative.h`, and runs `film_grain_negative_checks.hpp` and the film print checks of
`cuda_film_grain_negative_test.cpp`.

### G6: grain in the negative on OpenCL and Metal

**Objective and deliverables.** OpenCL and Metal give the G5 result in all three forms.

**Files.** `edit/runtime/opencl/opencl_film_print_pass.cpp`, `shader/film_print.cl`, `shader/film_print_scan.cl`,
`edit/runtime/metal/metal_film_print_pass.mm`, `shader/film_print.metal`, `shader/film_print_scan.metal`.

**Tests.** `GpuDagOpenClGradeTest` (`opencl_film_print_test.cpp`) and `GpuDagMetalDrtTest` (`metal_film_print_test.cpp`): the G5
statistics, and the standard deviation against CUDA within 3 %.

**Timing.** Metal, Apple M4, 6000 px: the film print with grain adds at most 10 ms.

**Exit criteria.** The tests pass on both platforms.

**Completion record.** Pending.

### G7: the format sets the film print image plane

U2 is yes (2026-11-05), so this phase runs.

**Objective and deliverables.** `MakeFilmPrintImagePlan` computes `pixel_um` from `FilmFrameLongSideMm(film_format)` in place of
`constants.frame_mm`. The light spread, the halation, and the inhibitor then scale with the format.

**Data rules.** `35mm` gives the same plan as today, because its frame is 36 mm. A change of `film_format` sets the dirty flag
`FilmExposure`, so the halation and flare canvases are rebuilt.

**Tests.** `FilmPrintImagePlan`: at `35mm` the plan equals the current plan exactly; at `16mm` the spread sigma in pixels is 36 / 12.52
times the `35mm` value. `GpuDagCudaDrtProductTest`: a format change rebuilds the canvases once.

**Exit criteria.** The tests pass. The lab expected values at `35mm` still pass.

**Completion record.** Complete (2026-11-06), on all backends: the image plan is backend-neutral, so CUDA, OpenCL, and Metal
read the format with no kernel change.

Design decisions of this phase:

- `MakeFilmPrintImagePlan` takes the format as a parameter (`FilmFormat format`), and the three backends pass
  `drt.Params().FilmFormat()`. The format is a field of the DRT node and not of `FilmPrintParams`, so the remembered film print
  settings of #672 keep their form.
- `FilmImagePlaneConstants` no longer holds `frame_mm`. The bundle keeps `common/frame_mm` as the frame of the lab values, and
  `FilmStockBundle::ReadCommonValues` throws when it is not `FilmFrameLongSideMm(Film35mm)` (36 mm). So a `35mm` document gets the
  lab values: the arithmetic of the plan is the same as before (36.0 * 1000 / long edge), so the `35mm` plan is bit-equal.
- `ApplyFilmFormat` marks `FilmFormat` and `FilmExposure`. `FilmExposure` already invalidates the Halation, flare, loupe flare, and
  prescan results, which all read the amounts after Light spread and Halation. No new invalidation rule.
- Enlarger flare is a share of the frame (`enlarger_flare_size`), so the format does not change it.

Primary call chain:

```text
ApplyFilmFormat(format) -> DrtDirty::FilmFormat | DrtDirty::FilmExposure
  -> RuntimeInvalidationState::CollectDrtChanges -> Halation, flare, loupe flare, prescan results invalid
RecordCudaFilmPrint / OpenCL / Metal film print pass
  -> MakeFilmPrintImagePlan(geometry, constants, layer_spread_um, has_inhibitor_blur, film, drt.Params().FilmFormat())
     -> pixel_um = FilmFrameLongSideMm(format) * 1000 / long edge -> PlanSpread, PlanInhibitor, PlanHalation, PlanFlare
```

Primary failure call chain:

```text
Format outside the enum          -> FilmFrameLongSideMm throws std::invalid_argument naming film_format, also with no image plane
Bundle frame not 36 mm           -> FilmStockBundle::Open throws, naming common/frame_mm and 35mm
Blur past kFilmBlurMaxRadius     -> FillLayer throws std::runtime_error with the sigma (no substitute blur)
```

What was proven (executed tests, debug build, RTX 3080 Laptop):

| Required name / criterion | Test | Binary | Result |
| --- | --- | --- | --- |
| At `35mm` the plan equals the current plan exactly | `FilmPrintImagePlan.A24MegapixelFrameHasTheSizesOfTheLab` and the other 12 plan tests, unchanged values | `GpuDagRawInputTest` | PASS |
| At `16mm` (and `70mm`) the sigma in pixels is 36 / frame times the `35mm` value: spread, inhibitor, Halation; flare unchanged | `FilmPrintImagePlan.TheFormatScalesEachLengthOnTheFilmBy36OverItsFrame` (1e-4 relative) | `GpuDagRawInputTest` | PASS |
| A format outside the enum throws with the field name | `FilmPrintImagePlan.AFormatOutsideTheEnumThrowsWithTheFieldName` | `GpuDagRawInputTest` | PASS |
| The bundle frame is the frame of `35mm` | `FilmPrintTablesTest.FrameThatIsNotTheFrameOf35mmThrows` | `FilmPrintTablesTest` | PASS |
| A format change marks the film exposure | `DrtFilmFormatModel.FormatChangeMarksTheFilmFormatAndTheFilmExposure` | `GpuDagModelGraphTest` | PASS |
| A format change rebuilds the canvases once; a change back gives the first render bit-equal | `CudaFilmPrintFixture.FormatChangeBuildsTheCanvasesOnce` | `GpuDagCudaDrtProductTest` | PASS |
| The lab expected values at `35mm` still pass | `FilmPrintTablesTest` (all, with the lab resources) and `GpuDagCudaDrtProductTest` (all) | both | PASS |

Suite totals: `GpuDagCudaDrtProductTest` 130 / 130. `FilmPrintTablesTest` 73 / 73. `GpuDagModelGraphTest` with the filter `DrtFilm*:Drt*Model*`
35 / 35. `GpuDagRawInputTest` with the filter `FilmPrintImagePlan*:FilmGrainPlan*:FilmFormatTest*:DrtPost*:FilmGrainNegative*` 40 / 40.
`GpuDagOpenClGradeTest` 127 / 133; the 6 failures are `OpenClMultiGradeFixture.*` of issue
[#496](https://github.com/zidage/AlcedoStudio/issues/496). Metal was not built (no Mac in this phase); its change is the format
argument of `MakeFilmPrintImagePlan` in `metal_film_print_pass.mm` and `metal_film_print_test.cpp`.

Commands (PowerShell, repository root):

```text
cmd /c scripts\msvc_env.cmd --build build\debug --parallel 8 --target FilmPrintTablesTest GpuDagModelGraphTest
  GpuDagRawInputTest GpuDagCudaDrtProductTest GpuDagOpenClGradeTest
FilmPrintTablesTest.exe; GpuDagCudaDrtProductTest.exe; GpuDagOpenClGradeTest.exe
GpuDagModelGraphTest.exe --gtest_filter="DrtFilm*:Drt*Model*"
GpuDagRawInputTest.exe --gtest_filter="FilmPrintImagePlan*:FilmGrainPlan*:FilmFormatTest*:DrtPost*:FilmGrainNegative*"
```

Effect at `16mm` (6000 px frame): the wide red spread goes from a sigma of 1.18 to 3.39 pixels, the inhibitor from 5.83 to 16.8
pixels (half-size radius 34), and Halation on the 2048 canvas from 6.8 to 19.6 texels (radius 79). The tap limit
`kFilmBlurMaxRadius` (128) then holds the widest spread at slider 3 up to a long edge of about 13900 pixels at `16mm`, against about
40000 at `35mm`; a larger render at `16mm` stops with the sigma in the message (issue
[#698](https://github.com/zidage/AlcedoStudio/issues/698)).

Remaining gaps: none for G7. The user checks the CUDA result by eye before G6.

### G8: measurements, user review, and documents

**Objective and deliverables.** The timing record of all backends, the user review, and the grain section of the explanatory
document.

**Steps.**

1. Record the Release timing of G3 to G6 in this plan.
2. Render a review set from `sample_image/raw/` (no lab photos): one portrait, one landscape, and one low-light image, with 5207,
   5219, Portra 400, Ektar 100, and Velvia 50, at 16mm, 35mm, and 70mm, at fit and at 100 %. The user reviews the set.
3. Add a grain section to `film_simulation/film_simulation_explained.md`, with the parameter table and the sources.

**Exit criteria.** The user accepts the review, or the review gives a slider default and no data change.

**Completion record.** Pending.

---

## 9. Test rules for all phases

- Grain tests are statistical. They use a fixed seed, a flat or ramp input, and expected values from the evaluator of G0 or from
  the host formula. No stored pixels.
- A tolerance names the quantity: the per-pixel standard deviation, the lag-1 correlation, the channel correlation, or the pattern
  correlation.
- A GPU test that runs a QML panel fails on a QTRY timeout (#526).
- AMD OpenCL compile checks use the offline repro of the memory note when no AMD GPU is present.

---

## 10. Risks

| Risk | Effect | Response |
| --- | --- | --- |
| No absolute anchor for the PGI films | Portra, Ektar, Gold, Ultramax, and Vericolor III have one common external scale | G0: no collected sheet gives both values; the recorded fotufilm value; the user review of G8 |
| The 3200 dpi scan cannot resolve a 2 um cloud | `sigma_c` keeps the literature values | G0: the scans give an upper limit only (5.9 to 7.8 um). An 8000 dpi Flextight scan of a 5207 frame with a sharp film edge gives the scanner MTF and a fit |
| Grain in the negative moves the mean through the print curve | A small tone shift in the shadows of the print | G5 measured it (G5 completion record): at 100 % the mean light of a print shadow rises by up to 0.15 stop at 35mm and 0.20 stop at 16mm (6000 px); from the mid grey up the shift is below 0.005 stop. This is the convex print curve acting on the grain, as on a real print. `GrainMovesTheMeanOfAGreyRampOnlyInThePrintShadows` keeps the bounds |
| The halo adds memory | One more float4 image at full size | The pool releases it at the end of the pass; Metal uses half4 |

---

## 11. Parameter table

| Symbol | Meaning | Unit | Value | Source |
| --- | --- | --- | --- | --- |
| `sigma_48` | RMS granularity through the 48 um aperture | density | per stock, per layer, per density | datasheet |
| `A_48` | Area of the 48 um aperture | um^2 | 1809.6 | definition |
| `G` | Selwyn granularity | density um | `sigma_48 sqrt(A_48)` | derived |
| `p` | Pixel pitch on the film | um | frame long side / image long side | derived |
| `sigma_c` | Gaussian sigma of the dye cloud | um | 2.0 (R, G), 2.8 (B) | literature; G0 scans: upper limit 5.9 to 7.8 |
| `I` | Pixel integral of the cloud and the pixel square | 1 / um | section 3.2 | derived |
| `sigma_b` | Blur sigma in pixels | px | rule `grain_kernel` | derived |
| `rho1` | Lag-1 correlation of the pixel field, one axis | none | 0 to 1 | derived |
| `w` | Discrete blur weights | none | sum 1 | derived |
| `k` | Reference pixels summed per render pixel per axis | none | 1 to 4 | design |
| `s_jk` | Channel `j` density of a unit of dye `k` | density | per stock | bundle dye spectra |
| `sigma_a_k` | Standard deviation of the dye amount of layer `k` at 48 um | dye amount | per stock | solved in G0 |
| `R`, `L` | Correlation of the unit fields of the three layers, and its lower Cholesky factor | none | rg 0.489, gb 0.2349, rb 0.1743 | fit in G0 on the 5207 scans |
| PGI | Print Grain Index | index | per stock and magnification | Kodak datasheets |
| 34.3 | PGI units per natural log of granularity | index | `2 / ln(1.06)` | E-58, US 5629769 |
| frame | Frame long side | mm | 12.52, 36, 70 | U1 |
| strength | Film Grain slider | none | 0 to 2; 1 is the datasheet | U3 |

---

## 12. References

- Kodak, *Print Grain Index: An Assessment of Print Graininess from Color Negative Films*, publication E-58, July 2000.
  [PDF](https://filmcolors.org/wp-content/uploads/2025/12/Kodak_Print-Grain-Index_E-58.pdf)
- Kodak Gold 200 technical data, E-7022 (2022). [PDF](https://kodakprofessional.com/sites/default/files/wysiwyg/E7022-1.pdf)
- Cookingham and Kane, *Apparatus and method for the measurement of grain in images*, US 5629769.
  [Patent](https://patents.google.com/patent/US5629769)
- Kodak and Fuji datasheets in `D:\Projects\spectral_film_lut\datasheets` (Vision3 50D, 200T, 250D, 500T; EXR 5248; ECN 5247;
  Ektar 100; Portra 400; Kennel and Sehlin on 5293).
- Jarvis, dye-cloud spread in C-41 coatings, J. Photogr. Sci. 40:105 (1992) and 43:136 (1995), as cited by fotufilm
  `docs/film-grain.md`.
- Wiener spectrum and dye-cloud size of colour negative film: J. Photogr. Sci., 1985 (cloud profile), 1995 and 1996 (noise spectrum
  and dye diffusion). [1995](https://informahealthcare.com/doi/abs/10.1080/00223638.1995.11738635),
  [1996](https://informahealthcare.com/doi/abs/10.1080/00223638.1996.11737647)
- Jarzynski and Olano, *Hash Functions for GPU Rendering*, JCGT 9(3), 2020.
- Giles, *Approximating the erfinv function*, GPU Computing Gems, 2010.
- spektrafilm `model/grain.py` and `runtime/params_schema.py` (`film_format_mm`), fotufilm `FilmFormat.swift` and `FilmStock.swift`.
