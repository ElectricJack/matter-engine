#include "animation/animation_ir.h"

// MatterEngine3/src/animation/animation_ir.cpp
//
// Out-of-line members of `animation_ir.h`. Only two things need a definition:
// the total order over diagnostics, and the canonical text encoding of a
// `CanonicalAnimationBuild`.
//
// Both exist for the same reason -- determinism. A bake must produce identical
// bytes for identical input regardless of the order checks fired in or the
// order containers happened to be walked, so diagnostics are sorted by source
// position and `encode()` writes a fixed field order at a fixed precision.

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace matter::animation {

// Total order over diagnostics: source position first (module, line, column,
// object), then code, then message. Message is included as the final
// tiebreaker so the order is total, not just deterministic-per-position --
// `std::sort` is not stable, so a partial order would leave equal-keyed
// entries free to permute between runs.
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

// Serializes the canonical build to the stable text the animation determinism
// hash is taken over. Field order, delimiters, and the stream's 9 significant
// digits (the round-trip precision of a 32-bit float) are all part of that
// contract -- changing any of them changes every asset's hash.
//
// Layout: one line per joint, then one `socket|...` line per socket, then one
// line per target (fixed fields, then the chain joints), then a single
// `graph|...` line of the topological node order, then `authored_state`
// verbatim. Fields are `|`-separated with `,` inside a vector.
//
// Authored names are written LENGTH-PREFIXED (`<bytes>:<text>`, the same
// convention `append_string` uses for `authored_state` in
// animation_validate.cpp) rather than verbatim. Nothing upstream rejects a
// `|` or a newline inside a joint/socket/target name, so an unescaped name
// could reproduce the delimiters and forge a record -- two different rigs
// encoding to identical bytes, i.e. a determinism-hash collision. Joint names
// in particular appear nowhere else: `authored_state` does not carry them, so
// this encoding is their only fingerprint.
std::string CanonicalAnimationBuild::encode() const {
    std::ostringstream output;
    output << std::setprecision(9);
    const auto name = [&output](const std::string& value) -> std::ostringstream& {
        output << value.size() << ':' << value;
        return output;
    };
    for (const CanonicalJoint& joint : rig.joints) {
        name(joint.name) << '|' << joint.parent << '|' << joint.subtree.begin << ':' << joint.subtree.end << '|'
               << joint.local.translation.x << ',' << joint.local.translation.y << ',' << joint.local.translation.z << '|'
               << joint.local.rotation.x << ',' << joint.local.rotation.y << ',' << joint.local.rotation.z << ',' << joint.local.rotation.w << '|'
               << joint.local.scale.x << ',' << joint.local.scale.y << ',' << joint.local.scale.z << '|' << joint.radius << '\n';
    }
    for (const CanonicalSocket& socket : rig.sockets) {
        output << "socket|";
        name(socket.name) << '|' << socket.joint << '|'
               << socket.local.translation.x << ',' << socket.local.translation.y << ',' << socket.local.translation.z << '|'
               << socket.local.rotation.x << ',' << socket.local.rotation.y << ',' << socket.local.rotation.z << ',' << socket.local.rotation.w << '|'
               << socket.local.scale.x << ',' << socket.local.scale.y << ',' << socket.local.scale.z << '\n';
    }
    for (const CanonicalTarget& target : targets) {
        name(target.name) << '|' << static_cast<int>(target.driver) << '|' << static_cast<int>(target.cadence) << '|';
        name(target.controller) << '|' << target.has_pole << '|' << target.pole.x << ',' << target.pole.y << ',' << target.pole.z << '|' << target.bend_axis.x << ',' << target.bend_axis.y << ',' << target.bend_axis.z << '|' << target.soften << '|' << target.twist << '|' << target.position_half_life << '|' << target.rotation_half_life << '|' << target.weight_half_life << '|' << target.enabled;
        for (JointIndex joint : target.chain) output << '|' << joint;
        output << '\n';
    }
    output << "graph";
    for (uint16_t node : graph_order) output << '|' << node;
    output << '\n' << authored_state;
    return output.str();
}

} // namespace matter::animation
