use std::env;
use std::net::IpAddr;

#[derive(Debug)]
pub struct AppConfig {
    pub host: String,
    pub port: u16,
    pub allow_remote_bind: bool,
    pub semantic: SemanticConfig,
    pub max_message_bytes: usize,
    pub credential_ttl_ms: u64,
    /// Optional directory of user-provider JSON configs, loaded after the built-in
    /// configs and allowed to add providers or override model defaults. `None`
    /// means built-ins only. See `service::provider_config`.
    pub provider_config_dir: Option<String>,
    /// Devices and idle unload times of the CLIP encoders (`service::encoder_residency`).
    pub residency: EncoderResidencyConfig,
}

/// Where each CLIP encoder loads and how long it stays loaded after its last request.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EncoderResidencyConfig {
    /// Device of the text encoder. The text encoder runs on the CPU unless this is changed.
    pub text_device: String,
    /// Device of the image encoder (and of a single multimodal session); `None` uses
    /// `SemanticConfig::device`.
    pub vision_device: Option<String>,
    /// Idle time of the text encoder (and of a multimodal session) in seconds: -1 never
    /// unloads, 0 unloads after each request.
    pub text_idle_unload_seconds: i64,
    /// Idle time of the image encoder in seconds, with the same rule.
    pub vision_idle_unload_seconds: i64,
}

impl Default for EncoderResidencyConfig {
    fn default() -> Self {
        Self {
            text_device: "cpu".to_string(),
            vision_device: None,
            text_idle_unload_seconds: 300,
            vision_idle_unload_seconds: 60,
        }
    }
}

impl EncoderResidencyConfig {
    /// The image encoder device: `vision_device`, or @p semantic_device when it is not set.
    pub fn vision_device_or<'a>(&'a self, semantic_device: &'a str) -> &'a str {
        self.vision_device.as_deref().unwrap_or(semantic_device)
    }
}

#[derive(Debug, Clone)]
pub struct SemanticConfig {
    /// The CLIP model profile (profile ID or model ID). Empty when the host passes no
    /// `--model-id`: the open project has no active model, and semantic requests then return
    /// `AI_ERROR_MODEL_NOT_READY` while segmentation and image analysis run.
    pub model_id: String,
    pub revision: String,
    pub model_root: String,
    pub hf_endpoint: String,
    pub device: String,
    pub allow_download: bool,
    pub batch_cap: usize,
    pub batch_wait_ms: u64,
}

impl SemanticConfig {
    /// True when a CLIP model profile is configured (`--model-id`).
    pub fn has_model(&self) -> bool {
        !self.model_id.trim().is_empty()
    }
}

impl AppConfig {
    pub fn load() -> anyhow::Result<Self> {
        Self::from_args(env::args().skip(1))
    }

    pub fn from_args<I, S>(args: I) -> anyhow::Result<Self>
    where
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        let mut config = Self::from_env()?;
        let mut args = args.into_iter().map(Into::into);

        while let Some(arg) = args.next() {
            match arg.as_str() {
                "--host" => config.host = next_value(&mut args, "--host")?,
                "--allow-remote-bind" => config.allow_remote_bind = true,
                "--port" => config.port = parse_next(&mut args, "--port")?,
                "--model-root" => {
                    config.semantic.model_root = next_value(&mut args, "--model-root")?
                }
                "--model-id" => config.semantic.model_id = next_value(&mut args, "--model-id")?,
                "--revision" => config.semantic.revision = next_value(&mut args, "--revision")?,
                "--hf-endpoint" => {
                    config.semantic.hf_endpoint = next_value(&mut args, "--hf-endpoint")?
                }
                "--device" => config.semantic.device = next_value(&mut args, "--device")?,
                "--no-download" => config.semantic.allow_download = false,
                "--allow-download" => config.semantic.allow_download = true,
                "--batch-cap" => config.semantic.batch_cap = parse_next(&mut args, "--batch-cap")?,
                "--batch-wait-ms" => {
                    config.semantic.batch_wait_ms = parse_next(&mut args, "--batch-wait-ms")?
                }
                "--max-message-bytes" => {
                    config.max_message_bytes = parse_next(&mut args, "--max-message-bytes")?
                }
                "--credential-ttl-ms" => {
                    config.credential_ttl_ms = parse_next(&mut args, "--credential-ttl-ms")?
                }
                "--provider-config-dir" => {
                    config.provider_config_dir =
                        Some(next_value(&mut args, "--provider-config-dir")?)
                }
                "--text-device" => {
                    config.residency.text_device = next_value(&mut args, "--text-device")?
                }
                "--vision-device" => {
                    config.residency.vision_device = Some(next_value(&mut args, "--vision-device")?)
                }
                "--text-idle-unload-seconds" => {
                    config.residency.text_idle_unload_seconds =
                        parse_next(&mut args, "--text-idle-unload-seconds")?
                }
                "--vision-idle-unload-seconds" => {
                    config.residency.vision_idle_unload_seconds =
                        parse_next(&mut args, "--vision-idle-unload-seconds")?
                }
                "--help" | "-h" => anyhow::bail!("{}", Self::usage()),
                other => anyhow::bail!("unknown argument {other:?}\n{}", Self::usage()),
            }
        }

        config.validate()?;
        Ok(config)
    }

    fn from_env() -> anyhow::Result<Self> {
        Ok(Self {
            host: env_value("ALCEDO_MIND_HOST", "127.0.0.1"),
            port: parse_env("ALCEDO_MIND_PORT", 50051)?,
            allow_remote_bind: parse_bool_env("ALCEDO_MIND_ALLOW_REMOTE_BIND", false)?,
            max_message_bytes: parse_env("ALCEDO_MIND_MAX_MESSAGE_BYTES", 16 * 1024 * 1024)?,
            semantic: SemanticConfig {
                model_id: env_value("ALCEDO_MIND_MODEL_ID", ""),
                revision: env_value(
                    "ALCEDO_MIND_REVISION",
                    crate::service::model_assets::MOBILECLIP2_ONNX_REVISION,
                ),
                model_root: env_value("ALCEDO_MIND_MODEL_ROOT", "./models/mobileclip2-s2-openclip"),
                hf_endpoint: env_value("ALCEDO_MIND_HF_ENDPOINT", "https://hf-mirror.com"),
                device: env_value("ALCEDO_MIND_DEVICE", "auto"),
                allow_download: parse_bool_env("ALCEDO_MIND_ALLOW_DOWNLOAD", false)?,
                batch_cap: parse_env("ALCEDO_MIND_BATCH_CAP", 512)?,
                batch_wait_ms: parse_env("ALCEDO_MIND_BATCH_WAIT_MS", 25)?,
            },
            credential_ttl_ms: parse_env("ALCEDO_MIND_CREDENTIAL_TTL_MS", 3_600_000)?,
            provider_config_dir: env::var("ALCEDO_MIND_PROVIDER_CONFIG_DIR")
                .ok()
                .filter(|s| !s.trim().is_empty()),
            residency: EncoderResidencyConfig::default(),
        })
    }

    pub fn listen_addr(&self) -> String {
        format!("{}:{}", self.host, self.port)
    }

    fn validate(&self) -> anyhow::Result<()> {
        if self.host.trim().is_empty() {
            anyhow::bail!("host must not be empty");
        }
        if !self.allow_remote_bind && !is_loopback_host(&self.host) {
            anyhow::bail!(
                "host {:?} is not loopback; pass --allow-remote-bind or set ALCEDO_MIND_ALLOW_REMOTE_BIND=1 to expose the unauthenticated sidecar",
                self.host
            );
        }
        if self.semantic.model_root.trim().is_empty() {
            anyhow::bail!("model-root must not be empty");
        }
        if self.semantic.revision.trim().is_empty() {
            anyhow::bail!("revision must not be empty");
        }
        if self.semantic.hf_endpoint.trim().is_empty() {
            anyhow::bail!("hf-endpoint must not be empty");
        }
        if self.semantic.batch_cap == 0 {
            anyhow::bail!("batch-cap must be greater than zero");
        }
        if self.max_message_bytes == 0 {
            anyhow::bail!("max-message-bytes must be greater than zero");
        }
        if self.residency.text_device.trim().is_empty() {
            anyhow::bail!("text-device must not be empty");
        }
        if self
            .residency
            .vision_device
            .as_deref()
            .is_some_and(|device| device.trim().is_empty())
        {
            anyhow::bail!("vision-device must not be empty");
        }
        for (flag, seconds) in [
            (
                "text-idle-unload-seconds",
                self.residency.text_idle_unload_seconds,
            ),
            (
                "vision-idle-unload-seconds",
                self.residency.vision_idle_unload_seconds,
            ),
        ] {
            crate::service::encoder_residency::IdleUnload::from_seconds(seconds)
                .map_err(|err| anyhow::anyhow!("{flag}: {err}"))?;
        }
        Ok(())
    }

    fn usage() -> &'static str {
        "usage: alcedo_mind [--host HOST] [--allow-remote-bind] [--port PORT] [--model-root PATH] [--model-id ID] [--revision REV] [--hf-endpoint URL] [--device auto|cpu|directml[:N]|coreml[:MODE]] [--no-download] [--allow-download] [--batch-cap N] [--batch-wait-ms N] [--max-message-bytes N] [--credential-ttl-ms N] [--provider-config-dir PATH] [--text-device DEVICE] [--vision-device DEVICE] [--text-idle-unload-seconds N] [--vision-idle-unload-seconds N]"
    }
}

fn is_loopback_host(host: &str) -> bool {
    let host = host.trim();
    if host.eq_ignore_ascii_case("localhost") {
        return true;
    }
    host.parse::<IpAddr>()
        .map(|addr| addr.is_loopback())
        .unwrap_or(false)
}

fn env_value(name: &str, default: &str) -> String {
    env::var(name).unwrap_or_else(|_| default.to_string())
}

fn parse_env<T>(name: &str, default: T) -> anyhow::Result<T>
where
    T: std::str::FromStr,
    T::Err: std::fmt::Display,
{
    match env::var(name) {
        Ok(value) => value
            .parse()
            .map_err(|err| anyhow::anyhow!("failed to parse {name}={value:?}: {err}")),
        Err(env::VarError::NotPresent) => Ok(default),
        Err(err) => Err(anyhow::anyhow!("failed to read {name}: {err}")),
    }
}

fn parse_bool_env(name: &str, default: bool) -> anyhow::Result<bool> {
    match env::var(name) {
        Ok(value) => match value.to_ascii_lowercase().as_str() {
            "1" | "true" | "yes" | "on" => Ok(true),
            "0" | "false" | "no" | "off" => Ok(false),
            _ => anyhow::bail!("failed to parse {name}={value:?} as bool"),
        },
        Err(env::VarError::NotPresent) => Ok(default),
        Err(err) => Err(anyhow::anyhow!("failed to read {name}: {err}")),
    }
}

fn next_value(args: &mut impl Iterator<Item = String>, flag: &str) -> anyhow::Result<String> {
    args.next()
        .ok_or_else(|| anyhow::anyhow!("missing value after {flag}"))
}

fn parse_next<T>(args: &mut impl Iterator<Item = String>, flag: &str) -> anyhow::Result<T>
where
    T: std::str::FromStr,
    T::Err: std::fmt::Display,
{
    let value = next_value(args, flag)?;
    value
        .parse()
        .map_err(|err| anyhow::anyhow!("failed to parse {flag} value {value:?}: {err}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_cli_overrides_and_keeps_validate_only_default() {
        let config = AppConfig::from_args([
            "--host",
            "0.0.0.0",
            "--allow-remote-bind",
            "--port",
            "5555",
            "--model-root",
            "C:/models/mobileclip",
            "--model-id",
            "repo/model:s2",
            "--revision",
            "abc123",
            "--hf-endpoint",
            "https://example.invalid",
            "--device",
            "cpu",
            "--no-download",
            "--batch-cap",
            "8",
            "--batch-wait-ms",
            "3",
            "--max-message-bytes",
            "4096",
        ])
        .expect("config should parse");

        assert_eq!(config.host, "0.0.0.0");
        assert!(config.allow_remote_bind);
        assert_eq!(config.port, 5555);
        assert_eq!(config.semantic.model_root, "C:/models/mobileclip");
        assert_eq!(config.semantic.model_id, "repo/model:s2");
        assert_eq!(config.semantic.revision, "abc123");
        assert_eq!(config.semantic.hf_endpoint, "https://example.invalid");
        assert_eq!(config.semantic.device, "cpu");
        assert!(!config.semantic.allow_download);
        assert_eq!(config.semantic.batch_cap, 8);
        assert_eq!(config.semantic.batch_wait_ms, 3);
        assert_eq!(config.max_message_bytes, 4096);
    }

    #[test]
    fn starts_without_model_id_and_reports_no_semantic_model() {
        let config = AppConfig::from_args(["--model-root", "C:/models"])
            .expect("config without --model-id should parse");
        assert!(config.semantic.model_id.is_empty());
        assert!(!config.semantic.has_model());

        let config = AppConfig::from_args(["--model-id", "repo/model:s2"]).expect("config");
        assert!(config.semantic.has_model());
    }

    #[test]
    fn parses_explicit_download_opt_in() {
        let config = AppConfig::from_args(["--allow-download"]).expect("config should parse");
        assert!(config.semantic.allow_download);
        assert_eq!(config.semantic.hf_endpoint, "https://hf-mirror.com");
    }

    #[test]
    fn rejects_empty_host() {
        let err = AppConfig::from_args(["--host", "  "]).expect_err("empty host rejected");
        assert!(err.to_string().contains("host must not be empty"));
    }

    #[test]
    fn rejects_remote_host_without_explicit_opt_in() {
        let err = AppConfig::from_args(["--host", "0.0.0.0"]).expect_err("remote host rejected");
        assert!(err.to_string().contains("not loopback"), "{err}");
    }

    #[test]
    fn accepts_remote_host_with_explicit_opt_in() {
        let config = AppConfig::from_args(["--host", "0.0.0.0", "--allow-remote-bind"])
            .expect("explicit remote bind accepted");
        assert_eq!(config.host, "0.0.0.0");
        assert!(config.allow_remote_bind);
    }

    #[test]
    fn rejects_empty_model_root() {
        let err =
            AppConfig::from_args(["--model-root", ""]).expect_err("empty model-root rejected");
        assert!(err.to_string().contains("model-root must not be empty"));
    }

    #[test]
    fn rejects_unknown_argument() {
        let err = AppConfig::from_args(["--nonsense"]).expect_err("unknown arg rejected");
        assert!(err.to_string().contains("unknown argument"));
    }

    #[test]
    fn rejects_missing_value_after_flag() {
        let err = AppConfig::from_args(["--port"]).expect_err("missing value rejected");
        assert!(err.to_string().contains("missing value after --port"));
    }

    #[test]
    fn rejects_non_numeric_port() {
        let err =
            AppConfig::from_args(["--port", "not-a-port"]).expect_err("non-numeric port rejected");
        assert!(err.to_string().contains("failed to parse --port"));
    }

    #[test]
    fn rejects_zero_batch_cap() {
        let err = AppConfig::from_args(["--batch-cap", "0"]).expect_err("zero batch-cap rejected");
        assert!(
            err.to_string()
                .contains("batch-cap must be greater than zero")
        );
    }

    #[test]
    fn parses_provider_config_dir_override() {
        let config =
            AppConfig::from_args(["--provider-config-dir", "C:/Users/me/alcedo/providers"])
                .expect("config should parse");
        assert_eq!(
            config.provider_config_dir.as_deref(),
            Some("C:/Users/me/alcedo/providers")
        );
    }

    #[test]
    fn parses_text_and_vision_device_flags() {
        let defaults = AppConfig::from_args(["--device", "directml:1"]).expect("defaults parse");
        assert_eq!(defaults.residency.text_device, "cpu");
        assert_eq!(
            defaults
                .residency
                .vision_device_or(&defaults.semantic.device),
            "directml:1"
        );
        assert_eq!(defaults.residency.text_idle_unload_seconds, 300);
        assert_eq!(defaults.residency.vision_idle_unload_seconds, 60);

        let config = AppConfig::from_args([
            "--device",
            "directml:1",
            "--text-device",
            "directml:0",
            "--vision-device",
            "cpu",
            "--text-idle-unload-seconds",
            "-1",
            "--vision-idle-unload-seconds",
            "0",
        ])
        .expect("residency flags parse");
        assert_eq!(config.residency.text_device, "directml:0");
        assert_eq!(
            config.residency.vision_device_or(&config.semantic.device),
            "cpu"
        );
        assert_eq!(config.residency.text_idle_unload_seconds, -1);
        assert_eq!(config.residency.vision_idle_unload_seconds, 0);

        let err = AppConfig::from_args(["--text-idle-unload-seconds", "-5"])
            .expect_err("negative idle time other than -1 rejected");
        assert!(
            err.to_string().contains("text-idle-unload-seconds"),
            "{err}"
        );

        // The engine parses the device values; CUDA and Metal stay rejected for both encoders.
        for device in ["cuda", "metal"] {
            assert!(crate::service::ort_runtime::parse_device_request(device).is_err());
            let config = AppConfig::from_args(["--text-device", device, "--vision-device", device])
                .expect("device text is checked by the engine");
            assert!(
                crate::service::ort_runtime::parse_device_request(&config.residency.text_device)
                    .is_err()
            );
        }
    }

    #[test]
    fn provider_config_dir_defaults_to_none() {
        let config = AppConfig::from_env().expect("env config should parse");
        assert!(config.provider_config_dir.is_none());
    }
}
