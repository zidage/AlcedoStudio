//! A fake backend for the engine and server tests: no model files, counted calls.

use std::any::Any;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use anyhow::{Result, bail};

use super::backend::{
    DecoderOutput, DecoderPrompt, HeadOutput, ImageFeatures, SegmentationBackend,
    SegmentationBackendFactory, SegmentationDecoder, SegmentationEncoder, SegmentationHead,
    SegmentationModelRole, SkyHeadModel, SubjectModel, SubjectOutput, UnsupportedDevice,
    features_of,
};
use super::model_set::{
    CENTER_LOGITS_SHAPE, CENTER_OFFSET_SHAPE, HEAD_THRESHOLDS_FILE, LOG_SIZE_SHAPE, LOGIT_SIZE,
    MASK_LOGITS_SHAPE, MASK_SCORES_SHAPE, PIXEL_VALUES_SHAPE, PRESENCE_LOGITS_SHAPE,
    SKY_ALPHA_SHAPE, SKY_LOGITS_SHAPE, SUBJECT_IMAGE_SIZE, SUBJECT_MASKS_SHAPE,
    SUBJECT_PIXEL_VALUES_SHAPE, SUBJECT_SCORES_SHAPE,
};
use super::tensor::TensorF32;

/// The features of the fake encoder: the number of the encoder run that made them.
pub struct FakeImageFeatures {
    pub encoder_run: usize,
}

impl ImageFeatures for FakeImageFeatures {
    fn as_any(&self) -> &dyn Any {
        self
    }
}

/// One fake decoder run: the features and the prompt that it received.
pub struct FakeDecoderCall {
    pub features: Arc<dyn ImageFeatures>,
    pub prompt: DecoderPrompt,
}

impl FakeDecoderCall {
    /// The encoder run of the features of this call.
    pub fn encoder_run(&self) -> usize {
        features_of::<FakeImageFeatures>(self.features.as_ref())
            .expect("fake features")
            .encoder_run
    }
}

#[derive(Default)]
pub struct FakeCalls {
    pub loads: AtomicUsize,
    pub encoder_runs: AtomicUsize,
    pub decoder_calls: Mutex<Vec<FakeDecoderCall>>,
    pub head_runs: AtomicUsize,
    /// Number of the next head runs that fail.
    pub head_failures: AtomicUsize,
    /// The output of every fake head run after the warm-up; `None`: no sky and no person.
    pub head_output: Mutex<Option<HeadOutput>>,
    pub subject_runs: AtomicUsize,
    /// Number of the next S3OD runs that fail.
    pub subject_failures: AtomicUsize,
    /// The alpha value of the selected fake S3OD mask (mask 1) on the whole input.
    pub subject_alpha: Mutex<f32>,
    /// The first-channel input value at the center of the 1024 x 1024 input of every S3OD run, so
    /// a test can tell a photo run from a crop run.
    pub subject_center_inputs: Mutex<Vec<f32>>,
    /// When true, the fake decoder marks token 0 at the cell `plane - 1 - x`, where x is the first
    /// decoder coordinate, so masks of different prompts differ.
    pub mark_prompt_cell: std::sync::atomic::AtomicBool,
    pub sky_head_runs: AtomicUsize,
    /// Number of the next sky head runs that fail.
    pub sky_head_failures: AtomicUsize,
    /// The sky alpha of every fake sky head run after the warm-up (1024 x 1024 values); `None`:
    /// all 0, no sky.
    pub sky_alpha: Mutex<Option<Vec<f32>>>,
    /// The first value of the encoder input of every sky head run after the warm-up, so a test
    /// can check that the sky head reads the encoder input of its image.
    pub sky_head_first_inputs: Mutex<Vec<f32>>,
}

/// A head output without sky and without people: all logits -10.
pub fn empty_head_output() -> HeadOutput {
    let tensor = |shape: &[usize]| {
        TensorF32::new(shape.to_vec(), vec![-10.0; shape.iter().product()]).expect("tensor")
    };
    HeadOutput {
        sky_logits: tensor(&SKY_LOGITS_SHAPE),
        center_logits: tensor(&CENTER_LOGITS_SHAPE),
        center_offset: TensorF32::zeros(CENTER_OFFSET_SHAPE.to_vec()),
        log_size: TensorF32::zeros(LOG_SIZE_SHAPE.to_vec()),
        presence_logits: tensor(&PRESENCE_LOGITS_SHAPE),
    }
}

pub struct FakeFactory {
    pub calls: Arc<FakeCalls>,
    pub failing_role: Option<SegmentationModelRole>,
}

impl FakeFactory {
    pub fn new() -> Self {
        Self {
            calls: Arc::new(FakeCalls::default()),
            failing_role: None,
        }
    }
}

impl SegmentationBackendFactory for FakeFactory {
    fn create(&self, device: &str) -> Result<Box<dyn SegmentationBackend>, UnsupportedDevice> {
        if device != "fake" && device != "fake:other" {
            return Err(UnsupportedDevice(format!(
                "fake device {device:?} is not supported"
            )));
        }
        Ok(Box::new(FakeBackend {
            device: device.to_string(),
            calls: self.calls.clone(),
            failing_role: self.failing_role,
        }))
    }
}

struct FakeBackend {
    device: String,
    calls: Arc<FakeCalls>,
    failing_role: Option<SegmentationModelRole>,
}

impl FakeBackend {
    fn count_load(&self, role: SegmentationModelRole) -> Result<()> {
        self.calls.loads.fetch_add(1, Ordering::SeqCst);
        if self.failing_role == Some(role) {
            bail!("fake {} file is damaged", role.as_str());
        }
        Ok(())
    }
}

impl SegmentationBackend for FakeBackend {
    fn backend_name(&self) -> &str {
        "fake"
    }

    fn device_label(&self, _role: SegmentationModelRole) -> String {
        self.device.clone()
    }

    fn load_encoder(&self, _directory: &Path) -> Result<Box<dyn SegmentationEncoder>> {
        self.count_load(SegmentationModelRole::Encoder)?;
        Ok(Box::new(FakeEncoder {
            calls: self.calls.clone(),
        }))
    }

    fn load_decoder(&self, _directory: &Path) -> Result<Box<dyn SegmentationDecoder>> {
        self.count_load(SegmentationModelRole::Decoder)?;
        Ok(Box::new(FakeDecoder {
            calls: self.calls.clone(),
        }))
    }

    fn load_head(&self, _directory: &Path) -> Result<Box<dyn SegmentationHead>> {
        self.count_load(SegmentationModelRole::Head)?;
        Ok(Box::new(FakeHead {
            calls: self.calls.clone(),
        }))
    }

    fn load_subject_model(&self, _directory: &Path) -> Result<Box<dyn SubjectModel>> {
        self.count_load(SegmentationModelRole::Subject)?;
        Ok(Box::new(FakeSubjectModel {
            calls: self.calls.clone(),
        }))
    }

    fn load_sky_head(&self, _directory: &Path) -> Result<Box<dyn SkyHeadModel>> {
        self.count_load(SegmentationModelRole::SkyHead)?;
        Ok(Box::new(FakeSkyHead {
            calls: self.calls.clone(),
        }))
    }
}

struct FakeSkyHead {
    calls: Arc<FakeCalls>,
}

impl SkyHeadModel for FakeSkyHead {
    fn run(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        pixel_values: &TensorF32,
    ) -> Result<TensorF32> {
        features_of::<FakeImageFeatures>(features.as_ref())?;
        pixel_values.expect_shape("pixel_values", &PIXEL_VALUES_SHAPE)?;
        self.calls.sky_head_runs.fetch_add(1, Ordering::SeqCst);
        self.calls
            .sky_head_first_inputs
            .lock()
            .expect("sky head inputs")
            .push(pixel_values.data()[0]);
        if take_failure(&self.calls.sky_head_failures) {
            bail!("fake sky head run failed");
        }
        match self.calls.sky_alpha.lock().expect("sky alpha").clone() {
            Some(alpha) => TensorF32::new(SKY_ALPHA_SHAPE.to_vec(), alpha),
            None => Ok(TensorF32::zeros(SKY_ALPHA_SHAPE.to_vec())),
        }
    }
}

struct FakeHead {
    calls: Arc<FakeCalls>,
}

impl SegmentationHead for FakeHead {
    fn run(&mut self, features: &Arc<dyn ImageFeatures>) -> Result<HeadOutput> {
        features_of::<FakeImageFeatures>(features.as_ref())?;
        self.calls.head_runs.fetch_add(1, Ordering::SeqCst);
        if take_failure(&self.calls.head_failures) {
            bail!("fake head run failed");
        }
        Ok(self
            .calls
            .head_output
            .lock()
            .expect("head output")
            .clone()
            .unwrap_or_else(empty_head_output))
    }
}

/// Takes one planned failure from @p failures; true when the run must fail.
fn take_failure(failures: &AtomicUsize) -> bool {
    failures
        .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |left| {
            left.checked_sub(1)
        })
        .is_ok()
}

struct FakeSubjectModel {
    calls: Arc<FakeCalls>,
}

impl SubjectModel for FakeSubjectModel {
    fn run(&mut self, pixel_values: &TensorF32) -> Result<SubjectOutput> {
        pixel_values.expect_shape("pixel_values", &SUBJECT_PIXEL_VALUES_SHAPE)?;
        self.calls.subject_runs.fetch_add(1, Ordering::SeqCst);
        let center = (SUBJECT_IMAGE_SIZE / 2) * SUBJECT_IMAGE_SIZE + SUBJECT_IMAGE_SIZE / 2;
        self.calls
            .subject_center_inputs
            .lock()
            .expect("center inputs")
            .push(pixel_values.data()[center]);
        if take_failure(&self.calls.subject_failures) {
            bail!("fake S3OD run failed");
        }
        let plane = SUBJECT_IMAGE_SIZE * SUBJECT_IMAGE_SIZE;
        let mut masks = vec![0.0f32; 3 * plane];
        masks[plane..2 * plane].fill(*self.calls.subject_alpha.lock().expect("alpha"));
        Ok(SubjectOutput {
            masks: TensorF32::new(SUBJECT_MASKS_SHAPE.to_vec(), masks)?,
            iou_scores: TensorF32::new(SUBJECT_SCORES_SHAPE.to_vec(), vec![0.2, 0.95, 0.4])?,
        })
    }
}

struct FakeEncoder {
    calls: Arc<FakeCalls>,
}

impl SegmentationEncoder for FakeEncoder {
    fn encode(&mut self, pixel_values: &TensorF32) -> Result<Arc<dyn ImageFeatures>> {
        pixel_values.expect_shape("pixel_values", &PIXEL_VALUES_SHAPE)?;
        let encoder_run = self.calls.encoder_runs.fetch_add(1, Ordering::SeqCst) + 1;
        Ok(Arc::new(FakeImageFeatures { encoder_run }))
    }
}

/// Token t of the fake decoder is positive on the first `FAKE_TOKEN_AREAS[t]` logits.
pub const FAKE_TOKEN_AREAS: [usize; 4] = [3000, 100, 900, 400];
pub const FAKE_TOKEN_SCORES: [f32; 4] = [0.5, 0.6, 0.9, 0.7];

struct FakeDecoder {
    calls: Arc<FakeCalls>,
}

impl SegmentationDecoder for FakeDecoder {
    fn decode(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        prompt: &DecoderPrompt,
    ) -> Result<DecoderOutput> {
        features_of::<FakeImageFeatures>(features.as_ref())?;
        self.calls
            .decoder_calls
            .lock()
            .expect("decoder calls")
            .push(FakeDecoderCall {
                features: features.clone(),
                prompt: prompt.clone(),
            });
        let plane = LOGIT_SIZE * LOGIT_SIZE;
        let mut logits = vec![-4.0f32; 4 * plane];
        for (token, area) in FAKE_TOKEN_AREAS.iter().enumerate() {
            logits[token * plane..token * plane + area].fill(4.0);
        }
        if self.calls.mark_prompt_cell.load(Ordering::SeqCst) {
            let x = prompt.point_coords.data()[0] as usize;
            logits[plane - 1 - x] = 4.0;
        }
        Ok(DecoderOutput {
            mask_logits: TensorF32::new(MASK_LOGITS_SHAPE.to_vec(), logits)?,
            iou_scores: TensorF32::new(MASK_SCORES_SHAPE.to_vec(), FAKE_TOKEN_SCORES.to_vec())?,
        })
    }
}

/// The thresholds of the lab head (`people_sky_head.json`, the fields that the engine reads).
pub const FAKE_HEAD_THRESHOLDS: &str = r#"{"validation": {
    "presence_best_f1_person": {"threshold": 0.533202052116394},
    "person_list_best_f1_nms": {"threshold": 0.3132133185863495}}}"#;

/// A fake model directory with the head thresholds file. The file is written once per process
/// and directory: the tests run in parallel, and a second write would truncate the file while
/// another test reads it.
pub fn model_directory(name: &str) -> PathBuf {
    static WRITTEN: Mutex<Vec<String>> = Mutex::new(Vec::new());
    let directory = std::env::temp_dir().join(format!("alcedo-segmentation-fake-{name}"));
    let mut written = WRITTEN.lock().expect("written directories");
    if !written.iter().any(|entry| entry == name) {
        std::fs::create_dir_all(&directory).expect("create fake model directory");
        std::fs::write(directory.join(HEAD_THRESHOLDS_FILE), FAKE_HEAD_THRESHOLDS)
            .expect("write fake head thresholds");
        written.push(name.to_string());
    }
    directory
}
