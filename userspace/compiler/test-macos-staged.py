import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_.@-]+', args.host) or args.host.startswith('-'):
        raise ValueError('Expected SSH host alias or user@hostname')
    build_report = json.loads((ROOT / 'build/gfx11-compiler/darwin-build-report.json').read_text())
    for name, expected in build_report['adapter_sources'].items():
        if hashlib.sha256((HERE / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Stale Darwin compiler source: ' + name)
    baseline = Path(json.loads((ROOT / 'build/gfx11-compiler/test-report.json').read_text())['artifact_directory'])
    files = {
        'hackynavi-aco': Path(build_report['binary']),
        'libhackynavi_gfx11.dylib': Path(build_report['library']),
        'spirv-val': ROOT / '.tools/userspace/build-spirv-tools-macos/tools/spirv-val',
        'compile.py': HERE / 'compile.py', 'check_c_abi.py': HERE / 'check_c_abi.py',
        'input.spv': ROOT / 'build/metal-compiler/apple-input/translated/shader.spv',
        'expected/shader.bin': baseline / 'shader.bin', 'expected/shader.json': baseline / 'shader.json',
    }
    data = {name: path.read_bytes() for name, path in files.items()}
    for name, key in [('hackynavi-aco', 'binary_sha256'), ('libhackynavi_gfx11.dylib', 'library_sha256')]:
        if hashlib.sha256(data[name]).hexdigest() != build_report[key]:
            raise RuntimeError('Darwin compiler binary hash mismatch')
    expected = {name: {'bytes': len(value), 'sha256': hashlib.sha256(value).hexdigest()} for name, value in data.items()}
    output = ROOT / 'build/gfx11-compiler/macos-native'
    output.mkdir(parents=True, exist_ok=True)
    archive_path = output / 'inputs.tar.gz'
    with tarfile.open(archive_path, 'w:gz') as archive:
        for name, value in data.items():
            info = tarfile.TarInfo(name)
            info.size = len(value)
            archive.addfile(info, io.BytesIO(value))
    script = '''import hashlib,json,pathlib,shutil,subprocess,sys,tarfile,tempfile
expected=json.loads(MANIFEST)
report={"commands":[],"cleanup":False,"inputs":expected,"hardware_executed":False}
def run(argv):
    result=subprocess.run(argv,capture_output=True,text=True,timeout=120)
    report["commands"].append({"argv":argv,"exit_code":result.returncode,"stdout":result.stdout,"stderr":result.stderr})
    return result.returncode==0
cache=pathlib.Path.home()/"Library/Caches"
report["free_before"]=shutil.disk_usage(cache).free
if report["free_before"] < 1024**3:
    raise RuntimeError("less than 1 GiB free; no staging created")
with tempfile.TemporaryDirectory(prefix="HackyNavi-GFX11-",dir=cache) as name:
    stage=pathlib.Path(name);report["staging"]=name
    with tarfile.open(fileobj=sys.stdin.buffer,mode="r|gz") as archive:
        seen=set()
        for member in archive:
            if member.name not in expected or member.name in seen or not member.isfile():
                raise RuntimeError("unexpected archive member")
            seen.add(member.name);value=archive.extractfile(member).read()
            if len(value)!=expected[member.name]["bytes"] or hashlib.sha256(value).hexdigest()!=expected[member.name]["sha256"]:
                raise RuntimeError("archive integrity mismatch")
            path=stage/member.name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(value)
            path.chmod(0o755 if member.name in ["hackynavi-aco","spirv-val"] else 0o644)
        if seen != set(expected): raise RuntimeError("missing archive member")
    run(["sw_vers"])
    run(["uname","-m"])
    run(["xcrun","lipo",str(stage/"hackynavi-aco"),"-verify_arch","x86_64"])
    run(["xcrun","lipo",str(stage/"libhackynavi_gfx11.dylib"),"-verify_arch","x86_64"])
    run(["xcrun","otool","-L",str(stage/"libhackynavi_gfx11.dylib")])
    cli=run(["/usr/bin/python3",str(stage/"compile.py"),str(stage/"input.spv"),str(stage/"cli"),
             "--compiler",str(stage/"hackynavi-aco"),"--validator",str(stage/"spirv-val")])
    if cli:
        report["cli_code_matches_windows"]=(stage/"cli/shader.bin").read_bytes()==(stage/"expected/shader.bin").read_bytes()
        report["cli_metadata_matches_windows"]=json.loads((stage/"cli/shader.json").read_text())==json.loads((stage/"expected/shader.json").read_text())
        report["native_code_sha256"]=hashlib.sha256((stage/"cli/shader.bin").read_bytes()).hexdigest()
        report["native_metadata"]=json.loads((stage/"cli/shader.json").read_text())
    run(["/usr/bin/python3",str(stage/"check_c_abi.py"),str(stage/"libhackynavi_gfx11.dylib"),
         str(stage/"spirv-val"),str(stage/"input.spv"),str(stage/"expected")])
    report["peak_stage_bytes"]=sum(p.stat().st_size for p in stage.rglob("*") if p.is_file())
report["cleanup"]=not pathlib.Path(report["staging"]).exists()
report["free_after"]=shutil.disk_usage(cache).free
report["success"]=report["cleanup"] and report.get("cli_code_matches_windows",False) and report.get("cli_metadata_matches_windows",False) and all(item["exit_code"]==0 for item in report["commands"])
print(json.dumps(report))
'''.replace('MANIFEST', repr(json.dumps(expected)))
    print(f'Staging {sum(len(value) for value in data.values()):,} bytes; no installs or GPU submission', flush=True)
    with archive_path.open('rb') as payload:
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', args.host,
                                 '/usr/bin/python3 -c ' + shlex.quote(script)], stdin=payload,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=600)
    if result.returncode:
        (output / 'transport-error.txt').write_bytes(result.stderr)
        raise RuntimeError(result.stderr.decode(errors='replace'))
    report = json.loads(result.stdout)
    report['archive_bytes'] = archive_path.stat().st_size
    report['archive_sha256'] = hashlib.sha256(archive_path.read_bytes()).hexdigest()
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ['success', 'cleanup', 'peak_stage_bytes', 'commands']}, indent=2))
    return 0 if report['success'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
