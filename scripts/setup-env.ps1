<#
.SYNOPSIS
    Instala y verifica las herramientas necesarias para compilar obs-network-device en Windows.
.DESCRIPTION
    Verifica git, CMake, Ninja y Visual Studio 2022 Build Tools (con soporte C++ y Windows SDK).
    Instala lo que falte mediante winget y muestra un resumen de verificacion.
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

function Refresh-SessionPath {
    $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $currentParts = @($env:Path -split ';')
    $extra = @(
        "$env:ProgramFiles\CMake\bin",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links",
        "$env:LOCALAPPDATA\Microsoft\WindowsApps"
    ) | Where-Object { Test-Path $_ }
    $combined = @($extra) + @($currentParts) + @($machinePath -split ';') + @($userPath -split ';') |
        Where-Object { $_ -and $_.Trim() -ne '' } |
        Select-Object -Unique
    $env:Path = $combined -join ';'
}

function Find-VsBuildTools {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($LASTEXITCODE -eq 0 -and $path) {
        return $path.Trim()
    }
    return $null
}

function Install-WingetPackage {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [string]$OverrideArgs = ""
    )
    Write-Host "Instalando $Id via winget..." -ForegroundColor Cyan
    $args = @(
        "install", "--id", $Id, "--exact",
        "--accept-package-agreements", "--accept-source-agreements"
    )
    if ($OverrideArgs -ne "") {
        $args += @("--override", $OverrideArgs)
    }
    & winget @args
    if ($LASTEXITCODE -ne 0) {
        throw "winget fallo instalando $Id (codigo de salida: $LASTEXITCODE)."
    }
}

Refresh-SessionPath

Write-Host "=== Verificando requisitos ===" -ForegroundColor Yellow

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "No se encontro 'winget'. Instala 'App Installer' desde Microsoft Store."
}

$hasGit = [bool](Get-Command git -ErrorAction SilentlyContinue)
$hasCmake = [bool](Get-Command cmake -ErrorAction SilentlyContinue)
$hasNinja = [bool](Get-Command ninja -ErrorAction SilentlyContinue)
$vsPath = Find-VsBuildTools

Write-Host ("git            : " + ($(if ($hasGit) { "OK" } else { "FALTA" })))
Write-Host ("cmake          : " + ($(if ($hasCmake) { "OK" } else { "FALTA" })))
Write-Host ("ninja          : " + ($(if ($hasNinja) { "OK" } else { "FALTA" })))
Write-Host ("VS 2022 C++    : " + ($(if ($vsPath) { "OK ($vsPath)" } else { "FALTA" })))

if (-not $CheckOnly) {
    if (-not $hasGit) {
        Install-WingetPackage -Id "Git.Git"
    }
    if (-not $hasCmake) {
        Install-WingetPackage -Id "Kitware.CMake"
    }
    if (-not $hasNinja) {
        Install-WingetPackage -Id "Ninja-build.Ninja"
    }
    if (-not $vsPath) {
        $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
        $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        if (-not $isAdmin) {
            Write-Host ""
            Write-Host "Visual Studio 2022 Build Tools (C++ y Windows SDK) requiere permisos de Administrador (UAC)." -ForegroundColor Red
            Write-Host "Abri una terminal de PowerShell como Administrador y ejecuta:" -ForegroundColor Yellow
            Write-Host "  cd `"$PSScriptRoot\..`"" -ForegroundColor White
            Write-Host "  .\scripts\setup-env.ps1" -ForegroundColor White
            exit 1
        }
        Write-Host "Instalando Visual Studio 2022 Build Tools (puede demorar varios minutos)..." -ForegroundColor Yellow
        Install-WingetPackage -Id "Microsoft.VisualStudio.2022.BuildTools" -OverrideArgs "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    }
    Refresh-SessionPath
    $vsPath = Find-VsBuildTools
}

Write-Host "`n=== Resumen de verificacion ===" -ForegroundColor Green

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    Write-Host ("CMake : " + (& $cmakeCmd.Source --version | Select-Object -First 1))
} else {
    Write-Warning "CMake sigue sin encontrarse en PATH."
}

$ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
if ($ninjaCmd) {
    Write-Host ("Ninja : " + (& $ninjaCmd.Source --version | Select-Object -First 1))
} else {
    Write-Warning "Ninja sigue sin encontrarse en PATH."
}

$gitCmd = Get-Command git -ErrorAction SilentlyContinue
if ($gitCmd) {
    Write-Host ("Git   : " + (& $gitCmd.Source --version | Select-Object -First 1))
} else {
    Write-Warning "Git sigue sin encontrarse en PATH."
}

if ($vsPath) {
    Write-Host ("VS C++: $vsPath")
    $msvcRoot = Join-Path $vsPath "VC\Tools\MSVC"
    if (Test-Path $msvcRoot) {
        $vers = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($vers) {
            Write-Host ("MSVC  : $($vers.Name)")
        }
    }
} else {
    Write-Warning "Visual Studio 2022 Build Tools con soporte C++ no detectado."
}

$sdkInclude = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
if (Test-Path $sdkInclude) {
    $sdks = Get-ChildItem $sdkInclude -Directory | Sort-Object Name -Descending | Select-Object -First 1
    if ($sdks) {
        Write-Host ("WinSDK: $($sdks.Name)")
    }
} else {
    Write-Warning "Windows 10/11 SDK no detectado."
}

if (-not $cmakeCmd -or -not $vsPath) {
    exit 1
}
