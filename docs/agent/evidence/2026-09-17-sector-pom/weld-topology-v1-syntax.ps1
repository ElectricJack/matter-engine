param([string]$ResponseFile)
$ErrorActionPreference='Stop'
Import-Module 'D:/shared with desktop/ai/matter-engine-cpp/tools/windows/MatterWindowsToolchain.psm1' -Force
$t = Resolve-MatterWindowsToolchain -RepositoryRoot 'D:/shared with desktop/ai/matter-engine-cpp'
$command = 'call "{0}" -arch=x64 -host_arch=x64 -winsdk={1} -vcvars_ver={2} && cl.exe /nologo /Zs @"{3}"' -f $t.VsDevCmd,$t.WindowsSdkVersion,$t.MsvcToolsVersion,$ResponseFile
& $env:ComSpec /d /s /c $command
exit $LASTEXITCODE
