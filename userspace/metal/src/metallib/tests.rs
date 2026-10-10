// SPDX-License-Identifier: MIT
// Positive checks use unchanged published SDL and actual Apple-compiled
// macOS libraries. Mutations are malformed-input tests, not GPU fixtures.
use super::*;

const SDL: &[u8] = include_bytes!("../../tests/fixtures/metallib/sdl-2.30.11-macos.metallib");
const APPLE: &[u8] = include_bytes!("../../tests/fixtures/metallib/apple-32023-macos15-add-one.metallib");
const EXPECTED: [(&str, Stage, usize, &str); 7] = [
    (
        "SDL_Solid_vertex",
        Stage::Vertex,
        2980,
        "b01ed46377eb9535ecfd42fc8f6cc067915950527e6d23ef266e78c7673fa3da",
    ),
    (
        "SDL_Copy_vertex",
        Stage::Vertex,
        2996,
        "c660ef78b06b693a1f4cadbbcbbc9677aca6e13b3b064c6410eefec0b7242870",
    ),
    (
        "SDL_Solid_fragment",
        Stage::Fragment,
        2284,
        "ecda648c89b3613278a4679dd2ad75559decf86dc9b14a94ac2d9a29edf5723e",
    ),
    (
        "SDL_Copy_fragment",
        Stage::Fragment,
        2640,
        "8ca9c15f0c8d69e0ac203945b5815abd1ab248a8e781313b3a2220e2b9f7718f",
    ),
    (
        "SDL_YUV_fragment",
        Stage::Fragment,
        3284,
        "2122f2050d3cbe5ce3c5e5f2923224f8ebfed1e94ec00f882ef5fd77985e6a30",
    ),
    (
        "SDL_NV12_fragment",
        Stage::Fragment,
        3168,
        "e5d521e233803af8a1581bcc61e3bad93ec35aaabc0c670a41cc77b3b8434343",
    ),
    (
        "SDL_NV21_fragment",
        Stage::Fragment,
        3180,
        "d29877abf402992b5ddaace325b7e0fd5f85653b16b821a36e135b113277cac2",
    ),
];

fn hex(input: &[u8]) -> String {
    input.iter().map(|b| format!("{b:02x}")).collect()
}
fn put32(input: &mut [u8], offset: usize, value: u32) {
    input[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}
fn put64(input: &mut [u8], offset: usize, value: u64) {
    input[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}
fn group(index: usize) -> Range<usize> {
    let mut offset = 92;
    for _ in 0..index {
        offset += u32_at(SDL, offset).unwrap() as usize;
    }
    offset..offset + u32_at(SDL, offset).unwrap() as usize
}
fn payload(index: usize, name: &[u8; 4]) -> Range<usize> {
    let g = group(index);
    tags(SDL, g.start + 4..g.end)
        .unwrap()
        .into_iter()
        .find(|t| &t.name == name)
        .unwrap()
        .payload
}
fn air_extent(index: usize) -> Range<usize> {
    let section = u64_at(SDL, 72).unwrap() as usize;
    let start = u64_at(SDL, payload(index, b"OFFT").start + 16).unwrap() as usize;
    let end = if index + 1 == EXPECTED.len() {
        SDL.len() - section
    } else {
        u64_at(SDL, payload(index + 1, b"OFFT").start + 16).unwrap() as usize
    };
    section + start..section + end
}
fn fails(input: &[u8], kind: ErrorKind) {
    assert_eq!(Library::parse(input).unwrap_err().kind, kind);
}

#[test]
fn real_sdl_library_selects_seven_exact_owned_functions() {
    assert_eq!(SDL.len(), 21858);
    assert_eq!(
        hex(&Sha256::digest(SDL)),
        "7c898daf138f3d72385140b81f99578628fb6972a8dde31cc3382a0c40e2bc26"
    );
    let mut source = SDL.to_vec();
    let library = Library::parse(&source).unwrap();
    source.fill(0); // The parsed snapshot is independent of its caller.
    assert_eq!(library.functions().len(), EXPECTED.len());
    for (i, (name, stage, size, digest)) in EXPECTED.iter().enumerate() {
        let info = &library.functions()[i];
        assert_eq!(info.name, *name);
        assert_eq!(info.stage, *stage);
        assert_eq!(info.air_version, [1, 8]);
        assert_eq!(info.language_version, [1, 1]);
        assert_eq!(hex(&info.wrapped_air_sha256), *digest);
        let mut selected = library.extract(name).unwrap();
        assert_eq!(&selected.info, info);
        assert_eq!(selected.air.len(), *size);
        assert_eq!(
            selected.air,
            SDL[air_extent(i).start + 20..air_extent(i).start + 20 + size]
        );
        assert_eq!(&selected.air[..4], b"BC\xc0\xde");
        assert!(selected.private_metadata.is_empty());
        if i < 2 {
            assert_eq!(selected.public_metadata.len(), 2);
            assert_eq!(selected.public_metadata[0].name, *b"VATT");
            assert_eq!(selected.public_metadata[1].name, *b"VATY");
        } else {
            assert!(selected.public_metadata.is_empty());
        }
        selected.air.fill(0);
        assert_eq!(&library.extract(name).unwrap().air[..4], b"BC\xc0\xde");
    }
    for missing in [
        "",
        "SDL_solid_vertex",
        "SDL_Solid_vertex\0",
        "add_one",
        "SDL_Solid_vertex ",
    ] {
        assert_eq!(
            library.extract(missing).unwrap_err().kind,
            ErrorKind::FunctionNotFound
        );
    }
}

#[test]
fn every_real_container_truncation_is_rejected() {
    for size in 0..SDL.len() {
        assert!(Library::parse(&SDL[..size]).is_err(), "accepted {size}");
    }
    let mut extra = SDL.to_vec();
    extra.push(0);
    fails(&extra, ErrorKind::FileSizeMismatch);
    put64(&mut extra, 16, (SDL.len() + 1) as u64);
    fails(&extra, ErrorKind::UnsupportedLayout);
}

#[test]
fn header_profile_and_section_bounds_are_closed() {
    for (offset, kind) in [
        (0, ErrorKind::InvalidMagic),
        (4, ErrorKind::UnsupportedPlatform),
        (6, ErrorKind::UnsupportedVersion),
        (8, ErrorKind::UnsupportedVersion),
        (10, ErrorKind::UnsupportedLibraryType),
        (11, ErrorKind::UnsupportedTarget),
        (12, ErrorKind::UnsupportedTarget),
        (15, ErrorKind::UnsupportedTarget),
    ] {
        let mut input = SDL.to_vec();
        input[offset] ^= 1;
        fails(&input, kind);
    }
    for offset in [24, 32, 40, 48, 56, 64, 72, 80] {
        for value in [u64::MAX, u64::MAX - 3, SDL.len() as u64 + 1] {
            let mut input = SDL.to_vec();
            put64(&mut input, offset, value);
            fails(&input, ErrorKind::InvalidRange);
        }
    }
    for offset in [24, 40, 56, 72] {
        let mut input = SDL.to_vec();
        put64(&mut input, offset, 0);
        fails(&input, ErrorKind::OverlappingSections);
    }
    for (offset, start) in [(40, 935), (56, 1081), (72, 1137)] {
        let mut input = SDL.to_vec();
        put64(&mut input, offset, start);
        fails(&input, ErrorKind::OverlappingSections);
    }
    let mut input = SDL.to_vec();
    input[936..940].copy_from_slice(b"UUID");
    fails(&input, ErrorKind::UnsupportedLayout);
    let too_big = vec![0; MAX_LIBRARY_BYTES + 1];
    fails(&too_big, ErrorKind::InputTooLarge);
}

#[test]
fn function_counts_lengths_and_tag_boundaries_are_checked() {
    for count in [0, MAX_FUNCTIONS as u32 + 1, u32::MAX] {
        let mut input = SDL.to_vec();
        put32(&mut input, 88, count);
        fails(&input, ErrorKind::InvalidFunctionCount);
    }
    for count in [1, 6, 8, MAX_FUNCTIONS as u32] {
        let mut input = SDL.to_vec();
        put32(&mut input, 88, count);
        fails(&input, ErrorKind::InvalidGroup);
    }
    for index in 0..EXPECTED.len() {
        let g = group(index);
        for size in [0, 4, 7, u32::MAX] {
            let mut input = SDL.to_vec();
            put32(&mut input, g.start, size);
            fails(&input, ErrorKind::InvalidGroup);
        }
        let mut input = SDL.to_vec();
        input[g.end - 4..g.end].copy_from_slice(b"BAD!");
        fails(&input, ErrorKind::InvalidGroup);
        for name in [b"NAME", b"TYPE", b"HASH", b"OFFT", b"VERS"] {
            let tag = payload(index, name);
            let mut input = SDL.to_vec();
            input[tag.start - 2..tag.start].copy_from_slice(&u16::MAX.to_le_bytes());
            fails(&input, ErrorKind::InvalidGroup);
        }
    }
    let mut input = SDL.to_vec();
    let tag = payload(0, b"VERS");
    input[tag.start - 6..tag.start - 2].copy_from_slice(b"OFFT");
    fails(&input, ErrorKind::DuplicateTag);
    input[tag.start - 6..tag.start - 2].copy_from_slice(b"MDSZ");
    fails(&input, ErrorKind::UnsupportedTag);
    // Remove a real tag, updating container lengths so the missing-tag check,
    // rather than a surrounding size check, is exercised.
    let tag = payload(0, b"TYPE");
    let removed = tag.len() + 6;
    let mut input = SDL.to_vec();
    input.drain(tag.start - 6..tag.end);
    put32(
        &mut input,
        group(0).start,
        (group(0).len() - removed) as u32,
    );
    for field in [16, 32, 40, 56, 72] {
        put64(
            &mut input,
            field,
            u64_at(SDL, field).unwrap() - removed as u64,
        );
    }
    fails(&input, ErrorKind::MissingTag);
}

#[test]
fn exact_names_stages_and_versions_are_checked() {
    let p = payload(0, b"NAME");
    for (offset, value) in [(p.start, 0), (p.start, 0xff), (p.end - 1, b'X')] {
        let mut input = SDL.to_vec();
        input[offset] = value;
        fails(&input, ErrorKind::InvalidName);
    }
    let mut input = SDL.to_vec();
    let src = payload(5, b"NAME");
    let dest = payload(6, b"NAME");
    input[dest].copy_from_slice(&SDL[src]);
    fails(&input, ErrorKind::DuplicateName);
    for stage in 3..=255 {
        let mut input = SDL.to_vec();
        input[payload(0, b"TYPE").start] = stage;
        fails(&input, ErrorKind::UnsupportedStage);
    }
    for at in [0, 4] {
        let mut input = SDL.to_vec();
        let p = payload(0, b"VERS");
        input[p.start + at..p.start + at + 2].fill(0);
        fails(&input, ErrorKind::InvalidVersion);
    }
}

#[test]
fn offsets_metadata_and_bitcode_cannot_overlap_or_escape() {
    for slot in 0..3 {
        for index in 0..EXPECTED.len() {
            let offset = payload(index, b"OFFT").start + slot * 8;
            let mut input = SDL.to_vec();
            put64(&mut input, offset, u64::MAX);
            assert!(Library::parse(&input).is_err());
            if index != 0 {
                let mut input = SDL.to_vec();
                put64(&mut input, offset, 0);
                fails(&input, ErrorKind::DuplicateOffset);
                let mut input = SDL.to_vec();
                put64(&mut input, offset, u64_at(SDL, offset).unwrap() - 1);
                assert!(Library::parse(&input).is_err());
            }
        }
        let mut input = SDL.to_vec();
        put64(&mut input, payload(0, b"OFFT").start + slot * 8, 1);
        fails(&input, ErrorKind::InvalidRange);
    }
    for offset in [940, 1082] {
        for size in [0, u32::MAX] {
            let mut input = SDL.to_vec();
            put32(&mut input, offset, size);
            fails(&input, ErrorKind::InvalidMetadata);
        }
    }
    let mut input = SDL.to_vec();
    input[1086..1090].copy_from_slice(b"NAME");
    fails(&input, ErrorKind::InvalidGroup);
    let public = tags(SDL, 944..985).unwrap();
    let mut input = SDL.to_vec();
    let second = &public[1];
    input[second.payload.start - 6..second.payload.start - 2].copy_from_slice(b"VATT");
    fails(&input, ErrorKind::DuplicateTag);
}

#[test]
fn wrappers_hashes_and_alignment_padding_are_verified_for_every_function() {
    for index in 0..EXPECTED.len() {
        let extent = air_extent(index);
        for offset in [0, 20] {
            let mut input = SDL.to_vec();
            input[extent.start + offset] ^= 1;
            fails(&input, ErrorKind::InvalidWrapper);
        }
        for offset in [4, 8, 16] {
            let mut input = SDL.to_vec();
            input[extent.start + offset] ^= 1;
            fails(&input, ErrorKind::UnsupportedWrapper);
        }
        for size in [0, 3, 5, u32::MAX, extent.len() as u32] {
            let mut input = SDL.to_vec();
            put32(&mut input, extent.start + 12, size);
            fails(&input, ErrorKind::InvalidWrapper);
        }
        let raw_end = extent.start + 20 + EXPECTED[index].2;
        for offset in raw_end..extent.end {
            let mut input = SDL.to_vec();
            input[offset] = 1;
            fails(&input, ErrorKind::InvalidWrapper);
        }
        let mut input = SDL.to_vec();
        input[extent.start + 24] ^= 1;
        fails(&input, ErrorKind::HashMismatch);
        let mut input = SDL.to_vec();
        input[payload(index, b"HASH").start] ^= 1;
        fails(&input, ErrorKind::HashMismatch);
    }
}

#[test]
fn arbitrary_short_inputs_and_bounded_header_mutations_do_not_panic() {
    let mut state = 0x9e3779b97f4a7c15u64;
    for size in 0..512 {
        let mut input = vec![0; size];
        for byte in &mut input {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            *byte = state as u8;
        }
        assert!(Library::parse(&input).is_err());
    }
    // Mutate each pre-AIR byte. Some metadata contents/versions legitimately
    // differ; any accepted mutation must still allow every bounded extraction.
    for at in 0..1138 {
        let mut input = SDL.to_vec();
        input[at] ^= 0xff;
        if let Ok(library) = Library::parse(&input) {
            for function in library.functions() {
                assert!(!library.extract(&function.name).unwrap().air.is_empty());
            }
        }
    }
}

fn apple_tag(name: &[u8; 4]) -> Range<usize> {
    tags(APPLE, 96..217).unwrap().into_iter().find(|t| &t.name == name).unwrap().payload
}

#[test]
fn actual_apple_macos15_compute_library_has_owned_exact_air() {
    assert_eq!(APPLE.len(), 3597);
    assert_eq!(hex(&Sha256::digest(APPLE)), "ed77099cd3e56ac404331fa68dd9401a7b326a7b7ea80d942b29b8b1fe0af865");
    let mut input = APPLE.to_vec();
    let parsed = Library::parse(&input).unwrap();
    input.fill(0xff);
    drop(input);
    assert_eq!(parsed.functions().len(), 1);
    let function = parsed.extract("add_one").unwrap();
    drop(parsed);
    assert_eq!(function.info.stage, Stage::Compute);
    assert_eq!(function.info.air_version, [2, 7]);
    assert_eq!(function.info.language_version, [2, 4]);
    assert_eq!(function.info.name, "add_one");
    assert_eq!(hex(&function.info.wrapped_air_sha256), "a0159626d94557813cf641c1bcc4b0cabbebeae6959c756d8283b9c5813448aa");
    assert_eq!(function.air.len(), 3332);
    assert_eq!(hex(&Sha256::digest(&function.air)), "8d5c89281870b3d1f8f6868ad56f9396fdd8804acc697c59297c9f338889a13f");
    assert_eq!(function.air, APPLE[257..3589]);
    assert!(function.public_metadata.is_empty() && function.private_metadata.is_empty());
}

#[test]
fn apple_modern_profile_does_not_infer_other_versions_or_targets() {
    for (offset, kind) in [(6, ErrorKind::UnsupportedVersion), (8, ErrorKind::UnsupportedVersion),
        (10, ErrorKind::UnsupportedLibraryType), (11, ErrorKind::UnsupportedTarget),
        (12, ErrorKind::UnsupportedTarget), (13, ErrorKind::UnsupportedTarget),
        (14, ErrorKind::UnsupportedTarget), (15, ErrorKind::UnsupportedTarget)] {
        let mut data = APPLE.to_vec(); data[offset] ^= 1; fails(&data, kind);
    }
    let mut data = APPLE.to_vec(); data[8] = 5;
    fails(&data, ErrorKind::UnsupportedTarget);
    let mut data = SDL.to_vec(); data[8] = 8;
    fails(&data, ErrorKind::UnsupportedTarget);
    data[11..16].copy_from_slice(&[0x81,15,0,0,0]);
    fails(&data, ErrorKind::MissingTag); // A modern header never grants absent MDSZ semantics.
}

#[test]
fn apple_mdsz_must_match_independently_bounded_wrapper_extent() {
    let p = apple_tag(b"MDSZ");
    for size in [0, 1, 3332, 3359, 3361, u64::MAX] {
        let mut data = APPLE.to_vec(); put64(&mut data, p.start, size);
        fails(&data, ErrorKind::InvalidRange);
    }
    for size in [0u16, 7, 9, u16::MAX] {
        let mut data = APPLE.to_vec(); data[p.start-2..p.start].copy_from_slice(&size.to_le_bytes());
        assert!(Library::parse(&data).is_err());
    }
    let mut data = APPLE.to_vec();
    data[p.start-6..p.start-2].copy_from_slice(b"HASH");
    fails(&data, ErrorKind::DuplicateTag);
    let mut data = APPLE.to_vec();
    data[p.start-6..p.start-2].copy_from_slice(b"SIZE");
    fails(&data, ErrorKind::UnsupportedTag);
    // Remove MDSZ while fixing every surrounding size/offset, so the required
    // tag check is reached instead of a length or partition failure.
    let mut data = APPLE.to_vec(); let removed = p.len() + 6;
    data.drain(p.start-6..p.end); put32(&mut data, 92, 125 - removed as u32);
    for field in [16,32,40,56,72] { put64(&mut data, field, u64_at(APPLE,field).unwrap()-removed as u64); }
    fails(&data, ErrorKind::MissingTag);
}

#[test]
fn apple_truncation_wrapper_hash_and_section_failures_are_rejected() {
    for size in 0..APPLE.len() { assert!(Library::parse(&APPLE[..size]).is_err(), "accepted {size}"); }
    for field in [24,32,40,48,56,64,72,80] {
        let mut data = APPLE.to_vec(); put64(&mut data, field, u64::MAX);
        fails(&data, ErrorKind::InvalidRange);
    }
    for offset in [237,257] {
        let mut data = APPLE.to_vec(); data[offset]^=1; fails(&data, ErrorKind::InvalidWrapper);
    }
    for offset in 3589..3597 {
        let mut data = APPLE.to_vec(); data[offset]=1; fails(&data, ErrorKind::InvalidWrapper);
    }
    let mut data = APPLE.to_vec(); data[270]^=1; fails(&data, ErrorKind::HashMismatch);
    let mut data = APPLE.to_vec(); data[apple_tag(b"HASH").start]^=1; fails(&data, ErrorKind::HashMismatch);
    for at in 0..237 {
        let mut data = APPLE.to_vec(); data[at]^=0xff;
        if let Ok(library)=Library::parse(&data) {
            for f in library.functions() { assert_eq!(library.extract(&f.name).unwrap().air, APPLE[257..3589]); }
        }
    }
}
