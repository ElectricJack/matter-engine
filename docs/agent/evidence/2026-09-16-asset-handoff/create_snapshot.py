"""Freeze the validated development runtime without replacing a release package."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

repo = Path(__file__).resolve().parents[4]
evidence = Path(__file__).resolve().parent
checks = evidence.parent / '2026-09-16-shared-vt-pixels'
prefix = 'feedback-pair-v1'
destination = repo / 'MatterEditor/build/asset-handoff/2026-09-16-r1'
assert not destination.exists(), 'Frozen handoffs are never overwritten; choose a new version.'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


build = json.loads((checks / f'{prefix}-build-manifest.json').read_text())
assert len(build['builds']) == 5 and all(row['exit'] == 0 for row in build['builds'])
assert not build['source_changes']
assert all(sha(repo / p) == digest for p, digest in build['sources'].items())
binary = 'MatterEditor/build/windows-msvc/editor.exe'
assert sha(repo / binary) == build['binaries'][binary]
cache = (repo / 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt').read_text()
assert 'MATTER_ENABLE_PHYSX:BOOL=OFF' in cache
assert 'MATTER_ENABLE_STREAMLINE:BOOL=OFF' in cache

spec = importlib.util.spec_from_file_location('staging', repo / 'tools/stage-windows-msvc-package.py')
staging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(staging)
destination.mkdir(parents=True)
shutil.copy2(repo / binary, destination / 'editor.exe')
shutil.copy2(repo / 'MatterEditor/build/windows-msvc/editor.pdb', destination / 'editor.pdb')
shutil.copytree(repo / 'projects/world_demo', destination / 'projects/world_demo', ignore=staging.ignored)
shutil.copytree(repo / 'MatterEngine3/shared-lib', destination / 'MatterEngine3/shared-lib', ignore=staging.ignored)
(destination / 'MatterEditor').mkdir()  # working directory expected by the frozen drive.py
for relative in ['tools/matter_agent.py', 'MatterEngine3/tools/drive.py',
                 'docs/agent/control-surface.md', 'docs/agent/agent-protocol.md',
                 'MatterEngine3/docs/authoring.md']:
    target = destination / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(repo / relative, target)
notice = ['MatterEngine third-party notices\n']
for identity, relative, mode in staging.NOTICE_COMPONENTS:
    notice.append(f'\n===== {identity}: {relative} =====\n{staging.read_notice(repo / relative, mode)}\n')
(destination / 'THIRD_PARTY_NOTICES.txt').write_text(''.join(notice))

(destination / 'verify-runtime.ps1').write_text(r'''$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'snapshot.json') -Raw | ConvertFrom-Json
foreach ($entry in $manifest.runtime_sha256.psobject.Properties) {
    $path = Join-Path $PSScriptRoot $entry.Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing frozen runtime: $($entry.Name)" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) {
        throw "Frozen runtime changed: $($entry.Name)"
    }
}
Write-Output "Frozen asset runtime verified: $($manifest.id)"
''')
(destination / 'launch.ps1').write_text(r'''[CmdletBinding()]
param(
    [string]$World = 'ClayBrickWallSurfaceProof',
    [ValidateRange(1,2048)][int]$TextureDensity = 512,
    [string]$CommandFile,
    [string]$ResultFile,
    [switch]$TextureDumps
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'verify-runtime.ps1')
# Start predictably, independent of previous engine QA environment overrides.
Get-ChildItem Env:MATTER_* | ForEach-Object { Remove-Item -LiteralPath ('Env:' + $_.Name) }
$env:TMP = Join-Path $env:LOCALAPPDATA 'Temp'
$env:TEMP = $env:TMP
$env:MATTER_WORLD = $World
$env:MATTER_VT_PROP_TEXELS_PER_METER = [string]$TextureDensity
$env:MATTER_ISSUE_DIR = Join-Path $PSScriptRoot 'issues'
if ($CommandFile) { $env:MATTER_CMD_FIFO = [IO.Path]::GetFullPath($CommandFile) }
if ($ResultFile) { $env:MATTER_AGENT_RESULT_FILE = [IO.Path]::GetFullPath($ResultFile) }
if ($TextureDumps) { $env:MATTER_TILESET_DUMP_PNG = '1' }
Push-Location -LiteralPath $PSScriptRoot
try { & (Join-Path $PSScriptRoot 'editor.exe'); $code = $LASTEXITCODE }
finally { Pop-Location }
exit $code
''')
(destination / 'launch.cmd').write_text('@echo off\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" %*\r\n')

(destination / 'README.md').write_text('''# Frozen asset-authoring build: 2026-09-16-r1

This directory contains a fixed MSVC RelWithDebInfo engine/editor binary,
embedded shaders, matching engine JS helpers, an editable copy of world_demo,
symbols, automation tools and third-party notices. Normal engine rebuilds do not
write here. Future handoffs get a new version directory; keep this one pinned.

## Start

Double-click `launch.cmd`, or run native PowerShell:

```powershell
.\\launch.ps1 -World ClayBrickWallSurfaceProof
.\\launch.ps1 -World ClayBrickMaze
```

The launcher sets prop texture density to 512 texels/metre for asset inspection
(the engine default is 16). Use `-TextureDensity 256` for a lighter preview.

The launcher verifies runtime hashes, clears inherited MATTER_* overrides and
starts with the editor panels available. F11 toggles presentation mode. The
default world is the three low-poly box walls with geometry-baked brick detail.
Use the Assets/Workbench panels to inspect parts and variations.

## Asset-agent working directory

Create and edit JS assets under **this copy** of `projects/world_demo/objects/`
and scenes under `projects/world_demo/scenes/`. Group new files by subject and
use unique module/class names across the project. Project-specific JS helpers
live in `projects/world_demo/shared-lib/`. Existing brick examples are under
`objects/texturing/bricks/` and `scenes/texturing/bricks/`.

The executable searches beside itself first, so edits to the main checkout do
not change this copy's assets or engine helpers. Project caches start cold and
are generated here. First bakes can take time. When work is ready, copy only the
authored source/assets back to the main checkout; exclude `.cache`, generated
screenshots and workbench scratch data. Preserve any concurrent source changes.

Keep `editor.exe`, `editor.pdb`, `MatterEngine3/shared-lib/` and automation tools
fixed. `verify-runtime.ps1` checks these runtime inputs. `snapshot.json` records
initial project hashes too; editing project files is expected and does not fail
runtime verification.

## Automation

The editor takes no command-line asset-export options. Use the frozen
`docs/agent/control-surface.md`, `docs/agent/agent-protocol.md` and
`tools/matter_agent.py` for scene inspection, edits and viewport capture.

```powershell
.\\launch.ps1 -World ClayBrickWallSurfaceProof `
  -CommandFile C:\\tmp\\asset-commands.txt -ResultFile C:\\tmp\\asset-results.jsonl
```

Use fresh command/result files for a new session. Paths passed to the Windows
editor must be Windows paths. The included `MatterEngine3/tools/drive.py` accepts
`--editor` pointing to this folder's editor.exe for scripted screenshot runs.

## Export capabilities in this build

* **Tileset debug texture PNGs:** launch with `-TextureDumps` (sets
  `MATTER_TILESET_DUMP_PNG=1`). On a real hardware-RT tileset bake, sidecars are
  written beside the `.gtex` in that world's cache: `-albedo.png`, `-normal.png`,
  `-orm.png`, `-height.png`, and horizon images when generated. A `.gtex` cache
  hit returns before dumping: use a fresh disposable scene/cache or a changed
  source to trigger the bake; do not delete another session's active cache.
* These PNGs are diagnostic: height is reduced from 16 to 8 bits; normals contain
  two encoded channels rather than a conventional three-channel normal map.
  This is the tileset path, not an exporter for composed wall/terrain VT pages.
* **General OBJ export:** no editor/agent command currently provides it. The
  existing OBJ writers cover contour tests and hydrology traces, and write
  positions/faces without an asset-ready UV/material bundle.
* Screenshots are available through the automation API. Full mesh+UV+MTL and
  lossless PBR/height export still need implementation if external DCC use is
  required.

## Scope and validation

This is a frozen **development snapshot** from the current working tree, with
source and binary hashes. It is not the formal clean-tree release package.
Vulkan/RT and autoremesher are enabled; optional PhysX fluids and NVIDIA
Streamline/DLSS are disabled in this build. The existing Box3D procedural
settling path is separate from the disabled PhysX feature.

See `validation/` for the native test results and clean-PATH packaged launch/
render evidence. Passing those checks supports this asset workflow; it does not
mean all rendering features or the broader texturing goal are complete. Shared
periodic materials are not yet bound to ordinary wall scenes; use the existing
finite wall recipes in this snapshot.
''')

runtime_files = [destination / 'editor.exe', destination / 'editor.pdb',
                 destination / 'launch.ps1', destination / 'launch.cmd', destination / 'verify-runtime.ps1',
                 destination / 'tools/matter_agent.py', destination / 'MatterEngine3/tools/drive.py']
runtime_files += [p for p in (destination / 'MatterEngine3/shared-lib').rglob('*') if p.is_file()]
manifest = {
    'id': destination.name,
    'kind': 'frozen-development-snapshot',
    'configuration': 'MSVC RelWithDebInfo',
    'source_revision': subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
    'source_tracked_dirty': bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=repo,text=True).strip()),
    'features': {'vulkan': True, 'autoremesher': True, 'physx': False, 'streamline': False},
    'validation_prefix': prefix,
    'launch_defaults': {'world': 'ClayBrickWallSurfaceProof', 'prop_texture_density': 512, 'editor_panels_visible': True},
    'runtime_sha256': {str(p.relative_to(destination)).replace('\\','/'):sha(p) for p in runtime_files},
    'initial_project_sha256': {str(p.relative_to(destination)).replace('\\','/'):sha(p)
        for p in sorted((destination / 'projects/world_demo').rglob('*')) if p.is_file()},
    'native_sources_sha256': build['sources'],
}
(destination / 'snapshot.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'snapshot':str(destination),'runtime_files':len(runtime_files),
                  'project_files':len(manifest['initial_project_sha256']),
                  'editor_sha256':sha(destination/'editor.exe')}),flush=True)
