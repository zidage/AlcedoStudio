//! Host rules of the task masks Subject, Background, Sky, People, and Person (SM3.C plan, section
//! 2; SM3.D plan, sections 2.3 and 2.4; SM3.E plan, the "sky found" rule of the sky head). Pure
//! functions; no model call.
//!
//! The rules follow the lab MVP (`mask_pipeline.py` of `alcedo-ai-mask-lab`): the person boxes of
//! `head_model.decode_people` with an overlap limit of 0.5, the S3OD letterbox of `s3od_graphs.py`,
//! and the person crops and `subject_fraction` of SM3.D. The People list threshold rescue (C2), the
//! list order (C3), and the "not found" rules (C1, C9) are decisions of the SM3.C plan; the crop
//! rectangle (E3), the stored crop (E4), and `subject_fraction` (E8) are decisions of the SM3.D
//! plan. The MVP Sky (the people and sky head regions and the union of their decoder masks, SM3.C
//! C4 and C5) was removed when the sky head ran on every backend (issue #538).

use anyhow::{Context, Result, bail};
use serde_json::Value;

use super::backend::{HeadOutput, SubjectOutput};
use super::model_set::{
    HEAD_GRID, SUBJECT_IMAGE_SIZE, SUBJECT_MASKS_SHAPE, SUBJECT_PIXEL_VALUES_SHAPE,
    SUBJECT_SCORES_SHAPE,
};
use super::sam2::{PIXEL_MEAN, PIXEL_STD, PromptBox};
use super::tensor::TensorF32;

/// Number of center heatmap peaks that the person decoding reads.
pub const PERSON_PEAK_COUNT: usize = 20;
/// Smallest center heatmap value of a person box.
pub const PERSON_MIN_SCORE: f32 = 0.05;
/// A person box is removed when its IoU with a box of higher score is at least this value.
pub const PERSON_OVERLAP_LIMIT: f32 = 0.5;
/// A Subject, Background, or sky alpha value of at least this value is inside the mask.
pub const ALPHA_INSIDE: f32 = 0.5;
/// Sky is found when at least this share of the sky head alpha values is inside (SM3.E plan,
/// decision S3): 0.05 % of the image. On the 1,500 MaSS13K test photos (lab
/// `results/sky_head/sky_found_rule_test.json`), the rule finds 906 of 938 photos with sky and 15
/// of 562 photos without sky, the most correct decisions of the measured shares.
pub const SKY_FOUND_SHARE: f32 = 5e-4;
/// The crop of a person is the clipped person box widened by this share of the box width on the
/// left and on the right, and of the box height on the top and on the bottom (SM3.D decision E3).
/// The value is float64, as in the lab rule, so that both give the same pixel edges.
pub const PERSON_CROP_MARGIN: f64 = 0.1;
/// A person is included in the Subject alpha when `subject_fraction` is at least this value (SM3.D
/// decision E8; the lab check of 2026-10-23: 48 of 50 people agree with the lab rule).
pub const PERSON_IN_SUBJECT_FRACTION: f32 = 0.15;
/// Tolerance of the pixel edges in [`pixel_span`], in pixels. A box corner is a float32 value (an
/// error of about 1e-8), so a corner that is a whole pixel in decimal gives that pixel and not the
/// next one.
pub const PIXEL_EDGE_TOLERANCE: f64 = 1e-4;

/// Presence and list thresholds of the people and sky head (`people_sky_head.json`, best F1 on
/// the COCO validation set).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HeadThresholds {
    pub person_presence: f32,
    pub people_list: f32,
}

/// Reads the thresholds from the text of `people_sky_head.json`. A missing threshold is an error.
pub fn head_thresholds_from_json(text: &str) -> Result<HeadThresholds> {
    let json: Value = serde_json::from_str(text).context("the head thresholds are not JSON")?;
    let read = |key: &str| -> Result<f32> {
        json["validation"][key]["threshold"]
            .as_f64()
            .map(|value| value as f32)
            .with_context(|| format!("the head thresholds have no validation.{key}.threshold"))
    };
    Ok(HeadThresholds {
        person_presence: read("presence_best_f1_person")?,
        people_list: read("person_list_best_f1_nms")?,
    })
}

pub fn sigmoid(value: f32) -> f32 {
    1.0 / (1.0 + (-value).exp())
}

/// Person presence of @p head (sigmoid of the second `presence_logits` value).
pub fn person_presence(head: &HeadOutput) -> f32 {
    sigmoid(head.presence_logits.data()[1])
}

/// A person box in head grid cells (x0, y0, x1, y1) and its center heatmap score.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PersonBox {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    pub score: f32,
}

impl PersonBox {
    fn area(&self) -> f32 {
        (self.x1 - self.x0).max(0.0) * (self.y1 - self.y0).max(0.0)
    }

    fn iou(&self, other: &PersonBox) -> f32 {
        let width = (self.x1.min(other.x1) - self.x0.max(other.x0)).max(0.0);
        let height = (self.y1.min(other.y1) - self.y0.max(other.y0)).max(0.0);
        let intersection = width * height;
        intersection / (self.area() + other.area() - intersection).max(1e-9)
    }

    fn center_x(&self) -> f32 {
        (self.x0 + self.x1) / 2.0
    }

    /// The box clipped to the head grid, in normalized image coordinates: the box prompt of the
    /// person.
    pub fn prompt_box(&self) -> PromptBox {
        let grid = HEAD_GRID as f32;
        let normalized = |value: f32| value.clamp(0.0, grid) / grid;
        PromptBox {
            x0: normalized(self.x0),
            y0: normalized(self.y0),
            x1: normalized(self.x1),
            y1: normalized(self.y1),
        }
    }
}

/// The person boxes of @p head, highest score first (`head_model.decode_people` with an overlap
/// limit of 0.5): the 3 x 3 local maxima of the center heatmap, the `PERSON_PEAK_COUNT` highest
/// peaks with a score >= `PERSON_MIN_SCORE`, the box from the center offset and the size, and
/// the removal of boxes that overlap a box of higher score.
pub fn decode_people(head: &HeadOutput) -> Vec<PersonBox> {
    let heatmap: Vec<f32> = head
        .center_logits
        .data()
        .iter()
        .copied()
        .map(sigmoid)
        .collect();
    let grid = HEAD_GRID as isize;
    let mut peaks: Vec<(f32, usize)> = Vec::new();
    for (cell, value) in heatmap.iter().enumerate() {
        let (row, column) = ((cell / HEAD_GRID) as isize, (cell % HEAD_GRID) as isize);
        let mut neighborhood_max = f32::NEG_INFINITY;
        for y in (row - 1).max(0)..=(row + 1).min(grid - 1) {
            for x in (column - 1).max(0)..=(column + 1).min(grid - 1) {
                neighborhood_max = neighborhood_max.max(heatmap[(y * grid + x) as usize]);
            }
        }
        if *value == neighborhood_max {
            peaks.push((*value, cell));
        }
    }
    peaks.sort_by(|a, b| b.0.total_cmp(&a.0).then_with(|| a.1.cmp(&b.1)));
    let plane = HEAD_GRID * HEAD_GRID;
    let offset = head.center_offset.data();
    let log_size = head.log_size.data();
    let boxes: Vec<PersonBox> = peaks
        .into_iter()
        .take(PERSON_PEAK_COUNT)
        .filter(|(score, _)| *score >= PERSON_MIN_SCORE)
        .map(|(score, cell)| {
            let center_x = (cell % HEAD_GRID) as f32 + offset[cell];
            let center_y = (cell / HEAD_GRID) as f32 + offset[plane + cell];
            let width = log_size[cell].exp();
            let height = log_size[plane + cell].exp();
            PersonBox {
                x0: center_x - width / 2.0,
                y0: center_y - height / 2.0,
                x1: center_x + width / 2.0,
                y1: center_y + height / 2.0,
                score,
            }
        })
        .collect();
    let mut keep = vec![true; boxes.len()];
    for index in 0..boxes.len() {
        if !keep[index] {
            continue;
        }
        for later in index + 1..boxes.len() {
            if boxes[index].iou(&boxes[later]) >= PERSON_OVERLAP_LIMIT {
                keep[later] = false;
            }
        }
    }
    boxes
        .into_iter()
        .zip(keep)
        .filter_map(|(person, kept)| kept.then_some(person))
        .collect()
}

/// The People list of @p boxes (highest score first): the boxes with a score >= @p
/// list_threshold; when none reaches it and @p presence_passed, the box with the best score (C2).
/// Sorted from left to right by the box center, equal centers by the score, highest first (C3).
pub fn people_list(
    boxes: &[PersonBox],
    list_threshold: f32,
    presence_passed: bool,
) -> Vec<PersonBox> {
    let mut listed: Vec<PersonBox> = boxes
        .iter()
        .copied()
        .filter(|person| person.score >= list_threshold)
        .collect();
    if listed.is_empty() && presence_passed {
        listed.extend(boxes.first().copied());
    }
    listed.sort_by(|a, b| {
        a.center_x()
            .total_cmp(&b.center_x())
            .then_with(|| b.score.total_cmp(&a.score))
    });
    listed
}

/// Number of alpha values >= `ALPHA_INSIDE`.
pub fn alpha_area(alpha: &[f32]) -> u32 {
    alpha.iter().filter(|value| **value >= ALPHA_INSIDE).count() as u32
}

/// The share of the values of the sky alpha @p alpha that are inside; 0 for an empty alpha.
pub fn sky_share(alpha: &[f32]) -> f32 {
    if alpha.is_empty() {
        return 0.0;
    }
    alpha_area(alpha) as f32 / alpha.len() as f32
}

/// An RGB8 image with a long edge of at most 1024, row-major: the S3OD input image of a photo (C8,
/// the photo resized to a long edge of 1024) or the stored crop of a person (SM3.D decision E4).
/// The padding of the letterbox is added by [`subject_pixel_values`].
#[derive(Debug, Clone, PartialEq)]
pub struct SubjectInputImage {
    pub width: usize,
    pub height: usize,
    pub rgb8: Vec<u8>,
}

/// The resized size (width, height) of a photo: each side times 1024 / long edge, rounded half to
/// even as Python `round` (`s3od.utils.get_pad_info`).
pub fn subject_resized_size(width: usize, height: usize) -> (usize, usize) {
    let scale = SUBJECT_IMAGE_SIZE as f64 / width.max(height) as f64;
    let resized = |side: usize| ((side as f64 * scale).round_ties_even() as usize).max(1);
    (resized(width), resized(height))
}

/// One interpolation tap of OpenCV `INTER_LINEAR`: the first input index (it can be -1 or the last
/// index on the vertical axis) and the fixed-point weights of the first and the second input.
#[derive(Debug, Clone, Copy)]
struct LinearTap {
    source: isize,
    first_weight: i32,
    second_weight: i32,
}

/// Interpolation taps of OpenCV `INTER_LINEAR` along one axis (`resizeGeneric` of
/// `imgproc/src/resize.cpp`): position `(index + 0.5) * scale - 0.5` in float32, the floor as the
/// first input and the rest as the fraction. Each weight is rounded separately to the nearest
/// integer, ties to even (`saturate_cast<short>((1 - fraction) * 2048)` and `(fraction * 2048)`).
/// With @p clamp (the horizontal axis), a position before the first input or at or after the last
/// input uses that input with the fraction 0. Without it (the vertical axis), OpenCV keeps the
/// fraction and clamps only the rows that it reads.
fn linear_taps(input: usize, output: usize, clamp: bool) -> Vec<LinearTap> {
    const COEFFICIENT_SCALE: f32 = 2048.0;
    let scale = 1.0 / (output as f64 / input as f64);
    (0..output)
        .map(|index| {
            let position = ((index as f64 + 0.5) * scale - 0.5) as f32;
            let mut source = position.floor() as isize;
            let mut fraction = position - source as f32;
            if clamp && source < 0 {
                source = 0;
                fraction = 0.0;
            }
            if clamp && source >= input as isize - 1 {
                source = input as isize - 1;
                fraction = 0.0;
            }
            LinearTap {
                source,
                first_weight: ((1.0 - fraction) * COEFFICIENT_SCALE).round_ties_even() as i32,
                second_weight: (fraction * COEFFICIENT_SCALE).round_ties_even() as i32,
            }
        })
        .collect()
}

/// The S3OD input image of an RGB8 image: the OpenCV `INTER_LINEAR` resize of 8-bit images
/// (half-pixel centers, no antialiasing) to [`subject_resized_size`], bit for bit as `cv2.resize`
/// in the lab and the port reference: the horizontal pass is `a * w0 + b * w1` with the weights
/// times 2048; the vertical pass is the SIMD rule of OpenCV `VResizeLinearVec_32s8u`,
/// `((((a >> 4) * w0) >> 16) + (((b >> 4) * w1) >> 16) + 2) >> 2`, saturated to RGB8.
pub fn subject_input_image(rgb8: &[u8], width: usize, height: usize) -> Result<SubjectInputImage> {
    if width == 0 || height == 0 || rgb8.len() != width * height * 3 {
        bail!(
            "RGB8 image {width}x{height} needs {} bytes, got {}",
            width * height * 3,
            rgb8.len()
        );
    }
    let (output_width, output_height) = subject_resized_size(width, height);
    let columns = linear_taps(width, output_width, true);
    let rows = linear_taps(height, output_height, false);
    let horizontal = |row: usize| -> Vec<i32> {
        let source = &rgb8[row * width * 3..(row + 1) * width * 3];
        let mut values = Vec::with_capacity(output_width * 3);
        for tap in &columns {
            let x = tap.source as usize;
            let next = (x + 1).min(width - 1);
            for channel in 0..3 {
                let a = i32::from(source[x * 3 + channel]);
                let b = i32::from(source[next * 3 + channel]);
                values.push(a * tap.first_weight + b * tap.second_weight);
            }
        }
        values
    };
    let last_row = height as isize - 1;
    let mut output = Vec::with_capacity(output_width * output_height * 3);
    for tap in &rows {
        let top = horizontal(tap.source.clamp(0, last_row) as usize);
        let bottom = horizontal((tap.source + 1).clamp(0, last_row) as usize);
        for (a, b) in top.iter().zip(&bottom) {
            let value = ((((a >> 4) * tap.first_weight) >> 16)
                + (((b >> 4) * tap.second_weight) >> 16)
                + 2)
                >> 2;
            output.push(value.clamp(0, 255) as u8);
        }
    }
    Ok(SubjectInputImage {
        width: output_width,
        height: output_height,
        rgb8: output,
    })
}

/// Top and left padding of the letterbox of @p image.
fn subject_padding(image: &SubjectInputImage) -> (usize, usize) {
    (
        (SUBJECT_IMAGE_SIZE - image.height) / 2,
        (SUBJECT_IMAGE_SIZE - image.width) / 2,
    )
}

/// The S3OD input of @p image: the image in the middle of a black 1024 x 1024 image, `(value / 255
/// - mean) / std` with the ImageNet values (computed in float64, as NumPy does), NCHW float32.
pub fn subject_pixel_values(image: &SubjectInputImage) -> Result<TensorF32> {
    let size = SUBJECT_IMAGE_SIZE;
    if image.width > size
        || image.height > size
        || image.rgb8.len() != image.width * image.height * 3
    {
        bail!(
            "S3OD input image {}x{} with {} bytes does not fit {size}x{size}",
            image.width,
            image.height,
            image.rgb8.len()
        );
    }
    let normalize = |value: u8, channel: usize| -> f32 {
        ((f64::from(value) / 255.0 - f64::from(PIXEL_MEAN[channel]))
            / f64::from(PIXEL_STD[channel])) as f32
    };
    let plane = size * size;
    let mut data = Vec::with_capacity(3 * plane);
    for channel in 0..3 {
        data.extend(std::iter::repeat_n(normalize(0, channel), plane));
    }
    let (top, left) = subject_padding(image);
    for y in 0..image.height {
        for x in 0..image.width {
            let pixel = &image.rgb8[(y * image.width + x) * 3..][..3];
            let target = (top + y) * size + left + x;
            for channel in 0..3 {
                data[channel * plane + target] = normalize(pixel[channel], channel);
            }
        }
    }
    TensorF32::new(SUBJECT_PIXEL_VALUES_SHAPE.to_vec(), data)
}

/// The Subject alpha of one image: the selected S3OD mask without the padding.
#[derive(Debug, Clone, PartialEq)]
pub struct SubjectAlpha {
    pub width: usize,
    pub height: usize,
    /// Row-major values in [0, 1].
    pub values: Vec<f32>,
    /// IoU score of the selected mask.
    pub score: f32,
}

/// Selects the S3OD mask with the largest IoU score (the first on equal scores) and removes the
/// padding of @p image.
pub fn select_subject_alpha(
    output: &SubjectOutput,
    image: &SubjectInputImage,
) -> Result<SubjectAlpha> {
    output
        .masks
        .expect_shape("S3OD masks", &SUBJECT_MASKS_SHAPE)?;
    output
        .iou_scores
        .expect_shape("S3OD IoU scores", &SUBJECT_SCORES_SHAPE)?;
    let scores = output.iou_scores.data();
    let mut best = 0;
    for (index, score) in scores.iter().enumerate() {
        if *score > scores[best] {
            best = index;
        }
    }
    let size = SUBJECT_IMAGE_SIZE;
    let mask = &output.masks.data()[best * size * size..(best + 1) * size * size];
    let (top, left) = subject_padding(image);
    let mut values = Vec::with_capacity(image.width * image.height);
    for y in 0..image.height {
        let row = (top + y) * size + left;
        values.extend_from_slice(&mask[row..row + image.width]);
    }
    Ok(SubjectAlpha {
        width: image.width,
        height: image.height,
        values,
        score: scores[best],
    })
}

/// A pixel rectangle of an image: the columns `x0..x1` and the rows `y0..y1` (ends exclusive).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PixelRect {
    pub x0: usize,
    pub y0: usize,
    pub x1: usize,
    pub y1: usize,
}

impl PixelRect {
    pub fn width(&self) -> usize {
        self.x1 - self.x0
    }

    pub fn height(&self) -> usize {
        self.y1 - self.y0
    }
}

/// The pixels `first..end` of the normalized span @p start to @p end on an axis of @p size pixels:
/// `floor(start * size)` and `ceil(end * size)` with `PIXEL_EDGE_TOLERANCE`, at least one pixel,
/// inside the axis. @p size is at least 1.
pub fn pixel_span(start: f64, end: f64, size: usize) -> (usize, usize) {
    let last_first = size as f64 - 1.0;
    let first = (start * size as f64 + PIXEL_EDGE_TOLERANCE)
        .floor()
        .clamp(0.0, last_first);
    let end = (end * size as f64 - PIXEL_EDGE_TOLERANCE)
        .ceil()
        .clamp(first + 1.0, size as f64);
    (first as usize, end as usize)
}

/// The crop rectangle of @p person on an image of @p width x @p height (SM3.D decision E3): the
/// clipped box ([`PersonBox::prompt_box`]) widened by `PERSON_CROP_MARGIN` of the box width on the
/// left and on the right and of the box height on the top and on the bottom, clipped to the image,
/// at least one pixel on each axis. Computed in float64 as the lab rule.
pub fn person_crop_rect(person: &PersonBox, width: usize, height: usize) -> PixelRect {
    let prompt = person.prompt_box();
    let (x0, y0, x1, y1) = (
        f64::from(prompt.x0),
        f64::from(prompt.y0),
        f64::from(prompt.x1),
        f64::from(prompt.y1),
    );
    let margin_x = PERSON_CROP_MARGIN * (x1 - x0);
    let margin_y = PERSON_CROP_MARGIN * (y1 - y0);
    let (left, right) = pixel_span((x0 - margin_x).max(0.0), (x1 + margin_x).min(1.0), width);
    let (top, bottom) = pixel_span((y0 - margin_y).max(0.0), (y1 + margin_y).min(1.0), height);
    PixelRect {
        x0: left,
        y0: top,
        x1: right,
        y1: bottom,
    }
}

/// The stored crop image of @p rect of an RGB8 image (SM3.D decision E4): the crop pixels when the
/// long edge is at most `SUBJECT_IMAGE_SIZE`, else [`subject_input_image`] of the crop pixels.
/// [`subject_input_image`] of the stored crop then gives the same S3OD input image as one resize of
/// the crop pixels (the resize to the same size is the identity).
pub fn person_crop_image(
    rgb8: &[u8],
    width: usize,
    height: usize,
    rect: PixelRect,
) -> Result<SubjectInputImage> {
    if rgb8.len() != width * height * 3
        || rect.x0 >= rect.x1
        || rect.y0 >= rect.y1
        || rect.x1 > width
        || rect.y1 > height
    {
        bail!(
            "crop {rect:?} is not inside the RGB8 image {width}x{height} with {} bytes",
            rgb8.len()
        );
    }
    let (crop_width, crop_height) = (rect.width(), rect.height());
    let mut pixels = Vec::with_capacity(crop_width * crop_height * 3);
    for y in rect.y0..rect.y1 {
        pixels.extend_from_slice(&rgb8[(y * width + rect.x0) * 3..(y * width + rect.x1) * 3]);
    }
    if crop_width.max(crop_height) <= SUBJECT_IMAGE_SIZE {
        return Ok(SubjectInputImage {
            width: crop_width,
            height: crop_height,
            rgb8: pixels,
        });
    }
    subject_input_image(&pixels, crop_width, crop_height)
}

/// The share of the pixels of @p alpha inside the clipped box of @p person (not widened) whose
/// alpha is >= `ALPHA_INSIDE` (SM3.D decision E8, section 2.3). The box pixels are the columns
/// `floor(x0 * w)` to `ceil(x1 * w) - 1` and the rows `floor(y0 * h)` to `ceil(y1 * h) - 1`, at
/// least one pixel on each axis ([`pixel_span`]).
pub fn subject_fraction(alpha: &SubjectAlpha, person: &PersonBox) -> f32 {
    let prompt = person.prompt_box();
    let (left, right) = pixel_span(f64::from(prompt.x0), f64::from(prompt.x1), alpha.width);
    let (top, bottom) = pixel_span(f64::from(prompt.y0), f64::from(prompt.y1), alpha.height);
    let inside: usize = (top..bottom)
        .map(|y| {
            alpha.values[y * alpha.width + left..y * alpha.width + right]
                .iter()
                .filter(|value| **value >= ALPHA_INSIDE)
                .count()
        })
        .sum();
    (inside as f64 / ((right - left) * (bottom - top)) as f64) as f32
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::service::segmentation::model_set::{
        CENTER_LOGITS_SHAPE, CENTER_OFFSET_SHAPE, LOG_SIZE_SHAPE, PRESENCE_LOGITS_SHAPE,
        SKY_LOGITS_SHAPE,
    };

    const LAB_THRESHOLDS: &str = r#"{"validation": {
        "presence_best_f1_sky": {"threshold": 0.4231989085674286},
        "presence_best_f1_person": {"threshold": 0.533202052116394},
        "person_list_best_f1_nms": {"threshold": 0.3132133185863495}}}"#;

    fn head_with(center_logits: Vec<f32>, offset: Vec<f32>, log_size: Vec<f32>) -> HeadOutput {
        HeadOutput {
            sky_logits: TensorF32::zeros(SKY_LOGITS_SHAPE.to_vec()),
            center_logits: TensorF32::new(CENTER_LOGITS_SHAPE.to_vec(), center_logits).unwrap(),
            center_offset: TensorF32::new(CENTER_OFFSET_SHAPE.to_vec(), offset).unwrap(),
            log_size: TensorF32::new(LOG_SIZE_SHAPE.to_vec(), log_size).unwrap(),
            presence_logits: TensorF32::zeros(PRESENCE_LOGITS_SHAPE.to_vec()),
        }
    }

    fn cell(row: usize, column: usize) -> usize {
        row * HEAD_GRID + column
    }

    #[test]
    fn head_thresholds_read_from_lab_json() {
        let thresholds = head_thresholds_from_json(LAB_THRESHOLDS).expect("thresholds");
        assert!((thresholds.person_presence - 0.5332).abs() < 1e-4);
        assert!((thresholds.people_list - 0.3132).abs() < 1e-4);
        let missing = r#"{"validation": {"presence_best_f1_sky": {"threshold": 0.4},
            "presence_best_f1_person": {"threshold": 0.5}}}"#;
        let err = head_thresholds_from_json(missing).expect_err("missing list threshold");
        assert!(err.to_string().contains("person_list_best_f1_nms"), "{err}");
        assert!(head_thresholds_from_json("not json").is_err());
    }

    #[test]
    fn decode_people_finds_peaks_and_suppresses_overlaps() {
        let plane = HEAD_GRID * HEAD_GRID;
        let mut center = vec![-10.0f32; plane];
        let mut offset = vec![0.0f32; 2 * plane];
        let mut log_size = vec![0.0f32; 2 * plane];
        // Peak 1 at (10, 10), score sigmoid(2); box 8 x 16 cells, offset (0.25, -0.5).
        center[cell(10, 10)] = 2.0;
        offset[cell(10, 10)] = 0.25;
        offset[plane + cell(10, 10)] = -0.5;
        log_size[cell(10, 10)] = 8f32.ln();
        log_size[plane + cell(10, 10)] = 16f32.ln();
        // Peak 2 at (10, 12): a lower score and the same box size; it overlaps peak 1.
        center[cell(10, 12)] = 1.0;
        log_size[cell(10, 12)] = 8f32.ln();
        log_size[plane + cell(10, 12)] = 16f32.ln();
        // Peak 3 at (40, 50): a separate person.
        center[cell(40, 50)] = 0.0;
        // A neighbor of peak 1 with a lower value is not a peak.
        center[cell(10, 11)] = 0.5;
        let boxes = decode_people(&head_with(center, offset, log_size));
        assert_eq!(boxes.len(), 2);
        assert!((boxes[0].score - sigmoid(2.0)).abs() < 1e-6);
        let expected = (10.25 - 4.0, 9.5 - 8.0, 10.25 + 4.0, 9.5 + 8.0);
        assert!((boxes[0].x0 - expected.0).abs() < 1e-5);
        assert!((boxes[0].y0 - expected.1).abs() < 1e-5);
        assert!((boxes[0].x1 - expected.2).abs() < 1e-5);
        assert!((boxes[0].y1 - expected.3).abs() < 1e-5);
        assert!((boxes[1].score - 0.5).abs() < 1e-6);
        assert_eq!((boxes[1].x0 + boxes[1].x1) / 2.0, 50.0);
    }

    fn person(center_x: f32, score: f32) -> PersonBox {
        PersonBox {
            x0: center_x - 1.0,
            y0: 0.0,
            x1: center_x + 1.0,
            y1: 4.0,
            score,
        }
    }

    #[test]
    fn people_list_orders_left_to_right_and_keeps_best_box() {
        let boxes = [person(30.0, 0.9), person(10.0, 0.6), person(20.0, 0.2)];
        let listed = people_list(&boxes, 0.3132, true);
        assert_eq!(
            listed.iter().map(|p| p.center_x()).collect::<Vec<_>>(),
            vec![10.0, 30.0]
        );
        // Equal centers: the higher score first.
        let equal = people_list(&[person(5.0, 0.4), person(5.0, 0.8)], 0.3, true);
        assert_eq!(equal[0].score, 0.8);
        // No box reaches the threshold: the best box when the presence passed (C2).
        let below = [person(12.0, 0.31267), person(40.0, 0.1)];
        let rescued = people_list(&below, 0.31321, true);
        assert_eq!(rescued, vec![below[0]]);
        assert!(people_list(&below, 0.31321, false).is_empty());
        assert!(people_list(&[], 0.3, true).is_empty());
    }

    #[test]
    fn person_prompt_box_is_clipped_and_normalized() {
        let person = PersonBox {
            x0: -2.0,
            y0: 8.0,
            x1: 70.0,
            y1: 40.0,
            score: 0.9,
        };
        let prompt = person.prompt_box();
        assert_eq!(
            (prompt.x0, prompt.y0, prompt.x1, prompt.y1),
            (0.0, 0.125, 1.0, 0.625)
        );
    }

    #[test]
    fn subject_input_image_matches_letterbox_size() {
        assert_eq!(subject_resized_size(4096, 2731), (1024, 683));
        assert_eq!(subject_resized_size(2731, 4096), (683, 1024));
        // 4096 x 2730 -> 682.5 rounds half to even: 682.
        assert_eq!(subject_resized_size(4096, 2730), (1024, 682));
        // 2048 x 1365 -> 682.5 rounds half to even: 682.
        assert_eq!(subject_resized_size(2048, 1365), (1024, 682));
        assert_eq!(subject_resized_size(512, 256), (1024, 512));
        let pixels: Vec<u8> = std::iter::repeat_n([10u8, 128, 250], 300 * 200)
            .flatten()
            .collect();
        let image = subject_input_image(&pixels, 300, 200).expect("resize");
        assert_eq!((image.width, image.height), (1024, 683));
        assert!(image.rgb8.chunks_exact(3).all(|rgb| rgb == [10, 128, 250]));
        assert!(subject_input_image(&pixels[..10], 300, 200).is_err());
    }

    #[test]
    fn subject_input_image_interpolates_between_pixels() {
        // A 2 x 1 image upsampled to 1024 x 512: OpenCV half-pixel linear interpolation.
        let image = subject_input_image(&[0, 0, 0, 200, 200, 200], 2, 1).expect("resize");
        assert_eq!((image.width, image.height), (1024, 512));
        let row: Vec<u8> = image
            .rgb8
            .chunks_exact(3)
            .take(1024)
            .map(|rgb| rgb[0])
            .collect();
        assert_eq!(row[0], 0);
        assert_eq!(row[255], 0);
        assert_eq!(row[1023], 200);
        assert!(row.windows(2).all(|pair| pair[0] <= pair[1]));
        // Output 511 samples the source position 0.4990234: 200 * 0.499 rounded.
        assert_eq!(row[511], 100);
    }

    /// Byte sum and selected pixels of `cv2.resize(image, size, interpolation=cv2.INTER_LINEAR)`
    /// (OpenCV 5.0.0, the lab environment) for an image whose value at (x, y, channel) is
    /// `(x * a + y * b + channel * c) mod 256`.
    #[test]
    fn subject_input_image_matches_opencv_resize_bit_for_bit() {
        let image = |width: usize, height: usize, (a, b, c): (usize, usize, usize)| -> Vec<u8> {
            (0..height)
                .flat_map(|y| {
                    (0..width).flat_map(move |x| {
                        (0..3).map(move |channel| ((x * a + y * b + channel * c) % 256) as u8)
                    })
                })
                .collect()
        };
        let pixel = |resized: &SubjectInputImage, x: usize, y: usize| -> Vec<u8> {
            resized.rgb8[(y * resized.width + x) * 3..][..3].to_vec()
        };
        let sum = |resized: &SubjectInputImage| -> u64 {
            resized.rgb8.iter().map(|value| u64::from(*value)).sum()
        };
        // Upscale of a 7 x 5 image: the first and last rows read clamped rows with the weights of
        // positions outside the image.
        let up = subject_input_image(&image(7, 5, (37, 91, 53)), 7, 5).expect("upscale");
        assert_eq!((up.width, up.height), (1024, 731));
        assert_eq!(sum(&up), 289_430_270);
        assert_eq!(pixel(&up, 0, 0), [0, 53, 106]);
        assert_eq!(pixel(&up, 511, 0), [111, 164, 217]);
        assert_eq!(pixel(&up, 700, 365), [85, 138, 191]);
        assert_eq!(pixel(&up, 1023, 730), [74, 127, 180]);
        assert_eq!(pixel(&up, 3, 729), [108, 161, 214]);
        // Downscale of a 900 x 1300 image.
        let down = subject_input_image(&image(900, 1300, (13, 7, 101)), 900, 1300).expect("down");
        assert_eq!((down.width, down.height), (709, 1024));
        assert_eq!(sum(&down), 277_420_315);
        assert_eq!(pixel(&down, 0, 0), [3, 104, 205]);
        assert_eq!(pixel(&down, 708, 1023), [41, 142, 243]);
        assert_eq!(pixel(&down, 333, 500), [213, 58, 159]);
    }

    #[test]
    fn subject_pixel_values_pad_center_and_normalize() {
        let image = SubjectInputImage {
            width: 1024,
            height: 2,
            rgb8: vec![255; 1024 * 2 * 3],
        };
        let tensor = subject_pixel_values(&image).expect("pixel values");
        assert_eq!(tensor.shape(), &SUBJECT_PIXEL_VALUES_SHAPE);
        let size = SUBJECT_IMAGE_SIZE;
        let red = &tensor.data()[..size * size];
        let padding = ((0.0 - 0.485f64) / 0.229) as f32;
        let white = ((1.0 - 0.485f64) / 0.229) as f32;
        // Top padding (1024 - 2) / 2 = 511 rows.
        assert_eq!(red[510 * size], padding);
        assert_eq!(red[511 * size], white);
        assert_eq!(red[512 * size + 1023], white);
        assert_eq!(red[513 * size], padding);
    }

    #[test]
    fn select_subject_alpha_takes_best_score_and_removes_padding() {
        let size = SUBJECT_IMAGE_SIZE;
        let image = SubjectInputImage {
            width: 1024,
            height: 4,
            rgb8: vec![0; 1024 * 4 * 3],
        };
        let mut masks = vec![0.0f32; 3 * size * size];
        // Mask 1: 0.8 on its unpadded rows (510 to 513).
        masks[size * size + 510 * size..size * size + 514 * size].fill(0.8);
        let output = SubjectOutput {
            masks: TensorF32::new(SUBJECT_MASKS_SHAPE.to_vec(), masks).unwrap(),
            iou_scores: TensorF32::new(SUBJECT_SCORES_SHAPE.to_vec(), vec![0.5, 0.9, 0.9]).unwrap(),
        };
        let alpha = select_subject_alpha(&output, &image).expect("alpha");
        assert_eq!((alpha.width, alpha.height), (1024, 4));
        assert_eq!(alpha.score, 0.9);
        assert!(alpha.values.iter().all(|value| *value == 0.8));
        assert_eq!(alpha_area(&alpha.values), 4096);
        assert_eq!(alpha_area(&[0.49, 0.2]), 0);
    }

    /// A person box with normalized corners (head grid cells / 64).
    fn normalized_person(x0: f32, y0: f32, x1: f32, y1: f32) -> PersonBox {
        let grid = HEAD_GRID as f32;
        PersonBox {
            x0: x0 * grid,
            y0: y0 * grid,
            x1: x1 * grid,
            y1: y1 * grid,
            score: 0.9,
        }
    }

    /// An RGB8 image whose pixels differ: (x mod 251, y mod 251, (x + y) mod 251).
    fn pattern_image(width: usize, height: usize) -> Vec<u8> {
        (0..height)
            .flat_map(|y| {
                (0..width)
                    .flat_map(move |x| [(x % 251) as u8, (y % 251) as u8, ((x + y) % 251) as u8])
            })
            .collect()
    }

    #[test]
    fn person_crop_rect_adds_margin_and_clips_to_image() {
        let rect = person_crop_rect(&normalized_person(0.2, 0.3, 0.4, 0.7), 1000, 500);
        assert_eq!(
            rect,
            PixelRect {
                x0: 180,
                y0: 130,
                x1: 420,
                y1: 370
            }
        );
        // A box at the left edge starts at x 0; a box past the right edge ends at the width.
        let left = person_crop_rect(&normalized_person(-0.05, 0.3, 0.1, 0.7), 1000, 500);
        assert_eq!((left.x0, left.x1), (0, 110));
        let right = person_crop_rect(&normalized_person(0.8, 0.3, 1.2, 0.7), 1000, 500);
        assert_eq!((right.x0, right.x1), (780, 1000));
    }

    #[test]
    fn person_crop_rect_is_at_least_one_pixel() {
        let rect = person_crop_rect(&normalized_person(0.5, 0.2, 0.5, 0.2), 1000, 500);
        assert_eq!((rect.width(), rect.height()), (1, 1));
        assert_eq!((rect.x0, rect.y0), (500, 100));
        // A zero-width box on the right edge stays inside the image.
        let edge = person_crop_rect(&normalized_person(1.0, 0.2, 1.0, 0.4), 1000, 500);
        assert_eq!((edge.x0, edge.x1), (999, 1000));
    }

    #[test]
    fn person_crop_image_keeps_small_crops_and_resizes_large_crops() {
        let (width, height) = (2500, 3500);
        let image = pattern_image(width, height);
        let cut = |rect: PixelRect| -> Vec<u8> {
            (rect.y0..rect.y1)
                .flat_map(|y| image[(y * width + rect.x0) * 3..(y * width + rect.x1) * 3].to_vec())
                .collect()
        };
        let small = PixelRect {
            x0: 100,
            y0: 200,
            x1: 400,
            y1: 800,
        };
        let stored = person_crop_image(&image, width, height, small).expect("small crop");
        assert_eq!((stored.width, stored.height), (300, 600));
        assert_eq!(stored.rgb8, cut(small));

        let large = PixelRect {
            x0: 250,
            y0: 300,
            x1: 2250,
            y1: 3300,
        };
        let stored = person_crop_image(&image, width, height, large).expect("large crop");
        assert_eq!((stored.width, stored.height), (683, 1024));
        assert_eq!(
            stored,
            subject_input_image(&cut(large), 2000, 3000).expect("resize")
        );
        let outside = PixelRect {
            x0: 2400,
            y0: 0,
            x1: 2600,
            y1: 10,
        };
        assert!(person_crop_image(&image, width, height, outside).is_err());
    }

    #[test]
    fn subject_input_of_stored_crop_equals_one_resize() {
        let (width, height) = (2000, 3000);
        let crop = pattern_image(width, height);
        let rect = PixelRect {
            x0: 0,
            y0: 0,
            x1: width,
            y1: height,
        };
        let stored = person_crop_image(&crop, width, height, rect).expect("stored crop");
        let from_stored =
            subject_input_image(&stored.rgb8, stored.width, stored.height).expect("resize");
        let once = subject_input_image(&crop, width, height).expect("one resize");
        assert_eq!(from_stored.rgb8, once.rgb8);
        assert_eq!((from_stored.width, from_stored.height), (683, 1024));
    }

    #[test]
    fn subject_fraction_counts_alpha_inside_box() {
        // A 64 x 64 alpha: one alpha pixel per head grid cell. The box covers the columns 10 to
        // 19 and the rows 10 to 29 (10 x 20 pixels); the alpha is 1 on the columns 10 to 19 and
        // the rows 10 to 19 (the inside square), 0.49 elsewhere.
        let mut values = vec![0.49f32; 64 * 64];
        for y in 10..20 {
            values[y * 64 + 10..y * 64 + 20].fill(1.0);
        }
        let alpha = SubjectAlpha {
            width: 64,
            height: 64,
            values,
            score: 0.9,
        };
        let person = PersonBox {
            x0: 10.0,
            y0: 10.0,
            x1: 20.0,
            y1: 30.0,
            score: 0.9,
        };
        let fraction = subject_fraction(&alpha, &person);
        assert_eq!(fraction, 0.5);
        assert!(fraction >= PERSON_IN_SUBJECT_FRACTION);
        // A box outside the inside square: no alpha >= 0.5, so not in the Subject.
        let outside = PersonBox {
            x0: 40.0,
            y0: 40.0,
            x1: 50.0,
            y1: 50.0,
            score: 0.9,
        };
        let fraction = subject_fraction(&alpha, &outside);
        assert_eq!(fraction, 0.0);
        assert!(fraction < PERSON_IN_SUBJECT_FRACTION);
    }
}
