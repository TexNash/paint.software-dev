# Stage 5 — mask-aware Crop to Selection

Implementation complete; awaiting manual acceptance.

## Reference behaviour

The user's Paint.NET tests confirm that crop clears the selection, clips its
bounds to the existing canvas, discards off-canvas pixels, and makes pixels
outside an irregular or rotated selection transparent. Selected transparent
interiors remain part of the selection geometry. The supplied before/after
screenshots confirm the rotated shape; they do not resolve fractional alpha.

An unfinished paste produces separate Finish and Crop history entries. Undo
Crop restores the original canvas and editable object at a context boundary;
moving it then leaves a hole. Undo Finish restores the earlier session that
preserves the background during movement.

## Implementation

`Document::cropToSelection()` is the mask-aware operation used by MainWindow.
The existing rectangular `cropTo(rect)` API remains compatible with its callers.
The UI settles the active tool first, including queued floating movement and
legacy selected-pixel gestures, before reading selection geometry.

The operation captures the independent selection mask and intersects its bounds
with the canvas. It prepares the effective floating raster, cropped buffers for
every layer, and cleared selection storage before changing layer content. Empty
or entirely off-canvas selections are no-ops. Null image allocations abort before
the content/history transaction; settling a preceding gesture is a separate edit.
As elsewhere in this application, C++ allocation exceptions are not recovered.

For an active session, a Finish snapshot materializes the target and suspends the
session. A Crop snapshot then installs the cropped images and removes the
session. An already suspended session reuses its existing context boundary;
crop does not insert a redundant Finish. Undo Crop restores suspended editing
intent; the existing lift path supplies the hole semantics on subsequent Move.
The immutable original source remains available in the Finish undo checkpoint.

All layers, including hidden and locked layers, are cropped in document space.
Their properties, stacking and active index are preserved. Layer origins become
zero and retained outside storage is discarded. The legacy Move Selected Pixels
path supplies its completed raster and selection without architectural migration.
No native serialization or CMake source-list changes are needed.

`cropCoveredPixel()` encapsulates crop-specific fractional coverage. It multiplies
each premultiplied colour channel and alpha by selection coverage with integer
division by 255, matching existing selection multiplication. This is applied
once during crop; an already covered edge can therefore be attenuated again.
Exact equivalence to Paint.NET's fractional-edge policy remains unconfirmed.
No alpha-based geometry inference or transformation sampling changes are made.

## Resource and performance limits

Crop is synchronous and allocates one cropped image per layer. History retains
the original document and the Finish raster through existing snapshot ownership
and QImage copy-on-write. The Finish checkpoint still uses full floating-layer
materialization so Undo Crop can preserve the complete pre-crop content. Very
large layers can pause during crop; extremely distant off-canvas content can
exceed the existing raster budget and reject the operation without cropping.
Further performance work remains deferred as requested.

## Validation and manual acceptance

Automated functional coverage includes independent mask/alpha, holes, partial
coverage, multiple layers, hidden/locked properties, offsets including extreme
coordinates, native save/reopen, empty/off-canvas no-ops, rotation and scaling,
active/suspended sessions, and two-step undo/redo. MainWindow integration covers
Undo Crop followed by movement leaving a hole, Undo Finish followed by movement
preserving the background, and crop during a legacy selected-pixel gesture.
Existing rectangular crop tests remain in place.

Manual checks:

1. Repeat the supplied rotated-paste example: crop should retain its shape with
   transparent corners, clear selection handles, and use canvas-clipped bounds.
2. Undo Crop and move the restored object; its previous position should become
   transparent. Undo the movement and Finish, then move again; the original
   background should remain intact.
3. Crop a lasso or Magic Wand selection with holes on several layers, including
   hidden and locked layers. Reveal them afterward to inspect the same mask.
4. Crop a partially off-canvas paste, then undo/redo and save/reopen the result.
5. Inspect fractional rotated edges against Paint.NET, preferably with actual
   pixel-alpha samples. Screenshots with a checkerboard cannot establish exact
   edge-alpha multiplication or rounding.

Application and all four regression executables built successfully. The final
`ctest --test-dir build --output-on-failure` run passed all four suites:
paste 10.07 s, functional 1.65 s, brush 3.18 s, adjustment preview 2.29 s;
total 17.18 s, zero failures. `git diff --check` also passed. Build warnings
are existing Qt deprecation warnings in unchanged geometric-transform code.
No later roadmap stages are included.
