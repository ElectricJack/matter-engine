#include "perf_gpu_stats.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

static void contains(const std::string& json, const std::string& expected) {
    if (json.find(expected) == std::string::npos) {
        std::cerr << "Missing: " << expected << "\nActual: " << json << '\n';
        std::exit(1);
    }
}
int main() {
    viewer::PerfGpuStats stats;
    matter::GpuTimingSample sample;
    sample.sequence = 8;
    sample.valid_mask = (1u << 0) | (1u << 20);
    sample.milliseconds[0] = 999;
    stats.reset(sample.sequence); // exclude retained pre-window observation
    stats.add(sample);
    for (int i = 1; i <= 20; ++i) {
        ++sample.sequence;
        sample.milliseconds[0] = static_cast<float>(i);
        sample.milliseconds[20] = i == 20 ? 100.0f : 0.0f;
        stats.add(sample);
        stats.add(sample); // held readback is not another frame
    }
    auto old_sample = sample;
    --old_sample.sequence;
    old_sample.milliseconds[0] = 8000;
    stats.add(old_sample); // an older observation cannot add a second sample
    ++sample.sequence;
    sample.valid_mask = 0;
    sample.milliseconds[0] = 5000;
    stats.add(sample); // unavailable results do not bias median or count
    ++sample.sequence;
    sample.valid_mask = 3;
    sample.milliseconds[0] = std::numeric_limits<float>::quiet_NaN();
    sample.milliseconds[1] = -1;
    stats.add(sample);
    std::ostringstream output;
    stats.append_json(output);
    contains(output.str(), "\"p95_method\":\"nearest_rank\",\"p99_method\":\"nearest_rank\"");
    contains(output.str(), "\"total\":{\"samples\":20,\"median_ms\":10.5,\"p95_ms\":19,\"p99_ms\":20,\"max_ms\":20}");
    contains(output.str(), "\"rt_local_direct\":{\"samples\":20,\"median_ms\":0,\"p95_ms\":0,\"p99_ms\":100,\"max_ms\":100}");
    contains(output.str(), "\"cull\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    contains(output.str(), "\"atmosphere\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    stats.reset();
    sample = {};
    sample.sequence = 1;
    sample.valid_mask = 1;
    sample.milliseconds[0] = 7;
    stats.add(sample);
    std::ostringstream single;
    stats.append_json(single);
    contains(single.str(), "\"total\":{\"samples\":1,\"median_ms\":7,\"p95_ms\":7,\"p99_ms\":7,\"max_ms\":7}");
    // One combined GI frame followed by two split-resolution frames. Children
    // must aggregate only actual dispatches, including legitimate zero time;
    // held/stale values from a combined frame must not become child samples.
    stats.reset();
    sample = {};
    sample.sequence = 1;
    sample.valid_mask = (1u << 8) | (1u << 11) | (1u << 21);
    sample.milliseconds[8] = 0.5f; // legacy final display transform
    sample.milliseconds[11] = 12;
    sample.milliseconds[21] = 4; // independent HDR lighting reconstruction
    sample.milliseconds[22] = 900;
    sample.milliseconds[23] = 900;
    stats.add(sample);
    std::ostringstream combined;
    stats.append_json(combined);
    contains(combined.str(), "\"rt_gi_diffuse\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    contains(combined.str(), "\"rt_gi_reflection_transmission\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    contains(combined.str(), "\"primary_light_cull\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    ++sample.sequence;
    sample.valid_mask |= (1u << 22) | (1u << 23) | (1u << 24);
    sample.milliseconds[11] = 10;
    sample.milliseconds[21] = 6;
    sample.milliseconds[22] = 0;
    sample.milliseconds[23] = 8;
    sample.milliseconds[24] = 0.125f;
    stats.add(sample);
    stats.add(sample);
    ++sample.sequence;
    sample.milliseconds[11] = 14;
    sample.milliseconds[21] = 8;
    sample.milliseconds[22] = 4;
    sample.milliseconds[23] = 10;
    sample.milliseconds[24] = 0.375f;
    stats.add(sample);
    // A partial/unavailable child pair cannot invent an execution.
    ++sample.sequence;
    sample.valid_mask = (1u << 21);
    sample.milliseconds[21] = std::numeric_limits<float>::infinity();
    stats.add(sample);
    std::ostringstream lighting;
    stats.append_json(lighting);
    contains(lighting.str(), "\"composite\":{\"samples\":3,\"median_ms\":0.5,\"p95_ms\":0.5,\"p99_ms\":0.5,\"max_ms\":0.5}");
    contains(lighting.str(), "\"rt_gi\":{\"samples\":3,\"median_ms\":12,\"p95_ms\":14,\"p99_ms\":14,\"max_ms\":14}");
    contains(lighting.str(), "\"hdr_lighting\":{\"samples\":3,\"median_ms\":6,\"p95_ms\":8,\"p99_ms\":8,\"max_ms\":8}");
    contains(lighting.str(), "\"rt_gi_diffuse\":{\"samples\":2,\"median_ms\":2,\"p95_ms\":4,\"p99_ms\":4,\"max_ms\":4}");
    contains(lighting.str(), "\"rt_gi_reflection_transmission\":{\"samples\":2,\"median_ms\":9,\"p95_ms\":10,\"p99_ms\":10,\"max_ms\":10}");
    contains(lighting.str(), "\"primary_light_cull\":{\"samples\":2,\"median_ms\":0.25,\"p95_ms\":0.375,\"p99_ms\":0.375,\"max_ms\":0.375}");
    stats.reset(sample.sequence);
    stats.add(sample);
    std::ostringstream reset_lighting;
    stats.append_json(reset_lighting);
    contains(reset_lighting.str(), "\"hdr_lighting\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    contains(reset_lighting.str(), "\"rt_gi_diffuse\":{\"samples\":0,\"median_ms\":null,\"p95_ms\":null,\"p99_ms\":null,\"max_ms\":null}");
    std::cout << "PASS raw GPU timing percentiles (median/p95/p99/max), validity, duplicates, reset, lighting dispatches\n";
}
