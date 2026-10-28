//! Files, tensor names, and tensor shapes of the AI Mask model set (master plan section 8.2.1).

use anyhow::{Result, anyhow, bail};
use serde_json::Value;

use super::backend::SegmentationModelRole;

/// Identity of the model set and the directory name that SM9 and SM2 use.
pub const MODEL_SET_ID: &str = "alcedo-ai-mask-v1";
/// Package ID of the AI Mask model set in `model_set.json` (AI Models plan, decisions P1 and P3).
pub const MODEL_SET_PACKAGE_ID: &str = "alcedo-ai-mask";
/// The file that identifies an installed model set and its version (AI Models plan, AM1).
pub const MODEL_SET_FILE: &str = "model_set.json";
/// AI Mask model set versions that this sidecar can run (AI Models plan, decision P1). The host
/// declares the same range (`kAiMaskModelSetMinimumVersion`, `kAiMaskModelSetMaximumVersion`).
pub const MODEL_SET_MINIMUM_VERSION: u64 = 1;
pub const MODEL_SET_MAXIMUM_VERSION: u64 = 1;
/// Side of the square encoder input. Prompt coordinates are normalized coordinates times this.
pub const IMAGE_SIZE: usize = 1024;
/// Side of the square decoder logits and of the mask input.
pub const LOGIT_SIZE: usize = 256;
/// Number of mask tokens of the decoder output.
pub const MASK_TOKEN_COUNT: usize = 4;
/// Largest decoder point count (box corners included); the range of the Core ML decoder.
pub const MAX_DECODER_POINTS: usize = 64;
/// Largest long edge of a session image.
pub const MAX_IMAGE_EDGE: u32 = 4096;

pub const PIXEL_VALUES_SHAPE: [usize; 4] = [1, 3, IMAGE_SIZE, IMAGE_SIZE];
pub const IMAGE_EMBED_SHAPE: [usize; 4] = [1, 256, 64, 64];
pub const HIGH_RES_FEATS_0_SHAPE: [usize; 4] = [1, 32, 256, 256];
pub const HIGH_RES_FEATS_1_SHAPE: [usize; 4] = [1, 64, 128, 128];
pub const MASK_INPUT_SHAPE: [usize; 4] = [1, 1, LOGIT_SIZE, LOGIT_SIZE];
pub const MASK_LOGITS_SHAPE: [usize; 4] = [1, MASK_TOKEN_COUNT, LOGIT_SIZE, LOGIT_SIZE];
pub const MASK_SCORES_SHAPE: [usize; 2] = [1, MASK_TOKEN_COUNT];

/// Side of the square cell grid of the people and sky head (the `image_embed` grid).
pub const HEAD_GRID: usize = 64;
/// Model-input pixels per head grid cell.
pub const HEAD_CELL_MODEL_PIXELS: usize = IMAGE_SIZE / HEAD_GRID;
pub const SKY_LOGITS_SHAPE: [usize; 4] = [1, 1, HEAD_GRID, HEAD_GRID];
pub const CENTER_LOGITS_SHAPE: [usize; 4] = [1, 1, HEAD_GRID, HEAD_GRID];
pub const CENTER_OFFSET_SHAPE: [usize; 4] = [1, 2, HEAD_GRID, HEAD_GRID];
pub const LOG_SIZE_SHAPE: [usize; 4] = [1, 2, HEAD_GRID, HEAD_GRID];
pub const PRESENCE_LOGITS_SHAPE: [usize; 2] = [1, 2];
/// File of the head thresholds (best F1 on the COCO validation set) in the model directory.
pub const HEAD_THRESHOLDS_FILE: &str = "people_sky_head.json";

/// Side of the square S3OD input.
pub const SUBJECT_IMAGE_SIZE: usize = 1024;
/// Number of S3OD masks and IoU scores.
pub const SUBJECT_MASK_COUNT: usize = 3;
pub const SUBJECT_PIXEL_VALUES_SHAPE: [usize; 4] = [1, 3, SUBJECT_IMAGE_SIZE, SUBJECT_IMAGE_SIZE];
pub const SUBJECT_MASKS_SHAPE: [usize; 4] = [
    1,
    SUBJECT_MASK_COUNT,
    SUBJECT_IMAGE_SIZE,
    SUBJECT_IMAGE_SIZE,
];
pub const SUBJECT_SCORES_SHAPE: [usize; 2] = [1, SUBJECT_MASK_COUNT];

/// Side of the square sky alpha of the sky head: the encoder input, stretched to 1024 x 1024.
pub const SKY_ALPHA_SIZE: usize = IMAGE_SIZE;
pub const SKY_ALPHA_SHAPE: [usize; 4] = [1, 1, SKY_ALPHA_SIZE, SKY_ALPHA_SIZE];

/// File and tensor names of one model. ONNX and Core ML models have the same input names; the
/// decoder output names differ.
#[derive(Debug)]
pub struct ModelSpec {
    pub role: SegmentationModelRole,
    pub onnx_file: &'static str,
    pub coreml_directory: &'static str,
    pub inputs: &'static [&'static str],
    pub onnx_outputs: &'static [&'static str],
    pub coreml_outputs: &'static [&'static str],
}

pub const ENCODER: ModelSpec = ModelSpec {
    role: SegmentationModelRole::Encoder,
    onnx_file: "point_encoder.onnx",
    coreml_directory: "point_encoder.mlmodelc",
    inputs: &["pixel_values"],
    onnx_outputs: &["image_embed", "high_res_feats_0", "high_res_feats_1"],
    coreml_outputs: &["image_embed", "high_res_feats_0", "high_res_feats_1"],
};

pub const DECODER: ModelSpec = ModelSpec {
    role: SegmentationModelRole::Decoder,
    onnx_file: "point_decoder.onnx",
    coreml_directory: "point_decoder.mlmodelc",
    inputs: &[
        "image_embed",
        "high_res_feats_0",
        "high_res_feats_1",
        "point_coords",
        "point_labels",
        "mask_input",
        "has_mask_input",
    ],
    onnx_outputs: &["mask_logits", "iou_scores"],
    coreml_outputs: &["masks", "iou_predictions"],
};

/// People and sky head: reads `image_embed`, the first encoder output.
pub const HEAD: ModelSpec = ModelSpec {
    role: SegmentationModelRole::Head,
    onnx_file: "people_sky_head.onnx",
    coreml_directory: "people_sky_head.mlmodelc",
    inputs: &["image_embed"],
    onnx_outputs: &[
        "sky_logits",
        "center_logits",
        "center_offset",
        "log_size",
        "presence_logits",
    ],
    coreml_outputs: &[
        "sky_logits",
        "center_logits",
        "center_offset",
        "log_size",
        "presence_logits",
    ],
};

/// S3OD `s3od-synth`: the Subject model.
pub const SUBJECT: ModelSpec = ModelSpec {
    role: SegmentationModelRole::Subject,
    onnx_file: "subject_masks.onnx",
    coreml_directory: "s3od.mlmodelc",
    inputs: &["pixel_values"],
    onnx_outputs: &["masks", "iou_scores"],
    coreml_outputs: &["masks", "iou_scores"],
};

/// Sky head (lab issue zidage/alcedo-ai-mask-lab#10): reads the three encoder outputs and the
/// encoder input, and gives the sky alpha of the stretched 1024 x 1024 image (SM3.E plan).
pub const SKY_HEAD: ModelSpec = ModelSpec {
    role: SegmentationModelRole::SkyHead,
    onnx_file: "sky_head.onnx",
    coreml_directory: "sky_head.mlmodelc",
    inputs: &[
        "image_embed",
        "high_res_feats_0",
        "high_res_feats_1",
        "pixel_values",
    ],
    onnx_outputs: &["sky_alpha"],
    coreml_outputs: &["sky_alpha"],
};

/// The model set version of the `model_set.json` text @p text (AI Models plan, AM1 completion
/// record): `schema` 1, `kind` `alcedo-ai-model-set`, `id` [`MODEL_SET_PACKAGE_ID`], and a
/// positive integer `model_set_version`. Other fields are allowed. The caller checks the range.
pub fn model_set_version_from_json(text: &str) -> Result<u64> {
    let value: Value = serde_json::from_str(text).map_err(|err| anyhow!("not JSON: {err}"))?;
    let object = value
        .as_object()
        .ok_or_else(|| anyhow!("not a JSON object"))?;
    if object.get("schema").and_then(Value::as_u64) != Some(1) {
        bail!("schema is not 1");
    }
    if object.get("kind").and_then(Value::as_str) != Some("alcedo-ai-model-set") {
        bail!("kind is not alcedo-ai-model-set");
    }
    if object.get("id").and_then(Value::as_str) != Some(MODEL_SET_PACKAGE_ID) {
        bail!("id is not {MODEL_SET_PACKAGE_ID}");
    }
    match object.get("model_set_version").and_then(Value::as_u64) {
        Some(version) if version > 0 => Ok(version),
        _ => bail!("model_set_version is not a positive integer"),
    }
}

pub fn spec(role: SegmentationModelRole) -> &'static ModelSpec {
    match role {
        SegmentationModelRole::Encoder => &ENCODER,
        SegmentationModelRole::Decoder => &DECODER,
        SegmentationModelRole::Head => &HEAD,
        SegmentationModelRole::Subject => &SUBJECT,
        SegmentationModelRole::SkyHead => &SKY_HEAD,
    }
}
