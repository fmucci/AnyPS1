/* Per-title camera profile for the player's mouse look (runtime/pad_input.h LookProfile). Disabled here: the mouse then turns the player with left/right presses.
 * To enable it for a title: observe in its RAM (1) that the camera follows the player's heading, (2) the player's heading field in his object, (3) globals holding the object pointer (find the address of the object, then the words that equal it) (see
 * docs/23-input-mapping.md and the first port's finding 0052) and fill the fields: ptr_addr[3] (three globals that hold the pointer to the player's object once it exists and are 0 before), head_off (the heading's offset in the object), bits (angle bits), sign, scale, orbit_addr (the camera's offset word, view target = orbit - heading, needed for camera-relative walking), enabled. */
#ifndef LOOK_PROFILE_H
#define LOOK_PROFILE_H
#include "pad_input.h"
static const LookProfile LOOK_PROFILE = { { 0, 0, 0 }, 0, 10, 1, 0.35, 0, 0 };
#endif
