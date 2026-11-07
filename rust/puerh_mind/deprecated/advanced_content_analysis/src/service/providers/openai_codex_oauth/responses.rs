use serde_json::{Value, json};

use crate::service::image_analysis::{
    AnalyzeImageInput, AnalyzeOutcome, BatchAnalyzeOutcome, DescribeOutcome, ProviderError,
    ScoreOutcome, Usage, output_confidence, output_description, output_rating_reason,
    output_rubric_id, output_rubric_version, output_scene, output_tags, validate_analyze,
    validate_batch_analyze, validate_rating, validate_understanding,
};
use crate::service::provider_config::ModelConfig;
use crate::service::providers::http_util::{
    extract_usage, json_pointer_str, parse_content_json, parse_rating_int,
    sanitized_provider_json_excerpt,
};

pub const UNDERSTANDING_SCHEMA_NAME: &str = "alcedo_image_understanding";
pub const RATING_SCHEMA_NAME: &str = "alcedo_image_rating";
pub const ANALYSIS_FLAT_SCHEMA_NAME: &str = "alcedo_image_analysis_flat";
pub const ANALYSIS_BATCH_SCHEMA_NAME: &str = "alcedo_image_analysis_batch";

pub fn build_single_image_body(
    slug: &str,
    model: Option<&ModelConfig>,
    data_uri: &str,
    schema: Value,
    schema_name: &str,
    strict: bool,
    system: &str,
    instruction: &str,
) -> Value {
    let mut body = json!({
        "model": slug,
        "instructions": "",
        "store": false,
        "stream": true,
        "input": [
            {
                "role": "system",
                "content": [
                    { "type": "input_text", "text": system }
                ]
            },
            {
                "role": "user",
                "content": [
                    { "type": "input_text", "text": instruction },
                    { "type": "input_image", "image_url": data_uri }
                ]
            }
        ],
        "text": {
            "format": {
                "type": "json_schema",
                "name": schema_name,
                "strict": strict,
                "schema": schema
            }
        }
    });
    if let Some(privacy) = model.and_then(|m| m.data_collection.as_deref()) {
        if privacy == "deny" {
            body["data_collection"] = Value::String("deny".to_string());
        }
    }
    body
}

pub fn build_batch_body(
    slug: &str,
    data_uris: &[String],
    schema: Value,
    strict: bool,
    system: &str,
    instruction: &str,
) -> Value {
    let mut user_content = vec![json!({ "type": "input_text", "text": instruction })];
    for (index, data_uri) in data_uris.iter().enumerate() {
        user_content.push(json!({ "type": "input_text", "text": format!("Image index {index}:") }));
        user_content.push(json!({ "type": "input_image", "image_url": data_uri }));
    }
    json!({
        "model": slug,
        "instructions": "",
        "store": false,
        "stream": true,
        "input": [
            {
                "role": "system",
                "content": [
                    { "type": "input_text", "text": system }
                ]
            },
            {
                "role": "user",
                "content": user_content
            }
        ],
        "text": {
            "format": {
                "type": "json_schema",
                "name": ANALYSIS_BATCH_SCHEMA_NAME,
                "strict": strict,
                "schema": schema
            }
        }
    })
}

pub fn parse_describe(
    body: &Value,
    model_id: &str,
    usage_pointer: Option<&str>,
    request_id: &str,
) -> Result<DescribeOutcome, ProviderError> {
    let parsed = output_json(body)?;
    let out = DescribeOutcome {
        caption: output_description(&parsed),
        tags: output_tags(&parsed),
        scene: output_scene(&parsed),
        confidence: output_confidence(&parsed),
        model_id: model_id.to_string(),
        usage: usage_from(body, usage_pointer),
        provider_request_id: request_id.to_string(),
    };
    validate_understanding(&out)?;
    Ok(out)
}

pub fn parse_score(
    body: &Value,
    model_id: &str,
    usage_pointer: Option<&str>,
    request_id: &str,
) -> Result<ScoreOutcome, ProviderError> {
    let parsed = output_json(body)?;
    let out = ScoreOutcome {
        rating: parsed.get("rating").and_then(parse_rating_int).unwrap_or(0),
        rubric_id: output_rubric_id(&parsed),
        rubric_version: output_rubric_version(&parsed),
        reasons: output_rating_reason(&parsed),
        model_id: model_id.to_string(),
        usage: usage_from(body, usage_pointer),
        provider_request_id: request_id.to_string(),
    };
    validate_rating(&out)?;
    Ok(out)
}

pub fn parse_analyze(
    body: &Value,
    model_id: &str,
    usage_pointer: Option<&str>,
    request_id: &str,
    include_understanding: bool,
    include_rating: bool,
) -> Result<AnalyzeOutcome, ProviderError> {
    let parsed = output_json(body)?;
    let usage = usage_from(body, usage_pointer);
    let understanding = include_understanding.then(|| DescribeOutcome {
        caption: output_description(&parsed),
        tags: output_tags(&parsed),
        scene: output_scene(&parsed),
        confidence: output_confidence(&parsed),
        model_id: model_id.to_string(),
        usage: usage.clone(),
        provider_request_id: request_id.to_string(),
    });
    let rating = include_rating.then(|| ScoreOutcome {
        rating: parsed.get("rating").and_then(parse_rating_int).unwrap_or(0),
        rubric_id: output_rubric_id(&parsed),
        rubric_version: output_rubric_version(&parsed),
        reasons: output_rating_reason(&parsed),
        model_id: model_id.to_string(),
        usage: usage.clone(),
        provider_request_id: request_id.to_string(),
    });
    let out = AnalyzeOutcome {
        understanding,
        rating,
        model_id: model_id.to_string(),
        usage,
        provider_request_id: request_id.to_string(),
    };
    validate_analyze(&out)?;
    Ok(out)
}

pub fn parse_batch_analyze(
    body: &Value,
    model_id: &str,
    usage_pointer: Option<&str>,
    request_id: &str,
    images: &[AnalyzeImageInput<'_>],
    include_understanding: bool,
    include_rating: bool,
) -> Result<BatchAnalyzeOutcome, ProviderError> {
    let parsed = output_json(body)?;
    let results = parsed
        .get("results")
        .and_then(|v| v.as_array())
        .ok_or(ProviderError::SchemaValidation)?;
    let usage = usage_from(body, usage_pointer);
    let mut items = Vec::with_capacity(images.len());
    for expected_index in 0..images.len() {
        let item = results
            .iter()
            .find(|v| {
                v.get("index")
                    .and_then(|i| i.as_u64())
                    .map(|i| i as usize == expected_index)
                    .unwrap_or(false)
            })
            .ok_or(ProviderError::SchemaValidation)?;
        let understanding = include_understanding.then(|| DescribeOutcome {
            caption: output_description(item),
            tags: output_tags(item),
            scene: output_scene(item),
            confidence: output_confidence(item),
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: request_id.to_string(),
        });
        let rating = include_rating.then(|| ScoreOutcome {
            rating: item.get("rating").and_then(parse_rating_int).unwrap_or(0),
            rubric_id: output_rubric_id(item),
            rubric_version: output_rubric_version(item),
            reasons: output_rating_reason(item),
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: request_id.to_string(),
        });
        let out = AnalyzeOutcome {
            understanding,
            rating,
            model_id: model_id.to_string(),
            usage: usage.clone(),
            provider_request_id: request_id.to_string(),
        };
        validate_analyze(&out)?;
        items.push(out);
    }
    let out = BatchAnalyzeOutcome {
        items,
        model_id: model_id.to_string(),
        usage,
        provider_request_id: request_id.to_string(),
    };
    validate_batch_analyze(&out)?;
    Ok(out)
}

pub fn output_excerpt(body: &Value) -> String {
    match output_text(body) {
        Some(text) => text.to_string(),
        None => sanitized_provider_json_excerpt(body, 1200),
    }
}

fn output_json(body: &Value) -> Result<Value, ProviderError> {
    let text = output_text(body).ok_or_else(|| {
        ProviderError::SchemaValidationMessage(format!(
            "Codex response did not contain output text; response excerpt: {}",
            sanitized_provider_json_excerpt(body, 1200)
        ))
    })?;
    parse_content_json(text)
}

fn output_text(body: &Value) -> Option<&str> {
    if let Some(s) = body.get("output_text").and_then(|v| v.as_str()) {
        return Some(s);
    }
    if let Some(s) = body
        .pointer("/output/0/content/0/text")
        .and_then(|v| v.as_str())
    {
        return Some(s);
    }
    if let Some(s) = body
        .pointer("/output/0/content/0/output_text")
        .and_then(|v| v.as_str())
    {
        return Some(s);
    }
    let output = body.get("output").and_then(|v| v.as_array())?;
    for item in output {
        if let Some(content) = item.get("content").and_then(|v| v.as_array()) {
            for block in content {
                let block_type = block.get("type").and_then(|v| v.as_str());
                if matches!(block_type, Some("output_text" | "text")) {
                    if let Some(text) = block
                        .get("text")
                        .or_else(|| block.get("output_text"))
                        .and_then(|v| v.as_str())
                    {
                        return Some(text);
                    }
                }
            }
        }
    }
    None
}

fn usage_from(body: &Value, pointer: Option<&str>) -> Usage {
    let usage = pointer
        .and_then(|p| json_pointer_str(body, p))
        .or_else(|| body.get("usage"));
    extract_usage(usage)
}
