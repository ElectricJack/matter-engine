[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$AuditScript,
    [Parameter(Mandatory = $true)] [string]$Executable,
    [Parameter(Mandatory = $true)] [string]$BuildDirectory,
    [Parameter(Mandatory = $true)] [string]$Ninja,
    [Parameter(Mandatory = $true)] [string]$AutoremesherLibrary
)

$output = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $AuditScript `
    -Executable $Executable -BuildDirectory $BuildDirectory -Ninja $Ninja `
    -AutoremesherLibrary $AutoremesherLibrary `
    -InputGraphFixture 'C:/review-fixture/response-hidden.a' 2>&1 | Out-String)
if ($LASTEXITCODE -eq 0) {
    throw 'Binary audit accepted a graph-hidden .a fixture'
}
if ($output -notmatch 'Non-MSVC input found in smoke input graph') {
    throw "Binary audit failed for the wrong reason:`n$output"
}
Write-Output 'matter_third_party_audit_rejects_graph_archive: clean'
