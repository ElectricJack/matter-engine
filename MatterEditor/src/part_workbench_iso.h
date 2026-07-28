#pragma once

// The Part Workbench's synthetic isolation world (part-workbench.md W2),
// extracted from part_workbench.cpp so the exact world text is headless-
// testable. The generation rules here are behavior, not formatting: the
// bake pipeline consumes this text verbatim, and the one field that has
// already caused a field-reported defect (expand) is pinned by
// MatterEditor/tests/test_workbench_actions.cpp.

#include <string>

namespace viewer {

// Identifier-safe form of a module name, shared by the generated class name
// and the iso world/cache name so they can never disagree about what
// "sanitized" means.
std::string workbench_iso_identifier(const std::string& module);

// The generated single-root world source. The root is placed UNEXPANDED,
// like every authored world's `static roots` entry: `expand: true` is the
// gallery-scatter feature that REPLACES a root with its children, and it
// hard-fails on leaf parts ("expand: root has no children"), which published
// nothing and made "Open in Workbench" look like a dead button (issue
// editor-workbench-actions-noop). Unexpanded, the part itself is instance 0 —
// which is also what the LOD Inspector (W4) and per-LOD authoring (W5)
// assume when they read instance_info(0).part_hash.
std::string workbench_iso_world_source(const std::string& module,
                                       const std::string& params_json);

}  // namespace viewer
