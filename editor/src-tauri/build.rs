use std::env;
use std::path::PathBuf;

fn main() {
    // Get the manifest directory
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();

    // Tell Cargo where to find the library
    // The dev.sh or build-all.sh scripts handle building the C++ library
    let lib_dir = PathBuf::from(&manifest_dir).join("libs");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=tracey_c_api");

    // Only rerun if Cargo.toml or build.rs changes
    // The C++ library is built by the shell scripts, not by build.rs
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=Cargo.toml");

    tauri_build::build()
}
