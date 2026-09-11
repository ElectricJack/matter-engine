# Native castle route batch

`tools/castle_site_walkthrough_batch.py` exports current compiled manifests and
prepares every actual staircase route plus seven supplemental connector detours
in the three individual castle scenes.
Preparation uses Node.js and the existing walkthrough driver's `prepare` checks;
it never launches the editor. A native run regenerates those manifests first,
then launches one acceptance process at a time, stopping at the first failure.

From this integration checkout in WSL:

```bash
python3 tools/castle_site_walkthrough_batch.py --output-root /mnt/d/tmp/castle-physical-acceptance --prepare-only
```

After building the integration editor with PhysX, run from native Windows:

```powershell
py -3 D:/tmp/matter-castle-assembly/tools/castle_site_walkthrough_batch.py --output-root D:/tmp/castle-physical-acceptance --run
```

Both commands create a new timestamped evidence directory. The prepared
`launch-native.cmd` invokes the batch again and therefore regenerates manifests;
it does not reuse potentially stale prepared routes. `route-commands.txt` lists
the exact 20 individual driver commands for inspection, including explicit
`--static-vertex-reserve-mb 4096 --static-index-reserve-mb 512`. For a complete
acceptance run use the batch launcher, which pins and monitors its inputs.

The default editor is `MatterEditor/build/windows-msvc/editor.exe`; its paired
cache is `MatterEditor/build/cmake/windows-msvc/relwithdebinfo/CMakeCache.txt`.
Both are relative to the checkout containing the runner. `--editor` and
`--cmake-cache` permit explicit matching paths. `--timeout` defaults to 7200
seconds **per route**. Native Python needs Node.js on its PATH for fresh export;
`--node` selects another executable. Output paths must contain no whitespace,
as required by the screenshot FIFO grammar.

## Planned coverage: 13 stairs plus seven connector detours

| Scene | Selected stair IDs | Routes |
| --- | --- | ---: |
| CastleClusteredCourt | `chapel:stair:choir-stair`, `hall:stair:stair-0`, `keep:stair:stair-0`, `keep:stair:stair-1` | 4 |
| CastleAngledBailey | `chapel:stair:choir-stair`, `gate:stair:household-stair`, `hall:stair:stair-0`, `keep:stair:stair-0`, `keep:stair:stair-1` | 5 |
| CastleBentPalace | `chapel:stair:choir-stair`, `hall:stair:stair-0`, `keep:stair:stair-0`, `solar:stair:stair-0` | 4 |

These are discovered from the fresh manifests, with expected counts checked to
catch disappearing stairs. Every route begins at the actual authored site spawn
and preserves the selected staircase's complete waypoint sequence. Upper keep
routes also retain the preceding ascent. Paths to other wings cross the actual
connector mouth planes; local stair approaches cross internal doors where those
are on the compiler-selected route. Native acceptance requires measured finite
opening crossings, grounded arrivals, complete stair progression, bounded motion,
no jumps, valid publication and current screenshots. No thresholds are relaxed.
Walking uses raster rendering and mesh representations (`MATTER_IMPOSTOR=0`),
matching the castle capture cache. Raster/RT lighting comparisons have separate
capture receipts; the physical test retains the full authored geometry in view.

The current 13 routes collectively plan 14 internal doorway crossings and 18
unique connector-mouth crossings (nine complete connectors). They do **not**
cover every passage. Seven supplemental overrides exercise the connectors outside
those original stair paths:

- Clustered court: `keep-service`, `service-chapel`.
- Angled bailey: `north-curtain`.
- Bent palace: `hall-kitchen`, `lower-court`, `solar-service`, `upper-court`.

Each supplemental override follows an existing entry-to-room route, uses
`routeManifestRoomSegment` to join the chosen inside mouth around the actual
room fixtures, traverses every published connector waypoint, reverses that
supported path to the authored spawn, then follows an existing complete stair
route. The runner uses the driver's existing `--route-file` facility and retains
its override hash. It asserts that both designated finite mouth planes are in the
driver's planned crossings and that neither the outbound nor return connector
sequence was shortened. No driver path or proof rules were changed.

Together the 20 routes plan all 13 stairs and all 32 mouth planes of all 16
connectors, plus 18 internal doorways. Twenty other internal doorways remain
unvisited. `batch.json` records both the original stair-path coverage and the
complete planned/unvisited lists. Planned coverage is not physical acceptance.

## Evidence and failure behavior

Each invocation records source hashes, fresh manifest hashes and authored
controller/spawn checks. Native mode additionally pins the editor and CMake
cache, requires PhysX, checks sources/binaries before and after every route, and
verifies the driver's receipt and emitted route match the planned inputs.
The two reserve options are explicitly passed through the driver's sanitized
environment and retained in its `result.json`.

Every route receives its own `route.json`, `publication.json`, `result.json`,
telemetry, command log and screenshots. The aggregate `batch.json` links and
hashes successful native receipts. Only completion of all 20 native routes
changes the batch status to `passed`; preparation remains `prepared`, and any
failure or changed input stops the batch. No native acceptance was performed
while preparing this tool.
