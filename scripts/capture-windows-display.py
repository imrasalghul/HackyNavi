#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read active Windows display paths and raw EDID; never change display state.

Only calls GetDisplayConfigBufferSizes, QueryDisplayConfig,
DisplayConfigGetDeviceInfo and WMI's read-only EDID block method.
No claim about negotiated FRL/DP link rate or active DSC is made.
"""
import argparse
import ctypes as c
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys

U32=c.c_uint32
class Luid(c.Structure):
    _fields_=[('low',U32),('high',c.c_int32)]
class Rational(c.Structure):
    _fields_=[('numerator',U32),('denominator',U32)]
class Region(c.Structure):
    _fields_=[('cx',U32),('cy',U32)]
class Signal(c.Structure):
    _fields_=[('pixelRate',c.c_uint64),('hSyncFreq',Rational),('vSyncFreq',Rational),
              ('activeSize',Region),('totalSize',Region),('additionalSignalInfo',U32),('scanLineOrdering',U32)]
class ModeUnion(c.Union):
    _fields_=[('signal',Signal),('opaque',c.c_byte*48)]
class Mode(c.Structure):
    _fields_=[('infoType',U32),('id',U32),('adapterId',Luid),('data',ModeUnion)]
class Source(c.Structure):
    _fields_=[('adapterId',Luid),('id',U32),('modeInfoIdx',U32),('statusFlags',U32)]
class Target(c.Structure):
    _fields_=[('adapterId',Luid),('id',U32),('modeInfoIdx',U32),('outputTechnology',U32),
              ('rotation',U32),('scaling',U32),('refreshRate',Rational),('scanLineOrdering',U32),
              ('targetAvailable',c.c_int32),('statusFlags',U32)]
class DisplayPath(c.Structure):
    _fields_=[('source',Source),('target',Target),('flags',U32)]
class Header(c.Structure):
    _fields_=[('type',U32),('size',U32),('adapterId',Luid),('id',U32)]
class TargetName(c.Structure):
    _fields_=[('header',Header),('flags',U32),('outputTechnology',U32),('edidManufactureId',c.c_uint16),
              ('edidProductCodeId',c.c_uint16),('connectorInstance',U32),
              ('monitorFriendlyDeviceName',c.c_wchar*64),('monitorDevicePath',c.c_wchar*128)]
class Color(c.Structure):
    _fields_=[('header',Header),('value',U32),('colorEncoding',U32),('bitsPerColorChannel',U32)]

def rational(value):
    return {'numerator':value.numerator,'denominator':value.denominator,
            'hz':value.numerator/value.denominator if value.denominator else None}

def query_paths():
    assert c.sizeof(Signal)==48 and c.sizeof(Mode)==64 and c.sizeof(DisplayPath)==72
    assert c.sizeof(TargetName)==420 and c.sizeof(Color)==32
    api=c.WinDLL('user32',use_last_error=True)
    api.GetDisplayConfigBufferSizes.argtypes=[U32,c.POINTER(U32),c.POINTER(U32)]
    api.GetDisplayConfigBufferSizes.restype=c.c_int32
    api.QueryDisplayConfig.argtypes=[U32,c.POINTER(U32),c.POINTER(DisplayPath),c.POINTER(U32),c.POINTER(Mode),c.c_void_p]
    api.QueryDisplayConfig.restype=c.c_int32
    api.DisplayConfigGetDeviceInfo.argtypes=[c.POINTER(Header)]
    api.DisplayConfigGetDeviceInfo.restype=c.c_int32
    # No virtual-mode flag: plain modeInfoIdx union interpretation is required.
    for attempt in range(4):
        np,nm=U32(),U32()
        result=api.GetDisplayConfigBufferSizes(2,c.byref(np),c.byref(nm))
        if result:raise OSError(result,'GetDisplayConfigBufferSizes')
        if not (0<np.value<=256 and 0<nm.value<=1024):raise ValueError('Unexpected path/mode counts')
        paths,modes=(DisplayPath*np.value)(),(Mode*nm.value)()
        result=api.QueryDisplayConfig(2,c.byref(np),paths,c.byref(nm),modes,None)
        if result==122:continue
        if result:raise OSError(result,'QueryDisplayConfig')
        break
    else:raise RuntimeError('Display topology kept changing')
    output=[]
    for path in paths[:np.value]:
        target=path.target
        item={'target_id':target.id,'adapter_luid':{'high':target.adapterId.high,'low':target.adapterId.low},
              'output_technology':target.outputTechnology,'output_technology_name':{5:'HDMI',10:'DisplayPort external'}.get(target.outputTechnology,'Other'),
              'target_available':bool(target.targetAvailable),'path_flags':path.flags,
              'path_refresh':rational(target.refreshRate),'rotation':target.rotation,'scaling':target.scaling}
        name=TargetName();name.header=Header(2,c.sizeof(name),target.adapterId,target.id)
        result=api.DisplayConfigGetDeviceInfo(c.byref(name.header));item['target_name_status']=result
        if not result:item.update(friendly_name=name.monitorFriendlyDeviceName,device_path=name.monitorDevicePath,
                                  edid_product_code=name.edidProductCodeId,edid_manufacturer_code=name.edidManufactureId)
        color=Color();color.header=Header(9,c.sizeof(color),target.adapterId,target.id)
        result=api.DisplayConfigGetDeviceInfo(c.byref(color.header));item['advanced_color_status']=result
        if not result:item['advanced_color']={'raw_flags':color.value,'encoding':color.colorEncoding,
            'encoding_name':{0:'RGB',1:'YCbCr444',2:'YCbCr422',3:'YCbCr420',4:'Intensity'}.get(color.colorEncoding,'Unknown'),
            'bits_per_channel':color.bitsPerColorChannel}
        index=target.modeInfoIdx
        if index>=nm.value:raise ValueError('Active target lacks a valid mode index')
        mode=modes[index]
        if mode.infoType!=2 or mode.id!=target.id or bytes(mode.adapterId)!=bytes(target.adapterId):
            raise ValueError('Target mode identity mismatch')
        s=mode.data.signal
        item['signal']={'pixel_clock_hz':s.pixelRate,'active_width':s.activeSize.cx,'active_height':s.activeSize.cy,
            'total_width':s.totalSize.cx,'total_height':s.totalSize.cy,'hsync':rational(s.hSyncFreq),
            'vsync':rational(s.vSyncFreq),'scanline_ordering':s.scanLineOrdering,
            'additional_signal_info':s.additionalSignalInfo}
        output.append(item)
    return output

def query_edid():
    script=r'''
$ErrorActionPreference='Stop'
$results=@()
foreach($monitor in @(Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorDescriptorMethods | Where-Object Active)) {
    $blocks=@(); $count=1
    for($index=0;$index -lt $count;$index++) {
        $raw=Invoke-CimMethod -InputObject $monitor -MethodName WmiGetMonitorRawEEdidV1Block -Arguments @{BlockId=[byte]$index}
        # Current Windows provider declares Boolean success; older documented
        # uint32 providers use zero. Keep the actual return value in evidence.
        $succeeded=if($raw.ReturnValue -is [bool]){$raw.ReturnValue}else{$null -ne $raw.ReturnValue -and $raw.ReturnValue -eq 0}
        if(-not $succeeded -or $raw.BlockContent.Count -ne 128){throw 'Raw EDID method failed'}
        $blocks+=,[pscustomobject]@{ index=$index;block_type=$raw.BlockType;return_value=$raw.ReturnValue;bytes=@($raw.BlockContent) }
        if($index -eq 0){$count=[int]$raw.BlockContent[126]+1}
        if($index -eq 1 -and $raw.BlockContent[0] -eq 2 -and $raw.BlockContent[1] -ge 3 -and
            $raw.BlockContent[2] -ge 7 -and $raw.BlockContent[2] -le 127 -and
            $raw.BlockContent[4] -eq 226 -and $raw.BlockContent[5] -eq 120) {
            $count=[int]$raw.BlockContent[6]+1
        }
        # Always read the first extension before applying the bounded count,
        # because a valid HF-EEODB can override the base count.
        if(($index -ge 1 -or $count -eq 1) -and ($count -lt 1 -or $count -gt 8)){throw 'EDID exceeds bounded eight-block capture'}
    }
    $conn=Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorConnectionParams | Where-Object InstanceName -eq $monitor.InstanceName
    $results+=,[pscustomobject]@{ instance=$monitor.InstanceName;active=$monitor.Active;output_technology=$conn.VideoOutputTechnology;blocks=$blocks }
}
ConvertTo-Json -InputObject $results -Depth 8 -Compress
'''
    result=subprocess.run(['powershell','-NoProfile','-NonInteractive','-Command',script],capture_output=True,text=True,timeout=45)
    if result.returncode:raise RuntimeError(result.stderr)
    return json.loads(result.stdout)

def main():
    if sys.platform!='win32':raise RuntimeError('This read-only collector requires Windows')
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[1]/'build/monitor-evidence')
    args=parser.parse_args();start=datetime.datetime.now(datetime.timezone.utc)
    out=args.output/start.strftime('%Y%m%dT%H%M%S%fZ');out.mkdir(parents=True,exist_ok=False)
    report={'started_utc':start.isoformat(),'source':'QueryDisplayConfig QDC_ONLY_ACTIVE_PATHS and WmiGetMonitorRawEEdidV1Block',
            'scope':'Read-only Windows-driver reported timing and EDID, not negotiated FRL/DSC or macOS evidence',
            'collector_sha256':hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest(),
            'paths':query_paths(),'edid':query_edid()}
    for index,monitor in enumerate(report['edid']):
        raw=b''.join(bytes(block['bytes']) for block in monitor['blocks'])
        monitor['block_checksums']=[sum(raw[n:n+128])%256 for n in range(0,len(raw),128)]
        monitor['sha256']=hashlib.sha256(raw).hexdigest()
        monitor['file']=f'active-monitor-{index}.edid';(out/monitor['file']).write_bytes(raw)
    report['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    (out/'capture.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'directory':str(out),'paths':report['paths'],'edid':[{'instance':m['instance'],'bytes':len(m['blocks'])*128,'sha256':m['sha256'],'checksums':m['block_checksums']} for m in report['edid']]},indent=2))
if __name__=='__main__':main()
