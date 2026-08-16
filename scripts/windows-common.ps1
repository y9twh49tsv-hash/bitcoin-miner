# Shared helpers for the Windows scripts.
#
# Dot-sourced by setup-windows.ps1 and build-windows.ps1:
#     . (Join-Path $PSScriptRoot "windows-common.ps1")
#
# Written for Windows PowerShell 5.1 (the version shipped with Windows 11), so
# no ternary operator, no null-coalescing, no PowerShell 7 syntax.

function Test-CommandExists {
    param([Parameter(Mandatory = $true)][string]$Name)
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    return ($null -ne $found)
}

<#
.SYNOPSIS
    Path of the Visual Studio installation that has the C++ tools, or $null.
#>
function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }

    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null

    if ($installPath) { return ($installPath | Select-Object -First 1) }
    return $null
}

<#
.SYNOPSIS
    True when any Visual Studio is installed, even without the C++ workload.
    Used to tell "VS missing" apart from "VS present but no C++ tools".
#>
function Test-AnyVisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $any = & $vswhere -latest -products * -property installationPath 2>$null
    return ($null -ne $any -and $any -ne "")
}

<#
.SYNOPSIS
    Locates cmake.exe without requiring a Developer PowerShell.

.DESCRIPTION
    Checks, in order: PATH, the Visual Studio installation (VS ships CMake but
    only exposes it inside a Developer PowerShell), then the standard
    standalone install directories.

    Returns the path to cmake.exe, or $null when nothing was found.
#>
function Find-CMakeExecutable {
    if (Test-CommandExists "cmake") { return "cmake" }

    $vsPath = Find-VisualStudio
    if ($vsPath) {
        $bundled = Join-Path $vsPath `
            "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $bundled) { return $bundled }
    }

    $candidates = @(
        (Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "CMake\bin\cmake.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }

    return $null
}

<#
.SYNOPSIS
    ctest.exe sitting next to the given cmake, or plain "ctest" as a fallback.
#>
function Find-CTestExecutable {
    param([Parameter(Mandatory = $true)][string]$CMakePath)

    if ($CMakePath -eq "cmake") { return "ctest" }

    $candidate = Join-Path (Split-Path -Parent $CMakePath) "ctest.exe"
    if (Test-Path $candidate) { return $candidate }
    return "ctest"
}
