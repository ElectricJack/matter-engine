#pragma once
// Fixture: a neutral value contract every component may use.
namespace fx {
struct Value {
    double x = 0.0, y = 0.0, z = 0.0;
};
double fx_length(const Value& value);
}  // namespace fx
