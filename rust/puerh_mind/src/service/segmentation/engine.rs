//! Segmentation engine: the loaded model set, the segmentation sessions, and the image embedding
//! cache.
//!
//! The engine owns the models that a load request loads, one image entry per session
//! ([`super::image_entry`]), and the image embedding cache of the SM3.B plan (decisions B1, B2,
//! B6). It runs the models only through the backend traits ([`super::backend`]), so it is the same
//! on every platform. Load and unload take the write lock of the model set; start, decode, and
//! task requests hold its read lock for the whole model run, so an unload waits for running calls
//! and then ends the sessions and clears the cache. Each model has its own mutex: one call per
//! model at a time. The engine never holds the session lock and the cache lock at the same time.
//!
//! The task masks (SM3.C and SM3.D plans) run the people and sky head, the decoder, and S3OD with
//! the rules of [`super::task_masks`] and keep their results in the image entry. The head runs once
//! at the session start; it makes the People list and the person crops (SM3.D decision E5).

use std::collections::HashMap;
use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, RwLock};
use std::time::Instant;

use tracing::info;

use super::backend::{
    DecoderPrompt, HeadOutput, ImageFeatures, SegmentationBackendFactory, SegmentationDecoder,
    SegmentationEncoder, SegmentationHead, SegmentationModelRole, SubjectModel,
};
use super::image_embedding_cache::ImageEmbeddingCache;
use super::image_entry::{ImageEntry, PeopleList, PersonCrop, ResultSlot, SkyResult};
use super::model_set::{HEAD_THRESHOLDS_FILE, MASK_INPUT_SHAPE, MAX_IMAGE_EDGE, MODEL_SET_ID};
use super::sam2::{self, DecodedMasks, SegmentationPrompt};
use super::task_masks::{self, HeadThresholds, SubjectAlpha, SubjectInputImage};
use super::tensor::TensorF32;

/// Largest number of sessions at one time. The tool keeps one session; a few more cover a host
/// that starts the next image before it ends the previous one.
pub const MAX_SESSIONS: usize = 4;
/// Largest number of cached image embeddings (SM3.B decision B2; about 16 MB each).
pub const IMAGE_EMBEDDING_CACHE_CAPACITY: usize = 4;
/// Largest length of an image key in bytes.
pub const MAX_IMAGE_KEY_BYTES: usize = 256;

/// Failure of an engine operation. The text is the real error for the host.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SegmentationError {
    /// The models are not loaded, or a model file does not load.
    ModelNotReady(String),
    /// A request value fails validation.
    InvalidInput(String),
    /// The named session does not exist.
    SessionNotFound(String),
    /// A start request without pixels names an image key that the cache does not hold.
    ImageNotCached(String),
    /// A model run failed.
    Inference(String),
}

impl fmt::Display for SegmentationError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::ModelNotReady(message)
            | Self::InvalidInput(message)
            | Self::SessionNotFound(message)
            | Self::ImageNotCached(message)
            | Self::Inference(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for SegmentationError {}

/// An RGB8 image of a start request: row-major, 3 bytes per pixel.
#[derive(Debug, Clone, Copy)]
pub struct RgbImageView<'a> {
    pub width: u32,
    pub height: u32,
    pub rgb8: &'a [u8],
}

/// A start request (SM3.B plan section 2.1). At least one of `image` and `image_key` is set.
#[derive(Debug, Clone, Copy, Default)]
pub struct StartSessionRequest<'a> {
    /// The pixels; `None` starts from the cache only.
    pub image: Option<RgbImageView<'a>>,
    /// The opaque key of the image content; empty: the session is not cached.
    pub image_key: &'a str,
}

/// Residency of one loaded model.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModelResidency {
    pub role: SegmentationModelRole,
    pub device: String,
    pub load_ms: u64,
}

/// Engine state for status reports.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SegmentationStatus {
    pub loaded: bool,
    pub model_set_id: &'static str,
    pub model_directory: String,
    pub device: String,
    pub backend: String,
    pub models: Vec<ModelResidency>,
    pub session_count: usize,
    pub cached_image_count: usize,
}

/// A started session.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StartedSession {
    pub session_id: String,
    /// Time of the preprocessing, the encoder, the head, and the person crops; 0 for a cache hit.
    pub encoder_ms: u64,
    /// True when the session uses a cached image embedding.
    pub from_cache: bool,
    /// Size of the image entry of the session (SM5 decision M14): the size of the pixels for a new
    /// image, and the size of the cached entry for a cache hit.
    pub image_width: u32,
    pub image_height: u32,
}

/// The masks of one decode request.
#[derive(Debug, Clone, PartialEq)]
pub struct DecodeResult {
    pub masks: DecodedMasks,
    pub decoder_ms: u64,
}

/// A task mask (SM3.C plan, section 2.1; SM3.D plan, section 2.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SegmentationTask {
    Subject,
    Background,
    Sky,
    People,
    /// The S3OD alpha of the crop of one person of the People list (SM3.D decision E2).
    Person,
}

/// A task request: the task and, for `Person` only, the index into the People list (SM3.D
/// decision E11).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TaskRequest {
    pub task: SegmentationTask,
    pub person_index: Option<u32>,
}

/// A normalized rectangle of the session image (x0, y0, x1, y1 in [0, 1]).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MaskBox {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
}

impl MaskBox {
    /// The whole session image.
    pub const FULL_IMAGE: MaskBox = MaskBox {
        x0: 0.0,
        y0: 0.0,
        x1: 1.0,
        y1: 1.0,
    };
}

/// The values of a task mask, row-major.
#[derive(Debug, Clone, PartialEq)]
pub enum TaskMaskValues {
    /// Sky: the union mask at the session image size, R8, `MASK_INSIDE` (255) inside and 0 outside
    /// (SM3.C plan, decision C5).
    Binary(Vec<u8>),
    /// Subject, Background, People, and Person: S3OD alpha in [0, 1] on the unpadded S3OD grid;
    /// inside where >= 0.5. The grid covers `mask_box` of the session image (SM3.D decision E10):
    /// the whole image, or the crop rectangle of a person.
    Alpha { values: Vec<f32>, mask_box: MaskBox },
}

/// The mask of a task result.
#[derive(Debug, Clone, PartialEq)]
pub struct TaskMask {
    pub width: u32,
    pub height: u32,
    pub values: TaskMaskValues,
}

/// One entry of the People list of a task result: the normalized box, clipped to the image, the
/// score, and the share of the box in the Subject alpha (SM3.D decision E8). `subject_fraction` is
/// 0 and `in_subject` false while the image has no Subject alpha (a Person request before any
/// Subject, Background, or People request).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PersonListEntry {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    pub score: f32,
    pub subject_fraction: f32,
    pub in_subject: bool,
}

/// The result of a task request (SM3.C plan, section 2.2; SM3.D plan, section 2.2). `mask` is
/// `None` exactly when the task finds nothing; `message` then says why.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct TaskResult {
    pub mask: Option<TaskMask>,
    pub message: String,
    pub presence: f32,
    pub presence_threshold: f32,
    pub region_count: u32,
    /// People and Person: the full People list.
    pub people: Vec<PersonListEntry>,
    /// Subject, Background, People: the S3OD IoU score of the photo run; Person: of the crop run.
    pub subject_score: f32,
    pub mask_area: u32,
    /// Time of the model runs of this request; 0 when all results came from the image entry.
    pub model_ms: u64,
}

struct LoadedModelSet {
    directory: PathBuf,
    device: String,
    backend: String,
    models: Vec<ModelResidency>,
    thresholds: HeadThresholds,
    encoder: Mutex<Box<dyn SegmentationEncoder>>,
    decoder: Mutex<Box<dyn SegmentationDecoder>>,
    head: Mutex<Box<dyn SegmentationHead>>,
    subject: Mutex<Box<dyn SubjectModel>>,
}

/// See the module documentation.
pub struct SegmentationEngine {
    factory: Arc<dyn SegmentationBackendFactory>,
    models: RwLock<Option<LoadedModelSet>>,
    sessions: Mutex<HashMap<String, Arc<ImageEntry>>>,
    cache: Mutex<ImageEmbeddingCache<Arc<ImageEntry>>>,
}

fn poisoned(what: &str) -> SegmentationError {
    SegmentationError::Inference(format!("segmentation {what} lock poisoned"))
}

fn not_loaded() -> SegmentationError {
    SegmentationError::ModelNotReady("segmentation models are not loaded".to_string())
}

fn inference(err: anyhow::Error) -> SegmentationError {
    SegmentationError::Inference(format!("{err:#}"))
}

fn elapsed_ms(started: Instant) -> u64 {
    started.elapsed().as_millis() as u64
}

/// The value of @p slot, computed by @p compute when the slot is empty. The slot mutex stays
/// locked during the computation, so a second request on the same image waits for the first
/// result. A failure stores nothing. The flag is true when this call computed the value.
fn result_of<T>(
    slot: &ResultSlot<T>,
    compute: impl FnOnce() -> Result<T, SegmentationError>,
) -> Result<(Arc<T>, bool), SegmentationError> {
    let mut stored = slot.lock().map_err(|_| poisoned("image entry"))?;
    if let Some(value) = stored.as_ref() {
        return Ok((value.clone(), false));
    }
    let value = Arc::new(compute()?);
    *stored = Some(value.clone());
    Ok((value, true))
}

/// Reads the head thresholds of the model directory (SM3.C plan, decision C6).
fn read_head_thresholds(directory: &Path) -> Result<HeadThresholds, SegmentationError> {
    let path = directory.join(HEAD_THRESHOLDS_FILE);
    let text = std::fs::read_to_string(&path).map_err(|err| {
        SegmentationError::ModelNotReady(format!(
            "segmentation head thresholds {} cannot be read: {err}",
            path.display()
        ))
    })?;
    task_masks::head_thresholds_from_json(&text)
        .map_err(|err| SegmentationError::ModelNotReady(format!("{}: {err:#}", path.display())))
}

/// The union of the SAM 2.1 masks @p members at the image size of @p entry, with its area (Sky).
fn union_mask<'a>(
    entry: &ImageEntry,
    members: impl IntoIterator<Item = &'a [f32]>,
) -> (TaskMask, u32) {
    let union = task_masks::union_mask(members, entry.width, entry.height);
    let area = task_masks::mask_area(&union);
    (
        TaskMask {
            width: entry.width as u32,
            height: entry.height as u32,
            values: TaskMaskValues::Binary(union),
        },
        area,
    )
}

fn alpha_mask(alpha: &SubjectAlpha, values: Vec<f32>, mask_box: MaskBox) -> TaskMask {
    TaskMask {
        width: alpha.width as u32,
        height: alpha.height as u32,
        values: TaskMaskValues::Alpha { values, mask_box },
    }
}

/// The People list of @p head (SM3.D plan, section 2.3, steps 1 and 2): the SM3.C presence and
/// list rules, and the crop of each listed person from the session image @p rgb8 of @p width x
/// @p height (decisions E3 and E4). Presence below the threshold or no box: no crops and the "not
/// found" message.
fn people_list_of(
    thresholds: &HeadThresholds,
    head: &HeadOutput,
    rgb8: &[u8],
    width: usize,
    height: usize,
) -> anyhow::Result<PeopleList> {
    let (_, presence) = task_masks::presences(head);
    let threshold = thresholds.person_presence;
    let mut people = PeopleList {
        presence,
        presence_threshold: threshold,
        persons: Vec::new(),
        message: String::new(),
    };
    if presence < threshold {
        people.message =
            format!("No people found: person presence {presence:.3} is below {threshold:.3}.");
        return Ok(people);
    }
    let boxes = task_masks::decode_people(head);
    let listed = task_masks::people_list(&boxes, thresholds.people_list, true);
    if listed.is_empty() {
        people.message =
            format!("No people found: presence {presence:.3}, but the head found no person box.");
        return Ok(people);
    }
    for person in listed {
        let rect = task_masks::person_crop_rect(&person, width, height);
        let image = task_masks::person_crop_image(rgb8, width, height, rect)?;
        people.persons.push(PersonCrop {
            person,
            rect,
            image,
        });
    }
    Ok(people)
}

/// The normalized rectangle of the crop of @p crop on an image of @p width x @p height (SM3.D
/// decision E10).
fn crop_mask_box(crop: &PersonCrop, width: usize, height: usize) -> MaskBox {
    let (width, height) = (width as f32, height as f32);
    MaskBox {
        x0: crop.rect.x0 as f32 / width,
        y0: crop.rect.y0 as f32 / height,
        x1: crop.rect.x1 as f32 / width,
        y1: crop.rect.y1 as f32 / height,
    }
}

/// The People list entries of @p people. With @p subject, the Subject alpha of the image, each
/// entry has its `subject_fraction` and `in_subject` (SM3.D decision E8); without it, 0 and false.
fn person_list_entries(
    people: &PeopleList,
    subject: Option<&SubjectAlpha>,
) -> Vec<PersonListEntry> {
    people
        .persons
        .iter()
        .map(|crop| {
            let prompt_box = crop.person.prompt_box();
            let subject_fraction = subject.map_or(0.0, |alpha| {
                task_masks::subject_fraction(alpha, &crop.person)
            });
            PersonListEntry {
                x0: prompt_box.x0,
                y0: prompt_box.y0,
                x1: prompt_box.x1,
                y1: prompt_box.y1,
                score: crop.person.score,
                subject_fraction,
                in_subject: subject.is_some()
                    && subject_fraction >= task_masks::PERSON_IN_SUBJECT_FRACTION,
            }
        })
        .collect()
}

/// Checks the task and the person index of @p request (SM3.D decision E11) before any model work.
fn validate_task_request(request: &TaskRequest) -> Result<(), SegmentationError> {
    match (request.task, request.person_index) {
        (SegmentationTask::Person, None) => Err(SegmentationError::InvalidInput(
            "the Person task needs a person index".to_string(),
        )),
        (SegmentationTask::Person, Some(_)) | (_, None) => Ok(()),
        (task, Some(_)) => Err(SegmentationError::InvalidInput(format!(
            "a person index is only valid for the Person task, not for {task:?}"
        ))),
    }
}

/// Checks the image and the key of @p request before any model work.
fn validate_start_request(request: &StartSessionRequest<'_>) -> Result<(), SegmentationError> {
    if request.image_key.len() > MAX_IMAGE_KEY_BYTES {
        return Err(SegmentationError::InvalidInput(format!(
            "segmentation image key has {} bytes, more than {MAX_IMAGE_KEY_BYTES}",
            request.image_key.len()
        )));
    }
    let Some(image) = request.image else {
        if request.image_key.is_empty() {
            return Err(SegmentationError::InvalidInput(
                "a segmentation start request needs pixels or an image key".to_string(),
            ));
        }
        return Ok(());
    };
    let (width, height) = (image.width, image.height);
    if width == 0 || height == 0 || width.max(height) > MAX_IMAGE_EDGE {
        return Err(SegmentationError::InvalidInput(format!(
            "segmentation image {width}x{height} must have a long edge of 1 to {MAX_IMAGE_EDGE} px"
        )));
    }
    let expected = width as usize * height as usize * 3;
    if image.rgb8.len() != expected {
        return Err(SegmentationError::InvalidInput(format!(
            "segmentation image {width}x{height} needs {expected} RGB8 bytes, got {}",
            image.rgb8.len()
        )));
    }
    Ok(())
}

impl SegmentationEngine {
    /// An engine without loaded models. @p factory makes the platform backend of each load.
    pub fn new(factory: Arc<dyn SegmentationBackendFactory>) -> Self {
        Self {
            factory,
            models: RwLock::new(None),
            sessions: Mutex::new(HashMap::new()),
            cache: Mutex::new(ImageEmbeddingCache::new(IMAGE_EMBEDDING_CACHE_CAPACITY)),
        }
    }

    /// Ends all sessions and clears the image embedding cache. The caller holds the write lock of
    /// the model set.
    fn clear_embeddings(&self) -> Result<(), SegmentationError> {
        self.sessions
            .lock()
            .map_err(|_| poisoned("session"))?
            .clear();
        self.cache.lock().map_err(|_| poisoned("cache"))?.clear();
        Ok(())
    }

    /// Loads the encoder, the decoder, the people and sky head, S3OD, and the head thresholds from
    /// @p directory on @p device (SM3.C plan, decision C6).
    ///
    /// The same directory and device again keep the loaded models, the sessions, and the cache.
    /// Another directory or device ends all sessions, clears the cache, and unloads the old models
    /// before it loads. A failure leaves no model loaded and returns the backend error.
    pub fn load(&self, directory: &Path, device: &str) -> Result<(), SegmentationError> {
        if directory.as_os_str().is_empty() || !directory.is_absolute() {
            return Err(SegmentationError::InvalidInput(format!(
                "segmentation model directory {:?} is not an absolute path",
                directory
            )));
        }
        let backend = self
            .factory
            .create(device)
            .map_err(|err| SegmentationError::InvalidInput(err.to_string()))?;
        let device_label = backend.device_label(SegmentationModelRole::Encoder);
        let mut models = self.models.write().map_err(|_| poisoned("model set"))?;
        if let Some(loaded) = models.as_ref()
            && loaded.directory == directory
            && loaded.device == device_label
            && loaded.backend == backend.backend_name()
        {
            return Ok(());
        }
        self.clear_embeddings()?;
        *models = None;

        let not_ready = |err: anyhow::Error| SegmentationError::ModelNotReady(format!("{err:#}"));
        let residency = |role: SegmentationModelRole, started: Instant| {
            let residency = ModelResidency {
                role,
                device: backend.device_label(role),
                load_ms: elapsed_ms(started),
            };
            info!(
                "loaded segmentation {} on {} in {} ms",
                role.as_str(),
                residency.device,
                residency.load_ms
            );
            residency
        };
        let thresholds = read_head_thresholds(directory)?;
        let started = Instant::now();
        let encoder = backend.load_encoder(directory).map_err(not_ready)?;
        let encoder_residency = residency(SegmentationModelRole::Encoder, started);
        let started = Instant::now();
        let decoder = backend.load_decoder(directory).map_err(not_ready)?;
        let decoder_residency = residency(SegmentationModelRole::Decoder, started);
        let started = Instant::now();
        let head = backend.load_head(directory).map_err(not_ready)?;
        let head_residency = residency(SegmentationModelRole::Head, started);
        let started = Instant::now();
        let subject = backend.load_subject_model(directory).map_err(not_ready)?;
        let subject_residency = residency(SegmentationModelRole::Subject, started);
        *models = Some(LoadedModelSet {
            directory: directory.to_path_buf(),
            device: device_label,
            backend: backend.backend_name().to_string(),
            models: vec![
                encoder_residency,
                decoder_residency,
                head_residency,
                subject_residency,
            ],
            thresholds,
            encoder: Mutex::new(encoder),
            decoder: Mutex::new(decoder),
            head: Mutex::new(head),
            subject: Mutex::new(subject),
        });
        Ok(())
    }

    /// Ends all sessions, clears the cache, and unloads the models. Waits for running model
    /// calls. Idempotent.
    pub fn unload(&self) -> Result<(), SegmentationError> {
        let mut models = self.models.write().map_err(|_| poisoned("model set"))?;
        self.clear_embeddings()?;
        if models.take().is_some() {
            info!("unloaded segmentation models");
        }
        Ok(())
    }

    pub fn status(&self) -> Result<SegmentationStatus, SegmentationError> {
        let models = self.models.read().map_err(|_| poisoned("model set"))?;
        let session_count = self.sessions.lock().map_err(|_| poisoned("session"))?.len();
        let cached_image_count = self.cache.lock().map_err(|_| poisoned("cache"))?.len();
        Ok(match models.as_ref() {
            Some(loaded) => SegmentationStatus {
                loaded: true,
                model_set_id: MODEL_SET_ID,
                model_directory: loaded.directory.display().to_string(),
                device: loaded.device.clone(),
                backend: loaded.backend.clone(),
                models: loaded.models.clone(),
                session_count,
                cached_image_count,
            },
            None => SegmentationStatus {
                loaded: false,
                model_set_id: MODEL_SET_ID,
                model_directory: String::new(),
                device: String::new(),
                backend: String::new(),
                models: Vec::new(),
                session_count,
                cached_image_count,
            },
        })
    }

    fn check_session_capacity(&self) -> Result<(), SegmentationError> {
        let count = self.sessions.lock().map_err(|_| poisoned("session"))?.len();
        if count >= MAX_SESSIONS {
            return Err(SegmentationError::InvalidInput(format!(
                "{count} segmentation sessions are open; end a session before starting another (limit {MAX_SESSIONS})"
            )));
        }
        Ok(())
    }

    fn insert_session(&self, entry: Arc<ImageEntry>) -> Result<String, SegmentationError> {
        let session_id = uuid::Uuid::new_v4().to_string();
        self.sessions
            .lock()
            .map_err(|_| poisoned("session"))?
            .insert(session_id.clone(), entry);
        Ok(session_id)
    }

    fn session_entry(&self, session_id: &str) -> Result<Arc<ImageEntry>, SegmentationError> {
        self.sessions
            .lock()
            .map_err(|_| poisoned("session"))?
            .get(session_id)
            .cloned()
            .ok_or_else(|| {
                SegmentationError::SessionNotFound(format!(
                    "segmentation session {session_id:?} does not exist"
                ))
            })
    }

    /// Starts a session (SM3.B plan section 2.1).
    ///
    /// A cached image key starts the session with the cached image entry and does not run the
    /// encoder. Otherwise the encoder runs once on the pixels, the S3OD input image is made from
    /// them (SM3.C plan, decision C8), the head runs on the encoder output, and the People list
    /// and the person crops are made from the head output and the pixels (SM3.D decisions E4 and
    /// E5); a non-empty key stores the new image entry in the cache. A request without pixels and
    /// with an unknown key fails with `ImageNotCached`. A failure of a model run is an `Inference`
    /// error. No session and no cache entry is kept after a failure.
    pub fn start_session(
        &self,
        request: StartSessionRequest<'_>,
    ) -> Result<StartedSession, SegmentationError> {
        validate_start_request(&request)?;
        let models = self.models.read().map_err(|_| poisoned("model set"))?;
        let loaded = models.as_ref().ok_or_else(not_loaded)?;
        self.check_session_capacity()?;

        let key = request.image_key;
        if !key.is_empty() {
            let cached = self.cache.lock().map_err(|_| poisoned("cache"))?.get(key);
            if let Some(entry) = cached {
                let (image_width, image_height) = (entry.width as u32, entry.height as u32);
                let session_id = self.insert_session(entry)?;
                info!("started segmentation session {session_id} from the cached image {key:?}");
                return Ok(StartedSession {
                    session_id,
                    encoder_ms: 0,
                    from_cache: true,
                    image_width,
                    image_height,
                });
            }
        }
        let Some(image) = request.image else {
            return Err(SegmentationError::ImageNotCached(format!(
                "the segmentation image {key:?} is not cached; send its pixels"
            )));
        };

        let started = Instant::now();
        let (width, height) = (image.width, image.height);
        let invalid = |err: anyhow::Error| SegmentationError::InvalidInput(format!("{err:#}"));
        let pixels =
            sam2::preprocess_image(image.rgb8, width as usize, height as usize).map_err(invalid)?;
        let subject_input =
            task_masks::subject_input_image(image.rgb8, width as usize, height as usize)
                .map_err(invalid)?;
        let features = loaded
            .encoder
            .lock()
            .map_err(|_| poisoned("encoder"))?
            .encode(&pixels)
            .map_err(inference)?;
        let head = loaded
            .head
            .lock()
            .map_err(|_| poisoned("head"))?
            .run(&features)
            .map_err(inference)?;
        let people = people_list_of(
            &loaded.thresholds,
            &head,
            image.rgb8,
            width as usize,
            height as usize,
        )
        .map_err(inference)?;
        let encoder_ms = elapsed_ms(started);
        let entry = Arc::new(ImageEntry::new(
            width as usize,
            height as usize,
            features,
            subject_input,
            head,
            people,
        ));

        self.check_session_capacity()?;
        if !key.is_empty() {
            self.cache
                .lock()
                .map_err(|_| poisoned("cache"))?
                .insert(key.to_string(), entry.clone());
        }
        let session_id = self.insert_session(entry)?;
        info!("started segmentation session {session_id} ({width}x{height}) in {encoder_ms} ms");
        Ok(StartedSession {
            session_id,
            encoder_ms,
            from_cache: false,
            image_width: width,
            image_height: height,
        })
    }

    /// Runs the decoder once with @p prompt on the embedding of @p session_id and selects the
    /// masks (section 2.4 of the SM3.A plan).
    pub fn decode(
        &self,
        session_id: &str,
        prompt: SegmentationPrompt,
    ) -> Result<DecodeResult, SegmentationError> {
        sam2::validate_prompt(&prompt).map_err(SegmentationError::InvalidInput)?;
        let models = self.models.read().map_err(|_| poisoned("model set"))?;
        let loaded = models.as_ref().ok_or_else(not_loaded)?;
        let entry = self.session_entry(session_id)?;

        let started = Instant::now();
        let masks = Self::decode_features(loaded, &entry.features, prompt)?;
        Ok(DecodeResult {
            masks,
            decoder_ms: elapsed_ms(started),
        })
    }

    /// Runs the decoder once with the valid @p prompt on @p features and selects the masks.
    fn decode_features(
        loaded: &LoadedModelSet,
        features: &Arc<dyn ImageFeatures>,
        prompt: SegmentationPrompt,
    ) -> Result<DecodedMasks, SegmentationError> {
        let is_click = prompt.is_click();
        let (coords, labels) = sam2::model_prompt(&prompt);
        let point_count = labels.len();
        let has_mask_input = !prompt.mask_input.is_empty();
        let tensor =
            |shape: Vec<usize>, data: Vec<f32>| TensorF32::new(shape, data).map_err(inference);
        let decoder_prompt = DecoderPrompt {
            point_coords: tensor(vec![1, point_count, 2], coords)?,
            point_labels: tensor(vec![1, point_count], labels)?,
            mask_input: if has_mask_input {
                tensor(MASK_INPUT_SHAPE.to_vec(), prompt.mask_input)?
            } else {
                TensorF32::zeros(MASK_INPUT_SHAPE.to_vec())
            },
            has_mask_input: tensor(vec![1], vec![if has_mask_input { 1.0 } else { 0.0 }])?,
        };
        let output = loaded
            .decoder
            .lock()
            .map_err(|_| poisoned("decoder"))?
            .decode(features, &decoder_prompt)
            .map_err(inference)?;
        sam2::select_masks(&output.mask_logits, &output.iou_scores, is_click).map_err(inference)
    }

    /// Runs the decoder once with @p prompt (several points or a box) on @p features and returns
    /// the logits of the selected mask.
    fn decode_single_mask(
        loaded: &LoadedModelSet,
        features: &Arc<dyn ImageFeatures>,
        prompt: SegmentationPrompt,
    ) -> Result<Vec<f32>, SegmentationError> {
        let mut masks = Self::decode_features(loaded, features, prompt)?;
        let selected = masks.selected_index;
        Ok(masks.masks.swap_remove(selected).logits)
    }

    /// Runs the task mask @p request on the image of @p session_id (SM3.C plan, section 2; SM3.D
    /// plan, sections 2.1 to 2.4).
    ///
    /// The Sky result, the Subject alpha, and the alpha of each person crop are computed once per
    /// image entry; later requests read them from the entry. A task that finds nothing is a
    /// successful result without a mask. An invalid task, person index, or session fails before
    /// any model run.
    pub fn run_task(
        &self,
        session_id: &str,
        request: TaskRequest,
    ) -> Result<TaskResult, SegmentationError> {
        validate_task_request(&request)?;
        let models = self.models.read().map_err(|_| poisoned("model set"))?;
        let loaded = models.as_ref().ok_or_else(not_loaded)?;
        let entry = self.session_entry(session_id)?;
        let started = Instant::now();
        let (mut result, computed) = match (request.task, request.person_index) {
            (SegmentationTask::Sky, _) => Self::sky_task(loaded, &entry)?,
            (SegmentationTask::People, _) => Self::people_task(loaded, &entry)?,
            (SegmentationTask::Person, Some(index)) => Self::person_task(loaded, &entry, index)?,
            (SegmentationTask::Person, None) => unreachable!("validate_task_request"),
            (SegmentationTask::Subject | SegmentationTask::Background, _) => {
                Self::subject_task(loaded, &entry, request.task)?
            }
        };
        if computed {
            result.model_ms = elapsed_ms(started);
            info!(
                "ran segmentation task {:?} in {} ms",
                request.task, result.model_ms
            );
        }
        Ok(result)
    }

    fn sky_task(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
    ) -> Result<(TaskResult, bool), SegmentationError> {
        let (sky, computed) =
            result_of(&entry.sky, || Self::compute_sky(loaded, entry, &entry.head))?;
        let mut result = TaskResult {
            presence: sky.presence,
            presence_threshold: sky.presence_threshold,
            region_count: sky.region_logits.len() as u32,
            message: sky.message.clone(),
            ..Default::default()
        };
        if !sky.region_logits.is_empty() {
            let (mask, area) = union_mask(entry, sky.region_logits.iter().map(Vec::as_slice));
            result.mask_area = area;
            result.mask = Some(mask);
        }
        Ok((result, computed))
    }

    /// Sky rule of the SM3.C plan, section 2.3.
    fn compute_sky(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
        head: &HeadOutput,
    ) -> Result<SkyResult, SegmentationError> {
        let (presence, _) = task_masks::presences(head);
        let threshold = loaded.thresholds.sky_presence;
        let mut sky = SkyResult {
            presence,
            presence_threshold: threshold,
            region_logits: Vec::new(),
            message: String::new(),
        };
        if presence < threshold {
            sky.message =
                format!("No sky found: sky presence {presence:.3} is below {threshold:.3}.");
            return Ok(sky);
        }
        let probability = task_masks::sky_probability(head);
        let regions = task_masks::sky_regions(&probability);
        if regions.is_empty() {
            sky.message = format!(
                "No sky found: presence {presence:.3}, but no region of at least {} cells with sky probability >= {}.",
                task_masks::SKY_POINTS_PER_REGION,
                task_masks::SKY_CELL_PROBABILITY
            );
            return Ok(sky);
        }
        for region in &regions {
            let prompt = SegmentationPrompt {
                points: task_masks::region_points(&probability, region),
                ..Default::default()
            };
            sky.region_logits
                .push(Self::decode_single_mask(loaded, &entry.features, prompt)?);
        }
        Ok(sky)
    }

    /// One S3OD run on @p input with the resident model (SM3.D decision D22), and the selected
    /// alpha without the padding. Used for the photo and for each person crop.
    fn subject_alpha_of(
        loaded: &LoadedModelSet,
        input: &SubjectInputImage,
    ) -> Result<SubjectAlpha, SegmentationError> {
        let started = Instant::now();
        let pixel_values = task_masks::subject_pixel_values(input).map_err(inference)?;
        let run_started = Instant::now();
        let output = loaded
            .subject
            .lock()
            .map_err(|_| poisoned("subject model"))?
            .run(&pixel_values)
            .map_err(inference)?;
        let run_ms = elapsed_ms(run_started);
        let alpha = task_masks::select_subject_alpha(&output, input).map_err(inference)?;
        // A model run far above the measured 200 ms on DirectML shows that the GPU memory of the
        // sidecar does not fit next to the other processes (issue #530).
        info!(
            "ran S3OD in {} ms (model run {run_ms} ms)",
            elapsed_ms(started)
        );
        Ok(alpha)
    }

    /// The Subject alpha of @p entry, computed once (SM3.C plan, section 2.5). The flag is true when
    /// this call ran S3OD.
    fn photo_subject_alpha(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
    ) -> Result<(Arc<SubjectAlpha>, bool), SegmentationError> {
        result_of(&entry.subject, || {
            Self::subject_alpha_of(loaded, &entry.subject_input)
        })
    }

    /// People rule of the SM3.D plan, section 2.3, step 3: not found without people, with no S3OD
    /// run (E6); else the Subject alpha, found also when no value reaches 0.5 (E7), and the People
    /// list with `subject_fraction` and `in_subject` (E8). No decoder run.
    fn people_task(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
    ) -> Result<(TaskResult, bool), SegmentationError> {
        let people = &entry.people;
        let mut result = TaskResult {
            presence: people.presence,
            presence_threshold: people.presence_threshold,
            message: people.message.clone(),
            ..Default::default()
        };
        if people.persons.is_empty() {
            return Ok((result, false));
        }
        let (alpha, computed) = Self::photo_subject_alpha(loaded, entry)?;
        result.people = person_list_entries(people, Some(&alpha));
        result.subject_score = alpha.score;
        result.mask_area = task_masks::alpha_area(&alpha.values);
        result.mask = Some(alpha_mask(
            &alpha,
            alpha.values.clone(),
            MaskBox::FULL_IMAGE,
        ));
        Ok((result, computed))
    }

    /// Person rule of the SM3.D plan, section 2.4: the S3OD alpha of the stored crop of person
    /// @p index, computed once (E2, E4); not found when no value reaches 0.5 (E9). The People list
    /// carries the fractions of the stored Subject alpha, when the image has one.
    fn person_task(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
        index: u32,
    ) -> Result<(TaskResult, bool), SegmentationError> {
        let people = &entry.people;
        let (Some(crop), Some(slot)) = (
            people.persons.get(index as usize),
            entry.person_alphas.get(index as usize),
        ) else {
            return Err(SegmentationError::InvalidInput(format!(
                "person index {index} is outside the People list of {} people",
                people.persons.len()
            )));
        };
        let (alpha, computed) = result_of(slot, || {
            let input = task_masks::subject_input_image(
                &crop.image.rgb8,
                crop.image.width,
                crop.image.height,
            )
            .map_err(inference)?;
            Self::subject_alpha_of(loaded, &input)
        })?;
        let subject = entry
            .subject
            .lock()
            .map_err(|_| poisoned("image entry"))?
            .clone();
        let mut result = TaskResult {
            presence: people.presence,
            presence_threshold: people.presence_threshold,
            people: person_list_entries(people, subject.as_deref()),
            subject_score: alpha.score,
            ..Default::default()
        };
        let area = task_masks::alpha_area(&alpha.values);
        if area == 0 {
            result.message = format!(
                "No person found in the crop of person {index}: no alpha value reaches {} (S3OD score {:.3}).",
                task_masks::ALPHA_INSIDE,
                alpha.score
            );
            return Ok((result, computed));
        }
        result.mask_area = area;
        result.mask = Some(alpha_mask(
            &alpha,
            alpha.values.clone(),
            crop_mask_box(crop, entry.width, entry.height),
        ));
        Ok((result, computed))
    }

    /// Subject and Background rules of the SM3.C plan, section 2.5 and decisions C1 and C9.
    fn subject_task(
        loaded: &LoadedModelSet,
        entry: &ImageEntry,
        task: SegmentationTask,
    ) -> Result<(TaskResult, bool), SegmentationError> {
        let (alpha, computed) = Self::photo_subject_alpha(loaded, entry)?;
        let mut result = TaskResult {
            subject_score: alpha.score,
            ..Default::default()
        };
        if task_masks::alpha_area(&alpha.values) == 0 {
            result.message = format!(
                "No subject found: no Subject alpha value reaches {} (S3OD score {:.3}).",
                task_masks::ALPHA_INSIDE,
                alpha.score
            );
            return Ok((result, computed));
        }
        let values: Vec<f32> = match task {
            SegmentationTask::Background => alpha.values.iter().map(|value| 1.0 - value).collect(),
            _ => alpha.values.clone(),
        };
        let area = task_masks::alpha_area(&values);
        if area == 0 {
            result.message = format!(
                "No background found: the subject covers the whole image (S3OD score {:.3}).",
                alpha.score
            );
            return Ok((result, computed));
        }
        result.mask_area = area;
        result.mask = Some(alpha_mask(&alpha, values, MaskBox::FULL_IMAGE));
        Ok((result, computed))
    }

    /// Ends @p session_id. Returns false when the session does not exist. The cache entry of the
    /// session image stays.
    pub fn end_session(&self, session_id: &str) -> Result<bool, SegmentationError> {
        Ok(self
            .sessions
            .lock()
            .map_err(|_| poisoned("session"))?
            .remove(session_id)
            .is_some())
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::Ordering;

    use super::super::model_set::{
        CENTER_LOGITS_SHAPE, CENTER_OFFSET_SHAPE, HEAD_GRID, LOG_SIZE_SHAPE, LOGIT_SIZE,
        PRESENCE_LOGITS_SHAPE, SKY_LOGITS_SHAPE,
    };
    use super::super::sam2::{PointLabel, PromptBox, PromptPoint};
    use super::super::task_masks::PixelRect;
    use super::super::test_support::{
        FAKE_TOKEN_AREAS, FakeCalls, FakeFactory, empty_head_output, model_directory,
    };
    use super::*;

    fn engine_with(factory: FakeFactory) -> (SegmentationEngine, Arc<FakeCalls>) {
        let calls = factory.calls.clone();
        (SegmentationEngine::new(Arc::new(factory)), calls)
    }

    fn loaded_engine() -> (SegmentationEngine, Arc<FakeCalls>) {
        let (engine, calls) = engine_with(FakeFactory::new());
        engine.load(&model_directory("a"), "fake").expect("load");
        (engine, calls)
    }

    fn gray_image(width: u32, height: u32) -> Vec<u8> {
        vec![100u8; width as usize * height as usize * 3]
    }

    fn start_keyed(
        engine: &SegmentationEngine,
        image_key: &str,
    ) -> Result<StartedSession, SegmentationError> {
        let pixels = gray_image(64, 48);
        engine.start_session(StartSessionRequest {
            image: Some(RgbImageView {
                width: 64,
                height: 48,
                rgb8: &pixels,
            }),
            image_key,
        })
    }

    fn start(engine: &SegmentationEngine) -> Result<StartedSession, SegmentationError> {
        start_keyed(engine, "")
    }

    fn start_key_only(
        engine: &SegmentationEngine,
        image_key: &str,
    ) -> Result<StartedSession, SegmentationError> {
        engine.start_session(StartSessionRequest {
            image: None,
            image_key,
        })
    }

    fn click(x: f32, y: f32) -> SegmentationPrompt {
        SegmentationPrompt {
            points: vec![PromptPoint {
                x,
                y,
                label: PointLabel::Add,
            }],
            ..Default::default()
        }
    }

    #[test]
    fn start_session_before_load_reports_model_not_ready() {
        let (engine, calls) = engine_with(FakeFactory::new());
        assert!(matches!(
            start(&engine),
            Err(SegmentationError::ModelNotReady(_))
        ));
        assert_eq!(engine.status().expect("status").session_count, 0);
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn load_failure_leaves_no_models_and_reports_backend_error() {
        let mut factory = FakeFactory::new();
        factory.failing_role = Some(SegmentationModelRole::Decoder);
        let (engine, _calls) = engine_with(factory);
        let err = engine
            .load(&model_directory("a"), "fake")
            .expect_err("decoder load fails");
        assert!(
            matches!(&err, SegmentationError::ModelNotReady(message) if message.contains("fake decoder file is damaged")),
            "{err}"
        );
        assert!(!engine.status().expect("status").loaded);
    }

    #[test]
    fn load_rejects_unsupported_device_and_relative_directory() {
        let (engine, calls) = engine_with(FakeFactory::new());
        assert!(matches!(
            engine.load(&model_directory("a"), "directml"),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert!(matches!(
            engine.load(Path::new("relative/models"), "fake"),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert_eq!(calls.loads.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn start_session_runs_encoder_once_and_keeps_its_outputs() {
        let (engine, calls) = loaded_engine();
        let session = start(&engine).expect("start");
        assert!(!session.from_cache);
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 1);
        let status = engine.status().expect("status");
        assert!(status.loaded);
        assert_eq!(status.session_count, 1);
        assert_eq!(status.cached_image_count, 0);
        assert_eq!(
            status
                .models
                .iter()
                .map(|model| model.role)
                .collect::<Vec<_>>(),
            SegmentationModelRole::ALL.to_vec()
        );
        assert_eq!(status.model_set_id, MODEL_SET_ID);

        engine
            .decode(&session.session_id, click(0.5, 0.5))
            .expect("decode");
        let decoder_calls = calls.decoder_calls.lock().expect("decoder calls");
        assert_eq!(decoder_calls.len(), 1);
        assert_eq!(decoder_calls[0].encoder_run(), 1);
    }

    #[test]
    fn start_session_rejects_invalid_image_without_running_encoder() {
        let (engine, calls) = loaded_engine();
        let short = vec![0u8; 10];
        let long_edge = gray_image(4097, 1);
        let cases = [
            RgbImageView {
                width: 64,
                height: 48,
                rgb8: &short,
            },
            RgbImageView {
                width: 0,
                height: 48,
                rgb8: &short,
            },
            RgbImageView {
                width: 4097,
                height: 1,
                rgb8: &long_edge,
            },
        ];
        for image in cases {
            assert!(matches!(
                engine.start_session(StartSessionRequest {
                    image: Some(image),
                    image_key: "",
                }),
                Err(SegmentationError::InvalidInput(_))
            ));
        }
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 0);
        assert_eq!(engine.status().expect("status").session_count, 0);
    }

    #[test]
    fn start_rejects_invalid_key_and_partial_empty_image() {
        let (engine, calls) = loaded_engine();
        let long_key = "k".repeat(MAX_IMAGE_KEY_BYTES + 1);
        assert!(matches!(
            start_keyed(&engine, &long_key),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert!(matches!(
            start_key_only(&engine, ""),
            Err(SegmentationError::InvalidInput(_))
        ));
        let bytes = vec![0u8; 12];
        assert!(matches!(
            engine.start_session(StartSessionRequest {
                image: Some(RgbImageView {
                    width: 0,
                    height: 0,
                    rgb8: &bytes,
                }),
                image_key: "k",
            }),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 0);
        let status = engine.status().expect("status");
        assert_eq!(status.session_count, 0);
        assert_eq!(status.cached_image_count, 0);
    }

    #[test]
    fn start_with_cached_key_skips_encoder_and_shares_embedding() {
        let (engine, calls) = loaded_engine();
        let first = start_keyed(&engine, "photo-a").expect("first start");
        let second = start_keyed(&engine, "photo-a").expect("second start");
        assert!(!first.from_cache);
        assert!(second.from_cache);
        assert_eq!(second.encoder_ms, 0);
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 1);
        let status = engine.status().expect("status");
        assert_eq!(status.session_count, 2);
        assert_eq!(status.cached_image_count, 1);

        engine
            .decode(&first.session_id, click(0.5, 0.5))
            .expect("decode first");
        engine
            .decode(&second.session_id, click(0.5, 0.5))
            .expect("decode second");
        let decoder_calls = calls.decoder_calls.lock().expect("decoder calls");
        assert!(Arc::ptr_eq(
            &decoder_calls[0].features,
            &decoder_calls[1].features
        ));
    }

    #[test]
    fn key_only_start_reports_image_not_cached_without_encoder() {
        let (engine, calls) = loaded_engine();
        assert!(matches!(
            start_key_only(&engine, "photo-a"),
            Err(SegmentationError::ImageNotCached(_))
        ));
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 0);
        assert_eq!(engine.status().expect("status").session_count, 0);
    }

    #[test]
    fn key_only_start_hits_after_session_end() {
        let (engine, calls) = loaded_engine();
        let first = start_keyed(&engine, "photo-a").expect("start");
        assert!(engine.end_session(&first.session_id).expect("end"));
        let cached = start_key_only(&engine, "photo-a").expect("cached start");
        assert!(cached.from_cache);
        assert_eq!((first.image_width, first.image_height), (64, 48));
        assert_eq!((cached.image_width, cached.image_height), (64, 48));
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 1);
        engine
            .decode(&cached.session_id, click(0.5, 0.5))
            .expect("decode");
        assert_eq!(
            calls.decoder_calls.lock().expect("decoder calls")[0].encoder_run(),
            1
        );
    }

    #[test]
    fn cache_evicts_fifth_key() {
        let (engine, calls) = loaded_engine();
        for index in 1..=IMAGE_EMBEDDING_CACHE_CAPACITY + 1 {
            let session = start_keyed(&engine, &format!("k{index}")).expect("start");
            engine.end_session(&session.session_id).expect("end");
        }
        assert_eq!(
            engine.status().expect("status").cached_image_count,
            IMAGE_EMBEDDING_CACHE_CAPACITY
        );
        assert!(matches!(
            start_key_only(&engine, "k1"),
            Err(SegmentationError::ImageNotCached(_))
        ));
        for index in 2..=IMAGE_EMBEDDING_CACHE_CAPACITY + 1 {
            let session = start_key_only(&engine, &format!("k{index}")).expect("cached start");
            assert!(session.from_cache);
            engine.end_session(&session.session_id).expect("end");
        }
        assert_eq!(
            calls.encoder_runs.load(Ordering::SeqCst),
            IMAGE_EMBEDDING_CACHE_CAPACITY + 1
        );
    }

    #[test]
    fn unload_and_model_change_clear_cached_images() {
        let (engine, _calls) = loaded_engine();
        start_keyed(&engine, "photo-a").expect("start");
        engine.unload().expect("unload");
        assert_eq!(engine.status().expect("status").cached_image_count, 0);
        engine.load(&model_directory("a"), "fake").expect("load");
        assert!(matches!(
            start_key_only(&engine, "photo-a"),
            Err(SegmentationError::ImageNotCached(_))
        ));

        start_keyed(&engine, "photo-b").expect("start");
        engine
            .load(&model_directory("a"), "fake")
            .expect("same load");
        assert_eq!(engine.status().expect("status").cached_image_count, 1);
        engine
            .load(&model_directory("b"), "fake")
            .expect("other directory");
        let status = engine.status().expect("status");
        assert_eq!(status.cached_image_count, 0);
        assert_eq!(status.session_count, 0);
        assert!(matches!(
            start_key_only(&engine, "photo-b"),
            Err(SegmentationError::ImageNotCached(_))
        ));
    }

    #[test]
    fn session_limit_rejects_fifth_session() {
        let (engine, _calls) = loaded_engine();
        for _ in 0..MAX_SESSIONS {
            start_keyed(&engine, "photo-a").expect("start within the limit");
        }
        assert!(matches!(
            start_keyed(&engine, "photo-a"),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert_eq!(engine.status().expect("status").session_count, MAX_SESSIONS);
    }

    #[test]
    fn same_load_request_keeps_models_and_sessions() {
        let (engine, calls) = loaded_engine();
        start(&engine).expect("start");
        engine
            .load(&model_directory("a"), "fake")
            .expect("same load");
        assert_eq!(calls.loads.load(Ordering::SeqCst), 4);
        assert_eq!(engine.status().expect("status").session_count, 1);
    }

    #[test]
    fn load_with_other_directory_ends_sessions() {
        let (engine, calls) = loaded_engine();
        start(&engine).expect("start");
        engine
            .load(&model_directory("b"), "fake")
            .expect("other directory");
        assert_eq!(calls.loads.load(Ordering::SeqCst), 8);
        assert_eq!(engine.status().expect("status").session_count, 0);
        engine
            .load(&model_directory("b"), "fake:other")
            .expect("other device");
        assert_eq!(calls.loads.load(Ordering::SeqCst), 12);
    }

    #[test]
    fn unload_ends_sessions_and_is_idempotent() {
        let (engine, _calls) = loaded_engine();
        let session = start(&engine).expect("start");
        engine.unload().expect("unload");
        engine.unload().expect("second unload");
        let status = engine.status().expect("status");
        assert!(!status.loaded);
        assert_eq!(status.session_count, 0);
        assert!(matches!(
            engine.decode(&session.session_id, click(0.5, 0.5)),
            Err(SegmentationError::ModelNotReady(_))
        ));
    }

    #[test]
    fn end_unknown_session_returns_false() {
        let (engine, _calls) = loaded_engine();
        let session = start(&engine).expect("start");
        assert!(!engine.end_session("missing").expect("end"));
        assert!(engine.end_session(&session.session_id).expect("end"));
        assert!(!engine.end_session(&session.session_id).expect("end again"));
    }

    #[test]
    fn decode_passes_embedding_and_mask_input_to_decoder() {
        let (engine, calls) = loaded_engine();
        let session = start(&engine).expect("start");
        let mut mask_input = vec![-1.0f32; LOGIT_SIZE * LOGIT_SIZE];
        mask_input[5] = 7.0;
        let prompt = SegmentationPrompt {
            points: vec![
                PromptPoint {
                    x: 0.25,
                    y: 0.5,
                    label: PointLabel::Add,
                },
                PromptPoint {
                    x: 0.75,
                    y: 0.5,
                    label: PointLabel::Subtract,
                },
            ],
            prompt_box: Some(PromptBox {
                x0: 0.1,
                y0: 0.2,
                x1: 0.9,
                y1: 0.8,
            }),
            mask_input: mask_input.clone(),
        };
        let result = engine.decode(&session.session_id, prompt).expect("decode");
        let decoder_calls = calls.decoder_calls.lock().expect("decoder calls");
        let call = &decoder_calls[0];
        assert_eq!(call.encoder_run(), 1);
        assert_eq!(call.prompt.point_coords.shape(), &[1, 4, 2]);
        assert_eq!(call.prompt.point_count(), 4);
        assert_eq!(call.prompt.point_labels.data(), &[2.0, 3.0, 1.0, 0.0]);
        assert_eq!(call.prompt.mask_input.shape(), &MASK_INPUT_SHAPE);
        assert_eq!(call.prompt.mask_input.data(), mask_input.as_slice());
        assert_eq!(call.prompt.has_mask_input.data(), &[1.0]);
        // A box selects one mask; the fake token 0 is stable, so it is selected.
        assert_eq!(result.masks.masks.len(), 1);
        assert_eq!(result.masks.masks[0].token, 0);
        assert_eq!(result.masks.masks[0].area as usize, FAKE_TOKEN_AREAS[0]);
    }

    #[test]
    fn decode_click_without_mask_input_returns_three_levels() {
        let (engine, calls) = loaded_engine();
        let session = start(&engine).expect("start");
        let result = engine
            .decode(&session.session_id, click(0.5, 0.5))
            .expect("decode");
        let tokens: Vec<u32> = result.masks.masks.iter().map(|level| level.token).collect();
        assert_eq!(tokens, vec![1, 3, 2]);
        assert_eq!(result.masks.selected_index, 2);
        let decoder_calls = calls.decoder_calls.lock().expect("decoder calls");
        assert_eq!(decoder_calls[0].prompt.has_mask_input.data(), &[0.0]);
        assert!(
            decoder_calls[0]
                .prompt
                .mask_input
                .data()
                .iter()
                .all(|value| *value == 0.0)
        );
    }

    #[test]
    fn decode_unknown_session_reports_session_not_found() {
        let (engine, _calls) = loaded_engine();
        assert!(matches!(
            engine.decode("missing", click(0.5, 0.5)),
            Err(SegmentationError::SessionNotFound(_))
        ));
        assert!(matches!(
            engine.decode("missing", SegmentationPrompt::default()),
            Err(SegmentationError::InvalidInput(_))
        ));
    }

    fn task(task: SegmentationTask) -> TaskRequest {
        TaskRequest {
            task,
            person_index: None,
        }
    }

    fn person(index: u32) -> TaskRequest {
        TaskRequest {
            task: SegmentationTask::Person,
            person_index: Some(index),
        }
    }

    /// A head output with the sky and person presence logits, a sky region of 4 cells, and person
    /// centers at the given (row, column) cells with their center logits; each box is 4 x 8 cells.
    fn head_output(presence: [f32; 2], persons: &[(usize, usize, f32)]) -> HeadOutput {
        let plane = HEAD_GRID * HEAD_GRID;
        let mut head = empty_head_output();
        let mut sky = vec![-10.0f32; plane];
        sky[..4].fill(5.0);
        head.sky_logits = TensorF32::new(SKY_LOGITS_SHAPE.to_vec(), sky).unwrap();
        let mut center = vec![-10.0f32; plane];
        let mut log_size = vec![0.0f32; 2 * plane];
        for &(row, column, logit) in persons {
            let cell = row * HEAD_GRID + column;
            center[cell] = logit;
            log_size[cell] = 4f32.ln();
            log_size[plane + cell] = 8f32.ln();
        }
        head.center_logits = TensorF32::new(CENTER_LOGITS_SHAPE.to_vec(), center).unwrap();
        head.center_offset = TensorF32::zeros(CENTER_OFFSET_SHAPE.to_vec());
        head.log_size = TensorF32::new(LOG_SIZE_SHAPE.to_vec(), log_size).unwrap();
        head.presence_logits =
            TensorF32::new(PRESENCE_LOGITS_SHAPE.to_vec(), presence.to_vec()).unwrap();
        head
    }

    fn set_head(calls: &FakeCalls, head: HeadOutput) {
        *calls.head_output.lock().expect("head output") = Some(head);
    }

    /// Two people with the person presence above its threshold: person 0 centered on the head grid
    /// cell (row 10, column 10), person 1 on (row 40, column 20), each 4 x 8 cells.
    fn two_people_head() -> HeadOutput {
        head_output([-5.0, 5.0], &[(40, 20, 2.0), (10, 10, 3.0)])
    }

    /// A 640 x 480 RGB8 image whose pixels differ: (x mod 251, y mod 251, (x + y) mod 251).
    fn pattern_image() -> Vec<u8> {
        (0..480usize)
            .flat_map(|y| {
                (0..640usize)
                    .flat_map(move |x| [(x % 251) as u8, (y % 251) as u8, ((x + y) % 251) as u8])
            })
            .collect()
    }

    fn start_pattern(
        engine: &SegmentationEngine,
        image_key: &str,
    ) -> Result<StartedSession, SegmentationError> {
        let pixels = pattern_image();
        engine.start_session(StartSessionRequest {
            image: Some(RgbImageView {
                width: 640,
                height: 480,
                rgb8: &pixels,
            }),
            image_key,
        })
    }

    /// The crop rectangles of `two_people_head` on the 640 x 480 image (SM3.D decision E3): the
    /// box of person 0 is x 8 to 12 cells (0.125 to 0.1875) and y 6 to 14 cells (0.09375 to
    /// 0.21875), widened by 0.00625 and 0.0125; person 1 is x 18 to 22 and y 36 to 44 cells.
    const PERSON_RECTS: [PixelRect; 2] = [
        PixelRect {
            x0: 76,
            y0: 39,
            x1: 124,
            y1: 111,
        },
        PixelRect {
            x0: 176,
            y0: 264,
            x1: 224,
            y1: 336,
        },
    ];

    fn entry_of(engine: &SegmentationEngine, session_id: &str) -> Arc<ImageEntry> {
        engine.session_entry(session_id).expect("session entry")
    }

    fn alpha_of(result: &TaskResult) -> (&[f32], MaskBox) {
        match &result.mask.as_ref().expect("mask").values {
            TaskMaskValues::Alpha { values, mask_box } => (values, *mask_box),
            TaskMaskValues::Binary(_) => panic!("the mask must be an alpha mask"),
        }
    }

    #[test]
    fn load_reports_four_models_and_fails_without_thresholds() {
        let (engine, _calls) = loaded_engine();
        assert_eq!(engine.status().expect("status").models.len(), 4);

        let empty = std::env::temp_dir().join(format!(
            "alcedo-segmentation-no-thresholds-{}",
            uuid::Uuid::new_v4()
        ));
        std::fs::create_dir_all(&empty).expect("create directory");
        let err = engine.load(&empty, "fake").expect_err("no thresholds");
        assert!(
            matches!(&err, SegmentationError::ModelNotReady(message) if message.contains(HEAD_THRESHOLDS_FILE)),
            "{err}"
        );
        assert!(!engine.status().expect("status").loaded);
        std::fs::remove_dir_all(&empty).expect("remove directory");

        for role in [SegmentationModelRole::Head, SegmentationModelRole::Subject] {
            let mut factory = FakeFactory::new();
            factory.failing_role = Some(role);
            let (engine, _calls) = engine_with(factory);
            assert!(matches!(
                engine.load(&model_directory("a"), "fake"),
                Err(SegmentationError::ModelNotReady(_))
            ));
            assert!(!engine.status().expect("status").loaded);
        }
    }

    #[test]
    fn start_session_runs_head_and_cuts_person_crops() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        let session = start_pattern(&engine, "").expect("start");
        assert_eq!(calls.head_runs.load(Ordering::SeqCst), 1);
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 0);
        let entry = entry_of(&engine, &session.session_id);
        let people = &entry.people;
        assert!(people.message.is_empty());
        // Left to right: person 0 (column 10) before person 1 (column 20).
        let rects: Vec<PixelRect> = people.persons.iter().map(|crop| crop.rect).collect();
        assert_eq!(rects, PERSON_RECTS.to_vec());
        assert_eq!(entry.person_alphas.len(), 2);
        let pixels = pattern_image();
        for crop in &people.persons {
            // Crops of at most 1024 px keep their pixels.
            assert_eq!((crop.image.width, crop.image.height), (48, 72));
            let (x, y) = (crop.rect.x0, crop.rect.y0);
            assert_eq!(&crop.image.rgb8[..3], &pixels[(y * 640 + x) * 3..][..3]);
        }
    }

    #[test]
    fn start_session_without_people_keeps_no_crops() {
        let (engine, calls) = loaded_engine();
        let session = start(&engine).expect("start");
        let entry = entry_of(&engine, &session.session_id);
        assert!(entry.people.persons.is_empty());
        assert!(entry.person_alphas.is_empty());
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert!(result.mask.is_none());
        assert!(result.people.is_empty());
        assert!(
            result
                .message
                .starts_with("No people found: person presence"),
            "{}",
            result.message
        );
        assert!(result.presence_threshold > 0.53 && result.presence_threshold < 0.54);
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 0);
        assert_eq!(calls.decoder_calls.lock().expect("calls").len(), 0);
    }

    #[test]
    fn head_failure_at_start_fails_start_and_caches_nothing() {
        let (engine, calls) = loaded_engine();
        calls.head_failures.store(1, Ordering::SeqCst);
        assert!(matches!(
            start_keyed(&engine, "photo-a"),
            Err(SegmentationError::Inference(message)) if message.contains("fake head run failed")
        ));
        let status = engine.status().expect("status");
        assert_eq!(status.session_count, 0);
        assert_eq!(status.cached_image_count, 0);
        assert!(matches!(
            start_key_only(&engine, "photo-a"),
            Err(SegmentationError::ImageNotCached(_))
        ));
    }

    #[test]
    fn run_task_sky_unions_regions_and_reports_not_found() {
        let (engine, calls) = loaded_engine();
        let session = start(&engine).expect("start");
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::Sky))
            .expect("sky without presence");
        assert!(result.mask.is_none());
        assert!(
            result.message.starts_with("No sky found: sky presence"),
            "{}",
            result.message
        );
        assert_eq!(calls.decoder_calls.lock().expect("calls").len(), 0);

        set_head(&calls, head_output([5.0, -5.0], &[]));
        let session = start(&engine).expect("second image");
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::Sky))
            .expect("sky");
        let mask = result.mask.expect("sky mask");
        // The union mask has the size of the 64 x 48 session image.
        assert_eq!((mask.width, mask.height), (64, 48));
        let TaskMaskValues::Binary(values) = &mask.values else {
            panic!("the sky mask must be an R8 mask");
        };
        assert_eq!(values.len(), 64 * 48);
        assert_eq!(result.region_count, 1);
        assert_eq!(result.mask_area, task_masks::mask_area(values));
        assert!(result.mask_area > 0);
        assert!(result.model_ms <= 1000);
        let decoder_calls = calls.decoder_calls.lock().expect("calls");
        assert_eq!(decoder_calls.len(), 1);
        assert_eq!(decoder_calls[0].prompt.point_count(), 3);
        assert_eq!(decoder_calls[0].prompt.has_mask_input.data(), &[0.0]);
    }

    #[test]
    fn run_task_sky_reads_stored_head() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, head_output([5.0, 5.0], &[(10, 10, 3.0)]));
        let session = start_keyed(&engine, "photo-a").expect("start");
        engine
            .run_task(&session.session_id, task(SegmentationTask::Sky))
            .expect("sky");
        engine.end_session(&session.session_id).expect("end");
        let cached = start_key_only(&engine, "photo-a").expect("cached start");
        let sky = engine
            .run_task(&cached.session_id, task(SegmentationTask::Sky))
            .expect("cached sky");
        assert_eq!(sky.model_ms, 0);
        assert_eq!(calls.head_runs.load(Ordering::SeqCst), 1);
        assert_eq!(calls.decoder_calls.lock().expect("calls").len(), 1);
    }

    #[test]
    fn run_task_people_returns_subject_alpha_and_list_without_decoder() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.8;
        let session = start_pattern(&engine, "").expect("start");
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert!(result.message.is_empty(), "{}", result.message);
        let mask = result.mask.as_ref().expect("people mask");
        // The 640 x 480 image resized to a long edge of 1024.
        assert_eq!((mask.width, mask.height), (1024, 768));
        let (values, mask_box) = alpha_of(&result);
        assert_eq!(mask_box, MaskBox::FULL_IMAGE);
        assert!(values.iter().all(|value| *value == 0.8));
        assert_eq!(result.mask_area, 1024 * 768);
        assert_eq!(result.subject_score, 0.95);
        assert!(result.presence >= result.presence_threshold);
        assert_eq!(result.people.len(), 2);
        // Person 0: x 8 to 12 cells.
        assert_eq!(
            (result.people[0].x0, result.people[0].x1),
            (8.0 / 64.0, 12.0 / 64.0)
        );
        assert!(
            result
                .people
                .iter()
                .all(|entry| entry.subject_fraction == 1.0 && entry.in_subject)
        );
        assert_eq!(calls.decoder_calls.lock().expect("calls").len(), 0);
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn run_task_people_shares_subject_alpha_with_subject_and_background() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.7;
        let session = start_pattern(&engine, "").expect("start");
        let subject = engine
            .run_task(&session.session_id, task(SegmentationTask::Subject))
            .expect("subject");
        engine
            .run_task(&session.session_id, task(SegmentationTask::Background))
            .expect("background");
        let people = engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
        assert_eq!(people.model_ms, 0);
        assert_eq!(alpha_of(&people), alpha_of(&subject));
    }

    #[test]
    fn run_task_people_found_when_subject_alpha_is_empty() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.2;
        let session = start_pattern(&engine, "").expect("start");
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert!(result.mask.is_some());
        assert!(result.message.is_empty());
        assert_eq!(result.mask_area, 0);
        assert_eq!(result.people.len(), 2);
        assert!(
            result
                .people
                .iter()
                .all(|entry| entry.subject_fraction == 0.0 && !entry.in_subject)
        );
    }

    #[test]
    fn run_task_people_keeps_best_box_below_list_threshold() {
        let (engine, calls) = loaded_engine();
        // sigmoid(-0.8) = 0.310 is below the list threshold 0.3132.
        set_head(
            &calls,
            head_output([-5.0, 5.0], &[(10, 10, -0.8), (40, 40, -1.5)]),
        );
        let session = start(&engine).expect("start");
        let result = engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert_eq!(result.people.len(), 1);
        assert!((result.people[0].score - task_masks::sigmoid(-0.8)).abs() < 1e-6);
        assert!(result.mask.is_some());
        assert_eq!(
            entry_of(&engine, &session.session_id).people.persons.len(),
            1
        );
    }

    #[test]
    fn run_task_person_runs_s3od_once_per_person() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.8;
        let session = start_pattern(&engine, "").expect("start");
        engine
            .run_task(&session.session_id, task(SegmentationTask::People))
            .expect("people");
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
        let first = engine
            .run_task(&session.session_id, person(1))
            .expect("person 1");
        let second = engine
            .run_task(&session.session_id, person(1))
            .expect("person 1 again");
        assert_eq!(second.model_ms, 0);
        assert_eq!(alpha_of(&second), alpha_of(&first));
        engine
            .run_task(&session.session_id, person(0))
            .expect("person 0");
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 3);
        // The S3OD inputs of the photo, person 1, and person 0 differ.
        let inputs = calls.subject_center_inputs.lock().expect("inputs").clone();
        assert_eq!(inputs.len(), 3);
        assert!(
            inputs[0] != inputs[1] && inputs[0] != inputs[2] && inputs[1] != inputs[2],
            "{inputs:?}"
        );
        // The People list of a Person result carries the stored Subject fractions.
        assert!(first.people.iter().all(|entry| entry.in_subject));
        assert_eq!(first.subject_score, 0.95);
        assert!(first.message.is_empty());
    }

    #[test]
    fn run_task_person_returns_crop_grid_and_mask_box() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.9;
        let session = start_pattern(&engine, "").expect("start");
        let result = engine
            .run_task(&session.session_id, person(0))
            .expect("person 0");
        let mask = result.mask.as_ref().expect("person mask");
        // The 48 x 72 crop resized to a long edge of 1024: 683 x 1024 (682.67 rounded).
        assert_eq!((mask.width, mask.height), (683, 1024));
        let (values, mask_box) = alpha_of(&result);
        assert_eq!(values.len(), 683 * 1024);
        assert_eq!(
            mask_box,
            MaskBox {
                x0: 76.0 / 640.0,
                y0: 39.0 / 480.0,
                x1: 124.0 / 640.0,
                y1: 111.0 / 480.0,
            }
        );
        assert_eq!(result.mask_area, 683 * 1024);
        // No Subject result yet: the People list has no fractions.
        assert_eq!(result.people.len(), 2);
        assert!(
            result
                .people
                .iter()
                .all(|entry| entry.subject_fraction == 0.0 && !entry.in_subject)
        );
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn run_task_person_not_found_when_crop_alpha_below_half() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.49;
        let session = start_pattern(&engine, "").expect("start");
        for _ in 0..2 {
            let result = engine
                .run_task(&session.session_id, person(1))
                .expect("person 1");
            assert!(result.mask.is_none());
            assert_eq!(
                result.message,
                "No person found in the crop of person 1: no alpha value reaches 0.5 (S3OD score 0.950)."
            );
            assert_eq!(result.mask_area, 0);
            assert_eq!(result.subject_score, 0.95);
        }
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn run_task_person_failure_keeps_no_alpha() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.8;
        calls.subject_failures.store(1, Ordering::SeqCst);
        let session = start_pattern(&engine, "").expect("start");
        assert!(matches!(
            engine.run_task(&session.session_id, person(0)),
            Err(SegmentationError::Inference(message)) if message.contains("fake S3OD run failed")
        ));
        let result = engine
            .run_task(&session.session_id, person(0))
            .expect("person 0 after the failure");
        assert!(result.mask.is_some());
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn run_task_rejects_person_index_rules() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        let session = start_pattern(&engine, "").expect("start");
        let requests = [
            TaskRequest {
                task: SegmentationTask::Person,
                person_index: None,
            },
            TaskRequest {
                task: SegmentationTask::Sky,
                person_index: Some(0),
            },
            TaskRequest {
                task: SegmentationTask::People,
                person_index: Some(0),
            },
            person(5),
        ];
        for request in requests {
            assert!(
                matches!(
                    engine.run_task(&session.session_id, request.clone()),
                    Err(SegmentationError::InvalidInput(_))
                ),
                "{request:?}"
            );
        }
        assert!(matches!(
            engine.run_task(&session.session_id, person(2)),
            Err(SegmentationError::InvalidInput(message)) if message.contains("2 people")
        ));
        // A person index on another task fails before the session lookup.
        assert!(matches!(
            engine.run_task(
                "missing",
                TaskRequest {
                    task: SegmentationTask::Subject,
                    person_index: Some(0),
                }
            ),
            Err(SegmentationError::InvalidInput(_))
        ));
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 0);
        assert_eq!(calls.decoder_calls.lock().expect("calls").len(), 0);
        assert_eq!(calls.head_runs.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn run_task_rejects_invalid_task_and_index() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        let session = start(&engine).expect("start");
        assert!(matches!(
            engine.run_task("missing", task(SegmentationTask::People)),
            Err(SegmentationError::SessionNotFound(_))
        ));
        assert!(matches!(
            engine.run_task("missing", person(0)),
            Err(SegmentationError::SessionNotFound(_))
        ));
        engine.unload().expect("unload");
        assert!(matches!(
            engine.run_task(&session.session_id, task(SegmentationTask::People)),
            Err(SegmentationError::ModelNotReady(_))
        ));
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn cached_key_only_start_runs_person_from_stored_crop() {
        let (engine, calls) = loaded_engine();
        set_head(&calls, two_people_head());
        *calls.subject_alpha.lock().expect("alpha") = 0.8;
        let first = start_pattern(&engine, "photo-a").expect("start");
        engine.end_session(&first.session_id).expect("end");
        let cached = start_key_only(&engine, "photo-a").expect("cached start");
        assert!(cached.from_cache);
        let from_cache = engine
            .run_task(&cached.session_id, person(0))
            .expect("person 0 from the cached image");
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);

        // A pixel start of the same image gives the same S3OD input and the same alpha.
        let pixels = start_pattern(&engine, "").expect("pixel start");
        let from_pixels = engine
            .run_task(&pixels.session_id, person(0))
            .expect("person 0 from the pixels");
        let inputs = calls.subject_center_inputs.lock().expect("inputs").clone();
        assert_eq!(inputs.len(), 2);
        assert_eq!(inputs[0], inputs[1]);
        assert_eq!(alpha_of(&from_cache), alpha_of(&from_pixels));
        assert_eq!(calls.encoder_runs.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn cached_start_runs_subject_without_pixels() {
        let (engine, calls) = loaded_engine();
        *calls.subject_alpha.lock().expect("alpha") = 0.8;
        let first = start_keyed(&engine, "photo-a").expect("start");
        engine.end_session(&first.session_id).expect("end");
        let cached = start_key_only(&engine, "photo-a").expect("cached start");
        let subject = engine
            .run_task(&cached.session_id, task(SegmentationTask::Subject))
            .expect("subject");
        let mask = subject.mask.as_ref().expect("subject mask");
        // The 64 x 48 image resized to a long edge of 1024: 1024 x 768.
        assert_eq!((mask.width, mask.height), (1024, 768));
        let (values, mask_box) = alpha_of(&subject);
        assert_eq!(mask_box, MaskBox::FULL_IMAGE);
        assert!(values.iter().all(|value| *value == 0.8));
        assert_eq!(subject.subject_score, 0.95);
        assert_eq!(subject.mask_area, 1024 * 768);
        // Background 0.2 everywhere: the subject covers the whole image.
        let background = engine
            .run_task(&cached.session_id, task(SegmentationTask::Background))
            .expect("background");
        assert!(background.mask.is_none());
        assert!(
            background.message.starts_with("No background found"),
            "{}",
            background.message
        );
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn subject_not_found_when_no_alpha_reaches_half() {
        let (engine, calls) = loaded_engine();
        *calls.subject_alpha.lock().expect("alpha") = 0.49;
        let session = start(&engine).expect("start");
        for request in [SegmentationTask::Subject, SegmentationTask::Background] {
            let result = engine
                .run_task(&session.session_id, task(request))
                .expect("task");
            assert!(result.mask.is_none());
            assert!(
                result.message.starts_with("No subject found"),
                "{}",
                result.message
            );
            assert_eq!(result.subject_score, 0.95);
            assert_eq!(result.mask_area, 0);
        }
    }

    #[test]
    fn subject_failure_keeps_no_alpha() {
        let (engine, calls) = loaded_engine();
        *calls.subject_alpha.lock().expect("alpha") = 0.3;
        calls.subject_failures.store(1, Ordering::SeqCst);
        let session = start(&engine).expect("start");
        assert!(matches!(
            engine.run_task(&session.session_id, task(SegmentationTask::Subject)),
            Err(SegmentationError::Inference(message)) if message.contains("fake S3OD run failed")
        ));
        let background = engine
            .run_task(&session.session_id, task(SegmentationTask::Background))
            .expect("background after the failure");
        assert_eq!(calls.subject_runs.load(Ordering::SeqCst), 2);
        // Subject alpha 0.3 everywhere: no subject, so no background either (C9).
        assert!(background.mask.is_none());
        assert!(background.message.starts_with("No subject found"));
    }
}
