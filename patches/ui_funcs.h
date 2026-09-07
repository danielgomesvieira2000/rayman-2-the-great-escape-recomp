/* Game-side UI declarations pulled in by RecompFrontend.
 *
 * recompui/src/api/ui_api_events.cpp includes this file by a hard-coded
 * relative path -- "../../../../../patches/ui_funcs.h" -- which resolves to
 * exactly this location in a port laid out the way the N64: Recompiled projects
 * are. It is marked "TODO: Forced game includes" upstream, and it is the seam
 * where a port declares the MIPS-side functions its own patches expose to the
 * frontend's UI.
 *
 * It is not merely a hook point, though: the event dispatcher in that file uses
 * RecompuiEventData, RecompuiEventType, RecompuiDragPhase and RecompuiMenuAction
 * WITHOUT including their header itself. recompui.h does not pull them in
 * either. So this file is also, in practice, where those structs get included
 * from -- the port is expected to include them because its own patches share
 * the same layout with the frontend across the recompiled boundary.
 *
 * Beyond that, this port has nothing to declare yet: patches/ is otherwise
 * empty until phase 06, where the C-compiled-to-MIPS override/hook layer
 * arrives along with widescreen and the settings the frontend surfaces.
 * Declarations for named game-side functions belong here once that exists.
 */

#ifndef RAYMAN2_UI_FUNCS_H
#define RAYMAN2_UI_FUNCS_H

#include "recompui/event_structs.h"

/* No game-side UI functions yet -- see the note above. */

#endif /* RAYMAN2_UI_FUNCS_H */
