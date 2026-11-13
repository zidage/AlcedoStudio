use std::fs::OpenOptions;
use std::path::Path;
use std::sync::Mutex;

use anyhow::Context;
use tracing_subscriber::{EnvFilter, fmt, layer::SubscriberExt, util::SubscriberInitExt};

fn env_filter() -> EnvFilter {
    EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"))
}

/// Installs the `tracing` subscriber of the process. `RUST_LOG` sets the filter (default `info`).
///
/// With `log_file` (`--log-file`, given by the host), every line goes to that file (appended) and
/// to stdout without ANSI colors. The host reads the level of each stdout line and copies the WARN
/// and ERROR lines to the application log. Each line is written to the file when it is logged, so
/// the file keeps the last lines when the host kills the process.
///
/// Without `log_file` (the process started by hand), the lines go to stderr only.
pub fn init_logging(log_file: Option<&Path>) -> anyhow::Result<()> {
    let Some(path) = log_file else {
        tracing_subscriber::registry()
            .with(env_filter())
            .with(fmt::layer().with_target(false).with_writer(std::io::stderr))
            .try_init()
            .context("failed to install the stderr log subscriber")?;
        return Ok(());
    };

    if let Some(parent) = path
        .parent()
        .filter(|parent| !parent.as_os_str().is_empty())
    {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("failed to create the log directory {}", parent.display()))?;
    }
    let file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .with_context(|| format!("failed to open the log file {}", path.display()))?;

    let stdout_layer = fmt::layer()
        .with_ansi(false)
        .with_target(false)
        .with_writer(std::io::stdout);
    let file_layer = fmt::layer()
        .with_ansi(false)
        .with_target(true)
        .with_writer(Mutex::new(file));
    tracing_subscriber::registry()
        .with(env_filter())
        .with(stdout_layer)
        .with(file_layer)
        .try_init()
        .context("failed to install the file log subscriber")?;
    tracing::info!(path = %path.display(), "file logging initialized");
    Ok(())
}
