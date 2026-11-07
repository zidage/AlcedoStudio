//! Generic OpenAI Chat-compatible image-analysis driver (Phase 6b,
//! `openai_chat_compatible`).
//!
//! This is the protocol-family driver for any server that speaks the OpenAI
//! Chat Completions shape (`POST /chat/completions`): Opencode's OpenAI-compatible
//! endpoint (`https://opencode.ai/zen/go/v1/chat/completions`), OpenRouter, a
//! local compatible server, or the real OpenAI API. It builds an OpenAI Chat
//! request from the loaded provider config: an Alcedo-owned system prompt, a user
//! message carrying the selected image rendition as a data URI plus a task
//! instruction, and an OpenAI Chat function tool (`tools[].function.name` plus
//! `tool_choice.function.name`) carrying the code-owned (sanitized to
//! strict-compatible) Alcedo schema. The response's tool-call arguments, or
//! `choices[0].message.content` for compatible shims that still answer as text,
//! are parsed as JSON, validated + normalized against the code-owned
//! understanding / rating contract, and returned as typed fields.
//!
//! ## OpenRouter-specific knobs are optional, config-gated, off by default
//!
//! The driver is generic: OpenRouter's routing/attribution knobs are read from
//! the provider config and only emitted when the config sets them. The OpenRouter
//! built-in config sets `attribution_headers`, `structured_output.provider_require_parameters`,
//! and a per-model `data_collection = "deny"`; the Opencode built-in config sets
//! none of those, so for an Opencode preset the `provider` routing object is
//! omitted entirely and no attribution headers are sent — "disabled for Opencode by
//! default" is automatic from the config, not a code branch. Phase 6g may retire
//! the `openrouter_chat` driver id once this generic driver covers the same
//! contract; until then `OpenRouterChatProvider` is a type alias for this type.
//!
//! ## Auth, secrets, structured output
//!
//! The API key is resolved from the Rust credential vault per request and sent
//! only as `Authorization: Bearer <secret>`; `SecretString::expose()` is called at
//! the single header-build site and is never logged. `auth: none` is supported for
//! a local no-key compatible server. Structured output is mandatory: if the config
//! does not request it (`mode = "none"`) or the selected model does not support
//! it, the driver fails closed before any HTTP call. A compatible endpoint that
//! ignores the requested structured tool output and returns non-JSON prose, or
//! returns JSON that violates the code-owned contract, maps to
//! `ProviderError::SchemaValidation` so
//! the service creates no active annotation (Phase 6b "unsupported structured
//! output" fail-closed path). Transport / 429 / 5xx failures retry under the same
//! bounded policy as the other drivers; provider and transport errors map to
//! `ProviderError` variants the service translates into `AiResponseStatus` /
//! `AiErrorCode`.

use serde_json::{Value, json};
use std::sync::Mutex;
use tracing::warn;

use crate::service::credential_vault::SecretString;
use crate::service::image_analysis::{
    AnalyzeImageInput, AnalyzeOutcome, BatchAnalyzeOutcome, DescribeOutcome, DiscoveredModel,
    ImageAnalysisProvider, ImageAnalysisSchemaSpec, ProviderError, ScoreOutcome, Usage,
    image_analysis_schema_json, output_confidence, output_description, output_rating_reason,
    output_rubric_id, output_rubric_version, output_scene, output_tags, validate_analyze,
    validate_batch_analyze, validate_rating, validate_understanding,
};
use crate::service::provider_config::{ModelConfig, ProviderConfig};
use crate::service::providers::http_util::{
    MAX_TRANSIENT_RETRIES, build_image_data_uri, build_rustls_client, compact_json_excerpt,
    compact_text_excerpt, extract_provider_request_id, extract_usage, json_pointer_str,
    parse_content_json, parse_discovered_models, parse_rating_int, read_response,
    sanitized_provider_json_excerpt, send_get_with_retry, send_with_retry, strict_schema_value,
};

/// Schema/function names injected into the OpenAI-compatible structured-output
/// request. Kept stable and Alcedo-namespaced so a provider's structured-output
/// dashboard identifies the contract unambiguously.
const UNDERSTANDING_SCHEMA_NAME: &str = "alcedo_image_understanding";
const RATING_SCHEMA_NAME: &str = "alcedo_image_rating";
const ANALYSIS_FLAT_SCHEMA_NAME: &str = "alcedo_image_analysis_flat";
const ANALYSIS_BATCH_SCHEMA_NAME: &str = "alcedo_image_analysis_batch";
const SCHEMA_REPAIR_RETRIES: u32 = 1;

#[derive(Debug, Clone)]
struct BatchItemRepair {
    index: usize,
    bad_json: String,
    error: ProviderError,
}

struct BatchRepairContext<'a> {
    previous_content: &'a str,
    failures: &'a [BatchItemRepair],
}

/// Read `choices[0].finish_reason` from an OpenAI-compatible response. Used to
/// distinguish a reasoning-token-starved response (`finish_reason = "length"`
/// with null content) from a genuine schema mismatch, so the driver can recover
/// by escalating `max_tokens` instead of burning a schema-repair retry the model
/// will repeat.
fn finish_reason_of(body: &Value) -> Option<&str> {
    body.pointer("/choices/0/finish_reason")
        .and_then(|v| v.as_str())
}

/// A short `(finish_reason: <reason>)` suffix appended to null-content
/// diagnostic messages so a non-length null (e.g. `content_filter`, or a
/// provider that returns null with `finish_reason = "stop"`) is identifiable
/// in the surfaced error. Empty when no `finish_reason` is reported.
fn finish_reason_suffix(body: &Value) -> String {
    match finish_reason_of(body) {
        Some(fr) => format!(" (finish_reason: {fr})"),
        None => String::new(),
    }
}

pub struct OpenAiChatCompatibleProvider {
    config: ProviderConfig,
    http: reqwest::Client,
    /// Models committed from live `list_models` discovery so an explicitly
    /// requested discovered slug passes the Phase 6c `resolve_model` gate. Each
    /// entry is synthesized with `supports_structured_output = true` (discovery
    /// cannot prove it, but the gate requires it for the call to proceed; a model
    /// that truly lacks support fails at the provider HTTP call as a normal
    /// per-item error). Idempotent: a slug already in `config.models` or already
    /// committed is not duplicated.
    discovered_models: Mutex<Vec<ModelConfig>>,
}

impl OpenAiChatCompatibleProvider {
    /// Construct from a validated provider config, building a rustls-backed
    /// `reqwest::Client`. Used by `main.rs` for the shipped sidecar.
    pub fn new(config: ProviderConfig) -> Result<Self, ProviderError> {
        let http = build_rustls_client()?;
        Ok(Self {
            config,
            http,
            discovered_models: Mutex::new(Vec::new()),
        })
    }

    /// Construct with an injected HTTP client. Used by tests to point the driver
    /// at a local mock server; the rustls client built by `new` already speaks
    /// `http://127.0.0.1` so tests can also use `new` with a localhost base_url.
    #[allow(dead_code)]
    pub fn with_client(config: ProviderConfig, http: reqwest::Client) -> Self {
        Self {
            config,
            http,
            discovered_models: Mutex::new(Vec::new()),
        }
    }

    fn url(&self) -> String {
        format!("{}{}", self.config.base_url, self.config.endpoint)
    }

    /// Phase 6c: the model-listing (discovery) URL. Defaults to
    /// `{base_url}/models` (the OpenAI-compatible default); a config
    /// `models_endpoint` override replaces the path.
    fn models_url(&self) -> String {
        let path = self.config.models_endpoint.as_deref().unwrap_or("/models");
        format!("{}{}", self.config.base_url, path)
    }

    /// Resolve the requested model slug (falling back to the config default) and
    /// the matching model entry (for per-model knobs) if present. Phase 6c: a
    /// non-empty explicit `requested` must resolve to an entry in
    /// `config.models[]` (built-in) or in the committed `discovered_models`
    /// (from a prior `list_models`); an unknown slug fails closed here, BEFORE
    /// any provider HTTP call, closing the Phase 6b review gap where an unlisted
    /// slug bypassed the `supports_structured_output` check. An empty `requested`
    /// uses the config default (config-authored, so it is "known" by virtue of
    /// being the default) and may yield `None` when the default is not itself
    /// listed. Returns an owned `ModelConfig` clone because committed discovered
    /// models live behind a `Mutex` whose guard cannot outlive the call.
    fn resolve_model(
        &self,
        requested: &str,
    ) -> Result<(String, Option<ModelConfig>), ProviderError> {
        if requested.trim().is_empty() {
            let slug = self.config.defaults.model.clone();
            let entry = self.config.models.iter().find(|m| m.slug == slug).cloned();
            return Ok((slug, entry));
        }
        if let Some(m) = self.config.models.iter().find(|m| m.slug == requested) {
            return Ok((m.slug.clone(), Some(m.clone())));
        }
        let committed = self
            .discovered_models
            .lock()
            .expect("discovered_models mutex poisoned");
        if let Some(m) = committed.iter().find(|m| m.slug == requested) {
            return Ok((m.slug.clone(), Some(m.clone())));
        }
        Err(ProviderError::UnknownModel(requested.to_string()))
    }

    /// Commit discovered models so a later explicit `model_id` resolves. Each
    /// discovered id not already present in the static `config.models` or the
    /// committed list is appended as a synthesized `ModelConfig` with
    /// `supports_structured_output = true` (required by `ensure_structured_output`
    /// for the call to proceed). Idempotent. Discovery cannot prove vision or
    /// structured-output support, so a model lacking support fails at the
    /// provider HTTP call as a normal per-item error rather than being silently
    /// dropped.
    fn commit_discovered_models(&self, models: &[DiscoveredModel]) {
        let mut committed = self
            .discovered_models
            .lock()
            .expect("discovered_models mutex poisoned");
        for m in models {
            let already_known = self.config.models.iter().any(|c| c.slug == m.model_id)
                || committed.iter().any(|c| c.slug == m.model_id);
            if already_known {
                continue;
            }
            committed.push(ModelConfig {
                slug: m.model_id.clone(),
                display_name: m.display_name.clone(),
                supports_vision: true,
                supports_structured_output: true,
                live_confirmed: false,
                max_image_bytes: Some(self.config.limits.max_image_bytes),
                recommended_rendition: None,
                cost_per_million_input_usd: None,
                cost_per_million_output_usd: None,
                data_collection: None,
            });
        }
    }

    /// Fail closed if structured output is not requested or the selected model
    /// does not support it (the plan: do not rely on best-effort free-form JSON).
    fn ensure_structured_output(&self, model: Option<&ModelConfig>) -> Result<(), ProviderError> {
        if self.config.structured_output.mode == "none" {
            return Err(ProviderError::Provider(
                "provider config does not request structured output; failing closed".to_string(),
            ));
        }
        if let Some(m) = model {
            if !m.supports_structured_output {
                return Err(ProviderError::Provider(
                    "selected model does not support structured output; failing closed".to_string(),
                ));
            }
        }
        Ok(())
    }

    /// Resolve the bearer token from the credential, or `None` for `auth: none`.
    /// The secret is `expose()`d only here, at the single call site that builds the
    /// `Authorization` header, and is never logged.
    fn bearer<'a>(
        &self,
        credential: Option<&'a SecretString>,
    ) -> Result<Option<&'a str>, ProviderError> {
        match (self.config.auth.auth_type.as_str(), credential) {
            ("none", _) => Ok(None),
            ("bearer", Some(s)) => Ok(Some(s.expose())),
            ("bearer", None) => Err(ProviderError::Provider(
                "bearer provider called without a credential".to_string(),
            )),
            (other, _) => Err(ProviderError::Provider(format!(
                "unsupported auth type {other}"
            ))),
        }
    }

    fn attribution_headers(&self) -> Vec<(String, String)> {
        self.config
            .attribution_headers
            .iter()
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    }

    /// Build the optional `provider` routing object: `require_parameters` when the
    /// config asks for it, plus `data_collection = "deny"` when the selected model
    /// declares the privacy-first knob. Both are OpenRouter-style knobs that an
    /// Opencode / plain OpenAI-compatible config does not set, so the object is
    /// empty and omitted for those presets (disabled by default).
    fn provider_knobs(&self, model: Option<&ModelConfig>) -> Value {
        let mut m = serde_json::Map::new();
        if self.config.structured_output.provider_require_parameters {
            m.insert("require_parameters".into(), Value::Bool(true));
        }
        if let Some(md) = model {
            if md.data_collection.as_deref() == Some("deny") {
                m.insert("data_collection".into(), Value::String("deny".into()));
            }
        }
        Value::Object(m)
    }

    fn build_chat_body(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uri: &str,
        schema: Value,
        schema_name: &str,
        system: &str,
        instruction: &str,
        repair: Option<(&str, &ProviderError)>,
    ) -> Value {
        let user_content = json!([
            { "type": "text", "text": instruction },
            { "type": "image_url", "image_url": { "url": data_uri } }
        ]);
        let messages = if let Some((previous_content, error)) = repair {
            json!([
                { "role": "system", "content": system },
                { "role": "user", "content": user_content },
                { "role": "assistant", "content": previous_content },
                { "role": "user", "content": schema_repair_instruction(schema_name, error) }
            ])
        } else {
            json!([
                { "role": "system", "content": system },
                { "role": "user", "content": user_content }
            ])
        };
        // `max_tokens` is intentionally omitted: reasoning models (e.g. kimi-k2.6)
        // consume reasoning tokens out of the same budget as the visible content,
        // so a server-side cap starves the content (content=null +
        // finish_reason="length"). Letting the provider/model generate as much as
        // it needs — bounded only by the model's own max output — avoids our side
        // throttling the generation. A length-starved response under no cap means
        // the model's own output window was reached; see
        // [`Self::model_output_budget_exhausted`].
        let mut body = json!({
            "model": slug,
            "messages": messages,
            "stream": false,
            "temperature": self.config.defaults.temperature
        });
        self.attach_structured_output(&mut body, schema_name, schema);
        let provider = self.provider_knobs(model);
        // Omit `provider` entirely when empty so a plain OpenAI-compatible /
        // Opencode body matches the OpenAI Chat shape (no OpenRouter routing
        // object). The OpenRouter config sets the knobs; others do not.
        let is_empty = provider.as_object().map(|o| o.is_empty()).unwrap_or(false);
        if !is_empty {
            body["provider"] = provider;
        }
        body
    }

    fn build_batch_chat_body(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uris: &[String],
        schema: Value,
        system: &str,
        instruction: &str,
        repair: Option<BatchRepairContext<'_>>,
    ) -> Value {
        let mut user_content = vec![json!({ "type": "text", "text": instruction })];
        for (index, data_uri) in data_uris.iter().enumerate() {
            user_content.push(json!({ "type": "text", "text": format!("Image index {index}:") }));
            user_content.push(json!({ "type": "image_url", "image_url": { "url": data_uri } }));
        }
        let user_content = Value::Array(user_content);
        let messages = if let Some(repair) = repair {
            json!([
                { "role": "system", "content": system },
                { "role": "user", "content": user_content },
                { "role": "assistant", "content": repair.previous_content },
                { "role": "user", "content": batch_schema_repair_instruction(repair.failures) }
            ])
        } else {
            json!([
                { "role": "system", "content": system },
                { "role": "user", "content": user_content }
            ])
        };
        // `max_tokens` intentionally omitted (see `build_chat_body`): reasoning
        // models burn the budget on reasoning and return content=null +
        // finish_reason="length"; a batch amplifies this. Let the model generate
        // what it needs, bounded by its own max output.
        let mut body = json!({
            "model": slug,
            "messages": messages,
            "stream": false,
            "temperature": self.config.defaults.temperature
        });
        self.attach_structured_output(&mut body, ANALYSIS_BATCH_SCHEMA_NAME, schema);
        let provider = self.provider_knobs(model);
        let is_empty = provider.as_object().map(|o| o.is_empty()).unwrap_or(false);
        if !is_empty {
            body["provider"] = provider;
        }
        body
    }

    fn attach_structured_output(&self, body: &mut Value, schema_name: &str, schema: Value) {
        body["tools"] = json!([
            {
                "type": "function",
                "function": {
                    "name": schema_name,
                    "description": "Return the Alcedo image-analysis result using this exact JSON schema.",
                    "parameters": schema,
                    "strict": self.config.structured_output.strict
                }
            }
        ]);
        body["tool_choice"] = json!({
            "type": "function",
            "function": { "name": schema_name }
        });
        body["parallel_tool_calls"] = Value::Bool(false);
    }

    async fn send_chat_body(
        &self,
        slug: &str,
        body: &Value,
        bearer: Option<&str>,
    ) -> Result<reqwest::Response, ProviderError> {
        send_with_retry(
            &self.http,
            &self.url(),
            body,
            &self.attribution_headers(),
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await
        .map_err(|err| {
            if let ProviderError::Provider(message) = &err {
                let lower = message.to_ascii_lowercase();
                if lower.contains("name is not set") || lower.contains("`name` is not set") {
                    return ProviderError::Provider(format!(
                        "{message}; OpenAI-compatible structured output request included function/tool name for model {slug}"
                    ));
                }
            }
            err
        })
    }

    async fn send_and_parse_describe(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uri: &str,
        schema: Value,
        system: &str,
        instruction: &str,
        bearer: Option<&str>,
    ) -> Result<(DescribeOutcome, String, u32), ProviderError> {
        let mut attempt = 0u32;
        let mut repair: Option<(String, ProviderError)> = None;
        loop {
            let body = self.build_chat_body(
                slug,
                model,
                data_uri,
                schema.clone(),
                UNDERSTANDING_SCHEMA_NAME,
                system,
                instruction,
                repair
                    .as_ref()
                    .map(|(content, err)| (content.as_str(), err)),
            );
            let resp = self.send_chat_body(slug, &body, bearer).await?;
            let (headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = self.response_content_excerpt(&resp_body);
            // The driver sends no `max_tokens`, so a length-starved response means
            // the model's own output window was reached — surface a clear error
            // instead of burning a schema-repair retry the model will repeat.
            if let Some(err) = self.model_output_budget_exhausted(&resp_body) {
                return Err(err);
            }
            match self.parse_describe(&resp_body, slug, &header_req_id) {
                Ok(outcome) => break Ok((outcome, provider_content, attempt)),
                Err(err) => {
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        provider_request_id = %header_req_id,
                        provider_content = %provider_content,
                        error = %err,
                        schema_repair_attempt = attempt,
                        "DescribeImage provider response parse failed"
                    );
                    if attempt >= SCHEMA_REPAIR_RETRIES {
                        return Err(err);
                    }
                    attempt += 1;
                    repair = Some((provider_content, err));
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        schema_repair_attempt = attempt,
                        "DescribeImage retrying after schema validation failure"
                    );
                }
            }
        }
    }

    async fn send_and_parse_score(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uri: &str,
        schema: Value,
        system: &str,
        instruction: &str,
        bearer: Option<&str>,
    ) -> Result<(ScoreOutcome, String, u32), ProviderError> {
        let mut attempt = 0u32;
        let mut repair: Option<(String, ProviderError)> = None;
        loop {
            let body = self.build_chat_body(
                slug,
                model,
                data_uri,
                schema.clone(),
                RATING_SCHEMA_NAME,
                system,
                instruction,
                repair
                    .as_ref()
                    .map(|(content, err)| (content.as_str(), err)),
            );
            let resp = self.send_chat_body(slug, &body, bearer).await?;
            let (headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = self.response_content_excerpt(&resp_body);
            if let Some(err) = self.model_output_budget_exhausted(&resp_body) {
                return Err(err);
            }
            match self.parse_score(&resp_body, slug, &header_req_id) {
                Ok(outcome) => break Ok((outcome, provider_content, attempt)),
                Err(err) => {
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        provider_request_id = %header_req_id,
                        provider_content = %provider_content,
                        error = %err,
                        schema_repair_attempt = attempt,
                        "ScoreImage provider response parse failed"
                    );
                    if attempt >= SCHEMA_REPAIR_RETRIES {
                        return Err(err);
                    }
                    attempt += 1;
                    repair = Some((provider_content, err));
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        schema_repair_attempt = attempt,
                        "ScoreImage retrying after schema validation failure"
                    );
                }
            }
        }
    }

    async fn send_and_parse_analyze(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uri: &str,
        schema: Value,
        system: &str,
        instruction: &str,
        bearer: Option<&str>,
    ) -> Result<(AnalyzeOutcome, String, u32), ProviderError> {
        let mut attempt = 0u32;
        let mut repair: Option<(String, ProviderError)> = None;
        loop {
            let body = self.build_chat_body(
                slug,
                model,
                data_uri,
                schema.clone(),
                ANALYSIS_FLAT_SCHEMA_NAME,
                system,
                instruction,
                repair
                    .as_ref()
                    .map(|(content, err)| (content.as_str(), err)),
            );
            let resp = self.send_chat_body(slug, &body, bearer).await?;
            let (headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = self.response_content_excerpt(&resp_body);
            if let Some(err) = self.model_output_budget_exhausted(&resp_body) {
                return Err(err);
            }
            match self.parse_analyze(&resp_body, slug, &header_req_id) {
                Ok(outcome) => break Ok((outcome, provider_content, attempt)),
                Err(err) => {
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        provider_request_id = %header_req_id,
                        provider_content = %provider_content,
                        error = %err,
                        schema_repair_attempt = attempt,
                        "AnalyzeImage provider response parse failed"
                    );
                    if attempt >= SCHEMA_REPAIR_RETRIES {
                        return Err(err);
                    }
                    attempt += 1;
                    repair = Some((provider_content, err));
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        schema_repair_attempt = attempt,
                        "AnalyzeImage retrying after schema validation failure"
                    );
                }
            }
        }
    }

    async fn send_and_parse_batch_analyze(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        data_uris: &[String],
        schema: Value,
        system: &str,
        instruction: &str,
        bearer: Option<&str>,
    ) -> Result<(BatchAnalyzeOutcome, String, u32), ProviderError> {
        let mut attempt = 0u32;
        let mut merged: Vec<Option<AnalyzeOutcome>> = vec![None; data_uris.len()];
        let mut required_indices: Vec<usize> = (0..data_uris.len()).collect();
        let mut repair_content: Option<String> = None;
        let mut repair_failures: Vec<BatchItemRepair> = Vec::new();
        loop {
            let repair = repair_content
                .as_deref()
                .map(|previous_content| BatchRepairContext {
                    previous_content,
                    failures: &repair_failures,
                });
            let body = self.build_batch_chat_body(
                slug,
                model,
                data_uris,
                schema.clone(),
                system,
                instruction,
                repair,
            );
            let resp = self.send_chat_body(slug, &body, bearer).await?;
            let (headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = self.response_content_excerpt(&resp_body);
            // The driver sends no `max_tokens`, so a length-starved response
            // (content=null + finish_reason="length" + no tool-call arguments,
            // making every required index "omitted") means the model's own output
            // window was reached. Surface a clear error rather than burning a
            // schema-repair retry the model will repeat.
            if let Some(err) = self.model_output_budget_exhausted(&resp_body) {
                return Err(err);
            }
            match self.parse_batch_analyze_items(
                &resp_body,
                slug,
                &header_req_id,
                data_uris.len(),
                &required_indices,
            ) {
                Ok(items) => {
                    for (index, outcome) in items {
                        if index < merged.len() {
                            merged[index] = Some(outcome);
                        }
                    }
                    let missing: Vec<_> = merged
                        .iter()
                        .enumerate()
                        .filter_map(|(index, item)| item.is_none().then_some(index))
                        .collect();
                    if missing.is_empty() {
                        let items = merged
                            .into_iter()
                            .map(|item| item.expect("checked no missing batch item"))
                            .collect::<Vec<_>>();
                        let mut usage = Usage::default();
                        let mut provider_request_id = String::new();
                        for item in &items {
                            usage.input_tokens += item.usage.input_tokens;
                            usage.output_tokens += item.usage.output_tokens;
                            usage.total_tokens += item.usage.total_tokens;
                            if provider_request_id.is_empty() {
                                provider_request_id = item.provider_request_id.clone();
                            }
                        }
                        let out = BatchAnalyzeOutcome {
                            items,
                            model_id: slug.to_string(),
                            usage,
                            provider_request_id,
                        };
                        validate_batch_analyze(&out)?;
                        break Ok((out, provider_content, attempt));
                    }
                    repair_failures = missing
                        .into_iter()
                        .map(|index| BatchItemRepair {
                            index,
                            bad_json: "null".to_string(),
                            error: ProviderError::SchemaValidationMessage(format!(
                                "batch response omitted required result index {index}"
                            )),
                        })
                        .collect();
                }
                Err((items, failures)) => {
                    for (index, outcome) in items {
                        if index < merged.len() {
                            merged[index] = Some(outcome);
                        }
                    }
                    repair_failures = failures
                        .into_iter()
                        .filter(|failure| failure.index < data_uris.len())
                        .collect();
                }
            }
            warn!(
                provider = %self.config.provider_id,
                model = %slug,
                provider_content = %provider_content,
                failed_indices = ?repair_failures.iter().map(|f| f.index).collect::<Vec<_>>(),
                schema_repair_attempt = attempt,
                "BatchAnalyzeImage provider response parse failed"
            );
            if attempt >= SCHEMA_REPAIR_RETRIES || repair_failures.is_empty() {
                return Err(repair_failures
                    .first()
                    .map(|f| f.error.clone())
                    .unwrap_or(ProviderError::SchemaValidation));
            }
            attempt += 1;
            required_indices = repair_failures.iter().map(|f| f.index).collect();
            repair_content = Some(provider_content);
            warn!(
                provider = %self.config.provider_id,
                model = %slug,
                schema_repair_attempt = attempt,
                failed_indices = ?required_indices,
                "BatchAnalyzeImage retrying failed items after schema validation failure"
            );
        }
    }

    fn parse_describe(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<DescribeOutcome, ProviderError> {
        let parsed = self.parsed_chat_content(body)?;
        let out = DescribeOutcome {
            caption: output_description(&parsed),
            tags: output_tags(&parsed),
            scene: output_scene(&parsed),
            confidence: output_confidence(&parsed),
            model_id: model_id.to_string(),
            usage: extract_usage(
                self.config
                    .response
                    .usage_json_pointer
                    .as_deref()
                    .and_then(|p| json_pointer_str(body, p)),
            ),
            provider_request_id: header_req_id.to_string(),
        };
        validate_understanding(&out)?;
        Ok(out)
    }

    /// True when the model exhausted its OWN max output budget on reasoning
    /// before producing any content: the configured content pointer resolves to
    /// null/absent or an empty string, there are no `tool_calls` /
    /// `function_call` arguments to fall back to, and `finish_reason == "length"`.
    /// The driver sends no `max_tokens`, so a `"length"` cutoff here is the
    /// provider/model's own output ceiling (not a cap we imposed) — there is
    /// nothing to escalate. A null content with a different `finish_reason` (e.g.
    /// `content_filter`) or with tool-call arguments is NOT this shape and falls
    /// through to the normal parse / schema-repair path.
    fn is_reasoning_length_starved(&self, body: &Value) -> bool {
        let content_pointer = self
            .config
            .response
            .content_json_pointer
            .as_deref()
            .unwrap_or("/choices/0/message/content");
        let content_null = match json_pointer_str(body, content_pointer) {
            None | Some(Value::Null) => true,
            Some(Value::String(s)) => s.trim().is_empty(),
            Some(_) => false,
        };
        if !content_null {
            return false;
        }
        let has_tool_args = body.pointer("/choices/0/message/tool_calls").is_some()
            || body.pointer("/choices/0/message/function_call").is_some();
        !has_tool_args && finish_reason_of(body) == Some("length")
    }

    /// When the provider returns `content: null` + `finish_reason: "length"` with
    /// no tool-call arguments, the model exhausted its own max output budget on
    /// reasoning (the driver sends no `max_tokens`, so this is the provider/model's
    /// ceiling, not a cap we imposed). There is nothing to retry — return a clear,
    /// actionable error so the host knows to use a model with a larger output
    /// window, fewer images per batch, or a non-reasoning model. Returns
    /// `Some(error)` when the response is length-starved, `None` otherwise.
    fn model_output_budget_exhausted(&self, body: &Value) -> Option<ProviderError> {
        if !self.is_reasoning_length_starved(body) {
            return None;
        }
        Some(ProviderError::Provider(
            "model exhausted its own max output budget (finish_reason=length) before producing \
             content; the request sent no max_tokens limit, so this is the provider/model's own \
             output ceiling — use a model with a larger output window, fewer images per batch, \
             or a non-reasoning model"
                .to_string(),
        ))
    }

    fn parsed_chat_content(&self, body: &Value) -> Result<Value, ProviderError> {
        let content_pointer = self
            .config
            .response
            .content_json_pointer
            .as_deref()
            .unwrap_or("/choices/0/message/content");
        if let Some(content) = json_pointer_str(body, content_pointer) {
            return match content {
                Value::String(s) => parse_content_json(s),
                Value::Null => Self::parsed_chat_content_fallback(body).unwrap_or_else(|| {
                    Err(ProviderError::SchemaValidationMessage(format!(
                        "provider response content at pointer {content_pointer} was null and no JSON tool-call arguments were found{}",
                        finish_reason_suffix(body)
                    )))
                }),
                other => Ok(other.clone()),
            };
        }
        Self::parsed_chat_content_fallback(body).unwrap_or_else(|| {
            Err(ProviderError::SchemaValidationMessage(format!(
                "provider response did not contain JSON content at pointer {content_pointer}; expected OpenAI-compatible choices[0].message.content or tool-call arguments{}",
                finish_reason_suffix(body)
            )))
        })
    }

    fn parsed_chat_content_fallback(body: &Value) -> Option<Result<Value, ProviderError>> {
        let message = body.pointer("/choices/0/message")?;
        if let Some(parsed) = message.get("parsed") {
            if !parsed.is_null() {
                return Some(Self::parse_json_value_or_string(parsed));
            }
        }
        if let Some(arguments) = message.pointer("/function_call/arguments") {
            return Some(Self::parse_json_value_or_string(arguments));
        }
        let tool_calls = message.get("tool_calls").and_then(|v| v.as_array())?;
        for call in tool_calls {
            if let Some(arguments) = call.pointer("/function/arguments") {
                return Some(Self::parse_json_value_or_string(arguments));
            }
        }
        None
    }

    fn parse_json_value_or_string(value: &Value) -> Result<Value, ProviderError> {
        match value {
            Value::String(s) => parse_content_json(s),
            other => Ok(other.clone()),
        }
    }

    fn parsed_chat_content_fallback_excerpt(body: &Value) -> Option<String> {
        let message = body.pointer("/choices/0/message")?;
        if let Some(parsed) = message.get("parsed") {
            if !parsed.is_null() {
                return Some(sanitized_provider_json_excerpt(parsed, 1200));
            }
        }
        if let Some(arguments) = message.pointer("/function_call/arguments") {
            return Some(match arguments {
                Value::String(s) => compact_text_excerpt(s, 1200),
                other => sanitized_provider_json_excerpt(other, 1200),
            });
        }
        let tool_calls = message.get("tool_calls").and_then(|v| v.as_array())?;
        for call in tool_calls {
            if let Some(arguments) = call.pointer("/function/arguments") {
                return Some(match arguments {
                    Value::String(s) => compact_text_excerpt(s, 1200),
                    other => sanitized_provider_json_excerpt(other, 1200),
                });
            }
        }
        None
    }

    fn analyze_from_flat_value(
        parsed: &Value,
        model_id: &str,
        header_req_id: &str,
        usage: Usage,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        if parsed.get("description").is_none() && parsed.get("caption").is_none() {
            return Err(ProviderError::SchemaValidationMessage(
                "provider response did not contain description".to_string(),
            ));
        }
        if parsed.get("rating").is_none() {
            return Err(ProviderError::SchemaValidationMessage(
                "provider response did not contain rating".to_string(),
            ));
        }
        let understanding = DescribeOutcome {
            caption: output_description(parsed),
            tags: output_tags(parsed),
            scene: output_scene(parsed),
            confidence: output_confidence(parsed),
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: header_req_id.to_string(),
        };
        let rating = ScoreOutcome {
            rating: parsed.get("rating").and_then(parse_rating_int).unwrap_or(0),
            rubric_id: output_rubric_id(parsed),
            rubric_version: output_rubric_version(parsed),
            reasons: output_rating_reason(parsed),
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: header_req_id.to_string(),
        };
        let out = AnalyzeOutcome {
            understanding: Some(understanding),
            rating: Some(rating),
            model_id: model_id.to_string(),
            usage,
            provider_request_id: header_req_id.to_string(),
        };
        validate_analyze(&out)?;
        Ok(out)
    }

    fn parse_score(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<ScoreOutcome, ProviderError> {
        let parsed = self.parsed_chat_content(body)?;
        // The remote LLM is asked for a 1..=5 integer star rating. Accept an exact
        // integer, or an integer-valued float a model may emit despite the
        // `integer` schema (e.g. `4.0`); a fractional float (e.g. `4.9`) is
        // schema-invalid and is NOT truncated — `parse_rating_int` returns None,
        // the rating falls back to 0 (outside the 1..=5 contract), and
        // `validate_rating` rejects it (fail closed, no active annotation).
        let rating = parsed.get("rating").and_then(parse_rating_int).unwrap_or(0);
        let out = ScoreOutcome {
            rating,
            rubric_id: output_rubric_id(&parsed),
            rubric_version: output_rubric_version(&parsed),
            reasons: output_rating_reason(&parsed),
            model_id: model_id.to_string(),
            usage: extract_usage(
                self.config
                    .response
                    .usage_json_pointer
                    .as_deref()
                    .and_then(|p| json_pointer_str(body, p)),
            ),
            provider_request_id: header_req_id.to_string(),
        };
        validate_rating(&out)?;
        Ok(out)
    }

    fn parse_analyze(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        let parsed = self.parsed_chat_content(body)?;
        let usage = extract_usage(
            self.config
                .response
                .usage_json_pointer
                .as_deref()
                .and_then(|p| json_pointer_str(body, p)),
        );
        Self::analyze_from_flat_value(&parsed, model_id, header_req_id, usage)
    }

    fn parse_batch_analyze_items(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
        expected_len: usize,
        required_indices: &[usize],
    ) -> Result<Vec<(usize, AnalyzeOutcome)>, (Vec<(usize, AnalyzeOutcome)>, Vec<BatchItemRepair>)>
    {
        let usage = extract_usage(
            self.config
                .response
                .usage_json_pointer
                .as_deref()
                .and_then(|p| json_pointer_str(body, p)),
        );
        let parsed = match self.parsed_chat_content(body) {
            Ok(parsed) => parsed,
            Err(error) => {
                return Err((
                    Vec::new(),
                    required_indices
                        .iter()
                        .map(|index| BatchItemRepair {
                            index: *index,
                            bad_json: self.response_content_excerpt(body),
                            error: error.clone(),
                        })
                        .collect(),
                ));
            }
        };
        let Some(results) = parsed.get("results").and_then(|v| v.as_array()) else {
            return Err((
                Vec::new(),
                required_indices
                    .iter()
                    .map(|index| BatchItemRepair {
                        index: *index,
                        bad_json: compact_json_excerpt(&parsed, 1200),
                        error: ProviderError::SchemaValidationMessage(
                            "batch response did not contain results array".to_string(),
                        ),
                    })
                    .collect(),
            ));
        };
        let mut found = vec![false; expected_len];
        let mut ok = Vec::new();
        let mut failures = Vec::new();
        for (position, item) in results.iter().enumerate() {
            let index = item
                .get("index")
                .and_then(|v| v.as_u64())
                .map(|v| v as usize)
                .unwrap_or(position);
            if index >= expected_len {
                failures.push(BatchItemRepair {
                    index,
                    bad_json: compact_json_excerpt(item, 1200),
                    error: ProviderError::SchemaValidationMessage(format!(
                        "batch item index {index} is outside expected range 0..{}",
                        expected_len.saturating_sub(1)
                    )),
                });
                continue;
            }
            found[index] = true;
            match Self::analyze_from_flat_value(item, model_id, header_req_id, usage.clone()) {
                Ok(outcome) => ok.push((index, outcome)),
                Err(error) => failures.push(BatchItemRepair {
                    index,
                    bad_json: compact_json_excerpt(item, 1200),
                    error,
                }),
            }
        }
        for index in required_indices {
            if *index < found.len() && !found[*index] {
                failures.push(BatchItemRepair {
                    index: *index,
                    bad_json: "null".to_string(),
                    error: ProviderError::SchemaValidationMessage(format!(
                        "batch response omitted required result index {index}"
                    )),
                });
            }
        }
        if failures.is_empty() {
            Ok(ok)
        } else {
            Err((ok, failures))
        }
    }

    fn response_content_excerpt(&self, body: &Value) -> String {
        let content_pointer = self
            .config
            .response
            .content_json_pointer
            .as_deref()
            .unwrap_or("/choices/0/message/content");
        match json_pointer_str(body, content_pointer) {
            Some(Value::String(s)) => compact_text_excerpt(s, 1200),
            Some(Value::Null) => Self::parsed_chat_content_fallback_excerpt(body)
                .unwrap_or_else(|| "null".to_string()),
            Some(other) => sanitized_provider_json_excerpt(other, 1200),
            None => Self::parsed_chat_content_fallback_excerpt(body)
                .unwrap_or_else(|| sanitized_provider_json_excerpt(body, 1200)),
        }
    }
}

fn describe_prompt(
    prompt_profile_id: &str,
    output_language: &str,
) -> Result<(String, String), ProviderError> {
    let prompt =
        crate::service::prompt_profiles::describe_prompt(prompt_profile_id, output_language)?;
    Ok((prompt.system, prompt.instruction))
}

fn score_prompt(
    prompt_profile_id: &str,
    rubric_id: &str,
    rating_severity: &str,
    output_language: &str,
    camera_context: &str,
    include_rating_reasons: bool,
) -> Result<(String, String), ProviderError> {
    let prompt = crate::service::prompt_profiles::score_prompt(
        prompt_profile_id,
        rubric_id,
        rating_severity,
        output_language,
        camera_context,
        include_rating_reasons,
    )?;
    Ok((prompt.system, prompt.instruction))
}

fn analyze_prompt(
    prompt_profile_id: &str,
    rubric_id: &str,
    rating_severity: &str,
    output_language: &str,
    camera_context: &str,
    include_understanding: bool,
    include_rating: bool,
    include_rating_reasons: bool,
) -> Result<(String, String), ProviderError> {
    let prompt = crate::service::prompt_profiles::analyze_prompt(
        prompt_profile_id,
        rubric_id,
        rating_severity,
        output_language,
        camera_context,
        include_understanding,
        include_rating,
        include_rating_reasons,
    )?;
    Ok((prompt.system, prompt.instruction))
}

fn batch_analyze_prompt(
    prompt_profile_id: &str,
    rubric_id: &str,
    rating_severity: &str,
    output_language: &str,
    images: &[AnalyzeImageInput<'_>],
    include_understanding: bool,
    include_rating: bool,
    include_rating_reasons: bool,
) -> Result<(String, String), ProviderError> {
    let camera_context = images
        .iter()
        .enumerate()
        .filter(|(_, image)| !image.camera_context.trim().is_empty())
        .map(|(index, image)| format!("Image index {index}: {}", image.camera_context.trim()))
        .collect::<Vec<_>>()
        .join("\n");
    let (system, mut instruction) = analyze_prompt(
        prompt_profile_id,
        rubric_id,
        rating_severity,
        output_language,
        &camera_context,
        include_understanding,
        include_rating,
        include_rating_reasons,
    )?;
    instruction.push_str(&format!(
        r#"

Analyze exactly {} images in this one request. Return one result object per image in a top-level "results" array. Each result object must include its zero-based "index" matching the image order. Do not omit images."#,
        images.len()
    ));
    Ok((system, instruction))
}

fn schema_repair_instruction(schema_name: &str, error: &ProviderError) -> String {
    let shape = match schema_name {
        UNDERSTANDING_SCHEMA_NAME => {
            r#"{
  "description": "non-empty string"
}"#
        }
        RATING_SCHEMA_NAME => {
            r#"{
  "rating": 1,
  "rating_reason": "string"
}"#
        }
        ANALYSIS_FLAT_SCHEMA_NAME => {
            r#"{
  "description": "non-empty string",
  "rating": 1,
  "rating_reason": "string"
}"#
        }
        _ => "{}",
    };
    format!(
        r#"The previous response did not match the required schema: {error}

Using the same image and task context, respond again with a JSON object matching `{schema_name}` exactly:
{shape}

Do not include prose or markdown. Do not add extra fields. For `{ANALYSIS_FLAT_SCHEMA_NAME}`, the object must be flat; do not nest fields under "understanding" or "rating"."#
    )
}

fn batch_schema_repair_instruction(failures: &[BatchItemRepair]) -> String {
    let failed = failures
        .iter()
        .map(|failure| {
            format!(
                r#"index {index}
bad_json: {bad_json}
error: {error}"#,
                index = failure.index,
                bad_json = failure.bad_json,
                error = failure.error
            )
        })
        .collect::<Vec<_>>()
        .join("\n\n");
    format!(
        r#"Some result items from the previous batch response did not match the required per-item schema.

Repair only the failed item indexes below. Call the same schema again and return a JSON object with a "results" array containing only corrected objects for these failed indexes.

{failed}

Each corrected item must match this exact shape:
{{
  "index": 0,
  "description": "non-empty string",
  "rating": 1,
  "rating_reason": "string"
}}

Do not include prose or markdown. Do not add extra fields. Do not return already-valid indexes."#
    )
}

#[tonic::async_trait]
impl ImageAnalysisProvider for OpenAiChatCompatibleProvider {
    fn provider_id(&self) -> &str {
        &self.config.provider_id
    }

    fn requires_credential(&self) -> bool {
        self.config.auth.auth_type != "none"
    }

    fn max_payload_bytes(&self) -> usize {
        self.config.limits.max_image_bytes as usize
    }

    fn capability(&self) -> crate::proto::alcedo::ai::AiCapability {
        // Real providers advertise their capabilities via the provider registry
        // (Phase 5a `build_provider_capability_descriptors`), which emits one
        // descriptor per advertised model. This trait method is a compliance
        // fallback returning the default model's understanding descriptor; it is
        // not used to advertise a compatible provider to the C++ host.
        use crate::proto::alcedo::ai::{AiCapability, AiInputKind, AiOutputKind};
        AiCapability {
            task_id: "image_understanding.describe".to_string(),
            provider_id: self.config.provider_id.clone(),
            model_id: self.config.defaults.model.clone(),
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
            requires_credential: self.requires_credential(),
            max_payload_bytes: self.config.limits.max_image_bytes as i64,
        }
    }

    async fn describe_image(
        &self,
        image_bytes: &[u8],
        model_id: &str,
        prompt_profile_id: &str,
        output_language: &str,
        credential: Option<&SecretString>,
    ) -> Result<DescribeOutcome, ProviderError> {
        let (slug, model) = self.resolve_model(model_id)?;
        self.ensure_structured_output(model.as_ref())?;
        let bearer = self.bearer(credential)?;
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::describe(),
        ))?;
        let (system, instruction) = describe_prompt(prompt_profile_id, output_language)?;
        let (outcome, provider_content, schema_repair_attempt) = self
            .send_and_parse_describe(
                &slug,
                model.as_ref(),
                &data_uri,
                schema,
                &system,
                &instruction,
                bearer,
            )
            .await?;
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            caption = %compact_text_excerpt(&outcome.caption, 240),
            tags = ?outcome.tags,
            provider_content = %provider_content,
            schema_repair_attempt,
            "DescribeImage completed"
        );
        Ok(outcome)
    }

    async fn score_image(
        &self,
        image_bytes: &[u8],
        model_id: &str,
        prompt_profile_id: &str,
        rubric_id: &str,
        rating_severity: &str,
        output_language: &str,
        camera_context: &str,
        include_rating_reasons: bool,
        credential: Option<&SecretString>,
    ) -> Result<ScoreOutcome, ProviderError> {
        let (slug, model) = self.resolve_model(model_id)?;
        self.ensure_structured_output(model.as_ref())?;
        let bearer = self.bearer(credential)?;
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::score(include_rating_reasons),
        ))?;
        let (system, instruction) = score_prompt(
            prompt_profile_id,
            rubric_id,
            rating_severity,
            output_language,
            camera_context,
            include_rating_reasons,
        )?;
        let (outcome, provider_content, schema_repair_attempt) = self
            .send_and_parse_score(
                &slug,
                model.as_ref(),
                &data_uri,
                schema,
                &system,
                &instruction,
                bearer,
            )
            .await?;
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            rating = outcome.rating,
            reasons = %compact_text_excerpt(&outcome.reasons, 360),
            provider_content = %provider_content,
            schema_repair_attempt,
            "ScoreImage completed"
        );
        Ok(outcome)
    }

    async fn analyze_image(
        &self,
        image_bytes: &[u8],
        model_id: &str,
        prompt_profile_id: &str,
        rubric_id: &str,
        rating_severity: &str,
        output_language: &str,
        camera_context: &str,
        include_understanding: bool,
        include_rating: bool,
        include_rating_reasons: bool,
        credential: Option<&SecretString>,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        let (slug, model) = self.resolve_model(model_id)?;
        self.ensure_structured_output(model.as_ref())?;
        let bearer = self.bearer(credential)?;
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::analyze(
                include_understanding,
                include_rating,
                include_rating_reasons,
            ),
        ))?;
        let (system, instruction) = analyze_prompt(
            prompt_profile_id,
            rubric_id,
            rating_severity,
            output_language,
            camera_context,
            include_understanding,
            include_rating,
            include_rating_reasons,
        )?;
        let (outcome, provider_content, schema_repair_attempt) = self
            .send_and_parse_analyze(
                &slug,
                model.as_ref(),
                &data_uri,
                schema,
                &system,
                &instruction,
                bearer,
            )
            .await?;
        let caption = outcome
            .understanding
            .as_ref()
            .map(|u| compact_text_excerpt(&u.caption, 240))
            .unwrap_or_default();
        let rating = outcome.rating.as_ref().map(|r| r.rating).unwrap_or(0);
        let reasons = outcome
            .rating
            .as_ref()
            .map(|r| compact_text_excerpt(&r.reasons, 360))
            .unwrap_or_default();
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            caption = %caption,
            rating,
            reasons = %reasons,
            provider_content = %provider_content,
            schema_repair_attempt,
            "AnalyzeImage completed"
        );
        Ok(outcome)
    }

    async fn batch_analyze_images(
        &self,
        images: &[AnalyzeImageInput<'_>],
        model_id: &str,
        prompt_profile_id: &str,
        rubric_id: &str,
        rating_severity: &str,
        output_language: &str,
        include_understanding: bool,
        include_rating: bool,
        include_rating_reasons: bool,
        credential: Option<&SecretString>,
    ) -> Result<BatchAnalyzeOutcome, ProviderError> {
        if images.is_empty() {
            return Err(ProviderError::SchemaValidationMessage(
                "batch_analyze_images requires at least one image".to_string(),
            ));
        }
        let (slug, model) = self.resolve_model(model_id)?;
        self.ensure_structured_output(model.as_ref())?;
        let bearer = self.bearer(credential)?;
        let data_uris = images
            .iter()
            .map(|image| build_image_data_uri(image.image_bytes))
            .collect::<Result<Vec<_>, _>>()?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::batch_analyze(
                include_understanding,
                include_rating,
                include_rating_reasons,
            ),
        ))?;
        let (system, instruction) = batch_analyze_prompt(
            prompt_profile_id,
            rubric_id,
            rating_severity,
            output_language,
            images,
            include_understanding,
            include_rating,
            include_rating_reasons,
        )?;
        let (outcome, provider_content, schema_repair_attempt) = self
            .send_and_parse_batch_analyze(
                &slug,
                model.as_ref(),
                &data_uris,
                schema,
                &system,
                &instruction,
                bearer,
            )
            .await?;
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            items = outcome.items.len(),
            provider_content = %provider_content,
            schema_repair_attempt,
            "BatchAnalyzeImage completed"
        );
        Ok(outcome)
    }

    async fn list_models(
        &self,
        credential: Option<&SecretString>,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        let bearer = self.bearer(credential)?;
        let resp = send_get_with_retry(
            &self.http,
            &self.models_url(),
            &self.attribution_headers(),
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (_headers, body) = read_response(resp).await?;
        let out = parse_discovered_models(
            &body,
            &self.config.models_response,
            &self.config.provider_id,
            "OpenAI-compatible",
        )?;
        // Phase 6c+: commit the discovered candidates so a later explicit
        // `model_id` selecting one of them passes the `resolve_model` gate and
        // the `supports_structured_output` check. This makes "Test & Refresh
        // Models" immediately usable: a discovered model becomes selectable and
        // functional without a sidecar restart or a hand-authored user config.
        self.commit_discovered_models(&out);
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::service::credential_vault::SecretString;
    use crate::service::provider_config::load_provider_configs;
    use serde_json::{Value, json};
    use wiremock::matchers::{header, method, path};
    use wiremock::{Mock, MockServer, ResponseTemplate};

    const TEST_SECRET: &str = "opencode-test-key-DO-NOT-LEAK";
    const TEST_PROMPT: &str = "Describe this image for a photo library.";
    const RAW_BODY_SENTINEL: &str = "RAW_OPENAI_COMPAT_BODY_SENTINEL";

    fn test_image_png() -> Vec<u8> {
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([10, 20, 30]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        cursor.into_inner()
    }

    /// Load the `opencode_go_openai` built-in (OpenAI-compatible, no attribution
    /// headers, no `provider_require_parameters`, no `data_collection`) and point
    /// it at the mock server. This is the Phase 6b product-facing preset shape.
    fn provider_for(server: &MockServer) -> OpenAiChatCompatibleProvider {
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("opencode_go_openai")
            .expect("opencode_go_openai built-in")
            .clone();
        config.base_url = server.uri();
        // OpenCode built-ins are already structured-output capable; these tests
        // exercise the wire shape against a mock endpoint.
        OpenAiChatCompatibleProvider::new(config).expect("provider builds")
    }

    /// An OpenAI-compatible response shape that simulates a reasoning model
    /// exhausting its OWN max output budget on reasoning before producing any
    /// content: `choices[0].message.content` is null and `finish_reason` is
    /// `"length"`, with no `tool_calls` to fall back to. The driver sends no
    /// `max_tokens`, so this shape means the provider/model's own output window
    /// was reached (not a cap we imposed) and the call fails closed with a
    /// clear, actionable error.
    fn length_starved_body() -> Value {
        json!({
            "id": "req-length-starved",
            "choices": [{
                "index": 0,
                "finish_reason": "length",
                "message": { "role": "assistant", "content": null }
            }],
            "usage": { "prompt_tokens": 10, "completion_tokens": 1200, "total_tokens": 1210 }
        })
    }

    fn ok_understanding_body(content_json: &str) -> Value {
        json!({
            "id": "opencode-req-123",
            "choices": [
                { "message": { "content": content_json } }
            ],
            "usage": { "prompt_tokens": 100, "completion_tokens": 40, "total_tokens": 140 }
        })
    }

    fn secret() -> SecretString {
        SecretString::new(TEST_SECRET.to_string())
    }

    #[tokio::test]
    async fn sends_bearer_authorization_without_attribution_or_routing_for_opencode() {
        // Opencode preset: bearer auth, but NO attribution headers and NO `provider`
        // routing object (OpenRouter knobs are off by default for Opencode).
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .and(header("authorization", format!("Bearer {TEST_SECRET}")))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                r#"{"caption":"a small image","tags":["test"],"scene":"studio","confidence":0.7}"#,
            )))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "profile-1", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "a small image");
        assert_eq!(out.tags, vec!["test".to_string()]);
        assert_eq!(out.provider_request_id, "opencode-req-123");
        assert_eq!(out.usage.total_tokens, 140);

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        // No OpenRouter attribution headers for an Opencode preset.
        assert!(
            reqs[0].headers.get("http-referer").is_none(),
            "OpenRouter HTTP-Referer sent for Opencode preset"
        );
        assert!(
            reqs[0].headers.get("x-openrouter-title").is_none(),
            "OpenRouter X-OpenRouter-Title sent for Opencode preset"
        );
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert!(
            body.get("provider").is_none(),
            "OpenRouter `provider` routing object present for Opencode preset: {body}"
        );
    }

    #[tokio::test]
    async fn request_body_has_chat_tool_names_and_image_data_uri() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        provider
            .describe_image(&test_image_png(), "kimi-k2.7-code", "", "", Some(&secret()))
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(body["model"], "kimi-k2.7-code");
        assert_eq!(body["stream"], false);
        assert!(
            body.get("response_format").is_none(),
            "OpenAI-compatible structured output uses tool calls, not response_format"
        );
        assert_eq!(
            body["tools"][0]["function"]["name"],
            "alcedo_image_understanding"
        );
        assert_eq!(
            body["tool_choice"]["function"]["name"],
            "alcedo_image_understanding"
        );
        assert_eq!(body["tools"][0]["function"]["strict"], true);
        assert_eq!(body["parallel_tool_calls"], false);
        let schema = &body["tools"][0]["function"]["parameters"];
        assert_eq!(schema["type"], "object");
        assert_eq!(schema["additionalProperties"], false);
        let required: Vec<&str> = schema["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert_eq!(required, vec!["description"]);
        // The image is carried as a data URI in the user message.
        let img_url = &body["messages"][1]["content"][1]["image_url"]["url"];
        assert!(
            img_url
                .as_str()
                .unwrap()
                .starts_with("data:image/png;base64,")
        );
    }

    #[tokio::test]
    async fn parses_understanding_response_and_captures_usage() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                r#"{"caption":"sunrise over mountains","tags":["sunrise","mountain","landscape"],"scene":"outdoor","confidence":0.82}"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "sunrise over mountains");
        assert_eq!(out.tags.len(), 3);
        assert_eq!(out.scene, "outdoor");
        assert!((out.confidence - 0.82).abs() < 1e-9);
        assert_eq!(out.model_id, "kimi-k2.7-code");
        assert_eq!(out.usage.input_tokens, 100);
        assert_eq!(out.usage.output_tokens, 40);
        assert_eq!(out.usage.total_tokens, 140);
        assert_eq!(out.provider_request_id, "opencode-req-123");
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[tokio::test]
    async fn parses_rating_response_and_captures_usage() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "opencode-req-456",
            "choices": [ { "message": { "content":
                r#"{"rating":4,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"good"}"# } } ],
            "usage": { "prompt_tokens": 80, "completion_tokens": 50, "total_tokens": 130 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .score_image(
                &test_image_png(),
                "",
                "",
                "alcedo-default-v1",
                "",
                "",
                "",
                true,
                Some(&secret()),
            )
            .await
            .expect("score ok");
        assert_eq!(out.rating, 4);
        assert_eq!(out.rubric_id, "alcedo-default-v1");
        assert_eq!(out.usage.total_tokens, 130);
        assert_eq!(out.provider_request_id, "opencode-req-456");
        validate_rating(&out).expect("canned rating validates");
    }

    #[tokio::test]
    async fn analyze_image_uses_flat_schema_and_parses_flat_response() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "opencode-req-flat",
            "choices": [ { "message": { "content":
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5,"rating":4,"rubric_id":"general","rubric_version":"","reasons":"solid"}"# } } ],
            "usage": { "prompt_tokens": 120, "completion_tokens": 60, "total_tokens": 180 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "normal",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("flat analyze ok");

        validate_analyze(&out).expect("flat analyze outcome validates");
        assert_eq!(out.understanding.as_ref().unwrap().caption, "c");
        assert_eq!(out.rating.as_ref().unwrap().rating, 4);
        assert_eq!(out.provider_request_id, "opencode-req-flat");

        let reqs = server.received_requests().await.expect("requests captured");
        let req_body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(
            req_body["tools"][0]["function"]["name"],
            "alcedo_image_analysis_flat"
        );
        assert_eq!(
            req_body["tool_choice"]["function"]["name"],
            "alcedo_image_analysis_flat"
        );
        let schema = &req_body["tools"][0]["function"]["parameters"];
        let required: Vec<&str> = schema["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|v| v.as_str().unwrap())
            .collect();
        assert!(required.contains(&"description"));
        assert!(required.contains(&"rating"));
        assert!(required.contains(&"rating_reason"));
        assert!(schema["properties"].get("understanding").is_none());
    }

    #[tokio::test]
    async fn analyze_image_request_includes_all_chat_tool_names() {
        let server = MockServer::start().await;
        let tool_body = json!({
            "id": "opencode-req-tool",
            "choices": [{
                "finish_reason": "tool_calls",
                "message": {
                    "role": "assistant",
                    "content": null,
                    "tool_calls": [{
                        "id": "call_1",
                        "type": "function",
                        "function": {
                            "name": ANALYSIS_FLAT_SCHEMA_NAME,
                            "arguments": r#"{"description":"c","rating":3,"rating_reason":"tool ok"}"#
                        }
                    }]
                }
            }],
            "usage": { "prompt_tokens": 120, "completion_tokens": 40, "total_tokens": 160 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(tool_body))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "normal",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("tool response succeeds");

        validate_analyze(&out).expect("tool response outcome validates");
        assert_eq!(out.understanding.as_ref().unwrap().caption, "c");
        assert_eq!(out.rating.as_ref().unwrap().rating, 3);

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert!(
            body.get("response_format").is_none(),
            "tool structured output should not also send response_format"
        );
        assert_eq!(
            body["tools"][0]["function"]["name"],
            ANALYSIS_FLAT_SCHEMA_NAME
        );
        assert_eq!(
            body["tool_choice"]["function"]["name"],
            ANALYSIS_FLAT_SCHEMA_NAME
        );
        assert_eq!(body["parallel_tool_calls"], false);
    }

    #[tokio::test]
    async fn analyze_image_repairs_schema_once_as_second_turn_only_after_failure() {
        let server = MockServer::start().await;
        let bad_body = json!({
            "id": "opencode-req-bad",
            "choices": [ { "message": { "content":
                r#"{"rating":4,"rating_reason":"solid"}"# } } ],
            "usage": { "prompt_tokens": 120, "completion_tokens": 60, "total_tokens": 180 }
        });
        let repaired_body = json!({
            "id": "opencode-req-repaired",
            "choices": [ { "message": { "content":
                r#"{"description":"c","rating":4,"rating_reason":"solid"}"# } } ],
            "usage": { "prompt_tokens": 150, "completion_tokens": 45, "total_tokens": 195 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(bad_body))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(repaired_body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "normal",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("repair succeeds");

        validate_analyze(&out).expect("repaired analyze outcome validates");
        assert_eq!(out.provider_request_id, "opencode-req-repaired");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 2);
        let first: Value = serde_json::from_slice(&reqs[0].body).expect("first body json");
        let second: Value = serde_json::from_slice(&reqs[1].body).expect("second body json");
        assert_eq!(first["messages"].as_array().unwrap().len(), 2);
        assert_eq!(second["messages"].as_array().unwrap().len(), 4);
        assert_eq!(second["messages"][2]["role"], "assistant");
        assert!(
            second["messages"][2]["content"]
                .as_str()
                .unwrap()
                .contains("\"rating\":4")
        );
        assert_eq!(second["messages"][3]["role"], "user");
        assert!(
            second["messages"][3]["content"]
                .as_str()
                .unwrap()
                .contains("previous response did not match")
        );
        assert!(
            second["messages"][1]["content"][1]["image_url"]["url"]
                .as_str()
                .unwrap()
                .starts_with("data:image/png;base64,")
        );
    }

    #[tokio::test]
    async fn rate_limit_maps_to_transient() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(429))
            .up_to_n_times(2)
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("transient after retries");
        assert_eq!(err, ProviderError::Transient);
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn server_500_is_retried_then_succeeds() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(500))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("succeeds after retry");
        assert_eq!(out.caption, "c");
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn client_4xx_is_not_retried_and_maps_to_provider_error() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(400)
                    .set_body_json(json!({ "error": { "message": RAW_BODY_SENTINEL } })),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("400 not retried");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
        // The raw provider body is NOT surfaced in the error string.
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body leaked: {err}"
        );
    }

    #[tokio::test]
    async fn batch_analyze_accepts_tool_call_arguments_when_content_is_null() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "opencode-req-tool-call",
            "choices": [
                {
                    "message": {
                        "content": null,
                        "tool_calls": [
                            {
                                "id": "call_1",
                                "type": "function",
                                "function": {
                                    "name": ANALYSIS_BATCH_SCHEMA_NAME,
                                    "arguments": r#"{"results":[{"index":0,"description":"第一张图","rating":4,"rating_reason":"构图完整"},{"index":1,"description":"第二张图","rating":3,"rating_reason":"主体较弱"}]}"#
                                }
                            }
                        ]
                    }
                }
            ],
            "usage": { "prompt_tokens": 100, "completion_tokens": 40, "total_tokens": 140 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let img_a = test_image_png();
        let img_b = test_image_png();
        let inputs = [
            AnalyzeImageInput {
                image_bytes: &img_a,
                camera_context: "",
            },
            AnalyzeImageInput {
                image_bytes: &img_b,
                camera_context: "",
            },
        ];

        let out = provider
            .batch_analyze_images(
                &inputs,
                "",
                "",
                "general",
                "normal",
                "zh",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("tool call arguments accepted");

        assert_eq!(out.items.len(), 2);
        assert_eq!(
            out.items[0].understanding.as_ref().unwrap().caption,
            "第一张图"
        );
        assert_eq!(out.items[0].rating.as_ref().unwrap().rating, 4);
        assert_eq!(out.items[1].rating.as_ref().unwrap().reasons, "主体较弱");
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn ignored_structured_tool_output_produces_no_active_annotation() {
        // Explicit "unsupported structured output" fail-closed path: a compatible
        // endpoint that ignores the requested tool output and returns plain prose
        // (not JSON) must not create an active annotation. The first schema failure gets one
        // repair turn; if the endpoint still ignores JSON, the call fails closed.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    "Sorry, I can only describe images in plain text.",
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("ignored structured tool output rejected");
        match err {
            ProviderError::SchemaValidation | ProviderError::SchemaValidationMessage(_) => {}
            other => panic!("expected schema validation, got {other:?}"),
        }
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn describe_accepts_markdown_fenced_json_content() {
        // Some compatible models wrap the JSON in a ```json fence despite the
        // structured output request. The driver must parse the JSON out of the
        // fence rather than failing on the leading ``` and burning a paid repair
        // retry the model tends to repeat.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                "```json\n{\"caption\":\"a small image\",\"tags\":[\"test\"],\"scene\":\"studio\",\"confidence\":0.7}\n```",
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("fenced JSON content accepted on first attempt");
        assert_eq!(out.caption, "a small image");
        assert_eq!(out.tags, vec!["test".to_string()]);
        assert!((out.confidence - 0.7).abs() < 1e-9);
        // No repair retry: exactly one provider call.
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn json_violating_contract_maps_to_schema_validation() {
        // Valid JSON but it violates the code-owned contract (empty caption) — the
        // validator rejects it; no active annotation.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("empty caption rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn bearer_required_without_credential_errors() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", None)
            .await
            .expect_err("missing credential");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        assert!(server.received_requests().await.unwrap().is_empty());
    }

    #[tokio::test]
    async fn provider_request_id_captured_from_response_header_when_configured() {
        // The Opencode OpenAI built-in uses the `/id` body pointer. Override the
        // config to use a response header instead, proving the header-capture path
        // (Phase 6b deliverable #4) works for the OpenAI-compatible driver.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_understanding_body(
                        r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                    ))
                    .insert_header("request-id", "hdr-opencode-999"),
            )
            .mount(&server)
            .await;
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("opencode_go_openai")
            .expect("opencode built-in")
            .clone();
        config.base_url = server.uri();
        config.models[0].supports_structured_output = true;
        config.response.provider_request_id_json_pointer = None;
        config.response.provider_request_id_header = Some("request-id".to_string());
        let provider = OpenAiChatCompatibleProvider::new(config).expect("provider builds");

        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.provider_request_id, "hdr-opencode-999");
    }

    #[test]
    fn no_secret_image_prompt_or_body_in_logs_or_error_strings() {
        // Capture tracing output emitted during a call that triggers a retry (warn)
        // then a 400 with a raw body sentinel. A current_thread runtime drives the
        // whole call on THIS thread, so the thread-local capturing subscriber set by
        // `with_default` is in scope for every `warn!` the driver emits. A
        // multi_thread runtime could poll the future's continuation on a worker
        // thread where the subscriber is NOT set, leaving `captured` empty and the
        // no-leak assertions passing trivially (a false pass). The first assertion
        // confirms capture actually worked; the rest confirm nothing leaked.
        use std::io::Write;
        use std::sync::{Arc, Mutex};
        use tracing_subscriber::fmt::MakeWriter;

        #[derive(Clone)]
        struct BufWriter(Arc<Mutex<Vec<u8>>>);
        impl<'a> MakeWriter<'a> for BufWriter {
            type Writer = BufWriteImpl;
            fn make_writer(&'a self) -> Self::Writer {
                BufWriteImpl {
                    inner: self.0.clone(),
                }
            }
        }
        struct BufWriteImpl {
            inner: Arc<Mutex<Vec<u8>>>,
        }
        impl Write for BufWriteImpl {
            fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
                self.inner.lock().unwrap().extend_from_slice(b);
                Ok(b.len())
            }
            fn flush(&mut self) -> std::io::Result<()> {
                Ok(())
            }
        }

        let buf = Arc::new(Mutex::new(Vec::new()));
        let subscriber = tracing_subscriber::fmt()
            .with_writer(BufWriter(buf.clone()))
            .with_ansi(false)
            .with_env_filter("warn")
            .finish();

        let rt = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("current_thread runtime");
        let err = tracing::subscriber::with_default(subscriber, || {
            rt.block_on(async {
                let server = MockServer::start().await;
                Mock::given(method("POST"))
                    .and(path("/chat/completions"))
                    .respond_with(ResponseTemplate::new(500))
                    .up_to_n_times(1)
                    .mount(&server)
                    .await;
                Mock::given(method("POST"))
                    .and(path("/chat/completions"))
                    .respond_with(
                        ResponseTemplate::new(400)
                            .set_body_json(json!({ "error": { "message": RAW_BODY_SENTINEL } })),
                    )
                    .mount(&server)
                    .await;
                let provider = provider_for(&server);
                provider
                    .describe_image(&test_image_png(), "", "profile-1", "", Some(&secret()))
                    .await
            })
        });
        let err = err.expect_err("400 after retry");

        let captured = String::from_utf8_lossy(&buf.lock().unwrap()).to_string();
        assert!(
            captured.contains("retrying"),
            "log capture did not work (no retry warn captured): {captured}"
        );
        assert!(
            !captured.contains(TEST_SECRET),
            "secret in logs: {captured}"
        );
        assert!(
            !captured.contains("data:image/png;base64,"),
            "image in logs: {captured}"
        );
        assert!(
            !captured.contains(TEST_PROMPT),
            "prompt in logs: {captured}"
        );
        assert!(
            !captured.contains(RAW_BODY_SENTINEL),
            "raw body in logs: {captured}"
        );
        assert!(
            !err.to_string().contains(TEST_SECRET),
            "secret in error: {err}"
        );
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body in error: {err}"
        );
    }

    // ----- Phase 6c: model_id tightening + model discovery -----

    /// Phase 6c: a non-empty explicit `model_id` that is not present in
    /// `config.models[]` fails closed with `UnknownModel` BEFORE any provider
    /// HTTP call. Closes the Phase 6b review gap where an unlisted slug bypassed
    /// `supports_structured_output` and was forwarded verbatim.
    #[tokio::test]
    async fn unknown_explicit_model_id_fails_before_http() {
        let server = MockServer::start().await;
        // No mock mounted: any HTTP call would fail the test by being recorded.
        let provider = provider_for(&server);

        let err = provider
            .describe_image(
                &test_image_png(),
                "not-a-real-model-slug",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect_err("unknown model id should fail closed");
        assert_eq!(
            err,
            ProviderError::UnknownModel("not-a-real-model-slug".to_string())
        );
        // No request should have hit the server.
        assert_eq!(server.received_requests().await.unwrap().len(), 0);
    }

    /// Phase 6c: discovery lists the OpenAI-compatible `/models` shape and
    /// returns unverified candidates with no capability flags.
    #[tokio::test]
    async fn list_models_parses_openai_shape() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "object": "list",
                "data": [
                    { "id": "kimi-k2.7-code", "object": "model", "owned_by": "openai" },
                    { "id": "kimi-k2.7-code-mini", "display_name": "Kimi K2.6" }
                ]
            })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let models = provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok");
        assert_eq!(models.len(), 2);
        assert_eq!(models[0].model_id, "kimi-k2.7-code");
        assert_eq!(models[0].display_name, "kimi-k2.7-code"); // falls back to id
        assert_eq!(models[1].model_id, "kimi-k2.7-code-mini");
        assert_eq!(models[1].display_name, "Kimi K2.6"); // uses display_name
        assert_eq!(models[0].source_provider_id, "opencode_go_openai");
    }

    /// Phase 6c: discovery sends `Authorization: Bearer <secret>` and no
    /// attribution/routing for the Opencode preset.
    #[tokio::test]
    async fn list_models_sends_bearer_auth() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .and(header("authorization", format!("Bearer {TEST_SECRET}")))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({ "data": [] })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok");
        let reqs = server.received_requests().await.unwrap();
        assert_eq!(reqs.len(), 1);
        assert_eq!(reqs[0].method, "GET");
        // No OpenRouter attribution headers for the Opencode preset.
        let hmap: std::collections::HashMap<&str, &str> = reqs[0]
            .headers
            .iter()
            .map(|(k, v)| (k.as_str(), v.to_str().unwrap_or("")))
            .collect();
        assert!(!hmap.contains_key("http-referer"));
        assert!(!hmap.contains_key("x-openrouter-title"));
    }

    /// Phase 6c: a 401 is a non-retryable 4xx -> `Provider` error (auth failure
    /// surfaced to the host's validate-connection flow).
    #[tokio::test]
    async fn list_models_4xx_maps_to_provider_error() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(401).set_body_string("unauthorized"))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let err = provider
            .list_models(Some(&secret()))
            .await
            .expect_err("401 fails");
        match err {
            ProviderError::Provider(_) => {}
            other => panic!("expected Provider for 401, got {other:?}"),
        }
        // 4xx is not retried.
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    /// Phase 6c: 429 then 200 — the transient status retries once and the
    /// final 200 succeeds (`MAX_TRANSIENT_RETRIES = 1` allows exactly one retry).
    #[tokio::test]
    async fn list_models_429_retries_then_succeeds() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(429))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({ "data": [
                { "id": "kimi-k2.7-code" }
            ] })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let models = provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok after retry");
        assert_eq!(models.len(), 1);
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    /// Phase 6c: exhausted 5xx retries -> `Transient`; the secret never appears
    /// in the error string.
    #[tokio::test]
    async fn list_models_exhausted_5xx_is_transient_and_redacts() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(500).set_body_string(RAW_BODY_SENTINEL))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let err = provider
            .list_models(Some(&secret()))
            .await
            .expect_err("500 fails");
        assert_eq!(err, ProviderError::Transient);
        assert!(!err.to_string().contains(TEST_SECRET));
        assert!(!err.to_string().contains(RAW_BODY_SENTINEL));
    }

    /// Phase 6c: a non-`data`-array body fails closed with a diagnostic
    /// schema-validation message so model refresh failures explain what the
    /// provider returned.
    #[tokio::test]
    async fn list_models_non_data_body_reports_response_excerpt() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "models": "oops",
                "api_key": TEST_SECRET
            })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        let err = provider
            .list_models(Some(&secret()))
            .await
            .expect_err("bad shape");
        let ProviderError::SchemaValidationMessage(message) = err else {
            panic!("expected schema validation detail, got {err:?}");
        };
        assert!(message.contains("OpenAI-compatible model list"));
        assert!(message.contains("\"data\""));
        assert!(message.contains("\"models\":\"oops\""));
        assert!(
            !message.contains(TEST_SECRET),
            "secret leaked in diagnostic: {message}"
        );
    }

    #[tokio::test]
    async fn list_models_uses_configured_models_response_path_for_ccswitch_shape() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "models": [
                    "route-a",
                    { "id": "route-b", "name": "Route B" }
                ]
            })))
            .mount(&server)
            .await;
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("ccswitch_openai")
            .expect("ccswitch_openai built-in")
            .clone();
        config.base_url = server.uri();
        let provider = OpenAiChatCompatibleProvider::new(config).expect("provider builds");

        let models = provider
            .list_models(None)
            .await
            .expect("ccswitch shape parses");
        assert_eq!(models.len(), 2);
        assert_eq!(models[0].model_id, "route-a");
        assert_eq!(models[0].display_name, "route-a");
        assert_eq!(models[1].model_id, "route-b");
        assert_eq!(models[1].display_name, "Route B");
    }

    /// Phase 6c+: a discovered model is committed during `list_models`, so a
    /// later explicit `model_id` selecting it resolves (passes the
    /// `resolve_model` slug gate and the synthesized
    /// `supports_structured_output = true` check) and the call proceeds to HTTP.
    /// Before discovery, the same slug fails closed as `UnknownModel`.
    #[tokio::test]
    async fn list_models_commits_discovered_model_so_it_becomes_selectable() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({ "data": [
                { "id": "discovered-vision-model", "display_name": "Discovered Vision" }
            ] })))
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);

        // Before discovery, the slug is unknown -> fail closed before HTTP.
        let err = provider
            .describe_image(
                &test_image_png(),
                "discovered-vision-model",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect_err("unknown slug fails before discovery");
        assert!(matches!(err, ProviderError::UnknownModel(_)));

        // Discovery commits the slug.
        let models = provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok");
        assert_eq!(models.len(), 1);

        // After discovery, the same slug resolves and the call proceeds.
        let out = provider
            .describe_image(
                &test_image_png(),
                "discovered-vision-model",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect("discovered slug selectable after list_models");
        assert_eq!(out.caption, "c");
        // The committed slug is sent in the request body.
        let reqs = server.received_requests().await.expect("requests captured");
        let post = reqs
            .iter()
            .find(|r| r.method.as_str() == "POST")
            .expect("POST captured");
        let body: Value = serde_json::from_slice(&post.body).expect("body json");
        assert_eq!(body["model"], "discovered-vision-model");
    }

    /// Output language: a `zh` directive is appended to the system prompt and
    /// reaches the provider request body; English/default leaves the prompt as-is.
    #[tokio::test]
    async fn describe_image_zh_output_language_appends_directive() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        provider
            .describe_image(&test_image_png(), "", "", "zh", Some(&secret()))
            .await
            .expect("describe ok");
        let reqs = server.received_requests().await.expect("requests captured");
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        let system = body["messages"][0]["content"]
            .as_str()
            .expect("system content");
        assert!(
            system.contains("Simplified Chinese"),
            "zh directive missing: {system}"
        );
    }

    // ----- No `max_tokens` limit + model-output-budget exhaustion (kimi-k2.6) -----
    //
    // The driver intentionally omits `max_tokens` from every request body: a
    // reasoning model (e.g. kimi-k2.6) consumes reasoning tokens out of the same
    // `max_tokens` budget as the visible content, so a server-side cap starves
    // the content (content=null + finish_reason="length"). Letting the
    // provider/model generate as much as it needs — bounded only by the model's
    // own max output — avoids our side throttling generation for every model.

    /// The describe request body MUST NOT carry a `max_tokens` field. The
    /// config's `max_output_tokens` (1200 in the OpenCode built-in) is ignored on
    /// the wire by design: reasoning models would otherwise be starved. Verified
    /// against the real wire body the driver builds.
    #[tokio::test]
    async fn describe_request_body_omits_max_tokens() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(
                ResponseTemplate::new(200).set_body_json(ok_understanding_body(
                    r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                )),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert!(
            body.get("max_tokens").is_none(),
            "max_tokens must be omitted so the model is not throttled: {body}"
        );
        // The rest of the body shape is unchanged.
        assert_eq!(body["model"], "kimi-k2.7-code");
        assert_eq!(body["stream"], false);
        assert_eq!(
            body["tools"][0]["function"]["name"],
            "alcedo_image_understanding"
        );
    }

    /// The batch request body MUST NOT carry a `max_tokens` field either. A
    /// batch amplifies the reasoning-token cost (every image is reasoned about in
    /// one response), so a cap is even more likely to starve the content here.
    #[tokio::test]
    async fn batch_request_body_omits_max_tokens() {
        let server = MockServer::start().await;
        let ok_batch = json!({
            "id": "req-batch-ok",
            "choices": [{ "message": { "content":
                r#"{"results":[{"index":0,"description":"first","rating":3,"rating_reason":"ok"},{"index":1,"description":"second","rating":4,"rating_reason":"good"}]}"# } }]
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_batch))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let img = test_image_png();
        let inputs = [
            AnalyzeImageInput {
                image_bytes: &img,
                camera_context: "",
            },
            AnalyzeImageInput {
                image_bytes: &img,
                camera_context: "",
            },
        ];
        provider
            .batch_analyze_images(
                &inputs,
                "",
                "",
                "general",
                "normal",
                "zh",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("batch ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert!(
            body.get("max_tokens").is_none(),
            "batch max_tokens must be omitted: {body}"
        );
    }

    /// When the model exhausts its OWN max output budget on reasoning (content=
    /// null + finish_reason="length" + no tool-call arguments, with no `max_tokens`
    /// sent), the driver surfaces a clear, actionable error and does NOT burn a
    /// schema-repair retry (the model would repeat the cutoff). This is the
    /// shape observed when a batch exceeds the model's output window.
    #[tokio::test]
    async fn model_output_budget_exhausted_surfaces_clear_error() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(length_starved_body()))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("budget exhausted fails closed");
        match err {
            ProviderError::Provider(msg) => {
                assert!(
                    msg.contains("finish_reason=length"),
                    "error should name the length cutoff: {msg}"
                );
                assert!(
                    msg.contains("max_tokens"),
                    "error should clarify no max_tokens limit was set: {msg}"
                );
                assert!(
                    msg.contains("larger output window"),
                    "error should point at the model's output window: {msg}"
                );
            }
            other => panic!("expected Provider error for budget exhaustion, got {other:?}"),
        }
        // No retry: the model's own ceiling was reached, retrying won't help.
        assert_eq!(
            server.received_requests().await.unwrap().len(),
            1,
            "budget exhaustion does not retry"
        );
    }

    /// A null content with a NON-`"length"` `finish_reason` (e.g.
    /// `content_filter`) is NOT budget exhaustion: the driver must take the
    /// normal schema-repair path, and the surfaced error includes the
    /// `finish_reason` for diagnosis.
    #[tokio::test]
    async fn null_content_with_non_length_finish_reason_takes_schema_repair_path() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "req-filtered",
            "choices": [{
                "index": 0,
                "finish_reason": "content_filter",
                "message": { "role": "assistant", "content": null }
            }],
            "usage": { "prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("non-length null content fails");
        // The error must surface the finish_reason for diagnosis.
        assert!(
            err.to_string().contains("finish_reason: content_filter"),
            "error should include finish_reason for a non-length null: {err}"
        );
        // Not budget exhaustion: the original call + one schema-repair retry fire.
        // The repair retry also returns null, so the call fails closed after the
        // schema-repair turn.
        assert_eq!(
            server.received_requests().await.unwrap().len(),
            2,
            "non-length null takes the schema-repair path"
        );
    }

    /// A null content WITH `tool_calls` is NOT budget exhaustion even when
    /// `finish_reason` is `"length"`: the driver parses the tool-call arguments
    /// via the existing fallback path and succeeds. Guards against the
    /// budget-exhaustion check accidentally consuming the tool-call case.
    #[tokio::test]
    async fn null_content_with_tool_calls_parsed_not_budget_exhausted() {
        let server = MockServer::start().await;
        let body = json!({
            "id": "req-tool-call",
            "choices": [{
                "index": 0,
                "finish_reason": "length",
                "message": {
                    "content": null,
                    "tool_calls": [{
                        "id": "call_1",
                        "type": "function",
                        "function": {
                            "name": ANALYSIS_FLAT_SCHEMA_NAME,
                            "arguments": r#"{"description":"from tool","rating":3,"rating_reason":"ok"}"#
                        }
                    }]
                }
            }],
            "usage": { "prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15 }
        });
        Mock::given(method("POST"))
            .and(path("/chat/completions"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "normal",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("tool-call arguments accepted");
        assert_eq!(out.understanding.as_ref().unwrap().caption, "from tool");
        assert_eq!(out.rating.as_ref().unwrap().rating, 3);
        assert_eq!(
            server.received_requests().await.unwrap().len(),
            1,
            "tool-call fallback parses without retry"
        );
    }
}
