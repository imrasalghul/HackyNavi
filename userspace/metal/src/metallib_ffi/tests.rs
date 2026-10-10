// SPDX-License-Identifier: MIT
use super::*;
use std::ffi::CStr;

const SDL: &[u8] = include_bytes!("../../tests/fixtures/metallib/sdl-2.30.11-macos.metallib");
const EXPECTED: [(&str, u32, usize, &str); 7] = [
    (
        "SDL_Solid_vertex",
        1,
        2980,
        "b01ed46377eb9535ecfd42fc8f6cc067915950527e6d23ef266e78c7673fa3da",
    ),
    (
        "SDL_Copy_vertex",
        1,
        2996,
        "c660ef78b06b693a1f4cadbbcbbc9677aca6e13b3b064c6410eefec0b7242870",
    ),
    (
        "SDL_Solid_fragment",
        2,
        2284,
        "ecda648c89b3613278a4679dd2ad75559decf86dc9b14a94ac2d9a29edf5723e",
    ),
    (
        "SDL_Copy_fragment",
        2,
        2640,
        "8ca9c15f0c8d69e0ac203945b5815abd1ab248a8e781313b3a2220e2b9f7718f",
    ),
    (
        "SDL_YUV_fragment",
        2,
        3284,
        "2122f2050d3cbe5ce3c5e5f2923224f8ebfed1e94ec00f882ef5fd77985e6a30",
    ),
    (
        "SDL_NV12_fragment",
        2,
        3168,
        "e5d521e233803af8a1581bcc61e3bad93ec35aaabc0c670a41cc77b3b8434343",
    ),
    (
        "SDL_NV21_fragment",
        2,
        3180,
        "d29877abf402992b5ddaace325b7e0fd5f85653b16b821a36e135b113277cac2",
    ),
];

fn sentinel() -> MetallibFunctionInfo {
    MetallibFunctionInfo {
        struct_bytes: 0xa5a5a5a5,
        stage: 0xa5a5a5a5,
        name_bytes: 0xa5a5a5a5,
        air_bytes: 0xa5a5a5a5,
        air_version: [0xa5a5; 2],
        language_version: [0xa5a5; 2],
        wrapped_air_sha256: [0xa5; 32],
        reserved: [0xa5a5a5a5; 2],
    }
}

#[test]
fn actual_fixture_owned_functions() {
    unsafe {
        assert_eq!(hn_metallib_abi_version(), 1);
        assert_eq!(crate::hn_shader_abi_version(), 1);
        let mut input = SDL.to_vec();
        let result = hn_metallib_parse(input.as_ptr(), input.len());
        assert!(!result.is_null());
        input.fill(0xff);
        drop(input);
        assert_eq!(CStr::from_ptr(hn_metallib_error(result)).to_bytes(), b"");
        assert_eq!(hn_metallib_function_count(result), 7);
        let mut copies = Vec::new();
        for (index, (name, stage, size, digest)) in EXPECTED.iter().enumerate() {
            let mut info = sentinel();
            assert_eq!(
                hn_metallib_function_info(result, index as u32, &mut info, 64),
                1
            );
            assert_eq!(
                (
                    info.struct_bytes,
                    info.stage,
                    info.name_bytes,
                    info.air_bytes
                ),
                (64, *stage, name.len() as u32 + 1, *size as u32)
            );
            assert_eq!(info.air_version, [1, 8]);
            assert_eq!(info.language_version, [1, 1]);
            assert_eq!(info.reserved, [0; 2]);
            let hex: String = info
                .wrapped_air_sha256
                .iter()
                .map(|b| format!("{b:02x}"))
                .collect();
            assert_eq!(&hex, digest);
            // Copy into an interior span; successful calls must preserve both guards.
            let mut copied_name = vec![0xcc; info.name_bytes as usize + 2];
            let mut air = vec![0xdd; *size + 2];
            assert_eq!(
                hn_metallib_copy_name(
                    result,
                    index as u32,
                    copied_name.as_mut_ptr().add(1).cast(),
                    info.name_bytes as usize
                ),
                1
            );
            assert_eq!(
                hn_metallib_copy_air(result, index as u32, air.as_mut_ptr().add(1), *size),
                1
            );
            assert_eq!((copied_name[0], *copied_name.last().unwrap()), (0xcc, 0xcc));
            assert_eq!((air[0], *air.last().unwrap()), (0xdd, 0xdd));
            assert_eq!(&copied_name[1..copied_name.len() - 2], name.as_bytes());
            assert_eq!(copied_name[copied_name.len() - 2], 0);
            copies.push((info, copied_name, air));
        }
        hn_metallib_free(result);
        let independent = Library::parse(SDL).unwrap();
        for (index, (info, name, air)) in copies.iter().enumerate() {
            let expected = independent.extract(EXPECTED[index].0).unwrap();
            assert_eq!(&air[1..air.len() - 1], expected.air.as_slice());
            assert_eq!(
                CStr::from_bytes_with_nul(&name[1..name.len() - 1])
                    .unwrap()
                    .to_str()
                    .unwrap(),
                EXPECTED[index].0
            );
            assert_eq!(info.wrapped_air_sha256, expected.info.wrapped_air_sha256);
        }
    }
}

#[test]
fn exact_size_and_index_errors_preserve_outputs() {
    unsafe {
        let result = hn_metallib_parse(SDL.as_ptr(), SDL.len());
        for index in 0..7u32 {
            let mut info = sentinel();
            for bytes in [0, 1, 63, 65, usize::MAX] {
                assert_eq!(
                    hn_metallib_function_info(result, index, &mut info, bytes),
                    0
                );
                assert_eq!(info, sentinel());
            }
            assert_eq!(
                hn_metallib_function_info(result, index, ptr::null_mut(), 64),
                0
            );
            assert_eq!(hn_metallib_function_info(result, index, &mut info, 64), 1);
            let mut output = vec![0xac; info.air_bytes as usize + 2];
            let old = output.clone();
            for bytes in [
                0,
                1,
                info.air_bytes as usize - 1,
                info.air_bytes as usize + 1,
                usize::MAX,
            ] {
                assert_eq!(
                    hn_metallib_copy_air(result, index, output.as_mut_ptr(), bytes),
                    0
                );
                assert_eq!(output, old);
            }
            for bytes in [
                0,
                1,
                info.name_bytes as usize - 1,
                info.name_bytes as usize + 1,
                usize::MAX,
            ] {
                assert_eq!(
                    hn_metallib_copy_name(result, index, output.as_mut_ptr().cast(), bytes),
                    0
                );
                assert_eq!(output, old);
            }
            assert_eq!(
                hn_metallib_copy_air(result, index, ptr::null_mut(), info.air_bytes as usize),
                0
            );
            assert_eq!(
                hn_metallib_copy_name(result, index, ptr::null_mut(), info.name_bytes as usize),
                0
            );
        }
        for index in [7, 8, u32::MAX] {
            let mut info = sentinel();
            let mut output = [0xee; 4096];
            assert_eq!(hn_metallib_function_info(result, index, &mut info, 64), 0);
            assert_eq!(info, sentinel());
            assert_eq!(
                hn_metallib_copy_air(result, index, output.as_mut_ptr(), 2980),
                0
            );
            assert_eq!(
                hn_metallib_copy_name(result, index, output.as_mut_ptr().cast(), 17),
                0
            );
            assert_eq!(output, [0xee; 4096]);
        }
        // Guaranteed misaligned destination in valid allocated storage.
        let mut aligned = [sentinel(); 2];
        let misaligned = (aligned.as_mut_ptr() as *mut u8).add(1).cast();
        assert_eq!(hn_metallib_function_info(result, 0, misaligned, 64), 0);
        assert_eq!(aligned, [sentinel(); 2]);
        let wrapping = (usize::MAX - 3) as *mut MetallibFunctionInfo;
        assert_eq!(hn_metallib_function_info(result, 0, wrapping, 64), 0);
        assert_eq!(
            hn_metallib_copy_air(result, 0, usize::MAX as *mut u8, 2980),
            0
        );
        assert_eq!(
            hn_metallib_copy_name(result, 0, usize::MAX as *mut c_char, 17),
            0
        );
        hn_metallib_free(result);
    }
}

#[test]
fn null_and_parse_failures_publish_no_functions() {
    unsafe {
        assert!(hn_metallib_error(ptr::null()).is_null());
        assert_eq!(hn_metallib_function_count(ptr::null()), 0);
        hn_metallib_free(ptr::null_mut());
        let errors = [
            hn_metallib_parse(ptr::null(), SDL.len()),
            hn_metallib_parse(SDL.as_ptr(), 0),
            hn_metallib_parse(SDL.as_ptr(), crate::MAX_INPUT_BYTES + 1),
            hn_metallib_parse(SDL.as_ptr(), usize::MAX),
            hn_metallib_parse(usize::MAX as *const u8, 88),
            hn_metallib_parse(SDL.as_ptr(), SDL.len() - 1),
            hn_metallib_parse(b"not a metallib".as_ptr(), 14),
        ];
        for result in std::iter::once(ptr::null_mut()).chain(errors) {
            let mut info = sentinel();
            let mut output = [0xaa; 64];
            assert_eq!(hn_metallib_function_count(result), 0);
            if !result.is_null() {
                let error = CStr::from_ptr(hn_metallib_error(result)).to_str().unwrap();
                assert!(!error.is_empty() && !error.contains("panicked"));
            }
            assert_eq!(hn_metallib_function_info(result, 0, &mut info, 64), 0);
            assert_eq!(info, sentinel());
            assert_eq!(
                hn_metallib_copy_name(result, 0, output.as_mut_ptr().cast(), 17),
                0
            );
            assert_eq!(hn_metallib_copy_air(result, 0, output.as_mut_ptr(), 64), 0);
            assert_eq!(output, [0xaa; 64]);
            hn_metallib_free(result);
        }
    }
}

#[test]
fn independent_handles_remain_valid() {
    unsafe {
        let first = hn_metallib_parse(SDL.as_ptr(), SDL.len());
        let second = hn_metallib_parse(SDL.as_ptr(), SDL.len());
        assert_ne!(first, second);
        hn_metallib_free(first);
        assert_eq!(hn_metallib_function_count(second), 7);
        let mut info = sentinel();
        assert_eq!(hn_metallib_function_info(second, 6, &mut info, 64), 1);
        assert_eq!(info.air_bytes, 3180);
        hn_metallib_free(second);
    }
}
