mod bootstrap;
mod config;
mod logging;
mod proto;
mod server;
mod service;

use anyhow::{Context, anyhow};

const SERVER_THREAD_STACK_BYTES: usize = 64 * 1024 * 1024;

fn main() -> anyhow::Result<()> {
    let server_thread = std::thread::Builder::new()
        .name("alcedo-mind-server".to_string())
        .stack_size(SERVER_THREAD_STACK_BYTES)
        .spawn(run_server)
        .context("failed to spawn alcedo_mind server thread")?;

    server_thread
        .join()
        .map_err(|_| anyhow!("alcedo_mind server thread panicked"))?
}

fn run_server() -> anyhow::Result<()> {
    logging::init_logging();
    let config = config::AppConfig::load()?;
    let semantic_engine = service::inference::build_semantic_engine(&config);
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .context("failed to initialize alcedo_mind tokio runtime")?;
    runtime
        .block_on(bootstrap::start_server(config, semantic_engine))
        .map_err(|err| anyhow!("{err}"))
}
