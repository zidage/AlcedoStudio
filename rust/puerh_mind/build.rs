use std::{env, path::PathBuf};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    emit_macos_swift_runtime_rpath();

    let out_dir = PathBuf::from(env::var("OUT_DIR")?);
    let descriptor_path = out_dir.join("semantic_descriptor.bin");

    // The file-descriptor-set path captures every compiled proto in one set
    // (named `semantic_descriptor.bin` for historical reasons); the new `ai`
    // protos are folded into the same set so gRPC reflection exposes them
    // without a separate registration.
    tonic_prost_build::configure()
        .file_descriptor_set_path(&descriptor_path)
        .compile_protos(
            &[
                "proto/common.proto",
                "proto/semantic.proto",
                "proto/ai_common.proto",
                "proto/segmentation.proto",
            ],
            &["proto"],
        )?;

    println!("cargo:rerun-if-changed=proto/common.proto");
    println!("cargo:rerun-if-changed=proto/semantic.proto");
    println!("cargo:rerun-if-changed=proto/ai_common.proto");
    println!("cargo:rerun-if-changed=proto/segmentation.proto");

    Ok(())
}

#[cfg(target_os = "macos")]
fn emit_macos_swift_runtime_rpath() {
    println!("cargo:rustc-link-arg=-Wl,-rpath,/usr/lib/swift");
}

#[cfg(not(target_os = "macos"))]
fn emit_macos_swift_runtime_rpath() {}
