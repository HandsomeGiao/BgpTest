param(
    [string]$BuildDir = "build",
    [string]$Configuration = "Release",
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$SkipBuild,
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
$buildSucceeded = $false

try {
    if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        $vcpkgCommand = Get-Command vcpkg -ErrorAction SilentlyContinue
        if ($vcpkgCommand -and $vcpkgCommand.Source) {
            $candidate = Split-Path -Parent $vcpkgCommand.Source
            if (Test-Path -LiteralPath (Join-Path $candidate "scripts\buildsystems\vcpkg.cmake")) {
                $VcpkgRoot = $candidate
            }
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

    $buildSucceeded = $true
} catch {
    Write-Host ""
    Write-Host "[FAILED] $($_.Exception.Message)" -ForegroundColor Red
    throw
} finally {
    if (!$NoPause) {
        Write-Host ""
        if ($buildSucceeded) {
            Write-Host "[OK] build.ps1 finished." -ForegroundColor Green
        } else {
            Write-Host "[FAILED] build.ps1 stopped before completion." -ForegroundColor Red
        }
        Read-Host "Press Enter to exit"
    }
}
