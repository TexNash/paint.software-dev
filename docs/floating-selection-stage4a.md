# Stage 4A — Floating-selection rendering and layer properties

Implementation complete; awaiting manual acceptance. Stage 5 and subsequent
stages have not begun. No commits or pushes were made.

## Profiling and scope

Production paths were profiled before modification against accepted Stage 3
(baseline `8912334`). The benchmark harness was added before capturing baseline
measurements. It separately exercises source sampling, mask generation, marquee
geometry, compositing, floating-layer/document thumbnails, UI invalidation,
opacity frames and transform frames.

For the Default build's 2000×1500 rotated rectangular source, standalone source
sampling averaged 146.6 ms, mask generation 134.0 ms, marquee rebuilding 106.2 ms
and compositing 38.5 ms. Floating-layer thumbnail sampling averaged 0.10 ms.
For the three-layer case, property invalidation/UI work averaged 6.56 ms, including
layer-list rebuilding and its ordinary-layer thumbnails. The issue was repeated
full-resolution work and unnecessary widget rebuilds, rather than the existing
bounded floating thumbnail sampler alone.

## Implementation

* One shared LRU cache has a **64 MiB charged-storage budget across all documents**.
  It uses immutable QImage values and holds no document/layer references. History
  snapshots and native files contain no cache objects.
* General affine transforms sample document-resolution source pixels and coverage
  together into 256×256 tiles. Mask assembly and rendering reuse those tiles.
  Keys contain immutable source/mask image identities, the complete transform
  and tile rectangle. Opacity, visibility and blending remain separate.
* Axis-aligned transforms retain the existing fast mask-copy/gather path. Their
  rendered source footprint is cached instead, with captured-base identity,
  offset and original-lift state included in its key. A nearby dirty rectangle
  can be retained directly to avoid an extra image copy; large empty bases are
  not retained for small sources. This path also excludes layer properties.
* Poses estimated to exceed the cache budget use the uncached renderer rather
  than repeatedly evicting their own samples. Entries for previous poses can
  remain until LRU eviction, permitting safe reuse after history navigation.
* Changed source pixels, independent geometry, transforms or base values select
  fresh keys. Changed document dimensions/clipping select the appropriate region.
  Suspended sessions use the current layer raster; resumed sessions capture fresh
  immutable inputs. Properties recompose valid pixels without resampling them.
* Layer-property notifications invalidate the **whole finite layer domain**,
  including transparent areas relevant to Overwrite. Layer rows and document-tab
  buttons refresh in place. Adjustment previews cancel on property changes.
* A translated marching outline is reused only after an exact comparison of old
  and new coverage confirms matching geometry. Otherwise the existing path is
  rebuilt. Its retained mask is a COW reference, not a deep raster copy.
* Pointer coalescing, final release handling, nearest-neighbour sampling and
  coverage replacement math remain unchanged. No lower-resolution canvas preview,
  worker renderer, new control or serialization change is introduced.

The cache uses full transform keys deliberately. Reusing normalized rotated
samples across integer translations could shift floating-point boundary rounding;
this implementation does not make that assumption.

## Repeatable measurements

Measured with Qt 6.10.2 on an Intel i9-11900H host, offscreen, using identical
fixtures before and after. Default has empty CMAKE_BUILD_TYPE and no optimization
flags; Release uses `-O3 -DNDEBUG`. Compilation and tests were not running during
the measured comparisons. Results are single runs with multiple samples per case;
scheduling/frequency variation remains possible.

All documents are 4000×3000. The event benchmark measures eight move frames and
three resize/rotation frames, including a requested 17 ms event-loop interval.
The component/profile benchmark measures twelve operations per phase without
that interval. Its transform phase translates an already-rotated source and
includes the initial unchanged pose in both versions. The three-layer fixture
has an antialiased elliptical mask, an offset target and an upper Multiply layer.
Opacity frames include property invalidation and canvas rendering, with warmed
samples. These are elapsed CPU/UI-path measurements, not display-latency promises.

Values below are **mean / maximum observed milliseconds**. For twelve samples,
the benchmark's nearest-rank p95 is also the maximum. The full component results
are in [the CSV](floating-selection-stage4a-benchmarks.csv).

| Case / operation | Default before | Default after | Release before | Release after |
|---|---:|---:|---:|---:|
| 2000x1500 / move | 68.5 / 71.8 | 57.5 / 64.1 | 39.7 / 40.7 | 37.1 / 43.3 |
| 2000x1500 / resize | 123.9 / 127.4 | 121.8 / 125.0 | 41.2 / 42.1 | 37.2 / 39.4 |
| 2000x1500 / rotate | 287.1 / 300.6 | 233.7 / 242.1 | 85.7 / 91.7 | 75.7 / 77.8 |
| 1 layer / rectangle / opacity_frame | 226.9 / 274.2 | 118.1 / 135.3 | 89.2 / 90.3 | 69.5 / 72.4 |
| 1 layer / rectangle / transform_frame | 444.2 / 510.4 | 247.3 / 278.7 | 179.7 / 222.8 | 66.9 / 80.6 |
| 3 layers / ellipse / opacity_frame | 304.4 / 329.4 | 205.8 / 211.5 | 191.1 / 206.9 | 146.3 / 156.4 |
| 3 layers / ellipse / transform_frame | 400.1 / 473.0 | 262.3 / 300.0 | 165.6 / 215.6 | 79.8 / 93.6 |

Small selections remain responsive:

| Case / operation | Default before | Default after | Release before | Release after |
|---|---:|---:|---:|---:|
| 300x200 / move | 21.8 / 24.7 | 21.7 / 24.1 | 21.2 / 23.4 | 21.3 / 22.5 |
| 300x200 / resize | 23.7 / 25.0 | 23.5 / 25.5 | 20.7 / 22.6 | 20.8 / 22.6 |
| 300x200 / rotate | 33.4 / 35.3 | 32.0 / 34.3 | 24.8 / 26.2 | 24.2 / 25.5 |

Both final opacity scenarios recorded **1,080 sample-cache hits and zero misses**
over twelve frames. The current rotated pose occupied about 22.7 MiB; retained
older poses brought the shared pool close to, but below, its 64 MiB budget.
Twenty identical held-pointer updates took 35.9 ms / 37.3 ms total in Default
(small / large patch), and 30.2 ms / 29.4 ms in Release. No stationary resampling
or accumulated input work is introduced.

Standalone source/mask/path probes deliberately bypass caching or force rebuilding.
Their timings are diagnostic costs, not a claim that the underlying sampling or
path algorithm became faster. Gains come from sharing/avoiding repeated work.

```bash
cmake -B build
cmake --build build --target paintdotnet_paste_tests -j 4
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-floating
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-stage4a

cmake -S . -B /tmp/paint4a-release -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/paint4a-release --target paintdotnet_paste_tests -j 4
QT_QPA_PLATFORM=offscreen /tmp/paint4a-release/paintdotnet_paste_tests --benchmark-floating
QT_QPA_PLATFORM=offscreen /tmp/paint4a-release/paintdotnet_paste_tests --benchmark-stage4a
```

These commands measure the current implementation. Baseline numbers above were
captured before the production changes using the same benchmark operations.
The normal application was built in `build`; the Release comparison builds the
paste benchmark executable containing the application components.

## Verification and review

The application built successfully and all four CTest suites passed:

* Functional: **1,717 checks**, zero failures.
* Paste: **1,292 checks**, zero failures.
* Brush regression: passed.
* Adjustment preview: passed.

New coverage checks cached versus uncached composition across every existing
blend mode and several opacities, partial alpha/coverage, offsets, unaligned
regions, off-canvas transforms, source replacement, suspended/resumed painting,
history and native reopening. Cache tests cover mutation isolation, eviction,
repopulation, independent documents and bounded storage under pressure. UI tests
compare regional property updates with forced full redraws, check row/tab reuse,
verify property-dialog cancellation and reject stale adjustment snapshots.
Existing coalescing, selection, history, layer and native-save regressions pass.
`git diff --check` passes. Review found no blocking ownership/lifetime concern.

Changed files:

* `src/core/floatingrendercache.{h,cpp}`: shared bounded cache and diagnostics.
* `src/core/floatingpixelsession.{h,cpp}`: combined source/coverage sampling and
  reusable base rendering; the uncached renderer remains a reference path.
* `src/core/document.{h,cpp}`: cache integration, keys and appearance notifications.
* `src/canvas/canvaswidget.{h,cpp}`: finite-domain repaint and exact outline reuse.
* `src/core/adjustmentpreview.cpp`: property-change invalidation.
* `src/mainwindow.cpp`: live property previews and notification subscriptions.
* `src/panels/imagelistbar.{h,cpp}`, `src/panels/layerspanel.cpp`: in-place UI refresh.
* `CMakeLists.txt`: cache source in shared core sources for all relevant targets.
* `tests/test_all.cpp`, `tests/test_paste.cpp`: regressions and opt-in profiling.
* This report and its CSV: methods, measurements and acceptance instructions.

## Remaining limitations and concerns

* Large changed rotations/scales still need sampling, mask work and sometimes
  marquee reconstruction. A cache cannot eliminate computation for every new pose.
* Opacity/blend changes still recomposite the affected layer stack and repaint
  full-resolution pixels. Multiple large layers and custom blend modes can remain
  visibly laggy; measurements do not establish smooth feedback on every machine.
* The 64 MiB budget bounds retained cache storage, not existing document/history
  buffers or transient rendering images. The LRU can retain derived images after
  a document closes until eviction; it does not keep that document alive.
* Eviction and very large poses reduce reuse. Full-size selection masks and
  synchronous save/materialization remain unchanged.
* Exact sampling quality, advanced handles and mask-aware cropping remain for
  their separately approved stages. Future sampler choices must become cache-key
  inputs; no such modes are implemented here.

## Recommended manual acceptance

1. Repeat 300×200 and 2000×1500 selections on 4000×3000 documents. Move, resize,
   rotate and rapidly change direction; hold stationary and release. Check the
   final location and absence of delayed movement or accumulated lag.
2. Leave a large paste unfinished, rotate/resize it, open Layer Properties (F4)
   and drag opacity repeatedly. Test both the pasted active layer and layers above
   and below it; repeat with multiple layers. Cancel, reopen and accept changes.
3. Repeat with partial transparency, feathered/irregular masks, Multiply,
   Overwrite and a custom blend mode. Move partly and fully off-canvas. Inspect
   old positions, transparent holes and areas beyond the source footprint.
4. Undo/redo gestures and properties; switch tools/layers and resume Move. Save
   unfinished native documents, close the application, reopen and move the content.
5. Keep several large documents open and alternate between them, including pastes
   and opacity changes. Confirm their pixels/selections remain independent and
   memory does not grow by another cache budget for every tab.
