/*
XBOX_MEMORY.H

The console's memory as the game sees it (port/xbox/src/nxdk_memory.c).
*/

#ifndef XBOX_MEMORY_H
#define XBOX_MEMORY_H

/* whether memory ran short as the last frame ended: the pages held back for
the game state's faults in use, or little free (main.c ends a game in
progress, rather than let an allocation fail) */
int xbox_memory_short(void);

#endif
