// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_COLOR_ENCODING_MATH_H
#define ALCEDO_COLOR_ENCODING_MATH_H

// Transfer functions of every color encoding the app uses, in both directions, shared by the
// host, CUDA, OpenCL C and Metal (docs/roadmap/alcedo_studio/edit/lut_color_encoding_plan.md,
// section 5.1). This file is the only place a curve constant appears.
//
// Conventions:
// - Decode maps a code value to linear light; encode is its inverse.
// - Scene-referred curves decode to scene-linear reflectance (0.18 is middle grey).
// - Display curves decode to linear light relative to the curve's own maximum: 1.0 at code value
//   1.0 for SDR curves, 1.0 = 10000 nits for ST 2084 (PQ), and the scene-linear HLG signal
//   E in [0, 1] (BT.2100 inverse OETF, no OOTF) for HLG. Display curves have no light below a
//   code value of 0; their functions clamp negative input to 0.
//
// OpenCL C cannot resolve #include, so each OpenCL program lists this file before the sources
// that use it (opencl_gpu_dag_programs.cpp). Metal shaders include it by relative path.

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define CE_INLINE static inline
#if defined(CE_OPENCL_POW_EXP2_LOG2)
// x^y as exp2(y log2 x) for x >= 0, with the 3 ulp log2 and exp2 of OpenCL. The relative error
// grows with |y log2 x|: about 2e-6 for the gamma and sRGB curves (the class of the 16 ulp of pow),
// and up to about 2e-5 near the black of the ST 2084 decode. The pow of the NVIDIA OpenCL driver
// costs about 0.16 ms per call and pixel at 24 MP. A program opts in with the build option
// -DCE_OPENCL_POW_EXP2_LOG2: the DRT/Post Film Grain, whose grain changes the light by about 1 %
// (issue 693).
#define CE_POW(x, y) exp2((y) * log2(x))
#else
#define CE_POW    pow
#endif
#define CE_EXP    exp
#define CE_EXP2   exp2
#define CE_LOG    log
#define CE_LOG2   log2
#define CE_SQRT   sqrt
#elif defined(__METAL_VERSION__)
// precise:: keeps Metal fast-math from moving the curves away from the CUDA and OpenCL results.
#define CE_INLINE static inline
#if defined(CE_METAL_POW_EXP2_LOG2)
// x^y as exp2(y log2 x) for x >= 0, with the fast log2 and exp2 of Metal: the form of
// CE_OPENCL_POW_EXP2_LOG2. precise::pow costs about 2 ms per call and pixel at 24 MP on an Apple M4. A shader opts in with -DCE_METAL_POW_EXP2_LOG2 on its own AIR file: the DRT/Post Film Grain,
// whose grain changes the light by about 1 % (phase G4).
#define CE_POW(x, y) fast::exp2((y) * fast::log2(x))
#else
#define CE_POW    precise::pow
#endif
#define CE_EXP    precise::exp
#define CE_EXP2   precise::exp2
#define CE_LOG    precise::log
#define CE_LOG2   precise::log2
#define CE_SQRT   precise::sqrt
#else
#include <cmath>
#ifdef __CUDACC__
#define CE_INLINE static __host__ __device__ inline
#else
#define CE_INLINE static inline
#endif
#define CE_POW  powf
#define CE_EXP  expf
#define CE_EXP2 exp2f
#define CE_LOG  logf
#define CE_LOG2 log2f
#define CE_SQRT sqrtf
#endif

// ---------------------------------------------------------------------------------------------
// Transfer function ids. Equal to alcedo::color::TransferFunctionId; ids 0 to 7 are also equal to
// ColorUtils::EOTF, DrtEotf and CudaDrtEotf, whose values are serialized and uploaded.
// ---------------------------------------------------------------------------------------------

#define CE_TF_LINEAR               0
#define CE_TF_ST2084               1
#define CE_TF_HLG                  2
#define CE_TF_GAMMA_2_6            3
#define CE_TF_BT1886               4
#define CE_TF_GAMMA_2_2            5
#define CE_TF_GAMMA_1_8            6
#define CE_TF_SRGB                 7
#define CE_TF_ACESCC               8
#define CE_TF_ACESCCT              9
#define CE_TF_ARRI_LOGC3_EI800     10
#define CE_TF_ARRI_LOGC4           11
#define CE_TF_SONY_SLOG3           12
#define CE_TF_FUJIFILM_FLOG        13
#define CE_TF_FUJIFILM_FLOG2       14
#define CE_TF_PANASONIC_VLOG       15
#define CE_TF_CANON_CLOG2          16
#define CE_TF_CANON_CLOG3          17
#define CE_TF_RED_LOG3G10          18
#define CE_TF_BLACKMAGIC_FILM_GEN5 19
#define CE_TF_DAVINCI_INTERMEDIATE 20
#define CE_TF_APPLE_LOG            21
#define CE_TF_NIKON_NLOG           22
#define CE_TF_DJI_DLOG             23
#define CE_TF_CANON_CLOG           24
#define CE_TF_CINEON               25
#define CE_TF_ARRI_LOGC3_EI160     26
#define CE_TF_ARRI_LOGC3_EI200     27
#define CE_TF_ARRI_LOGC3_EI250     28
#define CE_TF_ARRI_LOGC3_EI320     29
#define CE_TF_ARRI_LOGC3_EI400     30
#define CE_TF_ARRI_LOGC3_EI500     31
#define CE_TF_ARRI_LOGC3_EI640     32
#define CE_TF_ARRI_LOGC3_EI1000    33
#define CE_TF_ARRI_LOGC3_EI1280    34
#define CE_TF_ARRI_LOGC3_EI1600    35
#define CE_TF_LEICA_LLOG           36
#define CE_TF_KINEFINITY_KINELOG3  37
#define CE_TF_GOPRO_GPLOG2         38
#define CE_TF_COUNT                39

// ---------------------------------------------------------------------------------------------
// exp(u) - 1 and log(1 + v) without cancellation (W. Kahan's compensated forms: the rounding of
// exp(u) or 1 + v is divided out again).
// ---------------------------------------------------------------------------------------------

CE_INLINE float CeExpm1(float u) {
  const float e = CE_EXP(u);
  if (e == 1.0f) return u;
  const float em1 = e - 1.0f;
  if (em1 == -1.0f) return -1.0f;
  return em1 * u / CE_LOG(e);
}

CE_INLINE float CeLog1p(float v) {
  const float w = 1.0f + v;
  if (w == 1.0f) return v;
  return CE_LOG(w) * v / (w - 1.0f);
}

// ---------------------------------------------------------------------------------------------
// ACEScc (Academy S-2014-003) with Alcedo's linear extension below the encoding floor: a value
// below 0 encodes as floor + value, so the working space keeps negative light through a grade.
// ---------------------------------------------------------------------------------------------

#define CE_ACESCC_A                 9.72f
#define CE_ACESCC_B                 17.52f
/// ACEScc code value of 0.18 (middle grey), CeAcesccEncode(0.18f).
#define CE_ACESCC_MIDDLE_GREY       0.41358840f
/// Code values per stop of scene light.
#define CE_ACESCC_CODE_PER_STOP     (1.0f / CE_ACESCC_B)
#define CE_ACESCC_DENORM_OFFSET     0.0000152587890625f
#define CE_ACESCC_DENORM_TRANSITION 0.000030517578125f
#define CE_ACESCC_FLOOR             ((-16.0f + CE_ACESCC_A) / CE_ACESCC_B)
#define CE_ACESCC_DENORM_THRESHOLD  ((-15.0f + CE_ACESCC_A) / CE_ACESCC_B)

CE_INLINE float CeAcesccEncode(float value) {
  if (value < 0.0f) return CE_ACESCC_FLOOR + value;
  if (value < CE_ACESCC_DENORM_TRANSITION) {
    return (CE_LOG2(CE_ACESCC_DENORM_OFFSET + value * 0.5f) + CE_ACESCC_A) / CE_ACESCC_B;
  }
  return (CE_LOG2(value) + CE_ACESCC_A) / CE_ACESCC_B;
}

CE_INLINE float CeAcesccDecode(float value) {
  if (value < CE_ACESCC_FLOOR) return value - CE_ACESCC_FLOOR;
  if (value <= CE_ACESCC_DENORM_THRESHOLD) {
    return (CE_EXP2(value * CE_ACESCC_B - CE_ACESCC_A) - CE_ACESCC_DENORM_OFFSET) * 2.0f;
  }
  return CE_EXP2(value * CE_ACESCC_B - CE_ACESCC_A);
}

// ---------------------------------------------------------------------------------------------
// Camera log curves in the OpenColorIO LogCameraTransform form:
//   encode: x >= lin_break ? k * log2(lin_slope * x + lin_offset) + log_offset
//                          : linear_slope * x + linear_offset
//   decode: y >= log_break ? (exp2((y - log_offset) / k) - lin_offset) / lin_slope
//                          : (y - linear_offset) / linear_slope
// k is the log-side slope divided by log2(base). Where a vendor gives no linear segment, the
// segment is the tangent at lin_break (OCIO's construction); the derived values are written out.
// ---------------------------------------------------------------------------------------------

CE_INLINE float CeLogCameraEncode(float x, float k, float log_offset, float lin_slope,
                                  float lin_offset, float lin_break, float linear_slope,
                                  float linear_offset) {
  if (x >= lin_break) return k * CE_LOG2(lin_slope * x + lin_offset) + log_offset;
  return linear_slope * x + linear_offset;
}

/// The decode takes reciprocals (1 / k, 1 / lin_slope, 1 / linear_slope) and multiplies: OpenCL C
/// does not round single-precision division correctly by default; multiplication is exact.
CE_INLINE float CeLogCameraDecode(float y, float inv_k, float log_offset, float inv_lin_slope,
                                  float lin_offset, float log_break, float inv_linear_slope,
                                  float linear_offset) {
  if (y >= log_break) return (CE_EXP2((y - log_offset) * inv_k) - lin_offset) * inv_lin_slope;
  return (y - linear_offset) * inv_linear_slope;
}

// ACEScct (Academy S-2016-001). Log segment above 0.0078125: (log2(x) + 9.72) / 17.52.
#define CE_ACESCCT_ARGS_ENC                                                           \
  0.0570776255707762f, 0.554794520547945f, 1.0f, 0.0f, 0.0078125f, 10.5402377416545f, \
      0.0729055341958355f
#define CE_ACESCCT_ARGS_DEC                                                        \
  17.52f, 0.554794520547945f, 1.0f, 0.0f, 0.155251141552511f, 0.0948745203391427f, \
      0.0729055341958355f

// ARRI LogC3 at EI 800 ("ALEXA Log C Curve - Usage in VFX", ARRI 2017), with the parameters of
// the OCIO 2.5.1 builtin ARRI_ALEXA-LOGC-EI800-AWG_to_ACES2065-1.
#define CE_ARRI_LOGC3_ARGS_ENC                                                     \
  0.0744114957512506f, 0.385536998692443f, 5.55555555555556f, 0.0522722750251688f, \
      0.0105909904954696f, 5.36765479527298f, 0.0928093569961373f
#define CE_ARRI_LOGC3_ARGS_DEC                                                           \
  13.4387837511409f, 0.385536998692443f, 0.18f, 0.0522722750251688f, 0.149658137915835f, \
      0.186301101345163f, 0.0928093569961373f

// ARRI LogC3 at the other exposure indices: b, c and d of the "Linear Scene Exposure Factor"
// table for SUP 3.x in the same ARRI document (EI 160 to 1600; the document has no formula
// above EI 1600). a is 1 / 0.18 for every EI. As for EI 800, cut is where a * x + b = 1 / 9 and
// the linear segment is the tangent at cut; it matches the table's e within 1.1e-5 and its f
// within 1e-6.
#define CE_ARRI_LOGC3_EI160_ARGS_ENC                                                            \
  0.0809879059134549f, 0.381991f, 5.55555555555556f, 0.080216f, 0.00556112f, 5.84204251166616f, \
      0.0927771127780049f
#define CE_ARRI_LOGC3_EI160_ARGS_DEC                                                      \
  12.3475226173723f, 0.381991f, 0.18f, 0.080216f, 0.125265412230482f, 0.171173009782635f, \
      0.0927771127780049f
#define CE_ARRI_LOGC3_EI200_ARGS_ENC                                                            \
  0.0800760860565886f, 0.382478f, 5.55555555555556f, 0.076621f, 0.00620822f, 5.77626861238191f, \
      0.0927824664668119f
#define CE_ARRI_LOGC3_EI200_ARGS_DEC                                                      \
  12.4881228497271f, 0.382478f, 0.18f, 0.076621f, 0.128642812791574f, 0.173122142875492f, \
      0.0927824664668119f
#define CE_ARRI_LOGC3_EI250_ARGS_ENC                                                            \
  0.0791642661997224f, 0.382966f, 5.55555555555556f, 0.072941f, 0.00687062f, 5.71049471309767f, \
      0.0927865741669621f
#define CE_ARRI_LOGC3_EI250_ARGS_DEC                                                      \
  12.6319619697745f, 0.382966f, 0.18f, 0.072941f, 0.132021213352665f, 0.175116176485793f, \
      0.0927865741669621f
#define CE_ARRI_LOGC3_EI320_ARGS_ENC                                                            \
  0.0781555146842524f, 0.383508f, 5.55555555555556f, 0.068768f, 0.00762176f, 5.63772867265478f, \
      0.0927914651137031f
#define CE_ARRI_LOGC3_EI320_ARGS_DEC                                                      \
  12.7950024261243f, 0.383508f, 0.18f, 0.068768f, 0.135760880001796f, 0.177376397138514f, \
      0.0927914651137031f
#define CE_ARRI_LOGC3_EI400_ARGS_ENC                                                            \
  0.0772436948273862f, 0.383999f, 5.55555555555556f, 0.064901f, 0.00831782f, 5.57195477337053f, \
      0.0927957637098513f
#define CE_ARRI_LOGC3_EI400_ARGS_DEC                                                      \
  12.9460404792218f, 0.383999f, 0.18f, 0.064901f, 0.139142280562888f, 0.179470229151751f, \
      0.0927957637098513f
#define CE_ARRI_LOGC3_EI500_ARGS_ENC                                                          \
  0.07633187497052f, 0.384493f, 5.55555555555556f, 0.060939f, 0.00903098f, 5.50618087408628f, \
      0.0928004717737241f
#define CE_ARRI_LOGC3_EI500_ARGS_DEC                                                     \
  13.1006869723324f, 0.384493f, 0.18f, 0.060939f, 0.14252668112398f, 0.181614084765413f, \
      0.0928004717737241f
#define CE_ARRI_LOGC3_EI640_ARGS_ENC                                                           \
  0.0753234244850457f, 0.38504f, 5.55555555555556f, 0.056443f, 0.00984026f, 5.43343654836749f, \
      0.0928039652011629f
#define CE_ARRI_LOGC3_EI640_ARGS_DEC                                                    \
  13.2760825312521f, 0.38504f, 0.18f, 0.056443f, 0.146270393530602f, 0.18404558350837f, \
      0.0928039652011629f
#define CE_ARRI_LOGC3_EI1000_ARGS_ENC                                                           \
  0.0734997847713133f, 0.386036f, 5.55555555555556f, 0.047996f, 0.01136072f, 5.30188874979899f, \
      0.0928139210951686f
#define CE_ARRI_LOGC3_EI1000_ARGS_DEC                                                     \
  13.6054820175514f, 0.386036f, 0.18f, 0.047996f, 0.153047194652785f, 0.188612030012495f, \
      0.0928139210951686f
#define CE_ARRI_LOGC3_EI1280_ARGS_ENC                                                         \
  0.0724910332558433f, 0.38659f, 5.55555555555556f, 0.043137f, 0.01223534f, 5.2291227093561f, \
      0.0928187670512231f
#define CE_ARRI_LOGC3_EI1280_ARGS_DEC                                                    \
  13.7948095796992f, 0.38659f, 0.18f, 0.043137f, 0.156798861301916f, 0.191236667330596f, \
      0.0928187670512231f
#define CE_ARRI_LOGC3_EI1600_ARGS_ENC                                                          \
  0.0715792133989771f, 0.387093f, 5.55555555555556f, 0.038625f, 0.0130475f, 5.16334881007186f, \
      0.0928234682635954f
#define CE_ARRI_LOGC3_EI1600_ARGS_DEC                                                    \
  13.970536312352f, 0.387093f, 0.18f, 0.038625f, 0.160192261863008f, 0.193672757116342f, \
      0.0928234682635954f

// ARRI LogC4 ("ARRI LogC4 Logarithmic Color Space Specification", ARRI 2022), OCIO 2.5.1
// builtin ARRI_LOGC4_to_ACES2065-1 parameters (base 2).
#define CE_ARRI_LOGC4_ARGS_ENC                                                              \
  0.0647954196341293f, -0.295908392682586f, 2231.82630906769f, 64.0f, -0.0180569961199113f, \
      8.80303321033176f, 0.158956336522411f
#define CE_ARRI_LOGC4_ARGS_DEC                                                                   \
  15.4331896551724f, -0.295908392682586f, 0.00044806354147592f, 64.0f, 0.0f, 0.113597208610589f, \
      0.158956336522411f

// Sony S-Log3 ("Technical Summary for S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3", Sony 2016):
// (420 + log10((x + 0.01) / 0.19) * 261.5) / 1023 above 0.01125, OCIO 2.5.1 parameters.
#define CE_SONY_SLOG3_ARGS_ENC                                                              \
  0.076949505245485f, 0.410557184750733f, 5.26315789473684f, 0.0526315789473684f, 0.01125f, \
      6.62194371177582f, 0.0928641251221508f
#define CE_SONY_SLOG3_ARGS_DEC                                                           \
  12.9955351474943f, 0.410557184750733f, 0.19f, 0.0526315789473684f, 0.167360991879629f, \
      0.151013062557705f, 0.0928641251221508f

// Fujifilm F-Log ("F-Log Data Sheet", Fujifilm, Ver.1.2): c * log10(a * x + b) + d above cut1,
// e * x + f below; the decode switches at cut2. a 0.555556, b 0.009468, c 0.344676,
// d 0.790453, e 8.735631, f 0.092864, cut1 0.00089, cut2 0.100537775223865.
#define CE_FUJIFILM_FLOG_ARGS_ENC \
  0.103757814785478f, 0.790453f, 0.555556f, 0.009468f, 0.00089f, 8.735631f, 0.092864f
#define CE_FUJIFILM_FLOG_ARGS_DEC                                                 \
  9.63782826447845f, 0.790453f, 1.79999856000115f, 0.009468f, 0.100537775223865f, \
      0.114473699724725f, 0.092864f

// Fujifilm F-Log2 ("F-Log2 Data Sheet", Fujifilm, Ver.1.1): same form, a 5.555556,
// b 0.064829, c 0.245281, d 0.384316, e 8.799461, f 0.092864, cut1 0.000889,
// cut2 0.100686685370811.
#define CE_FUJIFILM_FLOG2_ARGS_ENC \
  0.073836938366457f, 0.384316f, 5.555556f, 0.064829f, 0.000889f, 8.799461f, 0.092864f
#define CE_FUJIFILM_FLOG2_ARGS_DEC                                                 \
  13.5433567821697f, 0.384316f, 0.179999985600001f, 0.064829f, 0.100686685370811f, \
      0.113643324289976f, 0.092864f

// Panasonic V-Log ("V-Log/V-Gamut Reference Manual", Panasonic 2014): 0.241514 *
// log10(x + 0.00873) + 0.598206 above 0.01. The linear segment is OCIO 2.5.1's tangent; the
// manual's rounded 5.6 * x + 0.125 agrees within 2e-6 on code values.
#define CE_PANASONIC_VLOG_ARGS_ENC \
  0.0727029583727908f, 0.598206f, 1.0f, 0.00873f, 0.01f, 5.60001054470806f, 0.124999583317922f
#define CE_PANASONIC_VLOG_ARGS_DEC                                                      \
  13.7545984700157f, 0.598206f, 1.0f, 0.00873f, 0.180999688765003f, 0.178571092324993f, \
      0.124999583317922f

// RED Log3G10 version 2 ("White Paper on REDWideGamutRGB and Log3G10", RED 2017):
// 0.224282 * log10((x + 0.01) * 155.975327 + 1) above -0.01. Linear segment: OCIO 2.5.1's
// tangent (the paper rounds its slope to 15.1927).
#define CE_RED_LOG3G10_ARGS_ENC \
  0.067515609487509f, 0.0f, 155.975327f, 2.55975327f, -0.01f, 15.1926885988506f, 0.151926885988506f
#define CE_RED_LOG3G10_ARGS_DEC                                                        \
  14.8113896562692f, 0.0f, 0.00641127041842971f, 2.55975327f, 0.0f, 0.06582113452096f, \
      0.151926885988506f

// Blackmagic Film Generation 5 ("Blackmagic Generation 5 Color Science", Blackmagic Design
// 2021): A * ln(x + B) + C above 0.005, D * x + E below; OCIO 2.5.1 parameters.
#define CE_BLACKMAGIC_FILM_GEN5_ARGS_ENC                                                          \
  0.0602544253575227f, 0.530013339229194f, 1.0f, 0.00549407243225781f, 0.005f, 8.28360593240249f, \
      0.0924657534246581f
#define CE_BLACKMAGIC_FILM_GEN5_ARGS_DEC                                                 \
  16.5962913772134f, 0.530013339229194f, 1.0f, 0.00549407243225781f, 0.133883783086671f, \
      0.120720373248124f, 0.0924657534246581f

// DaVinci Intermediate ("DaVinci Resolve 17 Wide Gamut Intermediate", Blackmagic Design 2020):
// (log2(x + 0.0075) + 7) * 0.07329248 above 0.00262409, x * 10.44426855 below.
#define CE_DAVINCI_INTERMEDIATE_ARGS_ENC \
  0.07329248f, 0.51304736f, 1.0f, 0.0075f, 0.00262409f, 10.44426855f, -3.43560336e-10f
#define CE_DAVINCI_INTERMEDIATE_ARGS_DEC                                                   \
  13.6439645649867f, 0.51304736f, 1.0f, 0.0075f, 0.0274067003158092f, 0.0957462933103152f, \
      -3.43560336e-10f

// DJI D-Log ("White Paper on D-Log and D-Gamut of DJI Cinema Color System", DJI 2017):
// 0.256663 * log10(0.9892 * x + 0.0108) + 0.584555 above 0.0078, OCIO 2.5.1 parameters.
#define CE_DJI_DLOG_ARGS_ENC                                                                   \
  0.0772632529629124f, 0.58455504907396f, 0.9892f, 0.0108f, 0.00758078675f, 6.02568345853766f, \
      0.0929045490709614f
#define CE_DJI_DLOG_ARGS_DEC                                                            \
  12.9427633661764f, 0.58455504907396f, 1.01091791346543f, 0.0108f, 0.138583970393138f, \
      0.165956278135242f, 0.0929045490709614f

// Kodak Cineon printing density ("Grayscale Transformations of Cineon Digital Film Data", Kodak
// v1.1: 0.002 density per 10-bit code value, negative gamma 0.6), with black at code 95 and
// linear 1.0 at code 685 as in the OCIO 2.5.1 builtin RED_REDLOGFILM-RWG_to_ACES2065-1:
// (685 + 300 * log10(x * (1 - b) + b)) / 1023, b = 10^((95 - 685) / 300). The curve has no
// linear segment; below code value 0 it continues on its tangent there.
#define CE_CINEON_ARGS_ENC                                                          \
  0.0882785911038068f, 0.669599217986315f, 0.989202248376723f, 0.0107977516232771f, \
      -0.00565081943945688f, 24.1906958133749f, 0.136697254156207f
#define CE_CINEON_ARGS_DEC                                                             \
  11.3277748035659f, 0.669599217986315f, 1.01091561573075f, 0.0107977516232771f, 0.0f, \
      0.041338207371741f, 0.136697254156207f

// Leica L-Log ("L-Log Reference Manual" V1.9, Leica Camera 2026, sections 4.1 and 4.2):
// 0.27 * log10(1.3 * x + 0.0115) + 0.6 above 0.006, 8 * x + 0.09 below; the decode switches
// at 0.138.
#define CE_LEICA_LLOG_ARGS_ENC 0.0812780988292749f, 0.6f, 1.3f, 0.0115f, 0.006f, 8.0f, 0.09f
#define CE_LEICA_LLOG_ARGS_DEC \
  12.3034373884717f, 0.6f, 0.769230769230769f, 0.0115f, 0.138f, 0.125f, 0.09f

// Kinefinity KineLOG3 ("KineLOG3 Technical Specifications", Kinefinity 2025):
// log10(a * x + 1) * b * c + d above cut, (x - cut) / s below; the decode switches at 0.
// a 66.64, b 0.296, c 0.907136, d 0.092864, cut -0.008239, s 0.017178.
#define CE_KINEFINITY_KINELOG3_ARGS_ENC \
  0.0808302432594058f, 0.092864f, 66.64f, 1.0f, -0.008239f, 58.2139946443125f, 0.479625101874491f
#define CE_KINEFINITY_KINELOG3_ARGS_DEC \
  12.3716069589291f, 0.092864f, 0.0150060024009604f, 1.0f, 0.0f, 0.017178f, 0.479625101874491f

// ---------------------------------------------------------------------------------------------
// Camera curves with their own segment forms
// ---------------------------------------------------------------------------------------------

// Canon Log 2 and Canon Log 3 (Canon "Canon Log Gamma Curves" and the Academy CSC
// CSC.Canon.CLog2/CLog3_CGamut_to_ACES), as in the OCIO 2.5.1 builtin curves. The 0.9 factor
// maps Canon's 90% reflectance white to 0.9 linear.
#define CE_CANON_CLOG2_SLOPE  0.24136077f
#define CE_CANON_CLOG2_OFFSET 0.092864125f
#define CE_CANON_CLOG2_GAIN   87.09937546f

CE_INLINE float CeCanonClog2Encode(float x) {
  const float v = x / 0.9f;
  if (v < 0.0f) {
    return -CE_CANON_CLOG2_SLOPE * (CE_LOG2(1.0f - CE_CANON_CLOG2_GAIN * v) / CE_LOG2(10.0f)) +
           CE_CANON_CLOG2_OFFSET;
  }
  return CE_CANON_CLOG2_SLOPE * (CE_LOG2(CE_CANON_CLOG2_GAIN * v + 1.0f) / CE_LOG2(10.0f)) +
         CE_CANON_CLOG2_OFFSET;
}

CE_INLINE float CeCanonClog2Decode(float y) {
  float v;
  if (y < CE_CANON_CLOG2_OFFSET) {
    v = -(CE_POW(10.0f, (CE_CANON_CLOG2_OFFSET - y) / CE_CANON_CLOG2_SLOPE) - 1.0f) /
        CE_CANON_CLOG2_GAIN;
  } else {
    v = (CE_POW(10.0f, (y - CE_CANON_CLOG2_OFFSET) / CE_CANON_CLOG2_SLOPE) - 1.0f) /
        CE_CANON_CLOG2_GAIN;
  }
  return v * 0.9f;
}

#define CE_CANON_CLOG3_SLOPE 0.36726845f
#define CE_CANON_CLOG3_GAIN  14.98325f

CE_INLINE float CeCanonClog3Encode(float x) {
  const float v = x / 0.9f;
  if (v < -0.014f) {
    return -CE_CANON_CLOG3_SLOPE * (CE_LOG2(1.0f - CE_CANON_CLOG3_GAIN * v) / CE_LOG2(10.0f)) +
           0.12783901f;
  }
  if (v <= 0.014f) return 1.9754798f * v + 0.12512219f;
  return CE_CANON_CLOG3_SLOPE * (CE_LOG2(CE_CANON_CLOG3_GAIN * v + 1.0f) / CE_LOG2(10.0f)) +
         0.12240537f;
}

CE_INLINE float CeCanonClog3Decode(float y) {
  float v;
  if (y < 0.097465473f) {
    v = -(CE_POW(10.0f, (0.12783901f - y) / CE_CANON_CLOG3_SLOPE) - 1.0f) / CE_CANON_CLOG3_GAIN;
  } else if (y <= 0.15277891f) {
    v = (y - 0.12512219f) / 1.9754798f;
  } else {
    v = (CE_POW(10.0f, (y - 0.12240537f) / CE_CANON_CLOG3_SLOPE) - 1.0f) / CE_CANON_CLOG3_GAIN;
  }
  return v * 0.9f;
}

// Canon Log ("Canon-Log Transfer Characteristic", L. Thorpe, Canon U.S.A. 2012-06-20):
// 0.529136 * log10(10.1596 * x + 1) + 0.0730597 with x relative to 90% reflectance white, in
// legal-range code values; here as full-range 10-bit code values (64 + 876 * y) / 1023. Below 0
// the curve is mirrored about its black level, like Canon Log 2 and Canon's input transforms.
#define CE_CANON_CLOG_SLOPE  0.453101794721408f
#define CE_CANON_CLOG_OFFSET 0.125122480156403f
#define CE_CANON_CLOG_GAIN   10.1596f

CE_INLINE float CeCanonClogEncode(float x) {
  const float v = x / 0.9f;
  if (v < 0.0f) {
    return -CE_CANON_CLOG_SLOPE * (CE_LOG2(1.0f - CE_CANON_CLOG_GAIN * v) / CE_LOG2(10.0f)) +
           CE_CANON_CLOG_OFFSET;
  }
  return CE_CANON_CLOG_SLOPE * (CE_LOG2(CE_CANON_CLOG_GAIN * v + 1.0f) / CE_LOG2(10.0f)) +
         CE_CANON_CLOG_OFFSET;
}

CE_INLINE float CeCanonClogDecode(float y) {
  float v;
  if (y < CE_CANON_CLOG_OFFSET) {
    v = -(CE_POW(10.0f, (CE_CANON_CLOG_OFFSET - y) / CE_CANON_CLOG_SLOPE) - 1.0f) /
        CE_CANON_CLOG_GAIN;
  } else {
    v = (CE_POW(10.0f, (y - CE_CANON_CLOG_OFFSET) / CE_CANON_CLOG_SLOPE) - 1.0f) /
        CE_CANON_CLOG_GAIN;
  }
  return v * 0.9f;
}

// GoPro GP-Log2 (GoPro Labs "GP-Log2"): log(599 * L + 1) / log(600), mirrored about 0 below 0.
// L is 0 at black and 1 at clipped white, and 18% grey is L = 0.0517, so scene reflectance x is
// L * 0.18 / 0.0517.
#define CE_GOPRO_GPLOG2_SCALE   0.287222222222222f
#define CE_GOPRO_GPLOG2_LN_BASE 6.39692965521615f

CE_INLINE float CeGoproGplog2Encode(float x) {
  const float l = x * CE_GOPRO_GPLOG2_SCALE;
  const float m = CeLog1p(599.0f * (l < 0.0f ? -l : l)) * (1.0f / CE_GOPRO_GPLOG2_LN_BASE);
  return l < 0.0f ? -m : m;
}

CE_INLINE float CeGoproGplog2Decode(float y) {
  const float l = CeExpm1((y < 0.0f ? -y : y) * CE_GOPRO_GPLOG2_LN_BASE) * (1.0f / 599.0f);
  return (y < 0.0f ? -l : l) * (1.0f / CE_GOPRO_GPLOG2_SCALE);
}

// Apple Log ("Apple Log Profile White Paper", Apple 2023), as in the OCIO 2.5.1 builtin
// APPLE_LOG_to_ACES2065-1: c * (x - R0)^2 between R0 and Rt, gamma * log2(x + beta) + delta above.
#define CE_APPLE_LOG_R0    -0.05641088f
#define CE_APPLE_LOG_RT    0.01f
#define CE_APPLE_LOG_C     47.28711236f
#define CE_APPLE_LOG_BETA  0.00964052f
#define CE_APPLE_LOG_GAMMA 0.08550479f
#define CE_APPLE_LOG_DELTA 0.69336945f

CE_INLINE float CeAppleLogEncode(float x) {
  if (x >= CE_APPLE_LOG_RT) {
    return CE_APPLE_LOG_GAMMA * CE_LOG2(x + CE_APPLE_LOG_BETA) + CE_APPLE_LOG_DELTA;
  }
  if (x >= CE_APPLE_LOG_R0) {
    const float d = x - CE_APPLE_LOG_R0;
    return CE_APPLE_LOG_C * d * d;
  }
  return 0.0f;
}

CE_INLINE float CeAppleLogDecode(float y) {
  const float d_t = CE_APPLE_LOG_RT - CE_APPLE_LOG_R0;
  if (y >= CE_APPLE_LOG_C * d_t * d_t) {
    return CE_EXP2((y - CE_APPLE_LOG_DELTA) / CE_APPLE_LOG_GAMMA) - CE_APPLE_LOG_BETA;
  }
  if (y < 0.0f) return CE_APPLE_LOG_R0;
  return CE_SQRT(y / CE_APPLE_LOG_C) + CE_APPLE_LOG_R0;
}

// Nikon N-Log ("N-Log Specification Document" Version 1.0.0, Nikon 2018), in 10-bit code
// values x: 650 * (y + 0.0075)^(1/3) below y 0.328, 150 * ln(y) + 619 above; the decode
// switches at x 452. Code values here are x / 1023.
CE_INLINE float CeNikonNlogEncode(float y) {
  if (y < 0.328f) {
    const float v    = y + 0.0075f;
    const float cube = v < 0.0f ? -CE_POW(-v, 1.0f / 3.0f) : CE_POW(v, 1.0f / 3.0f);
    return (650.0f / 1023.0f) * cube;
  }
  return (150.0f / 1023.0f) * CE_LOG(y) + 619.0f / 1023.0f;
}

CE_INLINE float CeNikonNlogDecode(float x) {
  if (x < 452.0f / 1023.0f) {
    const float v = x * (1023.0f / 650.0f);
    return v * v * v - 0.0075f;
  }
  return CE_EXP((x - 619.0f / 1023.0f) * (1023.0f / 150.0f));
}

// ---------------------------------------------------------------------------------------------
// Display curves
// ---------------------------------------------------------------------------------------------

// Pure power curves. Adobe RGB (1998) section 4.3.4.2: exponent 2 + 51/256.
#define CE_GAMMA_2_6_EXPONENT 2.6f
#define CE_GAMMA_2_2_EXPONENT 2.2f
#define CE_GAMMA_1_8_EXPONENT 1.8f
#define CE_ADOBE_RGB_GAMMA    (563.0f / 256.0f)

/// x^(1/gamma) for x > 0, else 0.
CE_INLINE float CeGammaEncode(float x, float gamma) {
  return x > 0.0f ? CE_POW(x, 1.0f / gamma) : 0.0f;
}

/// x^gamma for x > 0, else 0.
CE_INLINE float CeGammaDecode(float x, float gamma) { return x > 0.0f ? CE_POW(x, gamma) : 0.0f; }

// sRGB, IEC 61966-2-1: 12.92 * x below 0.0031308, 1.055 * x^(1/2.4) - 0.055 above; the decode
// switches at 0.04045. (OCIO's ExponentWithLinearTransform with gamma 2.4 and offset 0.055 derives
// the break point and slope instead; its encode differs by at most 1e-5.)
CE_INLINE float CeSrgbEncode(float x) {
  if (x <= 0.0f) return 0.0f;
  return x <= 0.0031308f ? 12.92f * x : 1.055f * CE_POW(x, 1.0f / 2.4f) - 0.055f;
}

CE_INLINE float CeSrgbDecode(float x) {
  if (x <= 0.0f) return 0.0f;
  return x <= 0.04045f ? x / 12.92f : CE_POW((x + 0.055f) / 1.055f, 2.4f);
}

// ITU-R BT.1886 with a zero black level and white 1.0: the gamma 2.4 power in both directions.
#define CE_BT1886_GAMMA 2.4f

// SMPTE ST 2084 (PQ). Linear 1.0 = 10000 nits.
#define CE_PQ_M1        0.1593017578125f
#define CE_PQ_M2        78.84375f
#define CE_PQ_C1        0.8359375f
#define CE_PQ_C2        18.8515625f
#define CE_PQ_C3        18.6875f
#define CE_PQ_PEAK_NITS 10000.0f

// The PQ functions are written without cancellation near the 10000-nit peak, where the textbook
// form loses about 5e-5 relative in single precision. They use c2 - c3 = 1 - c1 (exact).

/// Code value of linear @p x: 1 - N = (1 - c1) * (1 - x^m1) / (1 + c3 * x^m1).
CE_INLINE float CePqEncode(float x) {
  const float lm = x > 0.0f ? CE_POW(x, CE_PQ_M1) : 0.0f;
  const float w  = x > 0.0f ? -CeExpm1(CE_PQ_M1 * CE_LOG(x)) : 1.0f;
  const float r  = (1.0f - CE_PQ_C1) * w / (1.0f + CE_PQ_C3 * lm);
  return CE_EXP(CE_PQ_M2 * CeLog1p(-r));
}

/// Linear of code value @p x, with q = 1 - x^(1/m2): p - c1 = (1 - c1) - q and
/// c2 - c3 * p = (1 - c1) + c3 * q.
CE_INLINE float CePqDecode(float x) {
  if (x <= 0.0f) return 0.0f;
  const float q   = -CeExpm1(CE_LOG(x) / CE_PQ_M2);
  const float num = (1.0f - CE_PQ_C1) - q;
  if (num <= 0.0f) return 0.0f;
  return CE_POW(num / ((1.0f - CE_PQ_C1) + CE_PQ_C3 * q), 1.0f / CE_PQ_M1);
}

// ITU-R BT.2100 HLG. The OETF maps scene light E in [0, 1] to the signal; the OOTF of the
// reference display is a luminance gain, applied by the callers that need display light.
#define CE_HLG_A                  0.17883277f
#define CE_HLG_B                  0.28466892f
#define CE_HLG_C                  0.55991073f
#define CE_HLG_OOTF_GAMMA         1.2f
/// Luminance exponent of the OOTF gain, gamma - 1, written exactly.
#define CE_HLG_OOTF_GAIN_EXPONENT 0.2f
#define CE_BT2100_LUMA_R          0.2627f
#define CE_BT2100_LUMA_G          0.6780f
#define CE_BT2100_LUMA_B          0.0593f

CE_INLINE float CeHlgEncode(float e) {
  if (e <= 0.0f) return 0.0f;
  return e <= (1.0f / 12.0f) ? CE_SQRT(3.0f * e)
                             : CE_HLG_A * CE_LOG(12.0f * e - CE_HLG_B) + CE_HLG_C;
}

CE_INLINE float CeHlgDecode(float v) {
  if (v <= 0.0f) return 0.0f;
  return v <= 0.5f ? v * v / 3.0f : (CE_EXP((v - CE_HLG_C) / CE_HLG_A) + CE_HLG_B) / 12.0f;
}

/// HLG OOTF gain for scene luminance @p y_scene (BT.2100 luma of the inverse OETF signal).
CE_INLINE float CeHlgOotfGain(float y_scene) {
  return y_scene > 0.0f ? CE_POW(y_scene, CE_HLG_OOTF_GAIN_EXPONENT) : 0.0f;
}

/// Inverse HLG OOTF gain for display luminance @p y_display (1.0 = display peak). 1 at 0.
CE_INLINE float CeHlgInverseOotfGain(float y_display) {
  return y_display > 0.0f ? CE_POW(y_display, (1.0f - CE_HLG_OOTF_GAMMA) / CE_HLG_OOTF_GAMMA)
                          : 1.0f;
}

// ---------------------------------------------------------------------------------------------
// Dispatch by transfer function id
// ---------------------------------------------------------------------------------------------

/// Linear light to code value for transfer @p tf (CE_TF_*). An unknown id returns @p x.
CE_INLINE float CeEncode(int tf, float x) {
  switch (tf) {
    case CE_TF_LINEAR:
      return x;
    case CE_TF_ST2084:
      return CePqEncode(x);
    case CE_TF_HLG:
      return CeHlgEncode(x);
    case CE_TF_GAMMA_2_6:
      return CeGammaEncode(x, CE_GAMMA_2_6_EXPONENT);
    case CE_TF_BT1886:
      return CeGammaEncode(x, CE_BT1886_GAMMA);
    case CE_TF_GAMMA_2_2:
      return CeGammaEncode(x, CE_GAMMA_2_2_EXPONENT);
    case CE_TF_GAMMA_1_8:
      return CeGammaEncode(x, CE_GAMMA_1_8_EXPONENT);
    case CE_TF_SRGB:
      return CeSrgbEncode(x);
    case CE_TF_ACESCC:
      return CeAcesccEncode(x);
    case CE_TF_ACESCCT:
      return CeLogCameraEncode(x, CE_ACESCCT_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI800:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_ARGS_ENC);
    case CE_TF_ARRI_LOGC4:
      return CeLogCameraEncode(x, CE_ARRI_LOGC4_ARGS_ENC);
    case CE_TF_SONY_SLOG3:
      return CeLogCameraEncode(x, CE_SONY_SLOG3_ARGS_ENC);
    case CE_TF_FUJIFILM_FLOG:
      return CeLogCameraEncode(x, CE_FUJIFILM_FLOG_ARGS_ENC);
    case CE_TF_FUJIFILM_FLOG2:
      return CeLogCameraEncode(x, CE_FUJIFILM_FLOG2_ARGS_ENC);
    case CE_TF_PANASONIC_VLOG:
      return CeLogCameraEncode(x, CE_PANASONIC_VLOG_ARGS_ENC);
    case CE_TF_CANON_CLOG2:
      return CeCanonClog2Encode(x);
    case CE_TF_CANON_CLOG3:
      return CeCanonClog3Encode(x);
    case CE_TF_RED_LOG3G10:
      return CeLogCameraEncode(x, CE_RED_LOG3G10_ARGS_ENC);
    case CE_TF_BLACKMAGIC_FILM_GEN5:
      return CeLogCameraEncode(x, CE_BLACKMAGIC_FILM_GEN5_ARGS_ENC);
    case CE_TF_DAVINCI_INTERMEDIATE:
      return CeLogCameraEncode(x, CE_DAVINCI_INTERMEDIATE_ARGS_ENC);
    case CE_TF_APPLE_LOG:
      return CeAppleLogEncode(x);
    case CE_TF_NIKON_NLOG:
      return CeNikonNlogEncode(x);
    case CE_TF_DJI_DLOG:
      return CeLogCameraEncode(x, CE_DJI_DLOG_ARGS_ENC);
    case CE_TF_CANON_CLOG:
      return CeCanonClogEncode(x);
    case CE_TF_CINEON:
      return CeLogCameraEncode(x, CE_CINEON_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI160:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI160_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI200:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI200_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI250:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI250_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI320:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI320_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI400:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI400_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI500:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI500_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI640:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI640_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI1000:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI1000_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI1280:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI1280_ARGS_ENC);
    case CE_TF_ARRI_LOGC3_EI1600:
      return CeLogCameraEncode(x, CE_ARRI_LOGC3_EI1600_ARGS_ENC);
    case CE_TF_LEICA_LLOG:
      return CeLogCameraEncode(x, CE_LEICA_LLOG_ARGS_ENC);
    case CE_TF_KINEFINITY_KINELOG3:
      return CeLogCameraEncode(x, CE_KINEFINITY_KINELOG3_ARGS_ENC);
    case CE_TF_GOPRO_GPLOG2:
      return CeGoproGplog2Encode(x);
    default:
      return x;
  }
}

/// Code value to linear light for transfer @p tf (CE_TF_*). An unknown id returns @p x.
CE_INLINE float CeDecode(int tf, float x) {
  switch (tf) {
    case CE_TF_LINEAR:
      return x;
    case CE_TF_ST2084:
      return CePqDecode(x);
    case CE_TF_HLG:
      return CeHlgDecode(x);
    case CE_TF_GAMMA_2_6:
      return CeGammaDecode(x, CE_GAMMA_2_6_EXPONENT);
    case CE_TF_BT1886:
      return CeGammaDecode(x, CE_BT1886_GAMMA);
    case CE_TF_GAMMA_2_2:
      return CeGammaDecode(x, CE_GAMMA_2_2_EXPONENT);
    case CE_TF_GAMMA_1_8:
      return CeGammaDecode(x, CE_GAMMA_1_8_EXPONENT);
    case CE_TF_SRGB:
      return CeSrgbDecode(x);
    case CE_TF_ACESCC:
      return CeAcesccDecode(x);
    case CE_TF_ACESCCT:
      return CeLogCameraDecode(x, CE_ACESCCT_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI800:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_ARGS_DEC);
    case CE_TF_ARRI_LOGC4:
      return CeLogCameraDecode(x, CE_ARRI_LOGC4_ARGS_DEC);
    case CE_TF_SONY_SLOG3:
      return CeLogCameraDecode(x, CE_SONY_SLOG3_ARGS_DEC);
    case CE_TF_FUJIFILM_FLOG:
      return CeLogCameraDecode(x, CE_FUJIFILM_FLOG_ARGS_DEC);
    case CE_TF_FUJIFILM_FLOG2:
      return CeLogCameraDecode(x, CE_FUJIFILM_FLOG2_ARGS_DEC);
    case CE_TF_PANASONIC_VLOG:
      return CeLogCameraDecode(x, CE_PANASONIC_VLOG_ARGS_DEC);
    case CE_TF_CANON_CLOG2:
      return CeCanonClog2Decode(x);
    case CE_TF_CANON_CLOG3:
      return CeCanonClog3Decode(x);
    case CE_TF_RED_LOG3G10:
      return CeLogCameraDecode(x, CE_RED_LOG3G10_ARGS_DEC);
    case CE_TF_BLACKMAGIC_FILM_GEN5:
      return CeLogCameraDecode(x, CE_BLACKMAGIC_FILM_GEN5_ARGS_DEC);
    case CE_TF_DAVINCI_INTERMEDIATE:
      return CeLogCameraDecode(x, CE_DAVINCI_INTERMEDIATE_ARGS_DEC);
    case CE_TF_APPLE_LOG:
      return CeAppleLogDecode(x);
    case CE_TF_NIKON_NLOG:
      return CeNikonNlogDecode(x);
    case CE_TF_DJI_DLOG:
      return CeLogCameraDecode(x, CE_DJI_DLOG_ARGS_DEC);
    case CE_TF_CANON_CLOG:
      return CeCanonClogDecode(x);
    case CE_TF_CINEON:
      return CeLogCameraDecode(x, CE_CINEON_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI160:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI160_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI200:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI200_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI250:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI250_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI320:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI320_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI400:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI400_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI500:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI500_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI640:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI640_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI1000:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI1000_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI1280:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI1280_ARGS_DEC);
    case CE_TF_ARRI_LOGC3_EI1600:
      return CeLogCameraDecode(x, CE_ARRI_LOGC3_EI1600_ARGS_DEC);
    case CE_TF_LEICA_LLOG:
      return CeLogCameraDecode(x, CE_LEICA_LLOG_ARGS_DEC);
    case CE_TF_KINEFINITY_KINELOG3:
      return CeLogCameraDecode(x, CE_KINEFINITY_KINELOG3_ARGS_DEC);
    case CE_TF_GOPRO_GPLOG2:
      return CeGoproGplog2Decode(x);
    default:
      return x;
  }
}

// ---------------------------------------------------------------------------------------------
// Display encoding of the DRT output
// ---------------------------------------------------------------------------------------------

/// Inverse OOTF gain of the 1000-nit HLG reference display for display light r, g, b
/// (1.0 = peak). Negative channels count as 0.
CE_INLINE float CeHlgDisplayGain(float r, float g, float b) {
  r = r > 0.0f ? r : 0.0f;
  g = g > 0.0f ? g : 0.0f;
  b = b > 0.0f ? b : 0.0f;
  return CeHlgInverseOotfGain(CE_BT2100_LUMA_R * r + CE_BT2100_LUMA_G * g + CE_BT2100_LUMA_B * b);
}

/**
 * Display encoding of one channel of the DRT output. @p display_linear is in the DRT's scaled
 * display units: cd/m^2 for ST 2084, 1.0 = peak otherwise. @p hlg_gain is CeHlgDisplayGain of
 * the pixel and is read only for HLG. Negative light encodes as 0.
 */
CE_INLINE float CeDisplayEncodeChannel(int tf, float display_linear, float hlg_gain) {
  const float v = display_linear > 0.0f ? display_linear : 0.0f;
  if (tf == CE_TF_HLG) return CeHlgEncode(v * hlg_gain);
  if (tf == CE_TF_ST2084) return CePqEncode(v / CE_PQ_PEAK_NITS);
  return CeEncode(tf, v);
}

#undef CE_INLINE
#undef CE_POW
#undef CE_EXP
#undef CE_EXP2
#undef CE_LOG
#undef CE_LOG2
#undef CE_SQRT
#endif
