#pragma once
// Installs the native __dsl_* DSL bindings (and the seeded Math.random override)
// onto a QuickJS-ng context. The context's opaque must point at a dsl::DslState.
struct JSContext;
namespace dsl {
void install_bindings(JSContext* ctx);
}
