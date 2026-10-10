// SPDX-License-Identifier: MIT
//! Additive owned C interface to the existing, bounded real-container parser.
//! No container metadata substitutes for compilation of the actual AIR bytes.

use crate::metallib::{Library, Stage};
use std::{
    ffi::{c_char, CString},
    mem,
    panic::catch_unwind,
    ptr,
};

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MetallibFunctionInfo {
    pub struct_bytes: u32,
    pub stage: u32,
    pub name_bytes: u32,
    pub air_bytes: u32,
    pub air_version: [u16; 2],
    pub language_version: [u16; 2],
    pub wrapped_air_sha256: [u8; 32],
    pub reserved: [u32; 2],
}

const _: () = assert!(mem::size_of::<MetallibFunctionInfo>() == 64);
const _: () = assert!(mem::offset_of!(MetallibFunctionInfo, wrapped_air_sha256) == 24);

struct Function {
    info: MetallibFunctionInfo,
    name: CString,
    air: Vec<u8>,
}

pub struct MetallibResult {
    functions: Vec<Function>,
    error: CString,
}

fn span(pointer: *const u8, bytes: usize) -> bool {
    !pointer.is_null()
        && bytes != 0
        && bytes <= isize::MAX as usize
        && (pointer as usize).checked_add(bytes).is_some()
}

fn parse(input: &[u8]) -> Result<Vec<Function>, String> {
    let library = Library::parse(input).map_err(|error| error.to_string())?;
    let mut functions = Vec::with_capacity(library.functions().len());
    for entry in library.functions() {
        // Use the existing fully validated extractor. The temporary parser
        // snapshot is dropped after all independent function copies succeed.
        let function = library
            .extract(&entry.name)
            .map_err(|error| error.to_string())?;
        let name = CString::new(function.info.name).map_err(|_| "metallib name contains NUL")?;
        let air_bytes = u32::try_from(function.air.len()).map_err(|_| "AIR size exceeds ABI")?;
        if air_bytes == 0 || function.air.len() > crate::MAX_INPUT_BYTES {
            return Err("AIR size exceeds nonzero bounded ABI".into());
        }
        let info = MetallibFunctionInfo {
            struct_bytes: mem::size_of::<MetallibFunctionInfo>() as u32,
            stage: match function.info.stage {
                Stage::Compute => 0,
                Stage::Vertex => 1,
                Stage::Fragment => 2,
            },
            name_bytes: u32::try_from(name.as_bytes_with_nul().len())
                .map_err(|_| "name size exceeds ABI")?,
            air_bytes,
            air_version: function.info.air_version,
            language_version: function.info.language_version,
            wrapped_air_sha256: function.info.wrapped_air_sha256,
            reserved: [0; 2],
        };
        functions.push(Function {
            info,
            name,
            air: function.air,
        });
    }
    Ok(functions)
}

#[no_mangle]
pub extern "C" fn hn_metallib_abi_version() -> u32 {
    1
}

/// Safety: input must be readable for input_bytes; it is never retained.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_parse(
    input: *const u8,
    input_bytes: usize,
) -> *mut MetallibResult {
    let parsed = catch_unwind(|| {
        if input_bytes > crate::MAX_INPUT_BYTES || !span(input, input_bytes) {
            return Err("invalid metallib input pointer or length".into());
        }
        parse(std::slice::from_raw_parts(input, input_bytes))
    })
    .unwrap_or_else(|_| Err("metallib parser panicked; no functions published".into()));
    let result = match parsed {
        Ok(functions) => MetallibResult {
            functions,
            error: CString::default(),
        },
        Err(error) => MetallibResult {
            functions: Vec::new(),
            error: CString::new(error.replace('\0', "\\0")).unwrap(),
        },
    };
    Box::into_raw(Box::new(result))
}

/// Safety: nonnull result must be live. Returned text is borrowed until free.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_error(result: *const MetallibResult) -> *const c_char {
    result
        .as_ref()
        .map_or(ptr::null(), |result| result.error.as_ptr())
}

/// Safety: nonnull result must be live.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_function_count(result: *const MetallibResult) -> u32 {
    result
        .as_ref()
        .map_or(0, |result| result.functions.len() as u32)
}

/// Safety: live/null result and writable aligned exact-size output; no overlap.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_function_info(
    result: *const MetallibResult,
    index: u32,
    out: *mut MetallibFunctionInfo,
    out_bytes: usize,
) -> i32 {
    catch_unwind(|| {
        if out_bytes != mem::size_of::<MetallibFunctionInfo>()
            || !span(out.cast(), out_bytes)
            || (out as usize) % mem::align_of::<MetallibFunctionInfo>() != 0
        {
            return 0;
        }
        let Some(function) = result
            .as_ref()
            .and_then(|result| result.functions.get(index as usize))
        else {
            return 0;
        };
        out.write(function.info);
        1
    })
    .unwrap_or(0)
}

unsafe fn copy(value: Option<&[u8]>, out: *mut u8, out_bytes: usize) -> i32 {
    let Some(value) = value else {
        return 0;
    };
    if out_bytes != value.len() || !span(out, out_bytes) {
        return 0;
    }
    ptr::copy_nonoverlapping(value.as_ptr(), out, out_bytes);
    1
}

/// Safety: live/null result and writable exact-size output, disjoint from result.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_copy_name(
    result: *const MetallibResult,
    index: u32,
    out: *mut c_char,
    out_bytes: usize,
) -> i32 {
    catch_unwind(|| {
        copy(
            result
                .as_ref()
                .and_then(|r| r.functions.get(index as usize))
                .map(|function| function.name.as_bytes_with_nul()),
            out.cast(),
            out_bytes,
        )
    })
    .unwrap_or(0)
}

/// Safety: live/null result and writable exact-size output, disjoint from result.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_copy_air(
    result: *const MetallibResult,
    index: u32,
    out: *mut u8,
    out_bytes: usize,
) -> i32 {
    catch_unwind(|| {
        copy(
            result
                .as_ref()
                .and_then(|r| r.functions.get(index as usize))
                .map(|function| function.air.as_slice()),
            out,
            out_bytes,
        )
    })
    .unwrap_or(0)
}

/// Safety: result must be null or an unfreed parse handle, with no racing reads.
#[no_mangle]
pub unsafe extern "C" fn hn_metallib_free(result: *mut MetallibResult) {
    if !result.is_null() {
        drop(Box::from_raw(result));
    }
}

#[cfg(test)]
mod tests;
