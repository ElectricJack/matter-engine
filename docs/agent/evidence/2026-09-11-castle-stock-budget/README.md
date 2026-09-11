# Castle gallery stock census

The actual recursive `requires()` and `build()` census places all three castle
variations: **69,849 Part placements, 204 unique effective recipes**.
This counts merged default arguments, material handles and both declarations and
placements. Native meshing jobs and LOD representations are separate counts.

The ordinary masonry uses **8 stone recipes across 58,323 placements**.
Angled connector ends reuse two triangular stone recipes; true arch profiles
retain 18 separate cut recipes. Structure layers share 40 recipes across 110
placements. No geometry is omitted to meet the stock limits.

`gallery-census.json` retains executed source hashes and every effective recipe;
`stock-budgets.json` records the checked budgets and the previous census comparison.
The external paths in that comparison identify the original local run. Reproduce
from repository root after authoring sources settle:

```sh
CASTLE_STOCK_CENSUS_OUTPUT=/tmp/castle-gallery-census.json node projects/world_demo/tests/castle_site_stock_budget_tests.mjs
```

| Module | Unique recipes | Placements |
| --- | ---: | ---: |
| CastleWingStructure | 40 | 110 |
| CastleCutStone | 18 | 217 |
| CastleSiteConnector | 16 | 16 |
| CastleSiteConnectorAssembly | 16 | 16 |
| CastleWingMasonry | 15 | 15 |
| CastleWingStructureAssembly | 15 | 15 |
| CastleBeam | 12 | 3,420 |
| CastleStone | 8 | 58,323 |
| CastleWindowGlazing | 8 | 246 |
| CastleChandelier | 6 | 26 |
| CastleFixtureGlow | 6 | 142 |
| CastleChair | 4 | 24 |
| CastlePavingSlab | 4 | 4 |
| CastlePlank | 4 | 2,886 |
| CastleSconce | 4 | 113 |
| CastleSitePaving | 4 | 4 |
| CastleTable | 4 | 25 |
| CastleAltar | 2 | 3 |
| CastleBarrel | 2 | 27 |
| CastleBed | 2 | 7 |
| CastleBench | 2 | 39 |
| CastleChest | 2 | 43 |
| CastleClippedFlag | 2 | 521 |
| CastleCupboard | 2 | 35 |
| CastleEntranceApron | 2 | 3 |
| CastleTriangularStone | 2 | 816 |
| CastleMortarCore | 1 | 2,752 |
| CastlePlinth | 1 | 1 |

Refreshed from frozen authoring sources at **2026-09-11T17:42:13.065Z**, after the
legacy-roof support, 8 mm candle source radius and sun-side scene camera updates.
The comparison to the archived previous census preserves all **204 recipes and
69,849 placements**, including **8 ordinary stone recipes**; every per-module
recipe and placement count is unchanged. Executed source hashes were rechecked
against the checkout before retaining this evidence. The matching 32-frame camera
plan is `/mnt/d/tmp/castle-final-captures-v4/camera-manifest.json`.

These are CPU authoring measurements, not native visual or physical acceptance.
