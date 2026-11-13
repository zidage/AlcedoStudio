//! Log destination of the sidecar process (#823).
//!
//! The host passes `--log-file <log dir>/alcedo_<time>_<pid>_mind.log`; the process appends its
//! log there and creates nothing in `<exe dir>/log`. Started without `--log-file`, the process
//! writes its log to stderr only. Each test starts the `alcedo_mind` binary without a model,
//! waits for the line that the server writes before it listens, and kills the process.

use std::collections::BTreeMap;
use std::io::{BufRead, BufReader};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant, SystemTime};

const STARTUP_TIMEOUT: Duration = Duration::from_secs(60);

fn sidecar_binary() -> PathBuf {
    PathBuf::from(env!("CARGO_BIN_EXE_alcedo_mind"))
}

/// The directory where the earlier sidecar wrote its daily log files.
fn executable_log_dir() -> PathBuf {
    sidecar_binary()
        .parent()
        .expect("the binary has a directory")
        .join("log")
}

/// Name, size, and modification time of each file in `dir`; empty when `dir` does not exist.
fn directory_files(dir: &Path) -> BTreeMap<String, (u64, SystemTime)> {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return BTreeMap::new();
    };
    entries
        .filter_map(Result::ok)
        .filter_map(|entry| {
            let metadata = entry.metadata().ok()?;
            Some((
                entry.file_name().to_string_lossy().into_owned(),
                (metadata.len(), metadata.modified().ok()?),
            ))
        })
        .collect()
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0")
        .and_then(|listener| listener.local_addr())
        .expect("bind a free port")
        .port()
}

fn new_temp_dir(name: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(SystemTime::UNIX_EPOCH)
        .expect("clock after epoch")
        .as_nanos();
    let dir = std::env::temp_dir().join(format!("{name}_{}_{nanos}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("create temp dir");
    dir
}

fn spawn_sidecar(port: u16, model_root: &Path, log_file: Option<&Path>) -> Child {
    let mut command = Command::new(sidecar_binary());
    command
        .args(["--host", "127.0.0.1", "--port", &port.to_string()])
        .arg("--model-root")
        .arg(model_root)
        .env_remove("RUST_LOG")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    if let Some(path) = log_file {
        command.arg("--log-file").arg(path);
    }
    command.spawn().expect("start alcedo_mind")
}

/// Sends each line of `stream` to the returned receiver, on a reader thread.
fn read_lines(stream: impl std::io::Read + Send + 'static) -> mpsc::Receiver<String> {
    let (sender, receiver) = mpsc::channel();
    std::thread::spawn(move || {
        for line in BufReader::new(stream).lines().map_while(Result::ok) {
            if sender.send(line).is_err() {
                return;
            }
        }
    });
    receiver
}

/// Waits for the first line of `lines` that contains `text`.
fn wait_for_line(lines: &mpsc::Receiver<String>, text: &str) -> Option<String> {
    let deadline = Instant::now() + STARTUP_TIMEOUT;
    while let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        match lines.recv_timeout(remaining) {
            Ok(line) if line.contains(text) => return Some(line),
            Ok(_) => {}
            Err(_) => return None,
        }
    }
    None
}

fn stop(mut child: Child) {
    let _ = child.kill();
    let _ = child.wait();
}

#[test]
fn log_file_argument_writes_the_log_to_that_file_and_not_next_to_the_executable() {
    let exe_log_files_before = directory_files(&executable_log_dir());
    let dir = new_temp_dir("alcedo_mind_log_file");
    let log_file = dir.join("alcedo_20261111_101500_42_mind.log");
    let port = free_port();

    let mut child = spawn_sidecar(port, &dir, Some(&log_file));
    let stdout = read_lines(child.stdout.take().expect("stdout pipe"));
    let stdout_line = wait_for_line(&stdout, "starting alcedo_mind on");
    stop(child);

    let stdout_line = stdout_line.expect("the startup line reaches stdout");
    assert!(
        stdout_line.contains(" INFO "),
        "the stdout line has its level: {stdout_line:?}"
    );
    assert!(
        !stdout_line.contains('\u{1b}'),
        "the stdout line has no ANSI colors: {stdout_line:?}"
    );
    let text = std::fs::read_to_string(&log_file).expect("the log file exists");
    assert!(
        text.contains(&format!("starting alcedo_mind on 127.0.0.1:{port}")),
        "the log file holds the startup line: {text:?}"
    );
    assert_eq!(
        directory_files(&executable_log_dir()),
        exe_log_files_before,
        "no file in <exe dir>/log is created or changed"
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn without_log_file_argument_the_log_goes_to_stderr_only() {
    let exe_log_files_before = directory_files(&executable_log_dir());
    let dir = new_temp_dir("alcedo_mind_log_stderr");
    let port = free_port();

    let mut child = spawn_sidecar(port, &dir, None);
    let stderr = read_lines(child.stderr.take().expect("stderr pipe"));
    let stderr_line = wait_for_line(&stderr, "starting alcedo_mind on");
    stop(child);

    assert!(stderr_line.is_some(), "the startup line reaches stderr");
    assert_eq!(
        directory_files(&executable_log_dir()),
        exe_log_files_before,
        "no file in <exe dir>/log is created or changed"
    );
    let _ = std::fs::remove_dir_all(&dir);
}
