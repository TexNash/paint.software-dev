# Stage 6 — Workflow completion and acceptance audit

Stage 4C was accepted. Stage 6 adds acceptance coverage and records the remaining
manual comparisons; it does not replace the integration delivered in Stage 3.
The initial audit required no production changes. Subsequent manual acceptance
identified paste placement as a fidelity gap; the approved correction is described
below. Native format, sampling quality and recovery behaviour remain unchanged.

## Changes and audit findings

- `tests/test_paste.cpp`: zoomed/panned viewport placement, placement Undo/Redo,
  cross-document clipboard transfer with irregular fractional coverage and
  selected transparent pixels, independent source ownership, filtered native
  reopen, Select All/Undo Finish, and actual canvas events for Move Selection
  outline translation followed by returning to Move Selected Pixels.
- `tests/test_all.cpp`: layer identity through reorder/Undo, duplication of the
  materialized source, remove-target/Undo rebinding, and atomic rejection of a
  distant extent followed by moving back and successfully finishing.
- This report records the audit and manual acceptance procedure. Earlier
  uncommitted stages remain in the working tree; they are not new Stage 6 edits.

Following manual Paint.NET testing, internal clipboard Paste now uses the copied
area's original document coordinates. If that area intersects the visible canvas,
it stays at those coordinates. If entirely outside the viewport, its centre moves
along the line toward the visible canvas centre until the whole sample fits at
the view edge. A sample larger than the view is centred on the constrained axis;
positions are rounded to pixels and bounded by the destination canvas. This
centre-line interpretation implements the reported directional placement; exact
corner/partial-visibility behaviour remains a manual comparison item.

External images without our coordinate metadata retain the visible-top-left
fallback, and canvas expansion retains origin placement. Paste into New Layer
uses the same placement rules. No native/clipboard format changes were needed.

Correction files: `src/core/clipboardpixels.h`, `src/core/clipboardpixels.cpp`,
`src/mainwindow.cpp`, both existing functional/paste test files and this report.
The reusable clipboard placement helper performs only constant-time geometry,
with no image allocations or changes to source pixels, coverage or history.

Clipboard transfer crops to nonzero selection bounds, preserves coverage separately
from alpha, and retains selected fully transparent pixels.

Lifecycle review: selection replacement/inversion/modification finishes the source;
Move Selection finishes on its canvas press; painting and tool/layer switches
suspend it. Layer appearance changes reuse sampled source geometry. Reorder keeps
layer identity; duplicate suspends before copying; remove finishes before releasing
the target. Existing regression coverage also exercises effect-preview cancellation,
fill, delete, Crop, document switches, native persistence, and cached composition
through opacity and every supported blend mode. This is verification of the agreed
lifecycle, not a claim that every Paint.NET command has identical history grouping.

## Resource and failure review

The shared LRU charges source/mask tiles and prefilter levels against one 64 MiB
budget across documents. Existing tests exercise eviction, copy-on-write isolation,
caller ownership after eviction, and independent documents rebuilding evicted data.
Caches hold image values, not documents or layers. Document sessions and history
own their retained source/base images; layer targets are weak and restored history
rebinds targets. No new ownership or asynchronous lifetime mechanisms were added.

A distant transformed source can be previewed without allocating an empty bridge.
Materialization validates representable bounds and Qt's allocation budget. The new
1e12-coordinate test verifies rejection leaves source, layer and history intact,
and that returning to a normal pose recovers. Existing tests cover invalid masks,
transforms, overflowing offsets and malformed native input.

**Limits:** the cache budget is not a total-process memory bound. Source/base
images, active image buffers and undo history can consume substantially more;
eviction need not reduce RSS immediately. This audit does not prove absence of
all memory leaks or inject system-wide allocation failure. Checked null-image
failures and deterministic invalid-extent rejection do not guarantee recovery
from C++ allocation exceptions or the OS terminating an exhausted process.
Very distant off-canvas materialization/save/Finish can fail by design. Editable
crash recovery remains separate; autosave preserves flattened appearance.

## Reproduction

```sh
cmake -B build
cmake --build build --target paintdotnet paintdotnet_tests paintdotnet_paste_tests paintdotnet_brush_tests paintdotnet_preview_tests -j 4
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-stage4c
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-floating
```

The benchmarks retain the Stage 4C build configuration and full-resolution rendering.
They measure variability of the accepted implementation, not a before/after speedup:
Stage 6 changes no renderer. Twelve phase samples report mean and maximum (also the
reported p95 for this small sample count). Large Bilinear/Bicubic transformations
remain expensive; further optimisation is deferred. See the Stage 4C report for
baseline measurements and build-flag caveats.

## Results

The application and all four test targets built. Final CTest: **4/4 passed**,
17.75 s; functional **1,905 checks**, paste **1,377 checks**. `git diff --check`
passed. No new CMake sources or targets were necessary. Initial new-test failures
were fixture assumptions: clipboard capture crops empty margins, and an irregular
mask's bounding-box centre need not be selectable. The fixtures now check cropped
coverage and explicitly choose a selected interior point.

Both benchmark commands completed successfully. For a 2000×1500 source in a
4000×3000 document, transform frame timings are **mean / maximum milliseconds**:

| Sampling | One-layer rectangle | Three-layer ellipse |
| --- | ---: | ---: |
| Nearest | 142.4 / 166.4 | 149.6 / 179.7 |
| Bilinear | 302.8 / 356.5 | 320.0 / 357.1 |
| Bicubic | 459.1 / 508.5 | 484.2 / 545.1 |

All six opacity phases recorded 1,080 sampling-cache hits and zero misses;
charged cache storage stayed below 65,536 KiB. Bicubic opacity frames averaged
105.2 ms (one layer) and 195.1 ms (three layers). The phase benchmark initially
overlapped short regression/build activity; timings are informational and are not
an isolated performance comparison. [Full phase results](floating-selection-stage6-benchmarks.csv).

Real canvas-event benchmark results:

```text
BENCH 4000x3000 patch=300x200 press_ms=2.54551 frame_mean_ms=20.1188 frame_median_ms=20.1873 frame_max_ms=20.7521 stationary_20_ms=35.3598
BENCH resize patch=300x200 mean_ms=34.0062 median_ms=34.2371 max_ms=34.3443
BENCH rotate patch=300x200 mean_ms=38.0119 median_ms=36.1336 max_ms=42.9235
BENCH 4000x3000 patch=2000x1500 press_ms=3.21057 frame_mean_ms=20.3964 frame_median_ms=20.3414 frame_max_ms=20.8387 stationary_20_ms=33.9357
BENCH resize patch=2000x1500 mean_ms=497.015 median_ms=483.825 max_ms=534.703
BENCH rotate patch=2000x1500 mean_ms=538.677 median_ms=536.992 max_ms=542.696
```

No new performance target or improvement is claimed. The source sampler is
unchanged from accepted Stage 4C; high-quality large transforms remain deferred.

## Manual acceptance against Paint.NET

1. Copy an off-origin area and paste at fit and 100%: it should return to the
   copied coordinates. Zoom/pan so the copied area is entirely outside the view,
   horizontally, vertically and diagonally; paste and compare the edge position
   with Paint.NET. Repeat with a partly visible source, a sample larger than the
   viewport, another document, expansion and Paste into New Layer. Check rulers,
   canvas margins and Undo/Redo.
2. Rotate, release, resize, cross an opposite handle and relocate the pivot;
   Undo/Redo each gesture. Compare control frames with Paint.NET. The current
   frame is axis-aligned after a completed rotation; oriented-frame fidelity is
   a known difference, not newly implemented in this audit.
3. Enlarge, reduce and repeatedly rotate a small checkerboard and a feathered
   selection using all three modes. Confirm original-source detail survives
   uninterrupted transforms. Exact fractional-edge math, encoded-space filtering
   and strong anisotropic reductions remain the Stage 4C assumptions/limits.
4. Crop a transformed feathered selection over visible, hidden and offset layers;
   inspect transparency and edge alpha. Undo Crop, move the restored object,
   then Undo Finish. Compare history and appearance with Paint.NET.
5. Switch to Move Selection, alter the outline, return to Move Selected Pixels
   and move. Check only the newly selected artwork moves. Legacy outline controls
   lack pixel-tool custom-pivot/reflection parity and outline-specific undo history;
   this audit preserves that existing behaviour for separate product assessment.
6. Transfer soft/irregular selections between tabs, change opacity/blend, apply
   and cancel an effect, duplicate/reorder/remove layers, then Undo/Redo. Save,
   close the application and reopen. Check geometry, transparency and sampling.
7. On 4000×3000 documents, move/resize/rotate, hold stationary, drag off canvas,
   and adjust opacity. With several tabs and substantial undo history, monitor
   memory growth and report representative assets and operations if problematic.

These comparisons require human execution; automated tests cannot establish
visual equivalence with Paint.NET. Any additional behavioural changes revealed
by them should receive a concrete scope before implementation.

## Placement correction validation

Application and all four regression targets rebuilt successfully. Final CTest:
**4/4 passed**, 18.33 s; functional **1,914 checks**, paste **1,389 checks**,
plus brush and adjustment-preview suites. `git diff --check` passed.
New coverage includes original-position and partially visible placement,
horizontal/vertical/diagonal viewport edges, a smaller destination, a sample
larger than the viewport, actual zoom/pan Paste into New Layer, and Undo/Redo.
The existing external-image placement checks still pass. An older fill assertion
now checks the pasted selection's actual position rather than assuming origin.
No new CMake entries were needed because all changes use existing targets.

This correction adds constant-time placement geometry and does not change rendering;
performance benchmarks were not repeated after it. No commits or pushes were made.
Final human acceptance should compare the centre-line edge interpretation with
Paint.NET, especially partially visible areas and samples larger than the viewport.
