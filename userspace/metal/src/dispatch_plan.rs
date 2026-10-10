//! C access to the real translator's dispatch planner, tied to a compiled result.
//! No GPU, device registration, synthesized shader or submission authority.
// SPDX-License-Identifier: MIT
use crate::{KernelDispatch, ShaderResult};
use metal2vulkan::reflect::{ShaderReflection, ShaderStage, KERNEL_LOCAL_SIZE_SPEC_IDS};
use std::ffi::{c_char, CString};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DispatchRegion {
    pub local_size: [u32; 3],
    pub group_count: [u32; 3],
    pub thread_base: [u32; 3],
    pub threadgroup_base: [u32; 3],
    pub local_size_spec_ids: [u32; 3],
    pub push_constant_offset: u32,
    pub push_constant_size: u32,
    pub push_constants: [u32; 12],
}
pub struct DispatchPlan {
    regions: Vec<DispatchRegion>,
    error: CString,
}
fn product(value: [u32; 3]) -> Result<u64, String> {
    value.iter().try_fold(1u64, |n, v| n.checked_mul(u64::from(*v)))
        .ok_or_else(|| "dispatch dimension product overflows u64".into())
}
pub(crate) fn plan(reflection: &ShaderReflection, mode: u32, grid: [u32; 3], local: [u32; 3])
    -> Result<Vec<DispatchRegion>, String> {
    reflection.validate_descriptor_abi()?;
    if reflection.stage != ShaderStage::Kernel {
        return Err("dispatch requires a compiled compute kernel".into());
    }
    let dispatch=reflection.kernel_dispatch.ok_or("missing kernel dispatch contract")?;
    if local.contains(&0) {return Err("local dimensions must be nonzero".into());}
    let local_threads=product(local)?;
    if local_threads>u64::from(u32::MAX) {return Err("local size product overflows u32".into());}
    if reflection.max_work_group_size.is_some_and(|max| local_threads>u64::from(max)) {
        return Err("local size exceeds the compiled AIR maximum workgroup size".into());
    }
    product(grid)?;
    match (mode,dispatch) {
        (0,KernelDispatch::Workgroups) => {
            // The actual translator emits literal constants in this mode,
            // not SpecIds. A different local size requires a real recompile.
            if reflection.local_size!=Some(local) {
                return Err("whole-group local size differs from the compiled module".into());
            }
            if grid.contains(&0) {return Ok(Vec::new());}
            // Whole-group dispatch has no translator grid push payload. Check
            // its actual thread extent too; never silently wrap grid*local.
            let mut threads=[0u32;3];
            for n in 0..3 {threads[n]=grid[n].checked_mul(local[n]).ok_or("thread extent overflows u32")?;}
            product(threads)?;
            Ok(vec![DispatchRegion {local_size:local,group_count:grid,
                local_size_spec_ids:[u32::MAX;3],..DispatchRegion::default()}])
        }
        (1,KernelDispatch::ThreadsDynamic{..}|KernelDispatch::ThreadsFixed{..}) => {
            // Use the original translator planner, including every boundary
            // region and its exact48-byte payload. No rounded-up substitution.
            let planned=dispatch.plan(local,Some(grid))?;
            if planned.regions.len()>8 {return Err("translator exceeded the eight-region contract".into());}
            product(planned.threadgroups_per_grid)?;
            let push=dispatch.push_constant_range().ok_or("missing dispatch push range")?;
            if push.size!=48 {return Err("unsupported dispatch push size".into());}
            Ok(planned.regions.iter().map(|region| DispatchRegion {
                local_size:region.local_size,group_count:region.group_count,
                thread_base:region.thread_base,threadgroup_base:region.threadgroup_base,
                local_size_spec_ids:KERNEL_LOCAL_SIZE_SPEC_IDS,push_constant_offset:push.offset,
                push_constant_size:push.size,push_constants:planned.push_constants(*region),
            }).collect())
        }
        _=>Err("dispatch mode does not match the compiled reflection contract".into()),
    }
}
#[no_mangle]
pub extern "C" fn hn_shader_dispatch_abi_version()->u32 {1}
/// Safety: nonnull pointers refer to a live result and three readable u32s.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_plan_dispatch(shader:*const ShaderResult,mode:u32,
    grid:*const u32,local:*const u32)->*mut DispatchPlan {
    let result=std::panic::catch_unwind(|| {
        if shader.is_null()||grid.is_null()||local.is_null() {return Err("null dispatch input".into());}
        let shader=(*shader).shader.as_ref().ok_or("shader compilation did not succeed")?;
        let grid=std::ptr::read_unaligned(grid.cast::<[u32;3]>());
        let local=std::ptr::read_unaligned(local.cast::<[u32;3]>());
        plan(&shader.reflection,mode,grid,local)
    }).unwrap_or_else(|_|Err("dispatch planner panicked; no regions published".into()));
    let value=match result {
        Ok(regions)=>DispatchPlan{regions,error:CString::default()},
        Err(error)=>DispatchPlan{regions:Vec::new(),error:CString::new(error.replace('\0',"\\0")).unwrap()},
    };
    Box::into_raw(Box::new(value))
}
/// Safety: nonnull plan is a live handle returned above. String is borrowed.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_dispatch_plan_error(plan:*const DispatchPlan)->*const c_char {
    plan.as_ref().map_or(std::ptr::null(),|p|p.error.as_ptr())
}
/// Safety: nonnull plan is a live handle returned above.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_dispatch_plan_region_count(plan:*const DispatchPlan)->u32 {
    plan.as_ref().map_or(0,|p|p.regions.len() as u32)
}
/// Safety: live plan and writable, aligned output of the supplied size.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_dispatch_plan_region(plan:*const DispatchPlan,index:u32,
    output:*mut DispatchRegion,bytes:usize)->i32 {
    if output.is_null()||bytes!=std::mem::size_of::<DispatchRegion>()||
        (output as usize)%std::mem::align_of::<DispatchRegion>()!=0 {return 0;}
    let Some(region)=plan.as_ref().and_then(|p|p.regions.get(index as usize)) else {return 0;};
    output.write(*region);1
}
/// Safety: null or an owned handle returned above, exactly once.
#[no_mangle]
pub unsafe extern "C" fn hn_shader_dispatch_plan_free(plan:*mut DispatchPlan) {
    if !plan.is_null(){drop(Box::from_raw(plan));}
}

#[cfg(test)]
mod tests {
    use super::*;
    fn reflection()->ShaderReflection {
        serde_json::from_str(include_str!("../../compiler/fixtures/apple-add-one/translated/reflection.json")).unwrap()
    }
    #[test]
    fn actual_fixture_and_tail_payload() {
        assert_eq!(std::mem::size_of::<DispatchRegion>(),116);
        let r=reflection();let first=plan(&r,1,[4096,1,1],[64,1,1]).unwrap();assert_eq!(first.len(),1);
        assert_eq!(first[0].push_constants,[4096,1,1,0,0,0,0,0,0,64,1,1]);
        let tail=plan(&r,1,[4099,1,1],[64,1,1]).unwrap();assert_eq!(tail.len(),2);
        assert_eq!(tail[1].local_size,[3,1,1]);assert_eq!(tail[1].group_count,[1,1,1]);
        assert_eq!(tail[1].push_constants,[4099,1,1,4096,0,0,64,0,0,65,1,1]);
        assert_eq!(tail[1].local_size_spec_ids,[0,1,2]);
    }
    #[test]
    fn every_three_dimensional_thread_exactly_once() {
        let r=reflection();
        for x in 0..10 {for y in 0..8 {for z in 0..6 {
            let regions=plan(&r,1,[x,y,z],[4,3,2]).unwrap();assert!(regions.len()<=8);
            let mut seen=vec![0u8;(x*y*z)as usize];
            for region in regions {
                assert_eq!(&region.push_constants[0..3],&[x,y,z]);
                for a in 0..region.local_size[0]*region.group_count[0] {
                    for b in 0..region.local_size[1]*region.group_count[1] {
                        for c in 0..region.local_size[2]*region.group_count[2] {
                            let px=region.thread_base[0]+a;let py=region.thread_base[1]+b;let pz=region.thread_base[2]+c;
                            assert!(px<x&&py<y&&pz<z);seen[((pz*y+py)*x+px)as usize]+=1;
                        }
                    }
                }
            }
            assert!(seen.iter().all(|n|*n==1));
        }}}
    }
    #[test]
    fn mode_stage_limits_overflow_and_fixed_grid() {
        let mut r=reflection();assert!(plan(&r,0,[1,1,1],[64,1,1]).is_err());
        assert!(plan(&r,1,[1,1,1],[0,1,1]).is_err());
        assert!(plan(&r,1,[u32::MAX;3],[1;3]).is_err());
        assert!(plan(&r,1,[1;3],[u32::MAX,2,1]).is_err());
        r.max_work_group_size=Some(32);assert!(plan(&r,1,[64,1,1],[64,1,1]).is_err());
        r.max_work_group_size=None;r.kernel_dispatch=Some(KernelDispatch::Workgroups);
        let p=plan(&r,0,[8,2,1],[64,1,1]).unwrap();assert_eq!(p[0].group_count,[8,2,1]);assert_eq!(p[0].push_constant_size,0);
        assert_eq!(p[0].local_size_spec_ids,[u32::MAX;3]);
        assert!(plan(&r,0,[8,2,1],[32,1,1]).is_err());
        assert!(plan(&r,0,[u32::MAX,1,1],[64,1,1]).is_err());
        assert!(plan(&r,1,[1;3],[1;3]).is_err());
        r.kernel_dispatch=Some(KernelDispatch::ThreadsFixed{threads_per_grid:[7,1,1]});
        assert!(plan(&r,1,[8,1,1],[4,1,1]).is_err());assert_eq!(plan(&r,1,[7,1,1],[4,1,1]).unwrap().len(),2);
        r.kernel_dispatch=None;r.stage=ShaderStage::Vertex;assert!(plan(&r,1,[1;3],[1;3]).is_err());
    }
}
