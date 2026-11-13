//! Host rules of the SAM 2.1 tiny models (master plan section 8.17.3): image preprocessing,
//! prompt validation, decoder prompt encoding, and mask selection. Pure functions; no model call.

use anyhow::{Result, bail};

use super::model_set::{
    IMAGE_SIZE, LOGIT_SIZE, MASK_LOGITS_SHAPE, MASK_SCORES_SHAPE, MASK_TOKEN_COUNT,
    MAX_DECODER_POINTS,
};
use super::tensor::TensorF32;

/// ImageNet mean of the SAM 2.1 preprocessing.
pub const PIXEL_MEAN: [f32; 3] = [0.485, 0.456, 0.406];
/// ImageNet standard deviation of the SAM 2.1 preprocessing.
pub const PIXEL_STD: [f32; 3] = [0.229, 0.224, 0.225];
/// Selected logits are clipped to this magnitude, as the lab measurement scripts do.
pub const LOGIT_LIMIT: f32 = 32.0;
const STABILITY_DELTA: f32 = 0.05;
const STABILITY_THRESHOLD: f32 = 0.98;
const BOX_TOP_LEFT_LABEL: f32 = 2.0;
const BOX_BOTTOM_RIGHT_LABEL: f32 = 3.0;

/// Taps of a one-dimensional antialiased bilinear resize: for each output index, the first input
/// index and the normalized weights of the inputs that it reads.
struct ResizeTaps {
    starts: Vec<usize>,
    weights: Vec<Vec<f32>>,
}

/// Taps of PyTorch `interpolate(mode="bilinear", antialias=True, align_corners=False)` along one
/// axis (`_compute_weights_aa` of ATen): the triangle filter widened by the downscale factor,
/// centers at `(i + 0.5) * scale`, weights normalized to a sum of 1.
fn antialias_bilinear_taps(input_size: usize, output_size: usize) -> ResizeTaps {
    let scale = input_size as f64 / output_size as f64;
    let support = if scale >= 1.0 { scale } else { 1.0 };
    let inverse_scale = if scale >= 1.0 { 1.0 / scale } else { 1.0 };
    let mut starts = Vec::with_capacity(output_size);
    let mut weights = Vec::with_capacity(output_size);
    for index in 0..output_size {
        let center = scale * (index as f64 + 0.5);
        let start = ((center - support + 0.5) as i64).max(0) as usize;
        let end = ((center + support + 0.5) as i64).min(input_size as i64) as usize;
        let mut taps: Vec<f64> = (start..end)
            .map(|input| {
                let distance = (input as f64 - center + 0.5) * inverse_scale;
                (1.0 - distance.abs()).max(0.0)
            })
            .collect();
        let total: f64 = taps.iter().sum();
        if total != 0.0 {
            taps.iter_mut().for_each(|weight| *weight /= total);
        }
        starts.push(start);
        weights.push(taps.into_iter().map(|weight| weight as f32).collect());
    }
    ResizeTaps { starts, weights }
}

/// Resizes an interleaved image (HWC, @p channels per pixel) with the antialiased bilinear filter
/// of PyTorch: first along the width, then along the height, in float32. @p read converts one
/// source element to float.
fn resize_interleaved<T: Copy>(
    source: &[T],
    width: usize,
    height: usize,
    channels: usize,
    output_width: usize,
    output_height: usize,
    read: impl Fn(T) -> f32,
) -> Vec<f32> {
    let horizontal = antialias_bilinear_taps(width, output_width);
    let mut rows = vec![0.0f32; height * output_width * channels];
    for y in 0..height {
        let source_row = &source[y * width * channels..(y + 1) * width * channels];
        let target_row = &mut rows[y * output_width * channels..(y + 1) * output_width * channels];
        for (x, (start, taps)) in horizontal
            .starts
            .iter()
            .zip(&horizontal.weights)
            .enumerate()
        {
            for channel in 0..channels {
                let mut sum = 0.0f32;
                for (offset, weight) in taps.iter().enumerate() {
                    sum += weight * read(source_row[(start + offset) * channels + channel]);
                }
                target_row[x * channels + channel] = sum;
            }
        }
    }
    let vertical = antialias_bilinear_taps(height, output_height);
    let row_len = output_width * channels;
    let mut output = vec![0.0f32; output_height * row_len];
    for (y, (start, taps)) in vertical.starts.iter().zip(&vertical.weights).enumerate() {
        let target_row = &mut output[y * row_len..(y + 1) * row_len];
        for (offset, weight) in taps.iter().enumerate() {
            let source_row = &rows[(start + offset) * row_len..(start + offset + 1) * row_len];
            for (target, value) in target_row.iter_mut().zip(source_row) {
                *target += weight * value;
            }
        }
    }
    output
}

/// Resizes an interleaved float image (HWC) with the antialiased bilinear filter of PyTorch.
pub fn resize_bilinear_antialias(
    source: &[f32],
    width: usize,
    height: usize,
    channels: usize,
    output_width: usize,
    output_height: usize,
) -> Vec<f32> {
    resize_interleaved(
        source,
        width,
        height,
        channels,
        output_width,
        output_height,
        |value| value,
    )
}

/// The encoder input of an RGB8 image (row-major, 3 bytes per pixel): values / 255, stretched to
/// 1024 x 1024 with the antialiased bilinear filter, ImageNet normalization, shape
/// [1, 3, 1024, 1024]. This is torchvision `ToTensor`, `Resize((1024, 1024))`, `Normalize`.
pub fn preprocess_image(rgb8: &[u8], width: usize, height: usize) -> Result<TensorF32> {
    if width == 0 || height == 0 || rgb8.len() != width * height * 3 {
        bail!(
            "RGB8 image {}x{} needs {} bytes, got {}",
            width,
            height,
            width * height * 3,
            rgb8.len()
        );
    }
    let resized = resize_interleaved(rgb8, width, height, 3, IMAGE_SIZE, IMAGE_SIZE, |value| {
        f32::from(value) / 255.0
    });
    let plane = IMAGE_SIZE * IMAGE_SIZE;
    let mut chw = vec![0.0f32; 3 * plane];
    for (pixel, rgb) in resized.chunks_exact(3).enumerate() {
        for channel in 0..3 {
            chw[channel * plane + pixel] =
                (rgb[channel] - PIXEL_MEAN[channel]) / PIXEL_STD[channel];
        }
    }
    TensorF32::new(vec![1, 3, IMAGE_SIZE, IMAGE_SIZE], chw)
}

/// Label of a prompt point.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PointLabel {
    Subtract,
    Add,
}

impl PointLabel {
    fn decoder_value(self) -> f32 {
        match self {
            Self::Subtract => 0.0,
            Self::Add => 1.0,
        }
    }
}

/// A prompt point in normalized image coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PromptPoint {
    pub x: f32,
    pub y: f32,
    pub label: PointLabel,
}

/// A prompt box in normalized image coordinates; the corners can come in any order.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PromptBox {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
}

/// The decoder input of one stroke, click, or box: all points and the box of the selection, and
/// the selected logits of the previous decoder call.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SegmentationPrompt {
    pub points: Vec<PromptPoint>,
    pub prompt_box: Option<PromptBox>,
    /// Empty, or `LOGIT_SIZE * LOGIT_SIZE` logits.
    pub mask_input: Vec<f32>,
}

impl SegmentationPrompt {
    /// One point without a box: the decoder returns three levels.
    pub fn is_click(&self) -> bool {
        self.prompt_box.is_none() && self.points.len() == 1
    }

    fn decoder_point_count(&self) -> usize {
        self.points.len() + if self.prompt_box.is_some() { 2 } else { 0 }
    }
}

fn normalized_coordinate(value: f32) -> bool {
    value.is_finite() && (0.0..=1.0).contains(&value)
}

/// Checks the prompt rules of the SM3.A plan (section 2.4). The error is the message for the
/// host.
pub fn validate_prompt(prompt: &SegmentationPrompt) -> Result<(), String> {
    let count = prompt.decoder_point_count();
    if count == 0 {
        return Err("the prompt has no point and no box".to_string());
    }
    if count > MAX_DECODER_POINTS {
        return Err(format!(
            "the prompt has {count} decoder points (a box counts 2); the limit is {MAX_DECODER_POINTS}"
        ));
    }
    for (index, point) in prompt.points.iter().enumerate() {
        if !normalized_coordinate(point.x) || !normalized_coordinate(point.y) {
            return Err(format!(
                "point {index} ({}, {}) is outside the normalized range [0, 1]",
                point.x, point.y
            ));
        }
    }
    if let Some(prompt_box) = prompt.prompt_box {
        let corners = [prompt_box.x0, prompt_box.y0, prompt_box.x1, prompt_box.y1];
        if !corners.into_iter().all(normalized_coordinate) {
            return Err(format!(
                "box {corners:?} is outside the normalized range [0, 1]"
            ));
        }
        if prompt_box.x0 == prompt_box.x1 || prompt_box.y0 == prompt_box.y1 {
            return Err(format!("box {corners:?} has zero width or zero height"));
        }
    }
    let mask_len = LOGIT_SIZE * LOGIT_SIZE;
    if !prompt.mask_input.is_empty() {
        if prompt.mask_input.len() != mask_len {
            return Err(format!(
                "mask input has {} values, expected {mask_len}",
                prompt.mask_input.len()
            ));
        }
        if !prompt.mask_input.iter().all(|value| value.is_finite()) {
            return Err("mask input contains a value that is not finite".to_string());
        }
    }
    Ok(())
}

/// Decoder coordinates and labels of a valid prompt: the box corners first (minimum corner with
/// label 2, maximum corner with label 3), then the points; coordinates times 1024.
pub fn model_prompt(prompt: &SegmentationPrompt) -> (Vec<f32>, Vec<f32>) {
    let size = IMAGE_SIZE as f32;
    let mut coords = Vec::with_capacity(prompt.decoder_point_count() * 2);
    let mut labels = Vec::with_capacity(prompt.decoder_point_count());
    if let Some(prompt_box) = prompt.prompt_box {
        coords.extend([
            prompt_box.x0.min(prompt_box.x1) * size,
            prompt_box.y0.min(prompt_box.y1) * size,
            prompt_box.x0.max(prompt_box.x1) * size,
            prompt_box.y0.max(prompt_box.y1) * size,
        ]);
        labels.extend([BOX_TOP_LEFT_LABEL, BOX_BOTTOM_RIGHT_LABEL]);
    }
    for point in &prompt.points {
        coords.extend([point.x * size, point.y * size]);
        labels.push(point.label.decoder_value());
    }
    (coords, labels)
}

/// One decoder mask token, clipped.
#[derive(Debug, Clone, PartialEq)]
pub struct MaskLevel {
    /// `LOGIT_SIZE * LOGIT_SIZE` logits, row-major, clipped to +-`LOGIT_LIMIT`.
    pub logits: Vec<f32>,
    pub score: f32,
    pub token: u32,
    /// Number of logits > 0.
    pub area: u32,
}

/// The masks of one decoder call and the index of the selected mask.
#[derive(Debug, Clone, PartialEq)]
pub struct DecodedMasks {
    pub masks: Vec<MaskLevel>,
    pub selected_index: usize,
}

fn mask_level(all_logits: &[f32], scores: &[f32], token: usize) -> MaskLevel {
    let plane = LOGIT_SIZE * LOGIT_SIZE;
    let logits: Vec<f32> = all_logits[token * plane..(token + 1) * plane]
        .iter()
        .map(|value| value.clamp(-LOGIT_LIMIT, LOGIT_LIMIT))
        .collect();
    let area = logits.iter().filter(|value| **value > 0.0).count() as u32;
    MaskLevel {
        logits,
        score: scores[token],
        token: token as u32,
        area,
    }
}

/// Index of the first largest score among @p tokens.
fn best_scored(scores: &[f32], tokens: std::ops::Range<usize>) -> usize {
    let mut best = tokens.start;
    for token in tokens {
        if scores[token] > scores[best] {
            best = token;
        }
    }
    best
}

/// Selects the masks of a decoder call (section 2.4 of the SM3.A plan).
///
/// A click returns tokens 1 to 3 sorted by area, then token, with the best-scored token
/// selected. Another prompt returns one mask: token 0 when its stability (area of logits > 0.05
/// divided by the area of logits > -0.05) is at least 0.98, else the best-scored token 1 to 3.
pub fn select_masks(
    logits: &TensorF32,
    scores: &TensorF32,
    is_click: bool,
) -> Result<DecodedMasks> {
    logits.expect_shape("decoder mask logits", &MASK_LOGITS_SHAPE)?;
    scores.expect_shape("decoder scores", &MASK_SCORES_SHAPE)?;
    let all_logits = logits.data();
    let scores = scores.data();
    if is_click {
        let best = best_scored(scores, 1..MASK_TOKEN_COUNT);
        let mut levels: Vec<MaskLevel> = (1..MASK_TOKEN_COUNT)
            .map(|token| mask_level(all_logits, scores, token))
            .collect();
        levels.sort_by_key(|level| (level.area, level.token));
        let selected_index = levels
            .iter()
            .position(|level| level.token as usize == best)
            .unwrap_or(0);
        return Ok(DecodedMasks {
            masks: levels,
            selected_index,
        });
    }
    let plane = LOGIT_SIZE * LOGIT_SIZE;
    let first = &all_logits[..plane];
    let inner = first
        .iter()
        .filter(|value| **value > STABILITY_DELTA)
        .count();
    let outer = first
        .iter()
        .filter(|value| **value > -STABILITY_DELTA)
        .count();
    let stability = if outer > 0 {
        inner as f32 / outer as f32
    } else {
        1.0
    };
    let token = if stability >= STABILITY_THRESHOLD {
        0
    } else {
        best_scored(scores, 1..MASK_TOKEN_COUNT)
    };
    Ok(DecodedMasks {
        masks: vec![mask_level(all_logits, scores, token)],
        selected_index: 0,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pattern_image(width: usize, height: usize) -> Vec<f32> {
        let mut values = Vec::with_capacity(width * height * 3);
        for y in 0..height {
            for x in 0..width {
                for channel in 0..3 {
                    values.push(((x * 7 + y * 13 + channel * 29) % 17) as f32 / 16.0);
                }
            }
        }
        values
    }

    fn assert_close(actual: &[f32], expected: &[f32]) {
        assert_eq!(actual.len(), expected.len());
        for (index, (a, e)) in actual.iter().zip(expected).enumerate() {
            assert!((a - e).abs() < 1e-5, "element {index}: {a} != {e}");
        }
    }

    // Expected values: torch 2.14.1 `F.interpolate(x, size, mode="bilinear", antialias=True,
    // align_corners=False)` on the 7 x 5 pattern image of `pattern_image`, written as HWC.
    #[test]
    #[allow(clippy::excessive_precision)]
    fn resize_matches_pytorch_antialias_reference() {
        let source = pattern_image(7, 5);
        let downscaled = resize_bilinear_antialias(&source, 7, 5, 3, 3, 4);
        assert_close(
            &downscaled,
            &[
                0.4073660, 0.4933035, 0.5223213, 0.4531250, 0.6406250, 0.4375000, 0.4040179,
                0.5279018, 0.5569196, 0.5900297, 0.5431547, 0.4836309, 0.4166667, 0.5000000,
                0.4375000, 0.4538691, 0.6156994, 0.4360119, 0.5639880, 0.3843005, 0.5461309,
                0.5625000, 0.5000000, 0.5833334, 0.5163690, 0.4568452, 0.4099703, 0.4430803,
                0.4720982, 0.5959821, 0.5625000, 0.3593750, 0.5468750, 0.4776786, 0.5066964,
                0.5926339,
            ],
        );
        let upscaled = resize_bilinear_antialias(&source, 7, 5, 3, 9, 6);
        assert_eq!(upscaled.len(), 9 * 6 * 3);
        assert_close(
            &upscaled[..12],
            &[
                0.0000000, 0.7500000, 0.4375000, 0.2916667, 0.3333333, 0.7291667, 0.6319444,
                0.3194444, 0.5972222, 0.7361110, 0.6597223, 0.3472223,
            ],
        );
        assert_close(
            &upscaled[150..],
            &[
                0.6527777, 0.3402777, 0.2638890, 0.4027776, 0.6805557, 0.3680557, 0.2708334,
                0.6666666, 0.7083334, 0.5625000, 0.2500000, 1.0000000,
            ],
        );
    }

    #[test]
    fn preprocess_normalizes_constant_gray_image() {
        let (width, height) = (300, 200);
        let tensor =
            preprocess_image(&vec![128u8; width * height * 3], width, height).expect("preprocess");
        assert_eq!(tensor.shape(), &[1, 3, IMAGE_SIZE, IMAGE_SIZE]);
        let plane = IMAGE_SIZE * IMAGE_SIZE;
        for channel in 0..3 {
            let expected = (128.0 / 255.0 - PIXEL_MEAN[channel]) / PIXEL_STD[channel];
            let values = &tensor.data()[channel * plane..(channel + 1) * plane];
            assert!(
                values.iter().all(|value| (value - expected).abs() < 1e-5),
                "channel {channel}"
            );
        }
        assert!(preprocess_image(&[0u8; 10], 2, 2).is_err());
    }

    fn point(x: f32, y: f32, label: PointLabel) -> PromptPoint {
        PromptPoint { x, y, label }
    }

    #[test]
    fn model_prompt_puts_box_corners_first_and_scales_to_1024() {
        let prompt = SegmentationPrompt {
            points: vec![
                point(0.5, 0.25, PointLabel::Add),
                point(0.75, 1.0, PointLabel::Subtract),
            ],
            prompt_box: Some(PromptBox {
                x0: 0.5,
                y0: 0.5,
                x1: 0.25,
                y1: 0.125,
            }),
            mask_input: Vec::new(),
        };
        let (coords, labels) = model_prompt(&prompt);
        assert_eq!(
            coords,
            vec![256.0, 128.0, 512.0, 512.0, 512.0, 256.0, 768.0, 1024.0]
        );
        assert_eq!(labels, vec![2.0, 3.0, 1.0, 0.0]);
        assert!(!prompt.is_click());
    }

    #[test]
    fn validate_prompt_rejects_invalid_values() {
        let valid = SegmentationPrompt {
            points: vec![point(0.5, 0.5, PointLabel::Add)],
            ..Default::default()
        };
        assert_eq!(validate_prompt(&valid), Ok(()));
        let mut not_finite_mask = vec![0.0; LOGIT_SIZE * LOGIT_SIZE];
        not_finite_mask[7] = f32::INFINITY;
        let cases = [
            SegmentationPrompt::default(),
            SegmentationPrompt {
                points: vec![point(1.01, 0.5, PointLabel::Add)],
                ..Default::default()
            },
            SegmentationPrompt {
                points: vec![point(f32::NAN, 0.5, PointLabel::Add)],
                ..Default::default()
            },
            SegmentationPrompt {
                prompt_box: Some(PromptBox {
                    x0: 0.2,
                    y0: 0.3,
                    x1: 0.2,
                    y1: 0.6,
                }),
                ..Default::default()
            },
            SegmentationPrompt {
                points: vec![point(0.5, 0.5, PointLabel::Add); 63],
                prompt_box: Some(PromptBox {
                    x0: 0.1,
                    y0: 0.1,
                    x1: 0.2,
                    y1: 0.2,
                }),
                ..Default::default()
            },
            SegmentationPrompt {
                points: vec![point(0.5, 0.5, PointLabel::Add)],
                mask_input: vec![0.0; 100],
                ..Default::default()
            },
            SegmentationPrompt {
                points: vec![point(0.5, 0.5, PointLabel::Add)],
                mask_input: not_finite_mask,
                ..Default::default()
            },
        ];
        for (index, case) in cases.iter().enumerate() {
            assert!(validate_prompt(case).is_err(), "case {index} must fail");
        }
        let at_limit = SegmentationPrompt {
            points: vec![point(0.5, 0.5, PointLabel::Add); 64],
            ..Default::default()
        };
        assert_eq!(validate_prompt(&at_limit), Ok(()));
    }

    /// Decoder output with token t positive on the first `areas[t]` logits.
    fn decoder_output(
        areas: [usize; 4],
        scores: [f32; 4],
        magnitude: f32,
    ) -> (TensorF32, TensorF32) {
        let plane = LOGIT_SIZE * LOGIT_SIZE;
        let mut logits = vec![-magnitude; 4 * plane];
        for (token, area) in areas.iter().enumerate() {
            logits[token * plane..token * plane + area].fill(magnitude);
        }
        (
            TensorF32::new(MASK_LOGITS_SHAPE.to_vec(), logits).expect("logits"),
            TensorF32::new(MASK_SCORES_SHAPE.to_vec(), scores.to_vec()).expect("scores"),
        )
    }

    #[test]
    fn click_returns_three_levels_sorted_by_area() {
        let (logits, scores) = decoder_output([10, 500, 20, 300], [0.9, 0.5, 0.95, 0.7], 5.0);
        let decoded = select_masks(&logits, &scores, true).expect("select");
        let tokens: Vec<u32> = decoded.masks.iter().map(|level| level.token).collect();
        assert_eq!(tokens, vec![2, 3, 1]);
        let areas: Vec<u32> = decoded.masks.iter().map(|level| level.area).collect();
        assert_eq!(areas, vec![20, 300, 500]);
        assert_eq!(decoded.selected_index, 0);
        assert_eq!(decoded.masks[0].score, 0.95);
    }

    #[test]
    fn box_selects_token_zero_when_stable() {
        let (logits, scores) = decoder_output([4000, 10, 20, 30], [0.1, 0.9, 0.8, 0.7], 5.0);
        let decoded = select_masks(&logits, &scores, false).expect("select");
        assert_eq!(decoded.masks.len(), 1);
        assert_eq!(decoded.masks[0].token, 0);
        assert_eq!(decoded.masks[0].area, 4000);
    }

    #[test]
    fn box_selects_best_scored_token_when_unstable() {
        let (logits, scores) = decoder_output([1000, 10, 20, 30], [0.99, 0.2, 0.8, 0.7], 5.0);
        let mut values = logits.into_data();
        // 100 logits of token 0 between -0.05 and 0.05 lower its stability below 0.98.
        values[1000..1100].fill(0.0);
        values[..1000].fill(5.0);
        let logits = TensorF32::new(MASK_LOGITS_SHAPE.to_vec(), values).expect("logits");
        let decoded = select_masks(&logits, &scores, false).expect("select");
        assert_eq!(decoded.masks[0].token, 2);
    }

    #[test]
    fn selected_logits_are_clipped_to_32() {
        let (logits, scores) = decoder_output([5000, 1, 2, 3], [0.9, 0.1, 0.1, 0.1], 100.0);
        let decoded = select_masks(&logits, &scores, false).expect("select");
        let level = &decoded.masks[0];
        assert!(level.logits.iter().all(|value| value.abs() <= LOGIT_LIMIT));
        assert_eq!(level.logits[0], LOGIT_LIMIT);
        assert_eq!(level.logits[LOGIT_SIZE * LOGIT_SIZE - 1], -LOGIT_LIMIT);
    }
}
