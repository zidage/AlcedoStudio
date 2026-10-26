//! Port check of the segmentation engine (SM3.A.2 of the AI Segmentation Mask plan).
//!
//! The engine runs on the pixels and prompts of a reference directory, and its masks are compared
//! with the masks of `scripts/ai_models/sam2/make_segmentation_port_reference.py` (PyTorch
//! preprocessing, ONNX Runtime 1.31 CPU, the lab MVP selection rules). For every prompt, the
//! number of masks, the token of each mask, and the selected index must be equal, and the IoU of
//! each mask (logits > 0 after bilinear upsampling to the photo size) must be at least 0.999. The
//! test also prints the model load, encoder, and decoder times.
//!
//! Image embedding cache (SM3.B): every photo starts with its name as the image key. After all
//! photos, the photos that the cache keeps (the last 4) start again with the key only and decode
//! their first prompt; the masks must match the reference with the same rules. The first photo
//! has left the cache, so its key-only start must fail with `ImageNotCached`.
//!
//! Environment:
//! - `ALCEDO_SEGMENTATION_MODEL_DIR`: the model directory of the platform (SM3.A plan, 2.1);
//! - `ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR`: the output directory of the reference script;
//! - `ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE`: optional device value (`""` is the platform default).
//!
//! Task masks (SM3.C and SM3.D): when the reference has task masks, every photo also runs Sky,
//! People, Person (each listed person), Subject, and Background. The People list length and the
//! list boxes (within 1e-3 cell) must match; the Subject and Background alpha >= 0.5 must have an IoU of at least 0.998 on the S3OD grid; the People alpha
//! must equal the Subject alpha. For each person, the crop rectangle must be equal in pixels, the
//! crop alpha >= 0.5 must have an IoU of at least 0.998 on the crop grid, `subject_fraction` must
//! be within 0.01, and `in_subject` must be equal when the reference fraction is more than 0.01
//! from `PERSON_IN_SUBJECT_FRACTION`. On DirectML and Core ML `All` (S3OD in float16 on the GPU or
//! the Neural Engine), a crop alpha passes with an IoU of at least 0.99, or with at most 0.05 % of
//! its grid pixels different. A key-only start of a cached photo must read the stored
//! Subject result and give the same person alphas.
//!
//! Sky (SM3.E): Sky is compared with the sky head reference (`tasks.sky_head`). "Found" must be
//! equal unless the reference sky share is within `SKY_FOUND_SHARE_MARGIN` of `SKY_FOUND_SHARE`. A found sky alpha must have the
//! 1024 x 1024 grid and the whole-image box, an IoU of its values >= 0.5 of at least
//! `MIN_SKY_IOU`, and a largest alpha difference of at most `MAX_SKY_ALPHA_DIFFERENCE`.
//!
//! The test returns without a check when one of the first two variables is not set.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Instant;

use alcedo_mind::service::segmentation::engine::{
    IMAGE_EMBEDDING_CACHE_CAPACITY, MaskBox, RgbImageView, SegmentationEngine, SegmentationError,
    SegmentationTask, StartSessionRequest, TaskMaskValues, TaskRequest, TaskResult,
};
use alcedo_mind::service::segmentation::model_set::{HEAD_GRID, LOGIT_SIZE, SKY_ALPHA_SIZE};
use alcedo_mind::service::segmentation::platform_backend_factory;
use alcedo_mind::service::segmentation::sam2::{
    PointLabel, PromptBox, PromptPoint, SegmentationPrompt,
};
use alcedo_mind::service::segmentation::task_masks::{
    self, PERSON_IN_SUBJECT_FRACTION, PersonBox, SKY_FOUND_SHARE,
};
use serde_json::Value;

const MIN_PHOTO_IOU: f64 = 0.999;
/// Smallest IoU of the Subject and Background alpha >= 0.5 on the S3OD grid (the lab macOS
/// acceptance criterion for S3OD).
const MIN_SUBJECT_IOU: f64 = 0.998;
/// Smallest IoU of a person crop alpha >= 0.5 on the crop grid when S3OD runs in float16 on the GPU
/// or the Neural Engine (DirectML; Core ML `ComputeUnits::All`). Small person crops are upscaled
/// to 1024 px, and their alpha is near 0.5 on wide edges: onnxruntime-directml in Python on the
/// same inputs gives the same IoUs as the sidecar (SM3.D plan, section 12). The CPU devices keep
/// `MIN_SUBJECT_IOU`.
const MIN_PERSON_IOU_FLOAT16_DEVICE: f64 = 0.99;
/// On the float16 devices, a person crop alpha also passes when at most this share of its grid
/// pixels differs from the reference at 0.5. The IoU of a small mask changes with a few pixels: on
/// DirectML, `DSC_1535` person 5 has 1,719 inside pixels and 82 different pixels (0.022 %), IoU
/// 0.954, also in Python.
const MAX_PERSON_DIFFERENT_SHARE_FLOAT16_DEVICE: f64 = 0.0005;
/// Largest difference of a People list box corner, in head grid cells.
const MAX_BOX_DIFFERENCE_CELLS: f32 = 1e-3;
/// Largest difference of `subject_fraction` (SM3.D plan, section 8.3).
const MAX_FRACTION_DIFFERENCE: f32 = 0.01;
/// Smallest IoU of the sky head alpha >= 0.5 on the 1024 x 1024 grid against the reference. The
/// first runs gave 1.0 on all 12 photos with sky, on DirectML and on the CPU (SM3.E plan, section
/// 9.2).
const MIN_SKY_IOU: f64 = 0.999;
/// Largest difference of one sky head alpha value against the reference: the export tolerance of
/// the sky head that the user accepted (lab report, section 6). The first runs gave at most 3.1e-4
/// on the CPU and 3.9e-4 on DirectML.
const MAX_SKY_ALPHA_DIFFERENCE: f32 = 1e-3;
/// "Found" may differ from the reference when the reference sky share is this close to
/// `SKY_FOUND_SHARE`.
const SKY_FOUND_SHARE_MARGIN: f32 = 1e-4;
/// Number of decodes of one prompt on one session for the steady-state decode time.
const REPEATED_DECODES: usize = 20;

fn read_f32(path: &Path) -> Vec<f32> {
    let bytes = std::fs::read(path).unwrap_or_else(|err| panic!("{}: {err}", path.display()));
    bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
}

/// Taps of a bilinear upsampling with half-pixel centers (PyTorch `align_corners=False`, OpenCV
/// `INTER_LINEAR`): for each output index, the two inputs and the weight of the second.
fn upsample_taps(input: usize, output: usize) -> Vec<(usize, usize, f32)> {
    let scale = input as f32 / output as f32;
    (0..output)
        .map(|index| {
            let source = ((index as f32 + 0.5) * scale - 0.5).max(0.0);
            let low = (source.floor() as usize).min(input - 1);
            let high = (low + 1).min(input - 1);
            (low, high, source - low as f32)
        })
        .collect()
}

/// Inside flags (logit > 0) of 256 x 256 logits upsampled to width x height.
fn mask_at_photo_size(logits: &[f32], width: usize, height: usize) -> Vec<bool> {
    let columns = upsample_taps(LOGIT_SIZE, width);
    let rows = upsample_taps(LOGIT_SIZE, height);
    let mut mask = Vec::with_capacity(width * height);
    let mut row_values = vec![0.0f32; LOGIT_SIZE];
    for &(top, bottom, weight_y) in &rows {
        for (x, value) in row_values.iter_mut().enumerate() {
            let a = logits[top * LOGIT_SIZE + x];
            let b = logits[bottom * LOGIT_SIZE + x];
            *value = a + (b - a) * weight_y;
        }
        for &(left, right, weight_x) in &columns {
            let a = row_values[left];
            let b = row_values[right];
            mask.push(a + (b - a) * weight_x > 0.0);
        }
    }
    mask
}

fn iou(a: &[bool], b: &[bool]) -> f64 {
    let (mut intersection, mut union) = (0u64, 0u64);
    for (x, y) in a.iter().zip(b) {
        intersection += u64::from(*x && *y);
        union += u64::from(*x || *y);
    }
    if union == 0 {
        1.0
    } else {
        intersection as f64 / union as f64
    }
}

fn prompt_from(record: &Value, reference_dir: &Path) -> SegmentationPrompt {
    let points = record["points"]
        .as_array()
        .expect("points")
        .iter()
        .map(|point| PromptPoint {
            x: point[0].as_f64().expect("x") as f32,
            y: point[1].as_f64().expect("y") as f32,
            label: if point[2].as_i64() == Some(0) {
                PointLabel::Subtract
            } else {
                PointLabel::Add
            },
        })
        .collect();
    let prompt_box = record["box"].as_array().map(|corners| PromptBox {
        x0: corners[0].as_f64().expect("x0") as f32,
        y0: corners[1].as_f64().expect("y0") as f32,
        x1: corners[2].as_f64().expect("x1") as f32,
        y1: corners[3].as_f64().expect("y1") as f32,
    });
    let mask_input = record["mask_input"]
        .as_str()
        .map(|file| read_f32(&reference_dir.join(file)))
        .unwrap_or_default();
    SegmentationPrompt {
        points,
        prompt_box,
        mask_input,
    }
}

fn median(values: &mut [u64]) -> u64 {
    values.sort_unstable();
    values[values.len() / 2]
}

/// Results of the mask comparisons of the whole check.
struct Comparison {
    failures: Vec<String>,
    minimum_iou: f64,
    compared: usize,
    /// Smallest IoU of a person crop alpha for the S3OD device of this run.
    minimum_person_iou: f64,
    /// Largest share of different person crop alpha pixels that passes below the smallest IoU; 0
    /// on the CPU devices.
    maximum_person_different_share: f64,
    /// Sky head: the smallest IoU and the largest alpha difference of the compared photos.
    minimum_sky_iou: f64,
    maximum_sky_alpha_difference: f32,
    sky_alphas_compared: usize,
}

/// Decodes @p record on @p session_id and compares the masks with the reference. Returns the
/// time of the engine decode call in microseconds and the decoder point count.
fn decode_and_compare(
    engine: &SegmentationEngine,
    session_id: &str,
    record: &Value,
    reference_dir: &Path,
    (width, height): (usize, usize),
    label: &str,
    comparison: &mut Comparison,
) -> (u64, usize) {
    let prompt = prompt_from(record, reference_dir);
    let point_count = prompt.points.len() + if prompt.prompt_box.is_some() { 2 } else { 0 };
    let started = Instant::now();
    let result = engine
        .decode(session_id, prompt)
        .unwrap_or_else(|err| panic!("{label}: {err}"));
    let decode_us = started.elapsed().as_micros() as u64;
    let reference_masks = record["masks"].as_array().expect("masks");
    if reference_masks.len() != result.masks.masks.len() {
        comparison.failures.push(format!(
            "{label}: {} masks, reference {}",
            result.masks.masks.len(),
            reference_masks.len()
        ));
        return (decode_us, point_count);
    }
    if record["selected_index"].as_u64() != Some(result.masks.selected_index as u64) {
        comparison.failures.push(format!(
            "{label}: selected index {}, reference {}",
            result.masks.selected_index, record["selected_index"]
        ));
    }
    for (level, reference) in result.masks.masks.iter().zip(reference_masks) {
        if reference["token"].as_u64() != Some(u64::from(level.token)) {
            comparison.failures.push(format!(
                "{label}: token {}, reference {}",
                level.token, reference["token"]
            ));
            continue;
        }
        let reference_logits =
            read_f32(&reference_dir.join(reference["logits"].as_str().expect("logits")));
        let max_difference = level
            .logits
            .iter()
            .zip(&reference_logits)
            .map(|(a, b)| (a - b).abs())
            .fold(0.0f32, f32::max);
        let photo_iou = iou(
            &mask_at_photo_size(&level.logits, width, height),
            &mask_at_photo_size(&reference_logits, width, height),
        );
        comparison.compared += 1;
        comparison.minimum_iou = comparison.minimum_iou.min(photo_iou);
        println!(
            "{label} token {}: photo IoU {photo_iou:.6}, max |logit difference| {max_difference:.5}",
            level.token
        );
        if photo_iou < MIN_PHOTO_IOU {
            comparison.failures.push(format!(
                "{label} token {}: photo IoU {photo_iou:.6}",
                level.token
            ));
        }
    }
    (decode_us, point_count)
}

/// Times and sizes of the task requests of the whole check; times in milliseconds.
#[derive(Default)]
struct TaskTimes {
    /// The first Sky request of each photo: the sky head run.
    first_sky_ms: Vec<u64>,
    /// The first People request of each photo with people: S3OD on the photo with its pre- and
    /// postprocessing.
    first_people_ms: Vec<u64>,
    /// The first Person request of each listed person: S3OD on the crop.
    first_person_ms: Vec<u64>,
    /// The Subject request of each photo: S3OD on the photo, or 0 after a People request.
    first_subject_ms: Vec<u64>,
    /// Requests that read the stored results only, in microseconds.
    stored_request_us: Vec<u64>,
    /// Per photo with people: the CPU time of the person crops (the crop functions of the engine
    /// on the reference boxes), in microseconds, and the bytes of the stored crops.
    crop_us: Vec<(String, u64, usize, usize)>,
}

fn run_task(
    engine: &SegmentationEngine,
    session_id: &str,
    task: SegmentationTask,
    person_index: Option<u32>,
    label: &str,
) -> TaskResult {
    engine
        .run_task(session_id, TaskRequest { task, person_index })
        .unwrap_or_else(|err| panic!("{label} {task:?}: {err}"))
}

/// The alpha values and the mask box of an alpha task mask.
fn alpha_values(result: &TaskResult, label: &str) -> (Vec<f32>, MaskBox) {
    let Some(mask) = &result.mask else {
        panic!("{label}: no mask");
    };
    let TaskMaskValues::Alpha { values, mask_box } = &mask.values;
    (values.clone(), *mask_box)
}

/// The alpha values of the Person result of each listed person of each photo (`None`: not found).
type PersonAlphas = BTreeMap<String, Vec<Option<Vec<f32>>>>;

/// Compares the sky head result @p sky with the sky head reference @p reference (SM3.E plan).
fn compare_sky_alpha(
    sky: &TaskResult,
    reference: &Value,
    reference_dir: &Path,
    name: &str,
    comparison: &mut Comparison,
) {
    let reference_share = reference["share"].as_f64().expect("sky share") as f32;
    let reference_found = reference["found"].as_bool().expect("sky found");
    println!(
        "{name} sky head: share {:.6} (reference {reference_share:.6}), found {} (reference {reference_found}), {} ms, {}",
        sky.presence,
        sky.mask.is_some(),
        sky.model_ms,
        sky.message
    );
    if sky.mask.is_some() != reference_found {
        if (reference_share - SKY_FOUND_SHARE).abs() > SKY_FOUND_SHARE_MARGIN {
            comparison.failures.push(format!(
                "{name} sky head: found {}, reference {reference_found}",
                sky.mask.is_some()
            ));
        }
        return;
    }
    let Some(mask) = &sky.mask else {
        return;
    };
    let label = format!("{name} sky head");
    assert_eq!(
        (mask.width as usize, mask.height as usize),
        (SKY_ALPHA_SIZE, SKY_ALPHA_SIZE),
        "{label}: the sky alpha grid"
    );
    let (values, mask_box) = alpha_values(sky, &label);
    assert_eq!(mask_box, MaskBox::FULL_IMAGE, "{label}: the sky alpha box");
    let reference_alpha =
        read_f32(&reference_dir.join(reference["alpha"].as_str().expect("sky alpha")));
    let inside = |alpha: &[f32]| -> Vec<bool> {
        alpha
            .iter()
            .map(|value| *value >= task_masks::ALPHA_INSIDE)
            .collect()
    };
    let grid_iou = iou(&inside(&values), &inside(&reference_alpha));
    let differences: Vec<f32> = values
        .iter()
        .zip(&reference_alpha)
        .map(|(value, reference)| (value - reference).abs())
        .collect();
    let max_difference = differences.iter().copied().fold(0.0f32, f32::max);
    let mean_difference =
        differences.iter().map(|d| f64::from(*d)).sum::<f64>() / differences.len() as f64;
    let above_one_level = differences.iter().filter(|d| **d > 1.0 / 255.0).count();
    println!(
        "{label}: grid IoU {grid_iou:.6}, max |alpha difference| {max_difference:.5}, mean {mean_difference:.6}, {above_one_level} values above 1/255"
    );
    comparison.sky_alphas_compared += 1;
    comparison.minimum_sky_iou = comparison.minimum_sky_iou.min(grid_iou);
    comparison.maximum_sky_alpha_difference =
        comparison.maximum_sky_alpha_difference.max(max_difference);
    if grid_iou < MIN_SKY_IOU || max_difference > MAX_SKY_ALPHA_DIFFERENCE {
        comparison.failures.push(format!(
            "{label}: grid IoU {grid_iou:.6}, max |alpha difference| {max_difference:.5}"
        ));
    }
}

/// Runs the task masks on @p session_id and compares them with the task reference of @p photo
/// (SM3.C plan, section 8; SM3.D plan, section 8.3; SM3.E plan).
#[allow(clippy::too_many_arguments)]
fn compare_tasks(
    engine: &SegmentationEngine,
    session_id: &str,
    photo: &Value,
    reference_dir: &Path,
    size: (usize, usize),
    comparison: &mut Comparison,
    times: &mut TaskTimes,
    person_alphas: &mut PersonAlphas,
) {
    let name = photo["name"].as_str().expect("name");
    let tasks = &photo["tasks"];

    // Sky.
    let sky = run_task(engine, session_id, SegmentationTask::Sky, None, name);
    times.first_sky_ms.push(sky.model_ms);
    let reference = &tasks["sky_head"];
    assert!(
        reference.is_object(),
        "{name}: the reference has no sky head result"
    );
    compare_sky_alpha(&sky, reference, reference_dir, name, comparison);

    // People (SM3.D plan, section 8.3).
    let people = run_task(engine, session_id, SegmentationTask::People, None, name);
    let reference_people = tasks["people"]["people"].as_array().expect("people");
    println!(
        "{name} people: presence {:.4} (reference {:.4}), {} listed (reference {}), {} ms, {}",
        people.presence,
        tasks["person_presence"].as_f64().expect("person presence"),
        people.people.len(),
        reference_people.len(),
        people.model_ms,
        people.message
    );
    let mut alphas = Vec::new();
    if people.people.len() != reference_people.len() {
        comparison.failures.push(format!(
            "{name} people: {} listed, reference {}",
            people.people.len(),
            reference_people.len()
        ));
    } else if reference_people.is_empty() {
        if people.mask.is_some() {
            comparison
                .failures
                .push(format!("{name} people: a mask, reference not found"));
        }
    } else {
        times.first_people_ms.push(people.model_ms);
        let pixels = std::fs::read(reference_dir.join(photo["pixels"].as_str().expect("pixels")))
            .expect("pixels");
        let mut crop_us = 0u64;
        let (mut crop_bytes, mut largest_crop) = (0usize, 0usize);
        for (index, (entry, reference)) in people.people.iter().zip(reference_people).enumerate() {
            let label = format!("{name} person {index}");
            let cells: Vec<f32> = reference["box_cells"]
                .as_array()
                .expect("box cells")
                .iter()
                .map(|value| value.as_f64().expect("cell") as f32)
                .collect();
            let grid = HEAD_GRID as f32;
            let difference = [entry.x0, entry.y0, entry.x1, entry.y1]
                .iter()
                .zip(&cells)
                .map(|(value, cell)| (value * grid - cell.clamp(0.0, grid)).abs())
                .fold(0.0f32, f32::max);
            if difference > MAX_BOX_DIFFERENCE_CELLS {
                comparison
                    .failures
                    .push(format!("{label}: box differs by {difference} cells"));
            }
            // The crop CPU work of the start, with the crop functions on the reference box.
            let reference_box = PersonBox {
                x0: cells[0],
                y0: cells[1],
                x1: cells[2],
                y1: cells[3],
                score: 0.0,
            };
            let started = Instant::now();
            let rect = task_masks::person_crop_rect(&reference_box, size.0, size.1);
            let crop = task_masks::person_crop_image(&pixels, size.0, size.1, rect).expect("crop");
            crop_us += started.elapsed().as_micros() as u64;
            crop_bytes += crop.rgb8.len();
            largest_crop = largest_crop.max(crop.rgb8.len());

            let reference_fraction =
                reference["subject_fraction"].as_f64().expect("fraction") as f32;
            let reference_in_subject = reference["in_subject"].as_bool().expect("in_subject");
            let fraction_difference = (entry.subject_fraction - reference_fraction).abs();
            if fraction_difference > MAX_FRACTION_DIFFERENCE {
                comparison.failures.push(format!(
                    "{label}: subject_fraction {:.4}, reference {reference_fraction:.4}",
                    entry.subject_fraction
                ));
            }
            if (reference_fraction - PERSON_IN_SUBJECT_FRACTION).abs() > MAX_FRACTION_DIFFERENCE
                && entry.in_subject != reference_in_subject
            {
                comparison.failures.push(format!(
                    "{label}: in_subject {}, reference {reference_in_subject}",
                    entry.in_subject
                ));
            }

            let one = run_task(
                engine,
                session_id,
                SegmentationTask::Person,
                Some(index as u32),
                name,
            );
            times.first_person_ms.push(one.model_ms);
            let started = Instant::now();
            let stored = run_task(
                engine,
                session_id,
                SegmentationTask::Person,
                Some(index as u32),
                name,
            );
            times
                .stored_request_us
                .push(started.elapsed().as_micros() as u64);
            if stored.model_ms != 0 {
                comparison
                    .failures
                    .push(format!("{label}: the second request ran a model"));
            }
            let reference_rect: Vec<usize> = reference["rect"]
                .as_array()
                .expect("rect")
                .iter()
                .map(|value| value.as_u64().expect("rect value") as usize)
                .collect();
            let reference_alpha =
                read_f32(&reference_dir.join(reference["alpha"].as_str().expect("alpha")));
            let reference_inside: Vec<bool> =
                reference_alpha.iter().map(|value| *value >= 0.5).collect();
            let reference_found = reference_inside.iter().any(|inside| *inside);
            let Some(mask) = &one.mask else {
                println!(
                    "{label}: not found (reference found {reference_found}), {} ms, {}",
                    one.model_ms, one.message
                );
                if reference_found {
                    comparison
                        .failures
                        .push(format!("{label}: not found, reference found"));
                }
                alphas.push(None);
                continue;
            };
            let (values, mask_box) = alpha_values(&one, &label);
            let rect = [
                (mask_box.x0 * size.0 as f32).round() as usize,
                (mask_box.y0 * size.1 as f32).round() as usize,
                (mask_box.x1 * size.0 as f32).round() as usize,
                (mask_box.y1 * size.1 as f32).round() as usize,
            ];
            let reference_size = (
                reference["alpha_width"].as_u64().expect("alpha width") as u32,
                reference["alpha_height"].as_u64().expect("alpha height") as u32,
            );
            if rect.as_slice() != reference_rect.as_slice()
                || (mask.width, mask.height) != reference_size
            {
                comparison.failures.push(format!(
                    "{label}: crop {rect:?} grid {}x{}, reference {reference_rect:?} grid {}x{}",
                    mask.width, mask.height, reference_size.0, reference_size.1
                ));
                alphas.push(Some(values));
                continue;
            }
            let inside: Vec<bool> = values.iter().map(|value| *value >= 0.5).collect();
            let grid_iou = iou(&inside, &reference_inside);
            comparison.compared += 1;
            let max_difference = values
                .iter()
                .zip(&reference_alpha)
                .map(|(value, reference)| (value - reference).abs())
                .fold(0.0f32, f32::max);
            println!(
                "{label}: crop {rect:?}, grid IoU {grid_iou:.6}, max |alpha difference| {max_difference:.4}, subject_fraction {:.4} (reference {reference_fraction:.4}), in_subject {}, score {:.4} (reference {:.4}), {} ms",
                entry.subject_fraction,
                entry.in_subject,
                one.subject_score,
                reference["alpha_score"].as_f64().expect("alpha score"),
                one.model_ms
            );
            let different_share = inside
                .iter()
                .zip(&reference_inside)
                .filter(|(a, b)| a != b)
                .count() as f64
                / inside.len() as f64;
            if (grid_iou < comparison.minimum_person_iou
                && different_share > comparison.maximum_person_different_share)
                || !reference_found
            {
                comparison.failures.push(format!(
                    "{label}: grid IoU {grid_iou:.6}, {:.4} % different, reference found {reference_found}",
                    different_share * 100.0
                ));
            }
            alphas.push(Some(values));
        }
        times
            .crop_us
            .push((name.to_string(), crop_us, crop_bytes, largest_crop));
    }
    person_alphas.insert(name.to_string(), alphas);

    // Subject and Background.
    let subject_reference = &tasks["subject"];
    let reference_alpha =
        read_f32(&reference_dir.join(subject_reference["alpha"].as_str().expect("alpha")));
    let reference_size = (
        subject_reference["width"].as_u64().expect("width") as u32,
        subject_reference["height"].as_u64().expect("height") as u32,
    );
    for task in [SegmentationTask::Subject, SegmentationTask::Background] {
        let result = run_task(engine, session_id, task, None, name);
        let label = format!("{name} {task:?}");
        if task == SegmentationTask::Subject
            && people.mask.is_some()
            && result.mask.is_some()
            && alpha_values(&people, &label).0 != alpha_values(&result, &label).0
        {
            comparison.failures.push(format!(
                "{label}: the People alpha differs from the Subject alpha"
            ));
        }
        let reference_inside: Vec<bool> = reference_alpha
            .iter()
            .map(|value| match task {
                SegmentationTask::Background => 1.0 - value >= 0.5,
                _ => *value >= 0.5,
            })
            .collect();
        let Some(mask) = &result.mask else {
            if reference_inside.iter().any(|inside| *inside) {
                comparison
                    .failures
                    .push(format!("{label}: not found, reference found"));
            }
            continue;
        };
        let (values, _) = alpha_values(&result, &label);
        if (mask.width, mask.height) != reference_size {
            comparison.failures.push(format!(
                "{label}: size {}x{}, reference {}x{}",
                mask.width, mask.height, reference_size.0, reference_size.1
            ));
            continue;
        }
        let inside: Vec<bool> = values.iter().map(|value| *value >= 0.5).collect();
        let grid_iou = iou(&inside, &reference_inside);
        let max_difference = values
            .iter()
            .zip(&reference_alpha)
            .map(|(value, reference)| match task {
                SegmentationTask::Background => (value - (1.0 - reference)).abs(),
                _ => (value - reference).abs(),
            })
            .fold(0.0f32, f32::max);
        println!(
            "{label}: grid IoU {grid_iou:.6}, max |alpha difference| {max_difference:.4}, score {:.4} (reference {:.4}), {} ms",
            result.subject_score,
            subject_reference["score"].as_f64().expect("score"),
            result.model_ms
        );
        if task == SegmentationTask::Subject {
            times.first_subject_ms.push(result.model_ms);
        }
        if grid_iou < MIN_SUBJECT_IOU {
            comparison
                .failures
                .push(format!("{label}: grid IoU {grid_iou:.6}"));
        }
    }
}

fn photo_size(photo: &Value) -> (usize, usize) {
    (
        photo["width"].as_u64().expect("width") as usize,
        photo["height"].as_u64().expect("height") as usize,
    )
}

#[test]
fn segmentation_port_check_matches_reference_masks() {
    let (Some(model_dir), Some(reference_dir)) = (
        std::env::var_os("ALCEDO_SEGMENTATION_MODEL_DIR").map(PathBuf::from),
        std::env::var_os("ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR").map(PathBuf::from),
    ) else {
        eprintln!(
            "segmentation port check not run: ALCEDO_SEGMENTATION_MODEL_DIR and ALCEDO_SEGMENTATION_PORT_REFERENCE_DIR are not set"
        );
        return;
    };
    let device = std::env::var("ALCEDO_SEGMENTATION_PORT_CHECK_DEVICE").unwrap_or_default();
    let manifest: Value = serde_json::from_slice(
        &std::fs::read(reference_dir.join("manifest.json")).expect("manifest.json"),
    )
    .expect("manifest JSON");

    let engine = Arc::new(SegmentationEngine::new(platform_backend_factory()));
    let load_started = Instant::now();
    engine.load(&model_dir, &device).expect("load models");
    let load_ms = load_started.elapsed().as_millis();
    let status = engine.status().expect("status");
    println!(
        "backend {} device {} load ms {load_ms} {:?}",
        status.backend,
        status.device,
        status
            .models
            .iter()
            .map(|model| (model.role.as_str(), model.load_ms))
            .collect::<Vec<_>>()
    );

    let photos = manifest["photos"].as_array().expect("photos");
    let subject_device = status
        .models
        .iter()
        .find(|model| model.role.as_str() == "subject")
        .map(|model| model.device.clone())
        .unwrap_or_default();
    let float16_device =
        subject_device.starts_with("directml") || subject_device == "coreml-native:all";
    let mut comparison = Comparison {
        failures: Vec::new(),
        minimum_iou: 1.0,
        compared: 0,
        minimum_person_iou: if float16_device {
            MIN_PERSON_IOU_FLOAT16_DEVICE
        } else {
            MIN_SUBJECT_IOU
        },
        maximum_person_different_share: if float16_device {
            MAX_PERSON_DIFFERENT_SHARE_FLOAT16_DEVICE
        } else {
            0.0
        },
        minimum_sky_iou: 1.0,
        maximum_sky_alpha_difference: 0.0,
        sky_alphas_compared: 0,
    };
    println!(
        "S3OD device {subject_device}: minimum person crop IoU {}, or at most {} % different pixels",
        comparison.minimum_person_iou,
        comparison.maximum_person_different_share * 100.0
    );
    let mut encoder_ms = Vec::new();
    let mut decode_us: BTreeMap<usize, Vec<u64>> = BTreeMap::new();
    let mut first_decode_us = None;
    let mut task_times = TaskTimes::default();
    let mut person_alphas = PersonAlphas::new();
    for photo in photos {
        let name = photo["name"].as_str().expect("name");
        let size = photo_size(photo);
        let pixels = std::fs::read(reference_dir.join(photo["pixels"].as_str().expect("pixels")))
            .expect("pixels");
        let session = engine
            .start_session(StartSessionRequest {
                image: Some(RgbImageView {
                    width: size.0 as u32,
                    height: size.1 as u32,
                    rgb8: &pixels,
                }),
                image_key: name,
            })
            .expect("start session");
        assert!(
            !session.from_cache,
            "{name}: the first start must run the encoder"
        );
        encoder_ms.push(session.encoder_ms);
        for record in photo["prompts"].as_array().expect("prompts") {
            let label = format!("{name} {}", record["name"].as_str().expect("prompt name"));
            let (microseconds, point_count) = decode_and_compare(
                &engine,
                &session.session_id,
                record,
                &reference_dir,
                size,
                &label,
                &mut comparison,
            );
            first_decode_us.get_or_insert(microseconds);
            decode_us.entry(point_count).or_default().push(microseconds);
        }
        if photo["tasks"].is_object() {
            compare_tasks(
                &engine,
                &session.session_id,
                photo,
                &reference_dir,
                size,
                &mut comparison,
                &mut task_times,
                &mut person_alphas,
            );
        }
        engine
            .end_session(&session.session_id)
            .expect("end session");
    }

    // Image embedding cache: the last photos are cached, the first photo has left the cache.
    assert!(photos.len() > IMAGE_EMBEDDING_CACHE_CAPACITY);
    let first_name = photos[0]["name"].as_str().expect("name");
    assert!(matches!(
        engine.start_session(StartSessionRequest {
            image: None,
            image_key: first_name,
        }),
        Err(SegmentationError::ImageNotCached(_))
    ));
    let mut cached_start_us = Vec::new();
    let mut repeated_decode_us = Vec::new();
    for photo in &photos[photos.len() - IMAGE_EMBEDDING_CACHE_CAPACITY..] {
        let name = photo["name"].as_str().expect("name");
        let started = Instant::now();
        let session = engine
            .start_session(StartSessionRequest {
                image: None,
                image_key: name,
            })
            .expect("cached start");
        cached_start_us.push(started.elapsed().as_micros() as u64);
        assert!(
            session.from_cache,
            "{name}: the key-only start must use the cache"
        );
        let record = &photo["prompts"][0];
        let label = format!(
            "{name} {} (cached)",
            record["name"].as_str().expect("prompt name")
        );
        decode_and_compare(
            &engine,
            &session.session_id,
            record,
            &reference_dir,
            photo_size(photo),
            &label,
            &mut comparison,
        );
        if photo["tasks"].is_object() {
            let started = Instant::now();
            let subject = run_task(
                &engine,
                &session.session_id,
                SegmentationTask::Subject,
                None,
                name,
            );
            task_times
                .stored_request_us
                .push(started.elapsed().as_micros() as u64);
            assert_eq!(
                subject.model_ms, 0,
                "{name}: the cached image must keep its Subject result"
            );
            // The key-only start gives the same person alphas from the stored crops.
            for (index, stored) in person_alphas[name].iter().enumerate() {
                let label = format!("{name} person {index} (cached)");
                let person = run_task(
                    &engine,
                    &session.session_id,
                    SegmentationTask::Person,
                    Some(index as u32),
                    &label,
                );
                let values = person
                    .mask
                    .as_ref()
                    .map(|_| alpha_values(&person, &label).0);
                if person.model_ms != 0 || values.as_ref() != stored.as_ref() {
                    comparison.failures.push(format!(
                        "{label}: the cached image gives another alpha or ran a model"
                    ));
                }
            }
        }
        // The same prompt on the same features: the decoder session keeps the features bound.
        if repeated_decode_us.is_empty() {
            for _ in 0..REPEATED_DECODES {
                let started = Instant::now();
                engine
                    .decode(&session.session_id, prompt_from(record, &reference_dir))
                    .expect("repeated decode");
                repeated_decode_us.push(started.elapsed().as_micros() as u64);
            }
        }
        engine
            .end_session(&session.session_id)
            .expect("end session");
    }

    println!(
        "compared masks {}, minimum photo IoU {:.6}, encoder ms p50 {} (first {}), first decode us {}",
        comparison.compared,
        comparison.minimum_iou,
        median(&mut encoder_ms.clone()),
        encoder_ms[0],
        first_decode_us.unwrap_or_default()
    );
    for (points, values) in &mut decode_us {
        println!(
            "decode us p50 with {points} decoder points: {} ({} calls)",
            median(values),
            values.len()
        );
    }
    println!(
        "cached start us p50 {} ({} starts); repeated decode us p50 {} ({} calls)",
        median(&mut cached_start_us),
        IMAGE_EMBEDDING_CACHE_CAPACITY,
        median(&mut repeated_decode_us),
        REPEATED_DECODES
    );
    if !task_times.first_subject_ms.is_empty() {
        println!(
            "first Sky request ms p50 {} (first photo {}); first Subject request ms p50 {} (first photo {}); stored-result request us p50 {} ({} requests)",
            median(&mut task_times.first_sky_ms.clone()),
            task_times.first_sky_ms[0],
            median(&mut task_times.first_subject_ms.clone()),
            task_times.first_subject_ms[0],
            median(&mut task_times.stored_request_us),
            task_times.stored_request_us.len()
        );
    }
    if !task_times.first_people_ms.is_empty() {
        println!(
            "first People request ms p50 {} ({} photos: {:?}); first Person request ms p50 {} ({} people, min {}, max {})",
            median(&mut task_times.first_people_ms.clone()),
            task_times.first_people_ms.len(),
            task_times.first_people_ms,
            median(&mut task_times.first_person_ms.clone()),
            task_times.first_person_ms.len(),
            task_times
                .first_person_ms
                .iter()
                .min()
                .copied()
                .unwrap_or_default(),
            task_times
                .first_person_ms
                .iter()
                .max()
                .copied()
                .unwrap_or_default()
        );
        for (name, crop_us, bytes, largest) in &task_times.crop_us {
            println!(
                "{name}: person crops {crop_us} us, {bytes} bytes stored, largest crop {largest} bytes"
            );
        }
    }
    println!(
        "sky head: {} alphas compared, minimum grid IoU {:.6}, max |alpha difference| {:.5}",
        comparison.sky_alphas_compared,
        comparison.minimum_sky_iou,
        comparison.maximum_sky_alpha_difference
    );
    engine.unload().expect("unload");
    assert!(
        comparison.failures.is_empty(),
        "port check failures:\n{}",
        comparison.failures.join("\n")
    );
}
