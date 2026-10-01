param(
    [switch]$Build,
    [string]$OutputDirectory = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (Get-Process editor -ErrorAction SilentlyContinue) {
    throw "Close the existing Matter editor before launching the geometry demo."
}
if ($Build) { & "$PSScriptRoot/build-windows.ps1" -Config RelWithDebInfo -Target matter_editor }
$editor = Join-Path $repo "MatterEditor/build/windows-msvc/editor.exe"
if (!(Test-Path $editor)) { throw "Build the editor first, or pass -Build." }
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $env:TEMP ("MatterMountainGeometry-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$commands = Join-Path $OutputDirectory "commands.txt"
@"
wait_event bake.finished 60
render_path raster
set render.gi.enabled false
set render.pom.enabled false
set render.volumetrics.enabled false
set render.cloud_shadows.enabled false
wait_idle 3 240
cam 436 30 1473 421 22 1454
"@ | Set-Content -Encoding ASCII $commands
$settings = @{
    MATTER_WORLD = "StreamMountain"
    MATTER_GEOMETRY_PAGES = "1"
    MATTER_GEOMETRY_MODULE = "MountainDetailRock"
    MATTER_GEOMETRY_TERRAIN = "1"
    MATTER_GEOMETRY_RASTER_ONLY = "1"
    MATTER_GEOMETRY_MIN_TRIANGLES = "16384"
    MATTER_GEOMETRY_CPU_MB = "1024"
    MATTER_GEOMETRY_ROOT_MB = "1024"
    MATTER_GEOMETRY_READ_AHEAD_MB = "4"
    MATTER_GEOMETRY_UPLOAD_CPU_MS = "4"
    MATTER_GEOMETRY_GPU_MB = "1024"
    MATTER_GEOMETRY_PAGES_PROFILE = "1"
    MATTER_CAM = "436,30,1473,421,22,1454"
    MATTER_WINDOW_WIDTH = "1280"
    MATTER_WINDOW_HEIGHT = "720"
    MATTER_CMD_FIFO = $commands
    MATTER_AGENT_RESULT_FILE = (Join-Path $OutputDirectory "results.jsonl")
    MATTER_HIDE_WINDOW = "0"
    MATTER_HIDE_UI = $null
    MATTER_SCREENSHOT = $null
    MATTER_CAM_PATH = $null
    MATTER_REPLAY = $null
    MATTER_DISABLE_VK_RT = "1"
    MATTER_VOLUMETRICS = "0"
    MATTER_PRESENT_MODE = "immediate"
}
$previous = @{}
try {
    foreach ($name in $settings.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
        [Environment]::SetEnvironmentVariable($name, $settings[$name], "Process")
    }
    $process = Start-Process -FilePath $editor -WorkingDirectory (Join-Path $repo "MatterEditor") `
        -RedirectStandardOutput (Join-Path $OutputDirectory "stdout.log") `
        -RedirectStandardError (Join-Path $OutputDirectory "stderr.log") -PassThru
    Write-Output "Streaming Mountains geometry demo: editor PID $($process.Id)"
    Write-Output "Commands and logs: $OutputDirectory"
    Write-Output "Terrain virtual geometry baseline. UI enabled; RT, GI and POM disabled."
} finally {
    foreach ($name in $previous.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], "Process")
    }
}
