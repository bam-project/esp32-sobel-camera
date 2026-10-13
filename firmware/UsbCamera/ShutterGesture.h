// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

// Feed timestamped electrical transitions, then the latest level each loop.
// Debouncing uses elapsed time, including when a preview frame delays polling.
class ShutterGesture {
public:
    enum Event : uint8_t { None = 0, Photo = 1, TogglePreview = 2 };

    static constexpr uint32_t DebounceMs = 30;
    static constexpr uint32_t HoldMs = 900;

    // Discard the current press and wait for a debounced release before rearming.
    void reset(uint32_t nowMs, bool isPressed) {
        rawPressed = isPressed;
        stablePressed = true;
        readyForPress = false;
        pressInProgress = false;
        holdReported = false;
        lastTransitionAtMs = nowMs;
    }

    // Advance timing for the previous level, then record the next electrical transition.
    // Return event flags for a short release or a long hold.
    uint8_t update(uint32_t nowMs, bool isPressed) {
        uint8_t events = advanceState(nowMs);
        if (isPressed != rawPressed) {
            rawPressed = isPressed;
            lastTransitionAtMs = nowMs;
        }
        return events;
    }

private:
    bool rawPressed = true;
    bool stablePressed = true;
    bool readyForPress = false;
    bool pressInProgress = false;
    bool holdReported = false;
    uint32_t lastTransitionAtMs = 0;
    uint32_t pressStartedAtMs = 0;

    // Debounce transitions and emit at most one hold event during a continuous press.
    uint8_t advanceState(uint32_t nowMs) {
        uint8_t events = None;
        if (rawPressed != stablePressed && uint32_t(nowMs - lastTransitionAtMs) >= DebounceMs) {
            stablePressed = rawPressed;
            if (stablePressed) {
                if (readyForPress) {
                    pressInProgress = true;
                    holdReported = false;
                    pressStartedAtMs = lastTransitionAtMs;
                    readyForPress = false;
                }
            } else {
                if (pressInProgress && !holdReported) {
                    events = Photo;
                }
                pressInProgress = false;
                readyForPress = true;
            }
        }
        if (pressInProgress && !holdReported && rawPressed &&
            uint32_t(nowMs - pressStartedAtMs) >= HoldMs) {
            holdReported = true;
            events |= TogglePreview;
        }
        return events;
    }
};
