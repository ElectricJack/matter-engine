[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Import-Module (Join-Path $repositoryRoot 'tools/windows/MatterWindowsToolchain.psm1') -Force
$toolchain = Resolve-MatterWindowsToolchain -RepositoryRoot $repositoryRoot
$testEnvironment = Join-Path $PSScriptRoot '../build/test-python'
& $toolchain.Python -m venv $testEnvironment
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$testInterpreter = Join-Path $testEnvironment 'Scripts/python.exe'
& $testInterpreter -m pip install --disable-pip-version-check -r (Join-Path $PSScriptRoot 'requirements.txt')
exit $LASTEXITCODE
