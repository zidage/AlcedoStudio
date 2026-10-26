//! Native Core ML adapter of the segmentation models (macOS).
//!
//! The models are compiled `.mlmodelc` directories (master plan section 8.2.1), loaded with the
//! `coreml` crate as the SigLIP2 encoders are (`coreml_clip.rs`). The ONNX Runtime Core ML
//! execution provider is not used (lab decision U15). Compute units per model (U15): the encoder
//! runs with `ComputeUnits::All`, the decoder with `ComputeUnits::CpuOnly` (about 30 ms per call
//! and no new preparation when the point count changes). The `cpu` device runs every model with
//! `ComputeUnits::CpuOnly`; it exists for development and tests only.
//!
//! The encoder keeps its output arrays as [`CoreMlImageFeatures`] (SM3.B decision B4). A Float16
//! output is converted to Float32 once, at the encoder run. The decoder adds retained references
//! to the same arrays to its input for each call, so a decode does not copy the features. The
//! people and sky head adds the stored `image_embed` array to its input in the same way (SM3.C),
//! and the sky head adds the three stored arrays and a copy of the encoder input (SM3.E, issue
//! #538). The head, S3OD, and the sky head use `ComputeUnits::All` (lab U15; `CpuOnly` on the `cpu`
//! device).

use std::any::Any;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Result, anyhow, bail};
use coreml::{ComputeUnits, DataType, FeatureProvider, Model, ModelConfiguration, MultiArray};

use super::backend::{
    DecoderOutput, DecoderPrompt, HeadOutput, ImageFeatures, SegmentationBackend,
    SegmentationBackendFactory, SegmentationDecoder, SegmentationEncoder, SegmentationHead,
    SegmentationModelRole, SkyHeadModel, SubjectModel, SubjectOutput, UnsupportedDevice,
    features_of,
};
use super::model_set::{
    CENTER_LOGITS_SHAPE, CENTER_OFFSET_SHAPE, DECODER, ENCODER, HEAD, HIGH_RES_FEATS_0_SHAPE,
    HIGH_RES_FEATS_1_SHAPE, IMAGE_EMBED_SHAPE, LOG_SIZE_SHAPE, ModelSpec, PIXEL_VALUES_SHAPE,
    PRESENCE_LOGITS_SHAPE, SKY_ALPHA_SHAPE, SKY_HEAD, SKY_LOGITS_SHAPE, SUBJECT,
    SUBJECT_MASKS_SHAPE, SUBJECT_SCORES_SHAPE, spec,
};
use super::tensor::TensorF32;

/// Device of the Core ML segmentation models.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoreMlSegmentationDevice {
    /// Compute units per model of lab decision U15.
    PerModel,
    /// `ComputeUnits::CpuOnly` for every model.
    CpuOnly,
}

impl CoreMlSegmentationDevice {
    /// Parses a device value of the load request: `""` or `coreml` for the U15 compute units,
    /// `cpu` for CPU only.
    pub fn parse(value: &str) -> Result<Self, UnsupportedDevice> {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | "coreml" => Ok(Self::PerModel),
            "cpu" => Ok(Self::CpuOnly),
            _ => Err(UnsupportedDevice(format!(
                "segmentation device {value:?} is not supported by the Core ML backend; use \"coreml\" or \"cpu\""
            ))),
        }
    }

    fn compute_units(self, role: SegmentationModelRole) -> ComputeUnits {
        match (self, role) {
            (
                Self::PerModel,
                SegmentationModelRole::Encoder
                | SegmentationModelRole::Head
                | SegmentationModelRole::Subject
                | SegmentationModelRole::SkyHead,
            ) => ComputeUnits::All,
            (Self::PerModel, SegmentationModelRole::Decoder) => ComputeUnits::CpuOnly,
            (Self::CpuOnly, _) => ComputeUnits::CpuOnly,
        }
    }

    fn compute_units_label(self, role: SegmentationModelRole) -> &'static str {
        match self.compute_units(role) {
            ComputeUnits::All => "coreml-native:all",
            ComputeUnits::CpuOnly => "coreml-native:cpuonly",
            ComputeUnits::CpuAndGpu => "coreml-native:cpuandgpu",
            ComputeUnits::CpuAndNeuralEngine => "coreml-native:cpuandneuralengine",
        }
    }
}

/// Creates [`CoreMlSegmentationBackend`] values for the device of each load request.
pub struct CoreMlSegmentationBackendFactory;

impl SegmentationBackendFactory for CoreMlSegmentationBackendFactory {
    fn create(&self, device: &str) -> Result<Box<dyn SegmentationBackend>, UnsupportedDevice> {
        Ok(Box::new(CoreMlSegmentationBackend {
            device: CoreMlSegmentationDevice::parse(device)?,
        }))
    }
}

/// Loads the compiled Core ML models of the model set.
pub struct CoreMlSegmentationBackend {
    device: CoreMlSegmentationDevice,
}

/// Path of the compiled model directory of @p spec in @p directory.
pub fn model_path(directory: &Path, spec: &ModelSpec) -> PathBuf {
    directory.join(spec.coreml_directory)
}

impl CoreMlSegmentationBackend {
    fn load_model(&self, directory: &Path, role: SegmentationModelRole) -> Result<Model> {
        let path = model_path(directory, spec(role));
        if !path.is_dir() {
            bail!(
                "segmentation {} model {} does not exist",
                role.as_str(),
                path.display()
            );
        }
        let configuration =
            ModelConfiguration::new().with_compute_units(self.device.compute_units(role));
        Model::load_from_url(&path, &configuration).map_err(|e| {
            anyhow!(
                "failed to load {} with {}: {e}",
                path.display(),
                self.device.compute_units_label(role)
            )
        })
    }
}

impl SegmentationBackend for CoreMlSegmentationBackend {
    fn backend_name(&self) -> &str {
        "coreml-native"
    }

    fn device_label(&self, role: SegmentationModelRole) -> String {
        self.device.compute_units_label(role).to_string()
    }

    fn load_encoder(&self, directory: &Path) -> Result<Box<dyn SegmentationEncoder>> {
        Ok(Box::new(CoreMlEncoder {
            model: self.load_model(directory, SegmentationModelRole::Encoder)?,
        }))
    }

    fn load_decoder(&self, directory: &Path) -> Result<Box<dyn SegmentationDecoder>> {
        Ok(Box::new(CoreMlDecoder {
            model: self.load_model(directory, SegmentationModelRole::Decoder)?,
        }))
    }

    fn load_head(&self, directory: &Path) -> Result<Box<dyn SegmentationHead>> {
        let mut head = CoreMlHead {
            model: self.load_model(directory, SegmentationModelRole::Head)?,
        };
        // The warm-up run (SM3.C plan, decision C10).
        let mut zero = FeatureProvider::new();
        insert_tensor(
            &mut zero,
            HEAD.inputs[0],
            &TensorF32::zeros(IMAGE_EMBED_SHAPE.to_vec()),
        )?;
        head.predict(&zero)
            .map_err(|e| anyhow!("failed to run the people and sky head at load: {e:#}"))?;
        Ok(Box::new(head))
    }

    fn load_subject_model(&self, directory: &Path) -> Result<Box<dyn SubjectModel>> {
        Ok(Box::new(CoreMlSubjectModel {
            model: self.load_model(directory, SegmentationModelRole::Subject)?,
        }))
    }

    fn load_sky_head(&self, directory: &Path) -> Result<Box<dyn SkyHeadModel>> {
        let mut head = CoreMlSkyHead {
            model: self.load_model(directory, SegmentationModelRole::SkyHead)?,
        };
        // The warm-up run, as for the people and sky head (SM3.C plan, decision C10).
        let zero_features: Arc<dyn ImageFeatures> = Arc::new(CoreMlImageFeatures::zeros()?);
        head.run(
            &zero_features,
            &TensorF32::zeros(PIXEL_VALUES_SHAPE.to_vec()),
        )
        .map_err(|e| anyhow!("failed to run the sky head at load: {e:#}"))?;
        Ok(Box::new(head))
    }
}

/// The expected shapes of the encoder outputs, in the output order of the specification.
const FEATURE_SHAPES: [&[usize]; 3] = [
    &IMAGE_EMBED_SHAPE,
    &HIGH_RES_FEATS_0_SHAPE,
    &HIGH_RES_FEATS_1_SHAPE,
];

/// The encoder outputs of one image as Float32 Core ML arrays, under their encoder output names
/// (the decoder input names). The arrays are not changed after the encoder run.
pub struct CoreMlImageFeatures {
    arrays: FeatureProvider,
}

// The Rust wrapper of the feature provider does not declare Send or Sync. The provider and its
// arrays are not changed after the encoder run; the decoder only reads them, under its mutex.
unsafe impl Send for CoreMlImageFeatures {}
unsafe impl Sync for CoreMlImageFeatures {}

impl CoreMlImageFeatures {
    /// Features with zero values, for the warm-up run of the sky head.
    fn zeros() -> Result<Self> {
        let mut arrays = FeatureProvider::new();
        for (name, shape) in ENCODER.coreml_outputs.iter().zip(FEATURE_SHAPES) {
            insert_tensor(&mut arrays, name, &TensorF32::zeros(shape.to_vec()))?;
        }
        Ok(Self { arrays })
    }

    /// Adds retained references to the stored arrays @p names to @p inputs: no copy of the
    /// features.
    fn insert_into(&self, inputs: &mut FeatureProvider, names: &[&str]) -> Result<()> {
        for name in names {
            let array = self
                .arrays
                .get_multi_array(name)
                .ok_or_else(|| anyhow!("the Core ML image features have no array {name}"))?;
            inputs
                .try_insert_multi_array(name, array)
                .map_err(|e| anyhow!("failed to add Core ML input {name}: {e}"))?;
        }
        Ok(())
    }
}

impl ImageFeatures for CoreMlImageFeatures {
    fn as_any(&self) -> &dyn Any {
        self
    }
}

/// Adds a Float32 Core ML array with the values of @p tensor to @p provider as @p name.
fn insert_tensor(provider: &mut FeatureProvider, name: &str, tensor: &TensorF32) -> Result<()> {
    let mut array = MultiArray::new_f32(tensor.shape())
        .map_err(|e| anyhow!("failed to allocate Core ML input {name}: {e}"))?;
    array
        .copy_from_f32_slice(tensor.data())
        .map_err(|e| anyhow!("failed to fill Core ML input {name}: {e}"))?;
    provider
        .try_insert_multi_array(name, array)
        .map_err(|e| anyhow!("failed to add Core ML input {name}: {e}"))
}

struct CoreMlEncoder {
    model: Model,
}

// The Rust wrapper of MLModel does not declare Send. The engine calls the encoder under its own
// mutex, so one thread at a time uses the handle.
unsafe impl Send for CoreMlEncoder {}

impl SegmentationEncoder for CoreMlEncoder {
    fn encode(&mut self, pixel_values: &TensorF32) -> Result<Arc<dyn ImageFeatures>> {
        let mut inputs = FeatureProvider::new();
        insert_tensor(&mut inputs, ENCODER.inputs[0], pixel_values)?;
        let outputs = self
            .model
            .predict(&inputs)
            .map_err(|e| anyhow!("failed to run the segmentation encoder: {e}"))?;
        let mut arrays = FeatureProvider::new();
        for (name, expected) in ENCODER.coreml_outputs.iter().zip(FEATURE_SHAPES) {
            let array = outputs.get_multi_array(name).ok_or_else(|| {
                anyhow!(
                    "missing Core ML encoder output {name:?}; available outputs: {:?}",
                    outputs.keys()
                )
            })?;
            if array.shape() != expected {
                bail!(
                    "Core ML encoder output {name} has shape {:?}, expected {expected:?}",
                    array.shape()
                );
            }
            if array.data_type() == DataType::Float32 {
                arrays
                    .try_insert_multi_array(name, array)
                    .map_err(|e| anyhow!("failed to keep Core ML encoder output {name}: {e}"))?;
            } else {
                insert_tensor(&mut arrays, name, &read_multi_array(name, &array)?)?;
            }
        }
        Ok(Arc::new(CoreMlImageFeatures { arrays }))
    }
}

struct CoreMlDecoder {
    model: Model,
}

// See `CoreMlEncoder`: the engine calls the decoder under its own mutex.
unsafe impl Send for CoreMlDecoder {}

impl SegmentationDecoder for CoreMlDecoder {
    fn decode(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        prompt: &DecoderPrompt,
    ) -> Result<DecoderOutput> {
        let features = features_of::<CoreMlImageFeatures>(features.as_ref())?;
        let mut inputs = FeatureProvider::new();
        features.insert_into(&mut inputs, ENCODER.coreml_outputs)?;
        let prompt_inputs = [
            ("point_coords", &prompt.point_coords),
            ("point_labels", &prompt.point_labels),
            ("mask_input", &prompt.mask_input),
            ("has_mask_input", &prompt.has_mask_input),
        ];
        for (name, tensor) in prompt_inputs {
            insert_tensor(&mut inputs, name, tensor)?;
        }
        let outputs = self
            .model
            .predict(&inputs)
            .map_err(|e| anyhow!("failed to run the segmentation decoder: {e}"))?;
        let read = |name: &str| -> Result<TensorF32> {
            let array = outputs.get_multi_array(name).ok_or_else(|| {
                anyhow!(
                    "missing Core ML decoder output {name:?}; available outputs: {:?}",
                    outputs.keys()
                )
            })?;
            read_multi_array(name, &array)
        };
        Ok(DecoderOutput {
            mask_logits: read(DECODER.coreml_outputs[0])?,
            iou_scores: read(DECODER.coreml_outputs[1])?,
        })
    }
}

/// Reads the output @p name of @p outputs as a tensor and checks its shape against @p expected.
/// @p model names the model in the errors.
fn read_output(
    outputs: &FeatureProvider,
    model: &str,
    name: &str,
    expected: &[usize],
) -> Result<TensorF32> {
    let array = outputs.get_multi_array(name).ok_or_else(|| {
        anyhow!(
            "missing Core ML {model} output {name:?}; available outputs: {:?}",
            outputs.keys()
        )
    })?;
    let tensor = read_multi_array(name, &array)?;
    tensor.expect_shape(&format!("Core ML {model} output {name}"), expected)?;
    Ok(tensor)
}

struct CoreMlHead {
    model: Model,
}

// See `CoreMlEncoder`: the engine calls the head under its own mutex.
unsafe impl Send for CoreMlHead {}

impl CoreMlHead {
    fn predict(&mut self, inputs: &FeatureProvider) -> Result<HeadOutput> {
        let outputs = self
            .model
            .predict(inputs)
            .map_err(|e| anyhow!("failed to run the people and sky head: {e}"))?;
        let read = |index: usize, expected: &[usize]| {
            read_output(&outputs, "head", HEAD.coreml_outputs[index], expected)
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

impl SegmentationHead for CoreMlHead {
    fn run(&mut self, features: &Arc<dyn ImageFeatures>) -> Result<HeadOutput> {
        let features = features_of::<CoreMlImageFeatures>(features.as_ref())?;
        let mut inputs = FeatureProvider::new();
        features.insert_into(&mut inputs, HEAD.inputs)?;
        self.predict(&inputs)
    }
}

struct CoreMlSkyHead {
    model: Model,
}

// See `CoreMlEncoder`: the engine calls the sky head under its own mutex.
unsafe impl Send for CoreMlSkyHead {}

impl SkyHeadModel for CoreMlSkyHead {
    fn run(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        pixel_values: &TensorF32,
    ) -> Result<TensorF32> {
        let features = features_of::<CoreMlImageFeatures>(features.as_ref())?;
        let (feature_names, pixel_names) = SKY_HEAD.inputs.split_at(ENCODER.coreml_outputs.len());
        let mut inputs = FeatureProvider::new();
        features.insert_into(&mut inputs, feature_names)?;
        // The encoder input is stored as a tensor: one copy of 12 MB per sky head run.
        insert_tensor(&mut inputs, pixel_names[0], pixel_values)?;
        let outputs = self
            .model
            .predict(&inputs)
            .map_err(|e| anyhow!("failed to run the sky head: {e}"))?;
        read_output(
            &outputs,
            "sky head",
            SKY_HEAD.coreml_outputs[0],
            &SKY_ALPHA_SHAPE,
        )
    }
}

struct CoreMlSubjectModel {
    model: Model,
}

// See `CoreMlEncoder`: the engine calls S3OD under its own mutex.
unsafe impl Send for CoreMlSubjectModel {}

impl SubjectModel for CoreMlSubjectModel {
    fn run(&mut self, pixel_values: &TensorF32) -> Result<SubjectOutput> {
        let mut inputs = FeatureProvider::new();
        insert_tensor(&mut inputs, SUBJECT.inputs[0], pixel_values)?;
        let outputs = self
            .model
            .predict(&inputs)
            .map_err(|e| anyhow!("failed to run S3OD: {e}"))?;
        Ok(SubjectOutput {
            masks: read_output(
                &outputs,
                "S3OD",
                SUBJECT.coreml_outputs[0],
                &SUBJECT_MASKS_SHAPE,
            )?,
            iou_scores: read_output(
                &outputs,
                "S3OD",
                SUBJECT.coreml_outputs[1],
                &SUBJECT_SCORES_SHAPE,
            )?,
        })
    }
}

/// Copies a Float32 or Float16 multi-array into a row-major tensor. Follows the strides that
/// Core ML reports, so a padded output array reads correctly.
fn read_multi_array(name: &str, array: &MultiArray) -> Result<TensorF32> {
    let shape = array.shape();
    let strides = array.strides();
    let values = match array.data_type() {
        DataType::Float32 => {
            let storage = array
                .as_f32_slice()
                .ok_or_else(|| anyhow!("Core ML output {name} has no Float32 storage"))?;
            gather_row_major(&shape, &strides, |offset| storage[offset])
        }
        DataType::Float16 => {
            let storage = array
                .as_f16_slice()
                .ok_or_else(|| anyhow!("Core ML output {name} has no Float16 storage"))?;
            gather_row_major(&shape, &strides, |offset| storage[offset].to_f32())
        }
        other => {
            bail!("Core ML output {name} has data type {other:?}, expected Float32 or Float16")
        }
    };
    TensorF32::new(shape, values)
}

/// Reads the elements of an array with @p shape and @p strides in row-major order through
/// @p read (storage offset to value).
fn gather_row_major(shape: &[usize], strides: &[usize], read: impl Fn(usize) -> f32) -> Vec<f32> {
    let len: usize = shape.iter().product();
    let mut values = Vec::with_capacity(len);
    let mut index = vec![0usize; shape.len()];
    for _ in 0..len {
        let offset = index
            .iter()
            .zip(strides)
            .map(|(position, stride)| position * stride)
            .sum();
        values.push(read(offset));
        for axis in (0..shape.len()).rev() {
            index[axis] += 1;
            if index[axis] < shape[axis] {
                break;
            }
            index[axis] = 0;
        }
    }
    values
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coreml_device_values_select_u15_compute_units() {
        let device = CoreMlSegmentationDevice::parse("").expect("default device");
        assert_eq!(
            device.compute_units_label(SegmentationModelRole::Encoder),
            "coreml-native:all"
        );
        assert_eq!(
            device.compute_units_label(SegmentationModelRole::Decoder),
            "coreml-native:cpuonly"
        );
        for role in [
            SegmentationModelRole::Head,
            SegmentationModelRole::Subject,
            SegmentationModelRole::SkyHead,
        ] {
            assert_eq!(device.compute_units_label(role), "coreml-native:all");
        }
        let cpu = CoreMlSegmentationDevice::parse("cpu").expect("cpu device");
        assert_eq!(
            cpu.compute_units_label(SegmentationModelRole::Encoder),
            "coreml-native:cpuonly"
        );
        assert!(CoreMlSegmentationDevice::parse("directml").is_err());
    }

    #[test]
    fn gather_row_major_follows_padded_strides() {
        // Shape [2, 3] stored with a row stride of 4 (one padding value per row).
        let storage = [0.0, 1.0, 2.0, -1.0, 3.0, 4.0, 5.0, -1.0];
        let values = gather_row_major(&[2, 3], &[4, 1], |offset| storage[offset]);
        assert_eq!(values, vec![0.0, 1.0, 2.0, 3.0, 4.0, 5.0]);
    }

    #[test]
    fn coreml_features_keep_float32_array_without_copy() {
        let mut array = MultiArray::new_f32(&[1, 2]).expect("array");
        array.copy_from_f32_slice(&[1.5, -2.0]).expect("fill");
        let mut provider = FeatureProvider::new();
        provider
            .try_insert_multi_array("image_embed", array)
            .expect("insert");
        let first = provider.get_multi_array("image_embed").expect("first");
        let second = provider.get_multi_array("image_embed").expect("second");
        // Both references read the storage of one array.
        assert_eq!(
            first.as_f32_slice().expect("f32").as_ptr(),
            second.as_f32_slice().expect("f32").as_ptr()
        );
        assert_eq!(first.as_f32_slice().expect("f32"), &[1.5, -2.0]);
    }
}
