<#
.SYNOPSIS
    Descarga dependencias de OBS/Qt6 via CMake, compila obs-network-device, ejecuta tests y opcionalmente instala el plugin.
.PARAMETER Config
    Configuracion de compilacion (RelWithDebInfo, Release, Debug). Por defecto: RelWithDebInfo.
.PARAMETER Install
    Si se especifica, instala el plugin compilado en OBS Studio al finalizar.
.PARAMETER SkipTests
    Si se especifica, omite la ejecucion de ctest.
#>
[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$Config = "RelWithDebInfo",
    [switch]$Install,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
Push-Location $RepoRoot

try {
    function Refresh-Path {
        $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $extra = @(
            "$env:ProgramFiles\CMake\bin",
            "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer",
            "$env:LOCALAPPDATA\Microsoft\WinGet\Links"
        ) | Where-Object { Test-Path $_ }
        $wingetPkgs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages"
        if (Test-Path $wingetPkgs) {
            $cmakeDirs = Get-ChildItem $wingetPkgs -Directory -Filter "*CMake*" -ErrorAction SilentlyContinue |
                ForEach-Object { Get-ChildItem $_.FullName -Directory -Recurse -Filter "bin" -ErrorAction SilentlyContinue } |
                Select-Object -ExpandProperty FullName
            $extra += @($cmakeDirs)
        }
        $combined = @($extra) + @($env:Path -split ';') + @($machinePath -split ';') + @($userPath -split ';') |
            Where-Object { $_ -and $_.Trim() -ne '' } |
            Select-Object -Unique
        $env:Path = $combined -join ';'
    }

    Refresh-Path

    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "No se encontro 'cmake'. Ejecuta primero .\scripts\setup-env.ps1."
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = $null
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    }
    if (-not $vsPath) {
        throw "No se encontro Visual Studio 2022 Build Tools (C++). Abri PowerShell como Administrador y ejecuta: .\scripts\setup-env.ps1"
    }

    Write-Host "=== Configurando proyecto (preset windows-x64) ===" -ForegroundColor Cyan
    Write-Host "Nota: la primera ejecucion descarga obs-deps, Qt6 y obs-studio 32.0.4 en .deps/ y compila libobs." -ForegroundColor DarkGray
    & $cmake.Source --preset windows-x64
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --preset windows-x64 fallo (codigo $LASTEXITCODE)."
    }

    Write-Host "`n=== Compilando obs-network-device ($Config) ===" -ForegroundColor Cyan
    & $cmake.Source --build build_x64 --config $Config --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --build fallo (codigo $LASTEXITCODE)."
    }

    if (-not $SkipTests) {
        Write-Host "`n=== Ejecutando tests unitarios ===" -ForegroundColor Cyan
        $ctest = Join-Path (Split-Path $cmake.Source -Parent) "ctest.exe"
        if (-not (Test-Path $ctest)) { $ctest = "ctest" }
        & $ctest --test-dir build_x64 -C $Config --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Algunos tests unitarios fallaron (codigo $LASTEXITCODE)."
        }
    }

    if ($Install) {
        Write-Host "`n=== Instalando plugin en OBS Studio ===" -ForegroundColor Cyan
        & (Join-Path $PSScriptRoot "install.ps1") -Config $Config
    } else {
        Write-Host "`nCompilacion exitosa. Para instalar en OBS ejecuta: .\scripts\install.ps1 -Config $Config" -ForegroundColor Green
    }
} finally {
    Pop-Location
}
