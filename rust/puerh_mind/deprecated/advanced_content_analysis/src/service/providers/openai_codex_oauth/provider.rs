use std::sync::Mutex;

use serde_json::Value;
use tracing::warn;

use crate::service::credential_vault::SecretString;
use crate::service::image_analysis::{
    AnalyzeImageInput, AnalyzeOutcome, BatchAnalyzeOutcome, DescribeOutcome, DiscoveredModel,
    ImageAnalysisProvider, ImageAnalysisSchemaSpec, ProviderError, ScoreOutcome,
    image_analysis_schema_json,
};
use crate::service::provider_config::{ModelConfig, ProviderConfig};
use crate::service::providers::http_util::{
    build_image_data_uri, build_rustls_client, extract_provider_request_id, strict_schema_value,
};
use crate::service::providers::openai_codex_oauth::auth::{
    CodexOAuthProfile, CodexOAuthSettings, now_unix, refresh_profile,
};
use crate::service::providers::openai_codex_oauth::client::CodexBackendClient;
use crate::service::providers::openai_codex_oauth::responses::{
    ANALYSIS_FLAT_SCHEMA_NAME, RATING_SCHEMA_NAME, UNDERSTANDING_SCHEMA_NAME, build_batch_body,
    build_single_image_body, output_excerpt, parse_analyze, parse_batch_analyze, parse_describe,
    parse_score,
};

struct CachedProfile {
    source_fingerprint: String,
    profile: CodexOAuthProfile,
}

pub struct OpenAiCodexOAuthProvider {
    config: ProviderConfig,
    oauth_settings: CodexOAuthSettings,
    token_http: reqwest::Client,
    backend: CodexBackendClient,
    cached_profile: Mutex<Option<CachedProfile>>,
}

impl OpenAiCodexOAuthProvider {
    pub fn new(config: ProviderConfig) -> Result<Self, ProviderError> {
        let http = build_rustls_client()?;
        Ok(Self::with_clients(
            config,
            http.clone(),
            http,
            CodexOAuthSettings::default(),
        ))
    }

    pub fn with_clients(
        config: ProviderConfig,
        backend_http: reqwest::Client,
        token_http: reqwest::Client,
        oauth_settings: CodexOAuthSettings,
    ) -> Self {
        Self {
            config,
            oauth_settings,
            token_http,
            backend: CodexBackendClient::new(backend_http),
            cached_profile: Mutex::new(None),
        }
    }

    fn responses_usage_pointer(&self) -> Option<&str> {
        self.config.response.usage_json_pointer.as_deref()
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
        Err(ProviderError::UnknownModel(requested.to_string()))
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

    async fn auth_profile(
        &self,
        credential: Option<&SecretString>,
    ) -> Result<CodexOAuthProfile, ProviderError> {
        let credential = credential.ok_or_else(|| {
            ProviderError::Provider(
                "OpenAI Codex OAuth provider called without a credential profile".to_string(),
            )
        })?;
        let source = CodexOAuthProfile::from_secret(credential)?;
        let source_fingerprint = source.fingerprint_source();
        let now = now_unix();
        if let Some(cached) = self
            .cached_profile
            .lock()
            .expect("OpenAI Codex OAuth profile cache mutex poisoned")
            .as_ref()
            .filter(|cached| cached.source_fingerprint == source_fingerprint)
        {
            if !cached.profile.needs_refresh(now) {
                return Ok(cached.profile.clone());
            }
        }

        let candidate = self
            .cached_profile
            .lock()
            .expect("OpenAI Codex OAuth profile cache mutex poisoned")
            .as_ref()
            .filter(|cached| cached.source_fingerprint == source_fingerprint)
            .map(|cached| cached.profile.clone())
            .unwrap_or(source);
        let profile = if candidate.needs_refresh(now) {
            refresh_profile(
                &self.token_http,
                &self.oauth_settings,
                &candidate,
                now_unix(),
            )
            .await?
        } else {
            candidate
        };
        *self
            .cached_profile
            .lock()
            .expect("OpenAI Codex OAuth profile cache mutex poisoned") = Some(CachedProfile {
            source_fingerprint,
            profile: profile.clone(),
        });
        Ok(profile)
    }

    async fn send_and_parse_describe(
        &self,
        slug: &str,
        model: Option<&ModelConfig>,
        body: Value,
        auth: &CodexOAuthProfile,
    ) -> Result<DescribeOutcome, ProviderError> {
        let (headers, resp_body) = self
            .backend
            .post_responses(&self.config, auth, &body)
            .await?;
        let request_id = extract_provider_request_id(
            &headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        match parse_describe(
            &resp_body,
            slug,
            self.responses_usage_pointer(),
            &request_id,
        ) {
            Ok(outcome) => Ok(outcome),
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
                    provider_request_id = %request_id,
                    provider_content = %output_excerpt(&resp_body),
                    error = %err,
                    "DescribeImage Codex OAuth response parse failed"
                );
                let _ = model;
                Err(err)
            }
        }
    }

    async fn send_and_parse_score(
        &self,
        slug: &str,
        body: Value,
        auth: &CodexOAuthProfile,
    ) -> Result<ScoreOutcome, ProviderError> {
        let (headers, resp_body) = self
            .backend
            .post_responses(&self.config, auth, &body)
            .await?;
        let request_id = extract_provider_request_id(
            &headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        match parse_score(
            &resp_body,
            slug,
            self.responses_usage_pointer(),
            &request_id,
        ) {
            Ok(outcome) => Ok(outcome),
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
                    provider_request_id = %request_id,
                    provider_content = %output_excerpt(&resp_body),
                    error = %err,
                    "ScoreImage Codex OAuth response parse failed"
                );
                Err(err)
            }
        }
    }

    async fn send_and_parse_analyze(
        &self,
        slug: &str,
        body: Value,
        include_understanding: bool,
        include_rating: bool,
        auth: &CodexOAuthProfile,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        let (headers, resp_body) = self
            .backend
            .post_responses(&self.config, auth, &body)
            .await?;
        let request_id = extract_provider_request_id(
            &headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        match parse_analyze(
            &resp_body,
            slug,
            self.responses_usage_pointer(),
            &request_id,
            include_understanding,
            include_rating,
        ) {
            Ok(outcome) => Ok(outcome),
            Err(err) => {
                warn!(
                    provider = %self.config.provider_id,
                    model = %slug,
                    provider_request_id = %request_id,
                    provider_content = %output_excerpt(&resp_body),
                    error = %err,
                    "AnalyzeImage Codex OAuth response parse failed"
                );
                Err(err)
            }
        }
    }
}

#[tonic::async_trait]
impl ImageAnalysisProvider for OpenAiCodexOAuthProvider {
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
        crate::proto::alcedo::ai::AiCapability {
            task_id: "image_understanding.describe".to_string(),
            provider_id: self.config.provider_id.clone(),
            model_id: self.config.defaults.model.clone(),
            input_kinds: vec![crate::proto::alcedo::ai::AiInputKind::AiInputPreview as i32],
            output_kinds: vec![
                crate::proto::alcedo::ai::AiOutputKind::AiOutputCaption as i32,
                crate::proto::alcedo::ai::AiOutputKind::AiOutputTags as i32,
            ],
            max_payload_bytes: self.config.limits.max_image_bytes as i64,
            supports_batch: true,
            supports_cancel: true,
            requires_credential: self.requires_credential(),
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
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::describe(),
        ))?;
        let prompt =
            crate::service::prompt_profiles::describe_prompt(prompt_profile_id, output_language)?;
        let body = build_single_image_body(
            &slug,
            model.as_ref(),
            &data_uri,
            schema,
            UNDERSTANDING_SCHEMA_NAME,
            self.config.structured_output.strict,
            &prompt.system,
            &prompt.instruction,
        );
        let auth = self.auth_profile(credential).await?;
        self.send_and_parse_describe(&slug, model.as_ref(), body, &auth)
            .await
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
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::score(include_rating_reasons),
        ))?;
        let prompt = crate::service::prompt_profiles::score_prompt(
            prompt_profile_id,
            rubric_id,
            rating_severity,
            output_language,
            camera_context,
            include_rating_reasons,
        )?;
        let body = build_single_image_body(
            &slug,
            model.as_ref(),
            &data_uri,
            schema,
            RATING_SCHEMA_NAME,
            self.config.structured_output.strict,
            &prompt.system,
            &prompt.instruction,
        );
        let auth = self.auth_profile(credential).await?;
        self.send_and_parse_score(&slug, body, &auth).await
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
        let data_uri = build_image_data_uri(image_bytes)?;
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::analyze(
                include_understanding,
                include_rating,
                include_rating_reasons,
            ),
        ))?;
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
        let body = build_single_image_body(
            &slug,
            model.as_ref(),
            &data_uri,
            schema,
            ANALYSIS_FLAT_SCHEMA_NAME,
            self.config.structured_output.strict,
            &prompt.system,
            &prompt.instruction,
        );
        let auth = self.auth_profile(credential).await?;
        self.send_and_parse_analyze(&slug, body, include_understanding, include_rating, &auth)
            .await
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
        let (slug, model) = self.resolve_model(model_id)?;
        self.ensure_structured_output(model.as_ref())?;
        let data_uris = images
            .iter()
            .map(|image| build_image_data_uri(image.image_bytes))
            .collect::<Result<Vec<_>, _>>()?;
        let camera_context = images
            .iter()
            .enumerate()
            .filter(|(_, image)| !image.camera_context.trim().is_empty())
            .map(|(index, image)| format!("Image index {index}: {}", image.camera_context.trim()))
            .collect::<Vec<_>>()
            .join("\n");
        let mut prompt = crate::service::prompt_profiles::analyze_prompt(
            prompt_profile_id,
            rubric_id,
            rating_severity,
            output_language,
            &camera_context,
            include_understanding,
            include_rating,
            include_rating_reasons,
        )?;
        prompt.instruction.push_str(&format!(
            "\n\nAnalyze exactly {} images in this one request. Return one result object per image in a top-level \"results\" array. Each result object must include its zero-based \"index\" matching the image order. Do not omit images.",
            images.len()
        ));
        let schema = strict_schema_value(&image_analysis_schema_json(
            ImageAnalysisSchemaSpec::batch_analyze(
                include_understanding,
                include_rating,
                include_rating_reasons,
            ),
        ))?;
        let body = build_batch_body(
            &slug,
            &data_uris,
            schema,
            self.config.structured_output.strict,
            &prompt.system,
            &prompt.instruction,
        );
        let auth = self.auth_profile(credential).await?;
        let (headers, resp_body) = self
            .backend
            .post_responses(&self.config, &auth, &body)
            .await?;
        let request_id = extract_provider_request_id(
            &headers,
            &resp_body,
            self.config.response.provider_request_id_header.as_deref(),
            self.config
                .response
                .provider_request_id_json_pointer
                .as_deref(),
        );
        parse_batch_analyze(
            &resp_body,
            &slug,
            self.responses_usage_pointer(),
            &request_id,
            images,
            include_understanding,
            include_rating,
        )
    }

    async fn list_models(
        &self,
        credential: Option<&SecretString>,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        let auth = self.auth_profile(credential).await?;
        self.backend.list_models(&self.config, &auth).await
    }
}
