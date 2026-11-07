//! Runtime-loaded image-analysis prompt profiles.
//!
//! Provider drivers ask this module for a task prompt by profile id. The prompt
//! text itself lives in JSON (`configs/prompts/image_analysis_system_prompts.json`
//! by default, or `ALCEDO_MIND_PROMPT_PROFILE_PATH` when set), so users can author
//! alternate system prompts without editing Rust code.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::service::image_analysis::{
    ImageAnalysisSchemaSpec, ProviderError, image_analysis_schema_json, language_directive,
};

const KNOWN_SCHEMA_VERSION: u32 = 1;
const DEFAULT_PROMPT_PATH: &str = "configs/prompts/image_analysis_system_prompts.json";
const PROMPT_PATH_ENV: &str = "ALCEDO_MIND_PROMPT_PROFILE_PATH";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PromptPair {
    pub system: String,
    pub instruction: String,
}

#[derive(Debug, serde::Deserialize)]
struct PromptFile {
    schema_version: u32,
    default_profile_id: String,
    profiles: Vec<PromptProfile>,
}

#[derive(Debug, serde::Deserialize)]
struct PromptProfile {
    id: String,
    #[allow(dead_code)]
    display_name: Option<String>,
    describe: SimpleTaskPrompt,
    score: ScoreTaskPrompt,
    analyze: AnalyzeTaskPrompt,
}

#[derive(Debug, serde::Deserialize)]
struct SimpleTaskPrompt {
    system: String,
    instruction: String,
    return_instruction: String,
}

#[derive(Debug, serde::Deserialize)]
struct ScoreTaskPrompt {
    system: String,
    severity_append: HashMap<String, String>,
    instruction: String,
    return_instruction: String,
}

#[derive(Debug, serde::Deserialize)]
struct AnalyzeTaskPrompt {
    system: String,
    severity_append: HashMap<String, String>,
    instruction: String,
    return_instruction: String,
}

pub fn describe_prompt(
    prompt_profile_id: &str,
    output_language: &str,
) -> Result<PromptPair, ProviderError> {
    let file = load_prompt_file()?;
    let profile = select_profile(&file, prompt_profile_id)?;
    Ok(simple_prompt(
        &profile.describe,
        prompt_profile_id,
        output_language,
        ImageAnalysisSchemaSpec::describe(),
    ))
}

pub fn score_prompt(
    prompt_profile_id: &str,
    rubric_id: &str,
    rating_severity: &str,
    output_language: &str,
    camera_context: &str,
    include_rating_reasons: bool,
) -> Result<PromptPair, ProviderError> {
    let file = load_prompt_file()?;
    let profile = select_profile(&file, prompt_profile_id)?;
    let severity = normalize_rating_severity(rating_severity);
    let mut system = profile.score.system.trim().to_string();
    if let Some(append) = profile
        .score
        .severity_append
        .get(severity)
        .or_else(|| profile.score.severity_append.get("normal"))
    {
        append_sentence(&mut system, append);
    }
    append_schema_contract(
        &mut system,
        ImageAnalysisSchemaSpec::score(include_rating_reasons),
    );
    system.push_str(&language_directive(output_language));

    let mut instruction = profile.score.instruction.trim().to_string();
    append_rubric(&mut instruction, rubric_id);
    append_profile_trace(&mut instruction, prompt_profile_id);
    append_camera_context(&mut instruction, camera_context);
    append_sentence(&mut instruction, &profile.score.return_instruction);
    Ok(PromptPair {
        system,
        instruction,
    })
}

pub fn analyze_prompt(
    prompt_profile_id: &str,
    rubric_id: &str,
    rating_severity: &str,
    output_language: &str,
    camera_context: &str,
    include_understanding: bool,
    include_rating: bool,
    include_rating_reasons: bool,
) -> Result<PromptPair, ProviderError> {
    let file = load_prompt_file()?;
    let profile = select_profile(&file, prompt_profile_id)?;
    let task = &profile.analyze;
    let severity = normalize_rating_severity(rating_severity);

    let mut system = task.system.trim().to_string();
    if include_rating {
        if let Some(append) = task
            .severity_append
            .get(severity)
            .or_else(|| task.severity_append.get("normal"))
        {
            append_sentence(&mut system, append);
        }
    }
    append_schema_contract(
        &mut system,
        ImageAnalysisSchemaSpec::analyze(
            include_understanding,
            include_rating,
            include_rating_reasons,
        ),
    );
    system.push_str(&language_directive(output_language));

    let mut instruction = task.instruction.trim().to_string();
    append_rubric(&mut instruction, rubric_id);
    append_profile_trace(&mut instruction, prompt_profile_id);
    append_camera_context(&mut instruction, camera_context);
    append_sentence(&mut instruction, &task.return_instruction);
    Ok(PromptPair {
        system,
        instruction,
    })
}

fn simple_prompt(
    task: &SimpleTaskPrompt,
    prompt_profile_id: &str,
    output_language: &str,
    schema_spec: ImageAnalysisSchemaSpec,
) -> PromptPair {
    let mut system = task.system.trim().to_string();
    append_schema_contract(&mut system, schema_spec);
    system.push_str(&language_directive(output_language));
    let mut instruction = task.instruction.trim().to_string();
    append_profile_trace(&mut instruction, prompt_profile_id);
    append_sentence(&mut instruction, &task.return_instruction);
    PromptPair {
        system,
        instruction,
    }
}

fn load_prompt_file() -> Result<PromptFile, ProviderError> {
    let path = resolve_prompt_path()?;
    let raw = std::fs::read_to_string(&path).map_err(|err| {
        prompt_error(format!(
            "failed to read prompt profile JSON {}: {err}",
            path.display()
        ))
    })?;
    let file: PromptFile = serde_json::from_str(&raw).map_err(|err| {
        prompt_error(format!(
            "failed to parse prompt profile JSON {}: {err}",
            path.display()
        ))
    })?;
    validate_prompt_file(&file)?;
    Ok(file)
}

fn resolve_prompt_path() -> Result<PathBuf, ProviderError> {
    if let Ok(path) = std::env::var(PROMPT_PATH_ENV) {
        let path = PathBuf::from(path);
        if path.is_file() {
            return Ok(path);
        }
        return Err(prompt_error(format!(
            "{PROMPT_PATH_ENV} does not point to a JSON file: {}",
            path.display()
        )));
    }

    let mut candidates = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.join(DEFAULT_PROMPT_PATH));
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            candidates.push(parent.join(DEFAULT_PROMPT_PATH));
        }
    }
    candidates.push(Path::new(env!("CARGO_MANIFEST_DIR")).join(DEFAULT_PROMPT_PATH));

    candidates.into_iter().find(|p| p.is_file()).ok_or_else(|| {
        prompt_error(format!(
            "could not find default prompt profile JSON at {DEFAULT_PROMPT_PATH}"
        ))
    })
}

fn validate_prompt_file(file: &PromptFile) -> Result<(), ProviderError> {
    if file.schema_version != KNOWN_SCHEMA_VERSION {
        return Err(prompt_error(format!(
            "unsupported prompt schema_version {}; expected {KNOWN_SCHEMA_VERSION}",
            file.schema_version
        )));
    }
    require_non_empty("default_profile_id", &file.default_profile_id)?;
    if file.profiles.is_empty() {
        return Err(prompt_error("profiles must contain at least one profile"));
    }

    let mut seen = std::collections::HashSet::new();
    let mut has_default = false;
    for profile in &file.profiles {
        require_non_empty("profile.id", &profile.id)?;
        if !seen.insert(profile.id.as_str()) {
            return Err(prompt_error(format!(
                "duplicate prompt profile id {:?}",
                profile.id
            )));
        }
        if profile.id == file.default_profile_id {
            has_default = true;
        }
        validate_simple_task("describe", &profile.describe)?;
        validate_score_task(&profile.score)?;
        validate_analyze_task("analyze", &profile.analyze)?;
    }
    if !has_default {
        return Err(prompt_error(format!(
            "default_profile_id {:?} is not present in profiles",
            file.default_profile_id
        )));
    }
    Ok(())
}

fn validate_simple_task(name: &str, task: &SimpleTaskPrompt) -> Result<(), ProviderError> {
    require_non_empty(&format!("{name}.system"), &task.system)?;
    require_non_empty(&format!("{name}.instruction"), &task.instruction)?;
    require_non_empty(
        &format!("{name}.return_instruction"),
        &task.return_instruction,
    )
}

fn validate_score_task(task: &ScoreTaskPrompt) -> Result<(), ProviderError> {
    require_non_empty("score.system", &task.system)?;
    require_non_empty("score.instruction", &task.instruction)?;
    require_non_empty("score.return_instruction", &task.return_instruction)?;
    for key in ["lite", "normal", "high", "xhigh", "max"] {
        let value = task
            .severity_append
            .get(key)
            .ok_or_else(|| prompt_error(format!("score.severity_append.{key} is required")))?;
        require_non_empty(&format!("score.severity_append.{key}"), value)?;
    }
    Ok(())
}

fn validate_analyze_task(name: &str, task: &AnalyzeTaskPrompt) -> Result<(), ProviderError> {
    require_non_empty(&format!("{name}.system"), &task.system)?;
    require_non_empty(&format!("{name}.instruction"), &task.instruction)?;
    require_non_empty(
        &format!("{name}.return_instruction"),
        &task.return_instruction,
    )?;
    for key in ["lite", "normal", "high", "xhigh", "max"] {
        let value = task
            .severity_append
            .get(key)
            .ok_or_else(|| prompt_error(format!("{name}.severity_append.{key} is required")))?;
        require_non_empty(&format!("{name}.severity_append.{key}"), value)?;
    }
    Ok(())
}

fn select_profile<'a>(
    file: &'a PromptFile,
    prompt_profile_id: &str,
) -> Result<&'a PromptProfile, ProviderError> {
    let requested = prompt_profile_id.trim();
    let profile_id = if requested.is_empty() {
        file.default_profile_id.as_str()
    } else {
        requested
    };
    file.profiles
        .iter()
        .find(|p| p.id == profile_id)
        .or_else(|| {
            file.profiles
                .iter()
                .find(|p| p.id == file.default_profile_id)
        })
        .ok_or_else(|| prompt_error("default prompt profile disappeared after validation"))
}

fn normalize_rating_severity(severity: &str) -> &'static str {
    match severity.trim().to_ascii_lowercase().as_str() {
        "lite" => "lite",
        "high" => "high",
        "xhigh" | "x_high" => "xhigh",
        "max" => "max",
        _ => "normal",
    }
}

fn append_rubric(instruction: &mut String, rubric_id: &str) {
    if !rubric_id.trim().is_empty() {
        instruction.push_str(&format!(" Rubric: {rubric_id}."));
    }
}

fn append_profile_trace(instruction: &mut String, prompt_profile_id: &str) {
    if !prompt_profile_id.trim().is_empty() {
        instruction.push_str(&format!(" Prompt profile: {prompt_profile_id}."));
    }
}

fn append_camera_context(instruction: &mut String, camera_context: &str) {
    let camera_context = camera_context.trim();
    if camera_context.is_empty() {
        return;
    }
    instruction.push_str("\n\nUse this camera/EXIF metadata as additional context for the rating only when it is relevant:\n");
    instruction.push_str(camera_context);
}

fn append_sentence(target: &mut String, sentence: &str) {
    let sentence = sentence.trim();
    if sentence.is_empty() {
        return;
    }
    if !target.is_empty() && !target.ends_with(char::is_whitespace) {
        target.push(' ');
    }
    target.push_str(sentence);
}

fn append_schema_contract(target: &mut String, schema_spec: ImageAnalysisSchemaSpec) {
    append_sentence(
        target,
        "Return exactly one valid JSON object. The entire response must validate against this JSON Schema:",
    );
    target.push_str("\n\n");
    target.push_str(&image_analysis_schema_json(schema_spec));
    target.push_str("\n\nOutput contract:\n- Output only the JSON object, starting with { and ending with }.\n- Do not wrap the object in markdown code fences.\n- Do not include prose, comments, explanations, schema text, or examples.\n- Do not include fields not defined in the schema.\n- Use double quotes for all JSON strings and object keys.\n- Do not use trailing commas.");
}

fn require_non_empty(field: &str, value: &str) -> Result<(), ProviderError> {
    if value.trim().is_empty() {
        return Err(prompt_error(format!("{field} must not be empty")));
    }
    Ok(())
}

fn prompt_error(message: impl Into<String>) -> ProviderError {
    ProviderError::Provider(format!("invalid prompt profile config: {}", message.into()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_describe_prompt_loads_from_json() {
        let prompt = describe_prompt("", "zh").expect("prompt loads");
        assert!(prompt.system.contains("image understanding assistant"));
        assert!(prompt.system.contains("Simplified Chinese"));
        assert!(prompt.instruction.contains("Describe this image"));
    }

    #[test]
    fn score_severity_accepts_high() {
        let prompt =
            score_prompt("profile-1", "default", "high", "", "", true).expect("prompt loads");
        assert!(
            prompt
                .system
                .contains("strict but constructive master-level standard")
        );
        assert!(prompt.system.contains("Henri Cartier-Bresson"));
        assert!(prompt.system.contains("AlcedoImageRating"));
        assert!(prompt.system.contains("\"rating_reason\""));
        assert!(prompt.instruction.contains("Rubric: default."));
        assert!(prompt.instruction.contains("Prompt profile: profile-1."));
    }

    #[test]
    fn analyze_prompt_uses_flat_contract() {
        let prompt =
            analyze_prompt("", "", "normal", "", "", true, true, true).expect("prompt loads");
        assert!(prompt.system.contains("AlcedoImageAnalysisFlat"));
        for field in ["description", "rating", "rating_reason"] {
            assert!(prompt.system.contains(&format!("\"{field}\"")));
        }
        for field in ["tags", "confidence", "rubric_id", "rubric_version"] {
            assert!(!prompt.system.contains(&format!("\"{field}\"")));
        }
        assert!(prompt.instruction.contains("requested JSON object"));
    }
}
