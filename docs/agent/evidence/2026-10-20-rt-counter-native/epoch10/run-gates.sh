#!/usr/bin/env bash
# Execute once only under the epoch10 authorization; output directory must be new.
set -eu
exec env -u LD_PRELOAD -u MATTER_VK_END_FRAME_TRACE \
  DISPLAY=:0 VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  MATTER_VK_LAYER_DIR=/tmp/fleet-rapids-48-epoch18/toolchain/usr/share/vulkan/explicit_layer.d \
  MATTER_VK_LAYER_LIB=/tmp/fleet-rapids-48-epoch18/toolchain/usr/lib/x86_64-linux-gnu \
  LP_NUM_THREADS=1 MESA_GLTHREAD=false MATTER_PIPELINE_PROFILE=1 \
  MATTER_VK_STATIC_RESERVE_CLUSTER_MB=1 MATTER_VK_STATIC_RESERVE_VERTEX_MB=1 \
  MATTER_VK_STATIC_RESERVE_INDEX_MB=1 MATTER_VK_CPU_RESERVE_VERTEX_MB=0 \
  MATTER_VK_CPU_RESERVE_INDEX_MB=0 MATTER_IMPOSTOR_CELL_PX=16 \
  MATTER_SMOKE_EXE=/tmp/noble-zenith-31-epoch10/vulkan_smoke_tests \
  MATTER_SMOKE_LOG_DIR=/tmp/noble-zenith-31-epoch10/gates \
  aq test --aq-timeout 75 -n 0 \
  docs/agent/evidence/2026-10-20-rt-counter-native/test_rt_counter_native.py -q -s
