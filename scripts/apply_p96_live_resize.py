#!/usr/bin/env python3
"""Apply the demo-style P96 MemoryWindow live-resize fix to this branch.

Temporary development helper: the final PR keeps only display_p96pip.c.
All edits are anchored and fail if the base changes unexpectedly.
"""
from pathlib import Path

path = Path('player/amiga/display_p96pip.c')
src = path.read_text()


def replace_once(old, new):
    global src
    count = src.count(old)
    if count != 1:
        raise SystemExit(f'expected exactly one occurrence ({count}): {old[:100]!r}')
    src = src.replace(old, new, 1)


replace_once('''static void calculate_geometry(p96pip_state *s)
{
    if (s->force_full_dest) {
        /* One-shot PIPERR_CROPPED retry (see p96pip_toggle_fullscreen()):
         * fill the whole window instead of the normal aspect-fitted
         * rectangle, in case the driver's crop check is tripping on the
         * letterboxed inset rather than on anything about the window or
         * screen bounds themselves. */
''', '''static void calculate_geometry(p96pip_state *s)
{
    if (!s->fullscreen || s->force_full_dest) {
        /* Match P96PipDemo in ordinary windowed mode: the PIP fills the
         * entire inner window and its default relative rectangle follows
         * live Intuition resizing. Fullscreen retains aspect fit, unless
         * the PIPERR_CROPPED retry explicitly requests full-window fill. */
''')

replace_once(''' * lets its window hook adjust the hardware rectangle while Intuition is
 * resizing the containing window; using absolute dimensions makes affected
 * drivers snap the window back to its original size. The static margins can
 * temporarily stretch the picture during a drag, then the debounced reopen
 * calculates fresh aspect-correct margins for the final window dimensions.
''', ''' * lets its window hook adjust the hardware rectangle while Intuition is
 * resizing the containing window. A normal windowed PIP uses the demo's
 * default rectangle (no extra P96PIP geometry tags), fills its inner window,
 * and stays open during resizing; fullscreen uses explicit aspect fit.
''')

replace_once('''        /* P96's window hook cannot resize a PIP whose destination width and
         * height were opened as fixed absolute values: Intuition moves the
         * size gadget, then the window snaps back to the original size. In
         * relative mode these values are negative right/bottom margins, so
         * the hardware rectangle can follow the window during the drag.
         * Once IDCMP_NEWSIZE settles, reopen_pip() recalculates the exact
         * aspect-fit margins for the final size. */
''', '''        /* When an explicit rectangle is needed, express right/bottom as
         * relative margins. Native windowed playback uses PIP.c's default
         * relative full-window rectangle and never reopens for resizing. */
''')

replace_once('''        else if (cls == IDCMP_NEWSIZE) {
            /* Content size (border-excluded), matching what s->win_w/win_h
             * mean everywhere else in this file - not the raw outer
             * Width/Height. */
            int cw = s->win->Width  - s->win->BorderLeft - s->win->BorderRight;
            int ch = s->win->Height - s->win->BorderTop  - s->win->BorderBottom;
            s->pending_w = cw < 1 ? 1 : cw;
            s->pending_h = ch < 1 ? 1 : ch;
            s->resize_at = clock();
        }
''', '''        else if (cls == IDCMP_NEWSIZE) {
            int old_w = s->win_w, old_h = s->win_h;

            /* P96PipDemo leaves its MemoryWindow open while Intuition
             * resizes it: P96's window hook scales the source automatically.
             * Reopening here loses that behaviour and can snap the window
             * back to its original source size. Read back the real *inner*
             * dimensions for timing and fullscreen restoration only. */
            sync_content_geometry(s);
            s->have_window_geometry = 1;
            s->window_left = s->win->LeftEdge;
            s->window_top = s->win->TopEdge;
            s->window_width = s->win_w;
            s->window_height = s->win_h;
            calculate_geometry(s);
            s->geometry_valid = 1;
            if (g_display_want_time &&
                (old_w != s->win_w || old_h != s->win_h)) {
                printf("p96pip-resize: live MemoryWindow %dx%d -> %dx%d "
                       "(P96 scaling; no close/reopen)\\n",
                       old_w, old_h, s->win_w, s->win_h);
                Flush(Output());
            }
        }
''')

start = '''    if (s->pending_w != s->win_w || s->pending_h != s->win_h) {
        if (clock() - s->resize_at >= CLOCKS_PER_SEC / 10) {'''
end = '''    return s->quit ? MR_EV_QUIT : ev;
}'''
if src.count(start) != 1 or src.count(end) != 1:
    raise SystemExit('could not isolate the original delayed resize block')
first = src.index(start)
last = src.index(end, first)
if 'reopen_pip(s, "resize")' not in src[first:last]:
    raise SystemExit('resize block does not contain the expected reopen')
src = src[:first] + src[last:]

replace_once('''/* Black out the window area the PIP rectangle doesn't cover (letterbox /
 * pillarbox bars) - the PIP mechanism only ever draws its own dw x dh
 * rectangle at dx,dy; nothing else in this file touches the rest of the
 * window. A plain graphics.library RectFill on the window's own RastPort
 * works regardless of the underlying screen's depth/format (AGA, ECS, or
 * any RTG mode) since it goes through the normal blitter path, not a direct
 * bitmap lock. */
static void paint_letterbox(p96pip_state *s)
{
    if (!s->win || !s->win->RPort) return;
    SetAPen(s->win->RPort, 0);
    RectFill(s->win->RPort, (WORD)s->bl, (WORD)s->bt,
            (WORD)(s->bl + s->win_w - 1), (WORD)(s->bt + s->win_h - 1));
}
''', '''/* Paint only fullscreen letterbox bars, never the live windowed PIP.
 * Filling the whole window used to leave blank/grey areas when the PIP
 * source rectangle remained at its old size after a resize. */
static void paint_letterbox(p96pip_state *s)
{
    int l, t, r, b;
    if (!s->win || !s->win->RPort || !s->fullscreen) return;
    l = s->bl; t = s->bt;
    r = l + s->win_w - 1;
    b = t + s->win_h - 1;
    SetAPen(s->win->RPort, 0);
    if (s->dy > 0)
        RectFill(s->win->RPort, (WORD)l, (WORD)t,
                 (WORD)r, (WORD)(t + s->dy - 1));
    if (s->dy + s->dh < s->win_h)
        RectFill(s->win->RPort, (WORD)l, (WORD)(t + s->dy + s->dh),
                 (WORD)r, (WORD)b);
    if (s->dx > 0 && s->dh > 0)
        RectFill(s->win->RPort, (WORD)l, (WORD)(t + s->dy),
                 (WORD)(l + s->dx - 1), (WORD)(t + s->dy + s->dh - 1));
    if (s->dx + s->dw < s->win_w && s->dh > 0)
        RectFill(s->win->RPort, (WORD)(l + s->dx + s->dw),
                 (WORD)(t + s->dy), (WORD)r,
                 (WORD)(t + s->dy + s->dh - 1));
}
''')

# The historic pending-size/debounce state is no longer used by the PIP path.
replace_once('''    int            pending_w, pending_h;
    clock_t        resize_at;
''', '')
replace_once('''    s->pending_w = s->win_w;
    s->pending_h = s->win_h;
    rebuild_geometry(s, reason);
''', '''    rebuild_geometry(s, reason);
''')

# Fail before writing if any leftover old PIP resize logic remains.
if ('reopen_pip(s, "resize")' in src or 'resize_at' in src or 'pending_w' in src):
    raise SystemExit('old delayed-reopen resize code remains')
path.write_text(src)
print('P96 MemoryWindow live-resize fix applied')
