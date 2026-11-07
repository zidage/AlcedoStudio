//! Shared HTTP plumbing for the Phase 5c remote image-analysis drivers.
//!
//! Both the OpenRouter Chat provider and the Volcengine Ark Responses provider
//! use the same rustls-backed `reqwest` client, the same image->data-URI encoding,
//! the same bounded retry policy for transient transport / 429 / 5xx failures,
//! the same strict-compatible JSON Schema injection, and the same redaction
//! discipline: the driver never logs the secret, the image bytes / base64, the
//! prompt text, or the raw provider response body. Only status, attempt count,
//! latency, and the provider request id are logged. Driver-specific request
//! building and response parsing live in their own modules; this file holds only
//! the shared pieces, so a change to the retry / redaction / TLS policy applies
//! uniformly to every driver.

use std::time::Duration;

use base64::Engine;
use serde_json::Value;
use tracing::warn;

use crate::service::image_analysis::{DiscoveredModel, ProviderError, Usage};
use crate::service::provider_config::ModelListResponseConfig;

/// Backoff between retry attempts. Kept small and fixed because a provider call
/// is paid and non-idempotent — the plan's review focus is that retries stay
/// bounded and do not retry aggressively. The outer service timeout bounds the
/// total wall-clock anyway, so a long backoff would just be cut into a
/// `DEADLINE_EXCEEDED` rather than helping.
const RETRY_BACKOFF: Duration = Duration::from_millis(100);

/// Upper bound on a server-supplied `Retry-After` (seconds) that the driver will
/// honor, so a misbehaving or hostile server cannot stall the sidecar for longer
/// than the policy intends.
const MAX_RETRY_AFTER: Duration = Duration::from_secs(5);
const MAX_ERROR_BODY_CHARS: usize = 700;
const MAX_ERROR_BODY_BYTES: usize = 64 * 1024;
pub const MAX_PROVIDER_RESPONSE_BYTES: usize = 16 * 1024 * 1024;

pub fn compact_text_excerpt(text: &str, max_chars: usize) -> String {
    let compact = text.split_whitespace().collect::<Vec<_>>().join(" ");
    let excerpt: String = compact.chars().take(max_chars).collect();
    if compact.chars().count() > max_chars {
        format!("{excerpt}...")
    } else {
        excerpt
    }
}

pub fn compact_json_excerpt(value: &Value, max_chars: usize) -> String {
    let text = serde_json::to_string(value).unwrap_or_else(|_| value.to_string());
    compact_text_excerpt(&text, max_chars)
}

pub fn sanitized_provider_json_excerpt(value: &Value, max_chars: usize) -> String {
    let sanitized = sanitize_provider_observability_value(value);
    compact_json_excerpt(&sanitized, max_chars)
}

pub fn parse_discovered_models(
    body: &Value,
    response: &ModelListResponseConfig,
    source_provider_id: &str,
    protocol_label: &str,
) -> Result<Vec<DiscoveredModel>, ProviderError> {
    let data_pointer = configured_pointer(response.data_json_pointer.as_deref(), "/data");
    let data = json_pointer_str(body, data_pointer)
        .and_then(|d| d.as_array())
        .ok_or_else(|| {
            let expectation = if data_pointer == "/data" {
                "expected top-level \"data\" array of model objects".to_string()
            } else {
                format!("expected model array at JSON pointer {data_pointer:?}")
            };
            ProviderError::SchemaValidationMessage(format!(
                "{protocol_label} model list response failed schema validation: {expectation}; response excerpt: {}",
                sanitized_provider_json_excerpt(body, 1200)
            ))
        })?;
    let mut out = Vec::with_capacity(data.len());
    for item in data {
        let Some(id) = model_item_id(item, response.id_json_pointer.as_deref()) else {
            continue;
        };
        let display_name =
            model_item_display_name(item, response.display_name_json_pointer.as_deref(), id);
        out.push(DiscoveredModel {
            model_id: id.to_string(),
            display_name,
            source_provider_id: source_provider_id.to_string(),
        });
    }
    Ok(out)
}

fn configured_pointer<'a>(pointer: Option<&'a str>, default_pointer: &'a str) -> &'a str {
    pointer
        .map(str::trim)
        .filter(|p| !p.is_empty())
        .unwrap_or(default_pointer)
}

fn model_item_id<'a>(item: &'a Value, id_pointer: Option<&'a str>) -> Option<&'a str> {
    if let Some(id) = item.as_str() {
        return Some(id);
    }
    let pointer = id_pointer.map(str::trim).filter(|p| !p.is_empty());
    if let Some(pointer) = pointer {
        return json_pointer_str(item, pointer).and_then(|v| v.as_str());
    }
    item.get("id").and_then(|v| v.as_str())
}

fn model_item_display_name(
    item: &Value,
    display_name_pointer: Option<&str>,
    fallback_id: &str,
) -> String {
    let pointer = display_name_pointer
        .map(str::trim)
        .filter(|p| !p.is_empty());
    if let Some(display_name) = pointer
        .and_then(|p| json_pointer_str(item, p))
        .and_then(|v| v.as_str())
    {
        return display_name.to_string();
    }
    item.get("display_name")
        .or_else(|| item.get("name"))
        .and_then(|v| v.as_str())
        .unwrap_or(fallback_id)
        .to_string()
}

/// Remove provider-internal reasoning from values that may be written to logs or
/// replayed into schema-repair prompts. Structured parsers should still consume
/// the original provider body; this is only for observability and repair context.
pub fn sanitize_provider_observability_value(value: &Value) -> Value {
    match value {
        Value::Array(items) => Value::Array(
            items
                .iter()
                .map(sanitize_provider_observability_value)
                .collect(),
        ),
        Value::Object(map) => {
            if let Some(block_type) = map.get("type").and_then(Value::as_str) {
                if is_provider_reasoning_block_type(block_type) {
                    return serde_json::json!({
                        "type": block_type,
                        "omitted": "provider reasoning omitted"
                    });
                }
            }

            let mut sanitized = serde_json::Map::new();
            for (key, item) in map {
                if is_provider_sensitive_key(key) {
                    sanitized.insert(key.clone(), Value::String("[redacted]".to_string()));
                } else if is_provider_reasoning_key(key) {
                    sanitized.insert(
                        key.clone(),
                        Value::String("[provider reasoning omitted]".to_string()),
                    );
                } else {
                    sanitized.insert(key.clone(), sanitize_provider_observability_value(item));
                }
            }
            Value::Object(sanitized)
        }
        other => other.clone(),
    }
}

fn is_provider_reasoning_block_type(block_type: &str) -> bool {
    matches!(block_type, "thinking" | "redacted_thinking" | "reasoning")
}

fn is_provider_reasoning_key(key: &str) -> bool {
    matches!(
        key,
        "thinking"
            | "signature"
            | "reasoning"
            | "reasoning_content"
            | "chain_of_thought"
            | "thoughts"
            | "redacted_thinking"
    )
}

fn is_provider_sensitive_key(key: &str) -> bool {
    let normalized = key.to_ascii_lowercase().replace(['-', '_'], "");
    matches!(
        normalized.as_str(),
        "apikey"
            | "authorization"
            | "bearer"
            | "credential"
            | "password"
            | "refreshtoken"
            | "secret"
            | "token"
            | "accesstoken"
    )
}

/// Maximum number of retries after a transient failure. With the initial attempt
/// this is at most 2 calls for a transient transport / 429 / 5xx failure; a 4xx
/// (non-429) is never retried — it is a non-transient provider error such as a
/// bad request or an unauthorized/forbidden call, and retrying a paid
/// non-idempotent POST would only duplicate cost without changing the outcome.
pub const MAX_TRANSIENT_RETRIES: u32 = 1;

async fn http_error_message(resp: reqwest::Response, prefix: &str, code: u16) -> String {
    let body = read_error_body_limited(resp).await.unwrap_or_default();
    let body = body.split_whitespace().collect::<Vec<_>>().join(" ");
    if body.is_empty() {
        return format!("{prefix} HTTP {code}");
    }
    format!(
        "{prefix} HTTP {code}: {}",
        compact_text_excerpt(&body, MAX_ERROR_BODY_CHARS)
    )
}

async fn read_error_body_limited(mut resp: reqwest::Response) -> Option<String> {
    if resp
        .content_length()
        .is_some_and(|len| len > MAX_ERROR_BODY_BYTES as u64)
    {
        return Some(format!(
            "error body omitted because it exceeds {MAX_ERROR_BODY_BYTES} bytes"
        ));
    }
    let mut out = Vec::new();
    while let Some(chunk) = resp.chunk().await.ok()? {
        if out.len().saturating_add(chunk.len()) > MAX_ERROR_BODY_BYTES {
            return Some(format!(
                "error body omitted because it exceeds {MAX_ERROR_BODY_BYTES} bytes"
            ));
        }
        out.extend_from_slice(&chunk);
    }
    Some(String::from_utf8_lossy(&out).into_owned())
}

/// Install `ring` as the rustls default crypto provider and build a rustls-backed
/// `reqwest::Client`. `ring` is already used by `ort`'s `tls-rustls`, so this
/// reuses the existing native build rather than introducing `aws-lc-rs`. The
/// client uses the bundled webpki root store (from the `rustls-tls-webpki-roots`
/// feature) for certificate verification; invalid-certificate acceptance and
/// plaintext HTTP for production providers are never enabled (provider config
/// validation already enforces HTTPS-only except localhost dev).
///
/// `install_default` is process-global and idempotent; calling it again after
/// another component installed a provider returns `AlreadyInstalled`, which we
/// treat as success. Providers are constructed at startup on a single thread in
/// `main.rs`; in tests, parallel construction races on `install_default` but the
/// operation is internally synchronized and `AlreadyInstalled` is benign.
pub fn build_rustls_client() -> Result<reqwest::Client, ProviderError> {
    let _ = rustls::crypto::ring::default_provider().install_default();
    reqwest::Client::builder()
        .use_rustls_tls()
        .build()
        .map_err(|_| ProviderError::Provider("failed to build rustls HTTP client".to_string()))
}

/// Encode image bytes as a `data:<mime>;base64,...` URI. Common container formats
/// (PNG / JPEG / WebP / GIF) are detected by magic bytes and passed through
/// verbatim — the host selected the rendition, so the driver sends exactly what
/// it chose. Anything else is decoded with the `image` crate and re-encoded to
/// PNG; if that also fails the driver fails closed with a provider error rather
/// than sending an unrecognizable payload.
pub fn build_image_data_uri(bytes: &[u8]) -> Result<String, ProviderError> {
    let (mime, b64) = detect_image_base64(bytes)?;
    Ok(format!("data:{mime};base64,{b64}"))
}

/// Encode image bytes as a `(media_type, base64)` pair for providers that take
/// raw base64 image content — e.g. the Anthropic Messages `image` source block,
/// which uses `source: { type: "base64", media_type, data }`. Same magic-byte
/// pass-through / PNG re-encode policy as [`build_image_data_uri`]; only the
/// packaging differs (the media type and raw base64 are returned separately
/// instead of a `data:` URI).
pub fn build_image_base64(bytes: &[u8]) -> Result<(String, String), ProviderError> {
    detect_image_base64(bytes)
}

/// Detect the image media type by magic bytes (PNG / JPEG / WebP / GIF) and
/// base64-encode the bytes verbatim, or — for an unrecognized container — decode
/// with the `image` crate and re-encode to PNG (lossless, universally accepted by
/// vision providers). Returns `(media_type, base64)`. Empty input fails closed.
fn detect_image_base64(bytes: &[u8]) -> Result<(String, String), ProviderError> {
    if bytes.is_empty() {
        return Err(ProviderError::Provider("image bytes are empty".to_string()));
    }
    if let Some(mime) = detect_mime(bytes) {
        let b64 = base64::engine::general_purpose::STANDARD.encode(bytes);
        return Ok((mime.to_string(), b64));
    }
    // Re-encode to PNG via the `image` crate (lossless, universally accepted by
    // vision providers). The host's container format is not one we recognize by
    // magic bytes, so decode + re-encode rather than guessing a mime.
    let img = image::load_from_memory(bytes).map_err(|_| {
        ProviderError::Provider("could not decode image for provider upload".to_string())
    })?;
    let mut cursor = std::io::Cursor::new(Vec::new());
    img.write_to(&mut cursor, image::ImageFormat::Png)
        .map_err(|_| {
            ProviderError::Provider("could not encode image for provider upload".to_string())
        })?;
    let b64 = base64::engine::general_purpose::STANDARD.encode(&cursor.into_inner());
    Ok(("image/png".to_string(), b64))
}

fn detect_mime(bytes: &[u8]) -> Option<&'static str> {
    if bytes.len() >= 8 && &bytes[..8] == b"\x89PNG\r\n\x1a\n" {
        Some("image/png")
    } else if bytes.len() >= 3 && &bytes[..3] == [0xFF, 0xD8, 0xFF] {
        Some("image/jpeg")
    } else if bytes.len() >= 12 && &bytes[..4] == b"RIFF" && &bytes[8..12] == b"WEBP" {
        Some("image/webp")
    } else if bytes.len() >= 6 && (&bytes[..6] == b"GIF89a" || &bytes[..6] == b"GIF87a") {
        Some("image/gif")
    } else {
        None
    }
}

/// JSON Schema keys retained when sanitizing a code-owned schema for strict-mode
/// injection. OpenAI / OpenRouter strict structured output supports a limited
/// subset; unsupported keys (`$schema`, `title`, `minLength`, `minItems`,
/// `minimum`, `maximum`, `pattern`, `format`, ...) are dropped so the provider
/// does not reject the schema. The code-owned validator (`validate_understanding`
/// / `validate_rating`) still enforces the dropped constraints on the parsed
/// response, so fail-closed behavior is preserved — the injected schema only
/// guides the model, the validator decides whether the result is acceptable.
const STRICT_KEEP: &[&str] = &[
    "type",
    "description",
    "properties",
    "required",
    "items",
    "enum",
    "anyOf",
    "oneOf",
    "allOf",
    "additionalProperties",
];

/// Produce a strict-compatible JSON Schema `Value` from a code-owned schema
/// string: drop unsupported keys, recursively, and force `required` to list
/// every property at every object level (strict mode requires every property to
/// be required). `additionalProperties: false` is retained.
pub fn strict_schema_value(schema_json: &str) -> Result<Value, ProviderError> {
    let mut v: Value = serde_json::from_str(schema_json).map_err(|_| {
        ProviderError::Provider("code-owned image-analysis schema is not valid JSON".to_string())
    })?;
    sanitize_strict(&mut v);
    Ok(v)
}

fn sanitize_strict(v: &mut Value) {
    let Some(map) = v.as_object_mut() else {
        return;
    };
    // Drop unsupported keys.
    let keys: Vec<String> = map.keys().cloned().collect();
    for k in keys {
        if !STRICT_KEEP.contains(&k.as_str()) {
            map.remove(&k);
        }
    }
    // Recurse into container keys.
    if let Some(props) = map.get_mut("properties").and_then(|p| p.as_object_mut()) {
        for sub in props.values_mut() {
            sanitize_strict(sub);
        }
    }
    if let Some(items) = map.get_mut("items") {
        sanitize_strict(items);
    }
    for key in ["anyOf", "oneOf", "allOf"] {
        if let Some(arr) = map.get_mut(key).and_then(|a| a.as_array_mut()) {
            for sub in arr {
                sanitize_strict(sub);
            }
        }
    }
    // Force required = all property keys (strict mode requires every property to
    // be required). Sorted for deterministic output.
    if let Some(props) = map.get("properties").and_then(|p| p.as_object()) {
        // Force required = all property keys (strict mode requires every property to
        // be required). Collect as owned strings, sort, then lift to Values so the
        // comparison is on `&str` (an `Option<&str>` cmp would need an extra borrow).
        let mut required: Vec<String> = props.keys().cloned().collect();
        required.sort();
        let required: Vec<Value> = required.into_iter().map(Value::String).collect();
        map.insert("required".to_string(), Value::Array(required));
    }
}

/// Send a JSON POST with a bounded retry policy. `headers` are attribution /
/// routing headers set by the driver (never `Authorization` or `Content-Type`,
/// which the transport owns). `bearer` is the resolved secret, sent only as a
/// `Bearer` header; it is never logged. Transient transport errors, 429, and
/// 5xx are retried up to `max_retries` times; 4xx (non-429) is not retried. The
/// response body is drained on the error path but never logged.
pub async fn send_with_retry(
    client: &reqwest::Client,
    url: &str,
    body: &Value,
    headers: &[(String, String)],
    bearer: Option<&str>,
    max_retries: u32,
) -> Result<reqwest::Response, ProviderError> {
    let mut attempt = 0u32;
    loop {
        let mut req = client.post(url).json(body);
        for (k, v) in headers {
            req = req.header(k.as_str(), v.as_str());
        }
        if let Some(token) = bearer {
            req = req.bearer_auth(token);
        }
        match req.send().await {
            Ok(resp) => {
                let status = resp.status();
                if status.is_success() {
                    return Ok(resp);
                }
                let code = status.as_u16();
                let retryable = code == 429 || status.is_server_error();
                let header_req_id = resp
                    .headers()
                    .get("x-request-id")
                    .and_then(|h| h.to_str().ok())
                    .map(|s| s.to_string());
                if retryable && attempt < max_retries {
                    warn!(
                        attempt = attempt + 1,
                        status = code,
                        provider_request_id = header_req_id.as_deref().unwrap_or(""),
                        "provider returned a transient status; retrying"
                    );
                    // Read Retry-After before draining the body, since `text()`
                    // consumes `resp` and would make a later `&resp` a use-after-move.
                    let backoff = retry_after(&resp).unwrap_or(RETRY_BACKOFF);
                    // Drain the body without logging it.
                    let _ = resp.text().await;
                    tokio::time::sleep(backoff).await;
                    attempt += 1;
                    continue;
                }
                // Exhausted retries or non-retryable 4xx. The body is never logged,
                // but a bounded summary is returned to the host and then redacted by
                // the service vault before it reaches the UI.
                let provider_message = http_error_message(resp, "provider returned", code).await;
                if retryable {
                    return Err(ProviderError::Transient);
                }
                return Err(ProviderError::Provider(provider_message));
            }
            Err(err) => {
                if attempt < max_retries {
                    warn!(
                        attempt = attempt + 1,
                        category = transport_error_category(&err),
                        "transport error calling provider; retrying"
                    );
                    tokio::time::sleep(RETRY_BACKOFF).await;
                    attempt += 1;
                    continue;
                }
                return Err(ProviderError::Transient);
            }
        }
    }
}

/// Phase 6c: GET counterpart of [`send_with_retry`] for the model-discovery
/// (`/models`) dry run. Same bounded retry policy (429 + 5xx + transient
/// transport errors retried up to `max_retries`; 4xx non-429 not retried), same
/// redaction discipline (the body is drained on the error path but never
/// logged, and the bearer token is sent only via `bearer_auth` and never
/// logged). `headers` carries routing/attribution headers the driver sets
/// (never `Authorization`); `bearer` is the resolved secret. The response body
/// is the caller's responsibility — this helper returns the raw
/// `reqwest::Response` on a 2xx so the driver can parse the provider's model
/// list shape.
pub async fn send_get_with_retry(
    client: &reqwest::Client,
    url: &str,
    headers: &[(String, String)],
    bearer: Option<&str>,
    max_retries: u32,
) -> Result<reqwest::Response, ProviderError> {
    let mut attempt = 0u32;
    loop {
        let mut req = client.get(url);
        for (k, v) in headers {
            req = req.header(k.as_str(), v.as_str());
        }
        if let Some(token) = bearer {
            req = req.bearer_auth(token);
        }
        match req.send().await {
            Ok(resp) => {
                let status = resp.status();
                if status.is_success() {
                    return Ok(resp);
                }
                let code = status.as_u16();
                let retryable = code == 429 || status.is_server_error();
                if retryable && attempt < max_retries {
                    warn!(
                        attempt = attempt + 1,
                        status = code,
                        "provider model-list returned a transient status; retrying"
                    );
                    let backoff = retry_after(&resp).unwrap_or(RETRY_BACKOFF);
                    let _ = resp.text().await;
                    tokio::time::sleep(backoff).await;
                    attempt += 1;
                    continue;
                }
                let provider_message =
                    http_error_message(resp, "provider model-list returned", code).await;
                if retryable {
                    return Err(ProviderError::Transient);
                }
                return Err(ProviderError::Provider(provider_message));
            }
            Err(err) => {
                if attempt < max_retries {
                    warn!(
                        attempt = attempt + 1,
                        category = transport_error_category(&err),
                        "transport error calling provider model-list; retrying"
                    );
                    tokio::time::sleep(RETRY_BACKOFF).await;
                    attempt += 1;
                    continue;
                }
                return Err(ProviderError::Provider(transport_error_message(
                    "provider model-list transport error",
                    &err,
                )));
            }
        }
    }
}

fn transport_error_category(err: &reqwest::Error) -> &'static str {
    if err.is_connect() {
        "connect"
    } else if err.is_timeout() {
        "timeout"
    } else if err.is_decode() {
        "body-decode"
    } else {
        "other"
    }
}

fn transport_error_message(context: &str, err: &reqwest::Error) -> String {
    format!("{context}: {} ({err})", transport_error_category(err))
}

/// Parse a `Retry-After` header (seconds form only; the HTTP-date form is
/// intentionally not supported to keep the policy simple and bounded). Capped at
/// `MAX_RETRY_AFTER` so a server cannot stall the sidecar.
fn retry_after(resp: &reqwest::Response) -> Option<Duration> {
    let header = resp.headers().get("retry-after")?.to_str().ok()?;
    let secs: u64 = header.trim().parse().ok()?;
    Some(Duration::from_secs(secs).min(MAX_RETRY_AFTER))
}

/// Parse a content string (the model's JSON text) into a `Value`. Tolerates a
/// model that wraps the JSON in markdown fences (``` ```json … ``` ``` or
/// ``` ``` … ``` ```) or embeds it in prose, since many compatible providers /
/// models emit structured output as text despite a `json_schema`
/// `response_format`. A content string with no JSON object/array at all still
/// fails closed as a `SchemaValidationMessage` so the service reports a
/// payload-decode failure and creates no active annotation.
pub fn parse_content_json(content: &str) -> Result<Value, ProviderError> {
    // Fast path: the content is clean JSON.
    if let Ok(v) = serde_json::from_str::<Value>(content) {
        return Ok(v);
    }
    // Fallback: fenced or prose-embedded JSON. Accept it rather than discarding
    // a valid result and burning a paid retry the model tends to repeat.
    if let Some(v) = extract_json_from_text_block(content) {
        return Ok(v);
    }
    Err(ProviderError::SchemaValidationMessage(format!(
        "provider response content was not valid JSON; content excerpt: {}",
        compact_text_excerpt(content, 220)
    )))
}

/// Tolerantly extract a JSON object or array from a model text block that may
/// wrap the JSON in markdown fences (``` ```json … ``` ``` or ``` ``` … ``` ```)
/// or embed it in surrounding prose. Returns the first `Value` that parses, or
/// `None` when no JSON object/array can be found.
///
/// This is the fallback path for providers that accept a structured-output
/// request (Anthropic `tools` + `tool_choice`, or OpenAI
/// `response_format: json_schema`) but whose underlying model emits the
/// tool/json arguments as a `text` content block instead of a native structured
/// block. The Volcengine Ark Coding Plan serving Qwen does this consistently:
/// the shim honors `tool_choice` at the API layer but the model writes the JSON
/// out as text (often markdown-fenced). Accepting the JSON from the text block
/// keeps those responses usable instead of discarding a valid result and
/// burning a paid retry on a repair that the model typically repeats the same
/// way.
pub fn extract_json_from_text_block(text: &str) -> Option<Value> {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return None;
    }
    // 1) Markdown-fenced block: ```json\n{...}\n``` (or ```\n{...}\n```). The
    //    fence itself contains no braces, so the JSON inside is extracted whole.
    if let Some(inner) = extract_fenced_inner(trimmed) {
        if let Ok(v) = serde_json::from_str::<Value>(inner.trim()) {
            return Some(v);
        }
        // The fenced content may still carry framing prose; scan it for the
        // first balanced JSON region.
        if let Some(v) = first_balanced_json(inner) {
            return Some(v);
        }
    }
    // 2) The whole text is JSON.
    if let Ok(v) = serde_json::from_str::<Value>(trimmed) {
        return Some(v);
    }
    // 3) JSON embedded in prose: scan for the first balanced object/array.
    first_balanced_json(trimmed)
}

/// Extract the inner content of the first markdown code fence, without the
/// opening fence line or the closing fence.
fn extract_fenced_inner(text: &str) -> Option<&str> {
    let start = text.find("```")?;
    let after_open = &text[start + 3..];
    // The opening fence line may carry a language tag (e.g. `json`); skip to
    // the newline that ends it.
    let nl = after_open.find('\n')?;
    let content_start = start + 3 + nl + 1;
    let rest = &text[content_start..];
    let close = rest.find("```")?;
    Some(&rest[..close])
}

/// Scan `text` for the first balanced `{ … }` or `[ … ]` region that parses as
/// valid JSON, restarting the search after each candidate so a stray non-JSON
/// brace in prose does not prevent a later valid object from being found.
/// String literals and `\` escapes are respected so braces inside strings do
/// not unbalance the scan.
fn first_balanced_json(text: &str) -> Option<Value> {
    let bytes = text.as_bytes();
    let mut search_from = 0usize;
    while search_from < bytes.len() {
        let rel = bytes[search_from..]
            .iter()
            .position(|&c| c == b'{' || c == b'[')?;
        let start = search_from + rel;
        let open = bytes[start];
        let close = if open == b'{' { b'}' } else { b']' };
        match balanced_end(bytes, start, open, close) {
            Some(end) => {
                let candidate = &text[start..=end];
                if let Ok(v) = serde_json::from_str::<Value>(candidate) {
                    return Some(v);
                }
                // This region did not parse; try the next one after it.
                search_from = end + 1;
            }
            None => break,
        }
    }
    None
}

/// Find the index of the bracket that closes the `open` at `start`, respecting
/// nested brackets of the same kind, string literals, and `\` escapes. Returns
/// `None` if the opener is never closed.
fn balanced_end(bytes: &[u8], start: usize, open: u8, close: u8) -> Option<usize> {
    let mut depth = 0i32;
    let mut in_string = false;
    let mut escape = false;
    for (i, &c) in bytes[start..].iter().enumerate() {
        if in_string {
            if escape {
                escape = false;
            } else if c == b'\\' {
                escape = true;
            } else if c == b'"' {
                in_string = false;
            }
        } else if c == b'"' {
            in_string = true;
        } else if c == open {
            depth += 1;
        } else if c == close {
            depth -= 1;
            if depth == 0 {
                return Some(start + i);
            }
        }
    }
    None
}

/// Coerce a model-emitted `rating` value to the 1..=5 integer the contract
/// requires, WITHOUT silently truncating a fractional float.
///
/// The remote LLM is asked for an `integer` rating, but some models still emit
/// an integer-valued float (e.g. `4.0`); that form is accepted and coerced to
/// the integer 4. A genuinely fractional float (e.g. `4.9`) is schema-invalid
/// — truncating it to 4 would silently normalize bad output into a valid
/// rating, so it is rejected here (returns `None`). The caller maps `None` to
/// the out-of-contract sentinel 0, which `validate_rating` then turns into a
/// `SchemaValidation` error (fail closed, no active annotation). Non-numeric
/// values are likewise `None`.
pub fn parse_rating_int(value: &Value) -> Option<i32> {
    // An exact integer (JSON `4`) — the common, contract-correct case.
    if let Some(i) = value.as_i64() {
        return Some(i as i32);
    }
    // An integer-valued float (JSON `4.0`) — accept and coerce. A fractional
    // float (JSON `4.9`) has `fract() != 0.0` and falls through to `None`,
    // so it is NOT truncated into a valid rating.
    if let Some(f) = value.as_f64() {
        if f.fract() == 0.0 && f >= i32::MIN as f64 && f <= i32::MAX as f64 {
            return Some(f as i32);
        }
    }
    None
}

/// Tolerantly extract usage from a provider usage object. OpenRouter follows
/// OpenAI Chat (`prompt_tokens` / `completion_tokens` / `total_tokens`); Ark
/// Responses follows OpenAI Responses (`input_tokens` / `output_tokens` /
/// `total_tokens`). Accept either field name; 0 means "not reported".
pub fn extract_usage(usage: Option<&Value>) -> Usage {
    let Some(usage) = usage else {
        return Usage::default();
    };
    let input = usage
        .get("input_tokens")
        .or_else(|| usage.get("prompt_tokens"))
        .and_then(|v| v.as_i64())
        .unwrap_or(0);
    let output = usage
        .get("output_tokens")
        .or_else(|| usage.get("completion_tokens"))
        .and_then(|v| v.as_i64())
        .unwrap_or(0);
    let total = usage
        .get("total_tokens")
        .and_then(|v| v.as_i64())
        .unwrap_or(0);
    Usage {
        input_tokens: input,
        output_tokens: output,
        total_tokens: total,
    }
}

/// Resolve a JSON Pointer to a `&Value`. An empty pointer means the whole
/// document. Returns `None` if the pointer does not resolve.
pub fn json_pointer_str<'a>(root: &'a Value, pointer: &str) -> Option<&'a Value> {
    if pointer.is_empty() {
        Some(root)
    } else {
        root.pointer(pointer)
    }
}

/// Extract the provider's own request id from a response, when the compatible
/// endpoint reports one. Compatible providers do not all report the id the same
/// way: some echo it in a response header (e.g. Opencode's `request-id`), some
/// embed it in the JSON body (e.g. OpenAI Chat's `id`), and some report neither.
/// Both sources are therefore optional in the provider config:
/// `provider_request_id_header` (a header name) and `provider_request_id_json_pointer`
/// (a JSON Pointer into the body). The header takes precedence when both are set
/// and present; a missing/empty header falls through to the pointer; a missing
/// pointer yields an empty string. The field stays optional end to end — an empty
/// result means "not reported", not an error (Phase 6b deliverable).
pub fn extract_provider_request_id(
    headers: &reqwest::header::HeaderMap,
    body: &Value,
    header_name: Option<&str>,
    json_pointer: Option<&str>,
) -> String {
    if let Some(name) = header_name {
        if let Some(val) = headers.get(name).and_then(|h| h.to_str().ok()) {
            if !val.is_empty() {
                return val.to_string();
            }
        }
    }
    if let Some(pointer) = json_pointer {
        if let Some(v) = json_pointer_str(body, pointer) {
            if let Some(s) = v.as_str() {
                return s.to_string();
            }
        }
    }
    String::new()
}

/// Read a provider response into `(headers, body)` so a driver can capture the
/// provider request id from EITHER a response header OR a body JSON pointer
/// (compatible providers report it inconsistently — Opencode's Anthropic endpoint
/// echoes `request-id` as a header, OpenAI Chat embeds `id` in the body). The
/// headers are cloned (cheap, one response) before the body is consumed. A
/// non-JSON body maps to `SchemaValidation` — the service reports a payload-decode
/// failure and creates no active annotation.
pub async fn read_response(
    mut resp: reqwest::Response,
) -> Result<(reqwest::header::HeaderMap, Value), ProviderError> {
    let headers = resp.headers().clone();
    let bytes = read_body_limited(&mut resp, MAX_PROVIDER_RESPONSE_BYTES).await?;
    let body: Value = serde_json::from_slice(&bytes).map_err(|err| {
        let text = String::from_utf8_lossy(&bytes);
        ProviderError::SchemaValidationMessage(format!(
            "provider response body was not valid JSON: {err}; body excerpt: {}",
            compact_text_excerpt(&text, 220)
        ))
    })?;
    Ok((headers, body))
}

async fn read_body_limited(
    resp: &mut reqwest::Response,
    max_bytes: usize,
) -> Result<Vec<u8>, ProviderError> {
    if resp
        .content_length()
        .is_some_and(|len| len > max_bytes as u64)
    {
        return Err(ProviderError::Provider(format!(
            "provider response body exceeded {max_bytes} bytes"
        )));
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = resp.chunk().await.map_err(|err| {
        ProviderError::SchemaValidationMessage(format!(
            "provider response body could not be read: {err}"
        ))
    })? {
        if bytes.len().saturating_add(chunk.len()) > max_bytes {
            return Err(ProviderError::Provider(format!(
                "provider response body exceeded {max_bytes} bytes"
            )));
        }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detects_common_container_mimes() {
        // PNG magic bytes.
        let png = b"\x89PNG\r\n\x1a\n\x00\x00\x00";
        assert_eq!(detect_mime(png), Some("image/png"));
        // JPEG magic bytes.
        let jpeg = [0xFFu8, 0xD8, 0xFF, 0xE0, 0x00, 0x10];
        assert_eq!(detect_mime(&jpeg), Some("image/jpeg"));
        // Unknown bytes -> None (caller re-encodes via the image crate).
        assert_eq!(detect_mime(&[0u8, 1, 2, 3, 4, 5, 6, 7]), None);
    }

    #[test]
    fn build_image_base64_returns_mime_and_base64() {
        use base64::Engine;
        // A real 2x2 PNG: magic-byte pass-through yields ("image/png", <STANDARD b64>),
        // and the data-uri form packages exactly that base64.
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([1, 2, 3]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        let png = cursor.into_inner();
        let (mime, b64) = build_image_base64(&png).expect("png passes through");
        assert_eq!(mime, "image/png");
        assert_eq!(b64, base64::engine::general_purpose::STANDARD.encode(&png));
        assert_eq!(
            build_image_data_uri(&png).unwrap(),
            format!("data:image/png;base64,{b64}")
        );
    }

    #[test]
    fn build_data_uri_passes_through_png() {
        // A real 2x2 PNG encoded by the image crate.
        let img = image::RgbImage::from_pixel(2, 2, image::Rgb([1, 2, 3]));
        let mut cursor = std::io::Cursor::new(Vec::new());
        image::DynamicImage::ImageRgb8(img)
            .write_to(&mut cursor, image::ImageFormat::Png)
            .expect("encode png");
        let png = cursor.into_inner();
        let uri = build_image_data_uri(&png).expect("png passes through");
        assert!(uri.starts_with("data:image/png;base64,"), "{uri}");
    }

    #[test]
    fn build_data_uri_reencodes_unknown_bytes_to_png() {
        // Not a recognized container; the image crate cannot decode arbitrary
        // bytes, so this fails closed rather than sending garbage.
        let err = build_image_data_uri(&[0u8; 16]).unwrap_err();
        assert!(matches!(err, ProviderError::Provider(_)));
    }

    #[test]
    fn build_data_uri_rejects_empty_bytes() {
        let err = build_image_data_uri(&[]).unwrap_err();
        assert!(matches!(err, ProviderError::Provider(_)));
    }

    #[test]
    fn strict_schema_drops_unsupported_keys_and_forces_required() {
        let schema = crate::service::image_analysis::image_analysis_schema_json(
            crate::service::image_analysis::ImageAnalysisSchemaSpec::describe(),
        );
        let v = strict_schema_value(&schema).expect("understanding schema sanitizes");
        // $schema and title dropped.
        assert!(v.get("$schema").is_none());
        assert!(v.get("title").is_none());
        // Every property is required (sorted).
        let required: Vec<&str> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert_eq!(required, vec!["description"]);
        // additionalProperties retained.
        assert_eq!(v["additionalProperties"], false);
        // minLength dropped.
        assert!(v["properties"]["description"].get("minLength").is_none());
    }

    #[test]
    fn strict_schema_forces_required_on_rating_properties() {
        let schema = crate::service::image_analysis::image_analysis_schema_json(
            crate::service::image_analysis::ImageAnalysisSchemaSpec::score(true),
        );
        let v = strict_schema_value(&schema).expect("rating schema sanitizes");
        // The rating contract is a flat object: rating + rating_reason.
        // Strict mode forces every property to be required (sorted), and
        // drops the `minimum`/`maximum` range constraints (the code-owned
        // `validate_rating` re-enforces 1..=5 on the parsed response, so fail-closed
        // behavior is preserved — the injected schema only guides the model).
        let required: Vec<&str> = v["required"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert_eq!(required, vec!["rating", "rating_reason"]);
        assert_eq!(v["additionalProperties"], false);
        assert_eq!(v["properties"]["rating"]["type"], "integer");
        assert!(
            v["properties"]["rating"].get("minimum").is_none(),
            "minimum dropped by strict sanitization"
        );
        assert!(
            v["properties"]["rating"].get("maximum").is_none(),
            "maximum dropped by strict sanitization"
        );
        // No nested `scores` array and no `confidence` remain after sanitization.
        assert!(
            v["properties"].get("scores").is_none(),
            "scores array still present"
        );
        assert!(
            v["properties"].get("confidence").is_none(),
            "confidence still present"
        );
    }

    #[test]
    fn sanitized_provider_excerpt_omits_reasoning_but_keeps_final_output() {
        let body = serde_json::json!({
            "content": [
                {
                    "type": "thinking",
                    "thinking": "private chain of thought",
                    "signature": "signed-reasoning"
                },
                {
                    "type": "tool_use",
                    "name": "alcedo_image_understanding",
                    "input": {
                        "caption": "sunrise",
                        "tags": ["sun"],
                        "scene": "outdoor",
                        "confidence": 0.9
                    }
                }
            ],
            "choices": [{
                "message": {
                    "reasoning_content": "hidden reasoning",
                    "content": "{\"caption\":\"sunrise\"}"
                }
            }]
        });

        let excerpt = sanitized_provider_json_excerpt(&body, 1200);
        assert!(!excerpt.contains("private chain of thought"), "{excerpt}");
        assert!(!excerpt.contains("signed-reasoning"), "{excerpt}");
        assert!(!excerpt.contains("hidden reasoning"), "{excerpt}");
        assert!(excerpt.contains("provider reasoning omitted"), "{excerpt}");
        assert!(excerpt.contains("alcedo_image_understanding"), "{excerpt}");
        assert!(excerpt.contains("sunrise"), "{excerpt}");
    }

    #[test]
    fn extract_usage_tolerates_openai_chat_and_responses_field_names() {
        // OpenAI Chat shape.
        let chat: Value = serde_json::from_str(
            r#"{"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}"#,
        )
        .unwrap();
        let u = extract_usage(Some(&chat));
        assert_eq!(u.input_tokens, 10);
        assert_eq!(u.output_tokens, 5);
        assert_eq!(u.total_tokens, 15);

        // OpenAI Responses shape.
        let resp: Value =
            serde_json::from_str(r#"{"input_tokens": 7, "output_tokens": 3, "total_tokens": 10}"#)
                .unwrap();
        let u = extract_usage(Some(&resp));
        assert_eq!(u.input_tokens, 7);
        assert_eq!(u.output_tokens, 3);
        assert_eq!(u.total_tokens, 10);

        // Missing usage -> default zeros.
        assert_eq!(extract_usage(None), Usage::default());
    }

    #[test]
    fn parse_content_json_rejects_non_json() {
        let err = parse_content_json("not json").unwrap_err();
        assert!(
            matches!(
                err,
                ProviderError::SchemaValidation | ProviderError::SchemaValidationMessage(_)
            ),
            "expected schema validation, got {err:?}"
        );
        let ok = parse_content_json(r#"{"caption": "c"}"#).expect("valid json");
        assert_eq!(ok["caption"], "c");
    }

    #[test]
    fn parse_content_json_tolerates_markdown_fenced_json() {
        // A model that wraps its JSON in a ```json fence despite a json_schema
        // response_format must still parse: the fence is not a reason to discard
        // a valid result.
        let fenced = "```json\n{\"caption\": \"c\", \"tags\": [\"t\"]}\n```";
        let v = parse_content_json(fenced).expect("fenced json parses");
        assert_eq!(v["caption"], "c");
        assert_eq!(v["tags"][0], "t");

        // Fence without a language tag also parses.
        let bare_fence = "```\n{\"caption\": \"c\"}\n```";
        let v = parse_content_json(bare_fence).expect("bare-fenced json parses");
        assert_eq!(v["caption"], "c");
    }

    #[test]
    fn parse_content_json_tolerates_json_embedded_in_prose() {
        let prose =
            "Here is the analysis:\n{\"caption\": \"c\", \"tags\": [\"t\"]}\nHope it helps.";
        let v = parse_content_json(prose).expect("embedded json parses");
        assert_eq!(v["caption"], "c");
        assert_eq!(v["tags"][0], "t");
    }

    #[test]
    fn extract_json_from_text_block_handles_fenced_raw_and_prose() {
        // Raw JSON.
        let v = extract_json_from_text_block(r#"{"caption": "c"}"#).expect("raw");
        assert_eq!(v["caption"], "c");
        // ```json-fenced.
        let v = extract_json_from_text_block("```json\n{\"a\": 1}\n```").expect("fenced");
        assert_eq!(v["a"], 1);
        // Bare-fenced.
        let v = extract_json_from_text_block("```\n{\"a\": 2}\n```").expect("bare fenced");
        assert_eq!(v["a"], 2);
        // Embedded in prose (a stray non-JSON brace before the real object must
        // not stop the scan from finding the valid JSON later).
        let v = extract_json_from_text_block("result: {bad} but real: {\"a\": 3}").expect("prose");
        assert_eq!(v["a"], 3);
        // Top-level array.
        let v = extract_json_from_text_block("[1, 2, 3]").expect("array");
        assert_eq!(v[2], 3);
        // Braces inside strings do not unbalance the scan.
        let v =
            extract_json_from_text_block(r#"{"caption": "a {b} c"}"#).expect("braces in string");
        assert_eq!(v["caption"], "a {b} c");
    }

    #[test]
    fn extract_json_from_text_block_returns_none_for_non_json() {
        assert!(extract_json_from_text_block("not json at all").is_none());
        assert!(extract_json_from_text_block("").is_none());
        assert!(extract_json_from_text_block("   ").is_none());
        // Unclosed brace is not valid JSON.
        assert!(extract_json_from_text_block(r#"{"caption": "c""#).is_none());
    }

    #[test]
    fn json_pointer_resolves_root_and_paths() {
        let v: Value = serde_json::from_str(r#"{"a": {"b": 1}}"#).unwrap();
        assert_eq!(json_pointer_str(&v, "").unwrap(), &v);
        assert_eq!(json_pointer_str(&v, "/a/b").unwrap(), &1);
        assert!(json_pointer_str(&v, "/missing").is_none());
    }

    #[test]
    fn parse_rating_int_accepts_integer_and_integer_valued_float() {
        // Exact integer (the contract-correct form).
        assert_eq!(parse_rating_int(&serde_json::json!(4)), Some(4));
        // Integer-valued float a model may emit despite the `integer` schema.
        assert_eq!(parse_rating_int(&serde_json::json!(5.0)), Some(5));
        assert_eq!(parse_rating_int(&serde_json::json!(1.0)), Some(1));
    }

    #[test]
    fn parse_rating_int_rejects_fractional_float_without_truncation() {
        // A fractional rating is schema-invalid. It must NOT be truncated to 4
        // and pass validation — fail closed (None -> 0 -> SchemaValidation).
        assert_eq!(parse_rating_int(&serde_json::json!(4.9)), None);
        assert_eq!(parse_rating_int(&serde_json::json!(0.5)), None);
        assert_eq!(parse_rating_int(&serde_json::json!(-1.2)), None);
    }

    #[test]
    fn parse_rating_int_rejects_non_numeric() {
        assert_eq!(parse_rating_int(&serde_json::json!("4")), None);
        assert_eq!(parse_rating_int(&serde_json::json!(null)), None);
        assert_eq!(parse_rating_int(&serde_json::json!({"rating": 4})), None);
    }

    #[test]
    fn extract_provider_request_id_prefers_header_then_pointer() {
        use reqwest::header::{HeaderMap, HeaderValue};
        // Header set + pointer set -> header wins.
        let mut headers = HeaderMap::new();
        headers.insert("request-id", HeaderValue::from_static("hdr-123"));
        let body: Value = serde_json::from_str(r#"{"id": "body-123"}"#).unwrap();
        assert_eq!(
            extract_provider_request_id(&headers, &body, Some("request-id"), Some("/id")),
            "hdr-123"
        );
        // Header name set but absent -> fall through to pointer.
        let headers = HeaderMap::new();
        assert_eq!(
            extract_provider_request_id(&headers, &body, Some("request-id"), Some("/id")),
            "body-123"
        );
        // Only pointer set -> pointer.
        assert_eq!(
            extract_provider_request_id(&headers, &body, None, Some("/id")),
            "body-123"
        );
        // Only header set -> header.
        let mut headers = HeaderMap::new();
        headers.insert("request-id", HeaderValue::from_static("hdr-only"));
        assert_eq!(
            extract_provider_request_id(&headers, &body, Some("request-id"), None),
            "hdr-only"
        );
        // Neither set / empty header value -> empty string (not reported).
        let headers = HeaderMap::new();
        assert_eq!(extract_provider_request_id(&headers, &body, None, None), "");
        let mut headers = HeaderMap::new();
        headers.insert("request-id", HeaderValue::from_str("").unwrap());
        assert_eq!(
            extract_provider_request_id(&headers, &body, Some("request-id"), None),
            ""
        );
    }
}
