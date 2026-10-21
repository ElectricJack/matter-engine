// Reuse the actual codec test and its filesystem predicates during the build.
// The runtime executable retains the generated numeric/byte guards; it never
// runs the functions gated by MATTER_CODEC_SOURCE_QUALIFICATION.
#ifndef MATTER_QUALIFICATION_SOURCE
#error MATTER_QUALIFICATION_SOURCE must name the canonical test translation unit
#endif
#define MATTER_CODEC_SOURCE_QUALIFICATION 1
#include MATTER_QUALIFICATION_SOURCE
