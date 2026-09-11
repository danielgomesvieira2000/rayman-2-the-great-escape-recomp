/* The port's debug menu: one window inside RT64's developer UI, on F1.
 *
 * WHY IT EXISTS. This port has a lot of instrumentation and no way to look at
 * any of it while the game is running. Every facility reports itself by printing
 * a line when something happens -- the draw distance hook, the frame cap, the
 * memory search, the attract-mode scan, the capture key -- and most of them only
 * report at all when an environment variable was set before launch. So the
 * questions that come up while playing ("is the cap on?", "what aspect is the
 * culling matrix getting?", "did that capture actually write?") are answered by
 * quitting, setting a variable, relaunching, and getting back to the same place.
 * By then the thing that prompted the question is usually gone.
 *
 * This is the readout those facilities never had: their current state, all in
 * one place, updated live, in a build the player already has.
 *
 * IT IS READ-ONLY. Nothing in this window changes what the port is doing. That
 * is a deliberate first step rather than an unfinished one -- see the note in
 * include/debug_status.h -- and the controls come once the readouts have shown
 * which are worth having.
 *
 * WHERE IT DRAWS. RT64 owns the ImGui context, so a port cannot open a window of
 * its own; tools/patch_rt64_debug_menu.py adds a hook that RT64 calls once per
 * frame from inside its own UI frame, and install() below fills it in. That also
 * means this window appears with RT64's "Game editor", which is the other half
 * of the same toolkit: RT64's side answers "what did the renderer draw here",
 * and this side answers "what is the port doing about it".
 *
 * See docs/DEBUG-MENU.md.
 */

#ifndef RAYMAN2_DEBUG_MENU_H
#define RAYMAN2_DEBUG_MENU_H

namespace rayman2::debug_menu {

/* Read whether the menu is wanted and note when the session started, so the
 * window can show an uptime. Call once, early in main, before the renderer is
 * created. RAYMAN2_DEBUGMENU=0 turns it off; RT64's own developer UI stays on
 * F1 either way. */
void init();

/* Put the window into RT64's UI. Must run before the first frame, and is a
 * no-op when the menu is switched off, so both renderer contexts call it
 * unconditionally while building their RT64 application. */
void install();

}  // namespace rayman2::debug_menu

#endif /* RAYMAN2_DEBUG_MENU_H */
