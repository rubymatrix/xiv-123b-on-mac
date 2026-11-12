/* The game's launch argument for a session id, as tools/xiv_args.py's build() makes it. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define XIV_ARGS_TICK 0x10000u /* the tick the host starts the game's clock at (XIV_TICK) */

/* sqex0002<...>!//// for the 56-character session id, keyed to tick; 0 when it does not fit */
int xiv_launch_arg(const char* session_id, uint32_t tick, char* out, size_t n);
