/**
 * @file input_names.h
 * @brief The names a binding spec uses for host inputs, as one X-macro table
 * that the runtime and the tool both read. X(kind, name, value): GAMEPAD is a
 * bit number of enum rl_input_button, AXIS an index into the four stick
 * axes (left x, left y, right x, right y), KEY a USB HID keyboard usage and
 * MOUSE a bit number of enum rl_input_pointer_button.
 */
#ifndef RELIFT_INPUT_NAMES_H
#define RELIFT_INPUT_NAMES_H

#define RL_INPUT_NAMES(X) \
    X(GAMEPAD, "south", 0) \
    X(GAMEPAD, "east", 1) \
    X(GAMEPAD, "west", 2) \
    X(GAMEPAD, "north", 3) \
    X(GAMEPAD, "left_stick", 4) \
    X(GAMEPAD, "right_stick", 5) \
    X(GAMEPAD, "left_shoulder", 6) \
    X(GAMEPAD, "right_shoulder", 7) \
    X(GAMEPAD, "left_trigger", 8) \
    X(GAMEPAD, "right_trigger", 9) \
    X(GAMEPAD, "start", 10) \
    X(GAMEPAD, "back", 11) \
    X(GAMEPAD, "dpad_up", 12) \
    X(GAMEPAD, "dpad_right", 13) \
    X(GAMEPAD, "dpad_down", 14) \
    X(GAMEPAD, "dpad_left", 15) \
    X(GAMEPAD, "guide", 16) \
    X(GAMEPAD, "misc", 17) \
    X(AXIS, "left_x", 0) \
    X(AXIS, "left_y", 1) \
    X(AXIS, "right_x", 2) \
    X(AXIS, "right_y", 3) \
    X(KEY, "a", 4) \
    X(KEY, "b", 5) \
    X(KEY, "c", 6) \
    X(KEY, "d", 7) \
    X(KEY, "e", 8) \
    X(KEY, "f", 9) \
    X(KEY, "g", 10) \
    X(KEY, "h", 11) \
    X(KEY, "i", 12) \
    X(KEY, "j", 13) \
    X(KEY, "k", 14) \
    X(KEY, "l", 15) \
    X(KEY, "m", 16) \
    X(KEY, "n", 17) \
    X(KEY, "o", 18) \
    X(KEY, "p", 19) \
    X(KEY, "q", 20) \
    X(KEY, "r", 21) \
    X(KEY, "s", 22) \
    X(KEY, "t", 23) \
    X(KEY, "u", 24) \
    X(KEY, "v", 25) \
    X(KEY, "w", 26) \
    X(KEY, "x", 27) \
    X(KEY, "y", 28) \
    X(KEY, "z", 29) \
    X(KEY, "1", 30) \
    X(KEY, "2", 31) \
    X(KEY, "3", 32) \
    X(KEY, "4", 33) \
    X(KEY, "5", 34) \
    X(KEY, "6", 35) \
    X(KEY, "7", 36) \
    X(KEY, "8", 37) \
    X(KEY, "9", 38) \
    X(KEY, "0", 39) \
    X(KEY, "enter", 40) \
    X(KEY, "escape", 41) \
    X(KEY, "backspace", 42) \
    X(KEY, "tab", 43) \
    X(KEY, "space", 44) \
    X(KEY, "minus", 45) \
    X(KEY, "equals", 46) \
    X(KEY, "left_bracket", 47) \
    X(KEY, "right_bracket", 48) \
    X(KEY, "backslash", 49) \
    X(KEY, "semicolon", 51) \
    X(KEY, "apostrophe", 52) \
    X(KEY, "grave", 53) \
    X(KEY, "comma", 54) \
    X(KEY, "period", 55) \
    X(KEY, "slash", 56) \
    X(KEY, "f1", 58) \
    X(KEY, "f2", 59) \
    X(KEY, "f3", 60) \
    X(KEY, "f4", 61) \
    X(KEY, "f5", 62) \
    X(KEY, "f6", 63) \
    X(KEY, "f7", 64) \
    X(KEY, "f8", 65) \
    X(KEY, "f9", 66) \
    X(KEY, "f10", 67) \
    X(KEY, "f11", 68) \
    X(KEY, "f12", 69) \
    X(KEY, "right", 79) \
    X(KEY, "left", 80) \
    X(KEY, "down", 81) \
    X(KEY, "up", 82) \
    X(KEY, "left_ctrl", 224) \
    X(KEY, "left_shift", 225) \
    X(KEY, "left_alt", 226) \
    X(KEY, "left_meta", 227) \
    X(KEY, "right_ctrl", 228) \
    X(KEY, "right_shift", 229) \
    X(KEY, "right_alt", 230) \
    X(KEY, "right_meta", 231) \
    X(MOUSE, "left", 0) \
    X(MOUSE, "right", 1) \
    X(MOUSE, "middle", 2) \
    X(MOUSE, "back", 3) \
    X(MOUSE, "forward", 4)

#endif
