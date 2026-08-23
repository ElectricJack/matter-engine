[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$CMakePath
)

$ErrorActionPreference = 'Stop'
$probe = Join-Path $RepositoryRoot 'cmake\tests\package_project_name_probe.cmake'

function Invoke-Probe([string]$ProjectName) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $CMakePath "-DRepositoryRoot=$RepositoryRoot" "-DProjectName=$ProjectName" -P $probe 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output -join "`n" }
}

foreach ($name in @('world_demo', 'ravine-fluid-test', 'project_2.test')) {
    $result = Invoke-Probe $name
    if ($result.ExitCode -ne 0) { throw "safe project name '$name' was rejected:`n$($result.Output)" }
}

foreach ($name in @(
    '', '.', '..', '../escape', '..\escape', 'C:escape', 'two/parts',
    'two\parts', 'white space', 'world_demo.', 'world_demo..', 'CON',
    'con.txt', 'PRN', 'AUX.log', 'NUL', 'COM1', 'com9.data', 'LPT1',
    'lpt9.cache'
)) {
    $result = Invoke-Probe $name
    if ($result.ExitCode -eq 0) { throw "unsafe project name '$name' was accepted" }
    if ($result.Output -notmatch 'unsafe MATTER_DIST_PROJECT') {
        throw "unsafe project name '$name' did not produce the safety diagnostic:`n$($result.Output)"
    }
}

Write-Output 'CMake package project-name safety: PASS'
