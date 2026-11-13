#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float last[18], resume_accel[3];
    bool blocked;
    uint64_t resume_ns;
} MotionGate;

static inline void motion_gate_init(MotionGate *g) {
    memset(g, 0, sizeof *g);
    g->last[2] = -1;
    g->last[9] = g->last[13] = g->last[17] = 1;
}

// Keep the reported gravity and orientation continuous at menu transitions.
// A synthetic identity pose is a physical jerk to the game's shake detector.
static inline void motion_gate_sample(MotionGate *g, float sample[18], bool blocked, uint64_t now) {
    if (blocked) {
        memcpy(sample, g->last, sizeof g->last);
        memset(sample + 3, 0, 3 * sizeof(float));
        g->blocked = true;
    } else {
        if (g->blocked) {
            memcpy(g->resume_accel, g->last, sizeof g->resume_accel);
            g->resume_ns = now;
            g->blocked = false;
        }
        if (g->resume_ns && now - g->resume_ns < 250000000ull) {
            float blend = (float)(now - g->resume_ns) / 250000000.0f;
            for (int i = 0; i < 3; i++) sample[i] = g->resume_accel[i] + blend * (sample[i] - g->resume_accel[i]);
            memset(sample + 3, 0, 3 * sizeof(float));
        }
    }
    memcpy(g->last, sample, sizeof g->last);
}
