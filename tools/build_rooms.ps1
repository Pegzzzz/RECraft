# build_rooms.ps1 - builds RECraft_rooms.bin (every room's collision) from YOUR OWN copy of RE4, on Windows.
#
# Needs, all in one folder (default: a "bin" folder next to this script):
#   re4lfs.exe + xcompress64.dll   (emoose's re4-research tools: https://github.com/emoose/re4-research)
#   JADERLINK_DATUDAS_TOOL.exe     (JADERLINK's RE4 DATUDAS tool: https://github.com/JADERLINK)
#   RE4_SAT_EAT_EXTRACT.exe        (JADERLINK's RE4-SAT-EAT-TOOL: https://github.com/JADERLINK/RE4-SAT-EAT-TOOL)
# and Python 3 (https://www.python.org/, "Add python.exe to PATH" ticked).
#
# Run from PowerShell in this folder:
#   powershell -ExecutionPolicy Bypass -File .\build_rooms.ps1
#   (or add  -Game "D:\SteamLibrary\steamapps\common\Resident Evil 4"  if the game isn't in the default place)
# It works on copies in a "rooms" folder next to this script and never changes the game's files.
# At the end it copies RECraft_rooms.bin into the game's Bin32 folder (next to winmm.dll).

param(
    [string]$Game = "C:\Program Files (x86)\Steam\steamapps\common\Resident Evil 4",
    [string]$Tools = (Join-Path $PSScriptRoot "bin")
)
$ErrorActionPreference = "Stop"

foreach ($exe in "re4lfs.exe", "xcompress64.dll", "JADERLINK_DATUDAS_TOOL.exe", "RE4_SAT_EAT_EXTRACT.exe") {
    if (-not (Test-Path (Join-Path $Tools $exe))) { throw "Missing $exe in $Tools - see the top of this script." }
}
if (-not (Test-Path (Join-Path $Game "BIO4"))) { throw "RE4 not found at $Game - pass -Game `"<your Resident Evil 4 folder>`"" }
$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) { $python = (Get-Command py -ErrorAction SilentlyContinue) }
if (-not $python) { throw "Python 3 not found - install it from python.org (tick 'Add python.exe to PATH')." }

$lfsTool = Join-Path $Tools "re4lfs.exe"
$udasTool = Join-Path $Tools "JADERLINK_DATUDAS_TOOL.exe"
$satTool = Join-Path $Tools "RE4_SAT_EAT_EXTRACT.exe"
$work = Join-Path $PSScriptRoot "rooms"

$files = Get-ChildItem (Join-Path $Game "BIO4") -Directory -Filter "St*" | ForEach-Object {
    Get-ChildItem $_.FullName -File | Where-Object { $_.Name -match '^r[0-9a-f]{3}\.udas(\.lfs)?$' }
}
if (-not $files) { throw "No room files (St*\rXXX.udas.lfs) under $Game\BIO4." }
Write-Host "$($files.Count) room files found. This takes a few minutes."

$n = 0
foreach ($f in $files) {
    $n++
    $stage = $f.Directory.Name
    $room = $f.Name -replace '\.udas(\.lfs)?$', ''
    $dir = Join-Path $work "src\$stage"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $udas = Join-Path $dir "$room.udas"
    if (-not (Test-Path $udas)) {
        Copy-Item $f.FullName (Join-Path $dir $f.Name) -Force
        if ($f.Name -like "*.lfs") {
            Push-Location $Tools   # re4lfs needs xcompress64.dll next to it
            & $lfsTool -f (Join-Path $dir $f.Name) | Out-Null
            Pop-Location
            Remove-Item (Join-Path $dir $f.Name) -ErrorAction SilentlyContinue
        }
    }
    if (-not (Test-Path $udas)) { Write-Warning "$stage\$room could not be decompressed - skipped"; continue }
    Push-Location $dir
    if (-not (Test-Path (Join-Path $dir $room))) { & $udasTool -bat -idx "$room.udas" | Out-Null }
    if (Test-Path (Join-Path $dir $room)) {
        Push-Location (Join-Path $dir $room)
        Get-ChildItem -Filter *.SAT | ForEach-Object {
            if (-not (Test-Path ($_.BaseName + "_0.obj"))) { & $satTool $_.Name UHD | Out-Null }
        }
        Pop-Location
    }
    Pop-Location
    Write-Progress -Activity "Extracting room collision" -Status "$stage\$room" -PercentComplete (100 * $n / $files.Count)
}
Write-Progress -Activity "Extracting room collision" -Completed

Push-Location $work
& $python.Source (Join-Path $PSScriptRoot "build_rooms.py")
Pop-Location
$bin = Join-Path $work "RECraft_rooms.bin"
if (-not (Test-Path $bin)) { throw "build_rooms.py didn't produce RECraft_rooms.bin" }
$size = [math]::Round((Get-Item $bin).Length / 1MB, 1)
try {
    Copy-Item $bin (Join-Path $Game "Bin32\RECraft_rooms.bin") -Force
    Write-Host "Done: RECraft_rooms.bin ($size MB) copied to $Game\Bin32"
} catch {
    Write-Host "Built $bin ($size MB). Copy it into $Game\Bin32 yourself (Windows refused: $($_.Exception.Message))"
}
