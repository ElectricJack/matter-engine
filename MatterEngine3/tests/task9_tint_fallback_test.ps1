$source = Get-Content -Raw (Join-Path $PSScriptRoot '..\src\script_host.cpp')
$regionStart = $source.IndexOf('// Each non-empty region chunk')
$regionEnd = $source.IndexOf('tlas.build(blas);', $regionStart)
if ($regionStart -lt 0 -or $regionEnd -lt 0) {
    throw 'modifier-region direct-triangle build block not found'
}
$regionBuild = $source.Substring($regionStart, $regionEnd - $regionStart)

$protoInit = $regionBuild.IndexOf('TriEx proto{};')
$neutralInit = $regionBuild.IndexOf(
    'proto.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);')
$authoredCopy = $regionBuild.IndexOf(
    'if (!region_e[ri].empty()) proto = region_e[ri][0];')
$ensure = $regionBuild.IndexOf('ensure_triex(done, proto);')

if (!($protoInit -lt $neutralInit -and
      $neutralInit -lt $authoredCopy -and
      $authoredCopy -lt $ensure)) {
    throw 'modifier-region TriEx fallback is not neutral before authored tint override'
}

Write-Output 'Task 9 modifier-region tint fallback regression passed'
