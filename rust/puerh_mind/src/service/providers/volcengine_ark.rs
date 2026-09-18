//! Volcengine Ark Responses image-analysis driver (Phase 5c,
//! `volcengine_ark_responses`).
//!
//! Calls the Ark `/api/v3/responses` endpoint (OpenAI Responses-compatible): an
//! Alcedo-owned system prompt, a user `input` message carrying the image as an
//! `input_image` data URI plus an `input_text` task instruction, and
//! `text.format = json_schema` with the code-owned (sanitized to strict-compatible)
//! Alcedo schema. The API key is resolved from the Rust credential vault and sent
//! only as `Authorization: Bearer <secret>`. The response's
//! `output[].content[].output_text` is extracted with a driver-owned typed parser
//! (`content_json_pointer` is null for this driver), parsed as JSON, validated +
//! normalized against the code-owned understanding / rating contract, and returned
//! as typed fields. Transient / 429 / 5xx failures retry under the same bounded
//! policy as the OpenRouter driver; provider and transport errors map to
//! `ProviderError` variants the service translates into `AiResponseStatus` /
//! `AiErrorCode`. No `thinking`, `provider.require_parameters`, or
//! `data_collection` knobs are sent (the Ark model does not declare them; `thinking`
//! is intentionally omitted for the MVP and may be tuned after the live smoke).
//!
//! The request/response shape is kept compatible with the OpenAI Responses API
//! types that Ark mirrors (`input` array with `input_image` / `input_text` content
//! items, `text.format` for structured output, `max_output_tokens`), even though
//! the production Rust path calls the REST API directly with `reqwest`.

use serde_json::{Value, json};
use tracing::warn;

use crate::service::credential_vault::SecretString;
use crate::service::image_analysis::{
    DescribeOutcome, ImageAnalysisProvider, ImageAnalysisSchemaSpec, ProviderError, ScoreOutcome,
    image_analysis_schema_json, output_confidence, output_description, output_rating_reason,
    output_rubric_id, output_rubric_version, output_scene, output_tags, validate_rating,
    validate_understanding,
};
use crate::service::provider_config::{ModelConfig, ProviderConfig};
use crate::service::providers::http_util::{
    MAX_TRANSIENT_RETRIES, build_image_data_uri, build_rustls_client, compact_text_excerpt,
    extract_usage, json_pointer_str, parse_content_json, parse_rating_int, read_response,
    sanitized_provider_json_excerpt, send_with_retry, strict_schema_value,
};

const UNDERSTANDING_SCHEMA_NAME: &str = "alcedo_image_understanding";
const RATING_SCHEMA_NAME: &str = "alcedo_image_rating";

pub struct VolcengineArkResponsesProvider {
    config: ProviderConfig,
    http: reqwest::Client,
}

impl VolcengineArkResponsesProvider {
    pub fn new(config: ProviderConfig) -> Result<Self, ProviderError> {
        let http = build_rustls_client()?;
        Ok(Self { config, http })
    }

    #[allow(dead_code)]
    pub fn with_client(config: ProviderConfig, http: reqwest::Client) -> Self {
        Self { config, http }
    }

    fn url(&self) -> String {
        format!("{}{}", self.config.base_url, self.config.endpoint)
    }

    fn resolve_model<'a>(
        &'a self,
        requested: &str,
    ) -> Result<(String, Option<&'a ModelConfig>), ProviderError> {
        if requested.trim().is_empty() {
            let slug = self.config.defaults.model.clone();
            let entry = self.config.models.iter().find(|m| m.slug == slug);
            return Ok((slug, entry));
        }
        match self.config.models.iter().find(|m| m.slug == requested) {
            Some(m) => Ok((m.slug.clone(), Some(m))),
            None => Err(ProviderError::UnknownModel(requested.to_string())),
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

    fn build_responses_body(
        &self,
        slug: &str,
        data_uri: &str,
        schema: Value,
        schema_name: &str,
        system: &str,
        instruction: &str,
    ) -> Value {
        json!({
            "model": slug,
            "input": [
                { "role": "system", "content": [ { "type": "input_text", "text": system } ] },
                { "role": "user", "content": [
                    { "type": "input_image", "image_url": data_uri },
                    { "type": "input_text", "text": instruction }
                ]}
            ],
            "stream": false,
            "temperature": self.config.defaults.temperature,
            "max_output_tokens": self.config.limits.max_output_tokens,
            "text": {
                "format": {
                    "type": "json_schema",
                    "name": schema_name,
                    "strict": self.config.structured_output.strict,
                    "schema": schema
                }
            }
        })
    }

    /// Driver-owned typed content extraction: walk `output[].content[]` and return
    /// the first `output_text` item's `text`. The config sets
    /// `content_json_pointer = null` for this driver, so the parser is code-owned
    /// rather than config-driven — Ark Responses JSON does not have a single
    /// stable content pointer (the `output` array may hold reasoning + message
    /// items), so a typed walk is more robust than a fixed pointer.
    fn extract_output_text(body: &Value) -> Option<String> {
        let output = body.get("output")?.as_array()?;
        for item in output {
            if let Some(content) = item.get("content").and_then(|c| c.as_array()) {
                for c in content {
                    if c.get("type").and_then(|t| t.as_str()) == Some("output_text") {
                        if let Some(text) = c.get("text").and_then(|t| t.as_str()) {
                            return Some(text.to_string());
                        }
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
    ) -> Result<DescribeOutcome, ProviderError> {
        let text = Self::extract_output_text(body).ok_or(ProviderError::SchemaValidation)?;
        let parsed = parse_content_json(&text)?;
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
            provider_request_id: self
                .config
                .response
                .provider_request_id_json_pointer
                .as_deref()
                .and_then(|p| json_pointer_str(body, p))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string(),
        };
        validate_understanding(&out)?;
        Ok(out)
    }

    fn parse_score(&self, body: &Value, model_id: &str) -> Result<ScoreOutcome, ProviderError> {
        let text = Self::extract_output_text(body).ok_or(ProviderError::SchemaValidation)?;
        let parsed = parse_content_json(&text)?;
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
            provider_request_id: self
                .config
                .response
                .provider_request_id_json_pointer
                .as_deref()
                .and_then(|p| json_pointer_str(body, p))
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string(),
        };
        validate_rating(&out)?;
        Ok(out)
    }

    fn response_content_excerpt(body: &Value) -> String {
        Self::extract_output_text(body)
            .map(|text| compact_text_excerpt(&text, 1200))
            .unwrap_or_else(|| sanitized_provider_json_excerpt(body, 1200))
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

#[tonic::async_trait]
impl ImageAnalysisProvider for VolcengineArkResponsesProvider {
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
        // advertise Volcengine Ark to the C++ host.
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
        self.ensure_structured_output(model)?;
        let bearer = self.bearer(credential)?;
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::describe(),
        ))?;
        let (system, instruction) = describe_prompt(prompt_profile_id, output_language)?;
        let body = self.build_responses_body(
            &slug,
            &data_uri,
            schema,
            UNDERSTANDING_SCHEMA_NAME,
            &system,
            &instruction,
        );
        let resp = send_with_retry(
            &self.http,
            &self.url(),
            &body,
            &self.attribution_headers(),
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (_headers, resp_body) = read_response(resp).await?;
        let provider_content = Self::response_content_excerpt(&resp_body);
        let outcome = match self.parse_describe(&resp_body, &slug) {
            Ok(outcome) => outcome,
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
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
        self.ensure_structured_output(model)?;
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
        let body = self.build_responses_body(
            &slug,
            &data_uri,
            schema,
            RATING_SCHEMA_NAME,
            &system,
            &instruction,
        );
        let resp = send_with_retry(
            &self.http,
            &self.url(),
            &body,
            &self.attribution_headers(),
            bearer,
            MAX_TRANSIENT_RETRIES,
        )
        .await?;
        let (_headers, resp_body) = read_response(resp).await?;
        let provider_content = Self::response_content_excerpt(&resp_body);
        let outcome = match self.parse_score(&resp_body, &slug) {
            Ok(outcome) => outcome,
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::proto::alcedo::ai::image_analysis_service_server::ImageAnalysisService;
    use crate::service::credential_vault::SecretString;
    use crate::service::provider_config::load_provider_configs;
    use std::collections::HashMap;
    use std::sync::Arc;
    use wiremock::matchers::{header, method, path};
    use wiremock::{Mock, MockServer, ResponseTemplate};

    const TEST_SECRET: &str = "ark-test-key-DO-NOT-LEAK";
    const TEST_PROMPT: &str = "Describe this image for a photo library.";
    const RAW_BODY_SENTINEL: &str = "RAW_ARK_BODY_SENTINEL";

    fn test_image_png() -> Vec<u8> {
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([10, 20, 30]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        cursor.into_inner()
    }

    fn provider_for(server: &MockServer) -> VolcengineArkResponsesProvider {
        let mut config = load_provider_configs(None)
            .expect("built-ins load")
            .get("volcengine_ark")
            .expect("volcengine_ark built-in")
            .clone();
        config.base_url = server.uri();
        VolcengineArkResponsesProvider::new(config).expect("provider builds")
    }

    /// Build an Ark Responses-shaped success body. `content_json` is the model's
    /// JSON text, placed in `output[0].content[0].output_text` — the exact JSON path
    /// the driver-owned parser walks. This mirrors the real Ark response shape so
    /// the parser is exercised against the live fixture structure.
    fn ok_responses_body(content_json: &str) -> serde_json::Value {
        json!({
            "id": "ark-resp-789",
            "output": [
                {
                    "type": "message",
                    "role": "assistant",
                    "content": [
                        { "type": "output_text", "text": content_json }
                    ]
                }
            ],
            "usage": { "input_tokens": 90, "output_tokens": 35, "total_tokens": 125 }
        })
    }

    fn secret() -> SecretString {
        SecretString::new(TEST_SECRET.to_string())
    }

    #[tokio::test]
    async fn sends_bearer_authorization() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .and(header("authorization", format!("Bearer {TEST_SECRET}")))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_responses_body(
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
        assert_eq!(out.provider_request_id, "ark-resp-789");
    }

    #[tokio::test]
    async fn request_body_uses_responses_shape_with_structured_output() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_responses_body(
                r#"{"caption":"c","tags":["t"],"scene":"","confidence":0.5}"#,
            )))
            .mount(&server)
            .await;

        let provider = provider_for(&server);
        provider
            .describe_image(
                &test_image_png(),
                "doubao-seed-2-0-lite-260428",
                "",
                "",
                Some(&secret()),
            )
            .await
            .expect("describe ok");

        let reqs = server.received_requests().await.expect("requests captured");
        assert_eq!(reqs.len(), 1);
        let body: Value = serde_json::from_slice(&reqs[0].body).expect("body json");
        assert_eq!(body["model"], "doubao-seed-2-0-lite-260428");
        assert_eq!(body["stream"], false);
        // Responses shape: `input` array with system + user messages.
        assert_eq!(body["input"][0]["role"], "system");
        assert_eq!(body["input"][0]["content"][0]["type"], "input_text");
        assert_eq!(body["input"][1]["role"], "user");
        // input_image carries the data URI as a flat string (Responses shape).
        let img = &body["input"][1]["content"][0];
        assert_eq!(img["type"], "input_image");
        assert!(
            img["image_url"]
                .as_str()
                .unwrap()
                .starts_with("data:image/png;base64,")
        );
        assert_eq!(body["input"][1]["content"][1]["type"], "input_text");
        // max_output_tokens (Responses field name), not max_tokens.
        assert_eq!(body["max_output_tokens"], 1200);
        // Structured output via text.format.json_schema.
        assert_eq!(body["text"]["format"]["type"], "json_schema");
        assert_eq!(body["text"]["format"]["name"], "alcedo_image_understanding");
        assert_eq!(body["text"]["format"]["strict"], true);
        let schema = &body["text"]["format"]["schema"];
        assert_eq!(schema["type"], "object");
        assert_eq!(schema["additionalProperties"], false);
        // No `provider` routing object (Ark does not use OpenRouter-style knobs).
        assert!(body.get("provider").is_none());
    }

    #[tokio::test]
    async fn extracts_output_text_from_responses_output_text() {
        let server = MockServer::start().await;
        // The output array may hold extra items (e.g. reasoning); the parser must
        // find the message item's output_text regardless of ordering.
        let body = json!({
            "id": "ark-resp-100",
            "output": [
                { "type": "reasoning", "content": [ { "type": "reasoning_text", "text": "thinking..." } ] },
                { "type": "message", "role": "assistant", "content": [
                    { "type": "output_text", "text": "{\"caption\":\"sunrise\",\"tags\":[\"sun\",\"sky\"],\"scene\":\"outdoor\",\"confidence\":0.8}" }
                ]}
            ],
            "usage": { "input_tokens": 50, "output_tokens": 20, "total_tokens": 70 }
        });
        Mock::given(method("POST"))
            .and(path("/responses"))
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
        assert_eq!(out.usage.total_tokens, 70);
        assert_eq!(out.provider_request_id, "ark-resp-100");
        validate_understanding(&out).expect("canned outcome validates");
    }

    #[tokio::test]
    async fn parses_rating_response_and_captures_usage() {
        let server = MockServer::start().await;
        let body = ok_responses_body(
            r#"{"rating":4,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"r"}"#,
        );
        Mock::given(method("POST"))
            .and(path("/responses"))
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
        // Single 1..=5 integer star rating; no scores array, no confidence.
        assert_eq!(out.rating, 4);
        assert_eq!(out.rubric_id, "alcedo-default-v1");
        assert_eq!(out.rubric_version, "1");
        assert_eq!(out.reasons, "r");
        assert_eq!(out.usage.total_tokens, 125);
        validate_rating(&out).expect("canned rating validates");
    }

    #[tokio::test]
    async fn parses_rating_rejects_fractional_float() {
        // A fractional rating (4.9) is schema-invalid and must NOT be truncated
        // to 4 — fail closed: the parser yields 0 and validate_rating maps it to
        // SchemaValidation (no active annotation).
        let server = MockServer::start().await;
        let body = ok_responses_body(
            r#"{"rating":4.9,"rubric_id":"alcedo-default-v1","rubric_version":"1","reasons":"x"}"#,
        );
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(200).set_body_json(body))
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
    async fn rate_limit_maps_to_transient() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
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
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(500))
            .up_to_n_times(1)
            .mount(&server)
            .await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_responses_body(
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
    async fn ark_error_body_maps_to_provider_error_without_leaking_text() {
        // Ark returns a structured error body with `error.code` / `error.message`.
        // The driver maps any non-retryable 4xx to ProviderError::Provider and must
        // NOT surface the raw provider error text (the message is dropped before
        // placement by the service; here we assert the driver itself does not echo
        // the raw body into the ProviderError string).
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(400).set_body_json(json!({
                "error": {
                    "code": "BadRequest.ParameterValidationError",
                    "message": RAW_BODY_SENTINEL
                }
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
            !err.to_string()
                .contains("BadRequest.ParameterValidationError"),
            "error code leaked: {err}"
        );
    }

    #[tokio::test]
    async fn schema_failure_does_not_produce_active_result() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_responses_body(r#"{"description":""}"#)),
            )
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
    async fn missing_output_text_maps_to_schema_validation() {
        let server = MockServer::start().await;
        // 200 but no output_text item -> driver-owned parser returns None.
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(200).set_body_json(json!({
                "id": "ark-resp-x",
                "output": [ { "type": "message", "content": [ { "type": "text", "text": "not output_text" } ] } ]
            })))
            .mount(&server)
            .await;
        let provider = provider_for(&server);
        let err = provider
            .describe_image(&test_image_png(), "", "", "", Some(&secret()))
            .await
            .expect_err("no output_text");
        assert_eq!(err, ProviderError::SchemaValidation);
    }

    #[tokio::test]
    async fn bearer_required_without_credential_errors() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path("/responses"))
            .respond_with(ResponseTemplate::new(200).set_body_json(ok_responses_body(
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
        use std::sync::Mutex;
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
                    .and(path("/responses"))
                    .respond_with(ResponseTemplate::new(500))
                    .up_to_n_times(1)
                    .mount(&server)
                    .await;
                Mock::given(method("POST"))
                    .and(path("/responses"))
                    .respond_with(ResponseTemplate::new(400).set_body_json(
                        json!({ "error": { "code": "X", "message": RAW_BODY_SENTINEL } }),
                    ))
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
        // Capture worked: the retry warn was emitted and reached the buffer.
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
            .and(path("/responses"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_responses_body(
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

        let handle = vault.register("volcengine_ark", TEST_SECRET.to_string(), None);
        let request_id = "req-cancel-ark".to_string();
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
            provider_id: "volcengine_ark".to_string(),
            model_id: "doubao-seed-2-0-lite-260428".to_string(),
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
            .and(path("/responses"))
            .respond_with(
                ResponseTemplate::new(200)
                    .set_body_json(ok_responses_body(
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

        let handle = vault.register("volcengine_ark", TEST_SECRET.to_string(), None);
        let req = DescribeImageRequest {
            header: Some(AiRequestHeader {
                request_id: "req-timeout-ark".into(),
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
            provider_id: "volcengine_ark".to_string(),
            model_id: "doubao-seed-2-0-lite-260428".to_string(),
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
}
