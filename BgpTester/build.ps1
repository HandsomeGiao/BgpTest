param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$QtPrefix = "",
    [string]$CompilerRoot = "",
    [string]$Generator = "",
    [switch]$Gui,
    [switch]$Clean,
    [switch]$Test,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectDir ("build\" + $Configuration)

# Deployment only applies to the explicitly requested Qt desktop target.
if ($Deploy -and -not $Gui) {
    throw "-Deploy requires -Gui because only the Qt desktop application needs deployment."
}
if ($QtPrefix -and -not $Gui) {
    throw "-QtPrefix is only used for GUI builds. Add -Gui to build the Qt application."
}

$VsWherePath = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$VisualStudioPath = ""
if (Test-Path -LiteralPath $VsWherePath) {
    $VisualStudioResult = & $VsWherePath -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($VisualStudioResult) {
        $VisualStudioPath = ($VisualStudioResult | Select-Object -First 1).Trim()
    }
}

$CMakeCommand = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -First 1
if ($CMakeCommand) {
    $CMakeExecutable = $CMakeCommand.Source
} elseif ($VisualStudioPath) {
    $CMakeExecutable = Join-Path $VisualStudioPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (-not (Test-Path -LiteralPath $CMakeExecutable)) {
        throw "CMake was not found on PATH or in the Visual Studio installation."
    }
} else {
    throw "CMake was not found. Install CMake or the Visual Studio C++ CMake tools."
}

$CompilerArgs = @()
$QtBin = ""
$UseMingw = $false
$RequiredGccMajor = $null

if ($Gui) {
    if (-not $QtPrefix) {
        $QMake = Get-Command qmake6 -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $QMake) {
            $QMake = Get-Command qmake -ErrorAction SilentlyContinue | Select-Object -First 1
        }
        if (-not $QMake) {
            throw "qmake6/qmake was not found. Add the selected Qt kit's bin directory to PATH or pass -QtPrefix."
        }
        $QtPrefix = & $QMake.Source -query QT_INSTALL_PREFIX
        if (-not $QtPrefix) {
            throw "Unable to determine QT_INSTALL_PREFIX from qmake."
        }
    }

    $QtPrefix = (Resolve-Path -LiteralPath $QtPrefix).Path
    $QtBin = Join-Path $QtPrefix "bin"
    $QMakeSpecFile = Join-Path $QtPrefix "mkspecs\qconfig.pri"
    if (Test-Path -LiteralPath $QMakeSpecFile) {
        $QMakeSpecMatch = Select-String -LiteralPath $QMakeSpecFile -Pattern '^QT_GCC_MAJOR_VERSION\s*=\s*(\d+)' | Select-Object -First 1
        if ($QMakeSpecMatch) {
            $RequiredGccMajor = [int]$QMakeSpecMatch.Matches[0].Groups[1].Value
        }
    }

    if ((Split-Path -Leaf $QtPrefix) -like "mingw*") {
        $UseMingw = $true
        if (-not $CompilerRoot) {
            $QtRoot = Split-Path -Parent (Split-Path -Parent $QtPrefix)
            $Candidates = Get-ChildItem -LiteralPath (Join-Path $QtRoot "Tools") -Directory -Filter "mingw*_64" -ErrorAction SilentlyContinue
            foreach ($Candidate in $Candidates) {
                $CandidateCompiler = Join-Path $Candidate.FullName "bin\g++.exe"
                if (-not (Test-Path -LiteralPath $CandidateCompiler)) {
                    continue
                }
                $CandidateMajor = [int](& $CandidateCompiler -dumpversion).Split('.')[0]
                if (-not $RequiredGccMajor -or $CandidateMajor -eq $RequiredGccMajor) {
                    $CompilerRoot = $Candidate.FullName
                    break
                }
            }
        }
        if (-not $CompilerRoot) {
            throw "No MinGW compiler matching this Qt kit was found. Pass -CompilerRoot C:\path\to\mingw*_64."
        }
    }
} elseif ($CompilerRoot) {
    $UseMingw = $true
}

if ($UseMingw) {
    $CompilerRoot = (Resolve-Path -LiteralPath $CompilerRoot).Path
    $CompilerBin = Join-Path $CompilerRoot "bin"
    $Compiler = Join-Path $CompilerBin "g++.exe"
    $MakeProgram = Join-Path $CompilerBin "mingw32-make.exe"
    if (-not (Test-Path -LiteralPath $Compiler) -or -not (Test-Path -LiteralPath $MakeProgram)) {
        throw "The selected MinGW compiler root is incomplete: $CompilerRoot"
    }
    if (-not $Generator) {
        $Generator = "MinGW Makefiles"
    }
    $CompilerArgs = @(
        "-DCMAKE_CXX_COMPILER=$Compiler",
        "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
    )
    $env:PATH = "$CompilerBin;$env:PATH"
} elseif (-not $Generator -and $VisualStudioPath) {
    # The Visual Studio generator discovers MSVC without requiring the caller
    # to launch this script from a Developer PowerShell session.
    $Generator = "Visual Studio 17 2022"
}

if ($QtBin) {
    $env:PATH = "$QtBin;$env:PATH"
}

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    $ResolvedProject = (Resolve-Path -LiteralPath $ProjectDir).Path
    $ResolvedBuild = (Resolve-Path -LiteralPath $BuildDir).Path
    if (-not $ResolvedBuild.StartsWith($ResolvedProject, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the project."
    }
    Remove-Item -LiteralPath $ResolvedBuild -Recurse -Force
}

$BuildTesting = if ($Test) { "ON" } else { "OFF" }
$BuildGui = if ($Gui) { "ON" } else { "OFF" }

$ConfigureArgs = @(
    "-S", $ProjectDir,
    "-B", $BuildDir,
    "-DBUILD_TESTING=$BuildTesting",
    "-DBGPTESTER_BUILD_GUI=$BuildGui"
)
if ($Generator) {
    $ConfigureArgs += @("-G", $Generator)
}
$MultiConfigGenerator = $Generator -match '^(Visual Studio|Xcode|Ninja Multi-Config)'
if (-not $MultiConfigGenerator) {
    $ConfigureArgs += "-DCMAKE_BUILD_TYPE=$Configuration"
}
if ($Gui) {
    $ConfigureArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix"
}
$ConfigureArgs += $CompilerArgs

& $CMakeExecutable @ConfigureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $CMakeExecutable --build $BuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    $CTestExecutable = Join-Path (Split-Path -Parent $CMakeExecutable) "ctest.exe"
    if (-not (Test-Path -LiteralPath $CTestExecutable)) {
        $CTestCommand = Get-Command ctest -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $CTestCommand) {
            throw "ctest was not found next to CMake or on PATH."
        }
        $CTestExecutable = $CTestCommand.Source
    }

    & $CTestExecutable --test-dir $BuildDir -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Deploy) {
    $DeployTool = Join-Path $QtPrefix "bin\windeployqt.exe"
    $ExecutableDirectory = Join-Path $BuildDir "bin"
    $Executable = Join-Path $ExecutableDirectory "BgpTester.exe"
    if (-not (Test-Path -LiteralPath $DeployTool)) {
        throw "windeployqt.exe was not found under the selected Qt kit."
    }
    if (-not (Test-Path -LiteralPath $Executable)) {
        throw "The expected GUI executable was not built: $Executable"
    }
    & $DeployTool --no-translations --compiler-runtime $Executable
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
