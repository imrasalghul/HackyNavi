use hackynavi_metal_compiler::{compile, InputFormat, KernelDispatch, Request, ShaderStage};
use std::path::Path;

fn run() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.len() != 4 {
        return Err("usage: hackynavi-shader INPUT.air|INPUT.ll|INPUT.metallib ENTRY compute|vertex|fragment OUTPUT_DIRECTORY (must not exist)".into());
    }
    let stage = match args[2].as_str() {
        "compute" => ShaderStage::Compute,
        "vertex" => ShaderStage::Vertex,
        "fragment" => ShaderStage::Fragment,
        _ => return Err("unknown shader stage".into()),
    };
    let input = std::fs::read(&args[0]).map_err(|e| e.to_string())?;
    let format = match Path::new(&args[0]).extension().and_then(|s| s.to_str()) {
        Some("ll") => InputFormat::LlvmText,
        Some("metallib") => InputFormat::Metallib,
        _ => InputFormat::AirBitcode,
    };
    let compiled = compile(&Request {
        input: &input,
        format,
        stage,
        entry_point: &args[1],
        local_size: [64, 1, 1],
        dispatch: KernelDispatch::safe_default(),
        raster_samples: None,
    })?;
    let reflection = serde_json::to_vec_pretty(&compiled.reflection).map_err(|e| e.to_string())?;
    let module = serde_json::to_vec_pretty(&serde_json::json!({
        "abi_version": 1, "target_environment": "vulkan1.2",
        "spirv_entry_point": compiled.spirv_entry_point,
        "metal_entry_point": compiled.reflection.entry_point,
    }))
    .map_err(|e| e.to_string())?;
    let output = Path::new(&args[3]);
    // No stale success artifacts survive a failed translation. The directory
    // belongs to this one invocation and is published only after compilation.
    std::fs::create_dir(output).map_err(|e| format!("create output directory: {e}"))?;
    let write = || -> std::io::Result<()> {
        std::fs::write(output.join("shader.spv"), compiled.spirv_bytes())?;
        std::fs::write(output.join("reflection.json"), reflection)?;
        std::fs::write(output.join("module.json"), module)?;
        Ok(())
    };
    if let Err(error) = write() {
        let _ = std::fs::remove_dir_all(output);
        return Err(format!("write compiler result: {error}"));
    }
    println!(
        "Compiled {} SPIR-V words; reflection v{}; GPU execution not performed",
        compiled.spirv.len(),
        compiled.reflection.reflection_version
    );
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("shader compilation failed: {error}");
        std::process::exit(1);
    }
}
