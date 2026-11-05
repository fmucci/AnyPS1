# 23 — Keyboard layout file and mouse turning in the player (observed, second and third title)

The template player (`template/frontend/ps1_play.c`) reads its keyboard/mouse layout from a text file instead of fixed keys:

- `template/runtime/pad_input.h` (no SDL, unit-tested by `tests/runtime/test_pad_input.c`): the pad bit names (the bit map of `rt_pad_script_add`), the layout parser (`scheme wasd|classic`, `key <SDL name> <button>`, `key <name> = none`, `unbind`, `mouse on|off`, `sensitivity`, `mouse_left|right|middle <button>`) and the mouse generator.
- The player writes `input.cfg` with commented defaults on the first interactive start (not for scripted runs), loads it every start, logs lines it does not understand and unknown key names, and never lets Tab (fast-forward) be rebound. Package READMEs describe the file (`tools/package.py`).
- Default layout "wasd": W/A/S/D and arrows = d-pad, J/K/L/I = square/cross/circle/triangle (Space, Z = cross, X = circle), Q/E = L1/R1, R/F = L2/R2, Enter = start, Right Shift/Backspace = select. `scheme classic` is the earlier fixed layout.

## Mouse look (the pad is digital)
The BIOS pad word is 16 buttons: there is no analogue value to pass, so a mouse cannot drive a camera smoothly by itself. The player turns horizontal motion into left/right presses with a delta-sigma accumulator (one decision per virtual frame, 10 pixels per frame at sensitivity 1.0 keep a direction pressed every frame, clamped to 6 frames of backlog), which gives a duty cycle proportional to the motion; the game's own turn speed is the ceiling. F10 releases/captures the mouse (menus react to left/right), losing the window focus releases it, scripted runs and the `dummy` video driver never capture it. The recorded input (`--record`) is the final pad word, so mouse sessions replay identically.

## What a title still has to find out by itself (unknown until a person tries)
- What the game's buttons do (fire, action, strafe...): the defaults are conventions; the layout file lets a tester change them without a build.
- Whether turn speed is proportional to held time in that title, and whether a true free-look is possible: it needs the camera/heading variables (watch RAM while turning in a real gameplay moment: a hand-off test scene that is static changes nothing) and a host-side write; not part of the template.
