# Stage 4C — transformation sampling and fractional edges

Implementation complete; awaiting manual acceptance. Stage 6 has not begun.
No commits or pushes were made.

## Behaviour and explicit assumptions

Move Selected Pixels now offers Nearest Neighbor, Bilinear and Bicubic in its
tool-options row. New sessions default to Bicubic, following the documented
Paint.NET default: <https://paint.net/doc/latest/MoveTools.html>. Nearest Neighbor
is available for pixel art. Integer translations remain byte-exact in every mode.
The low-level session factory retains its original Nearest default; high-level
paste and ordinary lifts use the tool's selected policy.

The user authorised reasonable edge assumptions, subject to later corrections
from practical game-development use. These are the chosen policies, rather than
claims of pixel-for-pixel Paint.NET equivalence:

* Filter original premultiplied colour and independent Gray8 selection coverage
  together. Accumulate colour weighted by coverage, then normalise by filtered
  coverage. Unselected colours cannot bleed into a selected edge. Fully selected
  transparent pixels retain geometry and can still replace destination pixels.
  Reconstruction uses float channel accumulation (SSE2 on x86, with a scalar
  implementation elsewhere); exact rounding is an implementation assumption.
* Bilinear uses a 2×2 pixel-centred kernel. Bicubic uses Catmull-Rom convolution
  (`a=-0.5`, 4×4). Clamp resulting coverage to 0–255 and colour to valid
  premultiplied channels. Cubic reconstruction can sharpen or ring; clamping
  prevents invalid channel/alpha values but does not guarantee zero ringing.
* For reductions along both axes, use coverage-weighted 2×2 prefilter levels
  before reconstruction. This avoids aliasing fine checkerboards into one colour.
  Strong one-axis/anisotropic reductions retain a quality limitation; these
  modes do not claim to implement Paint.NET's Anisotropic filter.
* Sampling occurs in the current encoded premultiplied channel space. Gamma
  correction and full colour-management changes are not included.
* Clamp reconstruction taps at the outer source frame, and calculate its
  geometric pixel coverage separately by clipping the inverse-mapped destination
  pixel against that frame. Opaque integer enlargements therefore have no
  artificial translucent fringe; fractional/rotated boundaries are antialiased.
  Pixel-cell bounds include all contributing pixels without expanding a layer's
  finite Overwrite domain. Extremely ill-conditioned transforms use a bounded
  overlap fallback if polygon area cannot be represented reliably.
* Existing replacement compositing remains coverage interpolation, not
  source-over. Crop still applies its separate coverage multiplication once.
  An already softened edge can therefore receive another attenuation during
  Crop; exact Paint.NET fractional crop behaviour remains unconfirmed.

Paint.NET's Multisample Bilinear, Anisotropic and gamma controls are not exposed
under substitute algorithms. These remain fidelity gaps for later review.

## Architecture and changed files

* `src/core/floatingpixelsession.cpp/.h`: explicit sampling policy; filtered
  reconstruction, geometric coverage and coverage-aware prefilter levels;
  encapsulated sampling bounds;
  consistent direct rendering, coverage, transformed clipboard source and bounded
  thumbnail samples. Immutable original pixels/coverage and pivot rules remain.
* `src/core/document.cpp/.h`: policy changes are undoable. Old/new support regions
  and finite layer-domain changes invalidate correctly. Sample caches include the
  policy. Filtered axis-aligned work shares source/coverage tiles, while Nearest
  retains its fast row-copy/gather paths. Context lifts include filtered edges.
* `src/tools/movetool.cpp/.h`: sampling preference for future ordinary lifts.
* `src/panels/tooloptionspanel.cpp/.h`, `src/mainwindow.cpp`: Move-specific dropdown,
  translated labels, active-session changes, gesture settling, history/document
  synchronisation and paste preference. Locked targets reject sampling edits.
  Existing brush/wand sampling controls
  remain independent.
* `CMakeLists.txt`: optimise the sampler translation unit with `-O2` for an
  otherwise unoptimised, unspecified single-config GNU/Clang build. Explicit
  Debug, Release and multi-config builds retain their normal optimisation flags.
  Every application/test target uses the shared source and this setting.
* `tests/test_all.cpp`, `tests/test_paste.cpp`: analytical samples, transparent
  boundaries, opaque enlargement, rotated half-pixel areas, odd-size reductions,
  prefiltering, cache/mask/compositing agreement, old/new invalidation,
  native metadata validation, UI/history/reopen and benchmark modes.

## Native compatibility

The v1/v2 layer prefix and FPS1 payload remain intact. A filtered session adds an
optional versioned SMP1 trailer containing the sampling enum. Nearest sessions
retain the previous layout. New readers load old sessions as Nearest, preserving
their original appearance; new files restore their chosen mode. Unknown policy
versions, invalid enum values and truncated recognised metadata are rejected
before replacing the open document. The transient pivot is still not saved.

Layer-only readers can use the materialised layer prefix. Older FPS1-aware
versions that ignore SMP1 will reconstruct the editable session with Nearest;
they cannot reproduce the new filter. No caches or prefilter pyramids are saved.

## Resource, lifetime and performance review

Prefilter levels share the existing process-wide 64 MiB LRU with sampled tiles;
they are keyed by immutable source and geometry identities and level. They are
not retained by Document, sessions or history. Returned QImage values keep sample
buffers alive during eviction. No widgets/documents are owned by cache entries.
Temporary render buffers and source/history storage remain outside the cache's
charged storage budget, as before. No new workers, asynchronous rendering or
lower-resolution canvas previews were introduced.

Bounded thumbnails reuse available prefilter levels but never build them. With
a cold/evicted pyramid they use bounded direct sampling, so a thumbnail of a
strong reduction can differ from the fully prefiltered canvas. Fully off-canvas
coverage requests skip prefilter construction as well. Both have regressions.

Layer opacity and blend changes reuse transformed samples. Pointer-event
coalescing, exact final release, unchanged-transform suppression, custom pivot
history and source immutability remain in place. Strong reductions use prefilter
levels as part of full-resolution reconstruction, not as reduced canvas previews.

The initial no-build-type measurements exposed roughly 1.2-second Bicubic frames
for a rotated 2000×1500 source. This motivated the local sampler optimisation.
Benchmark comparisons must account for that compiler-setting change: the CMake
configuration is the same, but the delivered sampler TU uses `-O2`. This is not
an algorithm-only speed comparison. Explicit unoptimised Debug builds can still
be substantially slower; Nearest remains the least expensive option.

## Validation and repeatable benchmarks

Benchmarks use the existing 4000×3000 document scenarios, with a 2000×1500 rotated
rectangle on one layer and an irregular ellipse on three layers. Each phase has
12 samples; reported p95 is the maximum of this small sample set. The Stage 4B
baseline was captured before modifying sampling. The final run evaluates all
three modes and also adds 4× reduction frames. Runs are sequential; timing is
informational and not a CI threshold.

```bash
cmake -B build
cmake --build build --target paintdotnet paintdotnet_tests paintdotnet_paste_tests paintdotnet_brush_tests paintdotnet_preview_tests -j 4
ctest --test-dir build --output-on-failure
QT_QPA_PLATFORM=offscreen ./build/paintdotnet_paste_tests --benchmark-stage4c
```

Measured on the i9-11900H x86-64 host, using the same unspecified CMake build
type and offscreen Qt platform. The delivered sampler has the local `-O2` setting
and SSE2 channel accumulation described above. Values are **mean / maximum ms**;
p95 equals maximum with these 12-sample runs.

| Rotated 2000×1500 transform frame | One-layer rectangle | Three-layer ellipse |
| --- | ---: | ---: |
| Before: Nearest, unoptimised sampler | 261.4 / 304.5 | 262.3 / 293.9 |
| After: Nearest | 136.1 / 153.1 | 153.8 / 175.2 |
| After: Bilinear | 317.0 / 367.2 | 341.4 / 421.3 |
| After: Bicubic | 488.4 / 536.7 | 494.1 / 547.9 |

Opacity frames remain independent of filter cost: Bicubic averaged 115.3 ms
(one layer) and 202.5 ms (three layers), versus 126.1/208.9 ms before. Every mode
and scene recorded 1,080 cache hits and **zero sampling-cache misses** across its
opacity frames. Cache charged storage stayed below the shared 65,536 KiB limit.

Warm 4× reduction frames averaged 17.8/22.3 ms with Bicubic (max 29.9/53.2 ms).
These exclude initial prefilter construction, which occurs when entering the
reduced pose before the repeated frame measurements. Very large rotated/scaled
selections still visibly lag in filtered modes; these numbers are a quality/cost
tradeoff, not a claim of universally smooth interaction. Integer movement of an
untransformed paste retains the fast exact-copy path.

Full phase measurements: [floating-selection-stage4c-benchmarks.csv](floating-selection-stage4c-benchmarks.csv).

The application and all four regression executables built successfully. Final
`ctest --test-dir build --output-on-failure`: functional **1,893 checks**, paste
**1,347 checks**, brush and adjustment preview all passed; zero failures across
four suites, 17.34 s total. `git diff --check` passed. Remaining build warnings are
the existing Qt `QImage::mirrored()` deprecations in unrelated transform code.

## Manual acceptance for game assets

1. Enlarge a small asymmetric sprite using Nearest Neighbor. Inspect individual
   pixels and hard edges, then switch to Bilinear/Bicubic to compare smoothing.
2. Rotate a transparent sprite over light and dark backgrounds. Check for colour
   bleed, dark fringes, clipping, and cubic sharpening at high-contrast edges.
3. Reduce a fine checkerboard or texture substantially. Compare detail and
   aliasing in each mode; also check strongly nonuniform reductions separately.
4. Switch modes on a rotated/scaled live object; verify Undo/Redo, clipboard Copy,
   Finish, tool/layer boundaries and Crop. Check transparent interiors and masks.
5. Save, fully close and reopen a filtered session. Mode, pixels and geometry
   should return; a custom pivot should reset, as in Stage 4B.
6. Test all modes on 4000×3000 documents, including large selections, opacity,
   non-normal blends, stationary holds and movement off canvas. Record assets
   where quality or interaction differs from expectations for later refinement.
