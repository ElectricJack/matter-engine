"""Run the unchanged 1500 s native wrapper against the CPU Vulkan fallback."""
import importlib.util
import os
from pathlib import Path

EVIDENCE = Path(__file__).resolve().parent
TOOL = Path('/tmp/fleet-rapids-48-epoch18/toolchain/usr')
os.environ.update({
    'DISPLAY': ':0',
    'VK_ICD_FILENAMES': '/usr/share/vulkan/icd.d/lvp_icd.json',
    'MATTER_VK_LAYER_DIR': str(TOOL / 'share/vulkan/explicit_layer.d'),
    'MATTER_VK_LAYER_LIB': str(TOOL / 'lib/x86_64-linux-gnu'),
    'LP_NUM_THREADS': '1', 'MESA_GLTHREAD': 'false',
    'MATTER_PIPELINE_PROFILE': '1',
    'MATTER_VK_STATIC_RESERVE_CLUSTER_MB': '1',
    'MATTER_VK_STATIC_RESERVE_VERTEX_MB': '1',
    'MATTER_VK_STATIC_RESERVE_INDEX_MB': '1',
    'MATTER_VK_CPU_RESERVE_VERTEX_MB': '0',
    'MATTER_VK_CPU_RESERVE_INDEX_MB': '0',
    'MATTER_IMPOSTOR_CELL_PX': '16',
    'MATTER_SMOKE_EXE': '/tmp/keen-quest-34/build/vulkan_smoke_tests',
    'MATTER_SMOKE_LOG_DIR': '/tmp/keen-quest-34/native-v2',
})
os.environ.pop('LD_PRELOAD', None)
os.environ.pop('MATTER_VK_END_FRAME_TRACE', None)
spec = importlib.util.spec_from_file_location(
    'original_rt_wrapper', EVIDENCE.parent /
    '2026-10-20-rt-counter-native/test_rt_counter_native.py')
original = importlib.util.module_from_spec(spec)
spec.loader.exec_module(original)


def test_software_standalone_rt():
    code, text, validation = original.run_mode('rt')
    assert code == 0, f'rt exited {code}'
    assert validation and all(v == '0' for v in validation), validation
    assert 'Vulkan RT available=true' in text
    assert 'multi-LOD GI disabled' in text
    assert 'lighting scenarios SKIPPED' in text
    assert 'Unused RT lighting pipeline creation is also skipped' in text
    assert 'ray_tracing_lighting' not in text
    assert 'ray_tracing_diffuse' not in text
    assert 'ray_tracing_reflection' not in text
    assert 'attempt=3 phase=wait_idle.exit' in text
    assert 'ALL PASS' in text
