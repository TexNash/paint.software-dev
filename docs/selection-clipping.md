# Selection clipping

The tool options bar has a shared **Selection Clipping** selector for selection,
move and painting tools. It is independent of drawing antialiasing and of the
Move tool's resampling option. The initial setting is **Antialiased**, preserving
the previous selection-copy behaviour.

- **Pixelated** includes or excludes each boundary pixel entirely. Existing source
  colour and transparency are preserved for included pixels; there is no added
  feathering. Rotating or scaling uses the selected resampler for colours, with
  a separate hard boundary.
- **Antialiased** uses the selection's fractional coverage at its boundary.

Changing the setting preserves the underlying selection mask. Pixelated resolves
coverage values 128–255 to fully selected and 0–127 to excluded. An explicitly
feathered mask also becomes binary when used in Pixelated mode; toggling back
retains its original coverage. Exact boundary-pixel agreement with Paint.NET's
rasteriser still needs visual reference comparison.

Copy and Copy Merged capture coverage and clipping policy when copying, including
copies of floating pixels. Later changes to the tool option cannot change that
clipboard value. The private clipboard keeps its existing v1 pixel payload and
adds a separate clipping-policy MIME entry; legacy payloads default to AA.
External applications receive the ordinary image with coverage applied once.

Selection clipping also applies to painting, erasing, bucket and selection fills,
effects, adjustments, deletion, cropping and moving selected pixels. A deliberate
clipping change updates an active move and its original cutout, with an undoable
checkpoint. Paste starts with the copied policy even if the current tool option
differs. Native files retain the live move's clipping policy using an optional
CLP1 extension after the sampling extension. Opening a file does not reclip its
pixels to match the current toolbar preference.

## Manual verification

1. Make an ellipse over opaque coloured artwork. Choose Pixelated, copy, change
   the option to Antialiased, then paste over a contrasting opaque background.
   At high zoom the boundary must contain only whole included/excluded pixels.
2. Repeat with Antialiased at copy time, switching to Pixelated before paste.
   The pasted copy must retain its AA edge. Confirm Copy Merged too.
3. Rotate and scale the Pixelated copy using each resampling option. Colours
   inside may interpolate, but pixels outside its hard boundary must stay intact.
4. Repeat with translucent artwork, selected transparent areas and selection holes.
5. With an ordinary ellipse selection, compare painting, fill, erase, delete and
   an adjustment in both modes. Undo/redo each edit and cancel an adjustment preview.
6. Move an ellipse, switch clipping mode while it remains active, then undo/redo.
   Check both the moved boundary and its original cutout. Save/reopen a live move.
7. Switch between Rectangle Select, Ellipse Select, Lasso, Magic Wand and painting
   tools. The shared choice should persist. Test selection move/resize and cancel.

Automated regressions are in `tests/test_all.cpp`, alongside the existing
selection, transparency, layer-offset, clipboard, floating-pixel and history tests.

## Files changed

- Core: `src/core/selection.{h,cpp}`, `clipboardpixels.{h,cpp}`,
  `document.{h,cpp}`, `floatingpixelsession.{h,cpp}`, and `history.cpp`.
- UI: `src/mainwindow.cpp`, `src/panels/tooloptionspanel.{h,cpp}`, and `src/i18n.cpp`.
- Tools: `src/tools/tool.{h,cpp}`, `brushtool.cpp`, `clonestamptool.cpp`,
  `erasertool.cpp`, `filltool.cpp`, `gradienttool.cpp`, `linetool.cpp`,
  `moveselectiontool.cpp`, `penciltool.cpp`, `recolortool.cpp`, `shapetool.cpp`,
  and `texttool.cpp`.
- Verification and notes: `tests/test_all.cpp` and this document.
