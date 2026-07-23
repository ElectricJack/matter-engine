#include "animation/animation_ir.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace matter::animation {

bool DiagnosticLess::operator()(const Diagnostic& left, const Diagnostic& right) const {
    if (left.source.module != right.source.module) return left.source.module < right.source.module;
    if (left.source.line != right.source.line) return left.source.line < right.source.line;
    if (left.source.column != right.source.column) return left.source.column < right.source.column;
    if (left.source.object != right.source.object) return left.source.object < right.source.object;
    if (left.code != right.code) return left.code < right.code;
    return left.message < right.message;
}

void Diagnostics::add(const char* code, const SourceSpan& source, const char* message) {
    items.push_back({code, message, source});
}

void Diagnostics::sort() { std::sort(items.begin(), items.end(), DiagnosticLess{}); }

std::string CanonicalAnimationBuild::encode() const {
    std::ostringstream output;
    output << std::setprecision(9);
    for (const CanonicalJoint& joint : rig.joints) {
        output << joint.name << '|' << joint.parent << '|' << joint.subtree.begin << ':' << joint.subtree.end << '|'
               << joint.local.translation.x << ',' << joint.local.translation.y << ',' << joint.local.translation.z << '|'
               << joint.local.rotation.x << ',' << joint.local.rotation.y << ',' << joint.local.rotation.z << ',' << joint.local.rotation.w << '|'
               << joint.local.scale.x << ',' << joint.local.scale.y << ',' << joint.local.scale.z << '|' << joint.radius << '\n';
    }
    for (const CanonicalSocket& socket : rig.sockets) {
        output << "socket|" << socket.name << '|' << socket.joint << '|'
               << socket.local.translation.x << ',' << socket.local.translation.y << ',' << socket.local.translation.z << '|'
               << socket.local.rotation.x << ',' << socket.local.rotation.y << ',' << socket.local.rotation.z << ',' << socket.local.rotation.w << '|'
               << socket.local.scale.x << ',' << socket.local.scale.y << ',' << socket.local.scale.z << '\n';
    }
    for (const CanonicalTarget& target : targets) {
        output << target.name << '|' << static_cast<int>(target.driver) << '|' << static_cast<int>(target.cadence);
        for (JointIndex joint : target.chain) output << '|' << joint;
        output << '\n';
    }
    output << "graph";
    for (uint16_t node : graph_order) output << '|' << node;
    output << '\n' << authored_state;
    return output.str();
}

} // namespace matter::animation
