<#
.SYNOPSIS
    Builds AZD Bitcoin Miner on Windows with Visual Studio.

.DESCRIPTION
    Without -Cuda this produces the portable CPU build, which needs nothing but
    Visual Studio C++ and CMake. With -Cuda it additionally compiles the GPU
    backend, which requires the NVIDIA CUDA Toolkit.

.PARAMETER Cuda
    Enable the CUDA GPU backend (requires the NVIDIA CUDA Toolkit).

.PARAMETER Nvml
    Also enable NVML telemetry (GPU temperature / power / utilization).
    Implies -Cuda. Without it, those values stay unavailable and the dashboard
    shows N/A -- they are never faked.

.PARAMETER Arch
    CUDA architecture(s) to build for. Default is "120" (Blackwell / RTX 50
    series, which covers the RTX 5070). Use "86" for RTX 30, "89" for RTX 40,
    or a list such as "86;89;120".

.PARAMETER Config
    Build configuration: Release (default) or Debug.

.PARAMETER Clean
    Delete the build directory first.

.PARAMETER SkipTests
    Do not run ctest after building.

.EXAMPLE
    .\scripts\build-windows.ps1

.EXAMPLE
    .\scripts\build-windows.ps1 -Cuda

.EXAMPLE
    .\scripts\build-windows.ps1 -Cuda -Nvml -Arch 120
#>

[CmdletBinding()]
param(
    [switch]$Cuda,
    [switch]$Nvml,
    [string]$Arch = "120",
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"

<#
.SYNOPSIS
    Locates cmake.exe without requiring a Developer PowerShell.

.DESCRIPTION
    Looks in three places, in order of preference:
      1. PATH (a standalone install, or a Developer PowerShell)
      2. The Visual Studio installation, found via vswhere.exe -- VS ships its
         own CMake but only exposes it inside a Developer PowerShell
      3. The standard standalone install directories

    Returns the path to cmake.exe, or $null when nothing was found.
#>
function Find-CMakeExecutable {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return "cmake" }

    # vswhere.exe is installed at a fixed location by every VS 2017+ installer.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($installPath) {
            $bundled = Join-Path $installPath `
                "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $bundled) { return $bundled }
        }
    }

    foreach ($candidate in @(
            (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
            (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe"))) {
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

Push-Location $ProjectRoot
try {
    Write-Host "==============================================" -ForegroundColor DarkYellow
    Write-Host " AZD Bitcoin Miner -- Windows build" -ForegroundColor Yellow
    Write-Host "==============================================" -ForegroundColor DarkYellow
    Write-Host ""

    # --- Prerequisites ----------------------------------------------------
    #
    # CMake ships inside Visual Studio but is not on PATH in a normal
    # PowerShell -- only in a "Developer PowerShell". Rather than making that a
    # precondition the user has to know about, locate it ourselves.
    $cmake = Find-CMakeExecutable
    if (-not $cmake) {
        throw @"
CMake was not found.

Checked: PATH, the Visual Studio installation (via vswhere) and the standard
standalone install locations.

Fix it with ONE of these:

  1. Install Visual Studio 2022 (Community is free) and tick the workload
     "Desktop development with C++". That includes both the compiler and CMake.
     https://visualstudio.microsoft.com/downloads/

  2. If Visual Studio IS installed, open "Developer PowerShell for VS 2022"
     from the Start menu and run this script there.

  3. Install CMake standalone and tick "Add CMake to the system PATH".
     https://cmake.org/download/
"@
    }

    # ctest lives next to cmake.
    $ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
    if (-not (Test-Path $ctest)) { $ctest = "ctest" }

    if ($cmake -ne "cmake") {
        Write-Host "Using CMake: $cmake" -ForegroundColor DarkGray
    }

    if ($Nvml) { $Cuda = $true }

    if ($Cuda) {
        if (-not (Get-Command nvcc -ErrorAction SilentlyContinue)) {
            throw @"
-Cuda was requested but nvcc was not found on PATH.

Install the NVIDIA CUDA Toolkit (https://developer.nvidia.com/cuda-downloads)
and re-open PowerShell, or run without -Cuda to build the CPU-only version.
"@
        }
        Write-Host "CUDA toolkit:" -ForegroundColor Green
        nvcc --version | Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" }
        Write-Host "CUDA architectures: $Arch"
    }
    else {
        Write-Host "Building WITHOUT CUDA." -ForegroundColor Cyan
        Write-Host "GPU hashrate, temperature, power and utilization will be reported as"
        Write-Host "unavailable (N/A). They are never simulated."
    }
    Write-Host ""

    if ($Clean -and (Test-Path $BuildDir)) {
        Write-Host "Removing $BuildDir"
        Remove-Item -Recurse -Force $BuildDir
    }

    # --- Configure --------------------------------------------------------
    $cmakeArgs = @("-S", ".", "-B", "build")

    if ($Cuda) {
        $cmakeArgs += "-DAZD_ENABLE_CUDA=ON"
        $cmakeArgs += "-DCMAKE_CUDA_ARCHITECTURES=$Arch"
        if ($Nvml) { $cmakeArgs += "-DAZD_ENABLE_NVML=ON" }
    }
    else {
        $cmakeArgs += "-DAZD_ENABLE_CUDA=OFF"
    }

    Write-Host "--- Configure ---" -ForegroundColor DarkYellow
    & $cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    # --- Build ------------------------------------------------------------
    Write-Host ""
    Write-Host "--- Build ($Config) ---" -ForegroundColor DarkYellow
    & $cmake --build build --config $Config --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }

    # --- Test -------------------------------------------------------------
    if (-not $SkipTests) {
        Write-Host ""
        Write-Host "--- Test ---" -ForegroundColor DarkYellow
        & $ctest --test-dir build --build-config $Config --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed." }
    }

    # --- Self-test --------------------------------------------------------
    $exe = Join-Path $BuildDir "bin\$Config\azd-miner.exe"
    if (-not (Test-Path $exe)) {
        $exe = Join-Path $BuildDir "bin\azd-miner.exe"
    }

    if (Test-Path $exe) {
        Write-Host ""
        Write-Host "--- Self-test (real hashing, real timings) ---" -ForegroundColor DarkYellow
        & $exe --selftest
        if ($LASTEXITCODE -ne 0) { throw "Self-test failed." }

        Write-Host ""
        Write-Host "--- Detected backends ---" -ForegroundColor DarkYellow
        & $exe --devices
    }
    else {
        Write-Warning "Could not locate azd-miner.exe under $BuildDir\bin"
    }

    Write-Host ""
    Write-Host "==============================================" -ForegroundColor Green
    Write-Host " Build OK" -ForegroundColor Green
    Write-Host "==============================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:"
    Write-Host "  1. copy config\config.example.json config\config.json"
    Write-Host "  2. edit config\config.json with YOUR pool and wallet"
    Write-Host "     (config.json is gitignored -- never commit credentials)"
    Write-Host "  3. $exe --config config\config.json"
    Write-Host "  4. open http://127.0.0.1:8080"
    if (-not $Cuda) {
        Write-Host ""
        Write-Host "To enable the GPU backend later:" -ForegroundColor Cyan
        Write-Host "  .\scripts\build-windows.ps1 -Cuda -Nvml"
    }
}
finally {
    Pop-Location
}
