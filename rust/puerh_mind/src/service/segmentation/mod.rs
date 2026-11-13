//! Local AI Mask segmentation (master plan section 9.2, SM3.A, SM3.B, and SM3.C).
//!
//! The engine ([`engine`]) owns the loaded models (SAM 2.1 tiny, the people and sky head, and
//! S3OD), one image entry per session ([`image_entry`]), and the image embedding cache
//! ([`image_embedding_cache`]). It runs the models through the
//! platform interface of [`backend`]:
//!
//! - Windows: [`ort_backend`] and [`ort_decoder`], ONNX Runtime with DirectML (or the CPU for
//!   development);
//! - macOS: `coreml_backend`, compiled native Core ML models.
//!
//! [`sam2`] holds the host rules of the SAM 2.1 models (preprocessing, prompts, mask selection) and
//! [`task_masks`] the rules of the task masks Subject, Background, Sky, and People, as pure
//! functions. [`model_set`] names the files and tensors of the model set.

pub mod backend;
#[cfg(target_os = "macos")]
pub mod coreml_backend;
pub mod engine;
pub mod image_embedding_cache;
pub mod image_entry;
pub mod model_set;
pub mod ort_backend;
pub mod ort_decoder;
pub mod sam2;
pub mod task_masks;
pub mod tensor;
#[cfg(test)]
pub(crate) mod test_support;

use std::sync::Arc;

use backend::SegmentationBackendFactory;

/// The backend factory of the platform: native Core ML on macOS, ONNX Runtime elsewhere.
pub fn platform_backend_factory() -> Arc<dyn SegmentationBackendFactory> {
    #[cfg(target_os = "macos")]
    {
        Arc::new(coreml_backend::CoreMlSegmentationBackendFactory)
    }
    #[cfg(not(target_os = "macos"))]
    {
        Arc::new(ort_backend::OrtSegmentationBackendFactory)
    }
}
