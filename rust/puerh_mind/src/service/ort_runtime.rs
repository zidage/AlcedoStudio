use std::path::Path;
use std::sync::OnceLock;

use anyhow::{Result, bail};
use ort::{
    ep,
    session::{Session, builder::GraphOptimizationLevel},
};

const DEVICE_ERROR_MESSAGE: &str = "expected \"auto\", \"cpu\", \"directml\", \"dml\", \"directml:N\", \"dml:N\", \"coreml\", \"coreml:all\", \"coreml:cpuandgpu\", or \"coreml:cpuonly\" for ORT backend device";

static ORT_ENVIRONMENT_INIT: OnceLock<bool> = OnceLock::new();

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum DeviceRequest {
    Auto,
    Cpu,
    DirectMl(Option<i32>),
    CoreMl(CoreMlMode),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum CoreMlMode {
    All,
    CpuAndGpu,
    CpuOnly,
}

pub(crate) fn parse_device_request(value: &str) -> Result<DeviceRequest> {
    let value = value.trim();
    if value.is_empty() {
        bail!("unsupported ALCEDO_MIND_DEVICE value {value:?}, {DEVICE_ERROR_MESSAGE}");
    }

    let value_lower = value.to_ascii_lowercase();

    if value_lower == "auto" {
        return Ok(DeviceRequest::Auto);
    }
    if value_lower == "cpu" {
        return Ok(DeviceRequest::Cpu);
    }

    if value_lower == "directml" || value_lower == "dml" {
        return Ok(DeviceRequest::DirectMl(None));
    }
    if value_lower == "coreml" || value_lower == "coreml:all" {
        return Ok(DeviceRequest::CoreMl(CoreMlMode::All));
    }
    if value_lower == "coreml:cpuandgpu" {
        return Ok(DeviceRequest::CoreMl(CoreMlMode::CpuAndGpu));
    }
    if value_lower == "coreml:cpuonly" {
        return Ok(DeviceRequest::CoreMl(CoreMlMode::CpuOnly));
    }

    if let Some(ordinal_text) = value_lower
        .strip_prefix("directml:")
        .or_else(|| value_lower.strip_prefix("dml:"))
    {
        if ordinal_text.is_empty() {
            bail!("missing directml device ordinal in {value:?}");
        }

        let ordinal = ordinal_text.parse::<i32>().map_err(|_| {
            anyhow::anyhow!("invalid directml device ordinal {ordinal_text:?} in {value:?}")
        })?;

        if ordinal < 0 {
            bail!("directml device ordinal must be >= 0 in {value:?}");
        }

        return Ok(DeviceRequest::DirectMl(Some(ordinal)));
    }

    if value_lower.starts_with("coreml:") {
        bail!("unsupported Core ML mode in {value:?}, {DEVICE_ERROR_MESSAGE}");
    }

    if value_lower == "cuda"
        || value_lower.starts_with("cuda:")
        || value_lower == "metal"
        || value_lower.starts_with("metal:")
    {
        bail!(
            "ALCEDO_MIND_DEVICE={value:?} is not supported by the ORT backend; use \"directml\"/\"dml\" on Windows or \"cpu\""
        );
    }

    bail!("unsupported ALCEDO_MIND_DEVICE value {value:?}, {DEVICE_ERROR_MESSAGE}")
}

pub(crate) fn initialize_ort_environment() -> Result<()> {
    let _ = ORT_ENVIRONMENT_INIT.get_or_init(|| {
        ort::init()
            .with_execution_providers([ep::CPU::default().build()])
            .commit()
    });

    Ok(())
}

pub(crate) fn describe_device_request(device_request: DeviceRequest) -> String {
    match device_request {
        DeviceRequest::Auto => {
            if cfg!(target_os = "windows") {
                "auto (DirectML preferred, CPU fallback)".to_string()
            } else {
                "auto (CoreML preferred on macOS, CPU fallback elsewhere)".to_string()
            }
        }
        DeviceRequest::Cpu => "cpu".to_string(),
        DeviceRequest::DirectMl(None) => "directml".to_string(),
        DeviceRequest::DirectMl(Some(ordinal)) => format!("directml:{ordinal}"),
        DeviceRequest::CoreMl(CoreMlMode::All) => "coreml:all".to_string(),
        DeviceRequest::CoreMl(CoreMlMode::CpuAndGpu) => "coreml:cpuandgpu".to_string(),
        DeviceRequest::CoreMl(CoreMlMode::CpuOnly) => "coreml:cpuonly".to_string(),
    }
}

fn execution_providers_for_device_request(
    device_request: DeviceRequest,
) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    match device_request {
        DeviceRequest::Auto => Ok(platform_auto_execution_providers()),
        DeviceRequest::Cpu => Ok(vec![ep::CPU::default().build()]),
        DeviceRequest::DirectMl(device_id) => directml_execution_providers(device_id),
        DeviceRequest::CoreMl(mode) => coreml_execution_providers(mode),
    }
}

#[cfg(windows)]
fn platform_auto_execution_providers() -> Vec<ep::ExecutionProviderDispatch> {
    vec![ep::DirectML::default().build(), ep::CPU::default().build()]
}

#[cfg(target_os = "macos")]
fn platform_auto_execution_providers() -> Vec<ep::ExecutionProviderDispatch> {
    vec![
        ep::CoreML::default()
            .with_compute_units(ep::coreml::ComputeUnits::All)
            .build(),
        ep::CPU::default().build(),
    ]
}

#[cfg(not(any(windows, target_os = "macos")))]
fn platform_auto_execution_providers() -> Vec<ep::ExecutionProviderDispatch> {
    vec![ep::CPU::default().build()]
}

#[cfg(windows)]
fn directml_execution_providers(
    device_id: Option<i32>,
) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    let directml = match device_id {
        Some(ordinal) => ep::DirectML::default().with_device_id(ordinal).build(),
        None => ep::DirectML::default().build(),
    };
    Ok(vec![directml, ep::CPU::default().build()])
}

#[cfg(not(windows))]
fn directml_execution_providers(
    _device_id: Option<i32>,
) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    bail!(
        "ALCEDO_MIND_DEVICE requests DirectML, but DirectML is only supported on Windows for the ORT backend"
    )
}

#[cfg(target_os = "macos")]
fn coreml_execution_providers(mode: CoreMlMode) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    let units = match mode {
        CoreMlMode::All => ep::coreml::ComputeUnits::All,
        CoreMlMode::CpuAndGpu => ep::coreml::ComputeUnits::CPUAndGPU,
        CoreMlMode::CpuOnly => ep::coreml::ComputeUnits::CPUOnly,
    };
    Ok(vec![
        ep::CoreML::default().with_compute_units(units).build(),
        ep::CPU::default().build(),
    ])
}

#[cfg(not(target_os = "macos"))]
fn coreml_execution_providers(_mode: CoreMlMode) -> Result<Vec<ep::ExecutionProviderDispatch>> {
    bail!("device requests Core ML, but Core ML is only supported on macOS for the ORT backend")
}

pub(crate) fn load_session(path: &Path, device_request: DeviceRequest) -> Result<Session> {
    let builder = Session::builder()
        .map_err(|e| anyhow::anyhow!("failed to create ORT session builder: {e}"))?;
    let builder = builder
        .with_optimization_level(GraphOptimizationLevel::Level3)
        .map_err(|e| anyhow::anyhow!("failed to set ORT optimization level: {e}"))?;
    let execution_providers = execution_providers_for_device_request(device_request)?;
    let mut builder = builder
        .with_execution_providers(execution_providers)
        .map_err(|e| anyhow::anyhow!("failed to configure ORT execution providers: {e}"))?;

    builder
        .commit_from_file(path)
        .map_err(|e| anyhow::anyhow!("failed to load ONNX model {}: {e}", path.display()))
}
