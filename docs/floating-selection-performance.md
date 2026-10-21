# Stage 3 floating-selection performance correction

The subsequent [Stage 4A report](floating-selection-stage4a.md) covers bounded caching,
layer-opacity responsiveness and current benchmark/test results.

## Cause and correction

The regression ran several full-resolution operations for every pointer update,
including updates that did not change the transform. Selection coverage sampled
the whole canvas; document notifications invalidated the entire canvas and
rebuilt the document tabs using full-resolution flattened images. The layer
panel also rendered full-resolution pixels for its tiny thumbnail. Queued
pointer positions repeated that work on the UI thread.

The correction keeps the existing nearest-neighbour sampling and replacement
coverage policy:

* Unchanged synchronized transforms return without allocating masks, notifying
  views or invalidating pixels. Initial low-level session synchronization still
  works, including after history restoration.
* Base pixels copy through Qt's raster engine. Source sampling is restricted to
  its transformed footprint, intersected with the requested region before
  conversion to integer coordinates. Integer translations use row copies;
  axis-aligned scaling caches source columns; general affine sampling avoids
  per-pixel Qt accessor and mapping calls.
* Generated masks transfer ownership into Selection and scan only their possible
  coverage. Selection bounds cache against the image cache key; marquee region
  extraction scans those bounds.
* A regional transform notification preserves the canvas cache. Dirtiness
  includes both source footprints and the symmetric difference of the old/new
  finite layer domains. The latter preserves Overwrite behaviour when the source
  changes the domain beyond the base image. Each dirty region recomposites all
  relevant layers; transparent pixels replace cached pixels using Source mode.
* Pointer moves replace one pending position on a 16 ms single-shot frame timer.
  The timer is not restarted by subsequent input. Release applies its own final
  position, and context transitions flush pending input. History navigation
  discards obsolete pending input. Coalescing applies only to floating Move.
* Document and floating-layer thumbnails sample directly into bounded preview
  images. They no longer materialize full-resolution floating artwork. Thumbnail
  previews are approximate downsampled representations; document pixels and
  exports retain the original full-resolution calculation.
* Ending a changed gesture records history without redundantly invalidating the
  full raster that was already updated during preview.

## Repeatable measurements

Run the opt-in benchmark through the existing paste test executable:

```bash
cmake --build build --target paintdotnet_paste_tests -j 4
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-floating
```

It isolates settings and drives real MainWindow/CanvasWidget mouse events and
canvas renders on 4000×3000 documents. Each movement run measures eight positions,
then twenty identical held-pointer positions. Frame measurements include a
requested 17 ms event-loop interval, coalesced work and a canvas render. Press
measurements start with a warm canvas cache. Resize and rotation each measure
three subsequent frames. Values are elapsed milliseconds, not isolated kernel
times or guarantees about compositor/display latency.

Before and after used the same default CMake build (empty build type, no explicit
optimization flags), Qt offscreen platform and Intel i9-11900H host. Measurements
are single runs with the reported median and maximum; scheduling affects values.
The original baseline measured movement/press/stationary work. Resize/rotation
were added afterward and are reported separately, without an invented baseline.

| Selection | Measurement | Before (ms) | After (ms) |
|---|---|---:|---:|
| 300×200 | Press and render | 489.0 | 2.7 |
| 300×200 | Move frame median / maximum | 1812.6 / 1844.6 | 21.1 / 23.5 |
| 300×200 | Twenty stationary updates, total | 26845.3 | 34.9 |
| 2000×1500 | Press and render | 589.6 | 3.0 |
| 2000×1500 | Move frame median / maximum | 2220.0 / 2301.5 | 78.1 / 83.1 |
| 2000×1500 | Twenty stationary updates, total | 32103.5 | 43.1 |

| Selection | Resize median / maximum (ms) | Rotation median / maximum (ms) |
|---|---:|---:|
| 300×200 | 22.5 / 25.1 | 32.7 / 33.0 |
| 2000×1500 | 135.7 / 140.0 | 339.9 / 355.1 |

The measured multi-second backlog is removed. Large rotated selections remain
slower than translated selections; the 2000×1500 rotation is visibly less fluid
in this unoptimized build. Work still scales with affected pixel count, layer
count, blend modes and marquee complexity. Masks remain full canvas-sized values
for compatibility with current selection/history/native-save infrastructure.
Initial paste, context lifts, saving and very large materialization operations
remain synchronous. No worker-thread renderer, lower-quality drag mode, new
sampling policy or Stage 4 functionality is introduced.

## Verification and changed files

The application built successfully. All four CTest suites passed: functional
(1,605 checks), paste (1,257 checks), brush and adjustment preview.
`git diff --check` also passed. Existing coverage includes undo/redo, layer
transitions, painting, copy/paste, transparency and fresh native save/reopen.

New regression checks compare optimized pixels and masks with a scalar reference
for translation, fractional scaling, rotation, near-identity transforms, partial
coverage and off-canvas movement. Incremental composites must equal full renders
with layer offsets, partial opacity and Normal/Multiply/Overwrite modes. A real
canvas-cache test compares partial updates with forced full repaint. Input tests
verify that 200 queued moves produce one update, identical moves preserve the
mask cache key, release uses its final position, and history navigation cancels
pending input. Timing assertions are deliberately excluded from ordinary tests.

Correction files:

* `src/core/floatingpixelsession.{h,cpp}`: bounded sampling and fast paths.
* `src/core/document.{h,cpp}`: regional notifications and bounded document thumbnails.
* `src/core/selection.{h,cpp}`: bounded mask adoption and geometry caches.
* `src/canvas/canvaswidget.{h,cpp}`: regional cache updates and move coalescing.
* `src/tools/movetool.cpp`: flush pending input and avoid redundant invalidation.
* `src/mainwindow.cpp`, `src/panels/imagelistbar.cpp`,
  `src/panels/layerspanel.cpp`: regional subscriptions and bounded thumbnails.
* `tests/test_all.cpp`, `tests/test_paste.cpp`: correctness tests and opt-in benchmark.

All changed production sources already belong to the application's shared CMake
source lists and all relevant test targets. No serialization changes were needed
for this correction. No commits, pushes or later-stage work are included.

## Manual acceptance

1. Load a 4000×3000 image. Copy/paste a 300×200 area and then a 2000×1500 area.
   Hold the pointer stationary, drag rapidly in circles and across long distances,
   and release. Check that the object follows recent input, stops at the release
   position and does not keep processing old positions afterward.
2. Resize and right-drag rotate both selections. Repeat at fit zoom and 100% zoom,
   and move partially/entirely off-canvas and back. Inspect both old and new
   positions for stale pixels, holes or marquee remnants.
3. Repeat with transparent/partially transparent content and several layers,
   including partial layer opacity and Overwrite blending. Check both thumbnail
   areas update without pausing the canvas.
4. Undo/redo movement, resize and rotation; switch tools/layers and resume Move;
   Finish and undo Finish. Save an unfinished `.psw`, quit, reopen and move it.
5. Switch between two documents while a gesture is pending. Check source pixels,
   selection state and tool behaviour remain independent and no delayed motion
   appears in the other document.
