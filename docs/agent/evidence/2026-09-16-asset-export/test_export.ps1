[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Root,
      [Parameter(Mandatory=$true)][string]$World,
      [Parameter(Mandatory=$true)][string]$Module,
      [Parameter(Mandatory=$true)][string]$Prefix,
      [string]$WorkbenchModule,
      [string]$PartHash)
$ErrorActionPreference='Stop'
$rootPath=(Resolve-Path -LiteralPath $Root).Path
$out=Join-Path $rootPath ('validation/'+$Prefix)
if(Test-Path -LiteralPath $out){throw 'Choose a fresh validation prefix'}
[void](New-Item -ItemType Directory -Path $out)
$commands=Join-Path $out 'commands.txt';$results=Join-Path $out 'results.jsonl'
$destination=Join-Path $out 'asset'
$lines=@('wait_event bake.finished 300','wait_idle 5','wait_frames 30')
$requests=@(
    @{version=1;request_id='export';command='asset.export';args=@{source='world';module=$Module;directory=$destination;lod=0};timeout_ms=30000},
    @{version=1;request_id='existing';command='asset.export';args=@{source='world';module=$Module;directory=$destination;lod=0};timeout_ms=30000},
    @{version=1;request_id='bad-lod';command='asset.export';args=@{source='world';module=$Module;directory=($destination+'-bad');lod=99};timeout_ms=30000}
)
if($PartHash){$requests+=@{version=1;request_id='hash-export';command='asset.export';args=@{source='world';part_hash=$PartHash;directory=($destination+'-hash');lod=0};timeout_ms=30000}}
foreach($request in $requests){$lines+=('agent '+($request|ConvertTo-Json -Depth 10 -Compress));$lines+='wait_frames 1'}
$capture=@{version=1;request_id='capture';command='viewport.capture';args=@{path=(Join-Path $out 'engine.png')};timeout_ms=30000}
$lines+=('agent '+($capture|ConvertTo-Json -Depth 10 -Compress));$lines+='wait_frames 5'
if($WorkbenchModule){
    $lines+=('workbench '+$WorkbenchModule);$lines+='wait_frames 300';$lines+='wait_idle 10';
    $request=@{version=1;request_id='workbench';command='asset.export';args=@{source='workbench';module=$WorkbenchModule;directory=($destination+'-workbench');lod=0};timeout_ms=30000}
    $lines+=('agent '+($request|ConvertTo-Json -Depth 10 -Compress));$lines+='wait_frames 5'
    $capture.request_id='workbench-capture';$capture.args.path=Join-Path $out 'workbench.png'
    $lines+=('agent '+($capture|ConvertTo-Json -Depth 10 -Compress));$lines+='wait_frames 5'
}
$lines+='quit'
# Geometry completion can precede the deferred tileset bake. Try the export,
# retry only its explicit pre-write readiness failure, and queue the rest of
# the checks only after a success. Every attempt has a distinct protocol ID.
$tailLines=@($lines|Select-Object -Skip 5)
$requests[0].request_id='export-attempt-0'
$initial=@($lines|Select-Object -First 3)+@(('agent '+($requests[0]|ConvertTo-Json -Depth 10 -Compress)),'wait_frames 1')
[IO.File]::WriteAllLines($commands,$initial,[Text.UTF8Encoding]::new($false))
$attempt=0;$pendingRequestId='export-attempt-0';$completedRequestId='';$tailSent=$false
$info=New-Object Diagnostics.ProcessStartInfo
$info.FileName=Join-Path $rootPath 'editor.exe';$info.WorkingDirectory=$rootPath
$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
foreach($key in @($info.EnvironmentVariables.Keys)){if($key -like 'MATTER_*'){$info.EnvironmentVariables.Remove($key)}}
$info.EnvironmentVariables['PATH']="$env:SystemRoot\System32"
$info.EnvironmentVariables['TMP']="$env:LOCALAPPDATA\Temp";$info.EnvironmentVariables['TEMP']="$env:LOCALAPPDATA\Temp"
$info.EnvironmentVariables['MATTER_WORLD']=$World
$info.EnvironmentVariables['MATTER_CMD_FIFO']=$commands;$info.EnvironmentVariables['MATTER_AGENT_RESULT_FILE']=$results
$info.EnvironmentVariables['MATTER_HIDE_WINDOW']='1';if(-not $WorkbenchModule){$info.EnvironmentVariables['MATTER_HIDE_UI']='1'}
$info.EnvironmentVariables['MATTER_WINDOW_WIDTH']='1200';$info.EnvironmentVariables['MATTER_WINDOW_HEIGHT']='900'
$info.EnvironmentVariables['MATTER_VT_PROP_TEXELS_PER_METER']='512';$info.EnvironmentVariables['MATTER_VK_VALIDATION']='1'
$info.EnvironmentVariables['MATTER_VT_CHART_LOG']='1';$info.EnvironmentVariables['MATTER_ISSUE_DIR']=Join-Path $rootPath 'issues'
$process=New-Object Diagnostics.Process;$process.StartInfo=$info
if(-not $process.Start()){throw 'Cannot launch export validation'}
$process.Id|Set-Content -LiteralPath (Join-Path $out 'pid.txt')
Write-Output "Export validation process $($process.Id): $World"
$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$timer=[Diagnostics.Stopwatch]::StartNew()
while(-not $process.WaitForExit(1000)){
    if($timer.Elapsed.TotalSeconds -gt 600){$process.Kill();$process.WaitForExit();break}
    if(-not $tailSent -and (Test-Path -LiteralPath $results)){
        $data=[IO.File]::ReadAllText($results)
        $terminal=$null
        foreach($line in ($data -split "`n"|Select-Object -SkipLast 1)){
            if($line.Trim().Length){$row=$line|ConvertFrom-Json;if($row.request_id -eq $pendingRequestId){$terminal=$row}}
        }
        if($terminal){
            if($terminal.ok){
                $completedRequestId=$pendingRequestId;$tailSent=$true
                [IO.File]::AppendAllLines($commands,[string[]]$tailLines,[Text.UTF8Encoding]::new($false))
            }elseif($terminal.message -eq 'asset export requires a completed bake and idle publication queue' -or
                    $terminal.message -eq 'asset detail tileset is not loaded; wait for bake completion'){
                $attempt++;$pendingRequestId='export-attempt-'+$attempt;$requests[0].request_id=$pendingRequestId
                $retry=@('wait_frames 30',('agent '+($requests[0]|ConvertTo-Json -Depth 10 -Compress)),'wait_frames 1')
                [IO.File]::AppendAllLines($commands,[string[]]$retry,[Text.UTF8Encoding]::new($false))
            }else{
                $tailSent=$true;[IO.File]::AppendAllLines($commands,[string[]]@('quit'),[Text.UTF8Encoding]::new($false))
            }
        }
    }
}
$log=$stdout.Result+"`n"+$stderr.Result
[IO.File]::WriteAllText((Join-Path $out 'engine.log'),$log)
if($process.ExitCode -ne 0){throw "Editor exit $($process.ExitCode); inspect engine.log"}
if($log -match '(?i)Vulkan validation ERROR|VUID-|device lost|bake failed|FATAL:'){throw 'Export validation reported an engine failure'}
$rows=@(Get-Content -LiteralPath $results|ForEach-Object{$_|ConvertFrom-Json})
$rows|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $out 'results.json')
$ok=@($rows|Where-Object{$_.request_id -eq $completedRequestId})
if($ok.Count -ne 1 -or -not $ok[0].ok){throw 'Asset export did not succeed; inspect results.json'}
foreach($id in @('existing','bad-lod')){if(@($rows|Where-Object{$_.request_id -eq $id -and -not $_.ok}).Count -ne 1){throw "Negative check failed: $id"}}
foreach($id in @('capture')+$(if($WorkbenchModule){@('workbench','workbench-capture')}else{@()})+$(if($PartHash){@('hash-export')}else{@()})){if(@($rows|Where-Object{$_.request_id -eq $id -and $_.ok}).Count -ne 1){throw "Additional check failed: $id"}}
if(-not (Test-Path -LiteralPath (Join-Path $out 'engine.png'))){throw 'Missing engine screenshot'}
if(-not (Test-Path -LiteralPath (Join-Path $destination 'asset.glb'))){throw 'Missing GLB'}
if(Test-Path -LiteralPath ($destination+'-bad')){throw 'Failed export published output'}
[ordered]@{world=$World;module=$Module;export_request_id=$completedRequestId;readiness_retries=$attempt;seconds=$timer.Elapsed.TotalSeconds;exit=$process.ExitCode;zero_validation_errors=$true;clean_path=$true;editor_sha256=(Get-FileHash -LiteralPath $info.FileName -Algorithm SHA256).Hash.ToLowerInvariant()}|
    ConvertTo-Json|Set-Content -LiteralPath (Join-Path $out 'run.json')
Write-Output "Export validation passed: $destination"
