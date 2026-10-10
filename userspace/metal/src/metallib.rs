// SPDX-License-Identifier: MIT
//! Bounded extraction of AIR from verified macOS executable MTLB 2.5/2.8 layouts.
//!
//! This is a container reader, not an AIR validator or Metal device. In particular,
//! a container's name, stage, metadata and SHA-256 are not trusted executable
//! authority: the compiler must still verify the actual AIR entry point and stage.
//! See `METALLIB.md` for the real fixture, format sources and unsupported layouts.

use sha2::{Digest, Sha256};
use std::{collections::BTreeSet, fmt, ops::Range};

pub const MAX_LIBRARY_BYTES: usize = crate::MAX_INPUT_BYTES;
pub const MAX_FUNCTIONS: usize = 4096;
pub const MAX_NAME_BYTES: usize = 1024;
pub const MAX_TAGS_PER_GROUP: usize = 64;
const HEADER_BYTES: usize = 88;
const ENDT: [u8; 4] = *b"ENDT";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Stage {
    Vertex,
    Fragment,
    Compute,
}

impl Stage {
    pub fn shader_stage(self) -> crate::ShaderStage {
        match self {
            Self::Vertex => crate::ShaderStage::Vertex,
            Self::Fragment => crate::ShaderStage::Fragment,
            Self::Compute => crate::ShaderStage::Compute,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FunctionInfo {
    pub name: String,
    pub stage: Stage,
    pub air_version: [u16; 2],
    pub language_version: [u16; 2],
    /// Digest of the complete wrapped AIR extent, including alignment padding.
    /// This checks container consistency, not authenticity or shader safety.
    pub wrapped_air_sha256: [u8; 32],
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetadataTag {
    pub name: [u8; 4],
    /// Opaque container metadata. It is never used for GPU admission.
    pub bytes: Vec<u8>,
}

#[derive(Clone, Debug)]
pub struct OwnedFunction {
    pub info: FunctionInfo,
    /// Exact raw LLVM/AIR bitcode bytes, without the container's wrapper/padding.
    pub air: Vec<u8>,
    pub public_metadata: Vec<MetadataTag>,
    pub private_metadata: Vec<MetadataTag>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    InputTooLarge,
    Truncated,
    InvalidMagic,
    UnsupportedPlatform,
    UnsupportedVersion,
    UnsupportedLibraryType,
    UnsupportedTarget,
    FileSizeMismatch,
    InvalidRange,
    OverlappingSections,
    UnsupportedLayout,
    InvalidFunctionCount,
    InvalidGroup,
    TooManyTags,
    DuplicateTag,
    MissingTag,
    UnsupportedTag,
    InvalidName,
    DuplicateName,
    UnsupportedStage,
    InvalidVersion,
    DuplicateOffset,
    InvalidMetadata,
    InvalidWrapper,
    UnsupportedWrapper,
    HashMismatch,
    FunctionNotFound,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Error {
    pub kind: ErrorKind,
    /// Byte offset in the supplied container; zero for non-positional errors.
    pub offset: usize,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "metallib {:?} at byte {}", self.kind, self.offset)
    }
}
impl std::error::Error for Error {}
type Result<T> = std::result::Result<T, Error>;

fn error(kind: ErrorKind, offset: usize) -> Error {
    Error { kind, offset }
}

fn bytes(input: &[u8], offset: usize, size: usize) -> Result<&[u8]> {
    let end = offset
        .checked_add(size)
        .ok_or(error(ErrorKind::InvalidRange, offset))?;
    input
        .get(offset..end)
        .ok_or(error(ErrorKind::Truncated, offset))
}
fn u16_at(input: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(
        bytes(input, offset, 2)?.try_into().unwrap(),
    ))
}
fn u32_at(input: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        bytes(input, offset, 4)?.try_into().unwrap(),
    ))
}
fn u64_at(input: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(
        bytes(input, offset, 8)?.try_into().unwrap(),
    ))
}
fn bounded_range(offset: u64, size: u64, bound: usize, at: usize) -> Result<Range<usize>> {
    let end = offset
        .checked_add(size)
        .ok_or(error(ErrorKind::InvalidRange, at))?;
    if end > bound as u64 || size == 0 {
        return Err(error(ErrorKind::InvalidRange, at));
    }
    // Both values are <= the input slice's usize length, even on 32-bit hosts.
    Ok(offset as usize..end as usize)
}

#[derive(Debug)]
struct Record {
    // Relative section offsets before validation, never exposed to callers.
    offsets: [u64; 3],
    // Required MDSZ in the evidenced 2.8 profile; absent in legacy 2.5.
    declared_air_bytes: Option<u64>,
    air: Range<usize>,
    public_metadata: Vec<MetadataTag>,
    private_metadata: Vec<MetadataTag>,
}

/// Owns a validated container snapshot. Caller mutations cannot change a later
/// extraction, and extraction never returns an alias into caller-owned storage.
#[derive(Debug)]
pub struct Library {
    input: Box<[u8]>,
    functions: Vec<FunctionInfo>,
    records: Vec<Record>,
}

impl Library {
    pub fn parse(input: &[u8]) -> Result<Self> {
        if input.len() > MAX_LIBRARY_BYTES {
            return Err(error(ErrorKind::InputTooLarge, 0));
        }
        bytes(input, 0, HEADER_BYTES)?;
        if &input[..4] != b"MTLB" {
            return Err(error(ErrorKind::InvalidMagic, 0));
        }
        if u16_at(input, 4)? != 0x8001 {
            return Err(error(ErrorKind::UnsupportedPlatform, 4));
        }
        // Accept only layouts pinned to actual compiler/published fixtures.
        // Modern MDSZ is checked independently against each bounded extent.
        let has_mdsz = match (u16_at(input, 6)?, u16_at(input, 8)?) {
            (2, 5) => false,
            (2, 8) => true,
            _ => return Err(error(ErrorKind::UnsupportedVersion, 6)),
        };
        if input[10] != 0 {
            return Err(error(ErrorKind::UnsupportedLibraryType, 10));
        }
        // The modern tuple is the exact macOS15 target emitted by the pinned
        // Apple32023.864 compile. Do not infer other platform/version tuples.
        let target: &[u8] = if has_mdsz { &[0x81, 15, 0, 0, 0] } else { &[0; 5] };
        if &input[11..16] != target {
            return Err(error(ErrorKind::UnsupportedTarget, 11));
        }
        if u64_at(input, 16)? != input.len() as u64 {
            return Err(error(ErrorKind::FileSizeMismatch, 16));
        }
        // In this observed layout the function section size excludes its u32
        // count. The extension ENDT follows all groups and precedes public data.
        let table_size = u64_at(input, 32)?
            .checked_add(4)
            .ok_or(error(ErrorKind::InvalidRange, 32))?;
        let sections = [
            bounded_range(u64_at(input, 24)?, table_size, input.len(), 24)?,
            bounded_range(u64_at(input, 40)?, u64_at(input, 48)?, input.len(), 40)?,
            bounded_range(u64_at(input, 56)?, u64_at(input, 64)?, input.len(), 56)?,
            bounded_range(u64_at(input, 72)?, u64_at(input, 80)?, input.len(), 72)?,
        ];
        for (i, section) in sections.iter().enumerate() {
            if section.start < HEADER_BYTES {
                return Err(error(ErrorKind::OverlappingSections, section.start));
            }
            for previous in &sections[..i] {
                if section.start < previous.end && previous.start < section.end {
                    return Err(error(ErrorKind::OverlappingSections, section.start));
                }
            }
        }
        if sections[0].start != HEADER_BYTES
            || sections[0].end.checked_add(4) != Some(sections[1].start)
            || sections[1].end != sections[2].start
            || sections[2].end != sections[3].start
            || sections[3].end != input.len()
            || bytes(input, sections[0].end, 4)? != ENDT
        {
            return Err(error(ErrorKind::UnsupportedLayout, sections[0].end));
        }
        let count = u32_at(input, sections[0].start)? as usize;
        if count == 0 || count > MAX_FUNCTIONS {
            return Err(error(ErrorKind::InvalidFunctionCount, sections[0].start));
        }
        let mut functions = Vec::with_capacity(count);
        let mut records = Vec::with_capacity(count);
        let mut names = BTreeSet::new();
        let mut cursor = sections[0].start + 4;
        for _ in 0..count {
            let start = cursor;
            let size = u32_at(input, cursor)? as usize;
            // Function size includes the u32 size field, unlike metadata size.
            if size < 8 || size > sections[0].end.saturating_sub(cursor) {
                return Err(error(ErrorKind::InvalidGroup, cursor));
            }
            let end = cursor + size;
            let tags = tags(input, cursor + 4..end)?;
            let mut name = None;
            let mut stage = None;
            let mut digest = None;
            let mut offsets = None;
            let mut versions = None;
            let mut declared_air_bytes = None;
            for tag in tags {
                let payload = &input[tag.payload.clone()];
                match &tag.name {
                    b"NAME" => {
                        if payload.len() < 2
                            || payload.len() > MAX_NAME_BYTES + 1
                            || payload.last() != Some(&0)
                            || payload[..payload.len() - 1].contains(&0)
                        {
                            return Err(error(ErrorKind::InvalidName, tag.payload.start));
                        }
                        name = Some(
                            std::str::from_utf8(&payload[..payload.len() - 1])
                                .map_err(|_| error(ErrorKind::InvalidName, tag.payload.start))?
                                .to_owned(),
                        );
                    }
                    b"TYPE" if payload.len() == 1 => {
                        stage = Some(match payload[0] {
                            0 => Stage::Vertex,
                            1 => Stage::Fragment,
                            2 => Stage::Compute,
                            _ => return Err(error(ErrorKind::UnsupportedStage, tag.payload.start)),
                        });
                    }
                    b"HASH" if payload.len() == 32 => {
                        digest = Some(payload.try_into().unwrap());
                    }
                    b"MDSZ" if has_mdsz => {
                        if payload.len() != 8 {
                            return Err(error(ErrorKind::InvalidGroup, tag.payload.start));
                        }
                        declared_air_bytes = Some(u64_at(payload, 0)?);
                    }
                    b"OFFT" if payload.len() == 24 => {
                        offsets = Some([
                            u64_at(payload, 0)?,
                            u64_at(payload, 8)?,
                            u64_at(payload, 16)?,
                        ]);
                    }
                    b"VERS" if payload.len() == 8 => {
                        let values = [
                            u16_at(payload, 0)?,
                            u16_at(payload, 2)?,
                            u16_at(payload, 4)?,
                            u16_at(payload, 6)?,
                        ];
                        if values[0] == 0 || values[2] == 0 {
                            return Err(error(ErrorKind::InvalidVersion, tag.payload.start));
                        }
                        versions = Some(values);
                    }
                    b"TYPE" | b"HASH" | b"OFFT" | b"VERS" => {
                        return Err(error(ErrorKind::InvalidGroup, tag.payload.start));
                    }
                    _ => return Err(error(ErrorKind::UnsupportedTag, tag.payload.start - 6)),
                }
            }
            let missing = || error(ErrorKind::MissingTag, start);
            let name = name.ok_or_else(missing)?;
            if !names.insert(name.clone()) {
                return Err(error(ErrorKind::DuplicateName, start));
            }
            let versions = versions.ok_or_else(missing)?;
            if has_mdsz && declared_air_bytes.is_none() {
                return Err(missing());
            }
            functions.push(FunctionInfo {
                name,
                stage: stage.ok_or_else(missing)?,
                air_version: [versions[0], versions[1]],
                language_version: [versions[2], versions[3]],
                wrapped_air_sha256: digest.ok_or_else(missing)?,
            });
            records.push(Record {
                offsets: offsets.ok_or_else(missing)?,
                declared_air_bytes,
                air: 0..0,
                public_metadata: Vec::new(),
                private_metadata: Vec::new(),
            });
            cursor = end;
        }
        if cursor != sections[0].end {
            return Err(error(ErrorKind::InvalidGroup, cursor));
        }
        for section_index in 0..3 {
            let section = &sections[section_index + 1];
            let mut order: Vec<_> = (0..count).collect();
            order.sort_unstable_by_key(|i| records[*i].offsets[section_index]);
            if records[order[0]].offsets[section_index] != 0 {
                return Err(error(ErrorKind::InvalidRange, section.start));
            }
            for (position, &index) in order.iter().enumerate() {
                let start = records[index].offsets[section_index];
                let end = order
                    .get(position + 1)
                    .map(|i| records[*i].offsets[section_index])
                    .unwrap_or(section.len() as u64);
                if start == end {
                    return Err(error(ErrorKind::DuplicateOffset, section.start));
                }
                let size = end
                    .checked_sub(start)
                    .ok_or(error(ErrorKind::InvalidRange, section.start))?;
                let relative = bounded_range(start, size, section.len(), section.start)?;
                let extent = section.start + relative.start..section.start + relative.end;
                if section_index == 2 {
                    if records[index].declared_air_bytes.is_some_and(|bytes| bytes != size) {
                        return Err(error(ErrorKind::InvalidRange, extent.start));
                    }
                    let air = wrapper(input, extent.clone())?;
                    let digest: [u8; 32] = Sha256::digest(&input[extent.clone()]).into();
                    if digest != functions[index].wrapped_air_sha256 {
                        return Err(error(ErrorKind::HashMismatch, extent.start));
                    }
                    records[index].air = air;
                } else {
                    // Metadata group sizes EXCLUDE their u32 length field.
                    let size = u32_at(input, extent.start)? as usize;
                    if size.checked_add(4) != Some(extent.len()) {
                        return Err(error(ErrorKind::InvalidMetadata, extent.start));
                    }
                    let parsed = tags(input, extent.start + 4..extent.end)?;
                    let owned = parsed
                        .into_iter()
                        .map(|tag| MetadataTag {
                            name: tag.name,
                            bytes: input[tag.payload].to_vec(),
                        })
                        .collect();
                    if section_index == 0 {
                        records[index].public_metadata = owned;
                    } else {
                        records[index].private_metadata = owned;
                    }
                }
            }
        }
        Ok(Self {
            input: input.into(),
            functions,
            records,
        })
    }

    pub fn functions(&self) -> &[FunctionInfo] {
        &self.functions
    }

    /// Selects an exact, case-sensitive table name and returns owned AIR. The
    /// compiler must still match this name and stage against AIR reflection.
    pub fn extract(&self, name: &str) -> Result<OwnedFunction> {
        let index = self
            .functions
            .iter()
            .position(|f| f.name == name)
            .ok_or(error(ErrorKind::FunctionNotFound, 0))?;
        let record = &self.records[index];
        Ok(OwnedFunction {
            info: self.functions[index].clone(),
            air: self.input[record.air.clone()].to_vec(),
            public_metadata: record.public_metadata.clone(),
            private_metadata: record.private_metadata.clone(),
        })
    }
}

struct Tag {
    name: [u8; 4],
    payload: Range<usize>,
}

fn tags(input: &[u8], range: Range<usize>) -> Result<Vec<Tag>> {
    let mut cursor = range.start;
    let mut result = Vec::new();
    let mut seen = BTreeSet::new();
    while cursor < range.end {
        if range.end - cursor < 4 {
            return Err(error(ErrorKind::InvalidGroup, cursor));
        }
        let name: [u8; 4] = bytes(input, cursor, 4)?.try_into().unwrap();
        cursor += 4;
        if name == ENDT {
            return if cursor == range.end {
                Ok(result)
            } else {
                Err(error(ErrorKind::InvalidGroup, cursor))
            };
        }
        if !seen.insert(name) {
            return Err(error(ErrorKind::DuplicateTag, cursor - 4));
        }
        if result.len() >= MAX_TAGS_PER_GROUP {
            return Err(error(ErrorKind::TooManyTags, cursor - 4));
        }
        if range.end - cursor < 2 {
            return Err(error(ErrorKind::InvalidGroup, cursor));
        }
        let size = u16_at(input, cursor)? as usize;
        cursor += 2;
        if size > range.end - cursor {
            return Err(error(ErrorKind::InvalidGroup, cursor));
        }
        result.push(Tag {
            name,
            payload: cursor..cursor + size,
        });
        cursor += size;
    }
    Err(error(ErrorKind::InvalidGroup, cursor))
}

fn wrapper(input: &[u8], range: Range<usize>) -> Result<Range<usize>> {
    let wrapped = &input[range.clone()];
    if wrapped.len() < 24 || &wrapped[..4] != b"\xde\xc0\x17\x0b" {
        return Err(error(ErrorKind::InvalidWrapper, range.start));
    }
    // LLVM defines version zero. The verified AIR layout uses an immediate
    // bitstream and CPUType UINT32_MAX; other wrapper extensions are unsupported.
    if u32_at(wrapped, 4)? != 0 || u32_at(wrapped, 8)? != 20 || u32_at(wrapped, 16)? != u32::MAX {
        return Err(error(ErrorKind::UnsupportedWrapper, range.start));
    }
    let size = u32_at(wrapped, 12)? as usize;
    let end = 20usize
        .checked_add(size)
        .ok_or(error(ErrorKind::InvalidWrapper, range.start))?;
    if size < 4
        || size % 4 != 0
        || end > wrapped.len()
        || end.checked_add(15).map(|v| v & !15) != Some(wrapped.len())
        || &wrapped[20..24] != b"BC\xc0\xde"
        || wrapped[end..].iter().any(|b| *b != 0)
    {
        return Err(error(ErrorKind::InvalidWrapper, range.start));
    }
    Ok(range.start + 20..range.start + end)
}

#[cfg(test)]
mod tests;
