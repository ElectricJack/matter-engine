[CmdletBinding()]
param(
    [string]$RepositoryRoot
)

$ErrorActionPreference = 'Stop'

if (-not $RepositoryRoot) {
    $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}

$modulePath = Join-Path $PSScriptRoot 'windows\MatterWindowsToolchain.psm1'
Import-Module $modulePath -Force
Resolve-MatterWindowsToolchain -RepositoryRoot $RepositoryRoot -Json
