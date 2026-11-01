#pragma once
// R36S gamepad -> guest input, PlayStation 2 layout (by position: Cross = bottom, Circle = right,
// Square = left, Triangle = top; on the R36S those are the buttons labelled B, A, Y, X).
//
// Buttons always reach the game as its PC keyboard keys (the game's own default PC key bindings), never
// as joystick buttons: the game's joystick bindings do not include pause/reset, and sending both made
// every press arrive twice. The joystick only carries the analog steering (left stick X) while driving,
// so steering stays proportional. Keys are recomputed from the whole pad state on every event, so a
// button and the d-pad that both mean "left" never release each other.
//
// Driving (NFSU2 PS2 defaults): Cross accelerate, Square brake/reverse, Circle e-brake, Triangle camera,
//   R1 nitrous, L1 look back, R2/L2 shift up/down (manual gearbox), Start pause, Select reset car,
//   left stick steer. D-pad as on the consoles: Up engage event / enter shop (Enter), Down career status (1),
//   Left world map (M), Right SMS (Tab).
// Menus: d-pad / left stick move, Cross or Start accept, Circle or Triangle back, Square = Space.
#include <SDL.h>
#include <array>
#include <cmath>
#include <cstdint>

class SdlGamepad {
public:
    enum Button : unsigned {A,B,X,Y,L1,R1,L2,R2,Start,Select,ThumbL,ThumbR,Up,Down,Left,Right,ButtonCount};

    static int buttonFor(uint8_t b) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return A;            // south (R36S: physical B)
        case SDL_CONTROLLER_BUTTON_B: return B;            // east  (R36S: physical A)
        case SDL_CONTROLLER_BUTTON_X: return X;            // west  (R36S: physical Y)
        case SDL_CONTROLLER_BUTTON_Y: return Y;            // north (R36S: physical X)
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return L1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return R1;
        case SDL_CONTROLLER_BUTTON_START: return Start;
        case SDL_CONTROLLER_BUTTON_BACK: return Select;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ThumbL;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ThumbR;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return Up;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Down;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Left;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Right;
        default: return -1;
        }
    }
    template<class SetKey> bool button(uint8_t sdlButton, bool down, bool driving, SetKey&& setKey) {
        const int b = buttonFor(sdlButton);
        if (b < 0) return false;
        buttons_[b] = down;
        if (!down) latched_[b] = false;
        apply(driving, setKey);
        return true;
    }
    template<class SetKey> void axis(uint8_t sdlAxis, int16_t raw, bool driving, SetKey&& setKey) {
        const float v = raw < 0 ? raw / 32768.0f : raw / 32767.0f;
        switch (sdlAxis) {
        case SDL_CONTROLLER_AXIS_LEFTX: stickX_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_LEFTY: stickY_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: rightX_ = dead(v); break;
        case SDL_CONTROLLER_AXIS_RIGHTY: rightY_ = dead(v); break;
        // R36S L2/R2 are digital buttons that SDL reports as a trigger axis (0 or 32767).
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: leftTrigger_ = v > 0.05f ? v : 0.0f; break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: rightTrigger_ = v > 0.05f ? v : 0.0f; break;
        default: return;
        }
        apply(driving, setKey);
    }
    // Returns true when the driving/menu mode changed (the joystick state must be republished).
    template<class SetKey> bool refresh(bool driving, SetKey&& setKey) {
        if (driving == driving_) return false;
        apply(driving, setKey);
        return true;
    }
    template<class SetKey> void releaseAll(SetKey&& setKey) {
        buttons_.fill(false);
        latched_.fill(false);
        stickX_ = stickY_ = rightX_ = rightY_ = leftTrigger_ = rightTrigger_ = 0;
        apply(driving_, setKey);
    }
    bool held(Button b) const { return buttons_[b]; }

    // The guest joystick: left stick X while driving (proportional steering), nothing else.
    void padState(float axes[6], uint32_t& buttons, int& hatX, int& hatY) const {
        for (int i = 0; i < 6; ++i) axes[i] = 0;
        axes[0] = driving_ ? stickX_ : 0.0f;
        buttons = 0;
        hatX = hatY = 0;
    }

private:
    std::array<bool, ButtonCount> buttons_{};
    // Buttons held across a menu/driving switch send nothing until released.
    std::array<bool, ButtonCount> latched_{};
    float stickX_ = 0, stickY_ = 0, rightX_ = 0, rightY_ = 0, leftTrigger_ = 0, rightTrigger_ = 0;
    std::array<bool, 256> sent_{};
    bool driving_ = false;

    static float dead(float v) { return std::fabs(v) < 0.10f ? 0.0f : v; }

    template<class SetKey> void apply(bool driving, SetKey& setKey) {
        std::array<bool, 256> want{};
        if (driving != driving_) {
            latched_ = buttons_;
            // Cross held through the switch into a race keeps accelerating, as on the consoles.
            if (driving) latched_[A] = false;
        }
        auto on = [&](Button b) { return buttons_[b] && !latched_[b]; };
        // Hysteresis keeps a stick resting near the threshold from chattering.
        auto past = [&](float value, unsigned scan) { return value > (sent_[scan] ? .25f : .4f); };
        if (driving) {
            // PC default keys: Up/Down throttle/brake, Space e-brake, Left Alt nitrous, C camera,
            // B look back, Left Shift / Left Ctrl shift up/down, Escape pause, R reset; free-roam actions
            // Enter engage event / shop, 1 career status, M world map, Tab SMS (on the d-pad, as on the consoles).
            // The stick steers through the joystick (proportional).
            want[0xc8] = on(A);
            want[0xd0] = on(X);
            want[0x1c] = on(Up);
            want[0x02] = on(Down);
            want[0x32] = on(Left);
            want[0x0f] = on(Right);
            want[0x39] = on(B);
            want[0x2e] = on(Y);
            want[0x38] = on(R1);
            want[0x30] = on(L1);
            want[0x2a] = on(R2) || past(rightTrigger_, 0x2a);
            want[0x1d] = on(L2) || past(leftTrigger_, 0x1d);
            want[0x01] = on(Start);
            want[0x13] = on(Select);
        } else {
            want[0xcb] = on(Left) || past(-stickX_, 0xcb); want[0xcd] = on(Right) || past(stickX_, 0xcd);
            want[0xc8] = on(Up) || past(-stickY_, 0xc8); want[0xd0] = on(Down) || past(stickY_, 0xd0);
            want[0x1c] = on(A) || on(Start);
            want[0x01] = on(B) || on(Y);
            want[0x39] = on(X);
        }
        driving_ = driving;
        for (unsigned scan = 0; scan < 256; ++scan)
            if (want[scan] != sent_[scan]) { sent_[scan] = want[scan]; setKey(scan, want[scan]); }
    }
};
