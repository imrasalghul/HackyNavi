//! Owned, offline GFX1100 compute products from the real AIR/SPIR-V compiler.
//! This is neither MTLDevice registration nor a kernel code-admission proof.
// SPDX-License-Identifier: MIT
use crate::{dispatch_plan::{self, DispatchRegion}, CompiledShader, Scratch, ShaderResult};
use metal2vulkan::reflect::{ResourceAccess, ResourceKind};
use serde_json::Value;
use std::{collections::BTreeMap, ffi::{c_char, CStr, CString}, fs::{self, File},
    io::Read, path::Path, process::{Command, Stdio}, time::{Duration, Instant}};

const MAX_CODE: usize = 1024 * 1024;
const MAX_METADATA: usize = 64 * 1024;
const MESA: &str = "35b085c4c067f198ce0b6437d94678e47b9c645f";

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PipelineInfo {
    pub variant_count: u32, pub region_count: u32, pub resource_count: u32,
    pub descriptor_table_bytes: u32, pub push_constant_bytes: u32,
    pub descriptor_set: u32, pub descriptor_stride_bytes: u32, pub target_gfx: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct VariantInfo {
    pub local_size: [u32; 3], pub code_bytes: u32, pub executable_bytes: u32,
    pub code_alignment: u32, pub compute_pgm_rsrc: [u32; 3],
    pub dispatch_initiator_required_bits: u32, pub num_sgprs: u32, pub num_vgprs: u32,
    pub descriptor_binding_mask: u32, pub descriptor_read_mask: u32, pub descriptor_write_mask: u32,
    pub descriptor_table_bytes: u32, pub push_constant_bytes: u32, pub wave_size: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceInfo {
    pub descriptor_set: u32, pub binding: u32, pub metal_index: u32,
    /// Actual emitted SPIR-V access, conservative across every variant: read=1, write=2.
    pub access: u32, pub declared_element_bytes: u32, pub reflected_access: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct PipelineRegion {pub variant_index: u32, pub dispatch: DispatchRegion}
struct Variant {info: VariantInfo, code: Vec<u8>, metadata: Vec<u8>}
struct Product {
    info: PipelineInfo, variants: Vec<Variant>, regions: Vec<PipelineRegion>,
    resources: Vec<ResourceInfo>, spirv: Vec<u8>, reflection: Vec<u8>, entry: Vec<u8>,
}
pub struct PipelineResult {product: Option<Product>, error: CString}

fn instructions(words: &[u32]) -> Result<Vec<&[u32]>, String> {
    if words.len()<5 || words[0]!=0x0723_0203 || words.len()>crate::MAX_INPUT_BYTES/4 {
        return Err("invalid or oversized compiled SPIR-V".into());
    }
    let mut out=Vec::new(); let mut pos=5;
    while pos<words.len() {
        let count=(words[pos]>>16) as usize;
        if count==0 || count>words.len()-pos {return Err("invalid SPIR-V instruction extent".into());}
        out.push(&words[pos..pos+count]);pos+=count;
    }
    Ok(out)
}

/// Narrow, conservative pointer provenance for the backend's SSBO load/store
/// subset. A bitset is propagated through actual SSA pointer definitions; no
/// shader names, AIR argument names or caller-provided write masks are used.
/// Unsupported pointer forms are errors, not guessed read-only resources.
fn resource_contract(shader: &CompiledShader) -> Result<(Vec<ResourceInfo>,u32,u32),String> {
    let r=&shader.reflection;r.validate_descriptor_abi()?;
    if !r.argument_buffer_fields.is_empty() || !r.function_constants.is_empty() ||
        !r.runtime_sampler_specializations.is_empty() || !r.runtime_storage_image_specializations.is_empty() {
        return Err("pipeline v1 does not support argument buffers or runtime resource/function specialization".into());
    }
    let mut resources=BTreeMap::new();
    for binding in &r.bindings {
        if binding.kind!=ResourceKind::Buffer {return Err("pipeline v1 supports ordinary buffers only".into());}
        let d=binding.descriptor.ok_or("buffer lacks descriptor location")?;
        if d.set!=0 || d.binding>=32 || d.count!=1 || resources.contains_key(&d.binding) {
            return Err("pipeline requires distinct singleton buffers in set 0, bindings 0..31".into());
        }
        let access=match binding.access {Some(ResourceAccess::ReadOnly)=>1,Some(ResourceAccess::WriteOnly)=>2,
            Some(ResourceAccess::ReadWrite)=>3,Some(ResourceAccess::Unused)=>0,
            _=>return Err("buffer access is absent or unsupported".into())};
        resources.insert(d.binding,ResourceInfo{descriptor_set:0,binding:d.binding,metal_index:binding.metal_index,
            access:0,reflected_access:access,declared_element_bytes:binding.declared_size.unwrap_or(0)});
    }
    let ins=instructions(&shader.spirv)?;
    let mut pointer_types=BTreeMap::new();let mut decorations=BTreeMap::<u32,(Option<u32>,Option<u32>)>::new();
    for i in &ins {
        match i[0]&0xffff {
            32 if i.len()==4=>{pointer_types.insert(i[1],i[2]);}, // OpTypePointer
            71 if i.len()==4=>{ // OpDecorate DescriptorSet/Binding
                if i[2]==33 {decorations.entry(i[1]).or_default().1=Some(i[3]);}
                if i[2]==34 {decorations.entry(i[1]).or_default().0=Some(i[3]);}
            },
            // No passing pointers through calls, integer addresses, copy-memory
            // or atomics in this initial product ABI; the backend also rejects
            // unsupported final intrinsics independently.
            57|63|64|68|117|120|121|122|123|227..=242|318|319 =>
                return Err("unsupported SPIR-V call, pointer conversion, copy, array length or atomic".into()),
            _=>{},
        }
    }
    let mut pointers=BTreeMap::<u32,u32>::new(); let mut pending=Vec::<(u32,Vec<u32>)>::new();
    for i in &ins {
        let op=i[0]&0xffff;
        if i.len()<3 || !pointer_types.contains_key(&i[1]) {continue;}
        // Only result-bearing opcodes below interpret operand1 as a type.
        // Debug/decorations contain arbitrary IDs and are not results.
        if !matches!(op,59|61|65|66|67|70|83|124|169|245) {continue;}
        match op {
            59 if i.len()==4=>{
                let storage=pointer_types[&i[1]];
                if storage!=i[3] {return Err("inconsistent pointer storage class".into());}
                let mask=match storage {
                    12=>{
                        let (set,binding)=decorations.get(&i[2]).copied().ok_or("undecorated storage buffer")?;
                        let binding=binding.ok_or("missing SPIR-V binding")?;
                        if set!=Some(0)||binding>=32||!resources.contains_key(&binding) {
                            return Err("SPIR-V storage binding disagrees with reflection".into());
                        }
                        1<<binding
                    },
                    1|9=>0, // shader input / our checked dispatch push payload
                    _=>return Err("pipeline v1 rejects other pointer storage classes".into()),
                };pointers.insert(i[2],mask);
            },
            65|66|67|70 if i.len()>=5=>pending.push((i[2],vec![i[3]])),
            83 if i.len()==4=>pending.push((i[2],vec![i[3]])),
            169 if i.len()==6=>pending.push((i[2],vec![i[4],i[5]])),
            245 if i.len()>=5 && i.len()%2==1=>pending.push((i[2],i[3..].iter().step_by(2).copied().collect())),
            _=>return Err("unsupported pointer-producing SPIR-V operation".into()),
        }
    }
    if pointers.len()+pending.len()>4096 {return Err("pipeline pointer graph exceeds 4096 definitions".into());}
    // Acyclic provenance, at most64 levels. Pointer cycles are explicitly not
    // supported yet; non-pointer arithmetic/control flow remains unrestricted.
    for _ in 0..64 {
        let old=pending.len();let mut next=Vec::new();
        for (id,sources) in pending {
            if sources.iter().all(|s|pointers.contains_key(s)) {
                pointers.insert(id,sources.iter().fold(0,|m,s|m|pointers[s]));
            } else {next.push((id,sources));}
        }
        pending=next;if pending.is_empty()||pending.len()==old {break;}
    }
    if !pending.is_empty() {return Err("unresolved or cyclic SPIR-V buffer pointer provenance".into());}
    let mut reads=0;let mut writes=0;
    for i in &ins {
        match i[0]&0xffff {
            61 if i.len()>=4=>reads|=*pointers.get(&i[3]).ok_or("unknown load pointer provenance")?,
            62 if i.len()>=3=>{
                if pointers.contains_key(&i[2]) {return Err("storing pointers is unsupported".into());}
                writes|=*pointers.get(&i[1]).ok_or("unknown store pointer provenance")?;
            },
            124 if i.len()==4 && pointers.contains_key(&i[3])=>return Err("pointer bitcast is unsupported".into()),
            _=>{},
        }
    }
    for item in resources.values_mut() {
        item.access=u32::from(reads&(1<<item.binding)!=0)|u32::from(writes&(1<<item.binding)!=0)*2;
        if item.access & !item.reflected_access !=0 {return Err("actual SPIR-V load/store exceeds reflected buffer access".into());}
    }
    Ok((resources.into_values().collect(),reads,writes))
}

fn uint(v:&Value,k:&str)->Result<u32,String> {
    v[k].as_u64().and_then(|n|u32::try_from(n).ok()).ok_or_else(||format!("invalid ACO metadata {k}"))
}
fn expect(v:&Value,k:&str,want:Value)->Result<(),String> {
    if v[k]!=want {Err(format!("unsupported ACO metadata {k}"))} else {Ok(())}
}
fn parse_variant(metadata:&[u8],code_len:usize,local:[u32;3],mask:u32,reads:u32,writes:u32,table:u32)
    ->Result<VariantInfo,String> {
    let v:Value=serde_json::from_slice(metadata).map_err(|e|format!("ACO metadata JSON: {e}"))?;
    for (key,value) in [("abi_version",1),("wave_size",32),("code_alignment",256),("descriptor_set",0),
        ("descriptor_stride_bytes",16),("push_constant_bytes",48),("compute_pgm_rsrc3",0),
        ("dispatch_initiator_required_bits",32768),("num_shared_vgprs",0),("spilled_sgprs",0),
        ("spilled_vgprs",0),("lds_bytes",0),("scratch_bytes_per_wave",0),("constants_bytes",0)] {
        expect(&v,key,value.into())?;
    }
    for (key,value) in [("compiler","Mesa NIR/ACO"),("mesa_commit",MESA),("target","gfx1100"),
        ("target_kind","explicit_offline_compiler_target"),("descriptor_format","GFX11 raw buffer descriptor, byte bounds")] {
        expect(&v,key,value.into())?;
    }
    expect(&v,"workgroup_size",serde_json::json!(local))?;
    expect(&v,"relocations",serde_json::json!([]))?;
    expect(&v,"user_sgprs",serde_json::json!({"descriptor_table_gpu_va":[0,1],"push_constants_gpu_va":[2,3]}))?;
    expect(&v,"system_sgprs",serde_json::json!({"workgroup_x":4,"workgroup_y":5,"workgroup_z":6,"tg_size":7}))?;
    expect(&v,"system_vgprs",serde_json::json!({"packed_local_invocation_id":0}))?;
    let bytes=uint(&v,"code_bytes")?;let executable=uint(&v,"executable_bytes")?;
    if bytes as usize!=code_len||code_len==0||code_len>MAX_CODE||bytes%4!=0||executable==0||executable>bytes||executable%4!=0 {
        return Err("ACO code/executable extent mismatch".into());
    }
    let vgprs=uint(&v,"num_vgprs")?;let sgprs=uint(&v,"num_sgprs")?;let float=uint(&v,"float_mode")?;
    if vgprs==0||vgprs>256||sgprs>128||float>255 {return Err("ACO register allocation is outside launch ABI".into());}
    let ordered=v["mem_ordered"].as_bool().ok_or("missing memory ordering mode")?;
    expect(&v,"wgp_mode",true.into())?;
    let rsrc1=(vgprs-1)/8|(1<<21)|(float<<12)|(1<<29)|(u32::from(ordered)<<30);
    let rsrc2=0x788|((if local[2]>1 {2} else {u32::from(local[1]>1)})<<11);
    expect(&v,"compute_pgm_rsrc1",rsrc1.into())?;expect(&v,"compute_pgm_rsrc2",rsrc2.into())?;
    let used=uint(&v,"descriptor_binding_mask")?;
    if used & !mask !=0 {return Err("ACO references a descriptor absent from reflection".into());}
    Ok(VariantInfo{local_size:local,code_bytes:bytes,executable_bytes:executable,code_alignment:256,
        compute_pgm_rsrc:[rsrc1,rsrc2,0],dispatch_initiator_required_bits:32768,num_sgprs:sgprs,num_vgprs:vgprs,
        descriptor_binding_mask:used,descriptor_read_mask:reads,descriptor_write_mask:writes,
        descriptor_table_bytes:table,push_constant_bytes:48,wave_size:32})
}

fn read_bounded(path:&Path,max:usize)->Result<Vec<u8>,String> {
    let file=File::open(path).map_err(|e|format!("read {}: {e}",path.display()))?;
    let meta=file.metadata().map_err(|e|e.to_string())?;
    if !meta.is_file()||meta.len()>max as u64 {return Err("oversized/nonregular compiler output".into());}
    let mut bytes=Vec::new();file.take(max as u64+1).read_to_end(&mut bytes).map_err(|e|e.to_string())?;
    if bytes.len()>max {return Err("compiler output grew beyond bound".into());} Ok(bytes)
}
fn check_compile_budget(directory:&Path,deadline:Instant)->Result<(),String> {
    if Instant::now()>=deadline {return Err("ACO exceeded deadline; no product published".into());}
    for (name,max) in [("compiler.log",MAX_METADATA),("shader.bin",MAX_CODE),("shader.json",MAX_METADATA),("aco.txt",4*MAX_CODE)] {
        match fs::metadata(directory.join(name)) {
            Ok(meta) if !meta.is_file()||meta.len()>max as u64=>
                return Err("ACO exceeded output size or produced a nonregular file; no product published".into()),
            Ok(_)=>{},
            Err(e) if e.kind()==std::io::ErrorKind::NotFound=>{},
            Err(e)=>return Err(format!("check ACO output budget: {e}")),
        }
    }
    Ok(())
}
fn compile_variant(compiler:&Path,module:&Path,entry:&str,local:[u32;3],directory:&Path,deadline:Instant)->Result<(),String> {
    check_compile_budget(directory,deadline)?;
    fs::create_dir(directory).map_err(|e|e.to_string())?;
    let log_path=directory.join("compiler.log");let log=File::create(&log_path).map_err(|e|e.to_string())?;
    check_compile_budget(directory,deadline)?;
    let mut child=Command::new(compiler).arg(module).arg(entry).args(local.map(|n|n.to_string())).arg(directory)
        .stdin(Stdio::null()).stdout(log.try_clone().map_err(|e|e.to_string())?).stderr(log)
        .spawn().map_err(|e|format!("start real ACO compiler: {e}"))?;
    loop {
        let status=match child.try_wait() {Ok(s)=>s,Err(e)=>{let _=child.kill();let _=child.wait();return Err(e.to_string());}};
        // Check completed children as well as running ones: a fast exit must
        // not bypass either the shared deadline or final output-size limits.
        if let Err(error)=check_compile_budget(directory,deadline) {
            let _=child.kill();let _=child.wait();return Err(error);
        }
        if let Some(status)=status {
            if status.success() {return Ok(());}
            let log=read_bounded(&log_path,MAX_METADATA).unwrap_or_default();
            return Err(format!("ACO failed for {local:?}: {status}: {}",String::from_utf8_lossy(&log)));
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}
fn build(shader:&CompiledShader,mode:u32,grid:[u32;3],local:[u32;3],compiler:&Path)->Result<Product,String> {
    if !compiler.is_absolute()||!compiler.is_file() {return Err("an absolute path to the trusted real ACO compiler is required".into());}
    let nominal=local.iter().try_fold(1u32,|n,v|n.checked_mul(*v)).ok_or("nominal local product overflow")?;
    if nominal==0||nominal>1024 {return Err("nominal ACO local size must fit 1024 threads".into());}
    let planned=dispatch_plan::plan(&shader.reflection,mode,grid,local)?;
    if planned.is_empty() {return Err("an empty dispatch has no executable pipeline product".into());}
    let (resources,reads,writes)=resource_contract(shader)?;
    let mask=resources.iter().fold(0,|m,r|m|(1<<r.binding));
    let table=resources.iter().map(|r|(r.binding+1)*16).max().unwrap_or(0);
    let mut locals=Vec::new();let mut regions=Vec::new();
    for region in planned {
        let count=region.local_size.iter().try_fold(1u32,|n,v|n.checked_mul(*v)).ok_or("local product overflow")?;
        if count>1024||region.local_size.iter().any(|n|*n==0||*n>1024) {return Err("ACO local size must fit 1024 threads".into());}
        if region.push_constant_offset!=0||!matches!(region.push_constant_size,0|48) {
            return Err("ACO supports only the dispatch payload at push offset zero".into());
        }
        let variant=match locals.iter().position(|l|*l==region.local_size) {
            Some(index)=>index,None=>{locals.push(region.local_size);locals.len()-1},
        };
        regions.push(PipelineRegion{variant_index:variant as u32,dispatch:region});
    }
    if crate::execution_entry(&shader.spirv,crate::ShaderStage::Compute)?!=shader.spirv_entry_point {
        return Err("compiled entry disagrees with actual SPIR-V OpEntryPoint".into());
    }
    if shader.spirv_entry_point.len()>255 {return Err("ACO entry name exceeds 255 bytes".into());}
    let scratch=Scratch::new()?;
    crate::require_validator_with_tools(&scratch.0,shader.tools.as_ref())?;
    let spirv=shader.spirv_bytes();metal2vulkan::tools::spirv_val_bytes_with_tools(&spirv,&scratch.0,shader.tools.as_ref())?;
    let module=scratch.0.join("input.spv");fs::write(&module,&spirv).map_err(|e|e.to_string())?;
    let deadline=Instant::now()+Duration::from_secs(120);let mut variants=Vec::new();
    for (index,local) in locals.into_iter().enumerate() {
        let directory=scratch.0.join(format!("variant-{index}"));
        compile_variant(compiler,&module,&shader.spirv_entry_point,local,&directory,deadline)?;
        let code=read_bounded(&directory.join("shader.bin"),MAX_CODE)?;
        let metadata=read_bounded(&directory.join("shader.json"),MAX_METADATA)?;
        let info=parse_variant(&metadata,code.len(),local,mask,reads,writes,table)?;
        check_compile_budget(&directory,deadline)?;
        variants.push(Variant{info,code,metadata});
    }
    let reflection=serde_json::to_vec(&shader.reflection).map_err(|e|e.to_string())?;
    if Instant::now()>=deadline {return Err("ACO product deadline expired before publication".into());}
    Ok(Product{info:PipelineInfo{variant_count:variants.len() as u32,region_count:regions.len() as u32,
        resource_count:resources.len() as u32,descriptor_table_bytes:table,push_constant_bytes:48,
        descriptor_set:0,descriptor_stride_bytes:16,target_gfx:1100},variants,regions,resources,spirv,reflection,
        entry:shader.spirv_entry_point.as_bytes().to_vec()})
}

#[no_mangle]
pub extern "C" fn hn_pipeline_abi_version()->u32 {1}
/// Safety: live shader, readable triplets, NUL-terminated UTF-8 compiler path.
/// Caller controls a trusted compiler executable; this is not a sandbox.
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_build(shader:*const ShaderResult,mode:u32,grid:*const u32,local:*const u32,
    compiler:*const c_char)->*mut PipelineResult {
    let result=std::panic::catch_unwind(||{
        if shader.is_null()||grid.is_null()||local.is_null()||compiler.is_null() {return Err("null pipeline input".into());}
        let shader=(*shader).shader.as_ref().ok_or("shader compilation did not succeed")?;
        let grid=std::ptr::read_unaligned(grid.cast());let local=std::ptr::read_unaligned(local.cast());
        let compiler=CStr::from_ptr(compiler).to_str().map_err(|_|"compiler path must be UTF-8")?;
        build(shader,mode,grid,local,Path::new(compiler))
    }).unwrap_or_else(|_|Err("pipeline builder panicked; no product published".into()));
    let result=match result {
        Ok(product)=>PipelineResult{product:Some(product),error:CString::default()},
        Err(error)=>PipelineResult{product:None,error:CString::new(error.replace('\0',"\\0")).unwrap()},
    };Box::into_raw(Box::new(result))
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_error(result:*const PipelineResult)->*const c_char {
    result.as_ref().map_or(std::ptr::null(),|p|p.error.as_ptr())
}
unsafe fn copy_record<T:Copy>(value:Option<&T>,out:*mut T,bytes:usize)->i32 {
    if out.is_null()||bytes!=std::mem::size_of::<T>()||(out as usize)%std::mem::align_of::<T>()!=0 {return 0;}
    if let Some(value)=value {out.write(*value);1} else {0}
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_info(result:*const PipelineResult,out:*mut PipelineInfo,bytes:usize)->i32 {
    copy_record(result.as_ref().and_then(|r|r.product.as_ref()).map(|p|&p.info),out,bytes)
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_variant(result:*const PipelineResult,index:u32,out:*mut VariantInfo,bytes:usize)->i32 {
    copy_record(result.as_ref().and_then(|r|r.product.as_ref()).and_then(|p|p.variants.get(index as usize)).map(|v|&v.info),out,bytes)
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_region(result:*const PipelineResult,index:u32,out:*mut PipelineRegion,bytes:usize)->i32 {
    copy_record(result.as_ref().and_then(|r|r.product.as_ref()).and_then(|p|p.regions.get(index as usize)),out,bytes)
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_resource(result:*const PipelineResult,index:u32,out:*mut ResourceInfo,bytes:usize)->i32 {
    copy_record(result.as_ref().and_then(|r|r.product.as_ref()).and_then(|p|p.resources.get(index as usize)),out,bytes)
}
fn blob(product:&Product,kind:u32,index:u32)->Option<&[u8]> {
    match kind {
        0=>product.variants.get(index as usize).map(|v|v.code.as_slice()),
        1=>product.variants.get(index as usize).map(|v|v.metadata.as_slice()),
        2 if index==0=>Some(&product.spirv),3 if index==0=>Some(&product.reflection),4 if index==0=>Some(&product.entry),
        _=>None,
    }
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_blob_size(result:*const PipelineResult,kind:u32,index:u32)->usize {
    result.as_ref().and_then(|r|r.product.as_ref()).and_then(|p|blob(p,kind,index)).map_or(0,|b|b.len())
}
/// Copy exact bytes, with no trailing NUL for textual blobs. No borrowed code.
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_copy_blob(result:*const PipelineResult,kind:u32,index:u32,out:*mut u8,bytes:usize)->i32 {
    let Some(blob)=result.as_ref().and_then(|r|r.product.as_ref()).and_then(|p|blob(p,kind,index)) else {return 0;};
    if out.is_null()||bytes!=blob.len() {return 0;}
    std::ptr::copy_nonoverlapping(blob.as_ptr(),out,blob.len());1
}
#[no_mangle]
pub unsafe extern "C" fn hn_pipeline_free(result:*mut PipelineResult) {if !result.is_null(){drop(Box::from_raw(result));}}

#[cfg(test)]
mod tests {
    use super::*;
    fn actual_shader()->CompiledShader {
        let bytes=include_bytes!("../../compiler/fixtures/apple-add-one/translated/shader.spv");
        CompiledShader{spirv:bytes.chunks_exact(4).map(|b|u32::from_le_bytes(b.try_into().unwrap())).collect(),
            spirv_entry_point:"main".into(),reflection:serde_json::from_str(include_str!("../../compiler/fixtures/apple-add-one/translated/reflection.json")).unwrap(),tools:None}
    }
    #[test]
    fn actual_buffer_effects_and_contradictions() {
        let mut shader=actual_shader();let (resources,reads,writes)=resource_contract(&shader).unwrap();
        assert_eq!((reads,writes),(1,2));assert_eq!(resources[0].access,1);assert_eq!(resources[1].access,2);
        shader.reflection.bindings[1].access=Some(ResourceAccess::ReadOnly);
        assert!(resource_contract(&shader).unwrap_err().contains("exceeds reflected"));
        shader=actual_shader();shader.reflection.bindings[1].descriptor.as_mut().unwrap().binding=3;
        assert!(resource_contract(&shader).is_err());
        shader=actual_shader();shader.spirv[5]=0;assert!(resource_contract(&shader).is_err());
    }
    #[test]
    fn abi_layout_and_failure_preservation() {
        assert_eq!(std::mem::size_of::<PipelineInfo>(),32);assert_eq!(std::mem::size_of::<VariantInfo>(),72);
        assert_eq!(std::mem::size_of::<ResourceInfo>(),24);assert_eq!(std::mem::size_of::<PipelineRegion>(),120);
        unsafe {let mut sentinel=PipelineInfo{variant_count:99,..PipelineInfo::default()};
            assert_eq!(hn_pipeline_info(std::ptr::null(),&mut sentinel,32),0);assert_eq!(sentinel.variant_count,99);
            assert_eq!(hn_pipeline_blob_size(std::ptr::null(),0,0),0);}
    }
    #[test]
    fn reject_inconsistent_actual_aco_launch_metadata() {
        let source=include_bytes!("../../compiler/fixtures/apple-add-one/shader.json");
        let length=include_bytes!("../../compiler/fixtures/apple-add-one/shader.bin").len();
        let actual=parse_variant(source,length,[64,1,1],3,1,2,32).unwrap();
        assert_eq!(actual.compute_pgm_rsrc,[0x202c0002,0x788,0]);
        for (field,value) in [("wave_size",64),("scratch_bytes_per_wave",4),("num_shared_vgprs",1),
            ("spilled_sgprs",1),("spilled_vgprs",1),("lds_bytes",128),("code_bytes",1),
            ("executable_bytes",0),("compute_pgm_rsrc1",0),("compute_pgm_rsrc2",0),
            ("compute_pgm_rsrc3",1),("num_vgprs",0),("descriptor_binding_mask",4),("push_constant_bytes",64)] {
            let mut metadata:Value=serde_json::from_slice(source).unwrap();metadata[field]=value.into();
            assert!(parse_variant(&serde_json::to_vec(&metadata).unwrap(),length,[64,1,1],3,1,2,32).is_err(),"{field}");
        }
        assert!(parse_variant(source,length,[3,1,1],3,1,2,32).is_err());
        assert!(parse_variant(source,length-1,[64,1,1],3,1,2,32).is_err());
    }
    #[test]
    fn completed_outputs_and_expired_deadline_are_rejected() {
        let scratch=Scratch::new().unwrap();let future=Instant::now()+Duration::from_secs(60);
        assert!(check_compile_budget(&scratch.0,future).is_ok());
        assert!(check_compile_budget(&scratch.0,Instant::now()).is_err());
        for (name,max) in [("compiler.log",MAX_METADATA),("shader.bin",MAX_CODE),("shader.json",MAX_METADATA),("aco.txt",4*MAX_CODE)] {
            let path=scratch.0.join(name);let file=File::create(&path).unwrap();
            file.set_len(max as u64+1).unwrap();drop(file);
            assert!(check_compile_budget(&scratch.0,future).is_err(),"{name}");fs::remove_file(path).unwrap();
        }
    }
}
