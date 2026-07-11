param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Configuration = "Release",
    [string]$QtPrefix = "",
    [string]$CompilerRoot = "",
    [string]$Generator = "",
    [switch]$Clean,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectDir "build"

if (-not $QtPrefix) {
    $QMake = Get-Command qmake6 -ErrorAction SilentlyContinue
    if (-not $QMake) {
        $QMake = Get-Command qmake -ErrorAction SilentlyContinue
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

$CompilerArgs = @()
$QMakeSpec = ""
$QMakeSpecFile = Join-Path $QtPrefix "mkspecs\qconfig.pri"
if (Test-Path -LiteralPath $QMakeSpecFile) {
    $QMakeSpecMatch = Select-String -LiteralPath $QMakeSpecFile -Pattern '^QT_GCC_MAJOR_VERSION\s*=\s*(\d+)' | Select-Object -First 1
    if ($QMakeSpecMatch) {
        $RequiredGccMajor = [int]$QMakeSpecMatch.Matches[0].Groups[1].Value
    }
}

if ((Split-Path -Leaf $QtPrefix) -like "mingw*") {
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
    $env:PATH = "$CompilerBin;$QtBin;$env:PATH"
    $CompilerArgs = @(
        "-DCMAKE_CXX_COMPILER=$Compiler",
        "-DCMAKE_MAKE_PROGRAM=$MakeProgram"
    )
} else {
    if (-not $Generator) {
        $Generator = "Ninja"
    }
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

$ConfigureArgs = @(
    "-S", $ProjectDir,
    "-B", $BuildDir,
    "-G", $Generator,
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_PREFIX_PATH=$QtPrefix"
) + $CompilerArgs

& cmake @ConfigureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $BuildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir $BuildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Deploy) {
    $DeployTool = Join-Path $QtPrefix "bin\windeployqt.exe"
    $Executable = Join-Path $BuildDir "bin\BgpTester.exe"
    if (-not (Test-Path -LiteralPath $DeployTool)) {
        throw "windeployqt.exe was not found under the selected Qt kit."
    }
    & $DeployTool --no-translations --compiler-runtime $Executable
}
