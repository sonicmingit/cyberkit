#include "feature_ui.h"
#include "timer_state.h"
#include "application.h"
#include "board.h"
#include "display/display.h"
#include "assets/lang_config.h"
#include <esp_timer.h>
#include <mutex>

namespace {
std::recursive_mutex mutex;
feature::Timer timer;
}
extern "C" bool feature_timer_start_minutes(int32_t minutes) {
    if (minutes < 1 || minutes > 1440) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    timer.Start(minutes * 60000000LL, esp_timer_get_time());
    return true;
}
extern "C" bool feature_timer_pause() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return timer.Pause(esp_timer_get_time());
}
extern "C" bool feature_timer_resume() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return timer.Resume(esp_timer_get_time());
}
extern "C" bool feature_timer_cancel() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    timer.Cancel(); return true;
}
extern "C" feature_timer_state_t feature_timer_get_state() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return static_cast<feature_timer_state_t>(timer.state);
}
extern "C" int32_t feature_timer_get_remaining_seconds() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return (timer.Remaining(esp_timer_get_time()) + 999999) / 1000000;
}
extern "C" int32_t feature_timer_get_selected_minutes() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return timer.selected / 60000000;
}
extern "C" void feature_timer_adjust_minutes(int32_t delta) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (timer.state == feature::Timer::Running || timer.state == feature::Timer::Paused) return;
    int64_t next = timer.selected + delta * 60000000LL;
    timer.selected = next < 60000000 ? 60000000 : next > 86400000000LL ? 86400000000LL : next;
    timer.Cancel();
}
// Called on the application task by the bounded feature heartbeat. No stale
// esp_timer one-shot callback can ever complete a newly started countdown.
extern "C" void feature_timer_poll() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    uint64_t token;
    if (!timer.Poll(esp_timer_get_time(), token)) return;
    Application::GetInstance().Schedule([token]() {
        {
            std::lock_guard<std::recursive_mutex> lock(mutex);
            if (!timer.CanNotify(token)) return;
        }
        // All commands (UI and MCP) run on this same application task. Release
        // the state lock before display/audio calls to avoid lock inversion.
        if (auto* display = Board::GetInstance().GetDisplay()) display->ShowNotification("Timer finished");
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_ALARM);
    });
}
