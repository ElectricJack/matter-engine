#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace viewer {

// Pure deadline policy; the clock is supplied by the caller. A rate change,
// reset, or late frame starts immediately. Completing a wait schedules from
// the actual wake time, so a hitch never produces catch-up frames.
class FramePacerSchedule {
public:
    static double normalized_rate(double target_fps) noexcept {
        if (!std::isfinite(target_fps) || target_fps <= 0.0) return 0.0;
        return std::clamp(target_fps, 1.0, 1000.0);
    }

    double plan(double now_seconds, double target_fps) noexcept {
        const double rate = normalized_rate(target_fps);
        if (rate == 0.0 || !std::isfinite(now_seconds)) {
            reset();
            return now_seconds;
        }
        if (!active_ || rate != rate_) {
            active_ = true;
            rate_ = rate;
            deadline_ = now_seconds;
        }
        return std::max(now_seconds, deadline_);
    }

    void complete(double actual_seconds) noexcept {
        if (!active_ || !std::isfinite(actual_seconds)) return;
        deadline_ = actual_seconds + 1.0 / rate_;
    }

    void reset() noexcept {
        active_ = false;
        rate_ = 0.0;
        deadline_ = 0.0;
    }

private:
    bool active_ = false;
    double rate_ = 0.0;
    double deadline_ = 0.0;
};

// Call once before polling input. Disabled is the default, and does not sample
// the clock. No global timer-resolution or Vulkan synchronization changes.
class FramePacer {
public:
    FramePacer() = default;
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;
    ~FramePacer() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }

    void reset() noexcept { schedule_.reset(); }

    double wait(double target_fps) {
        return wait(target_fps, [] { return false; });
    }

    // Long waits poll through the caller between at most 25 ms sleep slices.
    // Return true to cancel (for example, a close request). Normal high-rate
    // frames never invoke the callback or create extra timer waits.
    template <typename Interrupt>
    double wait(double target_fps, Interrupt&& interrupt) {
        if (FramePacerSchedule::normalized_rate(target_fps) == 0.0) {
            reset();
            return 0.0;
        }
        const auto start = Clock::now();
        const double deadline_seconds = schedule_.plan(seconds(start), target_fps);
        const auto deadline = Clock::time_point(
            std::chrono::duration_cast<Clock::duration>(
                std::chrono::duration<double>(deadline_seconds)));
        if (deadline <= start) {
            schedule_.complete(seconds(start));
            return 0.0;
        }
        // Keep the final yield loop within 0.2 ms. Long periods always sleep;
        // an early kernel wake retries the coarse sleep instead of spinning.
        constexpr auto tail = std::chrono::microseconds(200);
        constexpr auto maximum_slice = std::chrono::milliseconds(25);
        auto now = start;
        while (deadline - now > tail) {
            const auto coarse_deadline = deadline - tail;
            const bool poll_after_slice = coarse_deadline - now > maximum_slice;
            sleep_until(poll_after_slice ? now + maximum_slice : coarse_deadline, now);
            now = Clock::now();
            if (poll_after_slice) {
                if (interrupt()) {
                    reset();
                    return std::chrono::duration<double, std::milli>(
                        Clock::now() - start).count();
                }
                // Polling can consume time too; never sleep from a stale stamp.
                now = Clock::now();
            }
        }
        while (now < deadline) {
            std::this_thread::yield();
            now = Clock::now();
        }
        schedule_.complete(seconds(now));
        return std::chrono::duration<double, std::milli>(now - start).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    FramePacerSchedule schedule_;
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool timer_attempted_ = false;
#endif

    static double seconds(Clock::time_point value) noexcept {
        return std::chrono::duration<double>(value.time_since_epoch()).count();
    }

    void sleep_until(Clock::time_point deadline, Clock::time_point now) {
#ifdef _WIN32
        if (!timer_attempted_) {
            timer_attempted_ = true;
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+). Passing
            // the flag numerically also builds against older SDK headers;
            // unsupported kernels fall back to the portable sleep below.
            constexpr DWORD high_resolution = 0x00000002;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, high_resolution,
                                           TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        if (timer_) {
            const double ticks = std::chrono::duration<double>(deadline - now).count() * 10000000.0;
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(std::max(1.0, std::ceil(ticks)));
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
                WaitForSingleObject(timer_, INFINITE) == WAIT_OBJECT_0) return;
        }
#else
        (void)now;
#endif
        std::this_thread::sleep_until(deadline);
    }
};

} // namespace viewer
