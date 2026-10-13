// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <U8g2lib.h>

// OLED error feedback, separate from successful photo celebrations.
class ErrorAnimation {
public:
    enum class Kind : uint8_t {
        CameraStartup,
        CameraUnavailable,
        CardUnavailable,
        FolderCreate,
        PhotoNamesFull,
        CaptureFailed,
        InvalidJpeg,
        CardWrite,
        CardFull,
        Count
    };
    static constexpr uint8_t Count = uint8_t(Kind::Count);
    static constexpr uint32_t DurationMs = 3000;

    // Start an error animation, falling back to CameraUnavailable for an invalid kind.
    void start(uint32_t nowMs, Kind kind) {
        currentKind = uint8_t(kind) < Count ? kind : Kind::CameraUnavailable;
        startedAtMs = nowMs;
        active = true;
        lastFrameIndex = 255;
    }

    // Release the display so another animation or the live preview can draw.
    void cancel() {
        active = false;
    }

    // Return the heading for the current error category.
    const char* title() const {
        switch (currentKind) {
            case Kind::CameraStartup:
            case Kind::CameraUnavailable:
                return "Camera error";
            case Kind::CardUnavailable:
                return "SD unavailable";
            case Kind::CaptureFailed:
            case Kind::InvalidJpeg:
                return "Capture failed";
            case Kind::CardFull:
                return "Card full";
            default:
                return "Save failed";
        }
    }

    // Return the specific failure reason shown below the icon.
    const char* detail() const {
        switch (currentKind) {
            case Kind::CameraStartup:
                return "Could not start";
            case Kind::FolderCreate:
                return "Cannot make folder";
            case Kind::PhotoNamesFull:
                return "Photo names full";
            case Kind::InvalidJpeg:
                return "Invalid JPEG";
            default:
                return "Photo not saved";
        }
    }

    // Return the suggested recovery action shown on the last display line.
    const char* action() const {
        switch (currentKind) {
            case Kind::CameraStartup:
            case Kind::CameraUnavailable:
                return "Restart camera";
            case Kind::CardUnavailable:
                return "Insert FAT32 card";
            case Kind::FolderCreate:
                return "Reinsert card & restart";
            case Kind::CaptureFailed:
            case Kind::InvalidJpeg:
                return "Try again";
            case Kind::CardFull:
            case Kind::PhotoNamesFull:
                return "Move photos to PC";
            default:
                return "Check SD card";
        }
    }

    // Advance even when camera initialization failed. On expiry the caller can
    // keep the final frame visible if no live preview is available.
    bool update(U8G2& display, uint32_t nowMs) {
        if (!active) {
            return false;
        }
        uint32_t elapsedMs = nowMs - startedAtMs;
        if (elapsedMs >= DurationMs) {
            active = false;
            return false;
        }
        uint8_t frameIndex = elapsedMs * 6 / DurationMs;
        if (frameIndex == lastFrameIndex) {
            return true;
        }
        lastFrameIndex = frameIndex;
        drawFrame(display, frameIndex);
        display.sendBuffer();
        return true;
    }

private:
    Kind currentKind = Kind::CameraUnavailable;
    bool active = false;
    uint32_t startedAtMs = 0;
    uint8_t lastFrameIndex = 255;

    // Center text within the 128-pixel display width.
    static void drawCenteredText(U8G2& display, uint8_t baseline, const char* text) {
        int textWidth = display.getStrWidth(text);
        display.drawStr(textWidth < 128 ? (128 - textWidth) / 2 : 0, baseline, text);
    }

    // Render the error-specific icon with a short horizontal shake and recovery text.
    void drawFrame(U8G2& display, uint8_t frameIndex) const {
        display.clearBuffer();
        display.setDrawColor(1);
        display.setFont(u8g2_font_6x12_tf);
        drawCenteredText(display, 11, title());
        const int shakeOffsets[] = {0, -2, 2, -2, 2, 0};
        const int iconX = 52 + shakeOffsets[frameIndex];
        switch (currentKind) {
            case Kind::CameraStartup:
            case Kind::CameraUnavailable:
            case Kind::CaptureFailed:
                display.drawRFrame(iconX - 4, 23, 30, 18, 3);
                display.drawFrame(iconX + 1, 19, 10, 5);
                display.drawCircle(iconX + 11, 32, 6);
                display.drawCircle(iconX + 11, 32, 3);
                display.drawBox(iconX + 18, 26, 4, 2);
                break;
            case Kind::InvalidJpeg:
                display.drawFrame(iconX - 2, 19, 27, 23);
                display.drawDisc(iconX + 4, 24, 2);
                display.drawLine(iconX + 1, 38, iconX + 9, 29);
                display.drawLine(iconX + 9, 29, iconX + 15, 35);
                display.drawLine(iconX + 15, 35, iconX + 19, 30);
                display.drawLine(iconX + 19, 30, iconX + 22, 38);
                display.drawLine(iconX, 20, iconX + 23, 41);
                break;
            case Kind::FolderCreate:
                display.drawLine(iconX - 5, 24, iconX - 5, 40);
                display.drawLine(iconX - 5, 40, iconX + 25, 40);
                display.drawLine(iconX + 25, 40, iconX + 25, 24);
                display.drawLine(iconX + 25, 24, iconX + 9, 24);
                display.drawLine(iconX + 9, 24, iconX + 5, 20);
                display.drawLine(iconX + 5, 20, iconX - 5, 20);
                display.drawLine(iconX - 5, 20, iconX - 5, 24);
                display.drawHLine(iconX - 4, 27, 28);
                break;
            case Kind::PhotoNamesFull:
                display.drawFrame(iconX - 5, 18, 25, 20);
                display.drawBox(iconX, 23, 25, 19);
                display.setDrawColor(0);
                display.drawBox(iconX + 1, 24, 23, 17);
                display.setDrawColor(1);
                display.drawHLine(iconX + 5, 30, 15);
                display.drawHLine(iconX + 5, 35, 15);
                display.drawVLine(iconX + 9, 27, 12);
                display.drawVLine(iconX + 16, 27, 12);
                break;
            default:
                // Memory-card outline; the full-card version preserves its filled gauge.
                display.drawLine(iconX + 5, 18, iconX + 22, 18);
                display.drawLine(iconX + 22, 18, iconX + 22, 40);
                display.drawLine(iconX + 22, 40, iconX, 40);
                display.drawLine(iconX, 40, iconX, 23);
                display.drawLine(iconX, 23, iconX + 5, 18);
                for (int contactIndex = 0; contactIndex < 3; contactIndex++) {
                    display.drawBox(iconX + 8 + contactIndex * 4, 21, 2, 5);
                }
                display.drawFrame(iconX + 4, 30, 15, 7);
                if (currentKind == Kind::CardFull) {
                    display.drawBox(iconX + 6, 32, 11, 3);
                } else {
                    display.drawLine(iconX + 6, 32, iconX + 16, 35);
                    display.drawLine(iconX + 6, 35, iconX + 16, 32);
                }
                break;
        }
        display.drawBox(iconX + 28, 24, 2, 8);
        display.drawBox(iconX + 28, 35, 2, 2);
        display.setFont(u8g2_font_5x7_tf);
        drawCenteredText(display, 51, detail());
        drawCenteredText(display, 62, action());
    }
};
