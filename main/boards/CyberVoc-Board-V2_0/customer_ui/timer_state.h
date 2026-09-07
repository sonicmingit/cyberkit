#pragma once
#include <cstdint>

namespace feature {
// Caller owns synchronization; all times are monotonic microseconds.
class Timer {
public:
    enum State { Idle, Running, Paused, Finished };
    State state = Idle;
    int64_t selected = 300000000LL;
    int64_t remaining = selected;
    int64_t deadline = 0;
    uint64_t generation = 0;
    void Start(int64_t duration, int64_t now) {
        ++generation; selected = remaining = duration;
        deadline = now + duration; state = Running;
    }
    int64_t Remaining(int64_t now) const {
        const int64_t value = state == Running ? deadline - now : remaining;
        return value > 0 ? value : 0;
    }
    bool Pause(int64_t now) {
        if (state != Running) return false;
        remaining = Remaining(now);
        if (!remaining) return false; // Poll owns expiry/notification.
        ++generation; state = Paused; return true;
    }
    bool Resume(int64_t now) {
        if (state != Paused || remaining <= 0) return false;
        ++generation; deadline = now + remaining; state = Running; return true;
    }
    void Cancel() { ++generation; remaining = selected; state = Idle; }
    bool Poll(int64_t now, uint64_t& token) {
        if (state != Running || now < deadline) return false;
        remaining = 0; state = Finished; token = generation; return true;
    }
    bool CanNotify(uint64_t token) const { return state == Finished && token == generation; }
};
}
