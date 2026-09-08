#pragma once

#include <cmath>
#include <cstdint>

// Platform-independent motion classifier. Acceleration is expressed in g and
// gyro data in degrees/second. It deliberately does not infer ignition, gear,
// pedal position, or absolute vehicle speed.
namespace car {

enum class Event {
    Cruise,
    Accelerate,
    Decelerate,
    HardBrake,
    Bump,
    Left,
    Right,
    SignalLost,
};

inline const char* Name(Event event) {
    static constexpr const char* kNames[] = {
        "cruise", "accelerate", "decelerate", "hard_brake",
        "bump", "turn_left", "turn_right", "signal_lost",
    };
    return kNames[static_cast<unsigned>(event)];
}

struct Sample {
    float acceleration[3];
    float gyro[3];
    int64_t timestamp_ms;
};

class Detector {
public:
    void Reset(int forward_axis = 0, int forward_sign = 1) {
        *this = Detector();
        forward_axis_ = forward_axis >= 0 && forward_axis < 3 ? forward_axis : 0;
        forward_sign_ = forward_sign < 0 ? -1 : 1;
    }

    bool Ready() const { return ready_; }
    bool AxisInvalid() const { return axis_invalid_; }
    int CalibrationProgress() const {
        if (ready_) return 100;
        return sample_count_ >= 100 ? 99 : sample_count_;
    }

    Event Feed(const Sample& sample) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(sample.acceleration[i]) || !std::isfinite(sample.gyro[i]) ||
                std::fabs(sample.acceleration[i]) >= 8.0f || std::fabs(sample.gyro[i]) >= 512.0f) {
                Reset(forward_axis_, forward_sign_);
                return Event::SignalLost;
            }
        }

        if (last_timestamp_ms_ != 0 &&
            (sample.timestamp_ms <= last_timestamp_ms_ || sample.timestamp_ms - last_timestamp_ms_ > 500)) {
            Reset(forward_axis_, forward_sign_);
        }
        const float dt = last_timestamp_ms_ ? (sample.timestamp_ms - last_timestamp_ms_) / 1000.0f : 0.02f;
        last_timestamp_ms_ = sample.timestamp_ms;

        if (!ready_) {
            Calibrate(sample);
            return Event::Cruise;
        }

        float linear[3], gyro[3], omega[3];
        for (int i = 0; i < 3; ++i) {
            gyro[i] = sample.gyro[i] - gyro_bias_[i];
            omega[i] = gyro[i] * 0.01745329252f;
        }
        // Gravity rotates opposite to the device angular velocity in body coordinates.
        const float cross[3] = {omega[1]*gravity_[2] - omega[2]*gravity_[1],
                                omega[2]*gravity_[0] - omega[0]*gravity_[2],
                                omega[0]*gravity_[1] - omega[1]*gravity_[0]};
        for (int i = 0; i < 3; ++i) gravity_[i] -= cross[i] * dt;
        float norm = std::sqrt(Dot(gravity_, gravity_));
        for (int i = 0; i < 3; ++i) gravity_[i] *= gravity_norm_ / norm;
        float residual[3];
        for (int i = 0; i < 3; ++i) residual[i] = sample.acceleration[i] - gravity_[i];
        // Correct small integration drift only; do not absorb sustained linear acceleration.
        if (Dot(residual, residual) < 0.0016f && Dot(gyro, gyro) < 9.0f) {
            const float correction = dt / (5.0f + dt);
            for (int i = 0; i < 3; ++i) gravity_[i] += correction * residual[i];
        }
        norm = std::sqrt(Dot(gravity_, gravity_));
        for (int i = 0; i < 3; ++i) {
            up_[i] = gravity_[i] / norm;
            forward_[i] = (i == forward_axis_ ? 1.0f : 0.0f) - up_[forward_axis_] * up_[i];
            linear[i] = sample.acceleration[i] - gravity_[i];
        }
        norm = std::sqrt(Dot(forward_, forward_));
        if (norm < 0.5f) {
            Reset(forward_axis_, forward_sign_);
            axis_invalid_ = true;
            return Event::Cruise;
        }
        for (float& value : forward_) value *= forward_sign_ / norm;

        const float alpha = dt / (0.08f + dt);
        longitudinal_ += alpha * (Dot(linear, forward_) - longitudinal_);
        yaw_ += alpha * (Dot(gyro, up_) - yaw_);

        if (std::fabs(Dot(linear, up_)) > 0.40f && sample.timestamp_ms >= bump_cooldown_ms_ &&
            event_ != Event::HardBrake) {
            bump_until_ms_ = sample.timestamp_ms + 400;
            bump_cooldown_ms_ = sample.timestamp_ms + 1000;
        }

        Event candidate = Event::Cruise;
        if (longitudinal_ < -0.30f) {
            candidate = Event::HardBrake;
        } else if (sample.timestamp_ms < bump_until_ms_) {
            candidate = Event::Bump;
        } else if (yaw_ > 12.0f) {
            candidate = Event::Left;
        } else if (yaw_ < -12.0f) {
            candidate = Event::Right;
        } else if (longitudinal_ < -0.10f) {
            candidate = Event::Decelerate;
        } else if (longitudinal_ > 0.10f) {
            candidate = Event::Accelerate;
        } else if ((event_ == Event::Left && yaw_ > 6.0f) ||
                   (event_ == Event::Right && yaw_ < -6.0f)) {
            candidate = event_;
        }

        if (candidate != candidate_) {
            candidate_ = candidate;
            candidate_since_ms_ = sample.timestamp_ms;
        }

        const int dwell_ms = candidate == Event::Bump ? 0 :
                             candidate == Event::HardBrake ? 100 : 240;
        if (sample.timestamp_ms - candidate_since_ms_ >= dwell_ms) {
            event_ = candidate;
        }
        return event_;
    }

private:
    static float Dot(const float* lhs, const float* rhs) {
        return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
    }

    void ClearCalibration() {
        sample_count_ = 0;
        unstable_count_ = 0;
        calibration_start_ms_ = 0;
        for (int i = 0; i < 3; ++i) {
            acceleration_sum_[i] = 0;
            gyro_bias_[i] = 0;
        }
    }

    void Calibrate(const Sample& sample) {
        float norm = std::sqrt(Dot(sample.acceleration, sample.acceleration));
        // QMI8658 samples can contain isolated vibration/noise spikes on a
        // desk or in a parked vehicle. Do not throw away the entire two-second
        // window for one bad sample; six consecutive unstable samples restart it.
        bool still = norm > 0.75f && norm < 1.25f;
        for (int i = 0; i < 3; ++i) {
            still = still && std::fabs(sample.gyro[i]) < 8.0f;
            if (sample_count_ > 0) {
                still = still &&
                        std::fabs(sample.acceleration[i] - acceleration_sum_[i] / sample_count_) < 0.10f;
            }
        }
        if (!still) {
            if (++unstable_count_ >= 6) ClearCalibration();
            return;
        }
        unstable_count_ = 0;

        if (sample_count_ == 0) {
            calibration_start_ms_ = sample.timestamp_ms;
        }
        for (int i = 0; i < 3; ++i) {
            acceleration_sum_[i] += sample.acceleration[i];
            gyro_bias_[i] += sample.gyro[i];
        }
        ++sample_count_;
        if (sample.timestamp_ms - calibration_start_ms_ < 2000 || sample_count_ < 50) {
            return;
        }

        for (int i = 0; i < 3; ++i) {
            gravity_[i] = acceleration_sum_[i] / sample_count_;
            gyro_bias_[i] /= sample_count_;
        }
        norm = std::sqrt(Dot(gravity_, gravity_));
        gravity_norm_ = norm;
        for (int i = 0; i < 3; ++i) {
            up_[i] = gravity_[i] / norm;
            forward_[i] = (i == forward_axis_ ? 1.0f : 0.0f) - up_[forward_axis_] * up_[i];
        }
        norm = std::sqrt(Dot(forward_, forward_));
        if (norm < 0.5f) {
            ClearCalibration();
            axis_invalid_ = true;
            return;
        }
        for (float& value : forward_) {
            value = value / norm * forward_sign_;
        }
        ready_ = true;
        axis_invalid_ = false;
    }

    int forward_axis_ = 0;
    int forward_sign_ = 1;
    int sample_count_ = 0;
    int unstable_count_ = 0;
    bool ready_ = false;
    bool axis_invalid_ = false;
    float gravity_norm_ = 1.0f;
    int64_t bump_until_ms_ = 0;
    int64_t bump_cooldown_ms_ = 0;
    float acceleration_sum_[3]{};
    float gravity_[3]{};
    float up_[3]{};
    float forward_[3]{};
    float gyro_bias_[3]{};
    float longitudinal_ = 0;
    float yaw_ = 0;
    int64_t last_timestamp_ms_ = 0;
    int64_t calibration_start_ms_ = 0;
    int64_t candidate_since_ms_ = 0;
    Event event_ = Event::Cruise;
    Event candidate_ = Event::Cruise;
};

}  // namespace car
