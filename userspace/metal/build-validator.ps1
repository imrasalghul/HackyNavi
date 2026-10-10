param([ValidateSet('Windows', 'MacOS')][string]$Target = 'Windows')
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runtime = Join-Path $taskRoot '.tools/userspace'
$source = Join-Path $runtime 'src/SPIRV-Tools-2025.2'
$headers = Join-Path $runtime 'src/SPIRV-Headers-aa6cef192b8e693916eb713e7a9ccadf06062ceb'
$zig = Join-Path $taskRoot '.tools/zig-x86_64-windows-0.17.0/zig.exe'
$cmake = Join-Path $taskRoot '.tools/build-python/cmake/data/bin/cmake.exe'
$ninja = Join-Path $taskRoot '.tools/build-python/bin/ninja.exe'
$expectedArchive = '4a6147f9cb00ca33adedeb717c4b397480cd7f7f3128ac24dee8c24b6145ecf3'
$archive = Join-Path $runtime 'downloads/spirv-tools-v2025.2.tar.gz'
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedArchive) {
    throw 'SPIRV-Tools source archive hash does not match the pinned version.'
}
$headerArchive = Join-Path $runtime 'downloads/spirv-headers-aa6cef.tar.gz'
if ((Get-FileHash -LiteralPath $headerArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne '1bf0568bad1f9e30fce8d3199e54ae7ade1c16dbe913b5afd9b499149a9bb9a5') {
    throw 'SPIRV-Headers source archive hash does not match the pinned version.'
}
foreach ($required in @($source, $headers, $zig, $cmake, $ninja)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Required workspace tool/source is missing: $required" }
}
$ar = Join-Path $runtime 'zig-ar.cmd'
$ranlib = Join-Path $runtime 'zig-ranlib.cmd'
Set-Content -LiteralPath $ar -Value ('@"' + $zig + '" ar %*')
Set-Content -LiteralPath $ranlib -Value ('@"' + $zig + '" ar s %*')
$rules = Join-Path $runtime 'spirv-rules.cmake'
$settings = 'set(CMAKE_AR "' + $ar.Replace('\', '/') + '")' + "`n" +
            'set(CMAKE_RANLIB "' + $ranlib.Replace('\', '/') + '")'
Set-Content -LiteralPath $rules -Value $settings
$build = Join-Path $runtime $(if ($Target -eq 'MacOS') { 'build-spirv-tools-macos' } else { 'build-spirv-tools' })
$env:FORCED_BUILD_VERSION_DESCRIPTION = 'archive sha256=' + $expectedArchive
$options = @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$zig", '-DCMAKE_C_COMPILER_ARG1=cc',
    "-DCMAKE_CXX_COMPILER=$zig", '-DCMAKE_CXX_COMPILER_ARG1=c++',
    "-DCMAKE_USER_MAKE_RULES_OVERRIDE=$rules", "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DSPIRV-Headers_SOURCE_DIR=$headers", '-DSPIRV_SKIP_TESTS=ON',
    '-DSPIRV_SKIP_EXECUTABLES=OFF', '-DSPIRV_WERROR=OFF')
if ($Target -eq 'MacOS') {
    # Generic disables host-Windows platform flags; Zig emits real Mach-O and
    # supplies Darwin libc/C++ support. The executable is run on the Mac.
    $options += @('-DCMAKE_SYSTEM_NAME=Generic', '-DCMAKE_C_FLAGS=-target x86_64-macos.15.0',
                   '-DCMAKE_CXX_FLAGS=-target x86_64-macos.15.0')
}
$options = @($options | ForEach-Object { $_.Replace('\', '/') })
& $cmake @options
if ($LASTEXITCODE -ne 0) { throw 'Validator CMake configuration failed.' }
& $cmake --build $build --target spirv-val -j 4
if ($LASTEXITCODE -ne 0) { throw 'Validator build failed.' }
