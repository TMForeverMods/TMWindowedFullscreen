# Installs TM Windowed Fullscreen into the TrackMania ModLoader.
#
# The ModLoader keeps one folder per mod and per version, each with a
# description.yaml:
#   %LOCALAPPDATA%\TMLoader\database\TmForever\products\TM Windowed Fullscreen\<version>\
#
# The descriptors are edited in the project root (description.yaml, version.yaml)
# and the build generates the release folder:
#   packaging\TM Windowed Fullscreen\
#       description.yaml                  (from description.yaml)
#       install-modloader.ps1             (this script, copied in by the build)
#       <version>\description.yaml        (from version.yaml)
#       <version>\TMWindowedFullscreen.dll
#
# It can be run from the project root or from the generated release folder:
#	.\install-modloader.ps1
#	or
#   powershell -ExecutionPolicy Bypass -File install-modloader.ps1

$version = "1.0.0"

# Release folder = the directory that holds description.yaml and <version>\. When
# run from the generated packaging folder that is $PSScriptRoot; when run from the
# project root it is packaging\TM Windowed Fullscreen.
$release = $PSScriptRoot
if (-not (Test-Path "$release\$version\TMWindowedFullscreen.dll")) {
    $release = Join-Path $PSScriptRoot "packaging\TM Windowed Fullscreen"
}

$desc = "$release\description.yaml"
$Dll = "$release\$version\TMWindowedFullscreen.dll"
$versionDesc = "$release\$version\description.yaml"
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) {
    throw "$Dll not found. Build first: cmake --build build --config Release"
}
if (-not (Test-Path $desc)) {
    throw "$desc not found."
}
if (-not (Test-Path $versionDesc)) {
    throw "$versionDesc not found."
}

if (Get-Process -Name TmForever -ErrorAction SilentlyContinue) {
    throw "TrackMania is running. Close the game first."
}

$loader = "$env:LOCALAPPDATA\TMLoader"
if (-not (Test-Path $loader)) {
    throw "The TrackMania ModLoader is not installed ($loader is missing). Get it from https://tomashu.dev/software/tmloader/"
}

$product = "$loader\database\TmForever\products\TM Windowed Fullscreen"
$target = "$product\$version"
New-Item -ItemType Directory -Force -Path $target | Out-Null

Copy-Item $desc "$product\description.yaml" -Force
Copy-Item $versionDesc "$target\description.yaml" -Force
Copy-Item $Dll "$target\TMWindowedFullscreen.dll" -Force

Write-Host "Installed TM Windowed Fullscreen $version to $target" -ForegroundColor Green
Write-Host "Open the ModLoader, tick 'TM Windowed Fullscreen' and start the game."
