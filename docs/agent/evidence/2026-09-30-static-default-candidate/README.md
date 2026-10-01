# Static-default candidate evidence

Status: native validation and all four serial captures complete. See the
[candidate report](../../../findings/static-default-candidate-validation-2026-09-30.md)
for the retained shader baseline, coverage differences and pacing limits.

The native build is MSVC RelWithDebInfo, with the repository's configured
Windows toolchain. Run from the repository root in WSL:

```sh
./tools/build-windows-from-wsl.sh RelWithDebInfo geometry_default_off_checks
repo_windows=$(wslpath -w "$PWD")
recipe_windows=$(wslpath -w "$PWD/docs/agent/evidence/2026-09-30-static-default-candidate/native_checks.ps1")
native_powershell=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
"$native_powershell" -NoProfile -ExecutionPolicy Bypass -File "$recipe_windows" -RepositoryRoot "$repo_windows" -Phase build
"$native_powershell" -NoProfile -ExecutionPolicy Bypass -File "$recipe_windows" -RepositoryRoot "$repo_windows" -Phase test
"$native_powershell" -NoProfile -ExecutionPolicy Bypass -File "$recipe_windows" -RepositoryRoot "$repo_windows" -Phase smoke
```

The test phase uses `ctest -j 1` and explicitly includes the retained shader
failure. [ctest.log](ctest.log) records the original 21/23 result; the
repaired fixture check is separate in [golden-rerun.log](golden-rerun.log).
[golden-update.log](golden-update.log) records existing update-mode use.
[smoke.log](smoke.log) records all four passing modes and validation counts.
These logs are correctness evidence; GPU contention during the native tests
makes their execution times unsuitable for product performance attribution.

The golden update and subsequent normal run use the native
`MatterEditor/build/cmake/windows-msvc/relwithdebinfo/obj_export_golden_tests.exe`,
with `MatterEngine3/tests` as working directory. Set
`MATTER_EXPORT_GOLDEN_UPDATE=1` only for the first invocation, then unset it
and rerun. `TMP` and `TEMP` use `C:/tmp`; forward them and the update variable
through `WSLENV` when invoking the native executable from WSL. Review the
fixture diff before accepting it.

Additional focused checks:

```sh
node projects/world_demo/tests/kreuzenstein_scene_tests.mjs
node projects/world_demo/tests/mountain_geometry_site_tests.mjs
node projects/world_demo/tests/mountain_detail_rocks_tests.mjs
node projects/world_demo/tests/mountain_rocks_tests.mjs
node projects/world_demo/tests/mountain_forest_tests.mjs
node projects/world_demo/tests/mountain_terrain_only_tests.mjs
node docs/agent/evidence/2026-09-30-rock-density-restore/population.mjs
make -C MatterEngine3 check-me3-source-basenames
python3 -m unittest discover -s tools/tests -p test_frame_attribution.py
```

The capture campaign and reducer run from the repository root:

```sh
python3 docs/agent/evidence/2026-09-30-static-default-candidate/capture.py
python3 docs/agent/evidence/2026-09-30-static-default-candidate/reduce.py
```

While the campaign runs, `reduce.py --partial` validates completed runs into
`partial-summary.json`, explicitly marked incomplete. The normal reducer
requires all four completed runs. The phase parser accepts intact numeric
duration tokens even when stderr interrupts the following unit. The census
parser removes only complete known PartStore/static-upload diagnostics in memory and
requires all 28 numeric STATS fields; raw logs remain unchanged. A retained
historical complete capture was re-reduced to verify unchanged frame counts,
distributions, hitch counts and settings.

The driver refuses to overwrite prior run directories. Select a new artifact
root with `--root` for a fresh campaign. Keep the source HEAD unchanged until
the campaign finishes so the original driver records one consistent source
hash. The campaign uses the existing <2,048 MiB whole-GPU idle gate and runs
static/VG early samples, then static/VG late samples serially. It records
camera/settings, source/binary/input hashes, screenshot completion and host
load; the reducer checks trace absence for static and actual geometry work
for VG. No test or capture runs concurrently with another C++/GPU workload
started by this worker.

Raw build/test logs, native CTest detail, the exact executable, campaign log,
GPU observations and eventual capture files are retained at
`C:/tmp/quick-meadow68-4-20260930` (WSL:
`/mnt/c/tmp/quick-meadow68-4-20260930`). This directory preserves
[protocol.json](protocol.json), [summary.json](summary.json), all four PNGs,
[host-load.csv](host-load.csv), native logs and the complete raw capture
directories as four `.tar.gz` archives. [archives.json](archives.json)
records their hashes; every archived raw artifact and screenshot was checked
against the summary's original hash before publication. The executable stays
outside Git and is identified by its SHA-256.
Standalone native/host diagnostic log copies normalize line endings and
trailing table padding for repository whitespace checks. Original native logs
remain under the artifact root; capture archive contents retain original bytes.

To re-reduce the preserved evidence, extract each run archive into a new root,
copy `protocol.json` there, and pass that root to the normal reducer. Archives
contain their original source/binary records, perf JSON, traces, raw logs,
commands, screenshots and GPU observations. Run from the repository root:

```sh
python3 docs/agent/evidence/2026-09-30-static-default-candidate/reduce.py --root /path/to/extracted-evidence
```

The source/binary capture hashes remain `b387c15d81ff4d5b26a195b14f1afd6793b6ad6b`
and `22bb85901df3a2313bf4966545686a8c47580937a64853f4d385bbe07b28671c`.
Later commits contain documentation and parser repairs, without changing the
captured native editor or authored scene inputs.
