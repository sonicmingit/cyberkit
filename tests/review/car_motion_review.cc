// Review probes for the actual V2.0 classifier. Exit 1 means a regression.
// These synthetic inputs are not hardware calibration evidence.
#include "../../main/boards/CyberVoc-Board-V2_0/car_mode/car_motion.h"
#include <cstdio>
#include <limits>

int failures = 0;
void Check(const char* name, bool ok, car::Event event) {
    std::printf("%s: %s (event=%s)\n", ok ? "PASS" : "FAIL", name, car::Name(event));
    if (!ok) ++failures;
}

struct Replay {
    car::Detector detector;
    int64_t time = 0;
    car::Event event = car::Event::Cruise;
    void Feed(float ax, float ay, float az, float gz = 0, float gy = 0) {
        time += 20;
        event = detector.Feed({{ax, ay, az}, {0, gy, gz}, time});
    }
    void Flat() { for (int i = 0; i < 110; ++i) Feed(0, 0, 1); }
};

int main() {
    {
        Replay r; r.Flat();
        Check("flat calibration", r.detector.Ready(), r.event);
        for (int i = 0; i < 40; ++i) r.Feed(0.2f, 0, 1);
        Check("sustained acceleration", r.event == car::Event::Accelerate, r.event);
    }
    {
        Replay r;
        for (int i = 0; i < 120; ++i) {
            // An isolated vibration sample must not erase the whole stable window.
            r.Feed(i == 45 ? 0.16f : 0.0f, 0, 1, 0, i == 45 ? 9.0f : 0.0f);
        }
        Check("calibration tolerates an isolated noisy sample",
              r.detector.Ready() && r.detector.CalibrationProgress() == 100, r.event);
    }
    {
        Replay r; r.Flat();
        for (int i = 0; i < 40; ++i) r.Feed(0, 0, 1, 20);
        Check("sustained turn", r.event == car::Event::Left, r.event);
    }
    {
        Replay r; r.Flat();
        // Slowly pitch 10 degrees, then remain at rest: gravity only, no translation.
        for (int i = 1; i <= 500; ++i) {
            const float angle = i / 500.0f * 0.174532925f;
            r.Feed(std::sin(angle), 0, std::cos(angle), 0, -1.0f);
        }
        for (int i = 0; i < 100; ++i) r.Feed(0.173648178f, 0, 0.984807753f);
        Check("gravity-only slope should settle to cruise", r.event == car::Event::Cruise, r.event);
    }
    {
        Replay r; r.Flat();
        bool detected = false;
        r.Feed(0, 0, 1.6f); // one sample at the 20 ms sampling interval
        detected |= r.event == car::Event::Bump;
        for (int i = 0; i < 30; ++i) {
            r.Feed(0, 0, 1);
            detected |= r.event == car::Event::Bump;
        }
        Check("single-frame impact visibility", detected, r.event);
    }
    {
        Replay r; r.Flat();
        // Finite but out of configured +/-8 g range must be rejected.
        for (int i = 0; i < 20; ++i) r.Feed(20, 0, 1);
        Check("out-of-range input should invalidate calibration",
              !r.detector.Ready() && r.event == car::Event::SignalLost, r.event);
    }
    {
        Replay r; r.Flat();
        r.Feed(std::numeric_limits<float>::quiet_NaN(), 0, 1);
        Check("NaN recovery", !r.detector.Ready() && r.event == car::Event::SignalLost, r.event);
    }
    {
        Replay r; r.Flat(); r.time += 600; r.Feed(0,0,1);
        Check("sample gap invalidates calibration", !r.detector.Ready(), r.event);
        r.Flat(); Check("recalibrates after recovery", r.detector.Ready(), r.event);
    }
    {
        Replay r; r.detector.Reset(2,1); r.Flat();
        Check("vertical forward axis is rejected", !r.detector.Ready() && r.detector.AxisInvalid(), r.event);
        r.detector.Reset(0,-1); r.Flat();
        for (int i=0; i<40; ++i) r.Feed(0.5f,0,1);
        Check("reversed forward axis detects braking", r.event == car::Event::HardBrake, r.event);
    }
    {
        Replay r; r.Flat();
        for (int i=0; i<1500; ++i) r.Feed(0.2f,0,1);
        Check("30 second acceleration is not absorbed into gravity", r.event == car::Event::Accelerate, r.event);
    }
    std::printf("Review probe failures: %d\n", failures);
    return failures ? 1 : 0;
}
