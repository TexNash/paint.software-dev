# Stage 4B — ordinary lifts and transformation controls

Implementation complete; awaiting manual acceptance. Stages 4C and 6 have not
begun. No commits or pushes were made.

## Reference and scope

The approved scope migrates ordinary selected-pixel gestures to floating
sessions, adds signed handle-crossing flips and a draggable rotation pivot,
and preserves the existing Shift/Alt/Ctrl interactions. Whole-layer movement
without a selection and the outline-only Move Selection tool retain their
existing paths. The controls in this stage belong to Move Selected Pixels.

The user confirmed that Paint.NET restores custom pivot positions through
Undo, but resets them after saving, closing and reopening. This implementation
keeps the pivot in history and deliberately omits it from native serialization.
Saving leaves the open document's pivot unchanged.

Reference: <https://paint.net/doc/latest/MoveTools.html> documents control-nub
crossing, relocating the rotation point off canvas, constrained rotation,
aspect-preserving/symmetric resizing, nudges and copy movement.

## Architecture and changed files

* `src/core/document.cpp/.h`: `prepareFloatingPixelGesture()` also initializes an
  ordinary selection from the active layer. Source pixels are unmasked and
  independent of Gray8 geometry. The captured base contains the selection hole;
  the exact original raster is retained for the initial pose. Copy gestures
  retain the original pixels. `setFloatingRotationPivot()` validates finite
  geometry and publishes an overlay-only update.
* `src/core/floatingpixelsession.cpp/.h`: an optional source-local pivot follows
  transforms. Value snapshots automatically preserve it through history. It is
  not a render-cache input and does not modify source pixels or coverage.
* `src/tools/movetool.cpp/.h`: ordinary selected-pixel drags and arrow nudges use
  the existing session/history path. The superseded selected-pixel bitmap-baking
  branches and temporary images are removed; whole-layer movement remains.
  The centre circle/cross is draggable with the left button. Right-drag always
  rotates. One completed pixel or pivot gesture creates one history command;
  clicks and stationary gestures preserve the redo branch.
* `src/tools/selectionhandles.cpp/.h`: signed resize transforms preserve axis
  orientation when handles cross an anchor. Shift constrains corner aspect;
  Alt anchors resizing at the centre; both modifiers combine with reflections.
  A small minimum extent prevents singular transforms at the crossing point.
  Pivot drawing and hit-testing shrink for small selections to leave room for
  moving pixels and grabbing resize nubs.
* `tests/test_all.cpp`, `tests/test_paste.cpp`: focused regressions plus updates
  to existing movement tests. Assertions now inspect effective rendered pixels
  instead of assuming previews are baked into `Layer::image()`. Movement fixtures
  grab away from the newly interactive pivot; pivot tests use explicit events.

No new compilation units or CMake target-list changes are required. All existing
application and test targets include the modified sources. Native file layout
and transformation sampling algorithms remain unchanged.

## Ownership, lifetime and responsiveness review

Document owns each session. Gestures retain a guarded Document pointer and an
owned snapshot command; history navigation discards stale gestures. Source images
remain immutable QImage values with copy-on-write snapshots. The pivot is a value,
with no layer/widget ownership. Nonfinite pivot positions and transforms that
overflow its mapped position are rejected.

Ordinary lifts immediately enter the same canvas event-coalescing path as paste.
Tests send 200 positions before a flush and require one update, then repeat a
stationary position and require no mask regeneration. Existing rendering caches,
partial invalidation and thumbnail paths are reused. Pivot-only changes preserve
pixel transforms and mask identities; they do not sample full-resolution pixels.

## Remaining limitations and review concerns

* Sampling remains the existing floating-session nearest-neighbour policy.
  Ordinary selection transforms now share that policy; the old legacy release
  path's smooth bitmap bake is gone. Sampling quality belongs to Stage 4C.
* Transform nubs still use the existing axis-aligned source envelope after a
  completed rotation. Oriented transform frames and outline-only tool parity
  remain separate fidelity questions for the later workflow audit.
* Large captures, transformations and materialization remain synchronous and can
  retain the already accepted large-image lag. No preview downsampling, threads,
  extra raster caches or performance tuning were introduced.
* Exact fractional crop-edge behaviour remains the Stage 5 reference limitation.

## Manual acceptance

1. Select an asymmetric area and move it repeatedly. The initial position should
   become a hole; intermediate destinations should restore their background.
   Repeat with Ctrl-drag, partial transparency, irregular geometry and offsets.
2. Drag corner/side nubs through the opposite side, including horizontal,
   vertical and double-axis flips. Repeat with Shift, Alt and Shift+Alt. Check
   that dragging back restores orientation and Undo/Redo restores the gesture.
3. Drag the circle/cross pivot outside the selection and outside the canvas,
   then right-drag to rotate about it. Check Shift snapping and Undo/Redo.
   Grab away from the circle when intending to move the selected pixels.
4. Save with a custom pivot: it stays put in the open document. Close and reopen:
   pixels and editable selection return, with the pivot reset to their centre.
5. Check tool/layer switches, transparent interiors, Crop and both crop history
   steps, Finish, and switching between independent documents.
6. Compare responsiveness on 800×600 and 4000×3000 documents, including stationary
   holds, off-canvas movement, opacity changes and repeated transformations.

## Build and regression results

Built the application and all four test executables using the existing build
configuration. `ctest --test-dir build --output-on-failure` passed all four suites:
paste 10.26 s, functional 1.70 s, brush 3.30 s, adjustment preview 2.27 s;
total 17.53 s, zero failures. `git diff --check` passed. The build reports the
existing Qt `QImage::mirrored()` deprecation warning in unchanged flip code.
