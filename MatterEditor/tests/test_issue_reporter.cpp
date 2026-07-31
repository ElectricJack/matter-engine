// Headless test for the issue reporter's writer half (issue_reporter.cpp).
// The dialog (issue_reporter_panel.cpp) needs ImGui and is not covered here;
// the crop math and the on-disk schema are what regress silently.
#include "issue_reporter.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "console_panel.h"
#include "ui.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// --- crop ------------------------------------------------------------------

void test_crop_passthrough() {
    // 4x2 RGBA, distinguishable per pixel.
    std::vector<uint8_t> src(4 * 2 * 4);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i);

    viewer::ShotRect full{};  // empty rect means "whole frame"
    const std::vector<uint8_t> out = viewer::crop_rgba(src, 4, 2, full);
    check(out == src, "empty rect returns the source unchanged");
    check(full.x == 0 && full.y == 0 && full.w == 4 && full.h == 2,
          "empty rect resolves to the full frame");

    viewer::ShotRect oversized{0, 0, 99, 99};
    const std::vector<uint8_t> out2 = viewer::crop_rgba(src, 4, 2, oversized);
    check(out2 == src, "rect covering everything returns the source");
    check(oversized.w == 4 && oversized.h == 2, "oversized rect clamps to frame");
}

void test_crop_subrect() {
    // 4x2: row-major RGBA where each pixel's R channel is its linear index.
    std::vector<uint8_t> src(4 * 2 * 4, 0);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            src[static_cast<size_t>(y * 4 + x) * 4] =
                static_cast<uint8_t>(y * 4 + x);

    viewer::ShotRect rect{1, 0, 2, 2};
    const std::vector<uint8_t> out = viewer::crop_rgba(src, 4, 2, rect);
    check(out.size() == 2u * 2u * 4u, "crop output is w*h*4 bytes");
    check(rect.w == 2 && rect.h == 2, "crop rect preserved");
    // Expected R channels: (1,2) from row 0, (5,6) from row 1.
    check(out[0] == 1 && out[4] == 2 && out[8] == 5 && out[12] == 6,
          "crop copies the right pixels from the right rows");
}

void test_crop_clamps_off_edge() {
    std::vector<uint8_t> src(4 * 2 * 4, 7);
    // A drag that ran off the bottom-right corner: normal, not an error.
    viewer::ShotRect rect{2, 1, 10, 10};
    const std::vector<uint8_t> out = viewer::crop_rgba(src, 4, 2, rect);
    check(rect.x == 2 && rect.y == 1 && rect.w == 2 && rect.h == 1,
          "off-edge rect clamps to the source bounds");
    check(out.size() == 2u * 1u * 4u, "clamped crop is sized to the clamp");

    // Fully off-screen degenerates to the full frame rather than zero bytes.
    viewer::ShotRect off{50, 50, 10, 10};
    const std::vector<uint8_t> out2 = viewer::crop_rgba(src, 4, 2, off);
    check(out2.size() == src.size(), "fully off-screen rect falls back to full");
}

// --- report ----------------------------------------------------------------

// --- downscale --------------------------------------------------------------

void test_downscale_passthrough() {
    std::vector<uint8_t> src(8 * 4 * 4, 3);
    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> out = viewer::downscale_rgba(src, 8, 4, 16, w, h);
    check(out == src && w == 8 && h == 4,
          "image already inside max_edge is returned unchanged");
}

void test_downscale_halves() {
    // 4x2 where the left half is 0 and the right half is 200, so a 2x1 box
    // filter must produce exactly those two averages.
    std::vector<uint8_t> src(4 * 2 * 4, 0);
    for (int y = 0; y < 2; ++y)
        for (int x = 2; x < 4; ++x)
            for (int c = 0; c < 4; ++c)
                src[(static_cast<size_t>(y) * 4 + x) * 4 + c] = 200;

    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> out = viewer::downscale_rgba(src, 4, 2, 2, w, h);
    check(w == 2 && h == 1, "downscale keeps aspect and honours max_edge");
    check(out.size() == 2u * 1u * 4u, "downscaled buffer is w*h*4");
    check(out[0] == 0, "left box averages to the dark source block");
    check(out[4] == 200, "right box averages to the bright source block");
}

void test_downscale_averages_not_samples() {
    // A 1px bright line in an otherwise dark 4x1 row must survive as a non-zero
    // average; nearest-neighbour would drop it entirely.
    std::vector<uint8_t> src(4 * 1 * 4, 0);
    for (int c = 0; c < 4; ++c) src[1 * 4 + c] = 255;
    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> out = viewer::downscale_rgba(src, 4, 1, 2, w, h);
    check(w == 2 && h == 1, "4x1 downscales to 2x1");
    check(out[0] > 0, "a thin bright line is averaged in, not sampled away");
}

viewer::IssueShot make_shot(const char* file, const char* caption, float ex) {
    viewer::IssueShot shot;
    shot.file = file;
    shot.caption = caption;
    shot.region = viewer::ShotRegion::Custom;
    shot.rect = viewer::ShotRect{10, 20, 300, 200};
    shot.camera.position = {ex, 4.0f, -8.0f};
    shot.camera.target = {0.0f, 1.5f, 0.0f};
    shot.sim_mode = matter::scene::SimulationMode::Play;
    shot.time_scale = 0.25f;
    shot.width = 300;
    shot.height = 200;
    shot.frame_ms = 16.5f;
    shot.instances_drawn = 1234;
    shot.triangles = 98765;
    shot.draw_batches = 42;
    return shot;
}

// Stands in for EditorProps' viewer.budget group; the writer only ever sees a
// Registry, so the test does not need the editor's real schema TU.
const matter::props::Group& test_budget_group() {
    static const auto def = matter::props::group<viewer::ViewerStats>(
        "viewer.budget", "Viewer Budget",
        matter::props::prop(&viewer::ViewerStats::pixel_budget, "pixel_budget")
            .range(0.05f, 4.0f));
    return def.group();
}

void test_write_report(const std::filesystem::path& root) {
    viewer::IssueReporterState state;
    std::snprintf(state.note, sizeof(state.note),
                  "LOD swap is visible while walking north. The swap should be "
                  "imperceptible at this speed.");
    state.shots.push_back(make_shot("shot-1.png", "before the pop", 12.5f));
    state.shots.push_back(make_shot("shot-2.png", "after the pop", 30.0f));

    viewer::IssueContext context;
    context.world = "Meadow";
    context.project_dir = "../projects/world_demo";
    context.camera.position = {1.0f, 2.0f, 3.0f};
    context.sim_mode = matter::scene::SimulationMode::Play;
    context.time_scale = 0.25f;
    context.frame_width = 1920;
    context.frame_height = 1080;

    viewer::ViewerStats stats{};
    stats.frame_ms = 16.5f;
    matter::FrameStats frame_stats{};
    frame_stats.instances_drawn = 1234;
    viewer::ConsoleLog log;
    log.push(viewer::LogSeverity::Warning, "TLAS draw capacity exceeded");

    // Property capture (design S6.3): every registered group's non-baseline
    // fields land in state.json, so a repro carries the tuning it was made
    // under. Bound here the way EditorProps binds the real groups.
    matter::props::Registry props;
    matter::props::Binding* budget =
        props.get(props.bind(test_budget_group(), &stats,
                             matter::props::Scope::User));
    matter::props::set_float(*budget, test_budget_group().fields[0], 3.5f);
    context.props = &props;

    const std::string dir =
        viewer::write_issue_report(state, context, stats, frame_stats, log);
    check(!dir.empty(), "write_issue_report returns a directory");
    if (dir.empty()) return;

    check(std::filesystem::exists(std::filesystem::path(dir) / "issue.md"),
          "issue.md written");
    check(std::filesystem::exists(std::filesystem::path(dir) / "state.json"),
          "state.json written");
    check(std::filesystem::exists(std::filesystem::path(dir) / "log-tail.txt"),
          "log-tail.txt written");

    // The directory is a bare GUID under the issues root, not a slug: naming
    // is ingestion's job.
    const std::string name = std::filesystem::path(dir).filename().string();
    check(name.size() == 36 && name[8] == '-' && name[13] == '-' &&
              name[18] == '-' && name[23] == '-',
          "report directory is GUID-named");
    check(std::filesystem::path(dir).parent_path() == root,
          "report lands under MATTER_ISSUE_DIR");

    const std::string md = read_file(std::filesystem::path(dir) / "issue.md");
    check(contains(md, "world: Meadow"), "frontmatter carries world");
    check(contains(md, "shots: 2"), "frontmatter carries shot count");
    check(contains(md, "id: " + name), "frontmatter id matches the directory");
    check(contains(md, "status: unprocessed"),
          "frontmatter marks the report for ingestion");
    // The reporter must not invent classification it never asked for.
    check(!contains(md, "\nkind:"), "no kind in frontmatter");
    check(!contains(md, "\nseverity:"), "no severity in frontmatter");
    check(!contains(md, "\narea:"), "no area in frontmatter");
    check(contains(md, "## Report"), "body uses the neutral Report heading");
    check(contains(md, "LOD swap is visible"), "note is included");
    check(contains(md, "imperceptible"), "the whole note is included");
    check(contains(md, "![before the pop](shot-1.png)"), "shot 1 embedded");
    check(contains(md, "![after the pop](shot-2.png)"), "shot 2 embedded");
    check(contains(md, "MATTER_WORLD=Meadow"), "repro names the world");
    check(contains(md, "MATTER_CAM=\"12.500,4.000,-8.000"),
          "repro rebuilds the FIRST shot's camera, not the file-time camera");
    check(contains(md, "MATTER_TIME_SCALE=0.25"),
          "repro carries a non-default time scale");
    check(contains(md, "MATTER_CAM=\"30.000,4.000,-8.000"),
          "a shot from a different viewpoint gets its own launch line");
    check(contains(md, "_TODO"), "acceptance is left as an explicit TODO");

    const std::string json =
        read_file(std::filesystem::path(dir) / "state.json");
    check(contains(json, "\"file\": \"shot-1.png\""), "state.json lists shots");
    check(contains(json, "\"region\": \"region\""),
          "state.json records the crop region kind");
    check(contains(json, "\"rect\": {\"x\": 10, \"y\": 20, \"w\": 300, \"h\": 200}"),
          "state.json records the crop rect");
    check(contains(json, "\"at_file_time\""), "state.json has file-time state");
    check(contains(json, "\"props\": {\"viewer.budget\":{\"pixel_budget\":3.5}}"),
          "state.json dumps non-baseline properties by group path");

    const std::string tail =
        read_file(std::filesystem::path(dir) / "log-tail.txt");
    check(contains(tail, "WARN"), "log tail keeps severity");
    check(contains(tail, "TLAS draw capacity exceeded"), "log tail keeps text");
}

void test_note_only_report(const std::filesystem::path&) {
    // Plenty of reports are about behaviour, not pixels — "this feature needs
    // reworking" has nothing to screenshot. A shotless report must still file.
    viewer::IssueReporterState state;
    std::snprintf(state.note, sizeof(state.note),
                  "The LOD quality dial needs reworking; one slider conflates "
                  "two independent budgets.");
    viewer::IssueContext context;
    context.world = "Meadow";
    viewer::ViewerStats stats{};
    matter::FrameStats frame_stats{};
    viewer::ConsoleLog log;

    const std::string dir =
        viewer::write_issue_report(state, context, stats, frame_stats, log);
    check(!dir.empty(), "report writes even with no shots");
    if (dir.empty()) return;
    const std::string md = read_file(std::filesystem::path(dir) / "issue.md");
    check(contains(md, "shots: 0"), "shot count is zero");
    check(contains(md, "conflates"), "the note survives");
    check(contains(md, "_No shots captured._"), "shotless report says so");
    // With no shots the repro still has to name a launchable state, falling
    // back to the camera at file time.
    check(contains(md, "MATTER_WORLD=Meadow"), "repro still names the world");
}

void test_empty_report(const std::filesystem::path&) {
    // No note and no shots: the panel disables filing in this state, but the
    // writer must not produce a file claiming content it does not have.
    viewer::IssueReporterState state;
    viewer::IssueContext context;
    context.world = "CornellBox";
    viewer::ViewerStats stats{};
    matter::FrameStats frame_stats{};
    viewer::ConsoleLog log;
    const std::string dir =
        viewer::write_issue_report(state, context, stats, frame_stats, log);
    check(!dir.empty(), "empty report still writes rather than failing");
    if (dir.empty()) return;
    const std::string md = read_file(std::filesystem::path(dir) / "issue.md");
    check(contains(md, "_No description given"), "empty note is called out");
    check(contains(md, "_No shots captured._"), "empty shot list is called out");
}

} // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "matter_issue_reporter_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("MATTER_ISSUE_DIR", root.string().c_str());
#else
    setenv("MATTER_ISSUE_DIR", root.string().c_str(), 1);
#endif
    check(viewer::issues_dir() == root.string(),
          "MATTER_ISSUE_DIR overrides the default issues root");

    test_crop_passthrough();
    test_crop_subrect();
    test_crop_clamps_off_edge();
    test_downscale_passthrough();
    test_downscale_halves();
    test_downscale_averages_not_samples();
    test_write_report(root);
    test_note_only_report(root);
    test_empty_report(root);

    std::filesystem::remove_all(root, ec);
    if (failures == 0) {
        std::printf("All issue_reporter tests passed\n");
        return 0;
    }
    std::printf("%d issue_reporter test(s) failed\n", failures);
    return 1;
}
