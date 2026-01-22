use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    // Get the project root (two directories up from src-tauri)
    let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let project_root = PathBuf::from(&manifest_dir)
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .to_path_buf();

    println!("cargo:warning=Project root: {}", project_root.display());

    // Build C++ library with CMake
    let build_dir = project_root.join("build");
    std::fs::create_dir_all(&build_dir).expect("Failed to create build directory");

    // Configure CMake
    let cmake_status = Command::new("cmake")
        .args(&[
            "-S",
            project_root.to_str().unwrap(),
            "-B",
            build_dir.to_str().unwrap(),
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DBUILD_C_API=ON",
        ])
        .status()
        .expect("Failed to run CMake configure");

    if !cmake_status.success() {
        panic!("CMake configuration failed");
    }

    // Build the C API target
    let build_status = Command::new("cmake")
        .args(&[
            "--build",
            build_dir.to_str().unwrap(),
            "--target",
            "tracey_c_api",
            "-j8",
        ])
        .status()
        .expect("Failed to run CMake build");

    if !build_status.success() {
        panic!("CMake build failed");
    }

    // Install to editor directory
    let install_status = Command::new("cmake")
        .args(&["--install", build_dir.to_str().unwrap()])
        .status()
        .expect("Failed to run CMake install");

    if !install_status.success() {
        println!("cargo:warning=CMake install failed (this is OK if you don't have sudo)");
    }

    // Tell Cargo where to find the library
    let lib_dir = PathBuf::from(&manifest_dir).join("libs");
    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=tracey_c_api");

    // Rerun if C++ source changes
    println!("cargo:rerun-if-changed=../../src");
    println!("cargo:rerun-if-changed=../../CMakeLists.txt");
    println!("cargo:rerun-if-changed=libs/libtracey_c_api.dylib");

    tauri_build::build()
}
