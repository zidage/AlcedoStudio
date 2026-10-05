# ICC Assets

This directory contains the ICC profiles bundled for export-time embedding on platforms that do not provide the needed profiles natively.

## Runtime-mapped profiles

- `rec709_bt1886.icc`: generated with LittleCMS 2 as the Alcedo Studio Rec.709 / BT.1886 export profile approximation.
- `rec709_gamma22.icc`: generated with LittleCMS 2.
- `srgb_piecewise.icc`: Rec.709 primaries with the IEC 61966-2-1 piecewise sRGB curve (ICC parametric type 3), written by `alcedo_studio/tests/resources/raster/generate_raster_fixtures.py`. Used for the `srgb_piecewise` display EOTF.
- `p3_d65_gamma22.icc`: generated with LittleCMS 2.
- `p3_d65_pq.icc`: generated with LittleCMS 2.
- `p3_d60_gamma26.icc`: generated with LittleCMS 2.
- `p3_dci_gamma26.icc`: downloaded from `https://raw.githubusercontent.com/saucecontrol/Compact-ICC-Profiles/master/profiles/DCI-P3-v4.icc` and redistributed under CC0-1.0.
- `xyz_gamma26.icc`: generated with LittleCMS 2 for Alcedo Studio's XYZ / gamma 2.6 export path.
- `rec2020_pq.icc`: generated with LittleCMS 2.
- `rec2020_hlg.icc`: generated with LittleCMS 2.

## Upstream reference downloads

- `upstream_displayp3_compat_v4.icc`: downloaded from `https://raw.githubusercontent.com/saucecontrol/Compact-ICC-Profiles/master/profiles/DisplayP3Compat-v4.icc`
- `upstream_rec2020_v4.icc`: downloaded from `https://raw.githubusercontent.com/saucecontrol/Compact-ICC-Profiles/master/profiles/Rec2020-v4.icc`

## Notes

- The Compact ICC Profiles repository is released under CC0-1.0: [https://github.com/saucecontrol/Compact-ICC-Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles)
- Generated profiles were created locally with LittleCMS 2 using the color primaries and transfer functions required by Alcedo Studio's current export UI combinations.
- A direct download of ICC's Rec.709 reference display profile was attempted, but the registry endpoint is currently protected by Cloudflare's browser challenge in this environment. The shipped `rec709_bt1886.icc` is therefore generated locally instead of mirrored from ICC's binary download.
