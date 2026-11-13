//! Provider configuration loader and registry (Phase 5a; Phase 6a preset model).
//!
//! Makes remote-provider selection data-driven without turning provider JSON
//! files into executable code. Built-in provider configs are embedded into the
//! sidecar binary with `include_str!` so Windows packaging cannot omit them;
//! optional user-provider configs are loaded from a configured directory and
//! may add providers or override model defaults, but they may not override
//! credential policy, disable schema validation, or enable arbitrary code
//! execution (there is no eval/script/shell surface — a config only describes
//! endpoints, auth slots, structured-output mode, response extraction, and
//! limits; the actual provider call is code-owned in Phase 5c drivers).
//!
//! The Alcedo task schema stays code-owned: provider configs describe *how* a
//! provider is called and *where* its response is extracted; they do not define
//! the business fields of `image_understanding.describe` / `image_rating.score`
//! (those live in `image_analysis.proto` and the JSON Schemas in
//! `crate::service::image_analysis`).
//!
//! Phase 6a preset model: the product mental model is "compatible protocol
//! preset", not "provider brand". A preset is a configured endpoint over one of
//! the protocol families (`openai_chat_compatible`, `anthropic_messages`, ...);
//! `provider_id` is kept on the wire for compatibility but means "configured
//! endpoint id". The two Opencode Go presets (`opencode_go_anthropic`,
//! `opencode_go_openai`) are the first product-facing compatible presets.
//! OpenRouter remains shipped only as an optional / legacy Phase 5 smoke preset
//! — it is not the primary recommendation. A preset is advertised as
//! image-analysis capable only when a model has
//! `supports_vision && supports_structured_output` OR a live smoke has
//! explicitly pinned that capability via `live_confirmed`. The OpenCode Go presets
//! ship with documented model ids and structured-output-capable protocol configs;
//! their public model-list endpoint currently exposes ids but not granular vision
//! metadata, so those defaults are treated as image-analysis candidates.
//!
//! See docs/roadmap/ai_sidecar_backend_plan.md (Phase 5a / Phase 6a) and
//! docs/roadmap/alcedo_studio/ai/ai_sidecar_phase0_requirements.md (section 1) for the frozen
//! control-surface contract these configs feed into.

use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::path::Path;

use reqwest::Url;
use serde_json::Value;

use crate::proto::alcedo::ai::{AiCapability, AiInputKind, AiOutputKind};

/// The only schema_version accepted by this loader. Bumping it requires a
/// coordinated Rust + C++ change, so an unknown version fails closed.
pub const KNOWN_SCHEMA_VERSION: u32 = 1;

/// Driver families the loader will accept. The Phase 5c drivers implement the
/// ones shipped in built-in configs (`openrouter_chat`, `volcengine_ark_responses`);
/// the rest are reserved driver ids that configs may name but no driver is wired
/// until that family lands. See the plan's "Initial driver families" list.
pub const KNOWN_DRIVER_IDS: &[&str] = &[
    "openrouter_chat",
    "volcengine_ark_responses",
    "volcengine_ark_chat",
    "openai_responses",
    "openai_codex_oauth",
    "openai_chat_compatible",
    "anthropic_messages",
    "gemini_generate_content",
    "generic_json_http",
];

/// Structured-output injection modes a config may select. The driver, not the
/// config, owns the exact wire field mapping; the config only declares which
/// mechanism to use (or `none` for models that cannot enforce a schema — those
/// cannot produce image-analysis capabilities).
const KNOWN_SO_MODES: &[&str] = &[
    "response_format_json_schema",
    "responses_json_schema",
    "tool",
    "none",
];

/// Auth mechanisms a config may declare. The secret itself never lives in the
/// config — `credential_slot` names the slot the Rust credential vault resolves
/// at call time.
const KNOWN_AUTH_TYPES: &[&str] = &["bearer", "api_key_header", "none"];

/// Bounded limits. The plan ships 60s / 4 MiB / 1200 tokens; these bounds keep a
/// user config from forcing the sidecar into an unreasonable wait or payload.
const MIN_TIMEOUT_MS: u64 = 1_000;
const MAX_TIMEOUT_MS: u64 = 300_000;
const MAX_IMAGE_BYTES: u64 = 16 * 1024 * 1024;
const MIN_OUTPUT_TOKENS: u64 = 1;
const MAX_OUTPUT_TOKENS: u64 = 8_192;

/// Rendition kinds a model may recommend. Matches `AiInputKind` thumbnail/preview/image.
const KNOWN_RENDITIONS: &[&str] = &["thumbnail", "preview", "image"];

/// Privacy/data-collection knobs a model may declare. `deny` maps to
/// `provider.data_collection = "deny"` on the wire (OpenRouter); other providers
/// ignore the knob. Kept as a closed set so a config cannot smuggle in arbitrary
/// provider routing flags.
const KNOWN_DATA_COLLECTION: &[&str] = &["allow", "deny"];

/// Reserved HTTP headers a provider config may not set as attribution headers —
/// the driver owns `Authorization` and `Content-Type`, and `Host`/`Content-Length`/
/// `Cookie` are transport-controlled. A config setting them would be ignored at
/// best and a header-injection footgun at worst, so validation rejects them.
const RESERVED_HEADERS: &[&str] = &[
    "authorization",
    "content-type",
    "host",
    "content-length",
    "cookie",
];

/// Typed validation / load error. Carries enough context to surface an
/// actionable diagnostic (which file, which field) without leaking secret
/// material — the raw-secret scan runs before this is ever built from a value.
#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("provider config {origin}: {message}")]
    Invalid { origin: String, message: String },
    #[error("provider config read failed ({path}): {error}")]
    Io {
        path: String,
        #[source]
        error: std::io::Error,
    },
    #[error("provider config parse failed ({origin}): {message}")]
    Parse { origin: String, message: String },
}

impl ConfigError {
    fn invalid(origin: impl Into<String>, message: impl Into<String>) -> Self {
        Self::Invalid {
            origin: origin.into(),
            message: message.into(),
        }
    }
}

/// Auth declaration. `credential_slot` is a slot *name* (e.g. `openrouter_api_key`),
/// never the secret itself; the Rust vault resolves the slot at call time.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct AuthConfig {
    #[serde(rename = "type")]
    pub auth_type: String,
    pub credential_slot: String,
}

/// Structured-output injection mode. The driver owns the exact wire mapping; the
/// config only selects the mechanism and whether strict mode is requested.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct StructuredOutputConfig {
    pub mode: String,
    /// Forward-looking: read by the Phase 5c driver when building the request body.
    #[serde(default)]
    #[allow(dead_code)]
    pub strict: bool,
    /// Forward-looking: read by the Phase 5c OpenRouter driver (`provider.require_parameters`).
    #[serde(default)]
    #[allow(dead_code)]
    pub provider_require_parameters: bool,
}

/// Response extraction. `content_json_pointer = null` means the driver owns
/// response-content extraction with a typed parser (the Volcengine Ark path);
/// a non-null pointer must be a valid JSON Pointer into the provider JSON.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct ResponseConfig {
    #[serde(default)]
    pub content_json_pointer: Option<String>,
    #[serde(default)]
    pub usage_json_pointer: Option<String>,
    #[serde(default)]
    pub provider_request_id_json_pointer: Option<String>,
    #[serde(default)]
    pub provider_request_id_header: Option<String>,
}

/// Provider defaults applied when a request omits the field. `stream` is forced
/// false in Phase 5 (no streaming MVP); a config asking for streaming fails closed.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct DefaultsConfig {
    pub model: String,
    #[serde(default)]
    pub stream: bool,
    #[serde(default)]
    pub temperature: f64,
}

/// Bounded call limits.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct LimitsConfig {
    pub timeout_ms: u64,
    pub max_image_bytes: u64,
    pub max_output_tokens: u64,
}

/// A curated model entry. A model is advertised as an image-analysis capability
/// only when `supports_vision && supports_structured_output` OR `live_confirmed`
/// is true (Phase 6a: fail closed rather than rely on best-effort free-form JSON,
/// and do not advertise a compatible-preset model as image-analysis capable until
/// a live smoke confirms both image input and structured JSON output).
#[derive(Debug, Clone, serde::Deserialize)]
pub struct ModelConfig {
    pub slug: String,
    pub display_name: String,
    #[serde(default)]
    pub supports_vision: bool,
    #[serde(default)]
    pub supports_structured_output: bool,
    /// Phase 6a: a live smoke has explicitly pinned that this model accepts image
    /// input AND returns validated structured JSON for the configured protocol.
    /// When true the model is advertised even if `supports_vision` /
    /// `supports_structured_output` are still false (the pin IS the confirmation).
    /// Ship preset models with this false until a live smoke confirms capability.
    #[serde(default)]
    #[allow(dead_code)]
    pub live_confirmed: bool,
    #[serde(default)]
    pub max_image_bytes: Option<u64>,
    #[serde(default)]
    pub recommended_rendition: Option<String>,
    #[serde(default)]
    pub cost_per_million_input_usd: Option<f64>,
    #[serde(default)]
    pub cost_per_million_output_usd: Option<f64>,
    #[serde(default)]
    pub data_collection: Option<String>,
}

/// A validated provider configuration. Built-ins are embedded; user configs are
/// loaded from a directory and may override a built-in by `provider_id` or add a
/// new one. Two user configs with the same `provider_id` is an error.
#[derive(Debug, Clone, serde::Deserialize)]
pub struct ProviderConfig {
    pub schema_version: u32,
    pub provider_id: String,
    /// Forward-looking: shown by the C++ host in Phase 6 provider settings.
    #[allow(dead_code)]
    pub display_name: String,
    pub driver: String,
    pub base_url: String,
    pub endpoint: String,
    /// Phase 6c: optional override for the model-listing (discovery) endpoint
    /// path. When `None`, the driver discovers models at `{base_url}/models`
    /// (the OpenAI- and Anthropic-compatible default). When `Some`, it must be a
    /// path beginning with `/` (e.g. `/v1/models`) for providers that expose
    /// listing elsewhere. Discovery is a dry run: discovered models are returned
    /// to the host as unverified candidates and are NOT advertised as
    /// image-analysis capable until a validation smoke pins capability.
    #[serde(default)]
    #[allow(dead_code)]
    pub models_endpoint: Option<String>,
    /// Optional parser knobs for model-list discovery responses. When omitted,
    /// drivers keep the compatible default: a top-level `data` array whose items
    /// are model objects with `id` plus optional `display_name` / `name`.
    #[serde(default)]
    pub models_response: ModelListResponseConfig,
    pub auth: AuthConfig,
    #[serde(default)]
    pub attribution_headers: HashMap<String, String>,
    pub defaults: DefaultsConfig,
    pub structured_output: StructuredOutputConfig,
    pub response: ResponseConfig,
    pub limits: LimitsConfig,
    #[serde(default)]
    pub models: Vec<ModelConfig>,
}

#[derive(Debug, Clone, Default, serde::Deserialize)]
pub struct ModelListResponseConfig {
    /// JSON Pointer to the model array. `None` or blank means the compatible
    /// default `/data`; local routers such as cc-switch can use `/models`.
    #[serde(default)]
    pub data_json_pointer: Option<String>,
    /// Optional JSON Pointer relative to each object item for the model id.
    /// Blank or absent keeps the default `id`; string array items are accepted
    /// as model ids regardless of this field.
    #[serde(default)]
    pub id_json_pointer: Option<String>,
    /// Optional JSON Pointer relative to each object item for display text.
    /// Blank or absent keeps the default `display_name`, then `name`, then id.
    #[serde(default)]
    pub display_name_json_pointer: Option<String>,
}

/// Ordered set of validated provider configs keyed by `provider_id` for lookup.
/// Insertion order is preserved so capability descriptors are deterministic.
#[derive(Debug, Clone, Default)]
pub struct ProviderRegistry {
    configs: Vec<ProviderConfig>,
    index: HashMap<String, usize>,
}

impl ProviderRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    #[allow(dead_code)]
    pub fn len(&self) -> usize {
        self.configs.len()
    }

    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        self.configs.is_empty()
    }

    pub fn iter(&self) -> impl Iterator<Item = &ProviderConfig> {
        self.configs.iter()
    }

    pub fn get(&self, provider_id: &str) -> Option<&ProviderConfig> {
        self.index.get(provider_id).map(|i| &self.configs[*i])
    }

    /// Insert a config, replacing any existing config with the same `provider_id`.
    /// Returns the replaced config if any.
    fn upsert(&mut self, config: ProviderConfig) -> Option<ProviderConfig> {
        match self.index.get(&config.provider_id) {
            Some(&i) => {
                let old = std::mem::replace(&mut self.configs[i], config);
                Some(old)
            }
            None => {
                self.index
                    .insert(config.provider_id.clone(), self.configs.len());
                self.configs.push(config);
                None
            }
        }
    }
}

/// Built-in provider configs embedded into the binary. Adding a built-in means
/// adding a file here and an `include_str!` entry — Windows packaging cannot drop
/// them because they live in the binary, not on disk.
const BUILTIN_PROVIDER_CONFIGS: &[(&str, &str)] = &[
    (
        "openrouter",
        include_str!("../../configs/providers/openrouter.json"),
    ),
    (
        "volcengine_ark",
        include_str!("../../configs/providers/volcengine_ark.json"),
    ),
    (
        "volcengine_ark_coding",
        include_str!("../../configs/providers/volcengine_ark_coding.json"),
    ),
    // Phase 6a OpenCode Go compatible presets (protocol-first). The public
    // /models endpoint exposes ids but not granular vision metadata, so
    // provider-authored defaults mark documented model ids as image-analysis
    // candidates.
    (
        "opencode_go_anthropic",
        include_str!("../../configs/providers/opencode_go_anthropic.json"),
    ),
    (
        "opencode_go_openai",
        include_str!("../../configs/providers/opencode_go_openai.json"),
    ),
    // CC Switch local routing presets. Credentials and upstream provider state
    // are owned by CC Switch; Alcedo only points compatible clients at the local
    // router.
    (
        "ccswitch_anthropic",
        include_str!("../../configs/providers/ccswitch_anthropic.json"),
    ),
    (
        "ccswitch_openai",
        include_str!("../../configs/providers/ccswitch_openai.json"),
    ),
    (
        "openai_codex_oauth",
        include_str!("../../configs/providers/openai_codex_oauth.json"),
    ),
];

/// Load built-in configs and optional user configs from `user_dir`.
///
/// Built-ins are validated strictly: an invalid built-in is a hard error (the
/// shipped binary is broken). User configs are validated individually: an
/// invalid user config is logged via `tracing::warn` and skipped (fail closed —
/// that provider is not offered), while valid user configs override a built-in
/// by `provider_id` or add a new provider. Two user configs sharing a
/// `provider_id` is an error (the first wins; a later one is skipped), whether
/// or not that id is also a built-in. Both built-in and user configs run the
/// raw-secret scan before deserializing, so unknown fields holding a key are
/// rejected rather than silently ignored by serde.
pub fn load_provider_configs(user_dir: Option<&Path>) -> Result<ProviderRegistry, ConfigError> {
    let mut registry = ProviderRegistry::new();

    for (id, raw) in BUILTIN_PROVIDER_CONFIGS {
        let config = parse_and_validate(raw, format!("builtin:{id}"))?;
        registry.upsert(config);
    }

    if let Some(dir) = user_dir {
        load_user_configs(&mut registry, dir)?;
    }

    Ok(registry)
}

fn load_user_configs(registry: &mut ProviderRegistry, dir: &Path) -> Result<(), ConfigError> {
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => {
            // Missing user dir is not an error: built-ins still load.
            tracing::info!(
                "provider config dir {:?} does not exist; using built-ins only",
                dir
            );
            return Ok(());
        }
        Err(err) => {
            return Err(ConfigError::Io {
                path: dir.display().to_string(),
                error: err,
            });
        }
    };

    // Deterministic order so duplicate-provider-id errors are reproducible.
    let mut paths: Vec<std::path::PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().and_then(|s| s.to_str()) == Some("json"))
        .collect();
    paths.sort();

    // Track which provider_ids have already been loaded from USER configs. This
    // is deliberately separate from `registry.get(...)` + `is_builtin(...)`:
    // a user may override a built-in by `provider_id` (e.g. openrouter), and
    // after that first override the registry entry is the user's config while
    // the id is still a built-in id. Without this set, a second user config
    // with the same built-in id would silently clobber the first user override,
    // because the old `!is_builtin` guard only caught user-on-user dupes for
    // non-builtin ids. With this set, the first user config wins and any later
    // user config sharing the id is skipped + warned, regardless of whether
    // the id is also a built-in.
    let mut seen_user_provider_ids: HashSet<String> = HashSet::new();

    for path in paths {
        let source = path.display().to_string();
        let raw = match std::fs::read_to_string(&path) {
            Ok(s) => s,
            Err(err) => {
                tracing::warn!("skipping user provider config {source}: {err}");
                continue;
            }
        };
        // A user config file may hold one config object or an array of configs.
        let value: Value = match serde_json::from_str(&raw) {
            Ok(v) => v,
            Err(err) => {
                tracing::warn!("skipping user provider config {source}: parse error: {err}");
                continue;
            }
        };
        // Scan the raw JSON for secret material BEFORE deserializing into the
        // typed struct, so unknown fields holding a key are caught too. This is
        // the same "configs are data only / no raw secrets" guard the built-in
        // path runs in `parse_and_validate`; without it, a user JSON with an
        // `api_key` / `Bearer ...` field would be silently accepted, because
        // serde ignores unknown fields and the typed `ProviderConfig` never
        // sees them. A user config that fails the scan is skipped + warned
        // (fail closed — that provider is not offered), not a hard error.
        if let Err(err) = scan_for_secrets(&value, &source) {
            tracing::warn!("skipping user provider config {source}: {err}");
            continue;
        }
        let configs: Vec<Value> = match value {
            Value::Array(items) => items,
            other => vec![other],
        };
        for item in configs {
            match serde_json::from_value::<ProviderConfig>(item.clone()) {
                Ok(config) => match validate_config(&config, &source) {
                    Ok(()) => {
                        if seen_user_provider_ids.contains(&config.provider_id) {
                            // Two user configs sharing a provider_id (whether or
                            // not it is also a built-in id) is an error surfaced
                            // as a skip + warning; the first user config wins.
                            tracing::warn!(
                                "skipping user provider config {source}: duplicate provider_id {:?} already loaded from another user config",
                                config.provider_id
                            );
                        } else {
                            seen_user_provider_ids.insert(config.provider_id.clone());
                            registry.upsert(config);
                        }
                    }
                    Err(err) => {
                        tracing::warn!("skipping user provider config {source}: {err}");
                    }
                },
                Err(err) => {
                    tracing::warn!("skipping user provider config {source}: {err}");
                }
            }
        }
    }
    Ok(())
}

fn parse_and_validate(raw: &str, source: String) -> Result<ProviderConfig, ConfigError> {
    // Scan the raw JSON for secret material BEFORE deserializing into the typed
    // struct, so unknown fields holding a key are caught too. This is the
    // "configs are data only / no raw secrets" guard.
    let value: Value = serde_json::from_str(raw).map_err(|e| ConfigError::Parse {
        origin: source.clone(),
        message: e.to_string(),
    })?;
    scan_for_secrets(&value, &source)?;
    let config: ProviderConfig = serde_json::from_value(value).map_err(|e| ConfigError::Parse {
        origin: source.clone(),
        message: e.to_string(),
    })?;
    validate_config(&config, &source)?;
    Ok(config)
}

fn validate_config(config: &ProviderConfig, source: &str) -> Result<(), ConfigError> {
    if config.schema_version != KNOWN_SCHEMA_VERSION {
        return Err(ConfigError::invalid(
            source,
            format!(
                "schema_version {} does not match known version {}",
                config.schema_version, KNOWN_SCHEMA_VERSION
            ),
        ));
    }
    if config.provider_id.trim().is_empty() {
        return Err(ConfigError::invalid(
            source,
            "provider_id must not be empty",
        ));
    }
    if !is_known(KNOWN_DRIVER_IDS, &config.driver) {
        return Err(ConfigError::invalid(
            source,
            format!(
                "unknown driver {:?}; known: {:?}",
                config.driver, KNOWN_DRIVER_IDS
            ),
        ));
    }
    validate_base_url(&config.base_url, source)?;
    validate_endpoint_path(&config.endpoint, "endpoint", source)?;
    if let Some(list_endpoint) = &config.models_endpoint {
        validate_endpoint_path(list_endpoint, "models_endpoint", source)?;
    }
    validate_model_list_response(&config.models_response, source)?;
    if !is_known(KNOWN_AUTH_TYPES, &config.auth.auth_type) {
        return Err(ConfigError::invalid(
            source,
            format!(
                "unknown auth.type {:?}; known: {:?}",
                config.auth.auth_type, KNOWN_AUTH_TYPES
            ),
        ));
    }
    if !is_valid_credential_slot(&config.auth.credential_slot) {
        return Err(ConfigError::invalid(
            source,
            format!(
                "credential_slot {:?} must be non-empty and match [a-z0-9_]+",
                config.auth.credential_slot
            ),
        ));
    }
    for (name, _value) in &config.attribution_headers {
        validate_header_name(name, source)?;
    }
    if !is_known(KNOWN_SO_MODES, &config.structured_output.mode) {
        return Err(ConfigError::invalid(
            source,
            format!(
                "unknown structured_output.mode {:?}; known: {:?}",
                config.structured_output.mode, KNOWN_SO_MODES
            ),
        ));
    }
    validate_optional_pointer(
        &config.response.content_json_pointer,
        "content_json_pointer",
        source,
    )?;
    validate_optional_pointer(
        &config.response.usage_json_pointer,
        "usage_json_pointer",
        source,
    )?;
    validate_optional_pointer(
        &config.response.provider_request_id_json_pointer,
        "provider_request_id_json_pointer",
        source,
    )?;
    if let Some(header) = &config.response.provider_request_id_header {
        validate_header_name(header, source)?;
    }
    validate_defaults(&config.defaults, source)?;
    validate_limits(&config.limits, source)?;
    validate_models(&config.models, source)?;
    Ok(())
}

fn validate_defaults(d: &DefaultsConfig, source: &str) -> Result<(), ConfigError> {
    if d.model.trim().is_empty() {
        return Err(ConfigError::invalid(
            source,
            "defaults.model must not be empty",
        ));
    }
    if d.stream {
        return Err(ConfigError::invalid(
            source,
            "defaults.stream=true is not supported in Phase 5; set stream:false",
        ));
    }
    if d.temperature.is_nan() || d.temperature < 0.0 || d.temperature > 2.0 {
        return Err(ConfigError::invalid(
            source,
            format!(
                "defaults.temperature {} out of range [0.0, 2.0]",
                d.temperature
            ),
        ));
    }
    Ok(())
}

fn validate_limits(l: &LimitsConfig, source: &str) -> Result<(), ConfigError> {
    if l.timeout_ms < MIN_TIMEOUT_MS || l.timeout_ms > MAX_TIMEOUT_MS {
        return Err(ConfigError::invalid(
            source,
            format!(
                "limits.timeout_ms {} out of range [{}, {}]",
                l.timeout_ms, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS
            ),
        ));
    }
    if l.max_image_bytes == 0 || l.max_image_bytes > MAX_IMAGE_BYTES {
        return Err(ConfigError::invalid(
            source,
            format!(
                "limits.max_image_bytes {} out of range [1, {}]",
                l.max_image_bytes, MAX_IMAGE_BYTES
            ),
        ));
    }
    if l.max_output_tokens < MIN_OUTPUT_TOKENS || l.max_output_tokens > MAX_OUTPUT_TOKENS {
        return Err(ConfigError::invalid(
            source,
            format!(
                "limits.max_output_tokens {} out of range [{}, {}]",
                l.max_output_tokens, MIN_OUTPUT_TOKENS, MAX_OUTPUT_TOKENS
            ),
        ));
    }
    Ok(())
}

fn validate_models(models: &[ModelConfig], source: &str) -> Result<(), ConfigError> {
    for m in models {
        if m.slug.trim().is_empty() {
            return Err(ConfigError::invalid(source, "model slug must not be empty"));
        }
        if m.display_name.trim().is_empty() {
            return Err(ConfigError::invalid(
                source,
                "model display_name must not be empty",
            ));
        }
        if let Some(rend) = &m.recommended_rendition {
            if !is_known(KNOWN_RENDITIONS, rend) {
                return Err(ConfigError::invalid(
                    source,
                    format!(
                        "model {:?} recommended_rendition {:?}; known: {:?}",
                        m.slug, rend, KNOWN_RENDITIONS
                    ),
                ));
            }
        }
        if let Some(dc) = &m.data_collection {
            if !is_known(KNOWN_DATA_COLLECTION, dc) {
                return Err(ConfigError::invalid(
                    source,
                    format!(
                        "model {:?} data_collection {:?}; known: {:?}",
                        m.slug, dc, KNOWN_DATA_COLLECTION
                    ),
                ));
            }
        }
        if let Some(c) = m.cost_per_million_input_usd {
            if c.is_nan() || c < 0.0 {
                return Err(ConfigError::invalid(
                    source,
                    format!("model {:?} cost_per_million_input_usd invalid", m.slug),
                ));
            }
        }
        if let Some(c) = m.cost_per_million_output_usd {
            if c.is_nan() || c < 0.0 {
                return Err(ConfigError::invalid(
                    source,
                    format!("model {:?} cost_per_million_output_usd invalid", m.slug),
                ));
            }
        }
        if let Some(b) = m.max_image_bytes {
            if b == 0 || b > MAX_IMAGE_BYTES {
                return Err(ConfigError::invalid(
                    source,
                    format!(
                        "model {:?} max_image_bytes {} out of range [1, {}]",
                        m.slug, b, MAX_IMAGE_BYTES
                    ),
                ));
            }
        }
    }
    Ok(())
}

fn validate_header_name(name: &str, source: &str) -> Result<(), ConfigError> {
    if name.is_empty() {
        return Err(ConfigError::invalid(
            source,
            "attribution header name must not be empty",
        ));
    }
    if RESERVED_HEADERS
        .iter()
        .any(|r| r.eq_ignore_ascii_case(name))
    {
        return Err(ConfigError::invalid(
            source,
            format!(
                "attribution header {:?} is reserved (driver/transport-owned)",
                name
            ),
        ));
    }
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '-') {
        return Err(ConfigError::invalid(
            source,
            format!(
                "attribution header {:?} contains characters outside [A-Za-z0-9-]",
                name
            ),
        ));
    }
    Ok(())
}

fn validate_model_list_response(
    response: &ModelListResponseConfig,
    source: &str,
) -> Result<(), ConfigError> {
    validate_optional_pointer(
        &response.data_json_pointer,
        "models_response.data_json_pointer",
        source,
    )?;
    validate_optional_pointer(
        &response.id_json_pointer,
        "models_response.id_json_pointer",
        source,
    )?;
    validate_optional_pointer(
        &response.display_name_json_pointer,
        "models_response.display_name_json_pointer",
        source,
    )?;
    Ok(())
}

fn validate_optional_pointer(
    p: &Option<String>,
    field: &str,
    source: &str,
) -> Result<(), ConfigError> {
    match p {
        None => Ok(()), // driver-owned parser mode
        Some(s) => validate_json_pointer(s, field, source),
    }
}

/// Validate a JSON Pointer (RFC 6901). Empty string means the whole document
/// (valid root pointer). A non-empty pointer must start with `/` and any `~` must
/// be `~0` or `~1`. `null` (None) is handled by the caller and means the driver
/// owns extraction — not a pointer at all.
fn validate_json_pointer(p: &str, field: &str, source: &str) -> Result<(), ConfigError> {
    if p.is_empty() {
        return Ok(());
    }
    if !p.starts_with('/') {
        return Err(ConfigError::invalid(
            source,
            format!("{field} {:?} must start with '/' or be empty", p),
        ));
    }
    let mut chars = p.chars().peekable();
    chars.next(); // leading '/'
    while let Some(c) = chars.next() {
        if c == '~' {
            match chars.next() {
                Some('0') | Some('1') => {}
                Some(other) => {
                    return Err(ConfigError::invalid(
                        source,
                        format!("{field} {:?}: invalid escape ~{other} (use ~0 or ~1)", p),
                    ));
                }
                None => {
                    return Err(ConfigError::invalid(
                        source,
                        format!("{field} {:?}: dangling '~' at end", p),
                    ));
                }
            }
        }
    }
    Ok(())
}

fn is_known(set: &[&str], value: &str) -> bool {
    set.iter().any(|s| *s == value)
}

fn validate_base_url(url: &str, source: &str) -> Result<(), ConfigError> {
    let parsed = Url::parse(url).map_err(|err| {
        ConfigError::invalid(
            source,
            format!("base_url {:?} is not a valid URL: {err}", url),
        )
    })?;
    if parsed.username() != "" || parsed.password().is_some() {
        return Err(ConfigError::invalid(
            source,
            format!("base_url {:?} must not contain userinfo", url),
        ));
    }
    if parsed.query().is_some() || parsed.fragment().is_some() {
        return Err(ConfigError::invalid(
            source,
            format!("base_url {:?} must not contain query or fragment", url),
        ));
    }
    match parsed.scheme() {
        "https" => Ok(()),
        "http" if parsed_host_is_loopback(&parsed) => Ok(()),
        "http" => Err(ConfigError::invalid(
            source,
            format!(
                "base_url {:?} must be https:// (http:// only allowed for loopback)",
                url
            ),
        )),
        other => Err(ConfigError::invalid(
            source,
            format!(
                "base_url {:?} has unsupported scheme {:?}; use https://",
                url, other
            ),
        )),
    }
}

fn parsed_host_is_loopback(url: &Url) -> bool {
    let Some(host) = url.host_str() else {
        return false;
    };
    if host.eq_ignore_ascii_case("localhost") {
        return true;
    }
    host.parse::<IpAddr>()
        .map(|addr| addr.is_loopback())
        .unwrap_or(false)
}

fn validate_endpoint_path(path: &str, field: &str, source: &str) -> Result<(), ConfigError> {
    if !path.starts_with('/') || path.starts_with("//") {
        return Err(ConfigError::invalid(
            source,
            format!(
                "{field} {:?} must be a relative URL path starting with one '/'",
                path
            ),
        ));
    }
    if path.contains('?') || path.contains('#') {
        return Err(ConfigError::invalid(
            source,
            format!("{field} {:?} must not contain query or fragment", path),
        ));
    }
    if path.chars().any(|c| c == '\r' || c == '\n') {
        return Err(ConfigError::invalid(
            source,
            format!("{field} {:?} must not contain line breaks", path),
        ));
    }
    Ok(())
}

fn is_valid_credential_slot(slot: &str) -> bool {
    !slot.is_empty()
        && slot
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
}

/// Secret-key field names a config must never contain. The config references a
/// credential *slot*, never the secret; a field named like a key is rejected
/// regardless of its value. Matched against the lowercased key name.
const SECRET_KEY_NAMES: &[&str] = &[
    "api_key",
    "apikey",
    "api_token",
    "secret",
    "secrets",
    "password",
    "passwd",
    "token",
    "tokens",
    "bearer",
    "bearer_token",
    "access_token",
    "refresh_token",
    "private_key",
    "client_secret",
    "signing_key",
];

/// Walk a parsed config JSON value and reject any field that looks like embedded
/// secret material. This is the "configs are data only / no raw secrets" guard
/// (Phase 5a review focus). Catches both denied key names and values that look
/// like leaked bearer/OpenAI-style keys.
fn scan_for_secrets(value: &Value, source: &str) -> Result<(), ConfigError> {
    match value {
        Value::Object(map) => {
            for (key, val) in map {
                let lower = key.to_ascii_lowercase();
                if SECRET_KEY_NAMES.contains(&lower.as_str())
                    || lower.ends_with("_key")
                    || lower.ends_with("_secret")
                    || lower.ends_with("_token")
                    || lower.ends_with("_password")
                    || lower.ends_with("_apikey")
                {
                    return Err(ConfigError::invalid(
                        source,
                        format!(
                            "field {:?} looks like embedded secret material; use auth.credential_slot instead",
                            key
                        ),
                    ));
                }
                scan_for_secrets(val, source)?;
            }
        }
        Value::Array(items) => {
            for item in items {
                scan_for_secrets(item, source)?;
            }
        }
        Value::String(s) => {
            if looks_like_leaked_secret(s) {
                return Err(ConfigError::invalid(
                    source,
                    "config contains a string value that looks like a leaked API key or bearer token",
                ));
            }
        }
        _ => {}
    }
    Ok(())
}

fn looks_like_leaked_secret(s: &str) -> bool {
    // OpenAI-style: "sk-" followed by 16+ word/hyphen/underscore chars.
    if let Some(rest) = s.strip_prefix("sk-") {
        if rest.len() >= 16
            && rest
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
        {
            return true;
        }
    }
    // Literal "Bearer <token>" header value.
    if s.starts_with("Bearer ") && s.len() > 8 {
        return true;
    }
    // AWS-style access key id.
    if s.starts_with("AKIA") && s.len() == 20 && s.chars().all(|c| c.is_ascii_alphanumeric()) {
        return true;
    }
    false
}

/// Build `AiCapability` descriptors for the advertised image-analysis tasks from
/// loaded provider configs. Only models with `supports_vision &&
/// supports_structured_output` are advertised (the plan: fail closed rather than
/// rely on best-effort free-form JSON). Each such model emits one
/// `image_understanding.describe` and one `image_rating.score` descriptor, so the
/// C++ host can display remote-provider availability before a task starts.
///
/// `requires_credential` is true for remote providers (auth is bearer via the
/// vault); the mock provider (Phase 5b) advertises its own no-credential
/// descriptor separately.
pub fn build_provider_capability_descriptors(registry: &ProviderRegistry) -> Vec<AiCapability> {
    let mut out = Vec::new();
    for config in registry.iter() {
        let requires_credential = config.auth.auth_type != "none";
        for model in &config.models {
            // Phase 6a advertisement rule: a preset is advertised for image
            // analysis only when the model has supports_vision &&
            // supports_structured_output OR a live smoke has explicitly pinned
            // that capability (live_confirmed). Compatible-preset models ship
            // unverified (both flags false, live_confirmed false) so they are
            // NOT advertised until a live smoke confirms image + structured JSON.
            let capable =
                (model.supports_vision && model.supports_structured_output) || model.live_confirmed;
            if !capable {
                continue;
            }
            // Phase 6b fail-closed: never advertise a capability whose driver is
            // not wired in this build, even if a user config pinned
            // live_confirmed = true. An advertised-but-unregistered provider
            // would surface as UNSUPPORTED_TASK at call time; suppressing the
            // descriptor here keeps ListCapabilities honest. The set of wired
            // drivers is owned by `providers::is_driver_wired` so this gate and
            // `build_real_image_providers` cannot drift apart.
            if !crate::service::providers::is_driver_wired(&config.driver) {
                continue;
            }
            let max_payload = model
                .max_image_bytes
                .unwrap_or(config.limits.max_image_bytes) as i64;

            out.push(AiCapability {
                task_id: "image_understanding.describe".to_string(),
                provider_id: config.provider_id.clone(),
                model_id: model.slug.clone(),
                input_kinds: vec![
                    AiInputKind::AiInputThumbnail as i32,
                    AiInputKind::AiInputPreview as i32,
                    AiInputKind::AiInputImage as i32,
                ],
                output_kinds: vec![
                    AiOutputKind::AiOutputCaption as i32,
                    AiOutputKind::AiOutputTags as i32,
                ],
                supports_batch: false,
                supports_cancel: true,
                requires_credential,
                max_payload_bytes: max_payload,
            });
            out.push(AiCapability {
                task_id: "image_rating.score".to_string(),
                provider_id: config.provider_id.clone(),
                model_id: model.slug.clone(),
                input_kinds: vec![
                    AiInputKind::AiInputThumbnail as i32,
                    AiInputKind::AiInputPreview as i32,
                    AiInputKind::AiInputImage as i32,
                ],
                output_kinds: vec![AiOutputKind::AiOutputScore as i32],
                supports_batch: false,
                supports_cancel: true,
                requires_credential,
                max_payload_bytes: max_payload,
            });
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn builtin_registry() -> ProviderRegistry {
        load_provider_configs(None).expect("built-in configs load and validate")
    }

    #[test]
    fn loads_built_in_configs() {
        let registry = builtin_registry();
        // OpenRouter + Volcengine Ark + Volcengine Ark Coding Plan + Opencode Go
        // (Anthropic) + Opencode Go (OpenAI) + 2 CC Switch routing presets +
        // OpenAI Codex OAuth.
        assert_eq!(registry.len(), 8);
        let openrouter = registry.get("openrouter").expect("openrouter present");
        assert_eq!(openrouter.driver, "openrouter_chat");
        assert_eq!(openrouter.base_url, "https://openrouter.ai/api/v1");
        assert_eq!(openrouter.endpoint, "/chat/completions");
        assert_eq!(openrouter.auth.credential_slot, "openrouter_api_key");
        assert_eq!(openrouter.defaults.model, "qwen/qwen3.7-plus");
        assert!(!openrouter.defaults.stream);
        assert_eq!(
            openrouter.structured_output.mode,
            "response_format_json_schema"
        );
        assert!(openrouter.structured_output.provider_require_parameters);
        assert_eq!(
            openrouter.response.content_json_pointer.as_deref(),
            Some("/choices/0/message/content")
        );
        // Phase 6a: OpenRouter copy is demoted to legacy / optional smoke wording.
        assert!(
            openrouter.display_name.to_lowercase().contains("legacy"),
            "openrouter display_name should mark it legacy: {}",
            openrouter.display_name
        );

        let ark = registry
            .get("volcengine_ark")
            .expect("volcengine_ark present");
        assert_eq!(ark.driver, "volcengine_ark_responses");
        // content_json_pointer: null -> driver-owned parser.
        assert_eq!(ark.response.content_json_pointer, None);
        assert_eq!(ark.defaults.model, "doubao-seed-2-0-lite-260428");

        let coding = registry
            .get("volcengine_ark_coding")
            .expect("volcengine_ark_coding present");
        assert_eq!(coding.driver, "anthropic_messages");
        assert_eq!(
            coding.base_url,
            "https://ark.cn-beijing.volces.com/api/coding"
        );
        assert_eq!(coding.endpoint, "/v1/messages");
        // Coding Plan uses Claude-Code-style bearer auth and reuses the Ark key slot.
        assert_eq!(coding.auth.auth_type, "bearer");
        assert_eq!(coding.auth.credential_slot, "volcengine_ark_api_key");
        assert_eq!(coding.defaults.model, "doubao-seed-2.0-lite");
        // Structured output via Anthropic tool-use; driver-owned parser.
        assert_eq!(coding.structured_output.mode, "tool");
        assert!(coding.structured_output.strict);
        assert_eq!(coding.response.content_json_pointer, None);

        // Phase 6a Opencode Go compatible presets (protocol-first).
        let oc_anthropic = registry
            .get("opencode_go_anthropic")
            .expect("opencode_go_anthropic present");
        assert_eq!(oc_anthropic.driver, "anthropic_messages");
        assert_eq!(oc_anthropic.base_url, "https://opencode.ai/zen/go/v1");
        assert_eq!(oc_anthropic.endpoint, "/messages");
        assert_eq!(oc_anthropic.auth.auth_type, "api_key_header");
        assert_eq!(oc_anthropic.auth.credential_slot, "opencode_api_key");
        assert_eq!(oc_anthropic.structured_output.mode, "tool");
        // OpenCode Go exposes provider-level structured output; model ids are provider-listed candidates.
        assert!(oc_anthropic.models[0].supports_vision);
        assert!(oc_anthropic.models[0].supports_structured_output);
        assert!(!oc_anthropic.models[0].live_confirmed);

        let oc_openai = registry
            .get("opencode_go_openai")
            .expect("opencode_go_openai present");
        assert_eq!(oc_openai.driver, "openai_chat_compatible");
        assert_eq!(oc_openai.base_url, "https://opencode.ai/zen/go/v1");
        assert_eq!(oc_openai.endpoint, "/chat/completions");
        assert_eq!(oc_openai.auth.credential_slot, "opencode_api_key");
        assert_eq!(
            oc_openai.structured_output.mode,
            "response_format_json_schema"
        );
        assert_eq!(
            oc_openai.response.content_json_pointer.as_deref(),
            Some("/choices/0/message/content")
        );
        assert!(oc_openai.models[0].supports_vision);
        assert!(oc_openai.models[0].supports_structured_output);
        assert!(!oc_openai.models[0].live_confirmed);

        let cc_anthropic = registry
            .get("ccswitch_anthropic")
            .expect("ccswitch_anthropic present");
        assert_eq!(cc_anthropic.driver, "anthropic_messages");
        assert_eq!(cc_anthropic.base_url, "http://127.0.0.1:15721");
        assert_eq!(cc_anthropic.endpoint, "/v1/messages");
        assert_eq!(cc_anthropic.models_endpoint.as_deref(), Some("/v1/models"));
        assert_eq!(
            cc_anthropic.models_response.data_json_pointer.as_deref(),
            Some("/models")
        );
        assert_eq!(cc_anthropic.auth.auth_type, "none");
        assert_eq!(cc_anthropic.auth.credential_slot, "ccswitch_local_route");
        assert_eq!(cc_anthropic.defaults.model, "ccswitch-routed");
        assert!(cc_anthropic.models[0].supports_vision);
        assert!(cc_anthropic.models[0].supports_structured_output);

        let cc_openai = registry
            .get("ccswitch_openai")
            .expect("ccswitch_openai present");
        assert_eq!(cc_openai.driver, "openai_chat_compatible");
        assert_eq!(cc_openai.base_url, "http://127.0.0.1:15721/v1");
        assert_eq!(cc_openai.endpoint, "/chat/completions");
        assert_eq!(
            cc_openai.models_response.data_json_pointer.as_deref(),
            Some("/models")
        );
        assert_eq!(cc_openai.auth.auth_type, "none");
        assert_eq!(cc_openai.auth.credential_slot, "ccswitch_local_route");
        assert_eq!(cc_openai.defaults.model, "ccswitch-routed");
        assert!(cc_openai.models[0].supports_vision);
        assert!(cc_openai.models[0].supports_structured_output);
    }

    #[test]
    fn user_config_overrides_builtin_model_default() {
        let dir = tempdir();
        // Same provider_id as a built-in -> user overrides the built-in.
        let user = r#"{
            "schema_version": 1,
            "provider_id": "openrouter",
            "display_name": "OpenRouter (custom)",
            "driver": "openrouter_chat",
            "base_url": "https://openrouter.ai/api/v1",
            "endpoint": "/chat/completions",
            "auth": {"type": "bearer", "credential_slot": "openrouter_api_key"},
            "defaults": {"model": "custom/qwen-lt", "stream": false, "temperature": 0.1},
            "structured_output": {"mode": "response_format_json_schema", "strict": true},
            "response": {"content_json_pointer": "/choices/0/message/content"},
            "limits": {"timeout_ms": 30000, "max_image_bytes": 2097152, "max_output_tokens": 800},
            "models": [
                {"slug": "custom/qwen-lt", "display_name": "custom", "supports_vision": true, "supports_structured_output": true}
            ]
        }"#;
        write_config(&dir, "openrouter_override.json", user);

        let registry = load_provider_configs(Some(dir.path())).expect("override loads");
        let openrouter = registry.get("openrouter").expect("openrouter present");
        assert_eq!(openrouter.display_name, "OpenRouter (custom)");
        assert_eq!(openrouter.defaults.model, "custom/qwen-lt");
        assert_eq!(openrouter.models[0].slug, "custom/qwen-lt");
    }

    #[test]
    fn user_config_adds_new_provider() {
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1,
            "provider_id": "local_ollama",
            "display_name": "Local Ollama",
            "driver": "openai_chat_compatible",
            "base_url": "https://localhost:11434",
            "endpoint": "/v1/chat/completions",
            "auth": {"type": "none", "credential_slot": "unused_slot"},
            "defaults": {"model": "llama", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {"content_json_pointer": "/choices/0/message/content"},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": []
        }"#;
        write_config(&dir, "ollama.json", user);

        let registry = load_provider_configs(Some(dir.path())).expect("add loads");
        assert!(
            registry.get("local_ollama").is_some(),
            "user provider added"
        );
        // Built-ins still present.
        assert!(registry.get("openrouter").is_some());
        assert!(registry.get("volcengine_ark").is_some());
    }

    #[test]
    fn duplicate_user_provider_id_is_rejected() {
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1,
            "provider_id": "dupe",
            "display_name": "Dupe A",
            "driver": "openai_chat_compatible",
            "base_url": "https://localhost:11434",
            "endpoint": "/v1/chat/completions",
            "auth": {"type": "none", "credential_slot": "unused_slot"},
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {"content_json_pointer": "/choices/0/message/content"},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": []
        }"#;
        write_config(&dir, "a.json", user);
        write_config(&dir, "b.json", user);

        // Not a hard error (fail closed: second dupe skipped), but only one "dupe" remains.
        let registry =
            load_provider_configs(Some(dir.path())).expect("load does not hard-fail on dupe");
        assert_eq!(
            registry.iter().filter(|c| c.provider_id == "dupe").count(),
            1
        );
    }

    #[test]
    fn duplicate_user_override_of_builtin_does_not_silently_clobber() {
        // Two user files both override the built-in `openrouter` id. The first
        // user override must win; the second must be skipped as a user-on-user
        // duplicate. The old `registry.get(..) && !is_builtin(..)` guard failed
        // here because `is_builtin("openrouter")` is always true, so the second
        // override silently clobbered the first. Deterministic order: a_first
        // sorts before b_second.
        let dir = tempdir();
        let first = r#"{
            "schema_version": 1,
            "provider_id": "openrouter",
            "display_name": "OpenRouter (first user override)",
            "driver": "openrouter_chat",
            "base_url": "https://openrouter.ai/api/v1",
            "endpoint": "/chat/completions",
            "auth": {"type": "bearer", "credential_slot": "openrouter_api_key"},
            "defaults": {"model": "first/model", "stream": false, "temperature": 0.1},
            "structured_output": {"mode": "response_format_json_schema", "strict": true},
            "response": {"content_json_pointer": "/choices/0/message/content"},
            "limits": {"timeout_ms": 30000, "max_image_bytes": 2097152, "max_output_tokens": 800},
            "models": [
                {"slug": "first/model", "display_name": "first", "supports_vision": true, "supports_structured_output": true}
            ]
        }"#;
        let second = r#"{
            "schema_version": 1,
            "provider_id": "openrouter",
            "display_name": "OpenRouter (second user override)",
            "driver": "openrouter_chat",
            "base_url": "https://openrouter.ai/api/v1",
            "endpoint": "/chat/completions",
            "auth": {"type": "bearer", "credential_slot": "openrouter_api_key"},
            "defaults": {"model": "second/model", "stream": false, "temperature": 0.1},
            "structured_output": {"mode": "response_format_json_schema", "strict": true},
            "response": {"content_json_pointer": "/choices/0/message/content"},
            "limits": {"timeout_ms": 30000, "max_image_bytes": 2097152, "max_output_tokens": 800},
            "models": [
                {"slug": "second/model", "display_name": "second", "supports_vision": true, "supports_structured_output": true}
            ]
        }"#;
        write_config(&dir, "a_first.json", first);
        write_config(&dir, "b_second.json", second);

        let registry =
            load_provider_configs(Some(dir.path())).expect("load does not hard-fail on dupe");
        let openrouter = registry.get("openrouter").expect("openrouter present");
        // The first user override wins; the second is skipped, not silently
        // clobbered, even though openrouter is a built-in id.
        assert_eq!(openrouter.display_name, "OpenRouter (first user override)");
        assert_eq!(openrouter.defaults.model, "first/model");
        assert_eq!(openrouter.models[0].slug, "first/model");
    }

    #[test]
    fn unknown_driver_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "no_such_driver", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("unknown driver rejected");
        assert!(err.to_string().contains("unknown driver"), "{err}");
    }

    #[test]
    fn invalid_https_policy_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "http://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("plaintext http rejected");
        assert!(err.to_string().contains("https://"), "{err}");
    }

    #[test]
    fn http_localhost_allowed_for_dev() {
        let config = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "http://localhost:11434",
                "endpoint": "/v1/chat/completions", "auth": {"type": "none", "credential_slot": "x_slot"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect("localhost http allowed");
        assert_eq!(config.base_url, "http://localhost:11434");
    }

    #[test]
    fn base_url_rejects_userinfo_query_and_fragment() {
        for (url, expected) in [
            ("https://user@example.com", "userinfo"),
            ("https://example.com/api?x=1", "query or fragment"),
            ("https://example.com/api#frag", "query or fragment"),
        ] {
            let raw = format!(
                r#"{{
                    "schema_version": 1, "provider_id": "x", "display_name": "X",
                    "driver": "openai_chat_compatible", "base_url": "{url}",
                    "endpoint": "/x", "auth": {{"type": "bearer", "credential_slot": "x_key"}},
                    "defaults": {{"model": "m", "stream": false, "temperature": 0.2}},
                    "structured_output": {{"mode": "none"}},
                    "response": {{}}, "limits": {{"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200}},
                    "models": []
                }}"#
            );
            let err = match parse_and_validate(&raw, "test".to_string()) {
                Ok(_) => panic!("{url} rejected"),
                Err(err) => err,
            };
            assert!(err.to_string().contains(expected), "{url}: {err}");
        }
    }

    #[test]
    fn endpoint_rejects_network_path_query_and_fragment() {
        for endpoint in ["//evil.test/x", "/x?token=1", "/x#frag"] {
            let raw = format!(
                r#"{{
                    "schema_version": 1, "provider_id": "x", "display_name": "X",
                    "driver": "openai_chat_compatible", "base_url": "https://example.com",
                    "endpoint": "{endpoint}", "auth": {{"type": "bearer", "credential_slot": "x_key"}},
                    "defaults": {{"model": "m", "stream": false, "temperature": 0.2}},
                    "structured_output": {{"mode": "none"}},
                    "response": {{}}, "limits": {{"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200}},
                    "models": []
                }}"#
            );
            let err = match parse_and_validate(&raw, "test".to_string()) {
                Ok(_) => panic!("{endpoint} rejected"),
                Err(err) => err,
            };
            assert!(err.to_string().contains("endpoint"), "{endpoint}: {err}");
        }
    }

    #[test]
    fn raw_secret_in_api_key_field_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "api_key": "sk-1234567890abcdef1234567890abcdef",
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("raw secret rejected");
        assert!(err.to_string().contains("secret material"), "{err}");
    }

    #[test]
    fn leaked_bearer_value_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "note": "Bearer abc123tokenvalue",
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("bearer literal rejected");
        assert!(err.to_string().contains("leaked"), "{err}");
    }

    #[test]
    fn credential_slot_value_is_not_treated_as_secret() {
        // "openrouter_api_key" as a credential_slot VALUE is a slot name, not a
        // secret — it must not be rejected by the secret scan.
        let registry = builtin_registry();
        let openrouter = registry.get("openrouter").expect("openrouter present");
        assert_eq!(openrouter.auth.credential_slot, "openrouter_api_key");
    }

    #[test]
    fn user_config_with_raw_secret_in_unknown_field_is_skipped() {
        // The `api_key` field is unknown to ProviderConfig, so serde would
        // silently ignore it (the typed struct never sees it). The raw-secret
        // scan must run on the user path too and reject the config before
        // deserialize, otherwise the leaked key is accepted (P1: the user path
        // previously did not call scan_for_secrets). Fail closed: skipped, not a
        // hard error.
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1,
            "provider_id": "leaky",
            "display_name": "Leaky",
            "driver": "openai_chat_compatible",
            "base_url": "https://example.com",
            "endpoint": "/x",
            "auth": {"type": "bearer", "credential_slot": "leaky_key"},
            "api_key": "sk-1234567890abcdef1234567890abcdef",
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": []
        }"#;
        write_config(&dir, "leaky.json", user);

        let registry = load_provider_configs(Some(dir.path()))
            .expect("load does not hard-fail on a bad user config");
        assert!(
            registry.get("leaky").is_none(),
            "user config with embedded secret must be skipped"
        );
        // Built-ins still load.
        assert!(registry.get("openrouter").is_some());
    }

    #[test]
    fn user_config_with_leaked_bearer_in_unknown_field_is_skipped() {
        // A `note` field (unknown to serde, so silently ignored) carries a
        // `Bearer <token>` value. The raw-secret scan must catch the leaked
        // bearer value on the user path and skip the config (P1).
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1,
            "provider_id": "leaky_bearer",
            "display_name": "Leaky Bearer",
            "driver": "openai_chat_compatible",
            "base_url": "https://example.com",
            "endpoint": "/x",
            "auth": {"type": "bearer", "credential_slot": "leaky_bearer_key"},
            "note": "Bearer abc123tokenvalue",
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": []
        }"#;
        write_config(&dir, "leaky_bearer.json", user);

        let registry = load_provider_configs(Some(dir.path()))
            .expect("load does not hard-fail on a bad user config");
        assert!(
            registry.get("leaky_bearer").is_none(),
            "user config with leaked bearer must be skipped"
        );
    }

    #[test]
    fn invalid_json_pointer_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {"content_json_pointer": "choices/0/message"},
                "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("pointer without leading / rejected");
        assert!(err.to_string().contains("must start with '/'"), "{err}");
    }

    #[test]
    fn invalid_json_pointer_escape_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {"usage_json_pointer": "/foo~2bar"},
                "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("invalid escape rejected");
        assert!(err.to_string().contains("invalid escape"), "{err}");
    }

    /// Phase 6c: a config may set `models_endpoint` to override the default
    /// `/models` discovery path; it must start with `/`. Absent is valid (the
    /// driver falls back to `/models`).
    #[test]
    fn models_endpoint_override_validated() {
        // Absent -> valid (None).
        let cfg = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "tool"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect("absent models_endpoint is valid");
        assert!(cfg.models_endpoint.is_none());

        // Present with leading "/" -> valid.
        let cfg = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "anthropic_messages", "base_url": "https://example.com",
                "endpoint": "/v1/messages", "models_endpoint": "/v1/models",
                "auth": {"type": "api_key_header", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "tool"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect("leading-slash models_endpoint is valid");
        assert_eq!(cfg.models_endpoint.as_deref(), Some("/v1/models"));

        // Present WITHOUT leading "/" -> rejected.
        let err = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "models_endpoint": "v1/models",
                "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "tool"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect_err("models_endpoint without leading / rejected");
        assert!(
            err.to_string().contains("models_endpoint")
                && err.to_string().contains("relative URL path"),
            "{err}"
        );
    }

    #[test]
    fn models_response_pointers_validated() {
        let cfg = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x",
                "models_response": {
                    "data_json_pointer": "/models",
                    "id_json_pointer": "/model/id",
                    "display_name_json_pointer": "/model/name"
                },
                "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "tool"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect("valid model-list pointers accepted");
        assert_eq!(
            cfg.models_response.data_json_pointer.as_deref(),
            Some("/models")
        );

        let err = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x",
                "models_response": {"data_json_pointer": "models"},
                "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "tool"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        )
        .expect_err("relative model-list pointer rejected");
        assert!(
            err.to_string()
                .contains("models_response.data_json_pointer"),
            "{err}"
        );
    }

    #[test]
    fn schema_version_mismatch_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 2, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("unknown schema version rejected");
        assert!(err.to_string().contains("schema_version 2"), "{err}");
    }

    #[test]
    fn stream_true_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": true, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("stream=true rejected");
        assert!(err.to_string().contains("stream=true"), "{err}");
    }

    #[test]
    fn reserved_attribution_header_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "attribution_headers": {"Authorization": "Bearer x"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        // Rejected either by the reserved-header check or the secret scan; both
        // are "configs are data only" guards, so assert a hard error either way.
        let err = result.expect_err("reserved header rejected");
        let msg = err.to_string();
        assert!(
            msg.contains("reserved") || msg.contains("secret") || msg.contains("leaked"),
            "{msg}"
        );
    }

    #[test]
    fn out_of_range_timeout_rejected() {
        let result = parse_and_validate(
            r#"{
                "schema_version": 1, "provider_id": "x", "display_name": "X",
                "driver": "openai_chat_compatible", "base_url": "https://example.com",
                "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
                "defaults": {"model": "m", "stream": false, "temperature": 0.2},
                "structured_output": {"mode": "none"},
                "response": {}, "limits": {"timeout_ms": 100, "max_image_bytes": 4194304, "max_output_tokens": 1200},
                "models": []
            }"#,
            "test".to_string(),
        );
        let err = result.expect_err("tiny timeout rejected");
        assert!(err.to_string().contains("timeout_ms 100"), "{err}");
    }

    // ---- Capability-registry tests ----

    #[test]
    fn built_ins_advertise_understanding_and_rating_descriptors() {
        let registry = builtin_registry();
        let caps = build_provider_capability_descriptors(&registry);
        // 9 advertised provider/model pairs (OpenRouter, Volcengine Ark,
        // Volcengine Ark Coding Plan, OpenCode Go Anthropic, and 2 OpenCode Go
        // OpenAI models, plus the 2 CC Switch routing presets and OpenAI Codex
        // OAuth), each emitting 2 descriptors (understanding + rating) = 18.
        assert_eq!(caps.len(), 18);

        let understanding: Vec<_> = caps
            .iter()
            .filter(|c| c.task_id == "image_understanding.describe")
            .collect();
        let rating: Vec<_> = caps
            .iter()
            .filter(|c| c.task_id == "image_rating.score")
            .collect();
        assert_eq!(understanding.len(), 9);
        assert_eq!(rating.len(), 9);
        assert!(
            caps.iter()
                .any(|c| c.provider_id == "opencode_go_anthropic"),
            "opencode_go_anthropic should advertise its documented model candidate"
        );
        assert!(
            caps.iter().any(|c| c.provider_id == "opencode_go_openai"),
            "opencode_go_openai should advertise its documented model candidate"
        );
        for id in ["ccswitch_anthropic", "ccswitch_openai"] {
            let cap = understanding
                .iter()
                .copied()
                .find(|c| c.provider_id == id)
                .unwrap_or_else(|| panic!("{id} understanding descriptor"));
            assert_eq!(cap.model_id, "ccswitch-routed");
            assert!(!cap.requires_credential);
        }

        let or_understanding = understanding
            .iter()
            .copied()
            .find(|c| c.provider_id == "openrouter")
            .expect("openrouter understanding descriptor");
        assert_eq!(or_understanding.model_id, "qwen/qwen3.7-plus");
        assert!(or_understanding.requires_credential);
        assert!(!or_understanding.supports_batch);
        assert!(or_understanding.supports_cancel);
        assert_eq!(or_understanding.max_payload_bytes, 4194304);
        assert!(
            or_understanding
                .input_kinds
                .contains(&(AiInputKind::AiInputPreview as i32))
        );
        assert!(
            or_understanding
                .output_kinds
                .contains(&(AiOutputKind::AiOutputCaption as i32))
        );
        assert!(
            or_understanding
                .output_kinds
                .contains(&(AiOutputKind::AiOutputTags as i32))
        );

        let ark_rating = rating
            .iter()
            .copied()
            .find(|c| c.provider_id == "volcengine_ark")
            .expect("volcengine rating descriptor");
        assert_eq!(ark_rating.model_id, "doubao-seed-2-0-lite-260428");
        assert!(ark_rating.requires_credential);
        assert_eq!(
            ark_rating.output_kinds,
            vec![AiOutputKind::AiOutputScore as i32]
        );
    }

    #[test]
    fn non_vision_or_non_structured_models_not_advertised() {
        let dir = tempdir();
        // supports_vision=true but supports_structured_output=false -> not advertised.
        let user = r#"{
            "schema_version": 1, "provider_id": "vision_only_noschema", "display_name": "V",
            "driver": "openai_chat_compatible", "base_url": "https://localhost:11434",
            "endpoint": "/v1/chat/completions", "auth": {"type": "none", "credential_slot": "x_slot"},
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "m", "display_name": "m", "supports_vision": true, "supports_structured_output": false}
            ]
        }"#;
        write_config(&dir, "vision_only.json", user);
        let registry = load_provider_configs(Some(dir.path())).expect("loads");
        let caps = build_provider_capability_descriptors(&registry);
        assert!(
            caps.iter().all(|c| c.provider_id != "vision_only_noschema"),
            "non-structured-output model is not advertised"
        );
    }

    // ---- Phase 6a: compatible protocol preset tests ----

    #[test]
    fn opencode_presets_are_https_with_no_raw_secret() {
        // The two Opencode Go presets must load (which runs the raw-secret scan
        // before deserialize on the built-in path) and must be HTTPS-only.
        let registry = builtin_registry();
        for id in ["opencode_go_anthropic", "opencode_go_openai"] {
            let cfg = registry.get(id).unwrap_or_else(|| panic!("{id} present"));
            assert!(
                cfg.base_url.starts_with("https://"),
                "{id} base_url must be https:// (got {})",
                cfg.base_url
            );
            // credential_slot is a slot label, not a secret; it must survive load
            // (the scan must not reject the label "opencode_api_key").
            assert_eq!(cfg.auth.credential_slot, "opencode_api_key");
        }
    }

    #[test]
    fn opencode_preset_with_injected_raw_secret_is_rejected() {
        // An Opencode-shaped preset carrying a raw API key in an unknown field
        // must be rejected by the raw-secret scan (configs are data only).
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1,
            "provider_id": "opencode_go_anthropic",
            "display_name": "Opencode Go (Anthropic-compatible)",
            "driver": "anthropic_messages",
            "base_url": "https://opencode.ai/zen/go/v1",
            "endpoint": "/messages",
            "auth": {"type": "api_key_header", "credential_slot": "opencode_api_key"},
            "api_key": "sk-1234567890abcdef1234567890abcdef",
            "defaults": {"model": "qwen3.7-plus", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "tool", "strict": true},
            "response": {"content_json_pointer": null},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "qwen3.7-plus", "display_name": "Qwen3.7 Plus", "supports_vision": true, "supports_structured_output": true, "live_confirmed": false}
            ]
        }"#;
        write_config(&dir, "leaky_opencode.json", user);
        let registry = load_provider_configs(Some(dir.path()))
            .expect("load does not hard-fail on a bad user config");
        // The user config is skipped (fail closed); the built-in Opencode preset
        // it tried to override remains, but the leaky override is NOT applied.
        assert!(
            registry.get("opencode_go_anthropic").is_some(),
            "built-in remains"
        );
    }

    #[test]
    fn opencode_duplicate_provider_id_is_rejected() {
        // Two user configs sharing the Opencode preset's provider_id ("configured
        // endpoint id"): the first user override wins, the second is skipped as a
        // user-on-user duplicate (fail closed, not a hard error, not a silent
        // clobber). Deterministic order: a_first sorts before b_second.
        let dir = tempdir();
        let first = r#"{
            "schema_version": 1,
            "provider_id": "opencode_go_anthropic",
            "display_name": "Opencode (first user override)",
            "driver": "anthropic_messages",
            "base_url": "https://opencode.ai/zen/go/v1",
            "endpoint": "/messages",
            "auth": {"type": "api_key_header", "credential_slot": "opencode_api_key"},
            "defaults": {"model": "first/model", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "tool", "strict": true},
            "response": {"content_json_pointer": null},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "first/model", "display_name": "first", "supports_vision": false, "supports_structured_output": false, "live_confirmed": false}
            ]
        }"#;
        let second = r#"{
            "schema_version": 1,
            "provider_id": "opencode_go_anthropic",
            "display_name": "Opencode (second user override)",
            "driver": "anthropic_messages",
            "base_url": "https://opencode.ai/zen/go/v1",
            "endpoint": "/messages",
            "auth": {"type": "api_key_header", "credential_slot": "opencode_api_key"},
            "defaults": {"model": "second/model", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "tool", "strict": true},
            "response": {"content_json_pointer": null},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "second/model", "display_name": "second", "supports_vision": false, "supports_structured_output": false, "live_confirmed": false}
            ]
        }"#;
        write_config(&dir, "a_first.json", first);
        write_config(&dir, "b_second.json", second);
        let registry =
            load_provider_configs(Some(dir.path())).expect("load does not hard-fail on dupe");
        let oc = registry
            .get("opencode_go_anthropic")
            .expect("opencode_go_anthropic present");
        assert_eq!(oc.display_name, "Opencode (first user override)");
        assert_eq!(oc.defaults.model, "first/model");
    }

    #[test]
    fn opencode_presets_advertise_documented_models() {
        // The shipped OpenCode presets advertise documented model candidates.
        // Explicit live confirmation remains accepted for user overrides.
        let registry = builtin_registry();
        let caps = build_provider_capability_descriptors(&registry);
        assert!(
            caps.iter()
                .any(|c| c.provider_id == "opencode_go_anthropic"),
            "opencode_go_anthropic should be advertised"
        );

        let dir = tempdir();
        let pinned = r#"{
            "schema_version": 1,
            "provider_id": "opencode_go_anthropic",
            "display_name": "Opencode Go (Anthropic-compatible, live-confirmed)",
            "driver": "anthropic_messages",
            "base_url": "https://opencode.ai/zen/go/v1",
            "endpoint": "/messages",
            "auth": {"type": "api_key_header", "credential_slot": "opencode_api_key"},
            "defaults": {"model": "qwen3.7-plus", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "tool", "strict": true},
            "response": {"content_json_pointer": null},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "qwen3.7-plus", "display_name": "Qwen3.7 Plus", "supports_vision": true, "supports_structured_output": true, "live_confirmed": true}
            ]
        }"#;
        write_config(&dir, "pinned.json", pinned);
        let registry = load_provider_configs(Some(dir.path())).expect("pinned override loads");
        let caps = build_provider_capability_descriptors(&registry);
        let oc_caps: Vec<_> = caps
            .iter()
            .filter(|c| c.provider_id == "opencode_go_anthropic")
            .collect();
        assert_eq!(
            oc_caps.len(),
            2,
            "live-confirmed opencode model advertises understanding + rating"
        );
        assert!(
            oc_caps
                .iter()
                .any(|c| c.task_id == "image_understanding.describe")
        );
        assert!(oc_caps.iter().any(|c| c.task_id == "image_rating.score"));
    }

    #[test]
    fn live_confirmed_alone_advertises_without_vision_or_structured_flags() {
        // Directly exercises the live-pin disjunct of the advertisement gate:
        // supports_vision=false && supports_structured_output=false but
        // live_confirmed=true -> advertised.
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1, "provider_id": "live_pinned", "display_name": "L",
            "driver": "anthropic_messages", "base_url": "https://example.com",
            "endpoint": "/messages", "auth": {"type": "bearer", "credential_slot": "x_key"},
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "tool"},
            "response": {"content_json_pointer": null},
            "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "m", "display_name": "m", "supports_vision": false, "supports_structured_output": false, "live_confirmed": true}
            ]
        }"#;
        write_config(&dir, "pinned.json", user);
        let registry = load_provider_configs(Some(dir.path())).expect("loads");
        let caps = build_provider_capability_descriptors(&registry);
        let pinned: Vec<_> = caps
            .iter()
            .filter(|c| c.provider_id == "live_pinned")
            .collect();
        assert_eq!(
            pinned.len(),
            2,
            "live_confirmed model is advertised (understanding + rating)"
        );
    }

    #[test]
    fn live_confirmed_unwired_driver_is_not_advertised() {
        // Phase 6b fail-closed: a user config that pins live_confirmed=true on a
        // reserved-but-unimplemented driver id must NOT be advertised. Without
        // the wired-driver check, ListCapabilities would surface this as a usable
        // capability and the call would then fail with UNSUPPORTED_TASK.
        let dir = tempdir();
        let user = r#"{
            "schema_version": 1, "provider_id": "reserved_live_pinned", "display_name": "R",
            "driver": "gemini_generate_content", "base_url": "https://example.com",
            "endpoint": "/x", "auth": {"type": "bearer", "credential_slot": "x_key"},
            "defaults": {"model": "m", "stream": false, "temperature": 0.2},
            "structured_output": {"mode": "none"},
            "response": {}, "limits": {"timeout_ms": 60000, "max_image_bytes": 4194304, "max_output_tokens": 1200},
            "models": [
                {"slug": "m", "display_name": "m", "supports_vision": false, "supports_structured_output": false, "live_confirmed": true}
            ]
        }"#;
        write_config(&dir, "reserved.json", user);
        let registry = load_provider_configs(Some(dir.path())).expect("loads");
        let caps = build_provider_capability_descriptors(&registry);
        assert!(
            caps.iter().all(|c| c.provider_id != "reserved_live_pinned"),
            "live-confirmed model on an unwired driver must not be advertised"
        );

        // Sanity: the same config on a WIRED driver IS advertised (controls for
        // the live_confirmed pin itself being well-formed).
        let dir2 = tempdir();
        let wired = user.replace("gemini_generate_content", "anthropic_messages");
        write_config(&dir2, "wired.json", &wired);
        let registry2 = load_provider_configs(Some(dir2.path())).expect("loads");
        let caps2 = build_provider_capability_descriptors(&registry2);
        let pinned: Vec<_> = caps2
            .iter()
            .filter(|c| c.provider_id == "reserved_live_pinned")
            .collect();
        assert_eq!(
            pinned.len(),
            2,
            "wired driver + live_confirmed is advertised"
        );
    }

    // ---- tempdir helpers (avoid pulling in tempfile) ----

    struct TempDir(std::path::PathBuf);
    impl TempDir {
        fn path(&self) -> &std::path::Path {
            &self.0
        }
    }
    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn tempdir() -> TempDir {
        // Use the test thread id via OUT_DIR-style unique path under std::env::temp_dir().
        let mut name = std::env::temp_dir();
        // Include a process-unique-ish suffix; tests run in parallel, so combine
        // counter + thread name.
        static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let n = COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        name.push(format!(
            "alcedo_provider_cfg_test_{}_{}",
            std::process::id(),
            n
        ));
        std::fs::create_dir_all(&name).expect("create temp dir");
        TempDir(name)
    }

    fn write_config(dir: &TempDir, file: &str, contents: &str) {
        let path = dir.path().join(file);
        let mut f = std::fs::File::create(&path).expect("create config file");
        f.write_all(contents.as_bytes()).expect("write config file");
    }
}
