#include "../src/frame_pacer.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
bool approximately_equal(double a, double b) { return std::fabs(a - b) < 1e-9; }
}

int main() {
    viewer::FramePacerSchedule schedule;
    check(approximately_equal(schedule.plan(10.0, 0.0), 10.0), "disabled never delays");
    schedule.complete(10.0);
    check(approximately_equal(schedule.plan(11.0, -1.0), 11.0), "negative rate disables");
    check(approximately_equal(schedule.plan(11.0, std::numeric_limits<double>::infinity()), 11.0),
          "infinite rate disables");
    check(approximately_equal(schedule.plan(11.0, std::numeric_limits<double>::quiet_NaN()), 11.0),
          "NaN rate disables");

    check(approximately_equal(schedule.plan(20.0, 100.0), 20.0), "first enabled frame starts immediately");
    schedule.complete(20.0);
    check(approximately_equal(schedule.plan(20.003, 100.0), 20.010), "normal work waits only remaining interval");
    schedule.complete(20.010);
    check(approximately_equal(schedule.plan(20.014, 100.0), 20.020), "steady deadlines maintain cadence");
    schedule.complete(20.020);

    check(approximately_equal(schedule.plan(20.070, 100.0), 20.070), "late frame never waits for obsolete deadline");
    schedule.complete(20.070);
    check(approximately_equal(schedule.plan(20.071, 100.0), 20.080), "hitch does not produce a catch-up burst");
    schedule.complete(20.084); // scheduler/timer overslept by four milliseconds
    check(approximately_equal(schedule.plan(20.085, 100.0), 20.094), "oversleep rebases the next interval");
    schedule.complete(20.094);

    check(approximately_equal(schedule.plan(20.096, 50.0), 20.096), "rate change discards previous rate deadline");
    schedule.complete(20.096);
    check(approximately_equal(schedule.plan(20.100, 50.0), 20.116), "new rate owns the next full interval");
    schedule.complete(20.116);
    schedule.reset();
    check(approximately_equal(schedule.plan(25.0, 50.0), 25.0), "resume starts immediately after reset");
    schedule.complete(25.0);
    check(approximately_equal(schedule.plan(25.001, 0.0), 25.001), "disable clears an armed deadline");
    check(approximately_equal(schedule.plan(25.002, 50.0), 25.002), "reenable starts fresh");

    schedule.reset();
    schedule.plan(30.0, 10000.0);
    schedule.complete(30.0);
    check(approximately_equal(schedule.plan(30.0001, 10000.0), 30.001), "positive rates clamp to 1000 Hz maximum");
    schedule.reset();
    schedule.plan(40.0, 0.1);
    schedule.complete(40.0);
    check(approximately_equal(schedule.plan(40.1, 0.1), 41.0), "positive rates clamp to 1 Hz minimum");

    // The disabled production API requires neither a timer nor a real wait.
    viewer::FramePacer pacer;
    check(pacer.wait(0.0) == 0.0, "disabled production pacer returns zero wait");
    int polls = 0;
    check(pacer.wait(0.0, [&] { ++polls; return true; }) == 0.0,
          "disabled callback overload returns zero wait");
    check(polls == 0, "disabled pacing never polls or calls interruption callback");
    pacer.reset();
    check(pacer.wait(1.0) == 0.0, "first low-rate frame starts without sleeping");
    bool interrupted = false;
    const double interrupted_ms = pacer.wait(1.0, [&] {
        interrupted = true;
        return true;
    });
    check(interrupted && interrupted_ms > 0.0,
          "a long production wait services its interrupt callback");
    check(pacer.wait(1.0) == 0.0,
          "interruption resets the next production deadline");
    std::printf("frame_pacer: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
