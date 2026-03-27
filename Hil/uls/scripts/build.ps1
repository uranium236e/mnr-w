param(
    [switch]$RunSmokeTest
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

$dllOutput = Join-Path $buildDir "hil_shell.dll"
$exeOutput = Join-Path $buildDir "hil_host.exe"

& $gxx @commonFlags `
    "-DHIL_SHELL_BUILD" `
    "-shared" `
    "-o" $dllOutput `
    (Join-Path $repoRoot "src\hil_shell.cpp") `
    "-L$libDir" `
    "-lglfw3" `
    "-lglew32" `
    "-lopengl32" `
    "-ldwmapi" `
    "-lshell32" `
    "-lgdi32" `
    "-luser32" `
    "-lkernel32"

if ($LASTEXITCODE -ne 0) {
    throw "DLL build failed."
}

& $gxx @commonFlags `
    "-o" $exeOutput `
    (Join-Path $repoRoot "app\main.cpp") `
    "-lkernel32" `
    "-luser32"

if ($LASTEXITCODE -ne 0) {
    throw "Host EXE build failed."
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
        Copy-Item -Force $source $buildDir
    }
}

if ($RunSmokeTest) {
    Push-Location $buildDir
    try {
        & $exeOutput --smoke-test
        if ($LASTEXITCODE -ne 0) {
            throw "Smoke test failed."
        }
    }
    finally {
        Pop-Location
    }
}
