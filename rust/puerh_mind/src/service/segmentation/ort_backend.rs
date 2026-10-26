//! ONNX Runtime adapter of the segmentation models: DirectML on Windows, CPU for development.
//!
//! Session settings follow the lab MVP (master plan section 8.2.3): graph optimization level 3,
//! 4 intra-op threads, 1 inter-op thread, CPU memory arena off, memory patterns off, sequential
//! execution. A DirectML session registers DirectML with `error_on_failure`, so a registration
//! failure is a load error and never a CPU session. The CPU entry of the list only applies the
//! CPU memory arena setting (`DisableCpuMemArena`) to the CPU execution provider that ONNX Runtime
//! always keeps for node placement.
//!
//! The DirectML sessions of the encoder, S3OD, and the sky head also switch off the DirectML memory
//! arena (issue #532). With the arena, DirectML rounds each work buffer of a run up to a power of
//! two and keeps it until the session closes: about 1 GB for each of these models (the sky head:
//! 1,150 MB against 122 MB after its first run, SM3.E plan decision S7). Without the arena, the
//! work buffers are freed after each run, and the outputs are the same. A run takes about 20 - 60
//! ms more. The decoder and the people and sky head keep the arena: their work buffers are small,
//! and without the arena a decoder run takes about 100 ms instead of 10 ms.
//!
//! The encoder keeps its three output values as [`OrtImageFeatures`] (SM3.B decision B4); the
//! decoder ([`super::ort_decoder`]) binds them without a host copy, the people and sky head reads
//! `image_embed` from them (SM3.C), and the sky head reads all three with the encoder input
//! (SM3.E). S3OD reads its own input image (SM3.C plan, section 2.5).

use std::any::Any;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result, anyhow, bail};
use ort::{
    ep,
    session::{
        Session, SessionInputValue, SessionOutputs,
        builder::{GraphOptimizationLevel, SessionBuilder},
    },
    value::{DynValue, Tensor, TensorRef},
};

use super::backend::{
    HeadOutput, ImageFeatures, SegmentationBackend, SegmentationBackendFactory,
    SegmentationDecoder, SegmentationEncoder, SegmentationHead, SegmentationModelRole,
    SkyHeadModel, SubjectModel, SubjectOutput, UnsupportedDevice, features_of,
};
use super::model_set::{
    CENTER_LOGITS_SHAPE, CENTER_OFFSET_SHAPE, DECODER, ENCODER, HEAD, HIGH_RES_FEATS_0_SHAPE,
    HIGH_RES_FEATS_1_SHAPE, IMAGE_EMBED_SHAPE, LOG_SIZE_SHAPE, ModelSpec, PIXEL_VALUES_SHAPE,
    PRESENCE_LOGITS_SHAPE, SKY_ALPHA_SHAPE, SKY_HEAD, SKY_LOGITS_SHAPE, SUBJECT,
    SUBJECT_MASKS_SHAPE, SUBJECT_SCORES_SHAPE,
};
use super::ort_decoder::OrtDecoder;
use super::tensor::TensorF32;

const INTRA_OP_THREADS: usize = 4;
const INTER_OP_THREADS: usize = 1;

/// Device of an ONNX Runtime segmentation session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OrtSegmentationDevice {
    Cpu,
    /// DirectML on the adapter with this index (Windows only).
    DirectMl(i32),
}

impl OrtSegmentationDevice {
    /// Parses a device value of the load request. `""` is the platform default: DirectML on
    /// adapter 0 on Windows. On other platforms only `cpu` is accepted.
    pub fn parse(value: &str) -> Result<Self, UnsupportedDevice> {
        let normalized = value.trim().to_ascii_lowercase();
        if normalized == "cpu" {
            return Ok(Self::Cpu);
        }
        if !cfg!(windows) {
            return Err(UnsupportedDevice(format!(
                "segmentation device {value:?} is not supported by the ONNX Runtime backend on this platform; use \"cpu\""
            )));
        }
        if normalized.is_empty() || normalized == "directml" {
            return Ok(Self::DirectMl(0));
        }
        if let Some(ordinal) = normalized.strip_prefix("directml:") {
            return match ordinal.parse::<i32>() {
                Ok(ordinal) if ordinal >= 0 => Ok(Self::DirectMl(ordinal)),
                _ => Err(UnsupportedDevice(format!(
                    "segmentation device {value:?} has an invalid DirectML adapter index"
                ))),
            };
        }
        Err(UnsupportedDevice(format!(
            "segmentation device {value:?} is not supported; use \"directml\", \"directml:N\", or \"cpu\""
        )))
    }

    pub fn label(self) -> String {
        match self {
            Self::Cpu => "cpu".to_string(),
            Self::DirectMl(ordinal) => format!("directml:{ordinal}"),
        }
    }
}

/// Creates [`OrtSegmentationBackend`] values for the device of each load request.
pub struct OrtSegmentationBackendFactory;

impl SegmentationBackendFactory for OrtSegmentationBackendFactory {
    fn create(&self, device: &str) -> Result<Box<dyn SegmentationBackend>, UnsupportedDevice> {
        Ok(Box::new(OrtSegmentationBackend {
            device: OrtSegmentationDevice::parse(device)?,
        }))
    }
}

/// Loads the ONNX files of the model set on one device.
pub struct OrtSegmentationBackend {
    device: OrtSegmentationDevice,
}

/// Session configuration key that switches off the DirectML memory arena.
const DIRECTML_DISABLE_MEMORY_ARENA: &str = "ep.dml.disable_memory_arena";

/// Whether a DirectML session of @p role frees its work buffers after each run (see the module
/// documentation).
fn frees_work_buffers_after_run(role: SegmentationModelRole) -> bool {
    matches!(
        role,
        SegmentationModelRole::Encoder
            | SegmentationModelRole::Subject
            | SegmentationModelRole::SkyHead
    )
}

fn session_builder(
    device: OrtSegmentationDevice,
    role: SegmentationModelRole,
) -> Result<SessionBuilder> {
    crate::service::ort_runtime::initialize_ort_environment()?;
    let mut builder = Session::builder()
        .map_err(|e| anyhow!("failed to create the ONNX Runtime session builder: {e}"))?
        .with_optimization_level(GraphOptimizationLevel::Level3)
        .map_err(|e| anyhow!("failed to set the graph optimization level: {e}"))?
        .with_intra_threads(INTRA_OP_THREADS)
        .map_err(|e| anyhow!("failed to set the intra-op thread count: {e}"))?
        .with_inter_threads(INTER_OP_THREADS)
        .map_err(|e| anyhow!("failed to set the inter-op thread count: {e}"))?
        .with_memory_pattern(false)
        .map_err(|e| anyhow!("failed to switch off memory patterns: {e}"))?
        .with_parallel_execution(false)
        .map_err(|e| anyhow!("failed to select sequential execution: {e}"))?;
    if matches!(device, OrtSegmentationDevice::DirectMl(_)) && frees_work_buffers_after_run(role) {
        builder = builder
            .with_config_entry(DIRECTML_DISABLE_MEMORY_ARENA, "1")
            .map_err(|e| anyhow!("failed to switch off the DirectML memory arena: {e}"))?;
    }
    builder
        .with_execution_providers(execution_providers(device)?)
        .map_err(|e| {
            anyhow!(
                "failed to register the {} execution provider: {e}",
                device.label()
            )
        })
}

fn cpu_arena_off() -> ep::ExecutionProviderDispatch {
    ep::CPU::default().with_arena_allocator(false).build()
}

#[cfg(windows)]
fn execution_providers(
    device: OrtSegmentationDevice,
) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    Ok(match device {
        OrtSegmentationDevice::Cpu => vec![cpu_arena_off().error_on_failure()],
        OrtSegmentationDevice::DirectMl(ordinal) => vec![
            ep::DirectML::default()
                .with_device_id(ordinal)
                .build()
                .error_on_failure(),
            cpu_arena_off(),
        ],
    })
}

#[cfg(not(windows))]
fn execution_providers(
    device: OrtSegmentationDevice,
) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    match device {
        OrtSegmentationDevice::Cpu => Ok(vec![cpu_arena_off().error_on_failure()]),
        OrtSegmentationDevice::DirectMl(_) => bail!("DirectML is only available on Windows"),
    }
}

/// Path of the ONNX file of @p spec in @p directory.
pub fn model_path(directory: &Path, spec: &ModelSpec) -> PathBuf {
    directory.join(spec.onnx_file)
}

/// Loads the ONNX file of @p spec from @p directory into a new session on @p device and checks
/// the input and output names of the specification.
pub(super) fn load_session(
    directory: &Path,
    spec: &ModelSpec,
    device: OrtSegmentationDevice,
) -> Result<Session> {
    let path = model_path(directory, spec);
    if !path.is_file() {
        bail!(
            "segmentation {} model {} does not exist",
            spec.role.as_str(),
            path.display()
        );
    }
    let session = session_builder(device, spec.role)?
        .commit_from_file(&path)
        .map_err(|e| {
            anyhow!(
                "failed to load {} on {}: {e}",
                path.display(),
                device.label()
            )
        })?;
    validate_session_names(&session, spec, &path)?;
    Ok(session)
}

fn validate_session_names(session: &Session, spec: &ModelSpec, path: &Path) -> Result<()> {
    let inputs: Vec<&str> = session.inputs().iter().map(|input| input.name()).collect();
    for name in spec.inputs {
        if !inputs.contains(name) {
            bail!(
                "{} has no input {name:?}; inputs: {inputs:?}",
                path.display()
            );
        }
    }
    let outputs: Vec<&str> = session
        .outputs()
        .iter()
        .map(|output| output.name())
        .collect();
    for name in spec.onnx_outputs {
        if !outputs.contains(name) {
            bail!(
                "{} has no output {name:?}; outputs: {outputs:?}",
                path.display()
            );
        }
    }
    Ok(())
}

impl SegmentationBackend for OrtSegmentationBackend {
    fn backend_name(&self) -> &str {
        match self.device {
            OrtSegmentationDevice::Cpu => "onnxruntime-cpu",
            OrtSegmentationDevice::DirectMl(_) => "onnxruntime-directml",
        }
    }

    fn device_label(&self, _role: SegmentationModelRole) -> String {
        self.device.label()
    }

    fn load_encoder(&self, directory: &Path) -> Result<Box<dyn SegmentationEncoder>> {
        Ok(Box::new(OrtEncoder {
            session: load_session(directory, &ENCODER, self.device)?,
        }))
    }

    fn load_decoder(&self, directory: &Path) -> Result<Box<dyn SegmentationDecoder>> {
        Ok(Box::new(OrtDecoder::load(
            directory,
            &DECODER,
            self.device,
        )?))
    }

    fn load_head(&self, directory: &Path) -> Result<Box<dyn SegmentationHead>> {
        let mut head = OrtHead {
            session: load_session(directory, &HEAD, self.device)?,
        };
        // The warm-up run (SM3.C plan, decision C10).
        let zero_features: Arc<dyn ImageFeatures> = Arc::new(OrtImageFeatures::zeros()?);
        head.run(&zero_features)
            .context("failed to run the people and sky head at load")?;
        Ok(Box::new(head))
    }

    fn load_subject_model(&self, directory: &Path) -> Result<Box<dyn SubjectModel>> {
        Ok(Box::new(OrtSubjectModel {
            session: load_session(directory, &SUBJECT, self.device)?,
        }))
    }

    fn load_sky_head(&self, directory: &Path) -> Result<Option<Box<dyn SkyHeadModel>>> {
        let mut head = OrtSkyHead {
            session: load_session(directory, &SKY_HEAD, self.device)?,
        };
        // The warm-up run, as for the people and sky head (SM3.C plan, decision C10).
        let zero_features: Arc<dyn ImageFeatures> = Arc::new(OrtImageFeatures::zeros()?);
        head.run(
            &zero_features,
            &TensorF32::zeros(PIXEL_VALUES_SHAPE.to_vec()),
        )
        .context("failed to run the sky head at load")?;
        Ok(Some(Box::new(head)))
    }
}

/// Copies the float32 output @p name of @p outputs into a tensor and checks its shape against
/// @p expected. @p model names the model in the errors.
pub(super) fn read_output(
    outputs: &SessionOutputs<'_>,
    model: &str,
    name: &str,
    expected: &[usize],
) -> Result<TensorF32> {
    let value = outputs
        .get(name)
        .with_context(|| format!("missing {model} output {name}"))?;
    let (shape, data) = value
        .try_extract_tensor::<f32>()
        .map_err(|e| anyhow!("{model} output {name} is not a float32 tensor: {e}"))?;
    let shape = shape
        .iter()
        .map(|&dim| {
            usize::try_from(dim)
                .map_err(|_| anyhow!("{model} output {name} has a negative dimension"))
        })
        .collect::<Result<Vec<_>>>()?;
    let tensor = TensorF32::new(shape, data.to_vec())?;
    tensor.expect_shape(&format!("{model} output {name}"), expected)?;
    Ok(tensor)
}

/// The expected shapes of the encoder outputs, in the output order of the specification.
const FEATURE_SHAPES: [&[usize]; 3] = [
    &IMAGE_EMBED_SHAPE,
    &HIGH_RES_FEATS_0_SHAPE,
    &HIGH_RES_FEATS_1_SHAPE,
];

/// The encoder outputs of one image as ONNX Runtime values in CPU memory, in the output order of
/// the encoder specification (`image_embed`, `high_res_feats_0`, `high_res_feats_1`). The values
/// are not changed after the encoder run.
pub struct OrtImageFeatures {
    values: [DynValue; 3],
}

impl OrtImageFeatures {
    /// Features with zero values, for the warm-up runs of the decoder sessions.
    pub(super) fn zeros() -> Result<Self> {
        let zero = |shape: &[usize]| -> Result<DynValue> {
            let len = shape.iter().product();
            Ok(Tensor::from_array((shape.to_vec(), vec![0.0f32; len]))
                .map_err(|e| anyhow!("failed to allocate zero features: {e}"))?
                .into_dyn())
        };
        Ok(Self {
            values: [
                zero(FEATURE_SHAPES[0])?,
                zero(FEATURE_SHAPES[1])?,
                zero(FEATURE_SHAPES[2])?,
            ],
        })
    }

    /// The values with their decoder input names.
    pub(super) fn named_values(&self) -> impl Iterator<Item = (&'static str, &DynValue)> {
        ENCODER.onnx_outputs.iter().copied().zip(self.values.iter())
    }

    /// The `image_embed` value, the input of the people and sky head.
    fn image_embed(&self) -> &DynValue {
        &self.values[0]
    }
}

impl ImageFeatures for OrtImageFeatures {
    fn as_any(&self) -> &dyn Any {
        self
    }
}

struct OrtEncoder {
    session: Session,
}

impl SegmentationEncoder for OrtEncoder {
    fn encode(&mut self, pixel_values: &TensorF32) -> Result<Arc<dyn ImageFeatures>> {
        let input_name = ENCODER.inputs[0];
        let view = TensorRef::from_array_view((pixel_values.shape(), pixel_values.data()))
            .map_err(|e| anyhow!("failed to bind input {input_name}: {e}"))?;
        let mut outputs = self
            .session
            .run(vec![(input_name, view)])
            .map_err(|e| anyhow!("failed to run the segmentation encoder: {e}"))?;
        let mut take = |index: usize| -> Result<DynValue> {
            let name = ENCODER.onnx_outputs[index];
            let value = outputs
                .remove(name)
                .with_context(|| format!("missing encoder output {name}"))?;
            let (shape, _) = value
                .try_extract_tensor::<f32>()
                .map_err(|e| anyhow!("encoder output {name} is not a float32 tensor: {e}"))?;
            let shape: Vec<usize> = shape.iter().map(|&dim| dim.max(0) as usize).collect();
            if shape != FEATURE_SHAPES[index] {
                bail!(
                    "encoder output {name} has shape {shape:?}, expected {:?}",
                    FEATURE_SHAPES[index]
                );
            }
            Ok(value)
        };
        let values = [take(0)?, take(1)?, take(2)?];
        Ok(Arc::new(OrtImageFeatures { values }))
    }
}

struct OrtHead {
    session: Session,
}

impl SegmentationHead for OrtHead {
    fn run(&mut self, features: &Arc<dyn ImageFeatures>) -> Result<HeadOutput> {
        let features = features_of::<OrtImageFeatures>(features.as_ref())?;
        let outputs = self
            .session
            .run(vec![(HEAD.inputs[0], features.image_embed())])
            .map_err(|e| anyhow!("failed to run the people and sky head: {e}"))?;
        let read = |index: usize, expected: &[usize]| {
            read_output(&outputs, "head", HEAD.onnx_outputs[index], expected)
        };
        Ok(HeadOutput {
            sky_logits: read(0, &SKY_LOGITS_SHAPE)?,
            center_logits: read(1, &CENTER_LOGITS_SHAPE)?,
            center_offset: read(2, &CENTER_OFFSET_SHAPE)?,
            log_size: read(3, &LOG_SIZE_SHAPE)?,
            presence_logits: read(4, &PRESENCE_LOGITS_SHAPE)?,
        })
    }
}

struct OrtSkyHead {
    session: Session,
}

impl SkyHeadModel for OrtSkyHead {
    fn run(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        pixel_values: &TensorF32,
    ) -> Result<TensorF32> {
        let features = features_of::<OrtImageFeatures>(features.as_ref())?;
        let pixel_name = SKY_HEAD.inputs[3];
        let pixels = TensorRef::from_array_view((pixel_values.shape(), pixel_values.data()))
            .map_err(|e| anyhow!("failed to bind input {pixel_name}: {e}"))?;
        let mut inputs: Vec<(&str, SessionInputValue<'_>)> = features
            .named_values()
            .map(|(name, value)| (name, value.into()))
            .collect();
        inputs.push((pixel_name, pixels.into()));
        let outputs = self
            .session
            .run(inputs)
            .map_err(|e| anyhow!("failed to run the sky head: {e}"))?;
        read_output(
            &outputs,
            "sky head",
            SKY_HEAD.onnx_outputs[0],
            &SKY_ALPHA_SHAPE,
        )
    }
}

struct OrtSubjectModel {
    session: Session,
}

impl SubjectModel for OrtSubjectModel {
    fn run(&mut self, pixel_values: &TensorF32) -> Result<SubjectOutput> {
        let input_name = SUBJECT.inputs[0];
        let view = TensorRef::from_array_view((pixel_values.shape(), pixel_values.data()))
            .map_err(|e| anyhow!("failed to bind input {input_name}: {e}"))?;
        let outputs = self
            .session
            .run(vec![(input_name, view)])
            .map_err(|e| anyhow!("failed to run S3OD: {e}"))?;
        Ok(SubjectOutput {
            masks: read_output(
                &outputs,
                "S3OD",
                SUBJECT.onnx_outputs[0],
                &SUBJECT_MASKS_SHAPE,
            )?,
            iou_scores: read_output(
                &outputs,
                "S3OD",
                SUBJECT.onnx_outputs[1],
                &SUBJECT_SCORES_SHAPE,
            )?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ort_device_values_parse_to_one_provider() {
        assert_eq!(
            OrtSegmentationDevice::parse("cpu"),
            Ok(OrtSegmentationDevice::Cpu)
        );
        assert_eq!(
            OrtSegmentationDevice::parse(" CPU "),
            Ok(OrtSegmentationDevice::Cpu)
        );
        if cfg!(windows) {
            assert_eq!(
                OrtSegmentationDevice::parse(""),
                Ok(OrtSegmentationDevice::DirectMl(0))
            );
            assert_eq!(
                OrtSegmentationDevice::parse("directml"),
                Ok(OrtSegmentationDevice::DirectMl(0))
            );
            assert_eq!(
                OrtSegmentationDevice::parse("directml:2"),
                Ok(OrtSegmentationDevice::DirectMl(2))
            );
        } else {
            assert!(OrtSegmentationDevice::parse("").is_err());
            assert!(OrtSegmentationDevice::parse("directml").is_err());
        }
        for rejected in ["coreml", "cuda", "directml:-1", "directml:", "auto"] {
            assert!(
                OrtSegmentationDevice::parse(rejected).is_err(),
                "{rejected} must be rejected"
            );
        }
    }

    #[test]
    fn only_encoder_subject_and_sky_head_free_work_buffers_after_run() {
        assert!(frees_work_buffers_after_run(SegmentationModelRole::Encoder));
        assert!(frees_work_buffers_after_run(SegmentationModelRole::Subject));
        assert!(frees_work_buffers_after_run(SegmentationModelRole::SkyHead));
        assert!(!frees_work_buffers_after_run(
            SegmentationModelRole::Decoder
        ));
        assert!(!frees_work_buffers_after_run(SegmentationModelRole::Head));
    }

    #[test]
    fn ort_backend_reports_missing_model_file_path() {
        let directory = std::env::temp_dir().join(format!(
            "alcedo-segmentation-empty-{}",
            uuid::Uuid::new_v4()
        ));
        std::fs::create_dir_all(&directory).expect("create directory");
        let backend = OrtSegmentationBackend {
            device: OrtSegmentationDevice::Cpu,
        };
        let err = backend
            .load_encoder(&directory)
            .err()
            .expect("missing encoder file");
        assert!(err.to_string().contains("point_encoder.onnx"), "{err}");
        let err = backend
            .load_decoder(&directory)
            .err()
            .expect("missing decoder file");
        assert!(err.to_string().contains("point_decoder.onnx"), "{err}");
        let err = backend
            .load_head(&directory)
            .err()
            .expect("missing head file");
        assert!(err.to_string().contains("people_sky_head.onnx"), "{err}");
        let err = backend
            .load_subject_model(&directory)
            .err()
            .expect("missing S3OD file");
        assert!(err.to_string().contains("subject_masks.onnx"), "{err}");
        let err = backend
            .load_sky_head(&directory)
            .err()
            .expect("missing sky head file");
        assert!(err.to_string().contains("sky_head.onnx"), "{err}");
        std::fs::remove_dir_all(&directory).expect("remove directory");
    }
}
