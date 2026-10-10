#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline geometry/checker audit only: never renders an image or calls Metal.

Compare the target smoke's sample classification with independent integer edge
cross-products from the actual retained Apple shader constants. No backend,
shader substitution, pixel-producing renderer or GPU result is implemented.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

ROOT=Path(__file__).resolve().parents[4]
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def constants(path):
    raw=path.read_bytes();words=struct.unpack('<'+'I'*(len(raw)//4),raw)
    assert words[0]==0x07230203
    at=5;float_types=set();values={};stores=[]
    while at<len(words):
        count=words[at]>>16;op=words[at]&65535
        assert count and at+count<=len(words)
        args=words[at+1:at+count]
        if op==22 and args[1]==32:float_types.add(args[0]) # OpTypeFloat
        if op==43 and args[0] in float_types:
            values[args[1]]=struct.unpack('<f',struct.pack('<I',args[2]))[0]
        if op==44 and all(x in values for x in args[2:]):
            values[args[1]]=[values[x] for x in args[2:]]
        if op==62:stores.append(args)
        at+=count
    return values,stores
def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path,required=True)
    out=parser.parse_args().output.resolve()
    if out.exists():parser.error('Use a fresh evidence directory')
    source=ROOT/'userspace/metal/native/render-smoke.mm'
    vertex=ROOT/'build/apple-graphics-integration-1/air-vertex/shader.spv'
    fragment=ROOT/'build/apple-graphics-integration-1/air-fragment/shader.spv'
    assert sha(vertex)=='6f37a517189a5f7fbca1127a323df021052664e44ba67962ab84590adf0ec3fb'
    prior=json.loads((ROOT/'build/apple-graphics-integration-1/report.json').read_text())
    assert sha(fragment)==prior['products'][1]['spirv_sha256']
    text=source.read_text()
    assert 'Side = 64' in text and 'int(2*y+1)-2*std::abs(int(2*x+1)-int(Side))' in text
    vv,_=constants(vertex);fv,stores=constants(fragment)
    points=[[-1.0,-1.0,0.0,1.0],[1.0,-1.0,0.0,1.0],[0.0,1.0,0.0,1.0]]
    assert all(p in vv.values() for p in points)
    assert len(stores)==1
    rgba=fv[stores[0][1]];assert rgba==[0.25,0.5,0.75,1.0]
    expected=[round(v*255) for v in rgba];assert expected==[64,128,191,255]
    # Metal transform: clip +Y maps toward the top. Doubled integer window
    # coordinates preserve exact sample centers and avoid floating rounding.
    points2=[(int((p[0]/p[3]+1)*64),int((1-p[1]/p[3])*64)) for p in points]
    assert points2==[(0,128),(128,128),(64,0)]
    def cross(a,b,p):return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])
    def reference(x,y):
        p=(2*x+1,2*y+1)
        sides=[cross(points2[i],points2[(i+1)%3],p) for i in range(3)]
        return all(v>=0 for v in sides) or all(v<=0 for v in sides)
    counts={'interior':0,'exterior':0,'edge_band':0};mirrored_mismatches=0;checks=0
    for y in range(64):
        for x in range(64):
            edge=2*y+1-2*abs(2*x+1-64)
            assert (edge>0)==reference(x,y);checks+=1
            if abs(edge)<=4:counts['edge_band']+=1;continue
            counts['interior' if reference(x,y) else 'exterior']+=1
            if reference(x,63-y)!=reference(x,y):mirrored_mismatches+=1
    assert counts=={'interior':1922,'exterior':1922,'edge_band':252}
    probes={(8,8):False,(8,56):True,(55,8):False,(55,56):True}
    for (x,y),inside in probes.items():
        assert reference(x,y)==inside and reference(x,63-y)!=inside;checks+=1
    # Actual texture_region planRead copies64 rows of256 bytes at host pitch272.
    # These numeric offsets must agree with the smoke's own guard classification.
    written={16+y*272+b for y in range(64) for b in range(256)}
    for i in range(16+64*272+16):
        marked=i>=16 and i<16+64*272 and (i-16)%272<256
        assert marked==(i in written);checks+=1
    out.mkdir(parents=True)
    report={'success':True,'checks':checks,'scope':'Offline independent geometry and host byte-span audit only; not a render/GPU outcome',
        'actual_vertex_constants':points,'metal_window_vertices':[[a/2,b/2] for a,b in points2],
        'actual_fragment_color':rgba,'expected_rgba8':expected,'classified_pixels':counts,
        'mirrored_render_would_fail_checked_pixels':mirrored_mismatches,
        'host_pixel_bytes':len(written),'host_guard_bytes':16+64*272+16-len(written),
        'files':{p.relative_to(ROOT).as_posix():sha(p) for p in (source,vertex,fragment,Path(__file__).resolve(),ROOT/'userspace/runtime/texture_region.cpp')},
        'gpu_executed':False,'metal_objects_created':False,'image_generated':False}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'sha256':sha(out/'report.json'),'checks':checks,'counts':counts,'mirror_mismatches':mirrored_mismatches}))
if __name__=='__main__':main()
