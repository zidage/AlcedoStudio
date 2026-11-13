//! ONNX Runtime decoder of the segmentation models (SM3.B plan, decisions B3 and B4).
//!
//! On DirectML, a decoder session is fast only for the point count of its first run (master plan
//! section 8.2.3). The decoder therefore loads one session for each of the point counts 1 (click),
//! 2 (box), and 4 (stroke), and one general session for the other counts. Each session runs twice
//! at load with its point count (the general session with 3): without and with a mask input, so
//! the first user decode of either kind does not pay the first-run cost. The `cpu` device has only
//! the general session.
//!
//! Each session has an `IoBinding`. The image features are bound once and stay bound until a
//! decode with other features: on DirectML, the bind copies the 16 MB of features to the device
//! of the session, so a decode with the same features copies only the prompt tensors. A session
//! keeps a strong reference to the features that it bound, so the identity comparison
//! (`Arc::ptr_eq`) cannot match a new allocation at the address of freed features.

use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result, anyhow};
use ort::{
    memory::{AllocationDevice, AllocatorType, MemoryInfo, MemoryType},
    session::{IoBinding, Session},
    value::Tensor,
};

use super::backend::{
    DecoderOutput, DecoderPrompt, ImageFeatures, SegmentationDecoder, features_of,
};
use super::model_set::{MASK_INPUT_SHAPE, ModelSpec};
use super::ort_backend::{OrtImageFeatures, OrtSegmentationDevice, load_session};
use super::tensor::TensorF32;

/// Point counts of the DirectML per-count decoder sessions: a click, a box, and a stroke.
pub const DIRECTML_DECODER_POINT_COUNTS: [usize; 3] = [1, 2, 4];
/// Point count of the warm-up run of the general session.
const GENERAL_SESSION_WARM_UP_POINTS: usize = 3;

/// The decoder sessions of @p device: `Some(n)` is the session of point count n, `None` is the
/// general session for every other count.
pub fn decoder_session_counts(device: OrtSegmentationDevice) -> Vec<Option<usize>> {
    match device {
        OrtSegmentationDevice::DirectMl(_) => DIRECTML_DECODER_POINT_COUNTS
            .iter()
            .map(|count| Some(*count))
            .chain(std::iter::once(None))
            .collect(),
        OrtSegmentationDevice::Cpu => vec![None],
    }
}

/// Index in @p counts of the session that runs @p point_count decoder points: the session of that
/// count, else the general session.
pub fn session_index_for(counts: &[Option<usize>], point_count: usize) -> Option<usize> {
    counts
        .iter()
        .position(|count| *count == Some(point_count))
        .or_else(|| counts.iter().position(Option::is_none))
}

struct DecoderSession {
    point_count: Option<usize>,
    session: Session,
    binding: IoBinding,
    /// The features that `binding` holds, or `None` before the first bind.
    bound_features: Option<Arc<dyn ImageFeatures>>,
}

impl DecoderSession {
    fn decode(
        &mut self,
        spec: &ModelSpec,
        features: &Arc<dyn ImageFeatures>,
        prompt: &DecoderPrompt,
    ) -> Result<DecoderOutput> {
        let is_bound = self
            .bound_features
            .as_ref()
            .is_some_and(|bound| Arc::ptr_eq(bound, features));
        if !is_bound {
            let values = features_of::<OrtImageFeatures>(features.as_ref())?;
            for (name, value) in values.named_values() {
                self.binding
                    .bind_input(name, value)
                    .map_err(|e| anyhow!("failed to bind decoder input {name}: {e}"))?;
            }
            self.bound_features = Some(features.clone());
        }
        let prompt_inputs = [
            ("point_coords", &prompt.point_coords),
            ("point_labels", &prompt.point_labels),
            ("mask_input", &prompt.mask_input),
            ("has_mask_input", &prompt.has_mask_input),
        ];
        for (name, tensor) in prompt_inputs {
            let value = Tensor::from_array((tensor.shape().to_vec(), tensor.data().to_vec()))
                .map_err(|e| anyhow!("failed to create decoder input {name}: {e}"))?;
            self.binding
                .bind_input(name, &value)
                .map_err(|e| anyhow!("failed to bind decoder input {name}: {e}"))?;
        }
        let outputs = self
            .session
            .run_binding(&self.binding)
            .map_err(|e| anyhow!("failed to run the segmentation decoder: {e}"))?;
        let read = |name: &str| -> Result<TensorF32> {
            let value = outputs
                .get(name)
                .with_context(|| format!("missing decoder output {name}"))?;
            let (shape, data) = value
                .try_extract_tensor::<f32>()
                .map_err(|e| anyhow!("decoder output {name} is not a float32 tensor: {e}"))?;
            let shape = shape
                .iter()
                .map(|&dim| {
                    usize::try_from(dim)
                        .map_err(|_| anyhow!("decoder output {name} has a negative dimension"))
                })
                .collect::<Result<Vec<_>>>()?;
            TensorF32::new(shape, data.to_vec())
        };
        Ok(DecoderOutput {
            mask_logits: read(spec.onnx_outputs[0])?,
            iou_scores: read(spec.onnx_outputs[1])?,
        })
    }
}

/// A zero prompt of @p point_count points for a warm-up run, with a zero mask input when
/// @p has_mask_input is true.
fn warm_up_prompt(point_count: usize, has_mask_input: bool) -> Result<DecoderPrompt> {
    Ok(DecoderPrompt {
        point_coords: TensorF32::zeros(vec![1, point_count, 2]),
        point_labels: TensorF32::new(vec![1, point_count], vec![1.0; point_count])?,
        mask_input: TensorF32::zeros(MASK_INPUT_SHAPE.to_vec()),
        has_mask_input: TensorF32::new(vec![1], vec![if has_mask_input { 1.0 } else { 0.0 }])?,
    })
}

/// The decoder sessions of one device. See the module documentation.
pub struct OrtDecoder {
    spec: &'static ModelSpec,
    counts: Vec<Option<usize>>,
    sessions: Vec<DecoderSession>,
}

impl OrtDecoder {
    /// Loads the sessions of @p device from the decoder file of @p spec in @p directory and runs
    /// each once. Any failure is the load error; no session of another device is created.
    pub fn load(
        directory: &Path,
        spec: &'static ModelSpec,
        device: OrtSegmentationDevice,
    ) -> Result<Self> {
        let counts = decoder_session_counts(device);
        let output_memory = MemoryInfo::new(
            AllocationDevice::CPU,
            0,
            AllocatorType::Device,
            MemoryType::Default,
        )
        .map_err(|e| anyhow!("failed to describe CPU memory for decoder outputs: {e}"))?;
        let zero_features: Arc<dyn ImageFeatures> = Arc::new(OrtImageFeatures::zeros()?);
        let mut sessions = Vec::with_capacity(counts.len());
        for point_count in &counts {
            let session = load_session(directory, spec, device)?;
            let mut binding = session
                .create_binding()
                .map_err(|e| anyhow!("failed to create a decoder input binding: {e}"))?;
            for name in spec.onnx_outputs {
                binding
                    .bind_output_to_device(*name, &output_memory)
                    .map_err(|e| anyhow!("failed to bind decoder output {name}: {e}"))?;
            }
            let mut decoder_session = DecoderSession {
                point_count: *point_count,
                session,
                binding,
                bound_features: None,
            };
            let warm_up_points = point_count.unwrap_or(GENERAL_SESSION_WARM_UP_POINTS);
            for has_mask_input in [false, true] {
                decoder_session
                    .decode(
                        spec,
                        &zero_features,
                        &warm_up_prompt(warm_up_points, has_mask_input)?,
                    )
                    .with_context(|| {
                        format!("failed to run the decoder session for {warm_up_points} points")
                    })?;
            }
            sessions.push(decoder_session);
        }
        Ok(Self {
            spec,
            counts,
            sessions,
        })
    }
}

impl SegmentationDecoder for OrtDecoder {
    fn decode(
        &mut self,
        features: &Arc<dyn ImageFeatures>,
        prompt: &DecoderPrompt,
    ) -> Result<DecoderOutput> {
        let point_count = prompt.point_count();
        let index = session_index_for(&self.counts, point_count)
            .with_context(|| format!("no decoder session runs {point_count} points"))?;
        let session = &mut self.sessions[index];
        debug_assert!(session.point_count.is_none_or(|count| count == point_count));
        session.decode(self.spec, features, prompt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decoder_session_counts_follow_device() {
        assert_eq!(
            decoder_session_counts(OrtSegmentationDevice::DirectMl(0)),
            vec![Some(1), Some(2), Some(4), None]
        );
        assert_eq!(
            decoder_session_counts(OrtSegmentationDevice::Cpu),
            vec![None]
        );
    }

    #[test]
    fn decoder_session_for_point_count_routes_counts() {
        let directml = decoder_session_counts(OrtSegmentationDevice::DirectMl(0));
        assert_eq!(session_index_for(&directml, 1), Some(0));
        assert_eq!(session_index_for(&directml, 2), Some(1));
        assert_eq!(session_index_for(&directml, 4), Some(2));
        for general in [3, 5, 64] {
            assert_eq!(session_index_for(&directml, general), Some(3));
        }
        let cpu = decoder_session_counts(OrtSegmentationDevice::Cpu);
        for count in [1, 2, 4, 64] {
            assert_eq!(session_index_for(&cpu, count), Some(0));
        }
    }
}
