#ifndef VIEWER_ASSET_BROWSER_IDS_H
#define VIEWER_ASSET_BROWSER_IDS_H

// MatterEditor/src/asset_browser_ids.h
//
// The one piece of the Asset Browser worth testing without a draw context: how
// a required-child row earns a unique ImGui identity.
//
// Split out of asset_browser.h purely so that rule can be exercised headlessly.
// MatterEditor/tests/test_asset_browser_ids.cpp includes this header and
// nothing else from the editor, and MatterEditor/Makefile builds it standalone
// as the `run-test-asset-browser-ids` target. asset_browser.cpp is the only
// production consumer (draw_object_row's requires tree).
//
// Header-only and dependency-free by design: no ImGui, no engine headers, no
// filesystem. Keep it that way, or the headless test stops being cheap.

#include <cstddef>
#include <string>
#include <string_view>

namespace viewer {

// Builds the ImGui ID-stack scope for one required-child row.
//
// The problem it solves: every child row draws a button literally labelled
// "Go", and one part may require the same module several times -- with
// different parameters, or with byte-identical ones. ImGui derives a control's
// identity from its label plus the enclosing ID scope, so without a
// distinguishing scope all those buttons collapse onto a single identity and
// only one of them responds.
//
// The encoding is length-prefixed ("<len>:<value>/" per component) rather than
// plain concatenation, so no pair of components can alias another by moving the
// separator: module "a/b" with params "c" cannot collide with module "a" and
// params "b/c". `occurrence` is the row's 0-based position among this parent's
// children and breaks the remaining tie between two identical requirements.
//
// Pure -- no ImGui calls, no state. The caller passes the result to
// ImGui::PushID. The three cases above are asserted by
// MatterEditor/tests/test_asset_browser_ids.cpp.
inline std::string required_child_row_identity(std::string_view module,
                                               std::string_view params_json,
                                               std::size_t occurrence) {
    std::string identity = "required-child/";
    const auto append_component = [&identity](std::string_view value) {
        identity += std::to_string(value.size());
        identity.push_back(':');
        identity.append(value.data(), value.size());
        identity.push_back('/');
    };
    append_component(module);
    append_component(params_json);
    identity += std::to_string(occurrence);
    return identity;
}

}  // namespace viewer

#endif  // VIEWER_ASSET_BROWSER_IDS_H
