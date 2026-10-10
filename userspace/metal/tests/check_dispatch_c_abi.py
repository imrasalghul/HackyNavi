#!/usr/bin/env python3
"""Exercise real compiled-result -> C dispatch plans; never opens a GPU."""
import ctypes as c
from pathlib import Path
import sys

library=c.CDLL(str(Path(sys.argv[1]).resolve()))
handle=c.c_void_p
triplet=c.c_uint32*3
class Region(c.Structure):
    _fields_=[(name,triplet) for name in ("local_size","group_count","thread_base","threadgroup_base","local_size_spec_ids")]+[
        ("push_constant_offset",c.c_uint32),("push_constant_size",c.c_uint32),("push_constants",c.c_uint32*12)]
library.hn_shader_compile.argtypes=[c.c_void_p,c.c_size_t,c.c_uint32,c.c_uint32,c.c_char_p]+[c.c_uint32]*5
library.hn_shader_compile.restype=handle
library.hn_shader_result_error.argtypes=[handle];library.hn_shader_result_error.restype=c.c_char_p
library.hn_shader_result_free.argtypes=[handle];library.hn_shader_result_free.restype=None
library.hn_shader_dispatch_abi_version.restype=c.c_uint32
library.hn_shader_plan_dispatch.argtypes=[handle,c.c_uint32,c.POINTER(c.c_uint32),c.POINTER(c.c_uint32)]
library.hn_shader_plan_dispatch.restype=handle
library.hn_shader_dispatch_plan_error.argtypes=[handle];library.hn_shader_dispatch_plan_error.restype=c.c_char_p
library.hn_shader_dispatch_plan_region_count.argtypes=[handle];library.hn_shader_dispatch_plan_region_count.restype=c.c_uint32
library.hn_shader_dispatch_plan_region.argtypes=[handle,c.c_uint32,c.POINTER(Region),c.c_size_t]
library.hn_shader_dispatch_plan_region.restype=c.c_int
library.hn_shader_dispatch_plan_free.argtypes=[handle];library.hn_shader_dispatch_plan_free.restype=None
assert c.sizeof(Region)==116 and library.hn_shader_dispatch_abi_version()==1
data=(Path(__file__).parent/"fixtures/apple/add_one.air").read_bytes()
memory=c.create_string_buffer(data)
shader=library.hn_shader_compile(memory,len(data),0,0,b"add_one",64,1,1,1,0)
assert shader and not library.hn_shader_result_error(shader),library.hn_shader_result_error(shader)
retained_plan=None
cases=0
try:
    for grid,local,mode,expected in [([4096,1,1],[64,1,1],1,1),([4099,1,1],[64,1,1],1,2),
            ([7,5,3],[4,3,2],1,8),([0,1,1],[64,1,1],1,0),([1,1,1],[0,1,1],1,None),
            ([1,1,1],[64,1,1],0,None),([1,1,1],[1,1,1],2,None),([0xffffffff]*3,[1]*3,1,None)]:
        plan=library.hn_shader_plan_dispatch(shader,mode,triplet(*grid),triplet(*local));assert plan
        try:
            count=library.hn_shader_dispatch_plan_region_count(plan)
            error=library.hn_shader_dispatch_plan_error(plan)
            assert bool(error)==(expected is None),error
            assert count==(expected or 0)
            unchanged=Region();c.memset(c.byref(unchanged),0xa5,c.sizeof(unchanged));before=bytes(unchanged)
            assert library.hn_shader_dispatch_plan_region(plan,count,c.byref(unchanged),c.sizeof(unchanged))==0
            assert bytes(unchanged)==before
            assert library.hn_shader_dispatch_plan_region(plan,0,c.byref(unchanged),c.sizeof(unchanged)-1)==0
            assert bytes(unchanged)==before
            for index in range(count):
                region=Region();assert library.hn_shader_dispatch_plan_region(plan,index,c.byref(region),c.sizeof(region))==1
                assert list(region.local_size_spec_ids)==[0,1,2]
                assert region.push_constant_offset==0 and region.push_constant_size==48
                assert list(region.push_constants[:3])==grid
                if grid[0]==4099 and index==1:
                    assert list(region.local_size)==[3,1,1]
                    assert list(region.push_constants)==[4099,1,1,4096,0,0,64,0,0,65,1,1]
            cases+=1
        finally:library.hn_shader_dispatch_plan_free(plan)
    retained_plan=library.hn_shader_plan_dispatch(shader,1,triplet(4099,1,1),triplet(64,1,1))
finally:library.hn_shader_result_free(shader)
try:
    assert not library.hn_shader_dispatch_plan_error(retained_plan)
    region=Region();assert library.hn_shader_dispatch_plan_region(retained_plan,1,c.byref(region),c.sizeof(region))==1
    assert list(region.thread_base)==[4096,0,0]
finally:library.hn_shader_dispatch_plan_free(retained_plan)
# Error handles stay error handles; nulls and failed compiler outputs cannot
# manufacture an executable plan or publish a partial result.
bad=library.hn_shader_compile(None,0,0,0,b"add_one",64,1,1,1,0)
for shader in (None,bad):
    plan=library.hn_shader_plan_dispatch(shader,1,triplet(1,1,1),triplet(1,1,1))
    assert plan and library.hn_shader_dispatch_plan_error(plan)
    assert library.hn_shader_dispatch_plan_region_count(plan)==0
    library.hn_shader_dispatch_plan_free(plan)
library.hn_shader_result_free(bad)
whole=library.hn_shader_compile(memory,len(data),0,0,b"add_one",64,1,1,0,0)
assert whole and not library.hn_shader_result_error(whole),library.hn_shader_result_error(whole)
try:
    for local,valid in ((64,True),(32,False)):
        plan=library.hn_shader_plan_dispatch(whole,0,triplet(7,1,1),triplet(local,1,1))
        try:
            assert bool(library.hn_shader_dispatch_plan_error(plan)) != valid
            assert library.hn_shader_dispatch_plan_region_count(plan)==int(valid)
            if valid:
                region=Region();assert library.hn_shader_dispatch_plan_region(plan,0,c.byref(region),c.sizeof(region))==1
                assert list(region.local_size_spec_ids)==[0xffffffff]*3
                assert region.push_constant_size==0 and list(region.push_constants)==[0]*12
        finally:library.hn_shader_dispatch_plan_free(plan)
        cases+=1
finally:library.hn_shader_result_free(whole)
assert library.hn_shader_dispatch_plan_region_count(None)==0
library.hn_shader_dispatch_plan_free(None)
print(f"Dispatch C ABI: {cases} actual-Apple-AIR plan cases, copied116B records, tail payload, failure preservation and independent lifetime passed; no GPU test")
