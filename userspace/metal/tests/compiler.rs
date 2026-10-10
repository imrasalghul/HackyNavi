use hackynavi_metal_compiler::{compile, InputFormat, KernelDispatch, Request, ShaderStage};
use std::{
    ffi::{CStr, CString},
    path::PathBuf,
    process::Command,
};

const FIXTURE: &[u8] = include_bytes!("fixtures/add_one.ll");
const APPLE_FIXTURE: &[u8] = include_bytes!("fixtures/apple/add_one.air");
fn request(input: &[u8]) -> Request<'_> {
    Request {
        input,
        format: InputFormat::LlvmText,
        stage: ShaderStage::Compute,
        entry_point: "add_one",
        local_size: [64, 1, 1],
        dispatch: KernelDispatch::safe_default(),
        raster_samples: None,
    }
}
fn opcodes(words: &[u32]) -> Vec<u16> {
    let mut result = Vec::new();
    let mut cursor = 5;
    while cursor < words.len() {
        let count = (words[cursor] >> 16) as usize;
        assert!(count > 0 && cursor + count <= words.len());
        result.push(words[cursor] as u16);
        cursor += count;
    }
    assert_eq!(cursor, words.len());
    result
}

#[test]
fn real_shader_retains_storage_load_add_and_store() {
    let shader = compile(&request(FIXTURE)).expect("authored add-one AIR must translate");
    let ops = opcodes(&shader.spirv);
    assert!(ops.contains(&61), "OpLoad must survive translation");
    assert!(ops.contains(&128), "OpIAdd must survive translation");
    assert!(ops.contains(&62), "OpStore must survive translation");
    assert_eq!(shader.reflection.entry_point.as_deref(), Some("add_one"));
    assert_eq!(shader.reflection.bindings.len(), 2);
    assert_eq!(
        shader.reflection.kernel_dispatch,
        Some(KernelDispatch::safe_default())
    );
    let json = serde_json::to_value(&shader.reflection).unwrap();
    assert_eq!(json["bindings"][0]["descriptor"]["binding"], 0);
    assert_eq!(json["bindings"][1]["descriptor"]["binding"], 1);
}

#[test]
fn apple_metal_compiler_air_translates_with_real_buffer_operations() {
    let mut req = request(APPLE_FIXTURE);
    req.format = InputFormat::AirBitcode;
    let shader = compile(&req).expect("actual Apple-generated AIR must compile");
    let ops = opcodes(&shader.spirv);
    assert!(ops.contains(&61) && ops.contains(&128) && ops.contains(&62));
    assert_eq!(shader.reflection.entry_point.as_deref(), Some("add_one"));
    assert_eq!(shader.reflection.bindings.len(), 2);
    assert_eq!(shader.reflection.bindings[0].metal_index, 0);
    assert_eq!(shader.reflection.bindings[1].metal_index, 1);
}

#[test]
fn llvm_bitcode_round_trip_compiles_the_same_shader() {
    let llvm_as =
        std::env::var_os("HACKYNAVI_LLVM_AS").expect("set HACKYNAVI_LLVM_AS to real llvm-as");
    let directory = std::env::temp_dir().join(format!("hn-air-fixture-{}", std::process::id()));
    std::fs::create_dir(&directory).unwrap();
    let input = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/add_one.ll");
    let air = directory.join("add_one.air");
    let output = Command::new(llvm_as)
        .arg(input)
        .arg("-o")
        .arg(&air)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let bytes = std::fs::read(&air).unwrap();
    let mut req = request(&bytes);
    req.format = InputFormat::AirBitcode;
    let actual = compile(&req).expect("real LLVM bitcode must translate through llvm-dis");
    let expected = compile(&request(FIXTURE)).unwrap();
    assert_eq!(actual.spirv, expected.spirv);
    std::fs::remove_dir_all(directory).unwrap();
}

#[test]
fn unsupported_gpu_intrinsic_returns_an_error() {
    let ir = std::str::from_utf8(FIXTURE).unwrap().replace(
        "%incremented = add i32 %value, 1",
        "%incremented = call i32 @llvm.agx3.emask.i32(i32 %value)",
    );
    let error = compile(&request(ir.as_bytes()))
        .err()
        .expect("unsupported operation must fail");
    assert!(error.contains("@llvm.agx3."), "{error}");
}

#[test]
fn requested_entry_cannot_silently_select_another_shader() {
    let mut req = request(FIXTURE);
    req.entry_point = "another_kernel";
    assert!(compile(&req)
        .err()
        .unwrap()
        .contains("entry point mismatch"));
}

#[test]
fn function_constants_cannot_silently_erase_part_of_a_shader() {
    // The unconditional store remains observable if an upstream default folds
    // the optional constant to zero. Reject even this partial-effect case.
    let mut req = request(include_bytes!("fixtures/function_constant_partial.ll"));
    req.entry_point = "partial_write";
    let error = compile(&req)
        .err()
        .expect("unsupplied constants must be rejected");
    assert!(error.contains("require explicit specialization"), "{error}");
}

#[test]
fn whole_workgroups_preserve_the_requested_local_size() {
    let mut req = request(FIXTURE);
    req.dispatch = KernelDispatch::Workgroups;
    req.local_size = [8, 4, 1];
    let shader = compile(&req).unwrap();
    assert_eq!(shader.reflection.local_size, Some([8, 4, 1]));
    assert_eq!(
        shader.reflection.kernel_dispatch,
        Some(KernelDispatch::Workgroups)
    );
}

#[test]
fn invalid_arguments_fail_without_output() {
    let mut req = request(FIXTURE);
    req.local_size = [0, 1, 1];
    assert!(compile(&req).err().unwrap().contains("nonzero"));
    req.local_size = [u32::MAX, 2, 1];
    assert!(compile(&req).err().unwrap().contains("overflows"));
    req.local_size = [1, 1, 1];
    req.format = InputFormat::AirBitcode;
    assert!(compile(&req).err().unwrap().contains("not LLVM bitcode"));
}

#[test]
fn c_abi_has_owned_results_and_no_error_shader() {
    use hackynavi_metal_compiler::*;
    unsafe {
        assert_eq!(hn_shader_abi_version(), 1);
        let entry = CString::new("add_one").unwrap();
        let result = hn_shader_compile(
            FIXTURE.as_ptr(),
            FIXTURE.len(),
            1,
            0,
            entry.as_ptr(),
            64,
            1,
            1,
            1,
            0,
        );
        let mut count = 0;
        let words = hn_shader_result_words(result, &mut count);
        assert!(!words.is_null() && count > 5);
        assert_eq!(*words, 0x07230203);
        assert!(CStr::from_ptr(hn_shader_result_error(result))
            .to_bytes()
            .is_empty());
        let reflection = CStr::from_ptr(hn_shader_result_reflection(result))
            .to_str()
            .unwrap();
        assert!(reflection.contains("add_one"));
        assert!(!CStr::from_ptr(hn_shader_result_spirv_entry_point(result))
            .to_bytes()
            .is_empty());
        hn_shader_result_free(result);
        let error = hn_shader_compile(std::ptr::null(), 0, 0, 0, entry.as_ptr(), 1, 1, 1, 0, 0);
        assert!(hn_shader_result_words(error, &mut count).is_null());
        assert_eq!(count, 0);
        assert!(!CStr::from_ptr(hn_shader_result_error(error))
            .to_bytes()
            .is_empty());
        hn_shader_result_free(error);
        hn_shader_result_free(std::ptr::null_mut());
    }
}

#[test]
fn missing_validator_never_publishes_cli_output() {
    let output = std::env::temp_dir().join(format!("hn-missing-validator-{}", std::process::id()));
    assert!(!output.exists());
    let input = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/add_one.ll");
    let result = Command::new(env!("CARGO_BIN_EXE_hackynavi-shader"))
        .args([
            input.as_os_str(),
            std::ffi::OsStr::new("add_one"),
            std::ffi::OsStr::new("compute"),
            output.as_os_str(),
        ])
        .env("METAL2VULKAN_SPIRV_VAL", output.join("missing-spirv-val"))
        .output()
        .unwrap();
    assert!(!result.status.success());
    assert!(String::from_utf8_lossy(&result.stderr).contains("cannot run spirv-val"));
    assert!(!output.exists());
}
