use serde_json::{Value, json};

use crate::service::image_analysis::ProviderError;
use crate::service::providers::http_util::{compact_text_excerpt, sanitized_provider_json_excerpt};

const MAX_SSE_EVENT_BYTES: usize = 4 * 1024 * 1024;
const MAX_SSE_TEXT_BYTES: usize = 4 * 1024 * 1024;

#[derive(Default)]
struct SseEvent {
    event: Option<String>,
    data: Option<String>,
}

pub async fn collect_completed_response_from_sse(
    mut resp: reqwest::Response,
) -> Result<(reqwest::header::HeaderMap, Value), ProviderError> {
    let headers = resp.headers().clone();
    let mut parser = SseParser::default();
    let mut collector = CodexResponseStreamCollector::default();

    while let Some(chunk) = resp.chunk().await.map_err(|err| {
        ProviderError::Provider(format!("Codex response stream read failed: {err}"))
    })? {
        for event in parser.push(&chunk)? {
            if let Some(response) = collector.push(event)? {
                return Ok((headers, response));
            }
        }
    }

    for event in parser.finish()? {
        if let Some(response) = collector.push(event)? {
            return Ok((headers, response));
        }
    }

    collector.finish().map(|response| (headers, response))
}

#[derive(Default)]
struct CodexResponseStreamCollector {
    latest_response: Option<Value>,
    latest_error: Option<Value>,
    text_delta: String,
    done_text: Option<String>,
}

impl CodexResponseStreamCollector {
    fn push(&mut self, event: SseEvent) -> Result<Option<Value>, ProviderError> {
        let Some(data) = event.data else {
            return Ok(None);
        };
        if data.trim() == "[DONE]" {
            return Ok(None);
        }
        let parsed: Value = serde_json::from_str(&data).map_err(|err| {
            ProviderError::SchemaValidationMessage(format!(
                "Codex SSE event was not valid JSON: {err}; event excerpt: {}",
                compact_text_excerpt(&data, 220)
            ))
        })?;

        let event_type = event
            .event
            .as_deref()
            .or_else(|| parsed.get("type").and_then(Value::as_str));

        if matches!(event_type, Some("error")) || parsed.get("error").is_some() {
            self.latest_error = Some(parsed);
            return Ok(None);
        }

        if matches!(event_type, Some("response.output_text.delta")) {
            if let Some(delta) = parsed.get("delta").and_then(Value::as_str) {
                if self.text_delta.len().saturating_add(delta.len()) > MAX_SSE_TEXT_BYTES {
                    return Err(ProviderError::Provider(format!(
                        "Codex SSE output text exceeded {MAX_SSE_TEXT_BYTES} bytes"
                    )));
                }
                self.text_delta.push_str(delta);
            }
        }

        if matches!(event_type, Some("response.output_text.done")) {
            if let Some(text) = parsed.get("text").and_then(Value::as_str) {
                if text.len() > MAX_SSE_TEXT_BYTES {
                    return Err(ProviderError::Provider(format!(
                        "Codex SSE completed text exceeded {MAX_SSE_TEXT_BYTES} bytes"
                    )));
                }
                self.done_text = Some(text.to_string());
            }
        }

        if let Some(response) = parsed.get("response") {
            self.latest_response = Some(response.clone());
            let completed = matches!(event_type, Some("response.completed"))
                || response
                    .get("status")
                    .and_then(Value::as_str)
                    .map(|status| status == "completed")
                    .unwrap_or(false);
            if completed {
                return Ok(Some(self.completed_response(response.clone())));
            }
        }

        Ok(None)
    }

    fn finish(self) -> Result<Value, ProviderError> {
        if let Some(response) = self.latest_response.as_ref() {
            return Ok(self.completed_response(response.clone()));
        }
        let text = self.done_text.unwrap_or(self.text_delta);
        if !text.is_empty() {
            return Ok(json!({
                "id": "",
                "output_text": text
            }));
        }
        let error_excerpt = self
            .latest_error
            .as_ref()
            .map(|v| format!(" Last error: {}", sanitized_provider_json_excerpt(v, 700)))
            .unwrap_or_default();
        Err(ProviderError::SchemaValidationMessage(format!(
            "Codex SSE stream ended without a completed response.{error_excerpt}"
        )))
    }

    fn completed_response(&self, mut response: Value) -> Value {
        let text = self
            .done_text
            .as_deref()
            .filter(|s| !s.is_empty())
            .or_else(|| (!self.text_delta.is_empty()).then_some(self.text_delta.as_str()));
        if let Some(text) = text {
            if !response_has_output_text(&response) {
                response["output_text"] = Value::String(text.to_string());
            }
        }
        response
    }
}

fn response_has_output_text(response: &Value) -> bool {
    response
        .get("output_text")
        .and_then(Value::as_str)
        .is_some()
        || response
            .pointer("/output/0/content/0/text")
            .and_then(Value::as_str)
            .is_some()
        || response
            .pointer("/output/0/content/0/output_text")
            .and_then(Value::as_str)
            .is_some()
}

#[derive(Default)]
struct SseParser {
    buffer: Vec<u8>,
}

impl SseParser {
    fn push(&mut self, chunk: &[u8]) -> Result<Vec<SseEvent>, ProviderError> {
        if self.buffer.len().saturating_add(chunk.len()) > MAX_SSE_EVENT_BYTES {
            return Err(ProviderError::Provider(format!(
                "Codex SSE event exceeded {MAX_SSE_EVENT_BYTES} bytes"
            )));
        }
        self.buffer.extend_from_slice(chunk);
        let mut events = Vec::new();
        while let Some((index, len)) = find_separator(&self.buffer) {
            let block = self.buffer[..index].to_vec();
            self.buffer.drain(..index + len);
            if !block.iter().all(|b| b.is_ascii_whitespace()) {
                events.push(parse_event_block(&block)?);
            }
        }
        Ok(events)
    }

    fn finish(self) -> Result<Vec<SseEvent>, ProviderError> {
        if self.buffer.iter().all(|b| b.is_ascii_whitespace()) {
            return Ok(Vec::new());
        }
        Ok(vec![parse_event_block(&self.buffer)?])
    }
}

fn find_separator(buffer: &[u8]) -> Option<(usize, usize)> {
    buffer
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .map(|index| (index, 4))
        .or_else(|| {
            buffer
                .windows(2)
                .position(|w| w == b"\n\n")
                .map(|index| (index, 2))
        })
}

fn parse_event_block(block: &[u8]) -> Result<SseEvent, ProviderError> {
    let text = std::str::from_utf8(block).map_err(|err| {
        ProviderError::SchemaValidationMessage(format!(
            "Codex SSE block was not valid UTF-8: {err}"
        ))
    })?;
    let mut event = SseEvent::default();
    let mut data_lines = Vec::new();
    for line in text.lines() {
        if let Some(value) = line.strip_prefix("event:") {
            event.event = Some(value.trim().to_string());
            continue;
        }
        if let Some(value) = line.strip_prefix("data:") {
            let value = value.strip_prefix(' ').unwrap_or(value);
            data_lines.push(value.to_string());
        }
    }
    if !data_lines.is_empty() {
        event.data = Some(data_lines.join("\n"));
    }
    Ok(event)
}
