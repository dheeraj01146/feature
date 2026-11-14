#pragma once

#include "driver/ledc.h"
#include "driver/gpio.h"
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

// ================= 6-SERVO (2 HEAD + 4 BODY/LIMB) CONFIGURATION =================
// Abhi jitne servos connected hain utna count rakh (1, 2, ya 6).
// Jab saare 6 laga de, bas ACTIVE_SERVO_COUNT ko 6 kar dena!
#define ACTIVE_SERVO_COUNT  2
#define MAX_SERVOS          6

// Servo Indices:
#define SERVO_HEAD_PAN      0   // Neck Left-Right (GPIO 18)
#define SERVO_HEAD_TILT     1   // Neck Up-Down (GPIO 8)
#define SERVO_LEFT_HAND     2   // Left Arm Wave (GPIO 48)
#define SERVO_RIGHT_HAND    3   // Right Arm Wave (GPIO 47)
#define SERVO_LEFT_LEG      4   // Left Leg/Foot Dance (GPIO 38)
#define SERVO_RIGHT_LEG     5   // Right Leg/Foot Dance (GPIO 39)

static const gpio_num_t kServoPins[MAX_SERVOS] = {
    GPIO_NUM_18, // 0: Head Pan (Left/Right)
    GPIO_NUM_8,  // 1: Head Tilt (Up/Down)
    GPIO_NUM_48, // 2: Left Hand
    GPIO_NUM_47, // 3: Right Hand
    GPIO_NUM_38, // 4: Left Leg/Foot
    GPIO_NUM_39  // 5: Right Leg/Foot
};

#define SERVO_LEDC_TIMER    LEDC_TIMER_2
#define SERVO_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_RES      LEDC_TIMER_14_BIT

// 6-Servo Full Pose Frame (Har step mein 6 servos ka angle!)
struct EmoPoseStep {
    float head_pan;     // Servo 0: 90 = Center, 60 = Left, 120 = Right
    float head_tilt;    // Servo 1: 90 = Level, 70 = Look Up, 115 = Look Down
    float left_hand;    // Servo 2: 90 = Down, 35 = Wave Up
    float right_hand;   // Servo 3: 90 = Down, 145 = Wave Up
    float left_leg;     // Servo 4: 90 = Flat, 65/115 = Tilt
    float right_leg;    // Servo 5: 90 = Flat, 65/115 = Tilt
    float speed;        // Movement speed
    int hold_ticks;     // 1 tick = 20ms
};

class ServoController {
public:
    static ServoController& GetInstance() {
        static ServoController instance;
        return instance;
    }

    void Initialize() {
        ledc_timer_config_t timer_conf = {
            .speed_mode = SERVO_LEDC_MODE,
            .duty_resolution = SERVO_LEDC_RES,
            .timer_num = SERVO_LEDC_TIMER,
            .freq_hz = 50,
            .clk_cfg = LEDC_AUTO_CLK
        };
        ledc_timer_config(&timer_conf);

        for (int i = 0; i < ACTIVE_SERVO_COUNT; i++) {
            gpio_reset_pin(kServoPins[i]);
            gpio_set_direction(kServoPins[i], GPIO_MODE_OUTPUT);

            ledc_channel_t ch = static_cast<ledc_channel_t>(LEDC_CHANNEL_2 + i);
            ledc_channel_config_t channel_conf = {
                .gpio_num = kServoPins[i],
                .speed_mode = SERVO_LEDC_MODE,
                .channel = ch,
                .intr_type = LEDC_INTR_DISABLE,
                .timer_sel = SERVO_LEDC_TIMER,
                .duty = 0,
                .hpoint = 0
            };
            ledc_channel_config(&channel_conf);

            current_angles_[i] = 90.0f;
            target_angles_[i] = 90.0f;
            step_speeds_[i] = 1.2f;
            idle_ticks_[i] = 0;
            pwm_detached_[i] = false;
            ApplyHardwareAngle(i, 90.0f);
        }

        xTaskCreate([](void* arg) {
            static_cast<ServoController*>(arg)->SmoothTask();
        }, "servo_smooth", 3072, this, 3, nullptr);
    }

    void SetServoTarget(int idx, float angle, float speed = 1.2f) {
        if (idx < 0 || idx >= ACTIVE_SERVO_COUNT) return;
        if (angle < 35.0f) angle = 35.0f;
        if (angle > 145.0f) angle = 145.0f;
        target_angles_[idx] = angle;
        step_speeds_[idx] = speed;
    }

    // Head Pan + Tilt dono ek sath set karne ke liye
    void SetHead(float pan_angle, float tilt_angle = 90.0f, float speed = 1.2f) {
        seq_len_ = 0;
        SetServoTarget(SERVO_HEAD_PAN, pan_angle, speed);
        SetServoTarget(SERVO_HEAD_TILT, tilt_angle, speed);
    }

    void SetTarget(float pan_angle, float speed = 1.2f) {
        SetHead(pan_angle, 90.0f, speed);
    }

    void PlayPoseSequence(const EmoPoseStep* steps, int count) {
        if (count <= 0 || count > 10) return;
        for (int i = 0; i < count; i++) {
            seq_[i] = steps[i];
        }
        seq_index_ = 0;
        seq_len_ = count;
        seq_wait_ticks_ = seq_[0].hold_ticks;
        ApplyPoseStep(seq_[0]);
    }

    // Video 00:18 Jaisa Full Robot Dance!
    void DanceMove() {
        const EmoPoseStep dance_steps[] = {
            // Pan, Tilt, L-Hand, R-Hand, L-Leg, R-Leg, Speed, Hold
            {65.0f,  75.0f,  45.0f, 135.0f,  65.0f, 115.0f, 2.2f, 12},
            {115.0f, 105.0f, 120.0f,  60.0f, 115.0f,  65.0f, 2.2f, 12},
            {65.0f,  75.0f,  45.0f, 135.0f,  65.0f, 115.0f, 2.2f, 12},
            {115.0f, 105.0f, 120.0f,  60.0f, 115.0f,  65.0f, 2.2f, 12},
            {90.0f,  70.0f,  40.0f,  40.0f,  75.0f, 105.0f, 2.0f, 15},
            {90.0f,  90.0f,  90.0f,  90.0f,  90.0f,  90.0f, 1.5f,  5}
        };
        PlayPoseSequence(dance_steps, 6);
    }

    // Video 06:20 Jaisa Cute Hand Wave + Head Tilt!
    void WaveHand() {
        const EmoPoseStep wave_steps[] = {
            {80.0f, 75.0f, 90.0f, 140.0f, 90.0f, 90.0f, 2.0f, 10},
            {85.0f, 75.0f, 90.0f, 105.0f, 90.0f, 90.0f, 2.2f,  8},
            {80.0f, 75.0f, 90.0f, 140.0f, 90.0f, 90.0f, 2.2f,  8},
            {90.0f, 90.0f, 90.0f,  90.0f, 90.0f, 90.0f, 1.5f,  5}
        };
        PlayPoseSequence(wave_steps, 4);
    }

    // Emotion ke hisaab se 2-Servo Head (Pan + Tilt) + Body Reaction!
    void ReactToEmotion(const char* emotion) {
        if (emotion == nullptr) return;

        if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 || strcmp(emotion, "funny") == 0) {
            const EmoPoseStep happy_moves[] = {
                {68.0f,  72.0f, 55.0f, 125.0f,  75.0f, 105.0f, 2.0f, 10}, // Look up-left + hands up
                {112.0f, 72.0f, 125.0f, 55.0f, 105.0f,  75.0f, 2.0f, 10}, // Look up-right
                {78.0f,  85.0f, 70.0f, 110.0f,  85.0f,  95.0f, 1.8f,  8},
                {90.0f,  90.0f, 90.0f,  90.0f,  90.0f,  90.0f, 1.4f,  5}
            };
            PlayPoseSequence(happy_moves, 4);
        }
        else if (strcmp(emotion, "angry") == 0 || strcmp(emotion, "frustrated") == 0) {
            // Gusse mein gardan thodi neeche झुका कर (Tilt 105°) left-right shake!
            const EmoPoseStep angry_moves[] = {
                {122.0f, 105.0f, 120.0f,  60.0f, 105.0f, 75.0f, 2.5f, 10},
                {58.0f,  105.0f,  60.0f, 120.0f,  75.0f, 105.0f, 2.5f, 10},
                {115.0f, 100.0f, 110.0f,  70.0f,  90.0f, 90.0f, 2.2f, 10},
                {90.0f,   90.0f,  90.0f,  90.0f,  90.0f, 90.0f, 1.6f,  5}
            };
            PlayPoseSequence(angry_moves, 4);
        }
        else if (strcmp(emotion, "thinking") == 0 || strcmp(emotion, "confused") == 0) {
            // Sochte waqt upar-left aasman ki taraf dekhna (Pan 62°, Tilt 70°)
            const EmoPoseStep think_moves[] = {
                {62.0f,  70.0f, 90.0f, 125.0f, 90.0f, 90.0f, 1.0f, 40},
                {110.0f, 75.0f, 90.0f,  90.0f, 90.0f, 90.0f, 0.9f, 35},
                {90.0f,  90.0f, 90.0f,  90.0f, 90.0f, 90.0f, 1.0f, 10}
            };
            PlayPoseSequence(think_moves, 3);
        }
        else if (strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0) {
            // Jhatke se gardan upar (Tilt 65°) aur left-right double-take!
            const EmoPoseStep shock_moves[] = {
                {55.0f,  65.0f, 45.0f, 135.0f, 90.0f, 90.0f, 2.8f,  8},
                {125.0f, 65.0f, 45.0f, 135.0f, 90.0f, 90.0f, 2.8f, 12},
                {90.0f,  90.0f, 90.0f,  90.0f, 90.0f, 90.0f, 2.0f,  5}
            };
            PlayPoseSequence(shock_moves, 3);
        }
        else if (strcmp(emotion, "cool") == 0 || strcmp(emotion, "wink") == 0 || strcmp(emotion, "confident") == 0) {
            // Swag look + Wave!
            WaveHand();
        }
        else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
            // Sad mein gardan neeche jhuka lena (Tilt 118°)
            SetHead(72.0f, 118.0f, 0.5f);
        }
        else if (strcmp(emotion, "sleepy") == 0) {
            // Sote waqt gardan aaram se neeche (Tilt 115°) aur side mein (Pan 78°)
            SetHead(78.0f, 115.0f, 0.4f);
        }
        else if (strcmp(emotion, "left") == 0) {
            SetHead(58.0f, 85.0f, 1.4f);
        }
        else if (strcmp(emotion, "right") == 0) {
            SetHead(122.0f, 85.0f, 1.4f);
        }
        else {
            SetHead(90.0f, 90.0f, 1.2f);
        }
    }

    void SetDirection(const char* emotion) {
        ReactToEmotion(emotion);
    }

    void OnIdleTick() {}

    void OnListening() {
        // Jab user bole, toh gardan thodi upar utha kar (Tilt 78° = Attention!) sune
        SetHead(90.0f, 78.0f, 1.8f);
    }

    void OnSpeakingTick() {
        if (seq_len_ > 0) return;

        static int action_cooldown = 0;
        if (action_cooldown > 0) {
            action_cooldown--;
            return;
        }

        // Bolte waqt 2-Servo Head Pan + Tilt Nodding (Haan mein gardan hilana + Left/Right)
        int r = esp_random() % 100;
        if (r < 35) {
            SetHead(80.0f + (esp_random() % 6), 82.0f, 0.7f); // Slight left + up nod
            action_cooldown = 1;
        } else if (r < 70) {
            SetHead(94.0f + (esp_random() % 6), 96.0f, 0.7f); // Slight right + down nod
            action_cooldown = 1;
        } else {
            SetHead(90.0f, 88.0f, 0.6f);
            action_cooldown = 1;
        }
    }

private:
    ServoController() = default;

    float current_angles_[MAX_SERVOS];
    float target_angles_[MAX_SERVOS];
    float step_speeds_[MAX_SERVOS];
    int idle_ticks_[MAX_SERVOS];
    bool pwm_detached_[MAX_SERVOS];

    EmoPoseStep seq_[10];
    int seq_len_ = 0;
    int seq_index_ = 0;
    int seq_wait_ticks_ = 0;

    void ApplyPoseStep(const EmoPoseStep& s) {
        SetServoTarget(SERVO_HEAD_PAN,   s.head_pan,   s.speed);
        SetServoTarget(SERVO_HEAD_TILT,  s.head_tilt,  s.speed);
        SetServoTarget(SERVO_LEFT_HAND,  s.left_hand,  s.speed);
        SetServoTarget(SERVO_RIGHT_HAND, s.right_hand, s.speed);
        SetServoTarget(SERVO_LEFT_LEG,   s.left_leg,   s.speed);
        SetServoTarget(SERVO_RIGHT_LEG,  s.right_leg,  s.speed);
    }

    void ApplyHardwareAngle(int idx, float angle) {
        ledc_channel_t ch = static_cast<ledc_channel_t>(LEDC_CHANNEL_2 + idx);
        float pulse_us = 500.0f + (angle / 180.0f) * 2000.0f;
        uint32_t duty = (uint32_t)((pulse_us / 20000.0f) * 16384.0f);
        ledc_set_duty(SERVO_LEDC_MODE, ch, duty);
        ledc_update_duty(SERVO_LEDC_MODE, ch);
        pwm_detached_[idx] = false;
    }

    void DetachIdlePwm(int idx) {
        if (!pwm_detached_[idx]) {
            ledc_channel_t ch = static_cast<ledc_channel_t>(LEDC_CHANNEL_2 + idx);
            ledc_set_duty(SERVO_LEDC_MODE, ch, 0);
            ledc_update_duty(SERVO_LEDC_MODE, ch);
            pwm_detached_[idx] = true;
        }
    }

    void SmoothTask() {
        while (true) {
            bool any_moving = false;

            for (int i = 0; i < ACTIVE_SERVO_COUNT; i++) {
                float diff = target_angles_[i] - current_angles_[i];
                if (std::abs(diff) > 0.3f) {
                    any_moving = true;
                    idle_ticks_[i] = 0;
                    if (std::abs(diff) < step_speeds_[i]) {
                        current_angles_[i] = target_angles_[i];
                    } else {
                        current_angles_[i] += (diff > 0) ? step_speeds_[i] : -step_speeds_[i];
                    }
                    ApplyHardwareAngle(i, current_angles_[i]);
                } else {
                    idle_ticks_[i]++;
                    if (idle_ticks_[i] > 50) {
                        DetachIdlePwm(i);
                    }
                }
            }

            if (!any_moving && seq_len_ > 0) {
                if (seq_wait_ticks_ > 0) {
                    seq_wait_ticks_--;
                } else {
                    seq_index_++;
                    if (seq_index_ < seq_len_) {
                        seq_wait_ticks_ = seq_[seq_index_].hold_ticks;
                        ApplyPoseStep(seq_[seq_index_]);
                    } else {
                        seq_len_ = 0;
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
};
