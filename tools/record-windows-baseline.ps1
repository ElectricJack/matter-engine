[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bash = 'C:\msys64\usr\bin\bash.exe'
$make = 'C:\msys64\usr\bin\make.exe'
$gpp = 'C:\msys64\ucrt64\bin\g++.exe'
$objdump = 'C:\msys64\ucrt64\bin\objdump.exe'
$glslc = 'C:\msys64\ucrt64\bin\glslc.exe'

function Get-Sha256([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Invoke-RequiredNative([string]$filePath, [string[]]$arguments, [string]$description) {
    # Native stderr can contain successful Vulkan-loader warnings; capture it
    # as evidence and decide success solely from the process exit code.
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $filePath @arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($exitCode -ne 0) {
        throw "$description failed with exit code ${exitCode}: $($output -join [Environment]::NewLine)"
    }
    return $output
}

foreach ($tool in @($bash, $make, $gpp, $objdump, $glslc)) {
    if (-not (Test-Path $tool)) { throw "Required MSYS2/UCRT64 tool is missing: $tool" }
}

# Use the documented MSYS2 shell and explicit UCRT64 executables.  Do not rely
# on a Windows PATH injection: callers from WSL do not inherit one.
$drive = $repo.Substring(0, 1).ToLowerInvariant()
$repoMsys = "/$drive/" + $repo.Substring(3).Replace('\', '/')
$tempRoot = [IO.Path]::GetTempPath().TrimEnd('\', '/')
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$tempDrive = $tempRoot.Substring(0, 1).ToLowerInvariant()
$tempMsys = "/$tempDrive/" + $tempRoot.Substring(3).Replace('\', '/')
# Disable only the optional compiler-cache wrapper: this keeps the recorder
# deterministic when a WSL-launched MSYS2 process lacks ccache's user config.
$buildCommand = "set -e; export PATH=/ucrt64/bin:/usr/bin:`$PATH TMPDIR=`"$tempMsys`" TMP=`"$tempMsys`" TEMP=`"$tempMsys`"; cd `"$repoMsys`"; /usr/bin/make -C MatterEngine3 CCACHE=; /usr/bin/make -C MatterEditor windows CCACHE="
& $bash -lc $buildCommand
if ($LASTEXITCODE -ne 0) { throw "MSYS2/UCRT64 baseline build failed ($LASTEXITCODE)" }

if (Get-Command py.exe -ErrorAction SilentlyContinue) {
    & py.exe -3 (Join-Path $repo 'tools\check-source-manifests.py') --root $repo
} else {
    & $bash -lc "cd `"$repoMsys`"; /usr/bin/python tools/check-source-manifests.py --root ."
}
if ($LASTEXITCODE -ne 0) { throw "Source manifest gate failed ($LASTEXITCODE)" }

$engine = Join-Path $repo 'MatterEngine3\build\libmatter_engine3.a'
$editor = Join-Path $repo 'MatterEditor\build\windows\editor.exe'
foreach ($artifact in @($engine, $editor)) {
    if (-not (Test-Path $artifact)) { throw "Baseline artifact was not produced: $artifact" }
}

$vulkanInfoExe = 'C:\VulkanSDK\1.4.357.0\Bin\vulkaninfoSDK.exe'
if (-not (Test-Path $vulkanInfoExe)) { throw "Required pinned Vulkan evidence tool is missing: $vulkanInfoExe" }
$vulkanInfo = (Invoke-RequiredNative $vulkanInfoExe @('--summary') 'vulkaninfoSDK' | Select-Object -First 40) -join "`n"
$compilerVersion = (Invoke-RequiredNative $gpp @('--version') 'g++ version query' | Select-Object -First 1) -join ''
$glslcVersion = (Invoke-RequiredNative $glslc @('--version') 'glslc version query' | Select-Object -First 1) -join ''
$objdumpOutput = Invoke-RequiredNative $objdump @('-p', $editor) 'objdump import query'
$imports = ($objdumpOutput | Select-String 'DLL Name:' | ForEach-Object {
    ($_ -replace '^.*DLL Name:\s*', '').Trim()
})
$features = [ordered]@{
    recorded_at_utc = [DateTime]::UtcNow.ToString('o')
    build = [ordered]@{
        shell = $bash
        make = $make
        compiler = [ordered]@{
            path = $gpp
            version = $compilerVersion
        }
        vulkan = [ordered]@{
            sdk = 'C:\VulkanSDK\1.4.357.0'
            sdk_version = '1.4.357.0'
            glslc_version = $glslcVersion
            summary = $vulkanInfo
        }
    }
    manifests = [ordered]@{
        missing_sources = 0
        duplicate_sources = 0
    }
    artifacts = [ordered]@{
        engine = [ordered]@{
            path = 'MatterEngine3/build/libmatter_engine3.a'
            sha256 = Get-Sha256 $engine
        }
        editor = [ordered]@{
            path = 'MatterEditor/build/windows/editor.exe'
            sha256 = Get-Sha256 $editor
            imports = @($imports)
        }
    }
}

$baselineDir = Join-Path $repo 'MatterEditor\build\baselines\mingw'
New-Item -ItemType Directory -Force -Path $baselineDir | Out-Null
$features | ConvertTo-Json -Depth 8 | Set-Content -Encoding utf8 (Join-Path $baselineDir 'build_features.json')
Write-Host "Recorded MinGW baseline: $(Join-Path $baselineDir 'build_features.json')"
