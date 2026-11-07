//! Image analysis task domain — provider trait, mock provider, code-owned JSON
//! Schemas, and the validator/normalizer (Phase 5b).
//!
//! This is the provider-independent Alcedo result contract for the two Phase 5
//! remote tasks. The proto messages (`proto/image_analysis.proto`) carry these
//! typed fields on the wire; the code-owned JSON Schemas below are what Phase 5c
//! drivers inject as `response_format: json_schema` and validate provider output
//! against. Provider configs select the *injection mode* (response_format_json_schema,
//! responses_json_schema, ...); they do not define the business fields — these
//! schemas and the proto do.
//!
//! `image_understanding.describe` and `image_rating.score` are distinct contracts:
//! distinct `task_id`s, distinct result message types, distinct domain outcomes.
//! A rating result can never overwrite or be reinterpreted as an understanding
//! result (Phase 5b review focus).

use std::time::Duration;

use serde_json::{Map, Value, json};

use crate::service::credential_vault::SecretString;

/// Provider-reported usage. Fields are optional in spirit: a provider may report
/// only some; 0 means "not reported". Captured for UI/cost summaries (Phase 6).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Usage {
    pub input_tokens: i64,
    pub output_tokens: i64,
    pub total_tokens: i64,
}

/// Phase 6c: a model id discovered by live-listing the configured endpoint's
/// `/models` (or configured override). This is provider-independent: the driver
/// parses the provider's list response into these DTOs and returns them to the
/// host as *unverified candidates*. Discovery only proves the account/endpoint
/// can see a model id; it does NOT prove image input or structured-output
/// support, so a discovered model carries NO capability verdict here — the host
/// merges candidates into preset state but keeps them unadvertised until a
/// validation smoke pins `live_confirmed` (Phase 6f). `source_provider_id`
/// records which configured endpoint produced the candidate so a future
/// prompt/model change does not reinterpret it under a different provider.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiscoveredModel {
    pub model_id: String,
    pub display_name: String,
    pub source_provider_id: String,
}

/// `image_understanding.describe` outcome (provider-neutral).
#[derive(Debug, Clone)]
pub struct DescribeOutcome {
    pub caption: String,
    pub tags: Vec<String>,
    pub scene: String,
    pub confidence: f64,
    /// The model id that actually served (echoed in `AiResponseHeader.model_id`).
    pub model_id: String,
    pub usage: Usage,
    /// The provider's own request id, when available.
    pub provider_request_id: String,
}

/// `image_rating.score` outcome (provider-neutral). A single 1–5 integer star
/// rating aligned with the EXIF-standard Rating the app already stores per file,
/// plus rubric identity and a short rationale. The remote LLM is NOT asked for a
/// confidence (Phase 5f rating-contract change): the rating is a discrete label,
/// not a calibrated probability, and the app's own Rating field has no confidence
/// counterpart to pair it with.
#[derive(Debug, Clone)]
pub struct ScoreOutcome {
    pub rating: i32,
    pub rubric_id: String,
    pub rubric_version: String,
    pub reasons: String,
    pub model_id: String,
    pub usage: Usage,
    pub provider_request_id: String,
}

/// Combined per-image outcome for runs that ask for multiple outputs. This is
/// still one provider request; persistence can split the typed results into the
/// existing understanding/rating tables.
#[derive(Debug, Clone)]
pub struct AnalyzeOutcome {
    pub understanding: Option<DescribeOutcome>,
    pub rating: Option<ScoreOutcome>,
    pub model_id: String,
    pub usage: Usage,
    pub provider_request_id: String,
}

#[derive(Debug, Clone)]
pub struct AnalyzeImageInput<'a> {
    pub image_bytes: &'a [u8],
    pub camera_context: &'a str,
}

#[derive(Debug, Clone)]
pub struct BatchAnalyzeOutcome {
    pub items: Vec<AnalyzeOutcome>,
    pub model_id: String,
    pub usage: Usage,
    pub provider_request_id: String,
}

/// Why a provider call did not produce a usable typed result. The service maps
/// these (plus the credential/timeout/cancel paths it owns) into the
/// `AiResponseHeader` status/error fields. Messages here must stay free of secret
/// material; provider-derived text is redacted by the caller before placement.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ProviderError {
    #[error("provider response failed schema validation")]
    SchemaValidation,
    #[error("{0}")]
    SchemaValidationMessage(String),
    #[error("provider returned a transient error")]
    Transient,
    #[error("provider returned an error")]
    Provider(String),
    /// Phase 6c: the request supplied a non-empty `model_id` that does not
    /// resolve to any entry in the provider's `config.models[]` (built-in or
    /// discovered/persisted user config). The call fails closed BEFORE any
    /// provider HTTP request, closing the Phase 6b review gap where an unlisted
    /// model slug could bypass `supports_structured_output` and be forwarded
    /// verbatim to the provider. The inner string is the offending slug; it is
    /// dropped by the service before placement (provider text is never echoed),
    /// so a slug that happens to look like a key cannot leak through this arm.
    #[error("unknown model id; not present in provider config")]
    UnknownModel(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ImageAnalysisSchemaSpec {
    pub include_understanding: bool,
    pub include_rating: bool,
    pub include_rating_reasons: bool,
    pub batch: bool,
}

impl ImageAnalysisSchemaSpec {
    pub fn describe() -> Self {
        Self {
            include_understanding: true,
            include_rating: false,
            include_rating_reasons: false,
            batch: false,
        }
    }

    pub fn score(include_rating_reasons: bool) -> Self {
        Self {
            include_understanding: false,
            include_rating: true,
            include_rating_reasons,
            batch: false,
        }
    }

    pub fn analyze(
        include_understanding: bool,
        include_rating: bool,
        include_rating_reasons: bool,
    ) -> Self {
        Self {
            include_understanding,
            include_rating,
            include_rating_reasons: include_rating && include_rating_reasons,
            batch: false,
        }
    }

    pub fn batch_analyze(
        include_understanding: bool,
        include_rating: bool,
        include_rating_reasons: bool,
    ) -> Self {
        Self {
            include_understanding,
            include_rating,
            include_rating_reasons: include_rating && include_rating_reasons,
            batch: true,
        }
    }
}

pub fn image_analysis_schema_value(spec: ImageAnalysisSchemaSpec) -> Value {
    let mut required = Vec::<String>::new();
    let mut properties = Map::<String, Value>::new();
    if spec.batch {
        required.push("index".to_string());
        properties.insert(
            "index".to_string(),
            json!({ "type": "integer", "minimum": 0 }),
        );
    }
    append_understanding_schema_fields(&mut required, &mut properties, spec.include_understanding);
    append_rating_schema_fields(
        &mut required,
        &mut properties,
        spec.include_rating,
        spec.include_rating_reasons,
    );

    let title = if spec.batch {
        "AlcedoImageAnalysisBatch"
    } else if spec.include_understanding && spec.include_rating {
        "AlcedoImageAnalysisFlat"
    } else if spec.include_rating {
        "AlcedoImageRating"
    } else {
        "AlcedoImageUnderstanding"
    };

    let item_schema = json!({
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "title": title,
        "type": "object",
        "additionalProperties": false,
        "required": required,
        "properties": properties,
    });

    if spec.batch {
        json!({
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "title": title,
            "type": "object",
            "additionalProperties": false,
            "required": ["results"],
            "properties": {
                "results": {
                    "type": "array",
                    "minItems": 1,
                    "items": item_schema,
                }
            }
        })
    } else {
        item_schema
    }
}

pub fn image_analysis_schema_json(spec: ImageAnalysisSchemaSpec) -> String {
    serde_json::to_string_pretty(&image_analysis_schema_value(spec))
        .expect("generated image analysis schema is serializable")
}

fn append_understanding_schema_fields(
    required: &mut Vec<String>,
    properties: &mut Map<String, Value>,
    include: bool,
) {
    if !include {
        return;
    }
    required.push("description".to_string());
    properties.insert(
        "description".to_string(),
        json!({
            "type": "string",
            "minLength": 1,
            "description": "A concise image description."
        }),
    );
}

fn append_rating_schema_fields(
    required: &mut Vec<String>,
    properties: &mut Map<String, Value>,
    include: bool,
    include_reasons: bool,
) {
    if !include {
        return;
    }
    required.push("rating".to_string());
    if include_reasons {
        required.push("rating_reason".to_string());
    }
    properties.insert(
        "rating".to_string(),
        json!({
            "type": "integer",
            "minimum": 1,
            "maximum": 5,
            "description": "The app star rating, from 1 (poor) to 5 (excellent)."
        }),
    );
    if include_reasons {
        properties.insert(
            "rating_reason".to_string(),
            json!({
                "type": "string",
                "description": "A concise image-grounded rationale for the rating. Do not restate a numeric score, star count, grade, or a different rating here."
            }),
        );
    }
}

/// Validate + normalize a `image_understanding.describe` outcome against the
/// code-owned contract. Returns `ProviderError::SchemaValidation` on any breach
/// so the service maps it to a non-active provider error (Phase 5b: a schema
/// failure must not produce an active annotation).
///
pub fn validate_understanding(out: &DescribeOutcome) -> Result<(), ProviderError> {
    if out.caption.trim().is_empty() {
        return Err(ProviderError::SchemaValidation);
    }
    Ok(())
}

/// Validate + normalize a `image_rating.score` outcome. The rating is a 1–5
/// integer star rating (the app's own Rating field uses 0–5 with 0=unrated; the
/// remote contract requires 1..=5 so a scored image is never confused with an
/// unrated one). No confidence is checked — the remote LLM is not asked for one.
pub fn validate_rating(out: &ScoreOutcome) -> Result<(), ProviderError> {
    if !(1..=5).contains(&out.rating) {
        return Err(ProviderError::SchemaValidation);
    }
    Ok(())
}

pub fn validate_analyze(out: &AnalyzeOutcome) -> Result<(), ProviderError> {
    if let Some(understanding) = &out.understanding {
        validate_understanding(understanding)?;
    }
    if let Some(rating) = &out.rating {
        validate_rating(rating)?;
    }
    if out.understanding.is_none() && out.rating.is_none() {
        return Err(ProviderError::SchemaValidation);
    }
    Ok(())
}

pub fn validate_batch_analyze(out: &BatchAnalyzeOutcome) -> Result<(), ProviderError> {
    if out.items.is_empty() {
        return Err(ProviderError::SchemaValidation);
    }
    for item in &out.items {
        validate_analyze(item)?;
    }
    Ok(())
}

pub fn output_description(parsed: &Value) -> String {
    first_string_field(parsed, &["description", "caption"])
        .trim()
        .to_string()
}

pub fn output_tags(parsed: &Value) -> Vec<String> {
    parsed
        .get("tags")
        .and_then(|v| v.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|t| t.as_str().map(|s| s.trim().to_string()))
                .filter(|s| !s.is_empty())
                .collect()
        })
        .unwrap_or_default()
}

pub fn output_scene(parsed: &Value) -> String {
    first_string_field(parsed, &["scene"]).to_string()
}

pub fn output_confidence(parsed: &Value) -> f64 {
    parsed
        .get("confidence")
        .and_then(|v| v.as_f64())
        .filter(|c| !c.is_nan() && *c >= 0.0 && *c <= 1.0)
        .unwrap_or(0.0)
}

pub fn output_rating_reason(parsed: &Value) -> String {
    first_string_field(parsed, &["rating_reason", "reasons"]).to_string()
}

pub fn output_rubric_id(parsed: &Value) -> String {
    first_string_field(parsed, &["rubric_id"]).to_string()
}

pub fn output_rubric_version(parsed: &Value) -> String {
    first_string_field(parsed, &["rubric_version"]).to_string()
}

fn first_string_field<'a>(parsed: &'a Value, names: &[&str]) -> &'a str {
    names
        .iter()
        .find_map(|name| parsed.get(*name).and_then(|v| v.as_str()))
        .unwrap_or("")
}

/// Build the output-language directive appended to a prompt's system message.
/// `output_language` is the host-resolved code ("" or "en" = English, the
/// default prompt language; "zh" = Simplified Chinese). English needs no
/// directive — the prompt is already English — so only a non-English code
/// produces a sentence. Unknown codes produce no directive (fail open to the
/// default English prompt rather than injecting garbage).
pub fn language_directive(output_language: &str) -> String {
    match output_language.trim().to_ascii_lowercase().as_str() {
        "zh" | "zh-cn" | "zh_cn" | "zh-hans" | "zh_hans" | "chinese" | "中文"
        | "简体中文" => {
            " All user-visible string values in the JSON output must be written in Simplified Chinese (简体中文), especially description and rating_reason. Do not use English unless it is a proper noun, camera model, brand name, file name, or unavoidable technical term.".to_string()
        }
        _ => String::new(),
    }
}

/// Rating strictness persona selected by the host's "评价严苛程度" slider. Each
/// persona is a distinct rating system prompt. The JSON contract is unchanged
/// across personas — `rating` stays a 1..=5 integer and persona flavor lives
/// inside `reasons`.
#[cfg(test)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RatingSeverity {
    Lite,
    Normal,
    High,
    XHigh,
    Max,
}

/// Normalize a host-supplied severity code to a persona. "" or "normal" (and
/// any unrecognized value) resolve to `Normal` — fail open to the balanced
/// default rather than injecting an unknown persona, mirroring
/// `language_directive`'s unknown-code handling.
#[cfg(test)]
pub fn normalize_rating_severity(severity: &str) -> RatingSeverity {
    match severity.trim().to_ascii_lowercase().as_str() {
        "lite" => RatingSeverity::Lite,
        "high" => RatingSeverity::High,
        "xhigh" | "x_high" => RatingSeverity::XHigh,
        "max" => RatingSeverity::Max,
        _ => RatingSeverity::Normal,
    }
}

/// The shared JSON-only contract every rating persona ends with — identical to
/// the tail of the original single rating prompt, so the injected schema + the
/// code-owned `validate_rating` remain the source of truth for the wire shape.
#[cfg(test)]
const RATING_JSON_CONTRACT: &str = " Do not include any other field — in particular, do not output a confidence. Output only the JSON object — no prose, no markdown code fences.";

/// The Lite (水) persona: generous and forgiving. Ordinary competent photos
/// default to 3–4; reserve 1–2 for clear technical failures and 5 for genuinely
/// exceptional work. Reasons are short and mild.
#[cfg(test)]
const RATING_LITE_PERSONA: &str = "You are a generous, forgiving image rating assistant for Alcedo Studio. Rate the supplied image against the given rubric and respond with a single JSON object matching the provided schema. The object must contain: \"rating\" (an integer from 1 to 5, where 1 is a poor photo and 5 is an excellent photo — the app's star rating), \"rubric_id\" (the rubric you applied), \"rubric_version\" (the rubric version, or an empty string), and \"reasons\" (a short rationale). Be lenient: an ordinary, in-focus, well-exposed photo deserves a 3; a competent, pleasing photo deserves a 4; reserve 1–2 only for clear technical failures (missed focus, severe exposure error) and 5 for genuinely exceptional work. Keep the rationale short, concrete, and kind.";

/// The Normal persona: the original balanced rubric, verbatim — the
/// behavior-preserving baseline. No severity clause is injected.
#[cfg(test)]
const RATING_NORMAL_PERSONA: &str = "You are an image rating assistant for Alcedo Studio. Rate the supplied image against the given rubric and respond with a single JSON object matching the provided schema. The object must contain: \"rating\" (an integer from 1 to 5, where 1 is a poor photo and 5 is an excellent photo — the app's star rating), \"rubric_id\" (the rubric you applied), \"rubric_version\" (the rubric version, or an empty string), and \"reasons\" (a short rationale).";

/// The High (大师) persona: strict but guiding. It cares more about visual
/// meaning, composition, narrative, expression, and completeness than thumbnail
/// sharpness or generic exposure trivia, and should point to master works as
/// constructive references.
#[cfg(test)]
const RATING_HIGH_PERSONA: &str = "You are a strict but constructive master-level photography mentor rating images for Alcedo Studio. Rate the supplied image against the given rubric and respond with a single JSON object matching the provided schema. The object must contain: \"rating\" (an integer from 1 to 5, where 1 is a poor photo and 5 is an excellent photo — the app's star rating), \"rubric_id\" (the rubric you applied), \"rubric_version\" (the rubric version, or an empty string), and \"reasons\" (a short rationale). Grade rigorously, but make the critique guiding rather than dismissive. Do not over-focus on exposure, blur, sharpness, or pixel-level defects unless they clearly damage the image's purpose. Put more weight on meaning, composition, narrative, subject relationship, emotional expression, timing, visual completeness, and whether every part of the frame serves the idea. When useful, cite a relevant master photographer or body of work as a direction for improvement, such as Henri Cartier-Bresson for decisive geometry, Fan Ho for light and urban rhythm, Saul Leiter for color and occlusion, Daido Moriyama for raw expressive energy, Alex Webb for layered color, Gregory Crewdson for staged narrative, or Rinko Kawauchi for quiet poetic attention. Keep the voice firm, specific, and encouraging.";

/// The XHigh (老法师) persona: old-school, gear-aware, parameter-obsessed, and
/// fussy about obvious visual impact. It is stricter than High and less
/// constructive, but not as internet-poisoned as Max.
#[cfg(test)]
const RATING_XHIGH_PERSONA: &str = "You are an old-school 老法师 photography critic rating images for Alcedo Studio. Rate the supplied image against the given rubric and respond with a single JSON object matching the provided schema. The object must contain: \"rating\" (an integer from 1 to 5, where 1 is a poor photo and 5 is an excellent photo — the app's star rating), \"rubric_id\" (the rubric you applied), \"rubric_version\" (the rubric version, or an empty string), and \"reasons\" (a short rationale). Be strict in a gear-and-parameter-conscious way: scrutinize camera and lens choice, focal length, aperture, ISO, shutter speed, tripod stability, filters, visible sharpness, tonal punch, contrast, saturation, background blur, pose, subject prominence, and whether the image looks like it used expensive equipment well. When EXIF/camera metadata is provided, judge whether the parameters actually fit the subject. Do not be subtle about weak composition, dull light, flat contrast, muddy color, poor posing, or empty scenery. You may sound like someone with decades of shooting experience giving blunt practical advice, occasionally mentioning gear discipline, contrast not being enough, large-aperture lenses, telephoto compression, tripod stability, or post-processing that is either too timid or too heavy. Keep the critique image-grounded; do not invent unavailable equipment details.";

/// The Max (懂哥) persona: the old most severe connoisseur mode. It is an
/// exacting, pretentious photo critic whose `reasons` carry a harsh-critic
/// voice. The catchphrases live inside the `reasons` string only — the `rating`
/// is still a plain 1..=5 integer and the response is still a single JSON object
/// matching the schema.
#[cfg(test)]
const RATING_MAX_PERSONA: &str = "You are an exacting, self-important photo connoisseur (懂哥) rating images for Alcedo Studio. Rate the supplied image against the given rubric and respond with a single JSON object matching the provided schema. The object must contain: \"rating\" (an integer from 1 to 5, where 1 is a poor photo and 5 is an excellent photo — the app's star rating), \"rubric_id\" (the rubric you applied), \"rubric_version\" (the rubric version, or an empty string), and \"reasons\" (a short rationale). Be harsh and nitpick composition, exposure, focus, lighting, and intent; default most photos to 2–3 and reserve 4–5 for work that genuinely impresses you. When camera/EXIF metadata is provided, judge whether the camera, lens, aperture, shutter speed, ISO, and focal length serve the image. Give a slight positive bias to Hasselblad or Leica bodies/lenses by implying the gear contributes a special rendering, micro-contrast, color, or atmosphere. If the metadata suggests low-end or entry-level gear, be more demanding about visible detail, tonal discipline, and technical execution. If the metadata suggests expensive gear but the image is weak under your standard, lower the score substantially and criticize the photographer as a gear-obsessed 器材党 who wasted excellent equipment. Write `reasons` in your pretentious critic voice and naturally lean on your habitual catchphrases where they fit — e.g. \"没意义\", \"你这个……\", \"建议多看看××大师的作品\", \"算不上摄影\". The catchphrases go inside the `reasons` string only; never emit them as separate fields.";

/// Build the full rating system prompt for one severity, with the
/// output-language directive appended. Centralized here so the three HTTP
/// drivers share one definition of the personas instead of each hardcoding the
/// rating prompt (Phase: 评价严苛程度 toggle). `Normal` reproduces the original
/// single rating prompt verbatim, so a host that leaves severity at the default
/// sees byte-identical prompt text to before this change.
#[cfg(test)]
pub fn rating_system_prompt(rating_severity: &str, output_language: &str) -> String {
    let persona = match normalize_rating_severity(rating_severity) {
        RatingSeverity::Lite => RATING_LITE_PERSONA,
        RatingSeverity::Normal => RATING_NORMAL_PERSONA,
        RatingSeverity::High => RATING_HIGH_PERSONA,
        RatingSeverity::XHigh => RATING_XHIGH_PERSONA,
        RatingSeverity::Max => RATING_MAX_PERSONA,
    };
    let mut system = persona.to_string();
    system.push_str(RATING_JSON_CONTRACT);
    system.push_str(&language_directive(output_language));
    system
}

/// A remote/local image-analysis provider. Phase 5b ships one implementation
/// (`MockImageAnalysisProvider`); Phase 5c adds `OpenRouterChatProvider` and
/// `VolcengineArkResponsesProvider` behind the same trait; Phase 6b generalizes the
/// OpenAI Chat-compatible path into `OpenAiChatCompatibleProvider` (of which
/// `OpenRouterChatProvider` is now a type alias) and keeps
/// `AnthropicMessagesProvider` generic over the base URL.
///
/// `credential` carries the resolved secret (from the Rust credential vault) when
/// `requires_credential()` is true; `None` otherwise. The provider must treat the
/// `SecretString` as a secret — only `expose()` it at the single call site that
/// builds the `Authorization` header, and never log it, base64, image bytes, or
/// prompt payloads (Phase 5c negative-test focus). The service resolves the handle
/// against the vault and passes the secret down, so the secret travels only across
/// the gRPC loopback -> vault -> trait call boundary, never through process args,
/// `AiSidecarRuntimeOptions`, or logs (Phase 3 invariant preserved).
#[tonic::async_trait]
pub trait ImageAnalysisProvider: Send + Sync {
    fn provider_id(&self) -> &str;
    /// True when the provider needs a resolved credential handle to call out.
    fn requires_credential(&self) -> bool;
    /// Maximum encoded image payload accepted per image by this provider.
    /// `0` means no provider-specific limit beyond the gRPC message cap.
    fn max_payload_bytes(&self) -> usize {
        0
    }
    /// The descriptor this provider advertises via `ListCapabilities` (mock only
    /// in Phase 5b; real providers advertise via provider configs in 5a).
    fn capability(&self) -> crate::proto::alcedo::ai::AiCapability;
    async fn describe_image(
        &self,
        image_bytes: &[u8],
        model_id: &str,
        prompt_profile_id: &str,
        output_language: &str,
        credential: Option<&SecretString>,
    ) -> Result<DescribeOutcome, ProviderError>;
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
    ) -> Result<ScoreOutcome, ProviderError>;
    async fn analyze_image(
        &self,
        _image_bytes: &[u8],
        _model_id: &str,
        _prompt_profile_id: &str,
        _rubric_id: &str,
        _rating_severity: &str,
        _output_language: &str,
        _camera_context: &str,
        _include_understanding: bool,
        _include_rating: bool,
        _include_rating_reasons: bool,
        _credential: Option<&SecretString>,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        Err(ProviderError::Provider(
            "combined image analysis is not supported by this provider driver".to_string(),
        ))
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
        let mut items = Vec::with_capacity(images.len());
        let mut usage = Usage::default();
        let mut provider_request_id = String::new();
        let mut model = String::new();
        for image in images {
            let out = self
                .analyze_image(
                    image.image_bytes,
                    model_id,
                    prompt_profile_id,
                    rubric_id,
                    rating_severity,
                    output_language,
                    image.camera_context,
                    include_understanding,
                    include_rating,
                    include_rating_reasons,
                    credential,
                )
                .await?;
            if model.is_empty() {
                model = out.model_id.clone();
            }
            if provider_request_id.is_empty() {
                provider_request_id = out.provider_request_id.clone();
            }
            usage.input_tokens += out.usage.input_tokens;
            usage.output_tokens += out.usage.output_tokens;
            usage.total_tokens += out.usage.total_tokens;
            items.push(out);
        }
        Ok(BatchAnalyzeOutcome {
            items,
            model_id: model,
            usage,
            provider_request_id,
        })
    }
    /// Phase 6c: list the model ids the configured endpoint exposes (a dry-run
    /// discovery probe, not an image-analysis call). The credential is the
    /// resolved vault secret when `requires_credential()` is true; it is
    /// `expose()`d only at the auth-header build site, exactly like the task
    /// calls. The default impl fails closed — only the OpenAI- and
    /// Anthropic-compatible drivers override it. Discovery never persists
    /// annotations; the host treats the returned candidates as unverified.
    async fn list_models(
        &self,
        _credential: Option<&SecretString>,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        Err(ProviderError::Provider(
            "model discovery is not supported by this provider".to_string(),
        ))
    }
}

/// How the mock should misbehave, for service-level failure tests.
#[derive(Debug, Clone)]
pub enum MockFailure {
    None,
    /// Return an outcome that fails the service validator (empty caption /
    /// out-of-range confidence) — exercises the schema-validation failure path.
    InvalidOutput,
    /// Sleep past the caller's timeout — exercises the DEADLINE_EXCEEDED path.
    Slow(Duration),
    /// Return `ProviderError::Provider` — exercises the provider-error path
    /// (maps to PROVIDER_ERROR / INTERNAL). The inner string is dropped by the
    /// service before placement, so provider text is not leaked in the header.
    Error,
    /// Return `ProviderError::Transient` — exercises the transient-error path
    /// (maps to PROVIDER_UNAVAILABLE / PROVIDER_5XX). Real 5xx/rate-limit
    /// detection lives in the Phase 5c HTTP drivers; this mock covers the
    /// service-side mapping so all three `provider_error_to_header` arms are
    /// exercised in Phase 5b.
    Transient,
}

/// A provider that returns valid typed results without any HTTP. Used by the
/// service tests and as the bundled dev/mock provider in Phase 5b.
pub struct MockImageAnalysisProvider {
    provider_id: String,
    model_id: String,
    requires_credential: bool,
    max_payload_bytes: usize,
    failure: MockFailure,
    /// Phase 6c: when set, `list_models` returns these candidates; otherwise it
    /// uses the default unsupported impl. Lets the server test the ListModels
    /// RPC plumbing (credential resolution, success/failure header) without HTTP.
    discovered_models: Vec<DiscoveredModel>,
}

impl MockImageAnalysisProvider {
    pub fn new(provider_id: impl Into<String>, model_id: impl Into<String>) -> Self {
        Self {
            provider_id: provider_id.into(),
            model_id: model_id.into(),
            requires_credential: false,
            max_payload_bytes: 0,
            failure: MockFailure::None,
            discovered_models: Vec::new(),
        }
    }

    pub fn with_requires_credential(mut self, v: bool) -> Self {
        self.requires_credential = v;
        self
    }

    pub fn with_max_payload_bytes(mut self, bytes: usize) -> Self {
        self.max_payload_bytes = bytes;
        self
    }

    pub fn with_failure(mut self, f: MockFailure) -> Self {
        self.failure = f;
        self
    }

    pub fn with_discovered_models(mut self, models: Vec<DiscoveredModel>) -> Self {
        self.discovered_models = models;
        self
    }

    fn canned_describe(&self) -> DescribeOutcome {
        DescribeOutcome {
            caption: "A mock caption describing the image.".to_string(),
            tags: vec!["mock".to_string(), "test".to_string()],
            scene: "studio".to_string(),
            confidence: 0.9,
            model_id: self.model_id.clone(),
            usage: Usage {
                input_tokens: 128,
                output_tokens: 32,
                total_tokens: 160,
            },
            provider_request_id: "mock-req-1".to_string(),
        }
    }

    fn canned_score(&self) -> ScoreOutcome {
        ScoreOutcome {
            rating: 4,
            rubric_id: "alcedo-default-v1".to_string(),
            rubric_version: "1".to_string(),
            reasons: "Mock rubric reasons.".to_string(),
            model_id: self.model_id.clone(),
            usage: Usage {
                input_tokens: 128,
                output_tokens: 48,
                total_tokens: 176,
            },
            provider_request_id: "mock-req-2".to_string(),
        }
    }
}

#[tonic::async_trait]
impl ImageAnalysisProvider for MockImageAnalysisProvider {
    fn provider_id(&self) -> &str {
        &self.provider_id
    }

    fn requires_credential(&self) -> bool {
        self.requires_credential
    }

    fn max_payload_bytes(&self) -> usize {
        self.max_payload_bytes
    }

    fn capability(&self) -> crate::proto::alcedo::ai::AiCapability {
        use crate::proto::alcedo::ai::{AiCapability, AiInputKind, AiOutputKind};
        let inputs = vec![
            AiInputKind::AiInputThumbnail as i32,
            AiInputKind::AiInputPreview as i32,
            AiInputKind::AiInputImage as i32,
        ];
        AiCapability {
            task_id: "image_understanding.describe".to_string(),
            provider_id: self.provider_id.clone(),
            model_id: self.model_id.clone(),
            input_kinds: inputs.clone(),
            output_kinds: vec![
                AiOutputKind::AiOutputCaption as i32,
                AiOutputKind::AiOutputTags as i32,
            ],
            supports_batch: false,
            supports_cancel: true,
            requires_credential: self.requires_credential,
            max_payload_bytes: 0,
        }
    }

    async fn describe_image(
        &self,
        _image_bytes: &[u8],
        _model_id: &str,
        _prompt_profile_id: &str,
        _output_language: &str,
        _credential: Option<&SecretString>,
    ) -> Result<DescribeOutcome, ProviderError> {
        if let MockFailure::Slow(d) = self.failure {
            tokio::time::sleep(d).await;
        }
        if matches!(self.failure, MockFailure::Error) {
            return Err(ProviderError::Provider("mock provider failed".to_string()));
        }
        if matches!(self.failure, MockFailure::Transient) {
            return Err(ProviderError::Transient);
        }
        let mut out = self.canned_describe();
        if matches!(self.failure, MockFailure::InvalidOutput) {
            // Corrupt the outcome so the service validator rejects it.
            out.caption.clear();
            out.confidence = 1.5;
        }
        Ok(out)
    }

    async fn score_image(
        &self,
        _image_bytes: &[u8],
        _model_id: &str,
        _prompt_profile_id: &str,
        _rubric_id: &str,
        _rating_severity: &str,
        _output_language: &str,
        _camera_context: &str,
        include_rating_reasons: bool,
        _credential: Option<&SecretString>,
    ) -> Result<ScoreOutcome, ProviderError> {
        if let MockFailure::Slow(d) = self.failure {
            tokio::time::sleep(d).await;
        }
        if matches!(self.failure, MockFailure::Error) {
            return Err(ProviderError::Provider("mock provider failed".to_string()));
        }
        if matches!(self.failure, MockFailure::Transient) {
            return Err(ProviderError::Transient);
        }
        let mut out = self.canned_score();
        if matches!(self.failure, MockFailure::InvalidOutput) {
            // Corrupt the outcome so the service validator rejects it: an
            // out-of-range rating (0 is the app's "unrated" sentinel, not a
            // valid scored rating, and outside the 1..=5 contract).
            out.rating = 0;
        }
        if !include_rating_reasons {
            out.reasons.clear();
        }
        Ok(out)
    }

    async fn analyze_image(
        &self,
        _image_bytes: &[u8],
        _model_id: &str,
        _prompt_profile_id: &str,
        _rubric_id: &str,
        _rating_severity: &str,
        _output_language: &str,
        _camera_context: &str,
        include_understanding: bool,
        include_rating: bool,
        include_rating_reasons: bool,
        _credential: Option<&SecretString>,
    ) -> Result<AnalyzeOutcome, ProviderError> {
        if let MockFailure::Slow(d) = self.failure {
            tokio::time::sleep(d).await;
        }
        if matches!(self.failure, MockFailure::Error) {
            return Err(ProviderError::Provider("mock provider failed".to_string()));
        }
        if matches!(self.failure, MockFailure::Transient) {
            return Err(ProviderError::Transient);
        }
        let mut understanding = self.canned_describe();
        let mut rating = self.canned_score();
        if matches!(self.failure, MockFailure::InvalidOutput) {
            understanding.caption.clear();
            rating.rating = 0;
        }
        if !include_rating_reasons {
            rating.reasons.clear();
        }
        Ok(AnalyzeOutcome {
            model_id: self.model_id.clone(),
            usage: Usage {
                input_tokens: 128,
                output_tokens: 72,
                total_tokens: 200,
            },
            provider_request_id: "mock-req-combined".to_string(),
            understanding: include_understanding.then_some(understanding),
            rating: include_rating.then_some(rating),
        })
    }

    /// Phase 6c: when the mock was given discovered candidates, return them;
    /// otherwise fail closed with the default unsupported error.
    async fn list_models(
        &self,
        _credential: Option<&SecretString>,
    ) -> Result<Vec<DiscoveredModel>, ProviderError> {
        if self.discovered_models.is_empty() {
            return Err(ProviderError::Provider(
                "model discovery is not supported by this provider".to_string(),
            ));
        }
        Ok(self.discovered_models.clone())
    }
}

#[cfg(test)]
mod schema_tests {
    use super::*;
    #[test]
    fn understanding_schema_is_well_formed_json() {
        let v = image_analysis_schema_value(ImageAnalysisSchemaSpec::describe());
        assert_eq!(v["type"], "object");
        assert!(v["required"].is_array());
        let required: Vec<&str> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert_eq!(required, vec!["description"]);
        assert_eq!(v["properties"]["description"]["type"], "string");
        assert!(v["properties"].get("tags").is_none());
        assert!(v["properties"].get("scene").is_none());
        assert!(v["properties"].get("confidence").is_none());
    }

    #[test]
    fn rating_schema_is_well_formed_json() {
        let v = image_analysis_schema_value(ImageAnalysisSchemaSpec::score(true));
        assert_eq!(v["type"], "object");
        let required: Vec<&str> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert!(required.contains(&"rating"));
        assert!(required.contains(&"rating_reason"));
        // The rating is a 1..=5 integer; no `scores` array and no `confidence`
        // are requested from the remote LLM (Phase 5f rating-contract change).
        assert_eq!(v["properties"]["rating"]["type"], "integer");
        assert_eq!(v["properties"]["rating"]["minimum"], 1);
        assert_eq!(v["properties"]["rating"]["maximum"], 5);
        assert!(
            v["properties"].get("scores").is_none(),
            "scores array still present"
        );
        assert!(
            v["properties"].get("confidence").is_none(),
            "confidence still present"
        );
        assert!(v["properties"].get("rubric_id").is_none());
        assert!(v["properties"].get("rubric_version").is_none());
    }

    #[test]
    fn rating_schema_omits_reasons_when_not_requested() {
        let v = image_analysis_schema_value(ImageAnalysisSchemaSpec::score(false));
        assert!(
            v["properties"].get("rating_reason").is_none(),
            "rating_reason present despite include_rating_reasons=false"
        );
        let required: Vec<&str> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert!(!required.contains(&"rating_reason"));
    }

    #[test]
    fn validator_accepts_valid_understanding() {
        let out = DescribeOutcome {
            caption: "c".into(),
            tags: vec!["t".into()],
            scene: "s".into(),
            confidence: 0.5,
            model_id: "m".into(),
            usage: Usage::default(),
            provider_request_id: "r".into(),
        };
        validate_understanding(&out).expect("valid");
    }

    #[test]
    fn validator_rejects_empty_caption() {
        let out = DescribeOutcome {
            caption: "  ".into(),
            tags: vec!["t".into()],
            scene: "".into(),
            confidence: 0.5,
            model_id: "m".into(),
            usage: Usage::default(),
            provider_request_id: "r".into(),
        };
        assert_eq!(
            validate_understanding(&out).unwrap_err(),
            ProviderError::SchemaValidation
        );
    }

    #[test]
    fn validator_allows_omitted_legacy_metadata() {
        let out = DescribeOutcome {
            caption: "c".into(),
            tags: vec![],
            scene: "".into(),
            confidence: f64::NAN,
            model_id: "m".into(),
            usage: Usage::default(),
            provider_request_id: "r".into(),
        };
        validate_understanding(&out).expect("legacy metadata is no longer required");
    }

    #[test]
    fn validator_rejects_out_of_range_rating() {
        // An out-of-range rating (0 is the app's "unrated" sentinel and outside
        // the 1..=5 remote contract) is rejected for rating.
        let bad_rating = ScoreOutcome {
            rating: 0,
            rubric_id: "r".into(),
            rubric_version: "1".into(),
            reasons: "".into(),
            model_id: "m".into(),
            usage: Usage::default(),
            provider_request_id: "r".into(),
        };
        assert_eq!(
            validate_rating(&bad_rating).unwrap_err(),
            ProviderError::SchemaValidation
        );

        // 6 is also out of range (the upper bound is inclusive 5).
        let too_high = ScoreOutcome {
            rating: 6,
            rubric_id: "r".into(),
            rubric_version: "1".into(),
            reasons: "".into(),
            model_id: "m".into(),
            usage: Usage::default(),
            provider_request_id: "r".into(),
        };
        assert_eq!(
            validate_rating(&too_high).unwrap_err(),
            ProviderError::SchemaValidation
        );
    }

    #[tokio::test]
    async fn mock_returns_canned_valid_results() {
        let mock = MockImageAnalysisProvider::new("mock", "alcedo-mock");
        let d = mock
            .describe_image(&[], "", "", "", None)
            .await
            .expect("describe");
        assert_eq!(d.caption, "A mock caption describing the image.");
        assert!(!d.tags.is_empty());
        validate_understanding(&d).expect("canned describe validates");
        let s = mock
            .score_image(&[], "", "", "", "", "", "", true, None)
            .await
            .expect("score");
        validate_rating(&s).expect("canned score validates");
    }

    #[test]
    fn rating_system_prompt_normal_matches_legacy_text() {
        // Normal is the behavior-preserving baseline: its system prompt must
        // equal the original single rating prompt (persona + JSON contract),
        // with no language directive appended for English.
        let prompt = rating_system_prompt("normal", "");
        assert!(prompt.starts_with("You are an image rating assistant for Alcedo Studio."));
        assert!(prompt.contains("\"rating\" (an integer from 1 to 5"));
        assert!(prompt.contains("Do not include any other field"));
        assert!(prompt.contains("no prose, no markdown code fences"));
        // No language directive for English.
        assert!(!prompt.contains("Respond in Simplified Chinese"));
    }

    #[test]
    fn rating_system_prompt_lite_is_generous_persona() {
        let prompt = rating_system_prompt("lite", "");
        assert!(prompt.contains("generous, forgiving"));
        assert!(prompt.contains("Be lenient"));
        // Still bound by the same JSON-only contract.
        assert!(prompt.contains("no prose, no markdown code fences"));
        // Lite must differ from Normal.
        assert_ne!(prompt, rating_system_prompt("normal", ""));
    }

    #[test]
    fn rating_system_prompt_high_is_master_guided() {
        let prompt = rating_system_prompt("high", "");
        assert!(prompt.contains("master-level photography mentor"));
        assert!(prompt.contains("meaning, composition, narrative"));
        assert!(prompt.contains("Henri Cartier-Bresson"));
        assert!(prompt.contains("no prose, no markdown code fences"));
        assert_ne!(prompt, rating_system_prompt("normal", ""));
    }

    #[test]
    fn rating_system_prompt_xhigh_is_old_school_gear_critic() {
        let prompt = rating_system_prompt("xhigh", "");
        assert!(prompt.contains("老法师"));
        assert!(prompt.contains("gear-and-parameter-conscious"));
        assert!(prompt.contains("tripod stability"));
        assert!(prompt.contains("post-processing"));
        assert!(prompt.contains("no prose, no markdown code fences"));
        assert_ne!(prompt, rating_system_prompt("normal", ""));
    }

    #[test]
    fn rating_system_prompt_max_contains_connoisseur_catchphrases() {
        // Max is the 懂哥 persona: its system prompt must carry the harsh critic
        // clause and the habitual catchphrases, and still end with the JSON-only
        // contract so the wire shape is unchanged.
        let prompt = rating_system_prompt("max", "");
        assert!(prompt.contains("exacting, self-important photo connoisseur"));
        assert!(prompt.contains("没意义"));
        assert!(prompt.contains("建议多看看"));
        assert!(prompt.contains("你这个"));
        // Catchphrases are described as living inside `reasons` only.
        assert!(prompt.contains("inside the `reasons` string only"));
        assert!(prompt.contains("no prose, no markdown code fences"));
        assert_ne!(prompt, rating_system_prompt("normal", ""));
    }

    #[test]
    fn rating_system_prompt_appends_zh_language_directive() {
        let en = rating_system_prompt("normal", "");
        let zh = rating_system_prompt("normal", "zh");
        let directive = language_directive("zh");
        assert!(zh.ends_with(&directive));
        assert!(directive.contains("JSON output"));
        assert!(directive.contains("description"));
        assert!(directive.contains("rating_reason"));
        assert_eq!(zh.len(), en.len() + directive.len());
    }

    #[test]
    fn language_directive_accepts_common_chinese_aliases() {
        for alias in ["zh-CN", "zh_Hans", "Chinese", "中文", "简体中文"] {
            let directive = language_directive(alias);
            assert!(directive.contains("Simplified Chinese"), "{alias}");
            assert!(directive.contains("description"), "{alias}");
            assert!(directive.contains("rating_reason"), "{alias}");
        }
    }

    #[test]
    fn rating_system_prompt_unknown_severity_falls_open_to_normal() {
        // "" / "normal" / any unrecognized code resolve to the Normal persona
        // (fail open), mirroring language_directive's unknown-code handling.
        let baseline = rating_system_prompt("normal", "");
        assert_eq!(rating_system_prompt("", ""), baseline);
        assert_eq!(rating_system_prompt("does-not-exist", ""), baseline);
        assert_eq!(rating_system_prompt("  NORMAL  ", ""), baseline);
    }

    #[test]
    fn normalize_rating_severity_maps_known_codes() {
        assert_eq!(normalize_rating_severity("lite"), RatingSeverity::Lite);
        assert_eq!(normalize_rating_severity("high"), RatingSeverity::High);
        assert_eq!(normalize_rating_severity("xhigh"), RatingSeverity::XHigh);
        assert_eq!(normalize_rating_severity("x_high"), RatingSeverity::XHigh);
        assert_eq!(normalize_rating_severity("max"), RatingSeverity::Max);
        assert_eq!(normalize_rating_severity("normal"), RatingSeverity::Normal);
        assert_eq!(normalize_rating_severity(""), RatingSeverity::Normal);
        assert_eq!(normalize_rating_severity("bogus"), RatingSeverity::Normal);
    }
}
