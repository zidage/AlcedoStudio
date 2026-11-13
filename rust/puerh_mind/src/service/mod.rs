#[cfg(target_os = "macos")]
pub mod coreml_clip;
pub mod embedding;
pub mod encoder_residency;
pub mod inference;
pub mod lazy_semantic_engine;
pub mod model_adapters;
pub mod registry;
pub mod segmentation;

pub mod model_assets;
pub mod ort_clip;
pub mod ort_runtime;
