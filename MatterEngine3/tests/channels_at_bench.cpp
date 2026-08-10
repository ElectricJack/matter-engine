// channels_at_bench.cpp — what does one habitat-tape sample actually cost?
//
// WHY THIS EXISTS. The scatter profiler reports ~22 us per habitat sample and
// three separate attempts to explain that number were wrong:
//   * "it's the JS<->C crossing"  -- fusing the crossing away left it at ~22 us
//   * "it's the channel count"    -- 12 -> 31 channels changed nothing measurable
//   * "it's the register zeroing" -- 4 KB of memset is ~50-100 ns, i.e. ~0.4%
// Each was reasoned from a profiler slot that bundles candidate generation,
// tape evaluation and the JS planner loop together. This times channels_at
// ALONE, and varies the two candidate explanations INDEPENDENTLY so they can
// be told apart:
//
//   op count   -- cheap ALU ops (add/mul/smoothstep), noise held constant
//   octaves    -- fbm work per noise op, op count held constant
//
// If cost tracks octaves and ignores op count, the lever is noise, and neither
// dead-code masking nor channel trimming can help. That is the question.
//
// Build (from MatterEngine3/tests):
//   g++ -O2 -std=c++17 -I../src -I../include -I../../libs/MathLib/include \
//       channels_at_bench.cpp ../build/obj/terrain_field.o -o build/channels_at_bench

#include "terrain_field.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace terrain_field;

namespace {

// A tape with `noise_ops` fbm ops at `octaves` each, then `alu_ops` cheap ops
// chained off them, exposing `channels` channels. Every knob independent.
std::string make_tape(int noise_ops, int octaves, int alu_ops, int channels) {
    std::string t;
    int reg = 0;
    std::vector<int> noise_regs;
    for (int i = 0; i < noise_ops; ++i) {
        // noise2w <seed> <freq> <octaves> <gain> <lac>
        t += "noise2w " + std::to_string(1000 + i) + " 0.004 " +
             std::to_string(octaves) + " 0.5 2.0\n";
        noise_regs.push_back(reg++);
    }
    if (noise_regs.empty()) { t += "const 0.5\n"; noise_regs.push_back(reg++); }
    // Chain cheap ALU ops so they cannot be folded away: each depends on the
    // previous result and on a noise register.
    int last = noise_regs[0];
    for (int i = 0; i < alu_ops; ++i) {
        const int other = noise_regs[i % noise_regs.size()];
        t += "add r" + std::to_string(last) + " r" + std::to_string(other) + "\n";
        last = reg++;
    }
    for (int c = 0; c < channels; ++c) {
        const int src = (c == 0) ? last : noise_regs[c % noise_regs.size()];
        t += "channel " + std::to_string(c) + " r" + std::to_string(src) + "\n";
    }
    return t;
}

// Returns ns/call, or -1 on parse failure (printed).
double bench(const char* label, int noise_ops, int octaves, int alu_ops,
             int channels, int iters) {
    SurfaceProgram prog;
    std::string err;
    const std::string text = make_tape(noise_ops, octaves, alu_ops, channels);
    if (!SurfaceProgram::parse(text, prog, err, TapeMode::Habitat)) {
        std::printf("  %-34s PARSE FAILED: %s\n", label, err.c_str());
        return -1.0;
    }
    const size_t ops = prog.ops.size();
    SurfaceRuntime rt(std::move(prog));

    float out[kMaxHabitatChannels];
    // Vary the sample position so no result can be cached, and consume the
    // output so the whole call cannot be optimized away.
    double sink = 0.0;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iters; ++i) {
        const float x = 100.0f + (float)(i & 1023) * 0.37f;
        const float z = -50.0f + (float)((i >> 3) & 1023) * 0.29f;
        rt.channels_at(x, z, nullptr, out);
        sink += out[0];
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
    std::printf("  %-34s ops=%-4zu  %8.1f ns/call   (sink %.3f)\n",
                label, ops, ns, sink / iters);
    return ns;
}

} // namespace

int main() {
    const int N = 200000;
    std::printf("channels_at microbenchmark (%d iterations each)\n\n", N);

    std::printf("A. OP COUNT, noise held at 1 op x 4 octaves:\n");
    bench("1 noise, 0 alu, 1 ch",        1, 4, 0,   1, N);
    bench("1 noise, 30 alu, 12 ch",      1, 4, 30, 12, N);
    bench("1 noise, 200 alu, 31 ch",     1, 4, 200, 31, N);
    bench("1 noise, 900 alu, 31 ch",     1, 4, 900, 31, N);

    std::printf("\nB. NOISE OPS, octaves held at 4, alu held at 0:\n");
    bench("1 noise x4oct",               1, 4, 0, 1, N);
    bench("4 noise x4oct",               4, 4, 0, 4, N);
    bench("12 noise x4oct",             12, 4, 0, 12, N);
    bench("31 noise x4oct",             31, 4, 0, 31, N);

    std::printf("\nC. OCTAVES, noise ops held at 12:\n");
    bench("12 noise x1oct",             12, 1, 0, 12, N);
    bench("12 noise x2oct",             12, 2, 0, 12, N);
    bench("12 noise x4oct",             12, 4, 0, 12, N);
    bench("12 noise x6oct",             12, 6, 0, 12, N);

    std::printf("\nD. shape closest to StreamMountain's real habitat tape:\n");
    bench("12 noise x4oct + 19 alu, 31ch", 12, 4, 19, 31, N);

    std::printf("\nThe scatter profiler attributes ~22000 ns to one sample.\n"
                "Whatever this benchmark does NOT account for is elsewhere:\n"
                "candidate generation, the JS planner loop, or marshalling.\n");
    return 0;
}
