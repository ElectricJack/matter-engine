#include <matter/contracts/contracts.h>
#include "../../../MatterEngine3/src/dsl_rng.h"
#define RAYMATH_STATIC_INLINE
#include "../../../third_party/raylib/src/raymath.h"
#include <cmath>
#include <cstring>
#include <iostream>

int main() {
  // Noncommuting operands catch the documented raymath argument reversal.
  auto parent = mm::translation({10,20,30});
  auto local = mm::scale({-2,3,4});
  auto combined = mm::multiply(parent, local);
  Matrix ray_parent, ray_local;
  static_assert(sizeof(Matrix) == sizeof(mm::Mat4));
  std::memcpy(&ray_parent, parent.m, sizeof(ray_parent));
  std::memcpy(&ray_local, local.m, sizeof(ray_local));
  auto ray = MatrixMultiply(ray_local, ray_parent);
  float values[16]; std::memcpy(values, &ray, sizeof(values));
  for (unsigned i = 0; i < 16; ++i) if (combined.m[i] != values[i]) return 1;
  auto p = mm::transform_point(combined, {1,2,3});
  auto rp = Vector3Transform({1,2,3}, ray);
  if (p.x != 8 || p.y != 26 || p.z != 42 || rp.x != p.x || rp.y != p.y || rp.z != p.z) return 1;
  auto wrong = MatrixMultiply(ray_parent, ray_local);
  if (Vector3Transform({1,2,3}, wrong).x == p.x) return 1;
  for (uint64_t seed : {uint64_t(0), uint64_t(1), UINT64_C(0x0123456789abcdef), UINT64_MAX}) {
    dsl::Rng legacy(seed), legacy_units(seed);
    matter::contracts::Rng portable(seed), portable_units(seed);
    for (unsigned i = 0; i < 128; ++i)
      if (legacy.next_u64() != portable.next_u64() || legacy_units.next_unit() != portable_units.next_unit()) return 1;
  }
  std::cout << "PASS GATE-LEGACY-MATH raymath operand reversal; GATE-LEGACY-RNG 4 seeds x 128 draws\n";
}
