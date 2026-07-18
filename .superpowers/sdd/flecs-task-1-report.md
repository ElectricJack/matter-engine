# Flecs task 1 report — Vendor and Pin Flecs v4.1.6

## Implementation

Downloaded the exact upstream Flecs `v4.1.6` distribution header, source, and
license into `Libraries/flecs`. Added `Libraries/flecs/VERSION` with the
release URL and SHA-256 hashes computed from the downloaded header and source.
The downloaded upstream artifacts were not regenerated or edited.

Commit: `176fbd0 build(ecs): vendor Flecs 4.1.6`

## Verification

Pre-vendor command and result:

```powershell
Test-Path Libraries\flecs\flecs.h
# False
```

The first download attempt failed because the sandbox could not connect to
`raw.githubusercontent.com`; the same prescribed download commands succeeded
after approved network escalation:

```powershell
New-Item -ItemType Directory -Force Libraries\flecs
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.h -o Libraries\flecs\flecs.h
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.c -o Libraries\flecs\flecs.c
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/LICENSE -o Libraries\flecs\LICENSE
# Exit code: 0
```

Manifest-generation command completed successfully:

```powershell
$headerHash = (Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.h).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.c).Hash.ToLowerInvariant()
$version = @(
    'version=4.1.6'
    'release=https://github.com/SanderMertens/flecs/releases/tag/v4.1.6'
    "header_sha256=$headerHash"
    "source_sha256=$sourceHash"
) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $PWD 'Libraries\flecs\VERSION'), $version + "`n")
# Exit code: 0
```

Post-download and post-commit verification command and results:

```powershell
Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.h, Libraries\flecs\flecs.c
# flecs.h: 526036A5A41678E2A43A3CB835E9EAA70FD1993868B1978950C0D275752F69B1
# flecs.c: 6005392EB13C0F3C7ABDECB2F85271E50934F63919AFEBF0BBE85F6DFC7320D6

Get-Content Libraries\flecs\VERSION
# version=4.1.6
# release=https://github.com/SanderMertens/flecs/releases/tag/v4.1.6
# header_sha256=526036a5a41678e2a43a3cb835e9eaa70fd1993868b1978950c0d275752f69b1
# source_sha256=6005392eb13c0f3c7abdecb2f85271e50934f63919afebf0bbe85f6dfc7320d6

Select-String -Path Libraries\flecs\LICENSE -Pattern "MIT License"
# LICENSE:1:MIT License

git show --stat --oneline --summary HEAD
# 176fbd0 build(ecs): vendor Flecs 4.1.6
# 4 files changed, 138859 insertions(+)
# Libraries/flecs/LICENSE, Libraries/flecs/VERSION,
# Libraries/flecs/flecs.c, Libraries/flecs/flecs.h
```

## Files changed

- `Libraries/flecs/flecs.h`
- `Libraries/flecs/flecs.c`
- `Libraries/flecs/LICENSE`
- `Libraries/flecs/VERSION`

## Self-review

- Confirmed the pre-vendor header did not exist.
- Confirmed the manifest contains the exact `4.1.6` version, release URL, and
  two actual lowercase 64-character SHA-256 values.
- Confirmed recomputed header and source hashes exactly match the manifest.
- Confirmed the vendored license contains `MIT License`.
- Confirmed the commit contains only the four requested `Libraries/flecs`
  artifacts; no later ECS or build-task files were changed.

## Concerns

No task-blocking concerns. The environment has no compiler/Make/WSL distro, so
only the prescribed file and hash verification was applicable and executed.
