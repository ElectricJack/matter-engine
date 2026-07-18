$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ui = Get-Content -Raw (Join-Path $root 'MatterViewer\ui.cpp')
$main = Get-Content -Raw (Join-Path $root 'MatterViewer\main.cpp')
$header = Get-Content -Raw (Join-Path $root 'Libraries\ImGuizmo\ImGuizmo.h')
$source = Get-Content -Raw (Join-Path $root 'Libraries\ImGuizmo\ImGuizmo.cpp')
$failures = [System.Collections.Generic.List[string]]::new()

if (-not $header.Contains('bool IsOver(OPERATION op)')) {
    $failures.Add('pinned ImGuizmo header lacks operation hover overload')
}
if (-not $source.Contains('bool IsOver(OPERATION op)')) {
    $failures.Add('pinned ImGuizmo source lacks operation hover implementation')
}
if (-not $ui.Contains('ImGuizmo::IsOver(ImGuizmo::TRANSLATE)')) {
    $failures.Add('Viewer does not query current TRANSLATE operation hover')
}
if ($ui.Contains('ImGuizmo::IsOver(),')) {
    $failures.Add('Viewer still uses previous-frame zero-argument IsOver')
}

$begin = $main.IndexOf('ui.begin_frame(frame, error)')
$panel = $main.IndexOf('ui.draw_sector_streaming_panel(')
$capture = $main.IndexOf('camera_input_order.decide_capture')
$snapshot = $main.IndexOf('const matter::CameraDesc frame_camera = camera;')
$follow = $main.IndexOf('ui.update_sector_streaming(*session, frame_camera)')
$tick = $main.IndexOf('session->tick(tick)')
$render = $main.IndexOf('session->render(frame_camera, frame, options, error)')
$uiEnd = $main.IndexOf('ui.end_frame(frame, error)')
$present = $main.IndexOf('vulkan->end_frame(frame, frame_presented, error)')
$cameraUpdate = $main.IndexOf('camera_controller.update(window, dt, camera)')
$ordered = @($begin, $panel, $capture, $snapshot, $follow, $tick, $render,
             $uiEnd, $present, $cameraUpdate)
if ($ordered | Where-Object { $_ -lt 0 }) {
    $failures.Add('Viewer frame-order source seam is incomplete')
} else {
    for ($index = 1; $index -lt $ordered.Count; ++$index) {
        if ($ordered[$index - 1] -ge $ordered[$index]) {
            $failures.Add('Viewer frame order is not UI/capture -> snapshot -> tick/render/present -> next camera')
            break
        }
    }
}

if ($failures.Count -ne 0) {
    [Console]::Error.WriteLine("Task 6 static check: FAIL ($($failures.Count))")
    $failures | ForEach-Object { [Console]::Error.WriteLine("  - $_") }
    exit 1
}
Write-Output 'Task 6 static check: PASS'
