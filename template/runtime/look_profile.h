/* Per-title camera profile for the player's mouse look (runtime/pad_input.h LookProfile). Disabled here: the mouse then turns the player with left/right presses.
 * To enable it for a title: observe in its RAM (1) the view yaw formula, (2) the orbit/offset global that the camera function follows, (3) the player's object id and heading field (see
 * docs/23-input-mapping.md and the first port's finding 0052) and fill the fields: yaw_addr, bits, id_addr, copy_addr, head_off, sign, scale, lock_addr (the byte that is non-zero while the view is locked to the heading, 0 if none), enabled. */
#ifndef LOOK_PROFILE_H
#define LOOK_PROFILE_H
#include "pad_input.h"
static const LookProfile LOOK_PROFILE = { 0, 10, 0, 0, 0, -1, 0.35, 0, 0 };
#endif
