"""Regression for tools/run-windows-build-bounded.ps1 (native wrapper interruption).

The static checks run anywhere. The native checks need WSL interop and a
scratch directory on a mounted Windows drive (the runner refuses UNC roots):

    MATTER_WINDOWS_SCRATCH=/mnt/d/scratch python3 tools/tests/test_windows_bounded_runner.py
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

RUNNER = Path(__file__).resolve().parents[1] / 'run-windows-build-bounded.ps1'
POWERSHELL = os.environ.get('MATTER_WINDOWS_POWERSHELL',
                            '/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe')
SCRATCH = os.environ.get('MATTER_WINDOWS_SCRATCH')

# Stands in for tools/build-windows.ps1: reports the console it was given and
# nests cmd.exe the way the real wrapper does.
STUB = r'''param([string]$Mode, [string]$Label)
Add-Type -Namespace MatterProbe -Name Console -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
'@
"console_window=$([MatterProbe.Console]::GetConsoleWindow().ToInt64())"
"parallel=$env:CMAKE_BUILD_PARALLEL_LEVEL"
"label=$Label"
[Console]::Error.WriteLine('stub-stderr')
if ($Mode -eq 'sleep') { & $env:ComSpec /d /s /c "ping -n 120 127.0.0.1 >nul" }
& $env:ComSpec /d /s /c "exit 3"
exit $LASTEXITCODE
'''


def windows_path(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


class StaticContract(unittest.TestCase):
    def test_build_child_has_no_console_window(self):
        text = RUNNER.read_text(encoding='utf-8')
        code = '\n'.join(line for line in text.splitlines() if not line.lstrip().startswith('#'))
        self.assertIn('$startInfo.UseShellExecute = $false', code)
        self.assertIn('$startInfo.CreateNoWindow = $true', code)
        self.assertIn('$startInfo.RedirectStandardInput = $true', code)
        # Start-Process without -NoNewWindow is the CREATE_NEW_CONSOLE launch
        # that left the 2026-10 native builds in a closable desktop window.
        self.assertNotIn('Start-Process', code)


@unittest.skipUnless(SCRATCH and Path(POWERSHELL).exists() and
                     Path('/proc/sys/fs/binfmt_misc/WSLInterop').exists(),
                     'needs WSL interop and MATTER_WINDOWS_SCRATCH on a mounted Windows drive')
class NativeRunner(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='bounded-runner-', dir=SCRATCH))
        self.addCleanup(shutil.rmtree, self.root, True)
        (self.root / 'src/tools').mkdir(parents=True)
        (self.root / 'src/tools/build-windows.ps1').write_text(STUB)
        shutil.copyfile(RUNNER, self.root / 'run-windows-build-bounded.ps1')

    def run_runner(self, evidence, *arguments, timeout_seconds=600):
        command = [POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                   windows_path(self.root / 'run-windows-build-bounded.ps1'),
                   '-SourceRoot', windows_path(self.root / 'src'),
                   '-EvidenceDirectory', windows_path(self.root) + '\\' + evidence,
                   '-TimeoutSeconds', str(timeout_seconds), *arguments]
        return subprocess.run(command, capture_output=True, text=True, timeout=180)

    def receipt(self, evidence):
        return json.loads((self.root / evidence / 'build-receipt.json').read_text(encoding='utf-8-sig'))

    def test_windowless_console_exit_code_and_streams(self):
        completed = self.run_runner('e1', '-Mode', 'exit', '-Label', 'two words')
        self.assertEqual(completed.returncode, 3, completed.stdout + completed.stderr)
        receipt = self.receipt('e1')
        self.assertEqual((receipt['status'], receipt['exit_code']), ('failed', 3))
        self.assertEqual(receipt['console'], 'CreateNoWindow')
        stdout = (self.root / 'e1/build.stdout.txt').read_text()
        self.assertIn('console_window=0\n', stdout.replace('\r\n', '\n'))
        self.assertIn('parallel=6', stdout)
        self.assertIn('label=two words', stdout)
        self.assertIn('stub-stderr', (self.root / 'e1/build.stderr.txt').read_text())

        # Existing evidence is preserved, never overwritten by a rerun.
        before = (self.root / 'e1/build-receipt.json').read_bytes()
        self.assertNotEqual(self.run_runner('e1', '-Mode', 'exit').returncode, 0)
        self.assertEqual((self.root / 'e1/build-receipt.json').read_bytes(), before)

    def test_timeout_kills_tree(self):
        completed = self.run_runner('e2', '-Mode', 'sleep', timeout_seconds=3)
        self.assertEqual(completed.returncode, 124, completed.stdout + completed.stderr)
        receipt = self.receipt('e2')
        self.assertEqual((receipt['status'], receipt['exit_code']), ('timeout', None))
        self.assertLess(receipt['elapsed_seconds'], 60)
        self.assertIn('SUCCESS', (self.root / 'e2/timeout-kill.txt').read_text())


if __name__ == '__main__':
    unittest.main()
