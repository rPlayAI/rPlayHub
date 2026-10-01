# Fetch GUI dependencies for Windows
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$deps = Join-Path $root "deps"
if (!(Test-Path $deps)) { New-Item -ItemType Directory -Path $deps -Force | Out-Null }

# 1. Dear ImGui
$imguiDir = Join-Path $deps "imgui"
if (!(Test-Path $imguiDir)) {
    Write-Host "Fetching Dear ImGui v1.91.8..."
    git clone --branch v1.91.8 --depth 1 https://github.com/ocornut/imgui.git $imguiDir
}

# 2. nlohmann/json
$jsonDir = Join-Path $deps "json\nlohmann"
if (!(Test-Path "$jsonDir\json.hpp")) {
    Write-Host "Fetching nlohmann/json v3.11.3..."
    New-Item -ItemType Directory -Path $jsonDir -Force | Out-Null
    Invoke-WebRequest -Uri "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" -OutFile "$jsonDir\json.hpp"
}

# 3. SDL2 Development Libraries (VC)
$sdlDir = Join-Path $deps "sdl2"
if (!(Test-Path $sdlDir)) {
    Write-Host "Fetching SDL2 VC development library..."
    $sdlZip = "$env:TEMP\SDL2-VC.zip"
    Invoke-WebRequest -Uri "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip" -OutFile $sdlZip
    $tempExtract = "$env:TEMP\sdl2_temp"
    if (Test-Path $tempExtract) { Remove-Item $tempExtract -Recurse -Force }
    Expand-Archive -Path $sdlZip -DestinationPath $tempExtract -Force
    $inner = Get-ChildItem -Path $tempExtract | Select-Object -First 1
    Move-Item -Path $inner.FullName -Destination $sdlDir -Force
    Remove-Item $sdlZip, $tempExtract -Recurse -Force
}

# Ensure include/SDL2 junction
$sdl2Include = Join-Path $sdlDir "include\SDL2"
if (!(Test-Path $sdl2Include)) {
    New-Item -ItemType Junction -Path $sdl2Include -Target (Join-Path $sdlDir "include") | Out-Null
}

Write-Host "Windows GUI dependencies ready in deps/"
