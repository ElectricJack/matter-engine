#pragma once

// Every query contributes to the height, so dead-code removal cannot discard
// seed/position changes or the return to the original query. The three source
// coordinates vary per sample; GPU liveness allocation may reuse registers.
namespace vt_cellular_test {
inline constexpr const char* query_stress =
    "input lx\ninput ly\ninput lz\n"
    "cellular3 317 gap r0 r1 r2\n"
    "cellular3 317 value r0 r1 r2\n"
    "cellular3 317 distance r0 r1 r2\n"
    "const 0.375\nadd r0 r6\n"
    "cellular3 317 value r7 r1 r2\n"
    "cellular3 4294967295 value r7 r1 r2\n"
    "cellular3 317 gap r0 r1 r2\n"
    "cellular3 317 value r0 r1 r2\n"
    "cellular3 317 distance r0 r1 r2\n"
    "add r3 r4\nadd r13 r5\nadd r14 r8\nadd r15 r9\n"
    "add r16 r10\nadd r17 r11\nadd r18 r12\n"
    "const 0.005\nmul r19 r20\nconst 1\nconst 0\n"
    "material 1 r22\nsource 1 r3 r8 r9 r22 r23 r22 r21 0 0.08\n";
}
