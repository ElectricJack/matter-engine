#!/bin/bash
# usage: smoke.sh <b|m16> <vt|vt-rt> <evidence-name>
src=$1 mode=$2 ev=$3
cd /mnt/d/mv15-78a4
setsid nohup /mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'D:\mv15-78a4\run_native_smoke.ps1' -SourceRoot "D:\\mv15-78a4\\$src" -EvidenceDirectory "D:\\mv15-78a4\\$ev" -ToolchainReceipt 'D:\mv15-78a4\e16-baseline-build\toolchain.json' -Mode $mode < /dev/null > /tmp/claude-1000/e16/$ev-driver.txt 2>&1
for i in $(seq 1 300); do
  grep -q '"status":  "running"' /mnt/d/mv15-78a4/$ev/smoke-receipt.json 2>/dev/null || { [ -f /mnt/d/mv15-78a4/$ev/smoke-receipt.json ] && break; }
  sleep 2
done
cat /mnt/d/mv15-78a4/$ev/smoke-receipt.json
