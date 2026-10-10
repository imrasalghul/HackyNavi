//! Shader compilation for the HackyNavi userspace driver. This does not submit
//! work, advertise a Metal device, or stand in for the missing RADV transport.
//! The vendored translator is LGPL-3.0-or-later; this adapter is MIT.

use metal2vulkan::{
    passes::{Stage, TransformOptions},
    reflect::ShaderReflection,
};
use std::{
    ffi::{c_char, CStr, CString},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

pub use metal2vulkan::reflect::KernelDispatch;
pub mod dispatch_plan;
pub mod metallib;
pub mod metallib_ffi;
pub mod pipeline_product;

pub const ABI_VERSION: u32 = 1;
pub const MAX_INPUT_BYTES: usize = 64 * 1024 * 1024;

#[derive(Clone, Copy, Debug)]
pub enum InputFormat {
    AirBitcode,
    LlvmText,
    Metallib,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShaderStage {
    Compute,
    Vertex,
    Fragment,
}

pub struct Request<'a> {
    pub input: &'a [u8],
    pub format: InputFormat,
    pub stage: ShaderStage,
    /// The translator must return this exact Metal entry point. Metallib input
    /// first selects this exact table name and verifies its requested stage.
    pub entry_point: &'a str,
    pub local_size: [u32; 3],
    pub dispatch: KernelDispatch,
    pub raster_samples: Option<u32>,
}

pub struct CompiledShader {
    /// Aligned words suitable for VkShaderModuleCreateInfo::pCode.
    pub spirv: Vec<u32>,
    /// Actual OpEntryPoint name for VkPipelineShaderStageCreateInfo::pName.
    /// This can differ from the Metal name retained in reflection.entry_point.
    pub spirv_entry_point: String,
    pub reflection: ShaderReflection,
    // Retained by value for later pipeline validation. None is the legacy
    // standalone environment-resolved path; explicit compilation never loses
    // its context or falls back to it.
    tools: Option<metal2vulkan::tools::ToolPaths>,
}

fn execution_entry(words: &[u32], stage: ShaderStage) -> Result<String, String> {
    let model = match stage {
        ShaderStage::Vertex => 0,
        ShaderStage::Fragment => 4,
        ShaderStage::Compute => 5,
    };
    let mut entry = None;
    let mut cursor = 5;
    while cursor < words.len() {
        let count = (words[cursor] >> 16) as usize;
        if count == 0 || count > words.len() - cursor {
            return Err("malformed SPIR-V instruction length".into());
        }
        if words[cursor] & 0xffff == 15 {
            if count < 4 || words[cursor + 1] != model || entry.is_some() {
                return Err(
                    "SPIR-V does not contain exactly one entry point of the requested stage".into(),
                );
            }
            let bytes: Vec<_> = words[cursor + 3..cursor + count]
                .iter()
                .flat_map(|w| w.to_le_bytes())
                .collect();
            let end = bytes
                .iter()
                .position(|b| *b == 0)
                .ok_or("unterminated SPIR-V entry point")?;
            let name =
                std::str::from_utf8(&bytes[..end]).map_err(|_| "non-UTF-8 SPIR-V entry point")?;
            if name.is_empty() {
                return Err("empty SPIR-V entry point".into());
            }
            entry = Some(name.to_owned());
        }
        cursor += count;
    }
    entry.ok_or_else(|| "SPIR-V has no entry point".into())
}

impl CompiledShader {
    pub fn spirv_bytes(&self) -> Vec<u8> {
        self.spirv
            .iter()
            .flat_map(|word| word.to_le_bytes())
            .collect()
    }
}

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Result<Self, String> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        for _ in 0..100 {
            let serial = NEXT.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("hackynavi-shader-{}-{serial}", std::process::id()));
            match std::fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(format!("create compiler scratch: {error}")),
            }
        }
        Err("unable to allocate unique compiler scratch directory".into())
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// Require a functional SPIRV-Tools validator. A success-only script, including
/// Cobalt64's development validator stub, cannot satisfy this negative probe.
/// Tool environment overrides must be fixed before starting compiler threads.
fn require_validator_with_tools(scratch: &Path, tools: Option<&metal2vulkan::tools::ToolPaths>) -> Result<(), String> {
    let (version, _) = match tools {
        Some(tools) => tools.run("spirv-val", &["--version"]),
        None => metal2vulkan::tools::run("spirv-val", &["--version"]),
    }?;
    if !String::from_utf8_lossy(&version).contains("SPIRV-Tools") {
        return Err("spirv-val did not identify itself as SPIRV-Tools".into());
    }
    // A header-only SPIR-V module has no memory model or entry point and must
    // fail Vulkan validation. This deliberately tests the actual executable.
    let invalid: Vec<u8> = [0x0723_0203u32, 0x0001_0500, 0, 1, 0]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect();
    if metal2vulkan::tools::spirv_val_bytes_with_tools(&invalid, scratch, tools).is_ok() {
        return Err("spirv-val accepted a deliberately invalid module".into());
    }
    Ok(())
}

pub fn compile(request: &Request<'_>) -> Result<CompiledShader, String> {
    compile_configured(request, None)
}
pub fn compile_with_tools(request: &Request<'_>, tools: &metal2vulkan::tools::ToolPaths) -> Result<CompiledShader, String> {
    compile_configured(request, Some(tools.clone()))
}
fn compile_configured(request: &Request<'_>, tools: Option<metal2vulkan::tools::ToolPaths>) -> Result<CompiledShader, String> {
    if request.input.is_empty() || request.input.len() > MAX_INPUT_BYTES {
        return Err("shader input is empty or exceeds 64 MiB".into());
    }
    if request.entry_point.is_empty() || request.entry_point.as_bytes().contains(&0) {
        return Err("an exact nonempty entry point is required".into());
    }
    if request.local_size.contains(&0) {
        return Err("local size dimensions must be nonzero".into());
    }
    request.dispatch.validate()?;
    // This is a compiler contract, not a claimed physical-device limit. Device
    // limits must also be enforced when a RADV pipeline is created.
    request
        .local_size
        .iter()
        .try_fold(1u32, |n, dim| n.checked_mul(*dim))
        .ok_or("local size product overflows u32")?;
    if request
        .raster_samples
        .is_some_and(|n| !matches!(n, 1 | 2 | 4 | 8 | 16 | 32 | 64))
    {
        return Err("invalid raster sample count".into());
    }
    let selected = if matches!(request.format, InputFormat::Metallib) {
        let library = metallib::Library::parse(request.input).map_err(|e| e.to_string())?;
        let function = library.extract(request.entry_point).map_err(|e| e.to_string())?;
        if function.info.stage.shader_stage() != request.stage {
            return Err(format!(
                "metallib stage mismatch: requested {:?}, table records {:?}",
                request.stage, function.info.stage
            ));
        }
        Some(function)
    } else {
        None
    };
    let source = selected.as_ref().map_or(request.input, |function| function.air.as_slice());
    let suffix = match request.format {
        InputFormat::LlvmText => {
            std::str::from_utf8(request.input).map_err(|_| "LLVM text must be UTF-8")?;
            "ll"
        }
        InputFormat::AirBitcode => {
            if !request.input.starts_with(b"BC\xc0\xde")
                && !request.input.starts_with(&[0xde, 0xc0, 0x17, 0x0b])
            {
                return Err("AIR input is not LLVM bitcode or a bitcode wrapper".into());
            }
            "air"
        }
        InputFormat::Metallib => "air",
    };
    let scratch = Scratch::new()?;
    require_validator_with_tools(&scratch.0, tools.as_ref())?;
    let input = scratch.0.join(format!("input.{suffix}"));
    std::fs::write(&input, source).map_err(|e| format!("write compiler input: {e}"))?;
    // This API has no MTLFunctionConstantValues parameter yet. The upstream
    // default can fold unsupplied constants to zero even when other observable
    // writes survive. Reject the entire feature before lowering; successful
    // SPIR-V validation cannot prove correct Metal specialization semantics.
    let (sanitized, _) = metal2vulkan::tools::air_to_sanitized_ll_with_tools(
        input.to_str().ok_or("scratch path must be UTF-8")?,
        &scratch.0,
        tools.as_ref(),
    )?;
    if !metal2vulkan::meta::parse_function_constants(&sanitized).is_empty()
        || sanitized
            .lines()
            .filter(|line| !line.trim_start().starts_with(';'))
            .any(|line| {
                [
                    "!air.function_constants",
                    ".MTL_FC_",
                    "air.fc_initializer",
                    "@air.is_function_constant_defined",
                    "@air.normalize_function_constant_predicate",
                    "!\"function_constant\"",
                    "!\"function_constant_disabled\"",
                ]
                .iter()
                .any(|token| line.contains(token))
            })
    {
        return Err("Metal function constants require explicit specialization values; unsupported by compiler ABI v1".into());
    }
    let stage = match request.stage {
        ShaderStage::Compute => Stage::Kernel,
        ShaderStage::Vertex => Stage::Vertex,
        ShaderStage::Fragment => Stage::Fragment,
    };
    let options = TransformOptions {
        kernel_local_size: request.local_size,
        kernel_dispatch: matches!(request.stage, ShaderStage::Compute).then_some(request.dispatch),
        raster_sample_count: request.raster_samples,
        ..TransformOptions::default()
    };
    let (bytes, reflection) = metal2vulkan::translate_reflected_with_options_and_tools(
        input.to_str().ok_or("scratch path must be UTF-8")?,
        stage,
        &scratch.0,
        options,
        tools.as_ref(),
    )?;
    if reflection.entry_point.as_deref() != Some(request.entry_point) {
        return Err(format!(
            "entry point mismatch: requested {:?}, translated {:?}",
            request.entry_point, reflection.entry_point
        ));
    }
    reflection.validate_descriptor_abi()?;
    // Keep validation at the adapter boundary even if an upstream translation
    // path changes. Never publish unvalidated bytes to the Vulkan backend.
    metal2vulkan::tools::spirv_val_bytes_with_tools(&bytes, &scratch.0, tools.as_ref())?;
    if bytes.len() < 20 || bytes.len() % 4 != 0 || !bytes.starts_with(&0x0723_0203u32.to_le_bytes())
    {
        return Err("translator returned malformed SPIR-V bytes".into());
    }
    let spirv: Vec<_> = bytes
        .chunks_exact(4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .collect();
    let spirv_entry_point = execution_entry(&spirv, request.stage)?;
    Ok(CompiledShader {
        spirv,
        spirv_entry_point,
        reflection,
        tools,
    })
}

/// C callers receive an opaque result on both success and ordinary failure.
/// Error results have no SPIR-V or reflection. Free with hn_shader_result_free.
pub struct ShaderResult {
    shader: Option<CompiledShader>,
    reflection_json: CString,
    spirv_entry_point: CString,
    error: CString,
}

fn result_handle(result: Result<CompiledShader, String>) -> *mut ShaderResult {
    let result = result.and_then(|shader| {
        let json = serde_json::to_string(&shader.reflection).map_err(|e| e.to_string())?;
        Ok((shader, CString::new(json).map_err(|e| e.to_string())?))
    });
    let value = match result {
        Ok((shader, json)) => ShaderResult {
            spirv_entry_point: CString::new(shader.spirv_entry_point.as_str()).unwrap(),
            shader: Some(shader),
            reflection_json: json,
            error: CString::default(),
        },
        Err(error) => ShaderResult {
            shader: None,
            reflection_json: CString::default(),
            spirv_entry_point: CString::default(),
            error: CString::new(error.replace('\0', "\\0")).unwrap(),
        },
    };
    Box::into_raw(Box::new(value))
}

#[no_mangle]
pub extern "C" fn hn_shader_abi_version() -> u32 {
    ABI_VERSION
}

/// Safety: all nonnull pointers must reference readable memory for the stated
/// length; entry must be a NUL-terminated UTF-8 string. No pointer is retained.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_compile(
    input: *const u8,
    input_size: usize,
    format: u32,
    stage: u32,
    entry: *const c_char,
    local_x: u32,
    local_y: u32,
    local_z: u32,
    exact_threads: u32,
    raster_samples: u32,
) -> *mut ShaderResult {
    compile_c(input,input_size,format,stage,entry,local_x,local_y,local_z,exact_threads,raster_samples,None)
}

#[repr(C)]
pub struct ShaderTools {
    pub struct_size: u32,
    pub version: u32,
    pub air_dis_path: *const c_char,
    pub spirv_val_path: *const c_char,
}
unsafe fn tool_paths(tools: *const ShaderTools) -> Result<metal2vulkan::tools::ToolPaths, String> {
    let tools = tools.as_ref().ok_or("null explicit compiler tools")?;
    if tools.struct_size as usize != std::mem::size_of::<ShaderTools>() || tools.version != 1 {
        return Err("unsupported explicit compiler tools structure".into());
    }
    unsafe fn path<'a>(p: *const c_char) -> Result<&'a Path, String> {
        if p.is_null() { return Err("null explicit tool path".into()); }
        let raw = CStr::from_ptr(p);
        if raw.is_empty() || raw.to_bytes().len() > 4096 { return Err("explicit tool path exceeds length bounds".into()); }
        Ok(Path::new(raw.to_str().map_err(|_| "explicit tool path must be UTF-8")?))
    }
    metal2vulkan::tools::ToolPaths::new(path(tools.air_dis_path)?,path(tools.spirv_val_path)?)
}
/// Safety: tools and its paths must be readable during this call; other
/// arguments have the same contract as hn_shader_compile. Paths are copied.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_compile_with_tools(
    tools: *const ShaderTools, input: *const u8, input_size: usize, format: u32, stage: u32,
    entry: *const c_char, local_x: u32, local_y: u32, local_z: u32,
    exact_threads: u32, raster_samples: u32,
) -> *mut ShaderResult {
    compile_c(input,input_size,format,stage,entry,local_x,local_y,local_z,exact_threads,raster_samples,Some(tools))
}
unsafe fn compile_c(
    input: *const u8, input_size: usize, format: u32, stage: u32, entry: *const c_char,
    local_x: u32, local_y: u32, local_z: u32, exact_threads: u32, raster_samples: u32,
    tools: Option<*const ShaderTools>,
) -> *mut ShaderResult {
    let result = std::panic::catch_unwind(|| {
        if input.is_null() || entry.is_null() || input_size == 0 || input_size > MAX_INPUT_BYTES {
            return Err("invalid compiler input pointers or length".into());
        }
        let format = match format {
            0 => InputFormat::AirBitcode,
            1 => InputFormat::LlvmText,
            2 => InputFormat::Metallib,
            _ => return Err("unknown input format".into()),
        };
        let stage = match stage {
            0 => ShaderStage::Compute,
            1 => ShaderStage::Vertex,
            2 => ShaderStage::Fragment,
            _ => return Err("unknown shader stage".into()),
        };
        let dispatch = match exact_threads {
            0 => KernelDispatch::Workgroups,
            1 => KernelDispatch::safe_default(),
            _ => return Err("unknown dispatch mode".into()),
        };
        let entry_point = CStr::from_ptr(entry)
            .to_str()
            .map_err(|_| "entry must be UTF-8")?;
        let tools = tools.map(|tools| tool_paths(tools)).transpose()?;
        compile_configured(&Request {
            input: std::slice::from_raw_parts(input, input_size),
            format,
            stage,
            entry_point,
            local_size: [local_x, local_y, local_z],
            dispatch,
            raster_samples: (raster_samples != 0).then_some(raster_samples),
        }, tools)
    })
    .unwrap_or_else(|_| Err("shader compiler panicked; no shader was published".into()));
    result_handle(result)
}

#[no_mangle]
pub unsafe extern "C" fn hn_shader_result_words(
    result: *const ShaderResult,
    count: *mut usize,
) -> *const u32 {
    if count.is_null() {
        return std::ptr::null();
    }
    *count = 0;
    let Some(result) = result.as_ref() else {
        return std::ptr::null();
    };
    let Some(shader) = &result.shader else {
        return std::ptr::null();
    };
    *count = shader.spirv.len();
    shader.spirv.as_ptr()
}

#[no_mangle]
pub unsafe extern "C" fn hn_shader_result_reflection(result: *const ShaderResult) -> *const c_char {
    result
        .as_ref()
        .map_or(std::ptr::null(), |r| r.reflection_json.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn hn_shader_result_spirv_entry_point(
    result: *const ShaderResult,
) -> *const c_char {
    result
        .as_ref()
        .map_or(std::ptr::null(), |r| r.spirv_entry_point.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn hn_shader_result_error(result: *const ShaderResult) -> *const c_char {
    result
        .as_ref()
        .map_or(std::ptr::null(), |r| r.error.as_ptr())
}

#[no_mangle]
pub unsafe extern "C" fn hn_shader_result_free(result: *mut ShaderResult) {
    if !result.is_null() {
        drop(Box::from_raw(result));
    }
}
