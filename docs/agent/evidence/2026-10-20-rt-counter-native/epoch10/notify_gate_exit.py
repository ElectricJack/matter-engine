"""One event-driven completion notification; never polls or starts a test."""
import os
from pathlib import Path
import select
import subprocess

pid = 3003594
thread = 'noble-zenith-31-epoch10-gates-completion'
session = '4024ba4b-275c-4337-93d2-ede50a372e85'
fd = os.pidfd_open(pid)
cmd = Path(f'/proc/{pid}/cmdline').read_bytes()
assert b'/home/jkern/.local/bin/aq\0test\0' in cmd
assert b'test_rt_counter_native.py' in cmd
Path('/tmp/noble-zenith-31-epoch10/gate-observer-ready.txt').write_text(
    f'pid={pid}\nthread={thread}\npidfd acquired; one blocking select,3100s bound\n')
ready, _, _ = select.select([fd], [], [], 3100)
os.close(fd)
state = 'exited' if ready else 'observer timeout; producer status unknown'
body = (f'Epoch10 original gate launcher PID{pid} {state}. '
        'Retrieve foreground exec36325 and /tmp/noble-zenith-31-epoch10/gates-pytest.log. '
        'This is a completion notification, not a test verdict. Do not resubmit.')
subprocess.run(['aq', 'message', 'send', '--to', 'session:' + session,
                '--from-kind', 'session', '--from-id', session,
                '--thread-id', thread, '--body', body, '--json'], check=True)
