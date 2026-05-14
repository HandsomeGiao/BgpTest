param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    $localCandidate = "C:\Users\giaogiao\AllMyLibFiles\vcpkg"
    if (Test-Path -LiteralPath $localCandidate) {
        $VcpkgRoot = $localCandidate
    }
}

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
    throw "VCPKG_ROOT is not set. Pass -VcpkgRoot <path> or set the VCPKG_ROOT environment variable."
}

$toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (!(Test-Path -LiteralPath $toolchain)) {
    throw "Could not find vcpkg toolchain file: $toolchain"
}

$sourceDir = $PSScriptRoot
if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $buildPath = $BuildDir
} else {
    $buildPath = Join-Path $sourceDir $BuildDir
}

cmake -S $sourceDir -B $buildPath -DCMAKE_TOOLCHAIN_FILE="$toolchain"

if (!$SkipBuild) {
    cmake --build $buildPath --config $Configuration
}

