[CmdletBinding()]
param(
    [ValidateSet('Validate', 'Generate', 'Build', 'Run')][string]$Mode = 'Validate',
    [string]$RepositoryRoot,
    [string]$PhysxRoot = $env:MATTER_PHYSX_ROOT,
    [string]$CudaRoot = $env:CUDA_PATH_V12_8,
    [string]$LockPath,
    [ValidateRange(1, 300)][int]$ObservationSeconds = 12
)

$ErrorActionPreference = 'Stop'
if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $PhysxRoot) {
    throw 'PhysX root is required. Pass -PhysxRoot or set MATTER_PHYSX_ROOT.'
}
if (-not $CudaRoot) {
    throw 'CUDA 12.8 root is required. Pass -CudaRoot or set CUDA_PATH_V12_8.'
}
if (-not $LockPath) {
    $LockPath = Join-Path $RepositoryRoot 'tools\deps\physx.lock.json'
}

Import-Module (Join-Path $PSScriptRoot 'MatterPhysxDependency.psm1') -Force
$dependency = Resolve-MatterPhysxDependency `
    -RepositoryRoot $RepositoryRoot `
    -PhysxRoot $PhysxRoot `
    -CudaRoot $CudaRoot `
    -LockPath $LockPath

Write-Output "MATTER_PHYSX_VALIDATE=PASS commit=$($dependency.Commit) sdk=$($dependency.SdkVersion) cuda=$($dependency.CudaVersion)"
if ($Mode -eq 'Validate') {
    return
}

$toolchainModule = Join-Path $RepositoryRoot 'tools\windows\MatterWindowsToolchain.psm1'
Import-Module $toolchainModule -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $RepositoryRoot
$compilerDirectory = Join-Path $dependency.SdkRoot "compiler\$($dependency.WindowsPreset)"

if ($Mode -eq 'Generate') {
    Assert-MatterPhysxGeneratorPath -PhysxRoot $dependency.PhysxRoot | Out-Null
    $savedPath = $env:PATH
    $savedCudaPath = $env:PM_CUDA_PATH
    try {
        $env:PATH = "$(Split-Path -Parent $toolchain.CMake);$savedPath"
        $shortCuda = @(& $env:ComSpec /d /s /c ('for %I in ("{0}") do @echo %~sI' -f $dependency.CudaRoot) 2>&1)
        if ($LASTEXITCODE -ne 0 -or -not $shortCuda) {
            throw "Could not obtain the short CUDA path for $($dependency.CudaRoot)"
        }
        $env:PM_CUDA_PATH = ([string]$shortCuda[0]).Trim()
        Push-Location $dependency.SdkRoot
        try {
            # The upstream generator writes informational CMake output to
            # stderr. Capture it without allowing Windows PowerShell's Stop
            # preference to turn a successful native process into an error.
            $savedGeneratorErrorAction = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Continue'
                $output = @(& (Join-Path $dependency.SdkRoot 'generate_projects.bat') $dependency.WindowsPreset 2>&1)
                $generatorExitCode = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $savedGeneratorErrorAction
            }
            if ($generatorExitCode -ne 0) {
                throw "PhysX project generation failed:`n$($output -join "`n")"
            }
        } finally {
            Pop-Location
        }
    } finally {
        $env:PATH = $savedPath
        $env:PM_CUDA_PATH = $savedCudaPath
    }
    $solution = Join-Path $compilerDirectory 'PhysXSDK.sln'
    if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
        throw "PhysX generator did not produce $solution"
    }
    Write-Output "MATTER_PHYSX_GENERATE=PASS solution=$solution"
    return
}

if (-not (Test-Path -LiteralPath $compilerDirectory -PathType Container)) {
    throw "PhysX compiler directory does not exist; run -Mode Generate first: $compilerDirectory"
}

if ($Mode -eq 'Build') {
    $output = @(& $toolchain.CMake --build $compilerDirectory --config release --target SnippetPBF 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "PhysX SnippetPBF build failed:`n$($output -join "`n")"
    }
    foreach ($required in @($dependency.ControlExecutable, $dependency.GpuRuntime)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "PhysX control build omitted required output: $required"
        }
    }
    Write-Output "MATTER_PHYSX_BUILD=PASS executable=$($dependency.ControlExecutable)"
    return
}

foreach ($required in @($dependency.ControlExecutable, $dependency.GpuRuntime)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "PhysX control runtime is missing; run -Mode Build first: $required"
    }
}

$process = $null
try {
    $process = Start-Process -FilePath $dependency.ControlExecutable `
        -WorkingDirectory $dependency.BinaryDirectory -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($ObservationSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $process.Refresh()
        if ($process.HasExited) {
            throw "SnippetPBF exited before the $ObservationSeconds-second observation window (exit $($process.ExitCode))"
        }
    }
    $gpu = @(& nvidia-smi.exe --query-gpu=name,driver_version,utilization.gpu,memory.used --format=csv,noheader,nounits 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "nvidia-smi failed while observing SnippetPBF:`n$($gpu -join "`n")"
    }
    Write-Output "MATTER_PHYSX_RUN=PASS seconds=$ObservationSeconds gpu=$($gpu -join '; ')"
} finally {
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id
            $process.WaitForExit()
        }
        $process.Dispose()
    }
}
