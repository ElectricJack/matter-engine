#!/bin/bash
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
cd /mnt/d/nr17
for t in b m; do
  $PS -NoProfile -NonInteractive -ExecutionPolicy Bypass -File 'D:\nr17\runner\run-windows-build-bounded.ps1' \
    -EvidenceDirectory "D:\\nr17\\e17-$t-build" -SourceRoot "D:\\nr17\\$t" \
    -Config RelWithDebInfo -Target vulkan_smoke_tests < /dev/null > /mnt/d/nr17/e17-$t-build.driver.txt 2>&1
  echo "$t runner exit=$?"
done
