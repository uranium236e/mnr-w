param(
    [switch]$BuildDll
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$msysRoot = "C:\msys64\mingw64"
$binDir = Join-Path $msysRoot "bin"
$libDir = Join-Path $msysRoot "lib"
$includeDir = Join-Path $msysRoot "include"
$gxx = Join-Path $binDir "g++.exe"

if (-not (Test-Path $gxx)) {
    throw "Unable to find MSYS2 g++ at $gxx"
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

if ($BuildDll) {
    & powershell.exe -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build_someshitdll.ps1")
}

function Test-FileLocked([string]$Path) {
    if (-not (Test-Path $Path)) { return $false }
    try {
        $stream = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $stream.Close()
        return $false
    } catch {
        return $true
    }
}

$exeOutput = Join-Path $buildDir "someshit_demo.exe"
if (Test-FileLocked $exeOutput) {
    $exeOutput = Join-Path $buildDir "someshit_demo_run.exe"
}

$commonFlags = @(
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-I$repoRoot\include",
    "-I$includeDir"
)

& $gxx @commonFlags `
    "-o" $exeOutput `
    (Join-Path $repoRoot "demo\\someshit_demo.cpp") `
    "-L$buildDir" `
    "-lsomeshitdll" `
    "-L$libDir" `
    "-lglfw3" `
    "-lglew32" `
    "-lopengl32" `
    "-lgdi32" `
    "-luser32" `
    "-lkernel32"

if ($LASTEXITCODE -ne 0) {
    throw "Demo build failed."
}

$runtimeDlls = @(
    "glfw3.dll",
    "glew32.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)

foreach ($dll in $runtimeDlls) {
    $source = Join-Path $binDir $dll
    if (Test-Path $source) {
        try {
            Copy-Item -Force $source $buildDir
        } catch {
            # Ignore locked/active runtime DLLs.
        }
    }
}
