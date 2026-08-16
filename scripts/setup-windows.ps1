<#
.SYNOPSIS
    Checks everything AZD Bitcoin Miner needs on Windows, and optionally
    installs what is missing.

.DESCRIPTION
    Run this FIRST on a fresh Windows machine. It reports exactly which
    prerequisites are present and which are not, and prints a ready-to-paste
    command for each gap.

    With -Install it runs those commands for you via winget.

    Nothing is installed without -Install. Visual Studio in particular is a
    multi-gigabyte download, so it is never pulled in silently.

.PARAMETER Install
    Actually install what is missing (via winget), instead of only reporting.

.PARAMETER Cuda
    Also check for the NVIDIA CUDA Toolkit. Only relevant on the machine with
    the GPU; the CUDA toolkit is never installed automatically.

.PARAMETER Python
    Also check for Python (only needed for scripts/mock_pool.py).

.EXAMPLE
    .\scripts\setup-windows.ps1

.EXAMPLE
    .\scripts\setup-windows.ps1 -Install

.EXAMPLE
    .\scripts\setup-windows.ps1 -Cuda -Python
#>

[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Cuda,
    [switch]$Python
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "windows-common.ps1")

$results = @()

function Add-Result {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Detail,
        [string]$FixCommand,
        [string]$FixUrl,
        [bool]$Required = $true
    )
    $script:results += [pscustomobject]@{
        Name       = $Name
        Ok         = $Ok
        Detail     = $Detail
        FixCommand = $FixCommand
        FixUrl     = $FixUrl
        Required   = $Required
    }
}

Write-Host ""
Write-Host "==============================================" -ForegroundColor DarkYellow
Write-Host " AZD Bitcoin Miner -- Windows setup check" -ForegroundColor Yellow
Write-Host "==============================================" -ForegroundColor DarkYellow
Write-Host ""

# --- PowerShell / winget --------------------------------------------------

Write-Host ("PowerShell version : {0}" -f $PSVersionTable.PSVersion)

$hasWinget = Test-CommandExists "winget"
if ($hasWinget) {
    Write-Host "winget             : available"
}
else {
    Write-Host "winget             : NOT available" -ForegroundColor Yellow
    Write-Host "                     (install 'App Installer' from the Microsoft Store," -ForegroundColor DarkGray
    Write-Host "                      or install each item manually from the links below)" -ForegroundColor DarkGray
}
Write-Host ""

# --- git ------------------------------------------------------------------

if (Test-CommandExists "git") {
    Add-Result -Name "Git" -Ok $true -Detail ((git --version) -join "")
}
else {
    Add-Result -Name "Git" -Ok $false -Detail "not found" `
        -FixCommand "winget install --id Git.Git -e" `
        -FixUrl "https://git-scm.com/download/win"
}

# --- Visual Studio with the C++ workload ----------------------------------

$vs = Find-VisualStudio
if ($vs) {
    Add-Result -Name "Visual Studio C++" -Ok $true -Detail $vs
}
elseif (Test-AnyVisualStudio) {
    Add-Result -Name "Visual Studio C++" -Ok $false `
        -Detail "Visual Studio is installed, but WITHOUT the C++ workload" `
        -FixCommand "winget install --id Microsoft.VisualStudio.2022.Community -e --override `"--quiet --wait --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended`"" `
        -FixUrl "Open 'Visual Studio Installer' -> Modify -> tick 'Desktop development with C++'"
}
else {
    Add-Result -Name "Visual Studio C++" -Ok $false -Detail "not found" `
        -FixCommand "winget install --id Microsoft.VisualStudio.2022.Community -e --override `"--quiet --wait --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended`"" `
        -FixUrl "https://visualstudio.microsoft.com/downloads/"
}

# --- CMake ----------------------------------------------------------------

$cmake = Find-CMakeExecutable
if ($cmake) {
    if ($cmake -eq "cmake") {
        $detail = "on PATH"
    }
    else {
        $detail = $cmake
    }
    Add-Result -Name "CMake" -Ok $true -Detail $detail
}
else {
    Add-Result -Name "CMake" -Ok $false `
        -Detail "not found (comes with the Visual Studio C++ workload)" `
        -FixCommand "winget install --id Kitware.CMake -e" `
        -FixUrl "https://cmake.org/download/"
}

# --- Optional: CUDA -------------------------------------------------------

if ($Cuda) {
    if (Test-CommandExists "nvcc") {
        Add-Result -Name "CUDA Toolkit" -Ok $true -Detail ((nvcc --version | Select-Object -Last 1) -join "") -Required $false
    }
    else {
        Add-Result -Name "CUDA Toolkit" -Ok $false -Required $false `
            -Detail "not found -- GPU backend cannot be built (CPU build is unaffected)" `
            -FixUrl "https://developer.nvidia.com/cuda-downloads  (install AFTER Visual Studio)"
    }
}

# --- Optional: Python -----------------------------------------------------

if ($Python) {
    if (Test-CommandExists "python") {
        Add-Result -Name "Python" -Ok $true -Detail ((python --version) -join "") -Required $false
    }
    else {
        Add-Result -Name "Python" -Ok $false -Required $false `
            -Detail "not found -- only needed for scripts/mock_pool.py" `
            -FixCommand "winget install --id Python.Python.3.12 -e" `
            -FixUrl "https://www.python.org/downloads/windows/"
    }
}

# --- Report ---------------------------------------------------------------

Write-Host "--- Prerequisites ---" -ForegroundColor DarkYellow
Write-Host ""

foreach ($item in $results) {
    if ($item.Ok) {
        $mark = "[ OK ]"
        $color = "Green"
    }
    elseif ($item.Required) {
        $mark = "[FAIL]"
        $color = "Red"
    }
    else {
        $mark = "[SKIP]"
        $color = "Yellow"
    }
    Write-Host ("{0} {1,-20} {2}" -f $mark, $item.Name, $item.Detail) -ForegroundColor $color
}
Write-Host ""

$missingRequired = @($results | Where-Object { -not $_.Ok -and $_.Required })
$missingOptional = @($results | Where-Object { -not $_.Ok -and -not $_.Required })

# --- Install --------------------------------------------------------------

if ($Install -and $missingRequired.Count -gt 0) {
    if (-not $hasWinget) {
        Write-Host "Cannot install automatically: winget is not available." -ForegroundColor Red
        Write-Host "Install the items manually using the links above." -ForegroundColor Red
        exit 1
    }

    Write-Host "--- Installing missing prerequisites ---" -ForegroundColor DarkYellow
    Write-Host "This can take a while; Visual Studio alone is several GB." -ForegroundColor DarkGray
    Write-Host ""

    foreach ($item in $missingRequired) {
        if (-not $item.FixCommand) {
            Write-Host ("Skipping {0}: no automatic installer, see {1}" -f $item.Name, $item.FixUrl) -ForegroundColor Yellow
            continue
        }
        Write-Host ("Installing {0} ..." -f $item.Name) -ForegroundColor Cyan
        Write-Host ("  {0}" -f $item.FixCommand) -ForegroundColor DarkGray
        # Run through cmd so the quoting of --override survives intact.
        & cmd /c $item.FixCommand
        Write-Host ""
    }

    Write-Host "==============================================" -ForegroundColor Green
    Write-Host " Installation finished." -ForegroundColor Green
    Write-Host "==============================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "IMPORTANT: close this window and open a NEW PowerShell," -ForegroundColor Yellow
    Write-Host "so the updated PATH takes effect. Then run:" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  .\scripts\setup-windows.ps1" -ForegroundColor White
    Write-Host ""
    exit 0
}

# --- Next steps -----------------------------------------------------------

if ($missingRequired.Count -eq 0) {
    Write-Host "==============================================" -ForegroundColor Green
    Write-Host " Everything required is present." -ForegroundColor Green
    Write-Host "==============================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next step -- build and test (CPU only, no GPU needed):" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  .\scripts\build-windows.ps1" -ForegroundColor White
    Write-Host ""

    if ($missingOptional.Count -gt 0) {
        Write-Host "Optional, not installed:" -ForegroundColor DarkGray
        foreach ($item in $missingOptional) {
            Write-Host ("  - {0}: {1}" -f $item.Name, $item.Detail) -ForegroundColor DarkGray
            if ($item.FixUrl) {
                Write-Host ("      {0}" -f $item.FixUrl) -ForegroundColor DarkGray
            }
        }
        Write-Host ""
    }
    exit 0
}

Write-Host "==============================================" -ForegroundColor Red
Write-Host (" {0} required item(s) missing." -f $missingRequired.Count) -ForegroundColor Red
Write-Host "==============================================" -ForegroundColor Red
Write-Host ""
Write-Host "Let this script install them for you:" -ForegroundColor Cyan
Write-Host ""
Write-Host "  .\scripts\setup-windows.ps1 -Install" -ForegroundColor White
Write-Host ""
Write-Host "Or install them yourself:" -ForegroundColor Cyan
Write-Host ""
foreach ($item in $missingRequired) {
    Write-Host ("  {0}:" -f $item.Name) -ForegroundColor White
    if ($item.FixCommand) {
        Write-Host ("    {0}" -f $item.FixCommand) -ForegroundColor DarkGray
    }
    if ($item.FixUrl) {
        Write-Host ("    {0}" -f $item.FixUrl) -ForegroundColor DarkGray
    }
    Write-Host ""
}
Write-Host "After installing, open a NEW PowerShell window (for the PATH) and" -ForegroundColor Yellow
Write-Host "run this script again." -ForegroundColor Yellow
Write-Host ""
exit 1
