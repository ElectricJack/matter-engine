#pragma once

// Typed, source-preserving procedural parameter edits.  This deliberately
// operates on the published canonical parameter object, never on JavaScript
// text: the engine's normal root-override/rebake path remains the only writer.

#include "agent_protocol.h"
#include "matter/json_doc.h"

#include <string>
#include <vector>

namespace viewer::procedural {

struct Root {
    agent::ObjectIdentity object;
    std::string module;
    std::string source_path;
    std::string params_json;
};

struct Plan {
    Root root;
    matter::jsondoc::Value before;
    matter::jsondoc::Value after;
    std::vector<std::string> changed;
};

// The source of truth contains parameter values and types, but no portable
// range metadata.  describe_json makes that absence explicit instead of
// inventing min/max constraints.
matter::jsondoc::Value describe_json(const Root& root);

// Validates every requested field before constructing `after`.  A failure
// leaves `out` untouched, providing the all-or-nothing boundary used by apply.
bool make_plan(const Root& root, const matter::jsondoc::Value& changes,
               Plan& out, std::string& error);

matter::jsondoc::Value plan_json(const Plan& plan, bool dry_run);

}  // namespace viewer::procedural
