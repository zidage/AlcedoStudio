//! Platform interface of the segmentation models.
//!
//! The engine runs the models only through these traits. Each platform adapter implements them:
//! [`super::ort_backend`] (ONNX Runtime; DirectML on Windows) and `super::coreml_backend`
//! (native Core ML on macOS). An adapter knows the file and tensor names of the model
//! specification ([`super::model_set`]); it does not know sessions, prompts, or mask rules.
//!
//! The encoder output stays in the native form of the backend ([`ImageFeatures`], SM3.B plan
//! decision B4), so a decoder call does not copy the 16 MB of features.

use std::any::Any;
use std::fmt;
use std::path::Path;
use std::sync::Arc;

use anyhow::{Result, anyhow};

use super::tensor::TensorF32;

/// The model of the set that a backend loads.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SegmentationModelRole {
    /// SAM 2.1 tiny image encoder.
    Encoder,
    /// SAM 2.1 tiny prompt encoder and mask decoder.
    Decoder,
    /// People and sky head on `image_embed` (People localization; its sky outputs are not used
    /// since the sky head, SM3.E).
    Head,
    /// S3OD `s3od-synth` (Subject and Background).
    Subject,
    /// Sky head on the encoder outputs and the encoder input (the Sky alpha, SM3.E).
    SkyHead,
}

impl SegmentationModelRole {
    pub const ALL: [SegmentationModelRole; 5] = [
        Self::Encoder,
        Self::Decoder,
        Self::Head,
        Self::Subject,
        Self::SkyHead,
    ];

    pub fn as_str(self) -> &'static str {
        match self {
            Self::Encoder => "encoder",
            Self::Decoder => "decoder",
            Self::Head => "head",
            Self::Subject => "subject",
            Self::SkyHead => "sky_head",
        }
    }
}

/// The encoder outputs of one image (`image_embed`, `high_res_feats_0`, `high_res_feats_1`) in the
/// native form of the backend that made them. The values do not change after the encoder run.
/// Sessions and the image embedding cache share one value through `Arc`.
pub trait ImageFeatures: Send + Sync {
    /// The adapter reads its own feature type through this method.
    fn as_any(&self) -> &dyn Any;
}

/// Reads the features of @p features as the type `T` of an adapter. Features of another backend
/// are an error.
pub fn features_of<T: 'static>(features: &dyn ImageFeatures) -> Result<&T> {
    features
        .as_any()
        .downcast_ref::<T>()
        .ok_or_else(|| anyhow!("the image features were made by another segmentation backend"))
}

/// A loaded image encoder.
pub trait SegmentationEncoder: Send {
    /// Runs the encoder once on @p pixel_values (shape [1, 3, 1024, 1024]) and returns its
    /// outputs in native form. The adapter checks the output shapes of the model specification.
    fn encode(&mut self, pixel_values: &TensorF32) -> Result<Arc<dyn ImageFeatures>>;
}

/// The prompt inputs of one decoder run, in the order and shapes of the model specification.
#[derive(Debug, Clone, PartialEq)]
pub struct DecoderPrompt {
    /// [1, N, 2], coordinates times 1024.
    pub point_coords: TensorF32,
    /// [1, N].
    pub point_labels: TensorF32,
    /// [1, 1, 256, 256].
    pub mask_input: TensorF32,
    /// [1].
    pub has_mask_input: TensorF32,
}

impl DecoderPrompt {
    /// The decoder point count N (box corners included).
    pub fn point_count(&self) -> usize {
        self.point_labels.data().len()
    }
}

/// The outputs of one decoder run.
#[derive(Debug, Clone, PartialEq)]
pub struct DecoderOutput {
    /// [1, 4, 256, 256].
    pub mask_logits: TensorF32,
    /// [1, 4].
    pub iou_scores: TensorF32,
}

/// A loaded prompt decoder.
pub trait SegmentationDecoder: Send {
    /// Runs the decoder once on @p features (made by the encoder of the same backend) with
    /// @p prompt. The adapter can keep a strong reference to @p features between calls, for
    /// example to keep a device copy bound.
    fn decode(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        prompt: &DecoderPrompt,
    ) -> Result<DecoderOutput>;
}

/// The outputs of one head run, in the shapes of the model specification. The People list reads
/// the person outputs; the sky outputs are part of the model output and are not used.
#[derive(Debug, Clone, PartialEq)]
pub struct HeadOutput {
    /// [1, 1, 64, 64].
    pub sky_logits: TensorF32,
    /// [1, 1, 64, 64].
    pub center_logits: TensorF32,
    /// [1, 2, 64, 64]: x offset, then y offset, in cells.
    pub center_offset: TensorF32,
    /// [1, 2, 64, 64]: natural logarithm of the box width, then height, in cells.
    pub log_size: TensorF32,
    /// [1, 2]: sky, then person.
    pub presence_logits: TensorF32,
}

/// A loaded people and sky head.
pub trait SegmentationHead: Send {
    /// Runs the head once on the `image_embed` of @p features (made by the encoder of the same
    /// backend). The adapter checks the output shapes.
    fn run(&mut self, features: &Arc<dyn ImageFeatures>) -> Result<HeadOutput>;
}

/// The outputs of one S3OD run, both after the sigmoid.
#[derive(Debug, Clone, PartialEq)]
pub struct SubjectOutput {
    /// [1, 3, 1024, 1024].
    pub masks: TensorF32,
    /// [1, 3].
    pub iou_scores: TensorF32,
}

/// A loaded Subject model.
pub trait SubjectModel: Send {
    /// Runs S3OD once on @p pixel_values (shape [1, 3, 1024, 1024]). The adapter checks the output
    /// shapes.
    fn run(&mut self, pixel_values: &TensorF32) -> Result<SubjectOutput>;
}

/// A loaded sky head.
pub trait SkyHeadModel: Send {
    /// Runs the sky head once on @p features (made by the encoder of the same backend) and on
    /// @p pixel_values, the encoder input of the same image (shape [1, 3, 1024, 1024]). Returns the
    /// sky alpha [1, 1, 1024, 1024] in [0, 1]. The adapter checks the output shape.
    fn run(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        pixel_values: &TensorF32,
    ) -> Result<TensorF32>;
}

/// The models of one platform and one device.
pub trait SegmentationBackend: Send + Sync {
    /// Short name of the runtime and device class, for example `onnxruntime-directml`.
    fn backend_name(&self) -> &str;
    /// Device label of @p role for status reports, for example `coreml-native:cpuonly`.
    fn device_label(&self, role: SegmentationModelRole) -> String;
    /// Loads the encoder from @p directory. The error names the file and carries the runtime
    /// message. The backend never loads a model on another device after an error.
    fn load_encoder(&self, directory: &Path) -> Result<Box<dyn SegmentationEncoder>>;
    /// Loads the decoder from @p directory, with the same error rules as the encoder.
    fn load_decoder(&self, directory: &Path) -> Result<Box<dyn SegmentationDecoder>>;
    /// Loads the people and sky head from @p directory and runs it once, with the same error
    /// rules as the encoder.
    fn load_head(&self, directory: &Path) -> Result<Box<dyn SegmentationHead>>;
    /// Loads S3OD from @p directory, with the same error rules as the encoder.
    fn load_subject_model(&self, directory: &Path) -> Result<Box<dyn SubjectModel>>;
    /// Loads the sky head from @p directory and runs it once, with the same error rules as the
    /// encoder.
    fn load_sky_head(&self, directory: &Path) -> Result<Box<dyn SkyHeadModel>>;
}

/// A device value that the platform does not support.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UnsupportedDevice(pub String);

impl fmt::Display for UnsupportedDevice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for UnsupportedDevice {}

/// Makes the backend of the platform for a device value of the load request.
pub trait SegmentationBackendFactory: Send + Sync {
    /// @p device is `""` for the platform default or one of the platform device values.
    fn create(&self, device: &str) -> Result<Box<dyn SegmentationBackend>, UnsupportedDevice>;
}
