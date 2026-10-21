# Floating selections — Stage 3 implementation

## Behaviour and architecture

Ordinary Paste starts an editable session on the active layer. Paste into New
Layer creates a layer above it. Both initialize one atomic, undoable operation;
canceling the oversized-paste dialog leaves the document and redo branch intact.
Move, resize, rotation, keyboard nudges and Ctrl-drag copy use the session. A
gesture samples its immutable source and records one history entry. A click
without movement rolls back gesture preparation without abandoning redo.

Uninterrupted pasted movement uses the unchanged captured destination base.
Moving A → B → C restores the original background at A and B, matching the
reported Paint.NET results. A tool or layer change materializes the current
result while preserving selection geometry. Returning to Move lifts the edited
raster, so subsequent movement leaves a transparent hole and carries any painting
with it. At the exact original lift pose the original raster is preserved,
avoiding a second fractional-coverage multiplication on a no-op lift.

Source pixels and coverage remain independent. Session creation deep-copies
inputs, including externally backed images; history shares only immutable image
values. Erasing a lifted base detaches it from the original-pose snapshot through
Qt copy-on-write. Session targets are weak layer references. Move gestures hold a
guarded document pointer and disconnect their history callback on completion or
destruction. Document snapshots rebind session targets when restoring layers.

The private clipboard representation stores unmasked premultiplied pixels and
an independent coverage mask. It validates dimensions, exact payload length,
version and premultiplied channel validity. Zero-coverage holes contain no copied
colour data. The ordinary image representation applies coverage once for other
applications; external image and local-file fallbacks remain available.

Native `.psw` saving preserves the editable session, following the fresh-process
Paint.NET save/reopen result. An optional versioned `FPS1` trailer stores source,
coverage, base, transforms, suspended state and selection. The existing v1/v2
layer prefix contains the materialized appearance, so legacy readers can display
it. Ordinary documents need no trailer. Saving uses an atomic `QSaveFile` and
does not finish the session. Undo history itself is not serialized.

Painting, fill, delete/cut, adjustments and effects consume the effective layer
raster. Opening and canceling an adjustment leaves the session untouched.
Structural operations finish it before changing the layer stack or canvas.
Each document owns its own session; returning to a floating document restores
its editing tool without imposing another document's tool transition on it.

## Changed files

* `src/core/clipboardpixels.{h,cpp}`: private clipboard codec and standard export.
* `src/core/document.{h,cpp}`: atomic paste, lifecycle, effective snapshots,
  competing-operation integration and native persistence.
* `src/core/floatingpixelsession.{h,cpp}`: suspended state, original lift pose,
  source geometry and transformed source extraction.
* `src/tools/movetool.{h,cpp}`: floating gestures, copy, nudges and history lifetime.
* `src/canvas/canvaswidget.{h,cpp}`: tool/document transitions and Finish input.
* `src/mainwindow.{h,cpp}`: clipboard commands, operation boundaries and per-document
  tool restoration.
* `src/core/adjustmentpreview.{h,cpp}`: previews from effective session pixels.
* `src/panels/layerspanel.cpp`: thumbnails include the floating content.
* `CMakeLists.txt`: clipboard source in shared core sources for all relevant targets.
* `tests/test_all.cpp`, `tests/test_paste.cpp`: core and UI regression coverage.

## Validation

The subsequent [performance correction report](floating-selection-performance.md)
contains current test totals, before/after benchmarks and responsiveness checks.

The application and all four test executables built successfully with CMake. All
four CTest suites passed (paste, functional, brush and adjustment preview). The expanded
functional suite has 1,299 checks and the paste suite has 1,237 checks. Additional
CTest suites cover brushes and adjustment previews. Coverage includes transparent
and partially transparent sources, independent fractional masks, offsets,
off-canvas geometry, invalid inputs, immutable pixels, history, clipboard payloads,
context boundaries, fresh native reopening and independent document sessions.
Native compatibility tests read the unchanged prefix as a legacy reader, load a
file without a trailer, and reject a truncated recognized trailer without
partially replacing the current document.

## Remaining concerns and later work

* Sampling remains nearest-neighbour. Resize and rotation use the existing
  axis-aligned handle framework; higher quality sampling and oriented handles
  require later-stage work and reference testing.
* Fractional replacement remains encapsulated coverage interpolation. Detailed
  Paint.NET edge behaviour after lifting or overlapping fractional selections
  still needs pixel-level reference testing.
* Zoomed/panned paste uses the visible top-left, clamped to the canvas. Exact
  Paint.NET placement remains a manual acceptance item.
* Structural operations finish the session. Irregular-mask crop semantics remain
  later-stage work; the existing crop still uses selection bounds.
* Materialization can need a large image spanning the base and off-canvas source.
  Excessive extents fail within the image allocation budget; the user can move
  the source closer before finishing. Effective snapshots and native metadata
  also increase memory/file size while a session is editable.
* Native files preserve editable state, not undo history. Older readers display
  the materialized result but cannot recover the session. Existing PNG autosave
  recovery preserves appearance rather than editable session metadata.
* No later stage, commit, push or pull request is included.

## Manual acceptance

1. Paste an opaque patch over detailed artwork. Move A → B → C without other
   interactions; inspect the restored background at A and B. Repeat with holes
   and partial alpha in the patch.
2. Paint over the paste, return to Move and drag it. Check that painting travels
   with the source and the old position becomes transparent. Repeat after a
   layer switch and return.
3. Finish with Enter, Escape and Ctrl+D; Undo and Redo. Check selection handles,
   holes and backgrounds against the Paint.NET workflow.
4. Save an unfinished `.psw`, close the application, reopen it and move the source
   without selecting it again. Check transparent pixels and off-canvas content.
5. Copy an irregular selection between two documents, paste on an existing layer
   and into a new layer, then switch tabs and edit each independently.
6. Check resize, right-drag rotation, Ctrl-drag copy and arrow nudges. Compare
   paste placement at several zoom levels and pan positions with Paint.NET.
