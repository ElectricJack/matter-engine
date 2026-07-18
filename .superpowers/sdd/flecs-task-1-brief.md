### Task 1: Vendor and Pin Flecs v4.1.6

**Files:**

- Create: `Libraries/flecs/flecs.h`
- Create: `Libraries/flecs/flecs.c`
- Create: `Libraries/flecs/LICENSE`
- Create: `Libraries/flecs/VERSION`

- [ ] **Step 1: Record the expected pre-vendor failure**

Run:

```powershell
Test-Path Libraries\flecs\flecs.h
```

Expected: `False`.

- [ ] **Step 2: Download the exact upstream release artifacts**

Run:

```powershell
New-Item -ItemType Directory -Force Libraries\flecs
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.h -o Libraries\flecs\flecs.h
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.c -o Libraries\flecs\flecs.c
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/LICENSE -o Libraries\flecs\LICENSE
```

Do not regenerate or edit the downloaded files.

- [ ] **Step 3: Add a reproducible pin manifest**

Generate `Libraries/flecs/VERSION` from the downloaded files:

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
```

The resulting file contains two actual lowercase 64-character hashes and no symbolic values.

- [ ] **Step 4: Verify pin contents and license**

Run:

```powershell
Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.h, Libraries\flecs\flecs.c
Get-Content Libraries\flecs\VERSION
Select-String -Path Libraries\flecs\LICENSE -Pattern "MIT License"
```

Expected: computed hashes match `VERSION`; the license query returns a match.

- [ ] **Step 5: Commit the dependency pin**

```powershell
git add Libraries/flecs
git commit -m "build(ecs): vendor Flecs 4.1.6"
```

---

