#pragma once

#include <atomic>
#include <cstdint>

namespace tracey_editor {

// Frame-coherent input state populated by the platform (Cocoa / WebView2) and
// drained by the render tick. All fields are simple PODs — copy by value to
// take a snapshot per frame.
struct InputState {
    // Mouse position in viewport-local pixels (0,0 = top-left of viewport).
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;

    // Per-frame deltas. Reset to 0 by the consumer each tick.
    float mouse_dx = 0.0f;
    float mouse_dy = 0.0f;
    float scroll_dx = 0.0f;
    float scroll_dy = 0.0f;

    // Button state (current frame).
    bool mouse_left = false;
    bool mouse_right = false;
    bool mouse_middle = false;

    // Movement keys. Bitset for everything else if needed later.
    bool key_w = false;
    bool key_a = false;
    bool key_s = false;
    bool key_d = false;
    bool key_q = false;
    bool key_e = false;
    bool key_shift = false;
    bool key_space = false;

    // Viewport size (logical points, before backing-scale multiplication).
    float viewport_w = 0.0f;
    float viewport_h = 0.0f;
};

}  // namespace tracey_editor
