"""Research probes: exercise the checked-out counter reporter without a GPU."""
from pathlib import Path
import subprocess

ROOT = Path('/tmp/noble-zenith-31-epoch3/pristine')


def test_actual_reporter_with_poison_equal_and_stale_slots(tmp_path):
    source = (ROOT / "libs/MatterGraphics/src/native/rt_dispatch.inl").read_text()
    start = source.index("void VkSceneRenderer::report_rt_trace_counters(")
    end = source.index("\n// Turns the selected geometry", start)
    reporter = source[start:end]
    contract = (ROOT / "libs/MatterGraphics/src/native/rt_contract.inl").read_text()
    counters = contract[:contract.index("\nstruct alignas")]
    prefix = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
constexpr int VK_NULL_HANDLE = 0;
static unsigned warnings = 0;
#define MATTER_LOGW(...) (++warnings)
#define MATTER_LOGI(...) ((void)0)
struct Buffer { int buffer = 1; void* mapped = nullptr; };
namespace matter {
bool map_buffer(Buffer& b, std::string&) { return b.mapped != nullptr; }
bool invalidate_buffer(Buffer&, size_t, size_t, std::string&) { return true; }
}
struct FrameResources {
    Buffer rt_error_counter, rt_test_output;
    bool primary_light_audit_pending = false;
    bool rt_adaptive_diagnostics_pending = false;
};
struct VkSceneRenderer {
    std::array<FrameResources, 2> frames_{};
    bool primary_light_audit_ = false, rt_adaptive_diagnostics_ = false;
    uint64_t primary_light_audit_frames_ = 0, rt_adaptive_diagnostic_frames_ = 0;
    uint32_t rt_invalid_part_records_last_ = 0;
    uint64_t rt_invalid_part_records_total_ = 0;
    void report_rt_trace_counters(uint32_t);
};
"""
    main = r"""
int main() {
    VkSceneRenderer r;
    std::array<GpuRtCounters, 2> memory{};
    for (unsigned s = 0; s < 2; ++s)
        r.frames_[s].rt_error_counter.mapped = &memory[s];
    // Model allocated slots never written by RT. The values are injected,
    // not claimed to be actual allocator output on this machine.
    memory[0].invalid_part_records = 4234134786u;
    memory[1].invalid_part_records = 1082262592u;
    for (unsigned i = 0; i < 6; ++i) r.report_rt_trace_counters(i % 2);
    assert(warnings == 6);
    assert(r.rt_invalid_part_records_total_ == 15949192134ull);
    std::printf("poison: warnings=%u total=%llu\n", warnings,
                (unsigned long long)r.rt_invalid_part_records_total_);
    // Equal values from two distinct legitimate samples should sum to 14.
    warnings = 0;
    r.rt_invalid_part_records_last_ = 0;
    r.rt_invalid_part_records_total_ = 0;
    memory[0].invalid_part_records = memory[1].invalid_part_records = 7;
    r.report_rt_trace_counters(0);
    r.report_rt_trace_counters(1);
    assert(warnings == 1 && r.rt_invalid_part_records_total_ == 7);
    std::printf("equal: warnings=%u total=%llu (two samples would sum to 14)\n",
                warnings, (unsigned long long)r.rt_invalid_part_records_total_);
    // Distinct old slot values get counted repeatedly without a new writer.
    warnings = 0;
    r.rt_invalid_part_records_last_ = 0;
    r.rt_invalid_part_records_total_ = 0;
    memory[0].invalid_part_records = 2;
    memory[1].invalid_part_records = 3;
    for (unsigned i = 0; i < 6; ++i) r.report_rt_trace_counters(i % 2);
    assert(warnings == 6 && r.rt_invalid_part_records_total_ == 15);
    std::printf("stale: warnings=%u total=%llu (one sample per slot sums to 5)\n",
                warnings, (unsigned long long)r.rt_invalid_part_records_total_);
}
"""
    cpp = tmp_path / "counter_probe.cpp"
    exe = tmp_path / "counter_probe"
    cpp.write_text(prefix + counters + reporter + main)
    subprocess.run(
        ["g++", "-std=c++17", "-O0", "-Wall", "-Wextra", str(cpp), "-o", str(exe)],
        check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    print(result.stdout)
    assert "poison: warnings=6 total=15949192134" in result.stdout
    assert "equal: warnings=1 total=7" in result.stdout
    assert "stale: warnings=6 total=15" in result.stdout

