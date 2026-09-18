//! Anthropic Messages image-analysis driver (Phase 5c follow-up, Phase 6b
//! generalized; `anthropic_messages`).
//!
//! Speaks the Anthropic Messages API (`POST /v1/messages`, or `/messages` on
//! Opencode): an Alcedo-owned `system` prompt, a single user message carrying the
//! image as an Anthropic `image` content block (`source.type = "base64"`, raw
//! base64 + `media_type` — NOT a data URI) plus a `text` task instruction, and
//! **tool-use** for structured output (`tools` + `tool_choice = { type: "tool",
//! name }`) with the code-owned (sanitized to Anthropic-strict-compatible) Alcedo
//! schema as the tool `input_schema`. Tool-use is more reliable than
//! `response_format.json_schema` for forcing a typed result, and does not depend
//! on a proxy honoring `json_schema`. The response's `content[].tool_use.input`
//! object (for the expected tool name) is extracted with a driver-owned typed
//! parser (`content_json_pointer` is null for this driver), validated + normalized
//! against the code-owned understanding / rating contract, and returned as typed
//! fields. Transient / 429 / 5xx failures retry under the same bounded policy as
//! the other drivers; provider and transport errors map to `ProviderError`
//! variants the service translates into `AiResponseStatus` / `AiErrorCode`.
//!
//! Auth is selected by `config.auth.auth_type`:
//! - `bearer` — `Authorization: Bearer <secret>` (Claude-Code-style drop-in; used
//!   by the shipped `volcengine_ark_coding` Coding Plan config and the Opencode
//!   Go Anthropic preset).
//! - `api_key_header` — `x-api-key: <secret>` (the real Anthropic API convention).
//! - `none` — no credential. The `anthropic-version: 2023-06-01` header is always
//!   sent. The secret is resolved from the Rust credential vault per request and
//!   travels only as a header value; `SecretString::expose()` is called solely at
//!   the header-build site and the cloned `String` is dropped right after the call.
//!
//! The driver is generic Anthropic Messages and is NOT coupled to any provider
//! brand. The same code targets the Volcengine Ark **Coding Plan**
//! (`https://ark.cn-beijing.volces.com/api/coding` + `/v1/messages`), the real
//! Anthropic API (`api.anthropic.com`), and Opencode's Anthropic-compatible
//! endpoint (`https://opencode.ai/zen/go/v1` + `/messages`), selected purely by
//! the config's `base_url` / `endpoint` / `credential_slot`. The provider request
//! id is captured from EITHER the config's `provider_request_id_header` (Opencode
//! echoes `request-id`) OR its `provider_request_id_json_pointer` (the Coding Plan
//! embeds `id` in the body), whichever the config sets — both are optional because
//! compatible providers do not all report the id the same way (Phase 6b
//! deliverable).
//!
//! ## Coding Plan usage-policy caveat (accepted risk)
//!
//! Volcengine positions the **Coding Plan** (`/api/coding/*`) for *AI coding
//! assistants* (Claude Code, Cursor, OpenClaw-style tools). Using this driver as
//! a coding-tool backend is therefore the intended usage class. The policy risk is
//! specifically routing **non-coding production image analysis** through
//! `volcengine_ark_coding`; that remains an operator decision the user owns. The
//! standard production Volcengine image-analysis path stays `volcengine_ark`
//! (`/api/v3/responses`). See the Phase 5c follow-up note in
//! `docs/roadmap/ai_sidecar_backend_plan.md`.
//!
//! ## Vision caveat
//!
//! Coding Plan / Opencode models are primarily coding / text models. Image
//! analysis only works if the selected model accepts image input. The live smoke
//! is the ground-truth check; if the configured `slug` rejects the image, adjust
//! `slug` to a confirmed vision-capable model — the driver itself is
//! model-agnostic.

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
    MAX_TRANSIENT_RETRIES, build_image_base64, build_rustls_client, compact_json_excerpt,
    compact_text_excerpt, extract_json_from_text_block, extract_provider_request_id, extract_usage,
    json_pointer_str, parse_discovered_models, parse_rating_int, read_response,
    sanitized_provider_json_excerpt, send_get_with_retry, send_with_retry, strict_schema_value,
};

const UNDERSTANDING_SCHEMA_NAME: &str = "alcedo_image_understanding";
const RATING_SCHEMA_NAME: &str = "alcedo_image_rating";
const ANALYSIS_FLAT_SCHEMA_NAME: &str = "alcedo_image_analysis_flat";
const ANALYSIS_BATCH_SCHEMA_NAME: &str = "alcedo_image_analysis_batch";
const ANTHROPIC_VERSION: &str = "2023-06-01";
const ANALYZE_SCHEMA_REPAIR_RETRIES: u32 = 1;

#[derive(Debug, Clone)]
struct BatchItemRepair {
    index: usize,
    bad_json: String,
    error: ProviderError,
}

struct BatchRepairContext<'a> {
    failures: &'a [BatchItemRepair],
}

pub struct AnthropicMessagesProvider {
    config: ProviderConfig,
    http: reqwest::Client,
    /// Models committed from live `list_models` discovery so an explicitly
    /// requested discovered slug passes the Phase 6c `resolve_model` gate. See
    /// `OpenAiChatCompatibleProvider::discovered_models` for the rationale.
    discovered_models: Mutex<Vec<ModelConfig>>,
}

impl AnthropicMessagesProvider {
    pub fn new(config: ProviderConfig) -> Result<Self, ProviderError> {
        let http = build_rustls_client()?;
        Ok(Self {
            config,
            http,
            discovered_models: Mutex::new(Vec::new()),
        })
    }

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
    /// `{base_url}/models` (the Anthropic-compatible default); a config
    /// `models_endpoint` override replaces the path.
    fn models_url(&self) -> String {
        let path = self.config.models_endpoint.as_deref().unwrap_or("/models");
        format!("{}{}", self.config.base_url, path)
    }

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

    /// Commit discovered models so a later explicit `model_id` resolves. See
    /// `OpenAiChatCompatibleProvider::commit_discovered_models` for the rationale
    /// (idempotent, synthesized `supports_structured_output = true`).
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

    /// Resolve the auth material for a request. Returns `(bearer, extra_header)`:
    /// `bearer` is borrowed from the credential for the `Authorization: Bearer`
    /// header; `extra_header` is an owned `(name, value)` pair for the
    /// `api_key_header` mode (`x-api-key`). The `api_key_header` value is a
    /// short-lived `String` clone of the secret, dropped after the request.
    fn build_auth<'a>(
        &'a self,
        credential: Option<&'a SecretString>,
    ) -> Result<(Option<&'a str>, Option<(String, String)>), ProviderError> {
        match (self.config.auth.auth_type.as_str(), credential) {
            ("none", _) => Ok((None, None)),
            ("bearer", Some(s)) => Ok((Some(s.expose()), None)),
            ("bearer", None) => Err(ProviderError::Provider(
                "bearer provider called without a credential".to_string(),
            )),
            ("api_key_header", Some(s)) => Ok((
                None,
                Some(("x-api-key".to_string(), s.expose().to_string())),
            )),
            ("api_key_header", None) => Err(ProviderError::Provider(
                "api_key_header provider called without a credential".to_string(),
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

    fn build_messages_body(
        &self,
        slug: &str,
        media_type: &str,
        image_b64: &str,
        schema: Value,
        schema_name: &str,
        system: &str,
        instruction: &str,
        repair: Option<&ProviderError>,
    ) -> Value {
        let mut user_content = vec![
            json!({ "type": "image", "source": {
                "type": "base64", "media_type": media_type, "data": image_b64
            }}),
            json!({ "type": "text", "text": instruction }),
        ];
        if let Some(error) = repair {
            user_content.push(json!({
                "type": "text",
                "text": analyze_schema_repair_instruction(error)
            }));
        }
        let messages = json!([
            { "role": "user", "content": user_content }
        ]);
        json!({
            "model": slug,
            "max_tokens": self.config.limits.max_output_tokens,
            "system": system,
            "temperature": self.config.defaults.temperature,
            "stream": false,
            "messages": messages,
            "tools": [
                { "name": schema_name,
                  "description": "Return the Alcedo image-analysis result as a JSON object matching input_schema.",
                  "input_schema": schema }
            ],
            "tool_choice": { "type": "tool", "name": schema_name }
        })
    }

    fn build_batch_messages_body(
        &self,
        slug: &str,
        images: &[(String, String)],
        schema: Value,
        system: &str,
        instruction: &str,
        repair: Option<BatchRepairContext<'_>>,
    ) -> Value {
        let mut user_content = Vec::new();
        user_content.push(json!({ "type": "text", "text": instruction }));
        for (index, (media_type, image_b64)) in images.iter().enumerate() {
            user_content.push(json!({ "type": "text", "text": format!("Image index {index}:") }));
            user_content.push(json!({ "type": "image", "source": {
                "type": "base64", "media_type": media_type, "data": image_b64
            }}));
        }
        if let Some(repair) = repair {
            user_content.push(json!({
                "type": "text",
                "text": batch_schema_repair_instruction(repair.failures)
            }));
        }
        let messages = json!([
            { "role": "user", "content": user_content }
        ]);
        json!({
            "model": slug,
            "max_tokens": self.config.limits.max_output_tokens,
            "system": system,
            "temperature": self.config.defaults.temperature,
            "stream": false,
            "messages": messages,
            "tools": [
                { "name": ANALYSIS_BATCH_SCHEMA_NAME,
                  "description": "Return Alcedo batch image-analysis results as JSON matching input_schema.",
                  "input_schema": schema }
            ],
            "tool_choice": { "type": "tool", "name": ANALYSIS_BATCH_SCHEMA_NAME }
        })
    }

    /// Driver-owned typed content extraction. First walks `content[]` for a
    /// `tool_use` item whose `name` matches `expected_name` — the native,
    /// reliable Anthropic path. If none is found, falls back to extracting a JSON
    /// object from a `text` content block: many Anthropic-compatible shims
    /// serving non-Claude models (e.g. Qwen on the Volcengine Ark Coding Plan)
    /// accept `tools` / `tool_choice` at the API layer but the model writes the
    /// tool arguments out as a `text` block (often markdown-fenced) instead of a
    /// native `tool_use` block. The text fallback keeps those responses usable
    /// instead of discarding a valid result and burning a paid retry the model
    /// tends to repeat the same way. `thinking` / `reasoning` blocks are never
    /// scanned — they hold draft reasoning, not the final output; the code-owned
    /// `validate_*` still enforces the contract on whatever is extracted.
    ///
    /// Returns an owned `Value` because the text-block path parses JSON that
    /// does not exist as a node in the response body; the `tool_use` path clones
    /// the `input` object for a uniform return type.
    fn extract_tool_input(body: &Value, expected_name: &str) -> Option<Value> {
        let content = body.get("content").and_then(|c| c.as_array())?;
        for item in content {
            if item.get("type").and_then(|t| t.as_str()) == Some("tool_use")
                && item.get("name").and_then(|n| n.as_str()) == Some(expected_name)
            {
                return item.get("input").cloned();
            }
        }
        for item in content {
            if item.get("type").and_then(|t| t.as_str()) == Some("text") {
                if let Some(text) = item.get("text").and_then(|t| t.as_str()) {
                    if let Some(v) = extract_json_from_text_block(text) {
                        return Some(v);
                    }
                }
            }
        }
        None
    }

    fn parse_describe(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<DescribeOutcome, ProviderError> {
        let parsed = Self::extract_tool_input(body, UNDERSTANDING_SCHEMA_NAME).ok_or_else(|| {
            ProviderError::SchemaValidationMessage(format!(
                "provider response did not contain tool_use input named {UNDERSTANDING_SCHEMA_NAME} and no JSON object was found in a text block; expected Anthropic-compatible structured tool output"
            ))
        })?;
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
            // Captured from the response header OR body JSON pointer by the caller
            // (Phase 6b: compatible providers report the id inconsistently).
            provider_request_id: header_req_id.to_string(),
        };
        validate_understanding(&out)?;
        Ok(out)
    }

    fn parse_score(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<ScoreOutcome, ProviderError> {
        let parsed = Self::extract_tool_input(body, RATING_SCHEMA_NAME).ok_or_else(|| {
            ProviderError::SchemaValidationMessage(format!(
                "provider response did not contain tool_use input named {RATING_SCHEMA_NAME} and no JSON object was found in a text block; expected Anthropic-compatible structured tool output"
            ))
        })?;
        // 1..=5 integer star rating. Accept an exact integer or an integer-valued
        // float (e.g. `4.0`); a fractional float (e.g. `4.9`) is NOT truncated —
        // `parse_rating_int` returns None, the rating falls back to 0 (outside the
        // 1..=5 contract), and `validate_rating` rejects it (fail closed).
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

    fn parse_analyze(
        &self,
        body: &Value,
        model_id: &str,
        header_req_id: &str,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        let parsed = Self::extract_tool_input(body, ANALYSIS_FLAT_SCHEMA_NAME).ok_or_else(|| {
            ProviderError::SchemaValidationMessage(format!(
                "provider response did not contain tool_use input named {ANALYSIS_FLAT_SCHEMA_NAME} and no JSON object was found in a text block; expected Anthropic-compatible structured tool output"
            ))
        })?;
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
        let usage = extract_usage(
            self.config
                .response
                .usage_json_pointer
                .as_deref()
                .and_then(|p| json_pointer_str(body, p)),
        );
        let understanding = DescribeOutcome {
            caption: output_description(&parsed),
            tags: output_tags(&parsed),
            scene: output_scene(&parsed),
            confidence: output_confidence(&parsed),
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: header_req_id.to_string(),
        };
        let rating = ScoreOutcome {
            rating: parsed.get("rating").and_then(parse_rating_int).unwrap_or(0),
            rubric_id: output_rubric_id(&parsed),
            rubric_version: output_rubric_version(&parsed),
            reasons: output_rating_reason(&parsed),
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
        let parsed = match Self::extract_tool_input(body, ANALYSIS_BATCH_SCHEMA_NAME) {
            Some(parsed) => parsed,
            None => {
                return Err((
                    Vec::new(),
                    required_indices
                        .iter()
                        .map(|index| BatchItemRepair {
                            index: *index,
                            bad_json: Self::response_content_excerpt(body),
                            error: ProviderError::SchemaValidationMessage(format!(
                                "provider response did not contain tool_use input named {ANALYSIS_BATCH_SCHEMA_NAME} and no JSON object was found in a text block; expected Anthropic-compatible structured tool output"
                            )),
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

    fn response_content_excerpt(body: &Value) -> String {
        sanitized_provider_json_excerpt(body.get("content").unwrap_or(body), 1200)
    }

    /// Build the per-request header set: `anthropic-version` always, then
    /// attribution headers, then the `api_key_header` secret when applicable. The
    /// bearer (when applicable) is returned separately and applied by
    /// `send_with_retry` via `bearer_auth`; it never appears in this vec, so it is
    /// never logged.
    fn request_headers(
        &self,
        extra_auth_header: Option<(String, String)>,
    ) -> Vec<(String, String)> {
        let mut headers = vec![(
            "anthropic-version".to_string(),
            ANTHROPIC_VERSION.to_string(),
        )];
        headers.extend(self.attribution_headers());
        if let Some((k, v)) = extra_auth_header {
            headers.push((k, v));
        }
        headers
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

fn analyze_schema_repair_instruction(error: &ProviderError) -> String {
    format!(
        r#"Your previous tool input did not match the required schema: {error}

Call the tool again with exactly this shape:
{{
  "description": "non-empty string",
  "rating": 1,
  "rating_reason": "string"
}}

The tool input must be a flat object. Do not nest fields under "understanding" or a rating object."#
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
        r#"Some result items from the previous batch tool input did not match the required per-item schema.

Repair only the failed item indexes below. Call the tool again and return a JSON object with a "results" array containing only corrected objects for these failed indexes.

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
impl ImageAnalysisProvider for AnthropicMessagesProvider {
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
        // Real providers advertise via the registry (Phase 5a); this trait method
        // is a compliance fallback for the default model and is not used to
        // advertise the provider to the C++ host.
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
        let (bearer, extra_auth_header) = self.build_auth(credential)?;
        let (media_type, image_b64) = build_image_base64(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::describe(),
        ))?;
        let (system, instruction) = describe_prompt(prompt_profile_id, output_language)?;
        let body = self.build_messages_body(
            &slug,
            &media_type,
            &image_b64,
            schema,
            UNDERSTANDING_SCHEMA_NAME,
            &system,
            &instruction,
            None,
        );
        let headers = self.request_headers(extra_auth_header);
        let resp = send_with_retry(
            &self.http,
            &self.url(),
            &body,
            &headers,
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (resp_headers, resp_body) = read_response(resp).await?;
        let header_req_id = extract_provider_request_id(
            &resp_headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        let provider_content = Self::response_content_excerpt(&resp_body);
        let outcome = match self.parse_describe(&resp_body, &slug, &header_req_id) {
            Ok(outcome) => outcome,
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
                    provider_request_id = %header_req_id,
                    provider_content = %provider_content,
                    error = %err,
                    "DescribeImage provider response parse failed"
                );
                return Err(err);
            }
        };
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            caption = %compact_text_excerpt(&outcome.caption, 240),
            tags = ?outcome.tags,
            provider_content = %provider_content,
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
        let (bearer, extra_auth_header) = self.build_auth(credential)?;
        let (media_type, image_b64) = build_image_base64(image_bytes)?;
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
        let body = self.build_messages_body(
            &slug,
            &media_type,
            &image_b64,
            schema,
            RATING_SCHEMA_NAME,
            &system,
            &instruction,
            None,
        );
        let headers = self.request_headers(extra_auth_header);
        let resp = send_with_retry(
            &self.http,
            &self.url(),
            &body,
            &headers,
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (resp_headers, resp_body) = read_response(resp).await?;
        let header_req_id = extract_provider_request_id(
            &resp_headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        let provider_content = Self::response_content_excerpt(&resp_body);
        let outcome = match self.parse_score(&resp_body, &slug, &header_req_id) {
            Ok(outcome) => outcome,
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
                    provider_request_id = %header_req_id,
                    provider_content = %provider_content,
                    error = %err,
                    "ScoreImage provider response parse failed"
                );
                return Err(err);
            }
        };
        warn!(
            provider = %self.config.provider_id,
            model = %slug,
            provider_request_id = %outcome.provider_request_id,
            rating = outcome.rating,
            reasons = %compact_text_excerpt(&outcome.reasons, 360),
            provider_content = %provider_content,
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
        let (bearer, extra_auth_header) = self.build_auth(credential)?;
        let (media_type, image_b64) = build_image_base64(image_bytes)?;
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
        let headers = self.request_headers(extra_auth_header);
        let mut attempt = 0u32;
        let mut repair: Option<ProviderError> = None;
        let (outcome, provider_content, schema_repair_attempt) = loop {
            let body = self.build_messages_body(
                &slug,
                &media_type,
                &image_b64,
                schema.clone(),
                ANALYSIS_FLAT_SCHEMA_NAME,
                &system,
                &instruction,
                repair.as_ref(),
            );
            let resp = send_with_retry(
                &self.http,
                &self.url(),
                &body,
                &headers,
                bearer,
                MAX_TRANSIENT_RETRIES,
            )
            .await?;
            let (resp_headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &resp_headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = Self::response_content_excerpt(&resp_body);
            match self.parse_analyze(&resp_body, &slug, &header_req_id) {
                Ok(outcome) => break (outcome, provider_content, attempt),
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
                    if attempt >= ANALYZE_SCHEMA_REPAIR_RETRIES {
                        return Err(err);
                    }
                    attempt += 1;
                    repair = Some(err);
                    warn!(
                        provider = %self.config.provider_id,
                        model = %slug,
                        schema_repair_attempt = attempt,
                        "AnalyzeImage retrying after schema validation failure"
                    );
                }
            }
        };
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
        let (bearer, extra_auth_header) = self.build_auth(credential)?;
        let encoded_images = images
            .iter()
            .map(|image| build_image_base64(image.image_bytes))
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
        let headers = self.request_headers(extra_auth_header);
        let mut attempt = 0u32;
        let mut merged: Vec<Option<AnalyzeOutcome>> = vec![None; images.len()];
        let mut required_indices: Vec<usize> = (0..images.len()).collect();
        let mut repair_failures: Vec<BatchItemRepair> = Vec::new();
        loop {
            let repair = (!repair_failures.is_empty()).then_some(BatchRepairContext {
                failures: &repair_failures,
            });
            let body = self.build_batch_messages_body(
                &slug,
                &encoded_images,
                schema.clone(),
                &system,
                &instruction,
                repair,
            );
            let resp = send_with_retry(
                &self.http,
                &self.url(),
                &body,
                &headers,
                bearer,
                MAX_TRANSIENT_RETRIES,
            )
            .await?;
            let (resp_headers, resp_body) = read_response(resp).await?;
            let header_req_id = extract_provider_request_id(
                &resp_headers,
                &resp_body,
                self.config.response.provider_request_id_header.as_deref(),
                self.config
                    .response
                    .provider_request_id_json_pointer
                    .as_deref(),
            );
            let provider_content = Self::response_content_excerpt(&resp_body);
            match self.parse_batch_analyze_items(
                &resp_body,
                &slug,
                &header_req_id,
                images.len(),
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
                            model_id: slug.clone(),
                            usage,
                            provider_request_id,
                        };
                        validate_batch_analyze(&out)?;
                        warn!(
                            provider = %self.config.provider_id,
                            model = %slug,
                            provider_request_id = %out.provider_request_id,
                            items = out.items.len(),
                            provider_content = %provider_content,
                            schema_repair_attempt = attempt,
                            "BatchAnalyzeImage completed"
                        );
                        return Ok(out);
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
                        .filter(|failure| failure.index < images.len())
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
            if attempt >= ANALYZE_SCHEMA_REPAIR_RETRIES || repair_failures.is_empty() {
                return Err(repair_failures
                    .first()
                    .map(|f| f.error.clone())
                    .unwrap_or(ProviderError::SchemaValidation));
            }
            attempt += 1;
            required_indices = repair_failures.iter().map(|f| f.index).collect();
        }
    }

    /// Phase 6c: discover models by listing the configured Anthropic-compatible
    /// `/models` endpoint. Auth follows the config convention (`bearer` ->
    /// `Authorization: Bearer`, `api_key_header` -> `x-api-key`, both with
    /// `anthropic-version`). The Anthropic list shape paginates with
    /// `has_more` + `last_id`; we follow `after_id` pages up to a bounded cap so
    /// a misbehaving server cannot stall discovery. Discovered models are
    /// unverified candidates — no capability flags are inferred.
    async fn list_models(
        &self,
        credential: Option<&SecretString>,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        let (bearer, extra_auth_header) = self.build_auth(credential)?;
        let headers = self.request_headers(extra_auth_header);
        let mut out = Vec::new();
        let mut after_id: Option<String> = None;
        // Bound pagination so a server that always reports has_more=true cannot
        // stall discovery. 50 pages × (typical 100/page) is far beyond any real
        // account's model list.
        for _ in 0..50 {
            let url = match &after_id {
                Some(id) => format!("{}?limit=100&after_id={}", self.models_url(), id),
                None => format!("{}?limit=100", self.models_url()),
            };
            let resp =
                send_get_with_retry(&self.http, &url, &headers, bearer, MAX_TRANSIENT_RETRIES)
                    .await?;
            let (_resp_headers, body) = read_response(resp).await?;
            let page = parse_discovered_models(
                &body,
                &self.config.models_response,
                &self.config.provider_id,
                "Anthropic-compatible",
            )?;
            out.extend(page);
            let has_more = body
                .get("has_more")
                .and_then(|v| v.as_bool())
                .unwrap_or(false);
            if !has_more {
                break;
            }
            // Follow the cursor: prefer the response's `last_id` field (Anthropic
            // shape); fall back to the last item's id. If neither is available
            // (server reports has_more with no items and no cursor), stop to
            // avoid an infinite empty-page loop.
            let next_after = body
                .get("last_id")
                .and_then(|v| v.as_str())
                .map(|s| s.to_string())
                .or_else(|| out.last().map(|m| m.model_id.clone()));
            let Some(next_after) = next_after else {
                break;
            };
            after_id = Some(next_after);
        }
        // Commit discovered candidates so a later explicit `model_id` resolves
        // (see OpenAiChatCompatibleProvider::list_models).
        self.commit_discovered_models(&out);
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::alcedo::ai::image_analysis_service_server::ImageAnalysisService;
    use crate::service::credential_vault::SecretString;
    use crate::service::provider_config::load_provider_configs;
    use std::collections::HashMap;
    use std::sync::Arc;
    use wiremock::matchers::{header, method, path, query_param};
    use wiremock::{Mock, MockServer, ResponseTemplate};

    const TEST_SECRET: &str = "ark-coding-test-key-DO-NOT-LEAK";
    const TEST_PROMPT: &str = "Describe this image for a photo library.";
    const RAW_BODY_SENTINEL: &str = "RAW_ARK_CODING_BODY_SENTINEL";

    fn test_image_png() -> Vec<u8> {
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([10, 20, 30]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        cursor.into_inner()
    }

    fn provider_for(server: &MockServer) -> AnthropicMessagesProvider {
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("volcengine_ark_coding")
            .expect("volcengine_ark_coding built-in")
            .clone();
        config.base_url = server.uri();
        AnthropicMessagesProvider::new(config).expect("provider builds")
    }

    /// Build an Anthropic Messages-shaped success body. `input_json` is the
    /// model's tool arguments, placed in `content[0].tool_use.input` as a parsed
    /// JSON object — the exact JSON object the driver-owned parser walks. Mirrors the
    /// real Anthropic response shape (where `input` is an object, not a string).
    fn ok_messages_body(tool_name: &str, input_json: &str) -> serde_json::Value {
        let input: Value = serde_json::from_str(input_json).expect("input json parses");
        json!({
            "id": "ark-coding-resp-789",
            "type": "message",
            "role": "assistant",
            "content": [
                { "type": "tool_use", "id": "toolu_01", "name": tool_name, "input": input }
            ],
            "usage": { "input_tokens": 90, "output_tokens": 35 }
        })
    }

    fn secret() -> SecretString {
        SecretString::new(TEST_SECRET.to_string())
    }

    #[tokio::test]
    async fn sends_authorization_and_anthropic_version_headers() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .and(header("authorization", format!("Bearer {TEST_SECRET}")))
            .and(header("anthropic-version", ANTHROPIC_VERSION))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
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

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        // bearer mode: x-api-key must NOT be sent.
        assert!(
            reqs[0].headers.get("x-api-key").is_none(),
            "x-api-key present in bearer mode"
        );
    }

    #[tokio::test]
    async fn request_body_uses_messages_shape_with_tool_use() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        provider
            .describe_image(
                &test_image_png(),
                "doubao-seed-2.0-lite",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(body["model"], "doubao-seed-2.0-lite");
        assert_eq!(body["stream"], false);
        // Anthropic Messages requires max_tokens (not max_output_tokens).
        assert_eq!(body["max_tokens"], 1200);
        assert!(body["system"].is_string(), "system must be a string");
        assert_eq!(body["messages"][0]["role"], "user");
        // Anthropic image block: source.type=base64, media_type, raw base64 data.
        let img = &body["messages"][0]["content"][0];
        assert_eq!(img["type"], "image");
        assert_eq!(img["source"]["type"], "base64");
        assert_eq!(img["source"]["media_type"], "image/png");
        let data = img["source"]["data"].as_str().expect("data string");
        assert!(
            !data.starts_with("data:"),
            "image data is a data URI: {data}"
        );
        assert!(!data.is_empty(), "image base64 empty");
        assert_eq!(body["messages"][0]["content"][1]["type"], "text");
        // Structured output via tools + tool_choice, not text.format.json_schema.
        assert_eq!(body["tools"][0]["name"], "alcedo_image_understanding");
        assert_eq!(body["tools"][0]["input_schema"]["type"], "object");
        assert_eq!(
            body["tools"][0]["input_schema"]["additionalProperties"],
            false
        );
        let required = body["tools"][0]["input_schema"]["required"]
            .as_array()
            .expect("required array");
        assert!(
            required.iter().any(|v| v == "description"),
            "description required"
        );
        assert_eq!(body["tool_choice"]["type"], "tool");
        assert_eq!(body["tool_choice"]["name"], "alcedo_image_understanding");
        // No Responses-shape fields leak in.
        assert!(
            body.get("input").is_none(),
            "Responses `input` field present"
        );
        assert!(body.get("text").is_none(), "Responses `text` field present");
        assert!(
            body.get("max_output_tokens").is_none(),
            "Responses `max_output_tokens` present"
        );
    }

    #[tokio::test]
    async fn extracts_tool_use_input_from_messages_tool_use_input() {
        let server = MockServer::start().await;
        // The content array may hold a text block before the tool_use block; the
        // walker must skip the text and find the right tool_use input.
        let body = json!({
            "id": "ark-coding-resp-100",
            "type": "message",
            "role": "assistant",
            "content": [
                { "type": "text", "text": "thinking about the image..." },
                { "type": "tool_use", "id": "toolu_02", "name": UNDERSTANDING_SCHEMA_NAME,
                  "input": { "caption": "sunrise", "tags": ["sun", "sky"], "scene": "outdoor", "confidence": 0.8 } }
            ],
            "usage": { "input_tokens": 50, "output_tokens": 20 }
        });
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "sunrise");
        assert_eq!(out.tags, vec!["sun".to_string(), "sky".to_string()]);
        assert_eq!(out.scene, "outdoor");
        assert!((out.confidence - 0.8).abs() < 1e-9);
        assert_eq!(out.usage.input_tokens, 50);
        assert_eq!(out.usage.output_tokens, 20);
        assert_eq!(out.provider_request_id, "ark-coding-resp-100");
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[test]
    fn response_content_excerpt_omits_thinking_blocks() {
        let body = json!({
            "content": [
                {
                    "type": "thinking",
                    "thinking": "private provider reasoning",
                    "signature": "reasoning-signature"
                },
                {
                    "type": "tool_use",
                    "id": "toolu_02",
                    "name": UNDERSTANDING_SCHEMA_NAME,
                    "input": {
                        "caption": "sunrise",
                        "tags": ["sun", "sky"],
                        "scene": "outdoor",
                        "confidence": 0.8
                    }
                }
            ]
        });

        let excerpt = AnthropicMessagesProvider::response_content_excerpt(&body);
        assert!(!excerpt.contains("private provider reasoning"), "{excerpt}");
        assert!(!excerpt.contains("reasoning-signature"), "{excerpt}");
        assert!(excerpt.contains("provider reasoning omitted"), "{excerpt}");
        assert!(excerpt.contains(UNDERSTANDING_SCHEMA_NAME), "{excerpt}");
        assert!(excerpt.contains("sunrise"), "{excerpt}");
    }

    #[tokio::test]
    async fn parses_understanding_response_and_captures_usage() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
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
        assert_eq!(out.scene, "studio");
        assert!((out.confidence - 0.7).abs() < 1e-9);
        // Anthropic usage has input/output tokens but no total_tokens.
        assert_eq!(out.usage.input_tokens, 90);
        assert_eq!(out.usage.output_tokens, 35);
        assert_eq!(out.usage.total_tokens, 0);
        assert_eq!(out.provider_request_id, "ark-coding-resp-789");
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[tokio::test]
    async fn parses_rating_response_and_captures_usage() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                RATING_SCHEMA_NAME,
                r#"{"rating":4,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"r"}"#,
            )))
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
        // Single 1..=5 integer star rating; no scores array, no confidence.
        assert_eq!(out.rating, 4);
        assert_eq!(out.rubric_id, "alcedo-default-v1");
        assert_eq!(out.rubric_version, "1");
        assert_eq!(out.reasons, "r");
        assert_eq!(out.usage.input_tokens, 90);
        validate_rating(&out).expect("canned rating validates");
    }

    #[tokio::test]
    async fn parses_rating_rejects_fractional_float() {
        // A fractional rating (4.9) is schema-invalid and must NOT be truncated
        // to 4 — fail closed: the parser yields 0 and validate_rating maps it to
        // SchemaValidation (no active annotation).
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                RATING_SCHEMA_NAME,
                r#"{"rating":4.9,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"x"}"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
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
            .expect_err("fractional rating rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn analyze_accepts_flat_tool_output() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_FLAT_SCHEMA_NAME,
                r#"{
                    "caption": "a macaque on a branch",
                    "tags": ["wildlife", "monkey"],
                    "scene": "rainforest",
                    "confidence": 0.85,
                    "rating": 3,
                    "rubric_id": "general",
                    "rubric_version": "v1",
                    "reasons": "competent but ordinary"
                }"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("flat analysis output accepted");
        let understanding = out.understanding.expect("understanding present");
        let rating = out.rating.expect("rating present");
        assert_eq!(understanding.caption, "a macaque on a branch");
        assert_eq!(rating.rating, 3);
        assert_eq!(rating.rubric_id, "general");
        assert_eq!(rating.rubric_version, "v1");
        assert_eq!(rating.reasons, "competent but ordinary");
        validate_analyze(&AnalyzeOutcome {
            understanding: Some(understanding),
            rating: Some(rating),
            model_id: out.model_id,
            usage: out.usage,
            provider_request_id: out.provider_request_id,
        })
        .expect("flat bundle validates after mapping");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        let schema = &body["tools"][0]["input_schema"];
        assert!(schema["properties"].get("description").is_some());
        assert!(schema["properties"].get("rating").is_some());
        assert!(schema["properties"].get("rating_reason").is_some());
        assert!(schema["properties"].get("tags").is_none());
        assert!(schema["properties"].get("rubric_id").is_none());
        assert!(schema["properties"].get("understanding").is_none());
    }

    #[tokio::test]
    async fn analyze_repairs_missing_description_once() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_FLAT_SCHEMA_NAME,
                r#"{
                    "rating": 3,
                    "rating_reason": "competent but ordinary"
                }"#,
            )))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_FLAT_SCHEMA_NAME,
                r#"{
                    "description": "a macaque on a branch",
                    "rating": 3,
                    "rating_reason": "competent but ordinary"
                }"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .analyze_image(
                &test_image_png(),
                "",
                "",
                "general",
                "",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("missing description repaired");
        assert_eq!(
            out.understanding.expect("understanding present").caption,
            "a macaque on a branch"
        );

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 2);
        let second_body: Value = serde_json::from_slice(&reqs[1].body).expect("body json");
        let messages = second_body["messages"].as_array().expect("messages array");
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0]["role"], "user");
        let original_instruction = messages[0]["content"][1]["text"]
            .as_str()
            .expect("original instruction string");
        assert!(
            !original_instruction.contains("tags must be an array, not a string"),
            "{original_instruction}"
        );
        let repair_instruction = messages[0]["content"][2]["text"]
            .as_str()
            .expect("repair instruction string");
        assert!(
            repair_instruction.contains("\"description\""),
            "{repair_instruction}"
        );
        assert_eq!(
            second_body["tool_choice"]["name"], ANALYSIS_FLAT_SCHEMA_NAME,
            "repair request still forces the Anthropic tool"
        );
        assert!(
            !messages.iter().any(|m| m["role"] == "assistant"),
            "repair request must not fake an assistant turn without tool_result"
        );
    }

    #[tokio::test]
    async fn batch_analyze_repair_request_keeps_single_tool_call_turn() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_BATCH_SCHEMA_NAME,
                r#"{
                    "results": [
                        {
                            "index": 0,
                            "rating": 3,
                            "rating_reason": "competent but ordinary"
                        }
                    ]
                }"#,
            )))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_BATCH_SCHEMA_NAME,
                r#"{
                    "results": [
                        {
                            "index": 0,
                            "description": "a red tram",
                            "rating": 3,
                            "rating_reason": "competent but ordinary"
                        }
                    ]
                }"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let img = test_image_png();
        let input = AnalyzeImageInput {
            image_bytes: &img,
            camera_context: "",
        };
        let out = provider
            .batch_analyze_images(
                &[input],
                "",
                "",
                "general",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("batch item repaired");
        assert_eq!(out.items.len(), 1);
        assert_eq!(
            out.items[0]
                .understanding
                .as_ref()
                .expect("understanding present")
                .caption,
            "a red tram"
        );

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 2);
        let second_body: Value = serde_json::from_slice(&reqs[1].body).expect("body json");
        let messages = second_body["messages"].as_array().expect("messages array");
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0]["role"], "user");
        assert!(
            !messages.iter().any(|m| m["role"] == "assistant"),
            "batch repair request must not fake an assistant turn without tool_result"
        );
        let content = messages[0]["content"].as_array().expect("content array");
        assert!(
            content.iter().any(|c| c["type"] == "text"
                && c["text"]
                    .as_str()
                    .unwrap_or_default()
                    .contains("Repair only the failed item indexes")),
            "batch repair instruction missing from single user turn: {content:?}"
        );
        assert_eq!(
            second_body["tool_choice"]["name"], ANALYSIS_BATCH_SCHEMA_NAME,
            "batch repair request still forces the Anthropic batch tool"
        );
    }

    #[tokio::test]
    async fn rate_limit_maps_to_transient() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
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
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(500))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
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
        // A 4xx with a structured Anthropic-style error body is not retried and
        // maps to ProviderError::Provider. The raw provider error text must NOT
        // surface in the ProviderError string (the service drops it before
        // placement; here we assert the driver itself does not echo the raw body).
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(400).set_body_json(json!({
                "type": "error",
                "error": { "type": "invalid_request_error", "message": RAW_BODY_SENTINEL }
            })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("400 not retried");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body leaked: {err}"
        );
        assert!(
            !err.to_string().contains("invalid_request_error"),
            "error type leaked: {err}"
        );
    }

    #[tokio::test]
    async fn schema_failure_does_not_produce_active_result() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"description":""}"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("empty description rejected");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn missing_tool_use_maps_to_schema_validation() {
        let server = MockServer::start().await;
        // 200 but only a text block (no tool_use) -> driver-owned parser returns None.
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "id": "ark-coding-resp-x",
                "type": "message",
                "role": "assistant",
                "content": [ { "type": "text", "text": "I cannot use a tool here." } ]
            })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("no tool_use");
        match err {
            ProviderError::SchemaValidationMessage(message) => {
                assert!(
                    message.contains("tool_use input named alcedo_image_understanding"),
                    "{message}"
                );
            }
            other => panic!("expected detailed schema validation message, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn wrong_tool_name_maps_to_schema_validation() {
        let server = MockServer::start().await;
        // 200 with a tool_use for a DIFFERENT tool name -> parser returns None.
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                "some_other_tool",
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("wrong tool name");
        match err {
            ProviderError::SchemaValidationMessage(message) => {
                assert!(
                    message.contains("tool_use input named alcedo_image_understanding"),
                    "{message}"
                );
            }
            other => panic!("expected detailed schema validation message, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn describe_accepts_text_block_json_without_tool_use() {
        // Real-world Volcengine Ark Coding Plan + Qwen behavior: the shim honors
        // `tool_choice` at the API layer but the model emits the JSON as a
        // markdown-fenced `text` block (after a `thinking` block) instead of a
        // native `tool_use` block. The driver must accept the text-block JSON on
        // the FIRST attempt instead of discarding a valid result and burning a
        // paid repair retry.
        let server = MockServer::start().await;
        let body = json!({
            "id": "ark-coding-resp-text",
            "type": "message",
            "role": "assistant",
            "content": [
                { "type": "thinking", "thinking": "draft reasoning that must be ignored", "signature": "sig" },
                { "type": "text", "text":
                    "```json\n{\n  \"caption\": \"a red tram at dusk\",\n  \"tags\": [\"tram\", \"dusk\"],\n  \"scene\": \"urban\",\n  \"confidence\": 0.86\n}\n```" }
            ],
            "usage": { "input_tokens": 70, "output_tokens": 30 }
        });
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("text-block JSON accepted on first attempt");
        assert_eq!(out.caption, "a red tram at dusk");
        assert_eq!(out.tags, vec!["tram".to_string(), "dusk".to_string()]);
        assert_eq!(out.scene, "urban");
        assert!((out.confidence - 0.86).abs() < 1e-9);
        validate_understanding(&out).expect("text-block outcome validates");
        // Crucially, only ONE provider call was made — no paid repair retry.
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn describe_accepts_raw_json_text_block_without_fences() {
        // Same fallback, but the model emits raw JSON (no markdown fence).
        let server = MockServer::start().await;
        let body = json!({
            "id": "ark-coding-resp-raw",
            "type": "message",
            "role": "assistant",
            "content": [
                { "type": "text", "text":
                    "{\"caption\":\"sunrise\",\"tags\":[\"sun\",\"sky\"],\"scene\":\"outdoor\",\"confidence\":0.8}" }
            ],
            "usage": { "input_tokens": 60, "output_tokens": 25 }
        });
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("raw text-block JSON accepted");
        assert_eq!(out.caption, "sunrise");
        assert_eq!(out.tags, vec!["sun".to_string(), "sky".to_string()]);
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn batch_analyze_accepts_text_block_json_without_tool_use() {
        // The exact failure pattern from the production log: a `thinking` block
        // followed by a `text` block holding the fenced `{"results":[...]}` JSON,
        // with NO `tool_use` block. Previously this failed with
        // "did not contain tool_use input named alcedo_image_analysis_batch"
        // and triggered a repair retry; now it succeeds on the first attempt.
        let server = MockServer::start().await;
        let body = json!({
            "id": "ark-coding-resp-batch-text",
            "type": "message",
            "role": "assistant",
            "content": [
                { "type": "thinking", "thinking": "draft reasoning that must be ignored", "signature": "sig" },
                { "type": "text", "text":
                    "```json\n{\n  \"results\": [\n    {\n      \"index\": 0,\n      \"caption\": \"a red tram\",\n      \"tags\": [\"tram\", \"city\"],\n      \"scene\": \"urban\",\n      \"confidence\": 0.8,\n      \"rating\": 3,\n      \"rubric_id\": \"general\",\n      \"rubric_version\": \"v1\",\n      \"reasons\": \"competent but ordinary\"\n    }\n  ]\n}\n```" }
            ],
            "usage": { "input_tokens": 90, "output_tokens": 40 }
        });
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let img = test_image_png();
        let input = AnalyzeImageInput {
            image_bytes: &img,
            camera_context: "",
        };
        let out = provider
            .batch_analyze_images(
                &[input],
                "",
                "",
                "general",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect("text-block batch JSON accepted on first attempt");
        assert_eq!(out.items.len(), 1);
        let understanding = out.items[0].understanding.as_ref().expect("understanding");
        assert_eq!(understanding.caption, "a red tram");
        assert_eq!(
            understanding.tags,
            vec!["tram".to_string(), "city".to_string()]
        );
        let rating = out.items[0].rating.as_ref().expect("rating");
        assert_eq!(rating.rating, 3);
        assert_eq!(rating.rubric_id, "general");
        // No paid repair retry: exactly one provider call.
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
    }

    #[tokio::test]
    async fn bearer_required_without_credential_errors() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
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
    async fn api_key_header_mode_sends_x_api_key() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .and(header("x-api-key", TEST_SECRET))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
            .mount(&server)
            .await;
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("volcengine_ark_coding")
            .expect("coding built-in")
            .clone();
        config.base_url = server.uri();
        config.auth.auth_type = "api_key_header".to_string();
        let provider = AnthropicMessagesProvider::new(config).expect("provider builds");

        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "c");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        assert_eq!(
            reqs[0].headers.get("x-api-key").unwrap().to_str().unwrap(),
            TEST_SECRET,
            "x-api-key header value"
        );
        assert!(
            reqs[0].headers.get("authorization").is_none(),
            "authorization present in api_key_header mode"
        );
    }

    #[tokio::test]
    async fn schema_repair_error_string_does_not_leak_secret_image_prompt_or_body() {
        let img = test_image_png();
        let (_, img_b64) = build_image_base64(&img).expect("encode image");
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                ANALYSIS_FLAT_SCHEMA_NAME,
                r#"{
                    "description": "",
                    "rating": 3,
                    "rating_reason": "RAW_ARK_CODING_BODY_SENTINEL"
                }"#,
            )))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .analyze_image(
                &img,
                "",
                "profile-1",
                "general",
                "",
                "",
                "",
                true,
                true,
                true,
                Some(&secret()),
            )
            .await
            .expect_err("schema repair budget exhausted");
        let err = err.to_string();
        assert!(!err.contains(TEST_SECRET), "secret in error: {err}");
        assert!(
            !err.contains(img_b64.as_str()),
            "image base64 in error: {err}"
        );
        assert!(!err.contains(TEST_PROMPT), "prompt in error: {err}");
        assert!(!err.contains(RAW_BODY_SENTINEL), "raw body in error: {err}");
        assert_eq!(server.received_requests().await.unwrap().len(), 2);
    }

    #[tokio::test]
    async fn cancellation_drops_in_flight_request() {
        use crate::proto::alcedo::ai::{
            AiErrorCode, AiPriority, AiRequestHeader, AiResponseStatus, DescribeImageRequest,
            RenditionMetadata as ProtoRendition,
        };
        use crate::server::image_analysis::ImageAnalysisServiceImpl;
        use crate::service::cancellation_registry::CancellationRegistry;
        use crate::service::credential_vault::CredentialVault;

        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_messages_body(
                        UNDERSTANDING_SCHEMA_NAME,
                        r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                    ))
                    .set_delay(std::time::Duration::from_secs(2)),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let vault = Arc::new(CredentialVault::new(None));
        let cancel_registry = Arc::new(CancellationRegistry::new());
        let mut providers: HashMap<String, Arc<dyn ImageAnalysisProvider>> = HashMap::new();
        let pid = provider.provider_id().to_string();
        providers.insert(pid.clone(), Arc::new(provider));
        let svc =
            ImageAnalysisServiceImpl::new(providers, pid, vault.clone(), cancel_registry.clone());

        let handle = vault.register("volcengine_ark_coding", TEST_SECRET.to_string(), None);
        let request_id = "req-cancel-ark-coding".to_string();
        let req = DescribeImageRequest {
            header: Some(AiRequestHeader {
                request_id: request_id.clone(),
                task_id: "image_understanding.describe".to_string(),
                timeout_ms: 60_000,
                priority: AiPriority::Normal as i32,
                trace_id: String::new(),
                credential_ref: handle,
                client_capabilities: vec![],
            }),
            image_bytes: test_image_png(),
            image_format_hint: "image/png".to_string(),
            rendition: Some(ProtoRendition {
                kind: "preview".to_string(),
                width: 2,
                height: 2,
                bytes: 64,
            }),
            provider_id: "volcengine_ark_coding".to_string(),
            model_id: String::new(),
            prompt_profile_id: String::new(),
            output_language: String::new(),
        };

        let cancel_registry2 = cancel_registry.clone();
        let rid = request_id.clone();
        tokio::spawn(async move {
            tokio::time::sleep(std::time::Duration::from_millis(50)).await;
            cancel_registry2.cancel(&rid);
        });

        let resp = svc
            .describe_image(tonic::Request::new(req))
            .await
            .expect("rpc ok");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusCancelled as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorCancelledByClient as i32);
        assert!(inner.result.is_none());
    }

    #[tokio::test]
    async fn timeout_returns_deadline_exceeded() {
        use crate::proto::alcedo::ai::{
            AiErrorCode, AiPriority, AiRequestHeader, AiResponseStatus, DescribeImageRequest,
            RenditionMetadata as ProtoRendition,
        };
        use crate::server::image_analysis::ImageAnalysisServiceImpl;
        use crate::service::cancellation_registry::CancellationRegistry;
        use crate::service::credential_vault::CredentialVault;

        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/v1/messages"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_messages_body(
                        UNDERSTANDING_SCHEMA_NAME,
                        r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
                    ))
                    .set_delay(std::time::Duration::from_secs(2)),
            )
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        let vault = Arc::new(CredentialVault::new(None));
        let cancel_registry = Arc::new(CancellationRegistry::new());
        let mut providers: HashMap<String, Arc<dyn ImageAnalysisProvider>> = HashMap::new();
        let pid = provider.provider_id().to_string();
        providers.insert(pid.clone(), Arc::new(provider));
        let svc =
            ImageAnalysisServiceImpl::new(providers, pid, vault.clone(), cancel_registry.clone());

        let handle = vault.register("volcengine_ark_coding", TEST_SECRET.to_string(), None);
        let req = DescribeImageRequest {
            header: Some(AiRequestHeader {
                request_id: "req-timeout-ark-coding".into(),
                task_id: "image_understanding.describe".into(),
                timeout_ms: 80,
                priority: AiPriority::Normal as i32,
                trace_id: String::new(),
                credential_ref: handle,
                client_capabilities: vec![],
            }),
            image_bytes: test_image_png(),
            image_format_hint: "image/png".to_string(),
            rendition: Some(ProtoRendition {
                kind: "preview".to_string(),
                width: 2,
                height: 2,
                bytes: 64,
            }),
            provider_id: "volcengine_ark_coding".to_string(),
            model_id: String::new(),
            prompt_profile_id: String::new(),
            output_language: String::new(),
        };

        let resp = svc
            .describe_image(tonic::Request::new(req))
            .await
            .expect("rpc ok (timeout in header)");
        let inner = resp.into_inner();
        let h = inner.header.expect("header present");
        assert_eq!(h.status, AiResponseStatus::AiStatusDeadlineExceeded as i32);
        assert_eq!(h.error_code, AiErrorCode::AiErrorProviderTimeout as i32);
        assert!(inner.result.is_none());
    }

    // --- Phase 6b: Opencode-compatible base URL / endpoint --------------------
    //
    // The driver is generic Anthropic Messages — these tests prove it accepts an
    // Opencode-compatible base URL + endpoint (`https://opencode.ai/zen/go/v1` +
    // `/messages`) from config, captures the provider request id from the
    // `request-id` response header (Opencode echoes a header, not a body `id`),
    // and stays fail-closed when the endpoint ignores tool-use. The Coding Plan
    // tests above cover the body-`id` path; these cover the header path.

    fn opencode_provider_for(server: &MockServer) -> AnthropicMessagesProvider {
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("opencode_go_anthropic")
            .expect("opencode_go_anthropic built-in")
            .clone();
        config.base_url = server.uri();
        // OpenCode built-ins are already structured-output capable; these tests
        // exercise the wire shape against a mock endpoint.
        AnthropicMessagesProvider::new(config).expect("provider builds")
    }

    #[tokio::test]
    async fn opencode_base_url_builds_messages_request_with_tool_use() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/messages"))
            .and(header("x-api-key", TEST_SECRET))
            .and(header("anthropic-version", ANTHROPIC_VERSION))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_messages_body(
                UNDERSTANDING_SCHEMA_NAME,
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
            .mount(&server)
            .await;

        let provider = opencode_provider_for(&server);
        provider
            .describe_image(&test_image_png(), "qwen3.7-plus", "", "", Some(&secret()))
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        // The Opencode endpoint is `/messages` (not the Coding Plan `/v1/messages`).
        assert_eq!(reqs[0].url.path(), "/messages");
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(body["model"], "qwen3.7-plus");
        // Anthropic image block: source.type=base64, media_type, raw base64 data.
        let img = &body["messages"][0]["content"][0];
        assert_eq!(img["type"], "image");
        assert_eq!(img["source"]["type"], "base64");
        assert_eq!(img["source"]["media_type"], "image/png");
        let data = img["source"]["data"].as_str().expect("data string");
        assert!(
            !data.starts_with("data:"),
            "image data is a data URI: {data}"
        );
        // Structured output via tools + tool_choice, not response_format.
        assert_eq!(body["tools"][0]["name"], "alcedo_image_understanding");
        assert_eq!(body["tools"][0]["input_schema"]["type"], "object");
        assert_eq!(
            body["tools"][0]["input_schema"]["additionalProperties"],
            false
        );
        assert_eq!(body["tool_choice"]["type"], "tool");
        assert_eq!(body["tool_choice"]["name"], "alcedo_image_understanding");
        // No OpenRouter / OpenAI-Chat fields leak in.
        assert!(
            body.get("provider").is_none(),
            "OpenRouter `provider` object present"
        );
        assert!(
            body.get("response_format").is_none(),
            "OpenAI `response_format` present"
        );
        // OpenCode's Anthropic-compatible /messages endpoint expects Anthropic
        // x-api-key auth; bearer returns a fast 401 "Missing API key".
        assert!(
            reqs[0].headers.get("authorization").is_none(),
            "authorization present in api_key_header mode"
        );
    }

    #[tokio::test]
    async fn opencode_extracts_tool_use_and_captures_request_id_header() {
        // Opencode echoes the provider request id as a `request-id` response header
        // (the config sets `provider_request_id_header: "request-id"`,
        // `provider_request_id_json_pointer: null`). The driver must capture it
        // from the header, not the body.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/messages"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_messages_body(
                        UNDERSTANDING_SCHEMA_NAME,
                        r#"{"caption":"a small image","tags":["test"],"scene":"studio","confidence":0.7}"#,
                    ))
                    .insert_header("request-id", "opencode-hdr-42"),
            )
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);
        let out = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect("describe ok");
        assert_eq!(out.caption, "a small image");
        assert_eq!(out.tags, vec!["test".to_string()]);
        // Captured from the `request-id` header, not a body field.
        assert_eq!(out.provider_request_id, "opencode-hdr-42");
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[tokio::test]
    async fn opencode_missing_tool_use_maps_to_schema_validation() {
        // Explicit "unsupported structured output" fail-closed path on the Opencode
        // endpoint: a 200 that returns only a text block (endpoint ignored
        // tool_choice) must not create an active annotation.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/messages"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "id": "opencode-resp-x",
                "type": "message",
                "role": "assistant",
                "content": [ { "type": "text", "text": "I cannot use a tool here." } ]
            })))
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("no tool_use");
        match err {
            ProviderError::SchemaValidationMessage(message) => {
                assert!(
                    message.contains("tool_use input named alcedo_image_understanding"),
                    "{message}"
                );
            }
            other => panic!("expected detailed schema validation message, got {other:?}"),
        }
    }

    #[tokio::test]
    async fn opencode_client_4xx_redacts_raw_body() {
        // A 4xx on the Opencode endpoint is not retried, maps to
        // ProviderError::Provider, and the raw provider error body must NOT surface
        // in the error string (redaction). The full log-capture redaction test
        // (`no_secret_image_prompt_or_body_in_logs_or_error_strings`) covers the
        // shared `send_with_retry` path with the Coding Plan config; this asserts
        // the Opencode path likewise does not leak the raw body in the error.
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/messages"))
            .respond_with(ResponseTemplate::new(400).set_body_json(json!({
                "type": "error",
                "error": { "type": "invalid_request_error", "message": RAW_BODY_SENTINEL }
            })))
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("400 not retried");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body leaked: {err}"
        );
        assert!(
            !err.to_string().contains(TEST_SECRET),
            "secret in error: {err}"
        );
    }

    // ----- Phase 6c: model_id tightening + model discovery -----

    #[tokio::test]
    async fn unknown_explicit_model_id_fails_before_http_anthropic() {
        let server = MockServer::start().await;
        let provider = opencode_provider_for(&server);
        let err = provider
            .describe_image(
                &test_image_png(),
                "not-a-real-anthropic-model",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect_err("unknown model id should fail closed");
        assert_eq!(
            err,
            ProviderError::UnknownModel("not-a-real-anthropic-model".to_string())
        );
        assert_eq!(server.received_requests().await.unwrap().len(), 0);
    }

    /// Phase 6c: Anthropic-compatible discovery follows `has_more` + `last_id`
    /// pagination and merges all pages into unverified candidates.
    #[tokio::test]
    async fn list_models_anthropic_opencode_x_api_key_parses_and_paginates() {
        let server = MockServer::start().await;
        // Page 1 (no after_id): has_more=true, last_id=claude-a.
        Mock::given(method("GET"))
            .and(path("/models"))
            .and(header("x-api-key", TEST_SECRET))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "data": [ { "id": "claude-a", "display_name": "Claude A" } ],
                "has_more": true,
                "last_id": "claude-a",
                "first_id": "claude-a"
            })))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        // Page 2 (after_id=claude-a): has_more=false.
        Mock::given(method("GET"))
            .and(path("/models"))
            .and(query_param("after_id", "claude-a"))
            .and(header("x-api-key", TEST_SECRET))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "data": [ { "id": "claude-b" } ],
                "has_more": false,
                "last_id": "claude-b"
            })))
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);

        let models = provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok");
        assert_eq!(models.len(), 2);
        assert_eq!(models[0].model_id, "claude-a");
        assert_eq!(models[0].display_name, "Claude A");
        assert_eq!(models[1].model_id, "claude-b");
        assert_eq!(models[1].display_name, "claude-b"); // falls back to id
        assert_eq!(models[0].source_provider_id, "opencode_go_anthropic");
        // Two GETs, second carrying the cursor.
        let reqs = server.received_requests().await.unwrap();
        assert_eq!(reqs.len(), 2);
        assert_eq!(reqs[0].method, "GET");
        assert_eq!(reqs[1].method, "GET");
        assert!(reqs[0].headers.get("authorization").is_none());
        assert!(reqs[1].headers.get("authorization").is_none());
    }

    /// A malformed model-list body stays a provider payload-decode failure, but
    /// includes the sanitized body shape so local routers can be diagnosed from
    /// the app logs instead of a generic schema-validation string.
    #[tokio::test]
    async fn list_models_anthropic_non_data_body_reports_response_excerpt() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "models": "oops",
                "api_key": TEST_SECRET
            })))
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);

        let err = provider
            .list_models(Some(&secret()))
            .await
            .expect_err("bad shape");
        let ProviderError::SchemaValidationMessage(message) = err else {
            panic!("expected schema validation detail, got {err:?}");
        };
        assert!(message.contains("Anthropic-compatible model list"));
        assert!(message.contains("\"data\""));
        assert!(message.contains("\"models\":\"oops\""));
        assert!(
            !message.contains(TEST_SECRET),
            "secret leaked in diagnostic: {message}"
        );
    }

    #[tokio::test]
    async fn list_models_anthropic_uses_configured_models_response_path_for_ccswitch_shape() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/v1/models"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "models": [
                    "route-a",
                    { "id": "route-b", "name": "Route B" }
                ],
                "has_more": false
            })))
            .mount(&server)
            .await;
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("ccswitch_anthropic")
            .expect("ccswitch_anthropic built-in")
            .clone();
        config.base_url = server.uri();
        let provider = AnthropicMessagesProvider::new(config).expect("provider builds");

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

    /// Phase 6c: `api_key_header` mode sends `x-api-key` + `anthropic-version`
    /// and NO `Authorization` for discovery, mirroring the task-call auth path.
    #[tokio::test]
    async fn list_models_anthropic_x_api_key_auth() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/v1/models"))
            .and(header("x-api-key", TEST_SECRET))
            .and(header("anthropic-version", "2023-06-01"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "data": [ { "id": "claude-a" } ],
                "has_more": false
            })))
            .mount(&server)
            .await;
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("volcengine_ark_coding")
            .expect("coding built-in")
            .clone();
        config.base_url = server.uri();
        config.endpoint = "/v1/messages".to_string();
        config.models_endpoint = Some("/v1/models".to_string());
        config.auth.auth_type = "api_key_header".to_string();
        let provider = AnthropicMessagesProvider::new(config).expect("provider builds");

        let models = provider
            .list_models(Some(&secret()))
            .await
            .expect("list ok");
        assert_eq!(models.len(), 1);
        let reqs = server.received_requests().await.unwrap();
        assert_eq!(reqs.len(), 1);
        assert!(
            reqs[0].headers.get("authorization").is_none(),
            "Authorization present in x-api-key mode"
        );
    }

    /// Phase 6c: a 401 on the model-list maps to a non-retryable `Provider`
    /// error; the raw body and secret are not leaked.
    #[tokio::test]
    async fn list_models_anthropic_4xx_redacts() {
        let server = MockServer::start().await;
        Mock::given(method("GET"))
            .and(path("/models"))
            .respond_with(ResponseTemplate::new(401).set_body_string(RAW_BODY_SENTINEL))
            .mount(&server)
            .await;
        let provider = opencode_provider_for(&server);

        let err = provider
            .list_models(Some(&secret()))
            .await
            .expect_err("401 fails");
        assert!(matches!(err, ProviderError::Provider(_)), "{err:?}");
        assert_eq!(server.received_requests().await.unwrap().len(), 1);
        assert!(
            !err.to_string().contains(TEST_SECRET),
            "secret in error: {err}"
        );
        assert!(
            !err.to_string().contains(RAW_BODY_SENTINEL),
            "raw body leaked: {err}"
        );
    }
}
