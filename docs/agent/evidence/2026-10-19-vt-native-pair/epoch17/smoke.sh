#!/bin/bash
# usage: smoke.sh <b|m> <vt|vt-rt> <evidence-name>
src=$1 mode=$2 ev=$3
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass \
  -File 'D:\nr17\runner\run_native_smoke_windowless.ps1' -SourceRoot "D:\\nr17\\$src" \
  -EvidenceDirectory "D:\\nr17\\$ev" -VulkanSdk 'C:\VulkanSDK\1.4.357.0' -Mode $mode < /dev/null > /mnt/d/nr17/$ev.driver.txt 2>&1
echo "$ev driver exit=$?"
