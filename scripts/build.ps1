<#
.SYNOPSIS
    Descarga dependencias de OBS/Qt6 via CMake, compila obs-network-device, ejecuta tests y opcionalmente instala el plugin.
.PARAMETER Config
    Configuracion de compilacion (RelWithDebInfo, Release, Debug). Por defecto: RelWithDebInfo.
.PARAMETER Toolchain
    Auto (por defecto), Portable (Ninja + MSVC portable sin Admin) o VisualStudio (MSBuild VS 2022).
.PARAMETER Install
    Si se especifica, instala el plugin compilado en OBS Studio al finalizar.
.PARAMETER SkipTests
    Si se especifica, omite la ejecucion de ctest.
#>
[CmdletBinding()]
param(
    [ValidateSet("RelWithDebInfo", "Release", "Debug")]
    [string]$Config = "RelWithDebInfo",
    [ValidateSet("Auto", "Portable", "VisualStudio")]
    [string]$Toolchain = "Auto",
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
            $ninjaPkgs = Get-ChildItem $wingetPkgs -Directory -Filter "*Ninja*" -ErrorAction SilentlyContinue |
                Select-Object -ExpandProperty FullName
            $extra += @($cmakeDirs) + @($ninjaPkgs)
        }

        $combined = @($extra) + @($env:Path -split ';') + @($machinePath -split ';') + @($userPath -split ';') |
            Where-Object { $_ -and $_.Trim() -ne '' } |
            Select-Object -Unique
        $env:Path = $combined -join ';'
    }

    Refresh-Path

    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $cmake) {
        throw "No se encontro 'cmake'. Ejecuta .\scripts\setup-env.ps1 (o winget install Kitware.CMake Ninja-build.Ninja)."
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = $null
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    }

    $portableEnv = "$env:LOCALAPPDATA\obs-portable-msvc\env.ps1"
    $hasPortable = Test-Path $portableEnv

    $usePortable = $false
    if ($Toolchain -eq "Portable") {
        if (-not $hasPortable) {
            throw "No se encontro MSVC portable en '$portableEnv'. Ejecuta primero: .\scripts\setup-portable-msvc.ps1"
        }
        $usePortable = $true
    } elseif ($Toolchain -eq "VisualStudio") {
        if (-not $vsPath) {
            throw "No se encontro Visual Studio 2022 Build Tools (C++). Ejecuta como Admin: .\scripts\setup-env.ps1"
        }
        $usePortable = $false
    } else {
        if ($vsPath) {
            $usePortable = $false
        } elseif ($hasPortable) {
            $usePortable = $true
        } else {
            throw @"
No se encontro compilador C++ MSVC ni toolchain portable.
Elegi una de las dos opciones:
  1) Sin permisos de Admin (recomendado): .\scripts\setup-portable-msvc.ps1
  2) Con PowerShell como Administrador:  .\scripts\setup-env.ps1
"@
        }
    }

    if ($usePortable) {
        $ninja = Get-Command ninja -ErrorAction SilentlyContinue
        if (-not $ninja) {
            throw "No se encontro 'ninja'. Instalalo con: winget install Ninja-build.Ninja"
        }
        Write-Host "=== Activando MSVC portable + Win11 SDK ===" -ForegroundColor Cyan
        . $portableEnv
        $preset = "windows-ninja-x64"
        $buildDir = Join-Path $RepoRoot "build_ninja_x64"
    } else {
        Write-Host "=== Usando Visual Studio 2022 ($vsPath) ===" -ForegroundColor Cyan
        $preset = "windows-x64"
        $buildDir = Join-Path $RepoRoot "build_x64"
    }

    Write-Host "=== Configurando proyecto (preset $preset) ===" -ForegroundColor Cyan
    Write-Host "Nota: la primera ejecucion descarga obs-deps, Qt6 y obs-studio 32.0.4 en .deps/ y compila libobs." -ForegroundColor DarkGray
    if ($usePortable) {
        & $cmake.Source --preset $preset "-DCMAKE_BUILD_TYPE=$Config"
    } else {
        & $cmake.Source --preset $preset
    }
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --preset $preset fallo (codigo $LASTEXITCODE)."
    }

    Write-Host "`n=== Compilando obs-network-device ($Config) ===" -ForegroundColor Cyan
    if ($usePortable) {
        & $cmake.Source --build $buildDir --parallel
    } else {
        & $cmake.Source --build $buildDir --config $Config --parallel
    }
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --build fallo (codigo $LASTEXITCODE)."
    }

    if (-not $SkipTests) {
        Write-Host "`n=== Ejecutando tests unitarios ===" -ForegroundColor Cyan
        $ctest = Join-Path (Split-Path $cmake.Source -Parent) "ctest.exe"
        if (-not (Test-Path $ctest)) { $ctest = "ctest" }
        if ($usePortable) {
            & $ctest --test-dir $buildDir --output-on-failure
        } else {
            & $ctest --test-dir $buildDir -C $Config --output-on-failure
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Algunos tests unitarios fallaron (codigo $LASTEXITCODE)."
        }
    }

    if ($Install) {
        Write-Host "`n=== Instalando plugin en OBS Studio ===" -ForegroundColor Cyan
        & (Join-Path $PSScriptRoot "install.ps1") -Config $Config -BuildDir $buildDir
    } else {
        Write-Host "`nCompilacion exitosa. Para instalar en OBS ejecuta: .\scripts\install.ps1 -Config $Config -BuildDir `"$buildDir`"" -ForegroundColor Green
    }
} finally {
    Pop-Location
}
