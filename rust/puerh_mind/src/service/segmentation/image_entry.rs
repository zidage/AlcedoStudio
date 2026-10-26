//! The image entry of the segmentation engine (SM3.C plan, decision C7; SM3.D plan, section 5.1).
//!
//! An image entry holds what the engine knows about one image: the image size, the image embedding
//! (SM3.B), the encoder input for the sky head (SM3.E), the S3OD input image (C8), the head output
//! and the People list with one crop image per listed person (SM3.D decisions E4 and E5), and the
//! task results. The sessions and the cache
//! entry of one image share one entry through `Arc`. The head output and the People list are
//! written once when the entry is made, at the session start, and are read only after that. Each
//! task result is computed once, at the first request that needs it, and stays until the entry is
//! dropped; the entry lifetime ends at the latest with an unload or a model change, so a result
//! never outlives the image or the model set that made it.
//!
//! Each task result has its own mutex. The engine holds that mutex while it computes the result, so
//! two requests on one image compute it once. A failed computation stores nothing. The engine
//! never holds two result mutexes at the same time.

use std::sync::{Arc, Mutex};

use super::backend::{HeadOutput, ImageFeatures};
use super::task_masks::{PersonBox, PixelRect, SubjectAlpha, SubjectInputImage};
use super::tensor::TensorF32;

/// The Sky mask of one image.
#[derive(Debug, Clone, PartialEq)]
pub enum SkyMask {
    /// The sky head alpha in [0, 1] on the image stretched to 1024 x 1024, row-major (SM3.E plan).
    Alpha(Vec<f32>),
    /// A model set without a sky head (Core ML until issue #538): the selected decoder mask of each
    /// people and sky head region, clipped logits (SM3.C plan, section 2.3).
    RegionLogits(Vec<Vec<f32>>),
}

/// Sky result of one image (SM3.C plan, section 2.3; SM3.E plan).
#[derive(Debug, Clone, PartialEq)]
pub struct SkyResult {
    /// With the sky head: the sky share of the alpha and the "sky found" share. Without it: the
    /// sky presence of the people and sky head and its threshold.
    pub presence: f32,
    pub presence_threshold: f32,
    /// `None` when no sky is found.
    pub mask: Option<SkyMask>,
    /// The "not found" text; empty when the mask exists.
    pub message: String,
}

/// One entry of the People list and its crop (SM3.D decisions E3 and E4).
#[derive(Debug, Clone, PartialEq)]
pub struct PersonCrop {
    pub person: PersonBox,
    /// The crop rectangle on the session image.
    pub rect: PixelRect,
    /// The crop pixels, resized to a long edge of 1024 when the crop is larger.
    pub image: SubjectInputImage,
}

/// The People list of one image, made at the session start (SM3.D plan, section 2.3, steps 1 and
/// 2).
#[derive(Debug, Clone, PartialEq)]
pub struct PeopleList {
    pub presence: f32,
    pub presence_threshold: f32,
    /// The listed people in the list order (C3); empty when no people are found.
    pub persons: Vec<PersonCrop>,
    /// The "not found" text; empty when the list has people.
    pub message: String,
}

/// A task result slot of an image entry: empty until the first successful computation.
pub type ResultSlot<T> = Mutex<Option<Arc<T>>>;

/// See the module documentation.
pub struct ImageEntry {
    /// Size of the session image; the size of the Sky union mask, and the size that the person
    /// crop rectangles and the mask boxes refer to.
    pub width: usize,
    pub height: usize,
    pub features: Arc<dyn ImageFeatures>,
    /// The encoder input of the image ([1, 3, 1024, 1024], 12 MB), the second input of the sky
    /// head; `None` when the model set has no sky head.
    pub encoder_input: Option<TensorF32>,
    pub subject_input: SubjectInputImage,
    pub head: HeadOutput,
    pub people: PeopleList,
    pub sky: ResultSlot<SkyResult>,
    pub subject: ResultSlot<SubjectAlpha>,
    /// The S3OD alpha of each person crop, in the order of `people.persons`.
    pub person_alphas: Vec<ResultSlot<SubjectAlpha>>,
}

impl ImageEntry {
    pub fn new(
        width: usize,
        height: usize,
        features: Arc<dyn ImageFeatures>,
        encoder_input: Option<TensorF32>,
        subject_input: SubjectInputImage,
        head: HeadOutput,
        people: PeopleList,
    ) -> Self {
        let person_alphas = people.persons.iter().map(|_| Mutex::new(None)).collect();
        Self {
            width,
            height,
            features,
            encoder_input,
            subject_input,
            head,
            people,
            sky: Mutex::new(None),
            subject: Mutex::new(None),
            person_alphas,
        }
    }
}
