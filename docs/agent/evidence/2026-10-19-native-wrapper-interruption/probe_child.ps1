param([Parameter(Mandatory = $true)][string]$Out)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace AzlProbe -Name Con -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
[DllImport("kernel32.dll")] public static extern uint GetConsoleProcessList(uint[] list, uint count);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'@
$list = New-Object 'uint32[]' 64
$count = [AzlProbe.Con]::GetConsoleProcessList($list, 64)
$hwnd = [AzlProbe.Con]::GetConsoleWindow()
$cls = New-Object System.Text.StringBuilder 256
if ($hwnd -ne [IntPtr]::Zero) { $null = [AzlProbe.Con]::GetClassName($hwnd, $cls, 256) }
$self = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
[ordered]@{
    pid = $PID
    parent_pid = $self.ParentProcessId
    console_window = $hwnd.ToInt64()
    console_window_visible = [AzlProbe.Con]::IsWindowVisible($hwnd)
    console_window_class = $cls.ToString()
    console_process_ids = @($list[0..([int]$count - 1)] | ForEach-Object { [int]$_ })
    console_processes = @($list[0..([int]$count - 1)] | ForEach-Object {
        $p = Get-CimInstance Win32_Process -Filter "ProcessId=$_"
        if ($p) { '{0}:{1}' -f $p.ProcessId, $p.Name } })
} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Out
