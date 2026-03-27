param()

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

$commonFlags = @(
    "-std=c++17",
    "-O2",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-I$repoRoot\include",
    "-I$includeDir",
    "-static-libstdc++",
    "-static-libgcc"
)

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

$dllOutput = Join-Path $buildDir "someshitdll.dll"
$implibOutput = Join-Path $buildDir "libsomeshitdll.dll.a"

if (Test-FileLocked $dllOutput) {
    $dllOutput = Join-Path $buildDir "someshitdll_new.dll"
    $implibOutput = Join-Path $buildDir "libsomeshitdll_new.dll.a"
}

& $gxx @commonFlags `
    "-DSOMESHITDLL_BUILD" `
    "-shared" `
    "-o" $dllOutput `
    "-Wl,--out-implib,$implibOutput" `
    (Join-Path $repoRoot "src\\someshitdll.cpp") `
    "-L$libDir" `
    "-ldwmapi" `
    "-lgdi32" `
    "-luser32" `
    "-lkernel32" `
    "-lole32" `
    "-luuid" `
    "-lwindowscodecs"

if ($LASTEXITCODE -ne 0) {
    throw "DLL build failed."
}
