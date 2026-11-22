# Touch panels: a UI that a finger can use

Status: the UI scale is built. The rest is a list of what to do next.

## What a touch panel needs

A finger covers about 7 to 10 mm. A widget that is 32 logical pixels tall is that size on a
desktop monitor at 100 ppi, and under 4 mm on a 4-inch 800x480 panel. Desktop widgets are right
for a pointer; on a touch panel they are too small to hit and to read. The desktop UI must not
change, so the answer cannot be new sizes in the widgets.

## Built: the UI scale (`FrameOptions::ui_scale`)

The frame runtime can lay the UI out in a smaller frame and draw it larger:

1. The tree is laid out in logical units: the panel's pixels divided by the scale.
2. The rasterizer multiplies every draw command by the scale on the way in
   (`Canvas::set_ui_scale`): coordinates, sizes, line thickness, text size.
3. A touch position in pixels is divided by the scale before it is routed to the tree.
4. Damage tracking measures its rects in pixels (`damage_between(..., scale)`).

At 1 (the default) nothing changes. At 1.5 a 32-unit button is 48 pixels tall, text is 21 pixels,
gaps and scrollbars grow with them, and the layout reflows in a smaller space (800x480 becomes
533x320 logical). No widget was edited. A board picks the scale for its panel; the Korvo-1 demos
do not set it yet (it needs the PRISM submodule bumped first).

simplify: fractional scales draw shapes and text at fractional positions, so edges are not
pixel-snapped. Upgrade: snap rect edges to pixels for the common 1.25, 1.5 and 2 scales.

## Next, in the order they pay off

1. **Tap slop.** A finger moves a few pixels while it presses. A press followed by a move under
   about 8 px should still be a click, not the start of a slider drag or a text selection.
2. **Pan to scroll.** Dragging inside a scroll view or a list should scroll it, with a little
   momentum. Today only the wheel and the scrollbar scroll.
3. **Bigger hit areas than the drawing.** A small control (a checkbox box, a slider thumb, a
   scrollbar) gets a touch target of at least the finger size around it, without growing on
   screen.
4. **No hover.** A touch UI has no hover: show the pressed state at once, and keep tooltips off
   or show them on long press.
5. **Long press as secondary click**, for context menus.
6. **Popups and menus**: row height at least the finger size, and placement away from where the
   finger covers.

Each of these changes what an input event does, so each needs its own test that drives events
through `route_event`, and none may change what a mouse does.
