# Open the authored scene with the editor UI. The capture script is a separate, finite QA run.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..'))
$env:MATTER_WORLD = 'Kreuzenstein'
$env:MATTER_IMPOSTOR = '0'
$env:MATTER_CAM = '43,11,76,-1,21,2'
$env:MATTER_WINDOW_WIDTH = '1400'
$env:MATTER_WINDOW_HEIGHT = '1000'
$env:TMP = [IO.Path]::GetTempPath()
$env:TEMP = $env:TMP
Remove-Item Env:MATTER_HIDE_UI -ErrorAction SilentlyContinue
Remove-Item Env:MATTER_SCREENSHOT -ErrorAction SilentlyContinue
# The command channel selects native RT after the scene is ready.
$previewDir = Join-Path $root 'build\qa\kreuzenstein'
New-Item -ItemType Directory -Force $previewDir | Out-Null
$env:MATTER_CMD_FIFO = Join-Path $previewDir 'preview-commands.txt'
[IO.File]::WriteAllText($env:MATTER_CMD_FIFO, "wait_idle 2 90`nrender_path native_rt`nset viewer.budget.pixel_budget 4`ncam 43 15 88 -1 23 0`n", (New-Object Text.UTF8Encoding $false))
Remove-Item Env:MATTER_AGENT_RESULT_FILE -ErrorAction SilentlyContinue
Set-Location (Join-Path $root 'MatterEditor')
& '.\build\windows-msvc\editor.exe'
exit $LASTEXITCODE
