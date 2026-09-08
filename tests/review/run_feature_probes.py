"""Compile actual menu and timer callbacks against host fakes for regression tests.

Usage: python tests/review/run_feature_probes.py --compiler PATH_TO_ZIG
No firmware or device is changed. Any FAIL is a regression.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
board = root / "main/boards/CyberVoc-Board-V2_0"
source = (board / "customer_ui/feature_ui.cc").read_text(encoding="utf-8")
bridge = (board / "ui_bridge.cc").read_text(encoding="utf-8")
service = (board / "customer_ui/timer_service.cc").read_text(encoding="utf-8")
prefix = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <functional>
#include <vector>
#include <atomic>
using lv_coord_t = int;
enum lv_event_code_t { LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_PRESS_LOST, LV_EVENT_LONG_PRESSED, LV_EVENT_CLICKED };
struct lv_point_t { int x, y; };
struct lv_indev_t {};
struct lv_event_t { lv_event_code_t code; };
lv_indev_t input;
lv_point_t current_point;
lv_event_code_t lv_event_get_code(lv_event_t* e) { return e->code; }
lv_indev_t* lv_indev_get_act() { return &input; }
void lv_indev_get_point(lv_indev_t*, lv_point_t* p) { *p = current_point; }
constexpr int UI_BRIDGE_CENTER_X = 180, UI_BRIDGE_CENTER_Y = 180;
#define LV_ABS(x) std::abs(x)
struct { int start_x, start_y; bool handled; } s_gesture_state;
bool s_pointer_cancelled = false;
std::atomic<uint32_t> s_ignore_click_until_tick{0};
uint32_t lv_tick_get() { return 1000; }
bool menu_long_press_handled = false;
int pressed_item = -1, menu_actions = 0, long_actions = 0;
void HandleMenuItem(int, bool detail) { ++menu_actions; if (detail) ++long_actions; }
enum feature_timer_state_t { FEATURE_TIMER_IDLE, FEATURE_TIMER_RUNNING, FEATURE_TIMER_PAUSED, FEATURE_TIMER_FINISHED };
int64_t fake_now = 0;
int64_t esp_timer_get_time() { return fake_now; }
int sounds = 0;
namespace Lang { namespace Sounds { constexpr int OGG_ALARM = 1; } }
struct Display { void ShowNotification(const char*) {} };
struct Board {
    Display display;
    static Board& GetInstance() { static Board b; return b; }
    Display* GetDisplay() { return &display; }
};
struct Application {
    std::vector<std::function<void()>> pending;
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> fn) { pending.push_back(fn); }
    void PlaySound(int) { ++sounds; }
    void Drain() { auto copy = std::move(pending); pending.clear(); for (auto& fn : copy) fn(); }
};
'''
callbacks = "\n".join(function(bridge, name) for name in (
    "static bool ui_bridge_should_ignore_click(", "void ui_bridge_track_pointer(",
    "bool ui_bridge_click_allowed(", "void ui_bridge_cancel_pointer("))
callbacks += "\n" + "\n".join(function(source, name) for name in ("int HitMenuItem(", "void MenuEvent("))
callbacks += "\n" + "\n".join(line for line in service.splitlines() if not line.startswith("#include"))
main = r'''
int failures = 0;
void Check(const char* name, bool ok) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; }
void Event(lv_event_code_t code) { lv_event_t event{code}; MenuEvent(&event); }
void Press(int x, int y) {
    current_point = {x,y}; s_gesture_state = {x,y,false}; s_pointer_cancelled = false;
    s_ignore_click_until_tick = 0; menu_actions = long_actions = 0; Event(LV_EVENT_PRESSED);
}
int main() {
    // The Figma ring has deliberate gaps at 0/60/... degrees. Exercise the
    // centre of the top-right segment (30 degrees) rather than the top gap.
    Press(245,67); current_point = {265,67}; Event(LV_EVENT_PRESSING); current_point = {245,67}; Event(LV_EVENT_CLICKED);
    Check("drag out and back must not click", menu_actions == 0);
    Press(245,67); current_point = {249,70}; Event(LV_EVENT_CLICKED);
    Check("tap tolerates normal jitter", menu_actions == 1);
    Press(245,67); Event(LV_EVENT_LONG_PRESSED); Event(LV_EVENT_CLICKED);
    Check("long press executes once, without toggle", menu_actions == 1 && long_actions == 1);
    Press(245,67); Event(LV_EVENT_PRESS_LOST); Event(LV_EVENT_CLICKED);
    Check("lost press cancels action", menu_actions == 0);
    Press(245,67); s_ignore_click_until_tick = 1200; Event(LV_EVENT_CLICKED);
    Check("page gesture suppression guards menu", menu_actions == 0);
    Check("center, top gap and outside circle are not items",
          HitMenuItem(180,180) == -1 && HitMenuItem(180,50) == -1 && HitMenuItem(0,0) == -1);
    Check("duration limits", !feature_timer_start_minutes(0) && !feature_timer_start_minutes(1441));
    feature_timer_start_minutes(1); fake_now = 60000000; feature_timer_poll(); feature_timer_cancel();
    Application::GetInstance().Drain(); Check("cancel invalidates queued alarm", sounds == 0);
    feature_timer_start_minutes(1); fake_now += 60000000; feature_timer_poll(); feature_timer_start_minutes(2);
    Application::GetInstance().Drain(); Check("restart invalidates queued alarm", sounds == 0 && feature_timer_get_remaining_seconds() == 120);
    fake_now += 10000000; Check("pause", feature_timer_pause());
    fake_now += 100000000; feature_timer_poll(); Check("paused time does not elapse", feature_timer_get_remaining_seconds() == 110);
    Check("resume", feature_timer_resume()); fake_now += 110000000;
    Check("expiry wins over late pause", !feature_timer_pause()); feature_timer_poll(); feature_timer_poll();
    Application::GetInstance().Drain(); Check("exactly one expiry sound", sounds == 1 && feature_timer_get_state() == FEATURE_TIMER_FINISHED);
    feature_timer_adjust_minutes(-2000); Check("adjust lower bound", feature_timer_get_selected_minutes() == 1);
    feature_timer_adjust_minutes(2000); Check("adjust upper bound", feature_timer_get_selected_minutes() == 1440);
    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory(prefix="miaoban-feature-review-") as tmp:
    cpp, exe = Path(tmp) / "probe.cc", Path(tmp) / "probe.exe"
    timer_header = (board / "customer_ui/timer_state.h").as_posix()
    cpp.write_text(f'#include "{timer_header}"\n' + prefix + callbacks + main, encoding="utf-8")
    subprocess.run([args.compiler, "c++", "-std=c++17", str(cpp), "-o", str(exe)], check=True)
    raise SystemExit(subprocess.run([str(exe)]).returncode)
