#include <matter/geometry_c.h>
#include <matter/contracts/contracts.h>
#include "requests.h"
#include "../../../tests/fuzz/support.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
namespace d = matter::contracts;
namespace f = matter::fuzz;
using V = d::Value;
template<class T> matter_abi_header header() { return {sizeof(T), MATTER_ABI_MAJOR, MATTER_ABI_MINOR}; }
matter_bytes_view bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t *>(text.data()), text.size()};
}
V &field(V &value, const char *name) { return std::get<V::Object>(value.data).at(name); }
std::string encode(const V &value) {
    std::string wire;
    f::check(d::canonical_json(value, wire).ok(), "property fixture encodes");
    return wire;
}
V parse(std::string_view wire) {
    V value; f::check(d::parse_json(wire, value).ok(), "published/diagnostic wire is strict JSON"); return value;
}
std::string epoch_hex(uint64_t epoch) {
    char out[17]; std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(epoch)); return out;
}
struct Instance {
    matter_geometry_api api{};
    matter_budget budget{};
    matter_job_envelope job{header<matter_job_envelope>(), bytes("property.request"), bytes("revision.seeded"), 0};
    explicit Instance(uint64_t epoch) {
        matter_geometry_config config{}; config.header = header<matter_geometry_config>();
        f::check(matter_geometry_default_config(&config) == MATTER_OK, "config defaults");
        config.max_results = 4; config.max_request_bytes = 16384; config.max_retained_bytes = 1 << 20;
        api.header = header<matter_geometry_api>();
        f::check(matter_geometry_create(&config, &api) == MATTER_OK, "instance creation");
        budget.header = header<matter_budget>();
        f::check(matter_geometry_default_budget(&budget) == MATTER_OK, "budget defaults");
        budget.execution_microseconds = 250000;
        job.generation_epoch = epoch ? epoch : 1;
    }
    ~Instance() { if (api.instance) f::check(matter_geometry_dispose(&api) == MATTER_OK, "dispose outstanding results"); }
    matter_call_result call(std::string_view wire, unsigned operation = 0, matter_result_handle source = 0,
                            const matter_control *control = nullptr) {
        if (operation == 1) return api.query(api.instance, source, bytes(wire), &job, &budget, control);
        if (operation == 2) return api.process(api.instance, source, bytes(wire), &job, &budget, control);
        return api.evaluate(api.instance, bytes(wire), &job, &budget, control);
    }
    std::string copy(matter_result_handle handle) {
        matter_bytes_view view{};
        f::check(api.read(api.instance, handle, &view) == MATTER_OK && view.data && view.byte_length, "read live result");
        return {reinterpret_cast<const char *>(view.data), size_t(view.byte_length)};
    }
    void diagnostics() {
        uint64_t required = 0;
        f::check(api.diagnostics(api.instance, {nullptr, 0}, &required) == MATTER_RESOURCE_LIMIT && required > 0,
                 "diagnostic sizing");
        std::vector<uint8_t> out(size_t(required + 2), 0xa5);
        f::check(api.diagnostics(api.instance, {out.data() + 1, required - 1}, &required) == MATTER_RESOURCE_LIMIT,
                 "short diagnostic buffer rejected");
        f::check(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0xa5; }), "short buffer untouched");
        f::check(api.diagnostics(api.instance, {out.data() + 1, required}, &required) == MATTER_OK, "copy diagnostics");
        f::check(out.front() == 0xa5 && out.back() == 0xa5, "diagnostic output bounds");
        const auto value = parse({reinterpret_cast<const char *>(out.data() + 1), size_t(required)});
        f::check(std::holds_alternative<V::Array>(value.data) && !std::get<V::Array>(value.data).empty(), "failure diagnostics present");
    }
    void inspect(matter_call_result result) {
        f::check(result.header.size == sizeof(result) && result.header.major == MATTER_ABI_MAJOR &&
                 result.header.minor == MATTER_ABI_MINOR, "versioned result");
        f::check(result.status <= MATTER_INVALID_HANDLE, "known status");
        if (result.status != MATTER_OK) {
            f::check(result.result == 0, "failure publishes no handle"); diagnostics(); return;
        }
        f::check(result.result != 0, "success owns a handle");
        auto wire = parse(copy(result.result));
        auto &echo = field(wire, "job");
        f::check(std::get<std::string>(field(echo, "request_id").data) == "property.request" &&
                 std::get<std::string>(field(echo, "source_revision").data) == "revision.seeded" &&
                 std::get<std::string>(field(echo, "generation_epoch").data) == epoch_hex(job.generation_epoch), "full job echo");
        if (std::get<std::string>(field(wire, "kind").data) == "asset") {
            d::AssetDocument asset;
            f::check(d::decode_asset(encode(field(wire, "asset")), asset).ok(), "successful asset independently validates");
        }
        f::check(api.release(api.instance, result.result) == MATTER_OK, "release result");
        f::check(api.release(api.instance, result.result) == MATTER_INVALID_HANDLE, "double release rejected");
        matter_bytes_view stale{reinterpret_cast<const uint8_t *>(uintptr_t(1)), 123};
        f::check(api.read(api.instance, result.result, &stale) == MATTER_INVALID_HANDLE && !stale.data && !stale.byte_length,
                 "stale read clears view");
    }
};
struct Cancellation {
    unsigned progress = 0, after = 0;
    static uint32_t cancelled(void *p) { const auto &c = *static_cast<Cancellation *>(p); return c.progress >= c.after; }
    static void report(void *p, double fraction, matter_bytes_view stage) {
        f::check(fraction >= 0 && fraction <= 1 && stage.data && stage.byte_length, "progress contract");
        ++static_cast<Cancellation *>(p)->progress;
    }
};
void valid_box(Instance &instance, f::Input &input) {
    auto request = parse(box_request);
    auto &node = std::get<V::Array>(field(field(request, "recipe"), "nodes").data).at(0);
    std::array<double, 3> size{};
    for (auto &axis : size) axis = (1 + input.byte()) / 8.0;
    field(field(node, "parameters"), "size") = V::Array{size[0], size[1], size[2]};
    const auto wire = encode(request);
    const auto first = instance.call(wire);
    f::check(first.status == MATTER_OK, "generated positive box succeeds");
    const auto owned = instance.copy(first.result);
    const auto second = instance.call(wire);
    f::check(second.status == MATTER_OK && first.result != second.result && instance.copy(second.result) == owned,
             "deterministic geometry, unique handles");
    auto result = parse(owned);
    auto &mesh = std::get<V::Array>(field(field(result, "asset"), "meshes").data).at(0);
    const auto &positions = std::get<V::Array>(field(mesh, "positions").data);
    std::array<double, 3> low{1e9, 1e9, 1e9}, high{-1e9, -1e9, -1e9};
    for (const auto &position : positions) {
        const auto &xyz = std::get<V::Array>(position.data);
        for (size_t axis = 0; axis < 3; ++axis) {
            const auto coordinate = std::get<double>(xyz.at(axis).data);
            low[axis] = std::min(low[axis], coordinate); high[axis] = std::max(high[axis], coordinate);
        }
    }
    for (size_t axis = 0; axis < 3; ++axis) f::check(high[axis] - low[axis] == size[axis], "box extents match generated dimensions");
    instance.inspect(first); instance.inspect(second);
}
void lifetime(Instance &instance, f::Input &input) {
    Instance other(0);
    std::vector<matter_result_handle> live;
    matter_result_handle previous = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (live.empty() || input.byte() % 2) {
            const auto result = instance.call(box_request);
            if (live.size() == 4) {
                f::check(result.status == MATTER_RESOURCE_LIMIT && result.result == 0, "live result cap enforced");
            } else {
                f::check(result.status == MATTER_OK && result.result > previous, "handles monotonic after release");
                previous = result.result; live.push_back(result.result);
                f::check(other.api.release(other.api.instance, result.result) == MATTER_INVALID_HANDLE, "cross-instance release rejected");
            }
        } else {
            const auto index = input.byte() % live.size();
            const auto handle = live[index];
            instance.inspect({header<matter_call_result>(), MATTER_OK, handle});
            live.erase(live.begin() + index);
        }
    }
    const auto stale = instance.api;
    f::check(matter_geometry_dispose(&instance.api) == MATTER_OK, "dispose with live handles");
    auto copy = stale;
    f::check(matter_geometry_dispose(&copy) == MATTER_INVALID_HANDLE, "copied table invalid after disposal");
    const auto failed = stale.evaluate(stale.instance, bytes(box_request), &instance.job, &instance.budget, nullptr);
    f::check(failed.status == MATTER_INVALID_HANDLE && failed.result == 0, "disposed instance rejects evaluation");
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    f::Input input{data, size};
    const auto mode = input.byte() % 9;
    Instance instance(input.word());
    if (mode == 4) { valid_box(instance, input); return 0; }
    if (mode == 8) { lifetime(instance, input); return 0; }
    if (mode == 6) {
        auto bad = instance.job;
        const auto selected = input.byte() % 4;
        const auto expected = selected == 0 || selected == 3 ? MATTER_INVALID_INPUT : MATTER_UNSUPPORTED_VERSION;
        if (selected == 0) bad.header.size = input.byte() % sizeof(bad);
        else if (selected == 1) bad.header.major = 2 + input.byte();
        else if (selected == 2) bad.header.minor = 1 + input.byte();
        else bad.generation_epoch = 0;
        const auto result = instance.api.evaluate(instance.api.instance, bytes(box_request), &bad, &instance.budget, nullptr);
        f::check(result.status == expected, "ABI header rejection"); instance.inspect(result);
    } else if (mode == 7) {
        Cancellation state{0, unsigned(input.byte() % 2)};
        matter_control control{header<matter_control>(), &state, Cancellation::cancelled, Cancellation::report};
        const auto result = instance.call(box_request, 0, 0, &control);
        f::check(result.status == MATTER_CANCELLED, "pre/progress cancellation publishes no asset"); instance.inspect(result);
    } else if (mode == 5) {
        switch (input.byte() % 5) {
        case 0: instance.budget.vertices = input.byte() % 8; break;
        case 1: instance.budget.output_bytes = input.byte() % 32; break;
        case 2: instance.budget.working_bytes = input.byte() * 64; break;
        case 3: instance.budget.operations = input.byte() % 2; break;
        default: instance.budget.dependency_depth = input.byte() % 2; break;
        }
        auto result = instance.call(box_request);
        f::check(result.status != MATTER_OK, "insufficient budget rejected"); instance.inspect(result);
    } else {
        matter_result_handle source = 0;
        std::string request;
        unsigned operation = 0;
        if (mode >= 2) {
            const auto baseline = instance.call(box_request);
            f::check(baseline.status == MATTER_OK, "source asset available"); source = baseline.result;
            operation = mode == 2 ? 1 : 2;
            request = f::mutate(mode == 2 ? query_request : process_request, input);
        } else request = mode == 0 ? input.text(4096) : f::mutate(box_request, input);
        instance.inspect(instance.call(request, operation, source));
        if (source) instance.inspect({header<matter_call_result>(), MATTER_OK, source});
    }
    // Any rejected input must leave the same instance usable with full budgets.
    instance.budget.header = header<matter_budget>();
    f::check(matter_geometry_default_budget(&instance.budget) == MATTER_OK, "restore budget");
    const auto recovered = instance.call(box_request);
    f::check(recovered.status == MATTER_OK, "instance recovers after failure"); instance.inspect(recovered);
    return 0;
}
#ifndef MATTER_FUZZ_DRIVER
int main() {
    matter::fuzz::seeded(0x43414249, 72, LLVMFuzzerTestOneInput);
    std::puts("ALL PASS: seeded C ABI properties");
}
#endif
