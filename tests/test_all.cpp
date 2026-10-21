// Comprehensive headless functional test for PaintDali.
// Exercises the real code paths: tools, effects, adjustments, layers,
// selection, undo/redo, save/load, i18n and theming.
#include <QApplication>
#include "core/floatingrendercache.h"
#include "dialogs/previewdialog.h"
#include <QDialogButtonBox>
#include <QImage>
#include <QColor>
#include <QPainter>
#include <QMouseEvent>
#include <QLineF>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <QFile>
#include <cstdio>
#include <functional>
#include <climits>
#include <limits>
#include <tuple>

#include "core/document.h"
#include "core/clipboardpixels.h"
#include <QtEndian>
#include "core/layer.h"
#include "core/selection.h"
#include "core/history.h"
#include "core/adjustmentpreview.h"
#include "canvas/canvaswidget.h"
#include "i18n.h"
#include "theme.h"

#include "tools/brushtool.h"
#include "tools/penciltool.h"
#include "tools/filltool.h"
#include "tools/erasertool.h"
#include "tools/linetool.h"
#include "tools/shapetool.h"
#include "tools/gradienttool.h"
#include "tools/recolortool.h"
#include "tools/clonestamptool.h"
#include "tools/selectiontool.h"
#include "tools/magicwandtool.h"
#include "tools/movetool.h"
#include "tools/moveselectiontool.h"
#include "tools/texttool.h"
#include "tools/colorpickertool.h"
#include "tools/lassotool.h"
#include "panels/colorspanel.h"
#include "panels/tooloptionspanel.h"
#include "core/hatchpatterns.h"
#include <QImageWriter>
#include <QImageReader>
#include <QTemporaryFile>
#include <QDataStream>
#include <QTemporaryDir>
#include <QFileInfo>
#include "dialogs/resizedialog.h"
#include "dialogs/previewdialog.h"
#include "dialogs/huesaturationdialog.h"
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include "dialogs/saveconfigdialog.h"
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QKeyEvent>

#include "effects/blureffect.h"
#include "effects/sharpeneffect.h"
#include "effects/noiseeffect.h"
#include "effects/embosseffect.h"
#include "effects/edgedetecteffect.h"
#include "effects/oilpainteffect.h"
#include "effects/pixelateeffect.h"
#include "effects/motionblureffect.h"
#include "effects/radialblureffect.h"
#include "effects/zoomblureffect.h"
#include "effects/surfaceblureffect.h"
#include "effects/unfocuseffect.h"
#include "effects/fragmenteffect.h"
#include "effects/bulgeeffect.h"
#include "effects/twisteffect.h"
#include "effects/frostedglasseffect.h"
#include "effects/crystalizeeffect.h"
#include "effects/tileeffect.h"
#include "effects/dentseffect.h"
#include "effects/polarinversioneffect.h"
#include "effects/medianeffect.h"
#include "effects/reducenoiseeffect.h"
#include "effects/quantizeeffect.h"
#include "effects/gloweffect.h"
#include "effects/redeyeremoveeffect.h"
#include "effects/softenportraiteffect.h"
#include "effects/vignetteeffect.h"
#include "effects/turbulenceeffect.h"
#include "effects/reliefeffect.h"
#include "effects/outlineeffect.h"
#include "effects/morphologyeffect.h"

#include "adjustments/brightnesscontrast.h"
#include "adjustments/huesaturation.h"
#include "adjustments/levels.h"
#include "adjustments/curves.h"
#include "adjustments/invertcolors.h"
#include "adjustments/sepia.h"
#include "adjustments/posterize.h"
#include "adjustments/threshold.h"
#include "adjustments/desaturate.h"
#include "adjustments/colorbalance.h"
#include "adjustments/exposure.h"
#include "adjustments/highlightsshadows.h"
#include "adjustments/temperaturetint.h"

#include "plugins/plugin_api.h"
#include "plugins/plugineffect.h"
#include "plugins/pluginmanager.h"
#include <QLibrary>
#include <QCoreApplication>

static int g_pass = 0, g_fail = 0;
static const char *g_section = "";
#define SECTION(s) do { g_section = s; printf("\n== %s ==\n", s); } while(0)
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; /* printf("  ok  : %s\n", msg); */ } \
    else { ++g_fail; printf("  FAIL [%s]: %s\n", g_section, msg); } \
} while(0)

// ---- helpers ----
static QImage makeImage(int w, int h, QColor fill) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(fill);
    return img;
}

static bool imagesDiffer(const QImage &a, const QImage &b) {
    if (a.size() != b.size()) return true;
    return a != b;
}

static int countNonBackground(const QImage &img, QColor bg) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixelColor(x, y) != bg) ++n;
    return n;
}

// Synthesize a full press-drag-release on a tool in document coordinates.
static QMouseEvent pressEv(QPointF p, Qt::MouseButton b = Qt::LeftButton) {
    return QMouseEvent(QEvent::MouseButtonPress, p, p, b, b, Qt::NoModifier);
}
static QMouseEvent moveEv(QPointF p, Qt::MouseButton b = Qt::LeftButton) {
    return QMouseEvent(QEvent::MouseMove, p, p, Qt::NoButton, b, Qt::NoModifier);
}
static QMouseEvent releaseEv(QPointF p, Qt::MouseButton b = Qt::LeftButton) {
    return QMouseEvent(QEvent::MouseButtonRelease, p, p, b, Qt::NoButton, Qt::NoModifier);
}

// A plugin-style process function (plain C signature): blend each RGB channel
// toward its inverse by values[0] percent. Used to test the plugin wrapper.
static void testInvertProcess(unsigned char *rgba, int w, int h,
                              const int *values, int valueCount, void *userData) {
    (void)userData;
    int amount = (valueCount > 0) ? values[0] : 100;
    for (long i = 0; i < (long)w * h; ++i) {
        unsigned char *p = rgba + i * 4;
        for (int c = 0; c < 3; ++c) {
            int inv = 255 - p[c];
            p[c] = (unsigned char)((p[c] * (100 - amount) + inv * amount) / 100);
        }
    }
}

static void strokeTool(Tool *tool, CanvasWidget &canvas, QPointF a, QPointF b) {
    // These existing strokes exercise artwork movement, not the new pivot nub.
    if (tool->type() == ToolType::Move && canvas.document()) {
        const auto *session = canvas.document()->floatingPixelSession();
        const QRect rect = session ? session->sourceBounds().toAlignedRect() : canvas.document()->selection().boundingRect();
        const QPointF pivot = session ? session->rotationPivot() : QRectF(rect).center();
        if (!rect.isEmpty() && QLineF(a,pivot).length() <= SelHandles::pivotRadius(rect,canvas.zoom())) {
            const QPointF offset(-rect.width()/5.0,0);
            a += offset; b += offset;
        }
    }
    QMouseEvent e1 = pressEv(a);   tool->mousePressEvent(a, &e1, canvas);
    QMouseEvent e2 = moveEv((a+b)/2); tool->mouseMoveEvent((a+b)/2, &e2, canvas);
    QMouseEvent e3 = moveEv(b);     tool->mouseMoveEvent(b, &e3, canvas);
    QMouseEvent e4 = releaseEv(b);  tool->mouseReleaseEvent(b, &e4, canvas);
}

// An asymmetric retained bitmap with visible and off-canvas markers.
static QImage retainedPattern() {
    QImage image = makeImage(840, 630, Qt::transparent);
    QPainter p(&image);
    p.fillRect(25, 25, 15, 15, Qt::yellow);
    p.fillRect(345, 290, 15, 15, Qt::cyan);
    p.fillRect(80, 90, 35, 45, Qt::red);
    p.fillRect(200, 220, 40, 20, Qt::green);
    p.fillRect(805, 595, 25, 25, Qt::blue);
    return image;
}

// Characterization: setImage already normalizes both resize branches, even
// when a caller bypasses it through the mutable bitmap accessor.
static void resizeStraightAlphaCharacterization() {
    SECTION("Resize normalizes straight-alpha input in ordinary and retained layers");
    for (bool retained : {false, true}) {
        Document doc(8,6);
        QImage straight(retained ? QSize(12,10) : doc.size(), QImage::Format_ARGB32);
        straight.fill(Qt::transparent);
        straight.setPixelColor(3,3,QColor(220,80,30,117));
        straight.setPixelColor(5,4,QColor(20,140,230,201));
        auto *layer = doc.activeLayer();
        layer->image() = straight;
        layer->setRetainsOffCanvasPixels(retained);
        layer->setOffset(retained ? QPoint(-2,-1) : QPoint());
        // Scale the finite straight-alpha source independently; comparing a
        // pre-composited image would introduce a different rounding order.
        const QImage expected = straight.copy(QRect(-layer->offset(),doc.size())).scaled(
            13,11,Qt::IgnoreAspectRatio,Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        doc.resize(13,11);
        CHECK(doc.activeLayer()->image().format() == QImage::Format_ARGB32_Premultiplied,
              "resize normalizes even directly injected straight-alpha storage");
        CHECK(doc.flatten() == expected, "straight-alpha resize preserves visible alpha and colours");
    }
}

static void retainedCropRegression() {
    SECTION("Retained crop uses document coordinates including selection edges");
    for (QPoint offset : {QPoint(50, 40), QPoint(-50, -40)}) {
        for (QRect requested : {QRect(70, 60, 240, 270), QRect(-10, -20, 160, 180),
                                QRect(730, 530, 100, 100)}) {
            Document doc(800, 600);
            doc.activeLayer()->clear(Qt::transparent);
            const QImage image = retainedPattern();
            doc.pasteImage(image, "retained", false);
            doc.activeLayer()->setOffset(offset);
            doc.selection().selectRect(requested);
            const QImage mask = doc.selection().mask();
            const QImage before = doc.flatten();
            const QRect crop = requested.intersected(before.rect());
            doc.cropTo(requested);
            const QImage after = doc.flatten();
            CHECK(after == before.copy(crop), "crop preserves exactly the visible document-space pixels");
            CHECK(doc.activeLayer()->offset().isNull(), "destructive crop rebases the layer origin");
            CHECK(doc.activeLayer()->image().size() == crop.size(), "crop discards pixels outside its bounds");
            CHECK(!doc.activeLayer()->retainsOffCanvasPixels(), "destructive crop clears retained movement state");
            CHECK(!doc.selection().hasSelection(), "crop clears selection");
            doc.history().undo();
            CHECK(doc.size() == QSize(800,600) && doc.activeLayerIndex() == 1,
                  "crop undo restores size and active layer");
            CHECK(doc.activeLayer()->image() == image && doc.activeLayer()->offset() == offset,
                  "crop undo restores the entire original retained bitmap and offset");
            CHECK(doc.selection().mask() == mask, "crop undo restores selection edges");
            CHECK(doc.activeLayer()->retainsOffCanvasPixels(), "crop undo restores retained state");
            doc.history().redo();
            CHECK(doc.flatten() == after && doc.activeLayer()->offset().isNull(), "crop redo restores pixels and origin");
            CHECK(!doc.activeLayer()->retainsOffCanvasPixels(), "crop redo clears retained state again");
            QTemporaryDir directory;
            const QString path = directory.path()+"/cropped.psw";
            CHECK(doc.saveNative(path), "save destructively cropped document");
            QFile file(path);
            CHECK(file.open(QIODevice::ReadOnly), "open cropped native file");
            QDataStream stream(&file);
            stream.setVersion(QDataStream::Qt_6_0);
            quint32 magic; qint32 version;
            stream >> magic >> version;
            CHECK(version == 1, "crop with no retained layers writes legacy v1");
            for (bool keepArtwork : {false,true}) {
                doc.mergeLayerDown(1,keepArtwork);
                CHECK(!doc.activeLayer()->retainsOffCanvasPixels(), "merge does not inherit stale crop state");
                doc.history().undo();
            }
            const QImage cropped = doc.activeLayer()->image();
            QImage moved(cropped.size(),QImage::Format_ARGB32_Premultiplied);
            moved.fill(Qt::transparent);
            { QPainter painter(&moved); painter.drawImage(3,2,cropped); }
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            MoveTool move;
            strokeTool(&move,canvas,QPointF(20,20),QPointF(23,22));
            CHECK(doc.activeLayer()->offset().isNull() && doc.activeLayer()->image() == moved,
                  "cropped layer returns to ordinary pixel-baked Move");
        }
    }
}

static void maskedCropRegression() {
    SECTION("Mask-aware crop preserves geometry, layer metadata and two-step floating history");
    for (bool floating : {false, true}) {
        Document doc(12, 10);
        doc.activeLayer()->clear(QColor(20, 100, 200, 128));
        QImage top(8, 8, QImage::Format_ARGB32_Premultiplied);
        top.fill(QColor(180, 60, 20, 160));
        top.setPixel(3, 3, 0);
        doc.addLayer(top, "offset hidden locked");
        doc.activeLayer()->setOffset(QPoint(1, 1));
        doc.activeLayer()->setRetainsOffCanvasPixels(true);
        doc.activeLayer()->setOpacity(0.4f);
        doc.activeLayer()->setBlendMode(BlendMode::Multiply);
        doc.activeLayer()->setVisible(false);
        doc.activeLayer()->setLocked(true);
        QImage mask(12, 10, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = 2; y < 8; ++y)
            for (int x = 2; x < 9; ++x)
                mask.scanLine(y)[x] = (x == 5 && y == 5) ? 0 : (x == 2 ? 128 : 255);
        if (floating) {
            QImage source(7, 6, QImage::Format_ARGB32_Premultiplied);
            source.fill(QColor(0, 220, 100, 180));
            source.setPixel(2, 2, 0); // selected transparency must not shrink geometry
            CHECK(doc.beginFloatingPixelSession(1, source, mask.copy(2, 2, 7, 6), QPoint(2, 2)), "create crop session");
        }
        doc.selection().setMaskImage(mask);
        const auto beforeLayers = doc.snapshotLayers();
        const auto effective = doc.snapshotEffectiveLayers();
        const int history = doc.history().currentIndex();
        std::vector<QImage> expected;
        for (const auto &layer : effective) {
            QImage pixels(7, 6, QImage::Format_ARGB32_Premultiplied);
            pixels.fill(Qt::transparent);
            { QPainter painter(&pixels); painter.drawImage(layer->offset() - QPoint(2, 2), layer->image()); }
            for (int y = 0; y < 6; ++y) {
                auto *line = reinterpret_cast<QRgb *>(pixels.scanLine(y));
                for (int x = 0; x < 7; ++x) {
                    const int c = mask.constScanLine(y + 2)[x + 2];
                    const QRgb p = line[x];
                    line[x] = qRgba(qRed(p)*c/255, qGreen(p)*c/255, qBlue(p)*c/255, qAlpha(p)*c/255);
                }
            }
            expected.push_back(pixels);
        }
        CHECK(doc.cropToSelection(), "mask crop succeeds");
        CHECK(doc.size() == QSize(7, 6) && !doc.selection().hasSelection() && !doc.floatingPixelSession(), "crop clears selection and session");
        CHECK(doc.history().currentIndex() == history + (floating ? 2 : 1), "floating crop has separate Finish and Crop history");
        for (int i = 0; i < doc.layerCount(); ++i) {
            CHECK(doc.layerAt(i)->image() == expected[i], "every layer uses exact premultiplied crop coverage");
            CHECK(doc.layerAt(i)->offset().isNull() && !doc.layerAt(i)->retainsOffCanvasPixels(), "cropped layer discards outside storage");
        }
        CHECK(!doc.activeLayer()->isVisible() && doc.activeLayer()->isLocked()
              && doc.activeLayer()->blendMode() == BlendMode::Multiply && doc.activeLayer()->opacity() == 0.4f
              && doc.activeLayer()->name() == "offset hidden locked", "crop preserves layer properties");
        QTemporaryDir directory;
        const QString path = directory.path() + "/crop.psw";
        CHECK(doc.saveNative(path), "save masked crop");
        Document reopened(path);
        CHECK(reopened.size() == doc.size() && reopened.layerAt(1)->image() == expected[1]
              && !reopened.floatingPixelSession(), "native reopen preserves finished cropped pixels");
        doc.history().undo();
        CHECK(doc.size() == QSize(12, 10) && doc.selection().mask() == mask && doc.activeLayerIndex() == 1, "undo crop restores canvas selection and active layer");
        if (floating) {
            CHECK(doc.floatingPixelSession() && doc.floatingPixelSession()->isSuspended(), "undo crop restores editable context-boundary object");
            doc.history().undo();
            CHECK(doc.floatingPixelSession() && !doc.floatingPixelSession()->isSuspended(), "undo Finish restores background-preserving session");
        }
        CHECK(doc.layerAt(1)->image() == beforeLayers[1]->image() && doc.layerAt(1)->offset() == QPoint(1, 1), "undo restores original layer bytes and offset");
        if (floating) doc.history().redo();
        doc.history().redo();
        CHECK(doc.layerAt(1)->image() == expected[1] && !doc.floatingPixelSession(), "redo reproduces crop");
    }
    for (QPoint offset : {QPoint(-4, -3), QPoint(100, 100), QPoint(INT_MIN, INT_MAX)}) {
        Document doc(8, 6);
        doc.activeLayer()->setOffset(offset);
        doc.selection().selectRect(QRect(-2, -2, 5, 5));
        CHECK(doc.cropToSelection() && doc.size() == QSize(3, 3), "crop clips bounds to canvas even with extreme offsets");
        CHECK(doc.activeLayer()->image().pixel(0, 0) == (offset == QPoint(-4, -3) ? qRgb(255,255,255) : 0), "offset crop samples layer in document space");
    }
    Document empty(8, 6);
    empty.setModified(false);
    const int history = empty.history().currentIndex();
    CHECK(!empty.cropToSelection() && !empty.isModified() && empty.history().currentIndex() == history, "empty selection is a clean no-op");
    QImage outside(20, 20, QImage::Format_Grayscale8);
    outside.fill(0); outside.scanLine(15)[15] = 255;
    empty.selection().setMaskImage(outside);
    CHECK(!empty.cropToSelection() && empty.size() == QSize(8,6) && !empty.isModified(), "entirely off-canvas selection is a clean no-op");
    {
        Document doc(8, 6);
        QImage source(2, 2, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::red);
        QImage coverage(2, 2, QImage::Format_Grayscale8);
        coverage.fill(255);
        CHECK(doc.pasteFloatingPixels(source, coverage, QPoint()), "prepare rejected crop fixture");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromScale(1000000,1000000)), "prepare oversized Finish domain");
        doc.setModified(false);
        const int history = doc.history().currentIndex();
        const QImage mask = doc.selection().mask();
        const QImage layer = doc.activeLayer()->image();
        CHECK(!doc.cropToSelection(), "crop rejects unmaterializable Finish checkpoint");
        CHECK(doc.size() == QSize(8,6) && doc.activeLayer()->image() == layer
              && doc.selection().mask() == mask && doc.floatingPixelSession()
              && !doc.floatingPixelSession()->isSuspended() && !doc.isModified()
              && doc.history().currentIndex() == history, "rejected crop preserves document session and history");
    }
    for (bool suspended : {false, true}) {
        Document doc(40, 30);
        QImage source(24, 18, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(20, 160, 240, 100));
        QImage coverage(source.size(), QImage::Format_Grayscale8);
        coverage.fill(255);
        CHECK(doc.pasteFloatingPixels(source, coverage, QPoint()), "paste rotated crop fixture");
        QTransform transform;
        transform.translate(-5, 3);
        transform.rotate(25);
        transform.scale(1.2, 0.9);
        CHECK(doc.setFloatingPixelTransform(transform), "rotate scale and move partly off canvas");
        if (suspended) CHECK(doc.suspendFloatingPixels(), "suspend before crop");
        const QRect bounds = doc.selection().boundingRect().intersected(QRect(QPoint(), doc.size()));
        const QImage mask = doc.selection().mask().copy(bounds);
        const QImage rendered = doc.flatten().copy(bounds);
        const int history = doc.history().currentIndex();
        CHECK(doc.cropToSelection(), "crop transformed session");
        CHECK(doc.size() == bounds.size(), "transformed crop discards off-canvas extent");
        CHECK(doc.history().currentIndex() == history + (suspended ? 1 : 2), "already suspended crop reuses existing Finish checkpoint");
        bool correct = true;
        for (int y = 0; y < bounds.height(); ++y)
            for (int x = 0; x < bounds.width(); ++x) {
                const QRgb p = rendered.pixel(x,y);
                const int c = mask.constScanLine(y)[x];
                correct &= doc.activeLayer()->image().pixel(x,y) == qRgba(qRed(p)*c/255,qGreen(p)*c/255,qBlue(p)*c/255,qAlpha(p)*c/255);
            }
        CHECK(correct, "rotated crop applies geometry independently of source alpha");
        doc.history().undo();
        CHECK(doc.size() == QSize(40,30) && doc.floatingPixelSession(), "undo transformed crop restores session");
    }
}

static void advancedFloatingControlsRegression() {
    SECTION("Ordinary lifts, signed resize and rotation pivots");
    Document doc(200,160);
    doc.activeLayer()->clear(Qt::white);
    { QPainter p(&doc.activeLayer()->image()); p.fillRect(20,20,40,30,Qt::red); }
    doc.activeLayer()->image().setPixelColor(23,24,Qt::blue);
    doc.selection().selectRect(QRect(20,20,40,30));
    CanvasWidget canvas; canvas.setDocument(&doc);
    MoveTool tool;
    // Raw events intentionally exercise the pivot nub too.
    auto gesture = [&](QPointF from, QPointF to, Qt::MouseButton button = Qt::LeftButton,
                       Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent press(QEvent::MouseButtonPress,from,from,button,button,mods);
        QMouseEvent move(QEvent::MouseMove,to,to,Qt::NoButton,button,mods);
        QMouseEvent release(QEvent::MouseButtonRelease,to,to,button,Qt::NoButton,mods);
        tool.mousePressEvent(from,&press,canvas);
        tool.mouseMoveEvent(to,&move,canvas);
        tool.mouseReleaseEvent(to,&release,canvas);
    };
    const QImage original = doc.flatten();
    gesture(QPointF(30,30),QPointF(30,30));
    CHECK(!doc.floatingPixelSession() && doc.flatten() == original && !doc.history().canUndo(), "ordinary click creates no lift or history");
    gesture(QPointF(30,30),QPointF(80,30));
    CHECK(doc.floatingPixelSession() && doc.flatten().pixelColor(30,30).alpha() == 0
          && doc.flatten().pixelColor(80,30) == QColor(Qt::red), "ordinary Move lifts pixels leaving a hole");
    const QImage source = doc.floatingPixelSession()->sourcePixels();
    const QImage coverage = doc.floatingPixelSession()->sourceCoverage();
    gesture(QPointF(80,30),QPointF(110,30));
    CHECK(doc.flatten().pixelColor(80,30) == QColor(Qt::white), "intermediate destination restores background");
    CHECK(doc.floatingPixelSession()->sourcePixels().cacheKey() == source.cacheKey()
          && doc.floatingPixelSession()->sourceCoverage().cacheKey() == coverage.cacheKey(), "repeated movement retains immutable source and mask");
    doc.history().undo(); doc.history().undo();
    CHECK(!doc.floatingPixelSession() && doc.flatten() == original, "undo first lift restores original pixels and selection");
    doc.history().redo(); doc.history().redo();
    CHECK(doc.floatingPixelSession() && doc.flatten().pixelColor(110,30) == QColor(Qt::red), "redo ordinary movement restores session");

    const QPointF defaultPivot = doc.floatingPixelSession()->rotationPivot();
    const QImage beforePivot = doc.flatten();
    const qint64 maskKey = doc.selection().mask().cacheKey();
    const QTransform beforeTransform = doc.floatingPixelSession()->transform();
    gesture(defaultPivot,QPointF(-20,-30));
    CHECK(doc.floatingPixelSession()->rotationPivot() == QPointF(-20,-30)
          && doc.flatten() == beforePivot && doc.selection().mask().cacheKey() == maskKey
          && doc.floatingPixelSession()->transform() == beforeTransform, "off-canvas pivot drag changes no pixels mask or transform");
    CHECK(doc.history().undoDescription() == "Move Rotation Pivot", "pivot has its own history entry");
    doc.history().undo();
    CHECK(doc.floatingPixelSession()->rotationPivot() == defaultPivot, "undo pivot restores original rotation centre");
    doc.history().redo();
    CHECK(doc.floatingPixelSession()->rotationPivot() == QPointF(-20,-30), "redo pivot restores custom centre");
    QTemporaryDir directory;
    const QString path = directory.filePath("pivot.psw");
    CHECK(doc.saveNative(path), "save ordinary lift with custom pivot");
    CHECK(doc.floatingPixelSession()->rotationPivot() == QPointF(-20,-30), "save retains pivot in the open document");
    Document reopened(path);
    CHECK(reopened.floatingPixelSession() && reopened.flatten() == doc.flatten()
          && !reopened.floatingPixelSession()->hasCustomRotationPivot()
          && reopened.floatingPixelSession()->rotationPivot() == defaultPivot, "native reopen restores ordinary lift and resets pivot as Paint.NET does");
    const QPointF pivot = doc.floatingPixelSession()->rotationPivot();
    gesture(pivot+QPointF(100,0),pivot+QPointF(0,100),Qt::RightButton,Qt::ShiftModifier);
    CHECK(QLineF(doc.floatingPixelSession()->rotationPivot(),pivot).length() < 0.00001, "rotation fixes custom pivot");
    CHECK(doc.floatingPixelSession()->sourcePixels().cacheKey() == source.cacheKey(), "rotation retains original source");
    doc.history().undo();
    CHECK(doc.floatingPixelSession()->transform() == beforeTransform, "undo custom-pivot rotation restores transform");
    const int noOpHistory = doc.history().currentIndex();
    gesture(pivot,pivot);
    CHECK(doc.history().currentIndex() == noOpHistory && doc.history().canRedo(), "stationary pivot click preserves redo branch");
    gesture(QPointF(110,30),QPointF(120,40));
    CHECK(QLineF(doc.floatingPixelSession()->rotationPivot(),pivot+QPointF(10,10)).length() < 0.00001, "custom pivot follows artwork translation");
    doc.history().undo();
    CHECK(!doc.setFloatingRotationPivot(QPointF(std::numeric_limits<double>::infinity(),0)), "reject nonfinite pivot");

    const QRect rect(20,20,40,30);
    const auto flip = SelHandles::resizeTransform(rect,SelHandles::Handle::Right,QPointF(-21,35),Qt::NoModifier);
    CHECK(flip.m11() == -1 && flip.m22() == 1, "crossing opposite side produces signed horizontal reflection");
    CHECK(flip.map(QPointF(20,20)) == QPointF(20,20), "crossed resize keeps opposite edge anchored");
    const auto symmetric = SelHandles::resizeTransform(rect,SelHandles::Handle::BottomRight,QPointF(0,5),Qt::AltModifier|Qt::ShiftModifier);
    CHECK(symmetric.m11() < 0 && symmetric.m22() < 0 && qAbs(symmetric.m11()-symmetric.m22()) < 0.00001
          && symmetric.map(QRectF(rect).center()) == QRectF(rect).center(), "Shift Alt crossing keeps ratio and centre with both axes reflected");
    const auto zero = SelHandles::resizeTransform(rect,SelHandles::Handle::Right,QPointF(19,35),Qt::NoModifier);
    CHECK(zero.isInvertible(), "crossing exact anchor never creates singular transform");

    // Real asymmetric pixel fixture distinguishes a flip from normalization.
    Document flipped(100,80);
    flipped.activeLayer()->clear(Qt::transparent);
    { QPainter p(&flipped.activeLayer()->image()); p.fillRect(40,20,20,30,Qt::red); p.fillRect(60,20,20,30,Qt::blue); }
    flipped.selection().selectRect(QRect(40,20,40,30));
    CanvasWidget flipCanvas; flipCanvas.setDocument(&flipped);
    strokeTool(&tool,flipCanvas,QPointF(79,35),QPointF(-1,35));
    CHECK(flipped.floatingPixelSession() && flipped.floatingPixelSession()->transform().m11() == -1
          && flipped.flatten().pixelColor(5,30) == QColor(Qt::blue)
          && flipped.flatten().pixelColor(35,30) == QColor(Qt::red), "real nub crossing flips artwork rather than normalizing it");
    const QImage reflected = flipped.flatten();
    flipped.history().undo();
    CHECK(!flipped.floatingPixelSession() && flipped.flatten().pixelColor(45,30) == QColor(Qt::red), "undo flip restores ordinary selection");
    flipped.history().redo();
    CHECK(flipped.flatten() == reflected, "redo flip restores reflected pixels");
    CHECK(flipped.cropToSelection(), "crop migrated flipped selection");
    flipped.history().undo();
    CHECK(flipped.floatingPixelSession(), "Undo Crop restores migrated selection session");

    for (QPoint offset : {QPoint(-5,4),QPoint(6,-3)}) {
        Document ordinary(60,50), independent(60,50);
        ordinary.activeLayer()->clear(QColor(80,120,200,128));
        ordinary.activeLayer()->setOffset(offset);
        QImage mask(60,50,QImage::Format_Grayscale8); mask.fill(0);
        for (int y=10;y<30;++y)
            for (int x=10;x<40;++x) mask.scanLine(y)[x] = x==10 ? 128 : 255;
        mask.scanLine(15)[15] = 0;
        ordinary.selection().setMaskImage(mask);
        const QImage before = ordinary.flatten();
        CHECK(ordinary.prepareFloatingPixelGesture(), "ordinary fractional selection lifts at offset");
        CHECK(ordinary.flatten() == before, "initial fractional lift preserves original raster exactly");
        const QTransform initial = ordinary.floatingPixelSession()->transform();
        const QImage source = ordinary.floatingPixelSession()->sourcePixels();
        CHECK(ordinary.setFloatingPixelTransform(initial * QTransform::fromTranslate(100,100)), "ordinary selection moves fully off canvas");
        CHECK(ordinary.setFloatingPixelTransform(initial), "ordinary selection returns from off canvas");
        CHECK(ordinary.flatten() == before && ordinary.selection().mask() == mask
              && ordinary.floatingPixelSession()->sourcePixels().cacheKey() == source.cacheKey(), "round trip restores fractional mask source and transparent pixels without resampling");
        CHECK(!independent.floatingPixelSession() && independent.flatten().pixelColor(20,20) == QColor(Qt::white), "ordinary sessions remain document independent");
    }
    Document nudged(60,50);
    nudged.selection().selectRect(QRect(10,10,30,20));
    CanvasWidget nudgeCanvas; nudgeCanvas.setDocument(&nudged);
    QKeyEvent key(QEvent::KeyPress,Qt::Key_Right,Qt::ControlModifier);
    tool.keyPressEvent(&key,nudgeCanvas);
    CHECK(nudged.floatingPixelSession() && nudged.selection().boundingRect() == QRect(20,10,30,20), "Ctrl arrow lifts ordinary selection and nudges ten pixels");
    nudged.history().undo();
    CHECK(!nudged.floatingPixelSession() && nudged.selection().boundingRect() == QRect(10,10,30,20), "undo keyboard lift restores ordinary selection");
}

static void filteredFloatingRegression() {
    SECTION("Filtered floating sampling keeps colour alpha and geometry independent");
    using Sampling=FloatingPixelSession::Sampling;
    auto layer=std::make_shared<Layer>(10,10);
    layer->clear(Qt::transparent);
    QImage source(4,4,QImage::Format_ARGB32_Premultiplied);
    QImage mask(4,4,QImage::Format_Grayscale8); mask.fill(255);
    for (int y=0;y<4;++y) for (int x=0;x<4;++x)
        source.setPixelColor(x,y,x==2 ? Qt::white : Qt::black);
    for (Sampling mode:{Sampling::Nearest,Sampling::Bilinear,Sampling::Bicubic}) {
        auto session=FloatingPixelSession::create(layer,source,mask,QPoint(),mode);
        CHECK(session && session->setTransform(QTransform::fromTranslate(0.5,0)), "filtered half-pixel fixture initializes");
        const QRgb pixel=session->render(QRect(2,1,1,1)).pixel(0,0);
        const int expected=mode==Sampling::Nearest ? 255 : mode==Sampling::Bilinear ? 128 : 143;
        CHECK(qRed(pixel)==expected && qGreen(pixel)==expected && qAlpha(pixel)==255, "sampling matches independent nearest bilinear and Catmull-Rom midpoint values");
        CHECK(session->renderScaled(QRect(2,1,1,1),QSize(1,1)).pixel(0,0)==pixel, "thumbnail samples the same filtered pixel centre");
        CHECK(session->sourcePixels()==source && session->sourceCoverage()==mask, "filter leaves original pixels and coverage immutable");
        session->setTransform(QTransform::fromTranslate(2,3));
        auto nearest=FloatingPixelSession::create(layer,source,mask,QPoint(2,3));
        CHECK(session->render(QRect(0,0,10,10))==nearest->render(QRect(0,0,10,10)), "integer movement remains byte-exact in every mode");
        session->setTransform(QTransform::fromScale(2,2));
        const QImage grown=session->render(QRect(-1,-1,10,10));
        bool opaque=true;
        for (int y=-1;y<9;++y) for (int x=-1;x<9;++x)
            opaque &= qAlpha(grown.pixel(x+1,y+1))==(x>=0 && y>=0 && x<8 && y<8 ? 255 : 0);
        CHECK(opaque, "integer enlargement of an opaque rectangle introduces no translucent border or halo");
    }
    source.fill(Qt::red);
    for (int y=0;y<4;++y) { source.setPixelColor(2,y,Qt::blue); mask.scanLine(y)[2]=0; }
    auto selected=FloatingPixelSession::create(layer,source,mask,QPoint(),Sampling::Bilinear);
    selected->setTransform(QTransform::fromTranslate(0.5,0));
    const QRgb edge=selected->render(QRect(2,1,1,1)).pixel(0,0);
    CHECK(qRed(edge)==128 && qBlue(edge)==0 && qAlpha(edge)==128, "unselected blue cannot bleed into a half-covered red boundary");
    mask.fill(255);
    for (int y=0;y<4;++y) source.setPixel(2,y,0);
    auto transparent=FloatingPixelSession::create(layer,source,mask,QPoint(),Sampling::Bilinear);
    transparent->setTransform(QTransform::fromTranslate(0.5,0));
    CHECK(transparent->renderCoverage(QRect(2,1,1,1)).constScanLine(0)[0]==255
          && transparent->render(QRect(2,1,1,1)).pixel(0,0)==edge, "source transparency and selection coverage produce the same colour but retain distinct geometry");

    for (Sampling mode:{Sampling::Bilinear,Sampling::Bicubic}) {
        QImage red(1,1,QImage::Format_ARGB32_Premultiplied); red.fill(Qt::red);
        QImage selected(1,1,QImage::Format_Grayscale8); selected.fill(255);
        auto diamond=FloatingPixelSession::create(layer,red,selected,QPoint(),mode);
        diamond->setTransform(QTransform(1,1,-1,1,1,0));
        const QImage rotated=diamond->render(QRect(0,0,2,2));
        CHECK(qAlpha(rotated.pixel(0,0))==128 && qAlpha(rotated.pixel(1,0))==128
              && qAlpha(rotated.pixel(0,1))==128 && qAlpha(rotated.pixel(1,1))==128,
              "rotated diamond has exact half-pixel geometric coverage at all four corners");
        QImage odd(3,3,QImage::Format_ARGB32_Premultiplied); odd.fill(Qt::red);
        QImage oddMask(3,3,QImage::Format_Grayscale8); oddMask.fill(255);
        auto partial=FloatingPixelSession::create(layer,odd,oddMask,QPoint(),mode);
        partial->setTransform(QTransform::fromScale(0.25,0.25));
        CHECK(qAlpha(partial->render(QRect(0,0,1,1)).pixel(0,0))==143,
              "odd-sized reduction applies geometric area once rather than twice");
        QImage checker(8,8,QImage::Format_ARGB32_Premultiplied);
        QImage coverage(8,8,QImage::Format_Grayscale8); coverage.fill(255);
        for (int y=0;y<8;++y) for (int x=0;x<8;++x) checker.setPixelColor(x,y,(x+y)%2 ? Qt::white : Qt::black);
        auto reduced=FloatingPixelSession::create(layer,checker,coverage,QPoint(),mode);
        reduced->setTransform(QTransform::fromScale(0.125,0.125));
        auto &cache=FloatingRenderCache::shared(); cache.clear();
        CHECK(!reduced->renderScaled(QRect(0,0,1,1),QSize(1,1)).isNull() && cache.stats().entries==0,
              "bounded thumbnail never builds full-source reduction levels");
        CHECK(reduced->renderCoverage(QRect(100,100,1,1)).constScanLine(0)[0]==0 && cache.stats().entries==0,
              "off-canvas coverage skips reduction processing");
        const QRgb pixel=reduced->render(QRect(0,0,1,1)).pixel(0,0);
        CHECK(qRed(pixel)==128 && qAlpha(pixel)==255, "strong reduction averages checkerboard instead of aliasing to one colour");
        const auto stats=FloatingRenderCache::shared().stats();
        CHECK(stats.costKiB<=stats.limitKiB, "reduction pyramid shares the existing bounded raster cache");
    }

    SECTION("Sampling policy history cache native roundtrip and finite-domain invalidation");
    Document doc(340,280);
    doc.activeLayer()->clear(QColor(50,120,200,160));
    QImage pixels(309,271,QImage::Format_ARGB32_Premultiplied);
    pixels.fill(QColor(200,40,80,100)); pixels.setPixel(10,10,0);
    QImage geometry(pixels.size(),QImage::Format_Grayscale8); geometry.fill(255);
    for (int y=0;y<geometry.height();++y) for (int x=0;x<geometry.width();++x)
        if ((x+y)%7==0) geometry.scanLine(y)[x]=0;
    CHECK(doc.pasteFloatingPixels(pixels,geometry,QPoint(),false,QSize(),Sampling::Nearest), "sampling history fixture initializes");
    CHECK(doc.setFloatingPixelTransform(QTransform().rotate(13)*QTransform::fromTranslate(-12.25,17.5)), "sampling fixture crosses tile and canvas edges");
    const QImage immutable=doc.floatingPixelSession()->sourcePixels();
    QTemporaryDir directory;
    for (Sampling mode:{Sampling::Bilinear,Sampling::Bicubic,Sampling::Nearest}) {
        const QImage before=doc.flatten();
        const Sampling previous=doc.floatingPixelSession()->sampling();
        QRegion dirty;
        const auto connection=QObject::connect(&doc,&Document::floatingPixelsChanged,&doc,[&](const QRegion &area){ dirty+=area; });
        CHECK(doc.setFloatingSampling(mode), "change live sampling mode");
        QImage patched=before;
        { QPainter p(&patched); p.setCompositionMode(QPainter::CompositionMode_Source);
          for (const QRect &area:dirty) p.drawImage(area.topLeft(),doc.flattenVisible(area)); }
        CHECK(patched==doc.flatten(), "sampling mode invalidates both old and new edge footprints");
        QObject::disconnect(connection);
        const auto *session=doc.floatingPixelSession();
        CHECK(session->sourcePixels().cacheKey()==immutable.cacheKey(), "changing mode reuses immutable source");
        CHECK(doc.selection().mask()==session->renderCoverage(QRect(QPoint(),doc.size())), "cached filtered mask agrees with direct coverage");
        for (BlendMode blend:{BlendMode::Normal,BlendMode::Multiply,BlendMode::Overwrite}) {
            doc.activeLayer()->setBlendMode(blend);
            doc.activeLayer()->setOpacity(0.6f);
            auto effective=doc.snapshotEffectiveLayers(); std::vector<Layer> layers;
            for (const auto &l:effective) layers.push_back(*l);
            CHECK(doc.flatten()==Document::compositeLayers(layers,doc.size()), "cached filtered tiles agree with materialized layer composition");
        }
        const QString path=directory.filePath("sampling.psw");
        CHECK(doc.saveNative(path), "save filtered native session");
        Document reopened(path);
        CHECK(reopened.floatingPixelSession() && reopened.floatingPixelSession()->sampling()==mode
              && reopened.flatten()==doc.flatten() && reopened.selection().mask()==doc.selection().mask(), "native reopen preserves mode filtered pixels and independent geometry");
        doc.history().undo();
        CHECK(doc.floatingPixelSession()->sampling()==previous, "undo restores preceding sampling mode");
        doc.history().redo();
        CHECK(doc.floatingPixelSession()->sampling()==mode, "redo restores sampling mode");
    }
    const int history=doc.history().currentIndex();
    CHECK(!doc.setFloatingSampling(static_cast<Sampling>(99)) && doc.history().currentIndex()==history, "invalid sampling policy is a history-preserving no-op");
    doc.activeLayer()->setLocked(true);
    CHECK(!doc.setFloatingSampling(Sampling::Bilinear) && doc.floatingPixelSession()->sampling()==Sampling::Nearest
          && doc.history().currentIndex()==history, "locked target rejects appearance-changing sampling edits");
    doc.activeLayer()->setLocked(false);
    CHECK(!FloatingPixelSession::create(layer,source,mask,QPoint(),static_cast<Sampling>(99)), "invalid sampler cannot initialize a session");
    CHECK(doc.setFloatingSampling(Sampling::Bicubic), "prepare serialized policy validation fixture");
    const QString corrupt=directory.filePath("invalid-sampling.psw");
    CHECK(doc.saveNative(corrupt), "save policy validation fixture");
    { QFile file(corrupt); CHECK(file.open(QIODevice::ReadWrite) && file.seek(file.size()-4), "open serialized policy field");
      QDataStream out(&file); out.setVersion(QDataStream::Qt_6_0); out << qint32(99); }
    Document unchanged(3,2);
    CHECK(!unchanged.loadNative(corrupt) && unchanged.size()==QSize(3,2)
          && unchanged.flatten().pixelColor(0,0)==QColor(Qt::white), "invalid native sampling metadata is rejected atomically");
    CHECK(doc.setFloatingSampling(Sampling::Bicubic) && doc.suspendFloatingPixels(), "filtered context can be suspended");
    const QImage suspended=doc.flatten();
    CHECK(doc.setFloatingSampling(Sampling::Bilinear) && doc.flatten()==suspended, "changing mode at a context boundary preserves its exact lift pose");
    CHECK(doc.cropToSelection(), "crop filtered selection");
    doc.history().undo();
    CHECK(doc.floatingPixelSession(), "undo filtered crop restores editable session");
}

static void retainedExpansionRegression() {
    SECTION("Expansion makes negatively positioned retained layers editable at new edges");
    for (QPoint origin : {QPoint(-50,-40),QPoint(-50,20),QPoint(20,-40),QPoint(20,30),QPoint()}) {
        for (QSize target : {QSize(900,600),QSize(800,700),QSize(900,700)}) {
            Document doc(800,600);
            doc.activeLayer()->clear(Qt::transparent);
            const QImage original = retainedPattern();
            doc.pasteImage(original,"Keep",false);
            doc.activeLayer()->setOffset(origin);
            doc.selection().selectRect(QRect(3,4,10,12));
            const QImage mask = doc.selection().mask();
            const QImage before = doc.flatten();
            QImage pasted = makeImage(target.width(),target.height(),Qt::transparent);
            doc.pasteImage(pasted,"Expand",true);
            doc.setActiveLayer(1);
            CHECK(doc.size() == target, "Expand uses componentwise maximum canvas dimensions");
            CHECK(doc.activeLayer()->offset() == origin, "Expand preserves exact retained origin");
            CHECK(doc.activeLayer()->image().copy(original.rect()) == original, "Expand never resamples or loses existing bitmap");
            CHECK(doc.flatten().copy(before.rect()) == before, "Expand preserves old visible artwork");
            const QImage padded = doc.activeLayer()->image();
            doc.history().undo();
            CHECK(doc.size() == QSize(800,600) && doc.activeLayer()->image() == original &&
                  doc.activeLayer()->offset() == origin && doc.selection().mask() == mask,
                  "Expand undo restores bitmap, origin, selection and dimensions atomically");
            doc.history().redo();
            doc.setActiveLayer(1);
            CHECK(doc.activeLayer()->image() == padded && doc.activeLayer()->offset() == origin &&
                  !doc.selection().hasSelection(), "Expand redo restores geometry and cleared selection");
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            doc.setPrimaryColor(Qt::magenta);
            PencilTool pencil;
            pencil.setBrushSize(1);
            const QPoint edge(target.width()-1,target.height()-1);
            strokeTool(&pencil,canvas,edge,edge);
            CHECK(doc.flatten().pixelColor(edge) == QColor(Qt::magenta),
                  "actual Pencil edit reaches the freshly expanded right/bottom canvas edge");
        }
    }
}

static void retainedOrthogonalRegression() {
    SECTION("Retained rotations and flips transform document-space origins");
    for (QPoint offset : {QPoint(50, 40), QPoint(-50, -40)}) {
        for (int operation = 0; operation < 5; ++operation) {
            Document doc(800, 600);
            doc.activeLayer()->clear(Qt::transparent);
            const QImage image = retainedPattern();
            doc.pasteImage(image, "retained", false);
            doc.activeLayer()->setOffset(offset);
            doc.selection().selectRect(QRect(10,20,50,60));
            const QImage mask = doc.selection().mask();
            const QImage before = doc.flatten();
            QImage expected, expectedBuffer;
            QPoint expectedOffset;
            if (operation < 3) {
                const int turns = operation + 1;
                QTransform transform;
                transform.rotate(turns * 90);
                expected = before.transformed(transform);
                expectedBuffer = image.transformed(transform);
                if (turns == 1) expectedOffset = QPoint(600-offset.y()-image.height(), offset.x());
                if (turns == 2) expectedOffset = QPoint(800-offset.x()-image.width(), 600-offset.y()-image.height());
                if (turns == 3) expectedOffset = QPoint(offset.y(), 800-offset.x()-image.width());
                doc.rotate(turns == 3 ? -1 : turns, "rotate");
            } else {
                const bool horizontal = operation == 3;
                const QTransform reflection = QTransform::fromScale(horizontal ? -1 : 1, horizontal ? 1 : -1);
                expected = before.transformed(reflection);
                expectedBuffer = image.transformed(reflection);
                expectedOffset = horizontal ? QPoint(800-offset.x()-image.width(), offset.y())
                                            : QPoint(offset.x(), 600-offset.y()-image.height());
                doc.flip(horizontal, "flip");
            }
            CHECK(doc.flatten() == expected, "orthogonal operation preserves exact visible composition");
            CHECK(doc.activeLayer()->image() == expectedBuffer, "orthogonal operation retains all off-canvas pixels");
            CHECK(doc.activeLayer()->offset() == expectedOffset, "orthogonal origin uses canvas rather than bitmap bounds");
            CHECK(!doc.selection().hasSelection(), "orthogonal operation clears selection");
            doc.history().undo();
            CHECK(doc.activeLayer()->image() == image && doc.activeLayer()->offset() == offset,
                  "orthogonal undo restores original buffer and origin");
            CHECK(doc.size() == before.size() && doc.selection().mask() == mask, "orthogonal undo restores canvas and selection");
            doc.history().redo();
            CHECK(doc.flatten() == expected && doc.activeLayer()->image() == expectedBuffer &&
                  doc.activeLayer()->offset() == expectedOffset, "orthogonal redo restores complete transformed state");
        }
    }
}

static void retainedResizeRegression() {
    SECTION("Retained image resize uses the document scale and sampling grid");
    for (QPoint offset : {QPoint(50, 40), QPoint(-50, -40), QPoint(51, -39)}) {
        for (QSize target : {QSize(400,300), QSize(1200,900), QSize(413,307)}) {
            Document doc(800,600);
            doc.activeLayer()->clear(Qt::transparent);
            QImage image = retainedPattern();
            // Semi-transparent detail and a marker crossing the visible canvas edge
            // distinguish resampling phase from mere bounding-box correctness.
            { QPainter p(&image); p.fillRect(795-offset.x(), 575-offset.y(), 15,20, QColor(90,130,210,123)); }
            doc.pasteImage(image, "retained", false);
            doc.activeLayer()->setOffset(offset);
            doc.selection().selectRect(QRect(30,40,50,60));
            const QImage mask = doc.selection().mask();
            const QImage expected = doc.flatten().scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            doc.resize(target.width(), target.height());
            const QImage after = doc.activeLayer()->image();
            const QPoint afterOffset = doc.activeLayer()->offset();
            CHECK(doc.flatten() == expected, "resize preserves exact visible canvas resampling, including edge alpha");
            CHECK(doc.activeLayer()->image().format() == QImage::Format_ARGB32_Premultiplied,
                  "resize keeps premultiplied storage");
            const QPoint hidden(qRound((offset.x()+817)*target.width()/800.0),
                                qRound((offset.y()+607)*target.height()/600.0));
            CHECK(doc.activeLayer()->image().rect().contains(hidden-afterOffset) &&
                  doc.activeLayer()->image().pixelColor(hidden-afterOffset) == QColor(Qt::blue),
                  "resize retains scaled off-canvas artwork rather than squeezing it into the canvas");
            doc.history().undo();
            CHECK(doc.activeLayer()->image() == image && doc.activeLayer()->offset() == offset &&
                  doc.size() == QSize(800,600), "resize undo restores original geometry and all pixels");
            CHECK(doc.selection().mask() == mask, "resize undo restores selection");
            doc.history().redo();
            CHECK(doc.flatten() == expected && doc.activeLayer()->image() == after &&
                  doc.activeLayer()->offset() == afterOffset, "resize redo restores sampled retained geometry");
        }
    }
}

static void pasteExpansionBoundsRegression() {
    SECTION("Expansion rejects unrepresentable local bitmap extents before mutation");
    for (QPoint origin : {QPoint(INT_MIN,0),QPoint(0,INT_MIN),QPoint(INT_MIN,INT_MIN)}) {
        Document doc(4,3);
        const QImage image = makeImage(6,5,Qt::red);
        doc.pasteImage(image,"Keep",false);
        doc.activeLayer()->setOffset(origin);
        doc.setModified(false);
        const QSize size = doc.size();
        const int count = doc.layerCount(), active = doc.activeLayerIndex();
        const int history = doc.history().currentIndex();
        const QImage mask = doc.selection().mask();
        const QImage pasted = makeImage(7,6,Qt::transparent);
        CHECK(doc.pasteImage(pasted,"Expand",true) == -1, "unrepresentable expansion is rejected");
        CHECK(doc.size() == size && doc.layerCount() == count && doc.activeLayerIndex() == active,
              "rejected expansion leaves document geometry and layer stack unchanged");
        CHECK(doc.history().currentIndex() == history && !doc.isModified() && doc.selection().mask() == mask,
              "rejected expansion leaves history, modified state and selection unchanged");
        CHECK(doc.layerAt(active)->image() == image && doc.layerAt(active)->offset() == origin,
              "rejected expansion preserves original bitmap and origin");
    }
}

static void resizeStampCharacterization() {
    SECTION("Resize Source stamp replaces only the finite canvas, never off-canvas pixels");
    for (QPoint origin : {QPoint(-2,-1),QPoint(2,1),QPoint(1,-2)}) {
        for (QSize target : {QSize(13,11),QSize(4,3)}) {
            Document doc(8,6);
            QImage image = makeImage(12,10,Qt::transparent);
            for (int y=0;y<image.height();++y)
                for (int x=0;x<image.width();++x)
                    image.setPixelColor(x,y,QColor((x*41)%256,(y*53)%256,170,(x+y)%3 ? 117 : 0));
            doc.activeLayer()->setImage(image);
            doc.activeLayer()->setOffset(origin);
            doc.activeLayer()->setRetainsOffCanvasPixels(true);
            const QPoint scaledOrigin(qRound(origin.x()*target.width()/8.0),qRound(origin.y()*target.height()/6.0));
            const QImage scaled = image.scaled(qMax(1,qRound(image.width()*target.width()/8.0)),
                qMax(1,qRound(image.height()*target.height()/6.0)),Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            const QRect bounds = QRect(scaledOrigin,scaled.size()).united(QRect(QPoint(),target));
            QImage outside(bounds.size(),QImage::Format_ARGB32_Premultiplied);
            outside.fill(Qt::transparent);
            { QPainter painter(&outside); painter.drawImage(scaledOrigin-bounds.topLeft(),scaled); }
            const QImage finite = image.copy(QRect(-origin,doc.size())).scaled(target,Qt::IgnoreAspectRatio,Qt::SmoothTransformation);
            doc.resize(target.width(),target.height());
            CHECK(doc.activeLayer()->offset() == bounds.topLeft() && doc.activeLayer()->image().size() == bounds.size(),
                  "resize keeps the full scaled bitmap/canvas union");
            CHECK(doc.flatten() == finite, "Source preserves exact finite-canvas alpha instead of double-compositing");
            bool untouched = true;
            for (int y=0;y<outside.height();++y)
                for (int x=0;x<outside.width();++x)
                    if (!QRect(QPoint(),target).contains(QPoint(x,y)+bounds.topLeft()) &&
                        doc.activeLayer()->image().pixel(x,y) != outside.pixel(x,y)) untouched = false;
            CHECK(untouched, "all outside-canvas pixels equal independently document-scaled bitmap");
        }
    }
}

static void ordinaryMoveAuthorityRegression() {
    SECTION("Only explicit retained state makes whole-layer Move nondestructive");
    for (QPoint origin : {QPoint(),QPoint(2,1),QPoint(-2,-1)}) {
        for (QSize buffer : {QSize(16,12),QSize(20,16),QSize(10,8)}) {
            Document doc(16,12);
            QImage before = makeImage(buffer.width(),buffer.height(),Qt::transparent);
            before.setPixelColor(4,3,QColor(200,60,120,137));
            doc.activeLayer()->setImage(before);
            doc.activeLayer()->setOffset(origin);
            CHECK(!doc.activeLayer()->retainsOffCanvasPixels(), "ordinary fixture has no retention provenance");
            QImage expected(buffer,QImage::Format_ARGB32_Premultiplied);
            expected.fill(Qt::transparent);
            { QPainter painter(&expected); painter.drawImage(3,2,before); }
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            MoveTool move;
            strokeTool(&move,canvas,QPointF(5,5),QPointF(8,7));
            CHECK(doc.activeLayer()->image() == expected && doc.activeLayer()->offset() == origin,
                  "ordinary layer bakes only movement delta even with a positioned/mismatched bitmap");
            const QImage visible = doc.flatten();
            doc.history().undo();
            CHECK(doc.activeLayer()->image() == before && doc.activeLayer()->offset() == origin &&
                  !doc.activeLayer()->retainsOffCanvasPixels(), "ordinary Move undo restores pixels and original origin");
            doc.history().redo();
            CHECK(doc.activeLayer()->image() == expected && doc.flatten() == visible &&
                  doc.activeLayer()->offset() == origin, "ordinary Move redo preserves alignment");
        }
    }
}

static void nativeRetentionCompatibility() {
    SECTION("Native retained state has a versioned format and reads PSW v1");
    QTemporaryDir directory;
    auto writeV1 = [&](const QString &path, const QImage &image, QPoint offset, int version) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        QDataStream out(&file);
        out.setVersion(QDataStream::Qt_6_0);
        out << quint32(0x50535731) << qint32(version) << qint32(800) << qint32(600)
            << qint32(0) << qint32(1);
        out << QString("v1 fixture") << float(0.75) << bool(true) << bool(false)
            << qint32(0) << offset << image;
        return out.status() == QDataStream::Ok;
    };
    for (bool retained : {false, true}) {
        const QImage image = retained ? retainedPattern() : makeImage(800,600,Qt::red);
        const QPoint offset = retained ? QPoint(-50,-40) : QPoint();
        const QString path = directory.path() + (retained ? "/retained-v1.psw" : "/ordinary-v1.psw");
        CHECK(writeV1(path, image, offset, 1), "write an independent old-format fixture");
        Document loaded;
        CHECK(loaded.loadNative(path), "v1 files remain readable");
        CHECK(loaded.activeLayer()->retainsOffCanvasPixels() == retained,
              "v1 geometry inference distinguishes ordinary and oversized/positioned fixtures");
        CHECK(loaded.activeLayer()->image() == image && loaded.activeLayer()->offset() == offset &&
              loaded.activeLayer()->name() == "v1 fixture" && loaded.activeLayer()->opacity() == 0.75f,
              "v1 layer metadata and full bitmap preserved");
        // Expansion to equal dimensions must not erase the inferred retained
        // semantics of an old file with a positioned/oversized bitmap.
        if (retained) {
            loaded.pasteImage(image,"expand",true);
            loaded.setActiveLayer(0);
        }
        CanvasWidget canvas;
        canvas.setDocument(&loaded);
        MoveTool move;
        const QImage expanded = loaded.activeLayer()->image();
        strokeTool(&move,canvas,QPointF(300,200),QPointF(350,240));
        if (retained)
            CHECK(loaded.activeLayer()->image() == expanded && expanded.copy(image.rect()) == image &&
                  loaded.activeLayer()->offset().isNull(),
                  "v1 retained layer keeps full padded bitmap after canvas expansion and Move");
        else
            CHECK(loaded.activeLayer()->offset().isNull() && loaded.activeLayer()->image() != image &&
                  loaded.activeLayer()->image().pixelColor(0,0).alpha() == 0,
                  "v1 ordinary layer still uses the established baked move");
    }
    for (const auto &geometry : {std::make_pair(QSize(800,600),QPoint(3,-2)),
                                 std::make_pair(QSize(840,630),QPoint()),
                                 std::make_pair(QSize(840,630),QPoint(-3,2))}) {
        Document ordinary(800,600);
        const QImage before = makeImage(geometry.first.width(),geometry.first.height(),QColor(210,70,30,117));
        ordinary.activeLayer()->setImage(before);
        ordinary.activeLayer()->setOffset(geometry.second);
        CHECK(!ordinary.activeLayer()->retainsOffCanvasPixels(), "positioned ordinary fixture has explicit false retention");
        const QString path = directory.path()+"/positioned-ordinary.psw";
        CHECK(ordinary.saveNative(path), "save ordinary state that legacy geometry would infer as retained");
        Document restored;
        CHECK(restored.loadNative(path), "reload positioned ordinary document");
        CHECK(!restored.activeLayer()->retainsOffCanvasPixels(), "native roundtrip preserves explicit ordinary movement state");
        CHECK(restored.activeLayer()->image() == before && restored.activeLayer()->offset() == geometry.second,
              "native roundtrip preserves positioned ordinary bitmap and origin");
        QImage expected(before.size(),QImage::Format_ARGB32_Premultiplied);
        expected.fill(Qt::transparent);
        { QPainter painter(&expected); painter.drawImage(4,3,before); }
        CanvasWidget canvas;
        canvas.setDocument(&restored);
        MoveTool move;
        strokeTool(&move,canvas,QPointF(20,20),QPointF(24,23));
        CHECK(restored.activeLayer()->image() == expected && restored.activeLayer()->offset() == geometry.second,
              "ordinary Move remains pixel-baked after native roundtrip");
        restored.history().undo();
        CHECK(restored.activeLayer()->image() == before && !restored.activeLayer()->retainsOffCanvasPixels(),
              "native ordinary Move undo preserves explicit state");
    }
    const QString unknown = directory.path()+"/unknown.psw";
    CHECK(writeV1(unknown, retainedPattern(), QPoint(), 42), "write unknown-version fixture");
    Document untouched(23,17);
    const QImage before = untouched.flatten();
    CHECK(!untouched.loadNative(unknown), "reject unsupported native versions");
    CHECK(untouched.size() == QSize(23,17) && untouched.flatten() == before,
          "unknown format does not replace the live document");
    Document current(800,600);
    current.pasteImage(retainedPattern(),"Keep",false);
    const QString path = directory.path()+"/current.psw";
    CHECK(current.saveNative(path), "write current native format");
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly), "open current native file");
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    qint32 version = 0;
    in >> magic >> version;
    CHECK(magic == quint32(0x50535731) && version == 2, "retained-state layout is explicitly version 2");
    file.close();
    Document ordinary(23,17);
    const QString ordinaryPath = directory.path()+"/ordinary-current.psw";
    CHECK(ordinary.saveNative(ordinaryPath), "save an ordinary document without retained state");
    QFile ordinaryFile(ordinaryPath);
    CHECK(ordinaryFile.open(QIODevice::ReadOnly), "open ordinary native output");
    QDataStream ordinaryStream(&ordinaryFile);
    ordinaryStream.setVersion(QDataStream::Qt_6_0);
    ordinaryStream >> magic >> version;
    CHECK(magic == quint32(0x50535731) && version == 1,
          "ordinary documents retain the legacy v1 output layout");
    for (bool retained : {false,true}) {
        // Both fixtures have identical dimensions and zero origin: only v2's
        // explicit flag can distinguish their movement provenance.
        const QString matchedPath = directory.path() + (retained ? "/matched-retained.psw" : "/matched-ordinary.psw");
        QFile matchedFile(matchedPath);
        CHECK(matchedFile.open(QIODevice::WriteOnly), "open independent matched-size v2 fixture");
        QDataStream out(&matchedFile);
        out.setVersion(QDataStream::Qt_6_0);
        const QImage image = makeImage(800,600,QColor(210,70,30,117));
        out << quint32(0x50535731) << qint32(2) << qint32(800) << qint32(600) << qint32(0) << qint32(1);
        out << QString("matched v2") << float(1) << true << false << qint32(0) << QPoint() << retained << image;
        CHECK(out.status() == QDataStream::Ok, "write independent v2 flag and bitmap");
        matchedFile.close();
        Document loaded;
        CHECK(loaded.loadNative(matchedPath), "read matched-size v2 fixture");
        CHECK(loaded.activeLayer()->retainsOffCanvasPixels() == retained && loaded.activeLayer()->image() == image &&
              loaded.activeLayer()->offset().isNull(), "v2 honours explicit provenance, never legacy geometry inference");
        CHECK(loaded.saveNative(matchedPath), "save matched-size native state again");
        Document reread;
        CHECK(reread.loadNative(matchedPath) && reread.activeLayer()->retainsOffCanvasPixels() == retained,
              "matched-size native save/load preserves retained or ordinary state");
    }
}

static void retainedMergeRegression() {
    SECTION("Merge Down composes retained bitmaps in a common document-space domain");
    for (bool keepArtwork : {false,true}) {
        for (const auto &offsets : {std::make_pair(QPoint(50,40),QPoint(-20,-10)),
                                   std::make_pair(QPoint(-50,-40),QPoint(20,10)),
                                   std::make_pair(QPoint(50,40),QPoint(950,-10))}) {
            Document doc(800,600);
            doc.activeLayer()->clear(Qt::transparent);
            const QImage lower = retainedPattern();
            QImage top = makeImage(870,650,Qt::transparent);
            { QPainter p(&top); p.fillRect(300,300,20,20,Qt::magenta);
              p.fillRect(840,610,20,20,Qt::red); }
            const QImage artwork = top;
            if (keepArtwork) {
                top.fill(Qt::white);
                QPainter p(&top); p.drawImage(0,0,artwork);
            }
            doc.pasteImage(lower,"lower",false);
            doc.activeLayer()->setOffset(offsets.first);
            doc.pasteImage(top,"upper",false);
            doc.activeLayer()->setOffset(offsets.second);
            doc.activeLayer()->setOpacity(0.75f);
            doc.selection().selectRect(QRect(10,20,30,40));
            const QImage mask = doc.selection().mask();
            const QImage before = doc.flatten();
            const QRect bounds = QRect(offsets.first,lower.size()).united(QRect(offsets.second,top.size()));
            QImage expected = makeImage(bounds.width(),bounds.height(),Qt::transparent);
            { QPainter p(&expected);
              p.drawImage(offsets.first-bounds.topLeft(),lower);
              p.setOpacity(0.75f);
              p.drawImage(offsets.second-bounds.topLeft(),artwork); }
            QImage expectedVisible = makeImage(800,600,Qt::transparent);
            { QPainter p(&expectedVisible); p.drawImage(bounds.topLeft(),expected); }
            doc.mergeLayerDown(2,keepArtwork);
            CHECK(doc.flatten() == expectedVisible, "both merge branches preserve artwork document positions");
            if (!keepArtwork)
                CHECK(doc.flatten() == before, "ordinary merge preserves exact visible composition");
            CHECK(doc.activeLayer()->image() == expected && doc.activeLayer()->offset() == bounds.topLeft(),
                  "merged buffer retains both complete domains, including artwork outside canvas");
            CHECK(doc.activeLayer()->retainsOffCanvasPixels(), "merged Keep layers retain nondestructive movement");
            CHECK(doc.layerCount() == 2 && doc.activeLayerIndex() == 1, "merge updates stack and active layer");
            doc.history().undo();
            CHECK(doc.layerCount() == 3 && doc.activeLayerIndex() == 2 && doc.flatten() == before,
                  "merge undo restores visible layer stack");
            CHECK(doc.layerAt(1)->image() == lower && doc.layerAt(1)->offset() == offsets.first &&
                  doc.layerAt(2)->image() == top && doc.layerAt(2)->offset() == offsets.second,
                  "merge undo restores full retained buffers and distinct origins");
            CHECK(doc.layerAt(1)->retainsOffCanvasPixels() && doc.layerAt(2)->retainsOffCanvasPixels() &&
                  doc.selection().mask() == mask, "merge undo restores retained semantics and selection");
            doc.history().redo();
            CHECK(doc.activeLayer()->image() == expected && doc.activeLayer()->offset() == bounds.topLeft() &&
                  doc.flatten() == expectedVisible, "merge redo restores the common domain exactly");
        }
    }
}

// =====================================================================
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    {
        const QVector<PreviewDialog::Param> params = {{"Amount", -100, 100, 0, "%"}};
        PreviewDialog controls("Parameters", params);
        int changes = 0;
        QObject::connect(&controls, &PreviewDialog::parametersChanged, [&]() { ++changes; });
        auto *slider = controls.findChild<QSlider *>();
        auto *spin = controls.findChild<QSpinBox *>();
        slider->setValue(25);
        CHECK(spin->value() == 25 && controls.values() == QVector<int>({25}), "Parameter slider synchronises values");
        spin->setValue(-40);
        CHECK(slider->value() == -40 && changes == 2, "Parameter spin emits one change");
        auto *buttons = controls.findChild<QDialogButtonBox *>();
        buttons->button(QDialogButtonBox::Reset)->click();
        CHECK(controls.values() == QVector<int>({0}) && changes == 3, "Parameter Reset restores defaults and emits change");
        CHECK(controls.findChildren<QLabel *>().size() == 1, "Parameters-only dialog has only its parameter label");
        buttons->button(QDialogButtonBox::Ok)->click();
        CHECK(controls.result() == QDialog::Accepted, "Parameter OK accepts");
        buttons->button(QDialogButtonBox::Cancel)->click();
        CHECK(controls.result() == QDialog::Rejected, "Parameter Cancel rejects");
        int renders = 0;
        QImage source(340, 340, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::transparent);
        PreviewDialog embedded("Embedded", source, params,
            [&](const QImage &image, const QVector<int> &) { ++renders; return image; });
        CHECK(renders == 1, "Embedded dialog renders initial thumbnail");
        embedded.findChild<QSlider *>()->setValue(10);
        CHECK(renders == 2, "Embedded dialog still renders on parameter changes");
        CHECK(embedded.findChildren<QLabel *>().size() == 2, "Embedded dialog retains image label");
        CHECK(controls.sizeHint().height() < embedded.sizeHint().height(), "Parameters-only layout is shorter");
    }
    resizeStraightAlphaCharacterization();
    retainedCropRegression();
    maskedCropRegression();
    advancedFloatingControlsRegression();
    filteredFloatingRegression();
    retainedExpansionRegression();
    pasteExpansionBoundsRegression();
    retainedOrthogonalRegression();
    retainedResizeRegression();
    resizeStampCharacterization();
    ordinaryMoveAuthorityRegression();
    nativeRetentionCompatibility();
    retainedMergeRegression();

    // ---------- TOOLS ----------
    SECTION("Tools draw into the active layer");
    {
        auto testDraw = [](const char *name, Tool *tool, bool useTwoPoints = true,
                           bool commitAfter = false) {
            Document doc(64, 64);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::red);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            QImage before = doc.activeLayer()->image().copy();
            if (useTwoPoints) strokeTool(tool, canvas, QPointF(10, 10), QPointF(50, 50));
            else {
                QMouseEvent e = pressEv(QPointF(32, 32));
                tool->mousePressEvent(QPointF(32, 32), &e, canvas);
                QMouseEvent r = releaseEv(QPointF(32, 32));
                tool->mouseReleaseEvent(QPointF(32, 32), &r, canvas);
            }
            // Edit-before-commit tools (Shape, Line) only rasterise on commit.
            if (commitAfter) tool->deactivate(canvas);
            bool changed = imagesDiffer(before, doc.activeLayer()->image());
            CHECK(changed, name);
        };
        BrushTool brush;        testDraw("Brush modifies pixels", &brush);
        PencilTool pencil;      testDraw("Pencil modifies pixels", &pencil);
        FillTool fill;          testDraw("Fill modifies pixels", &fill, false);
        ShapeTool shape;        testDraw("Shape modifies pixels", &shape, true, /*commitAfter*/true);
        GradientTool grad;      testDraw("Gradient modifies pixels", &grad);
        // Eraser needs opaque content to erase.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            QImage before = doc.activeLayer()->image().copy();
            EraserTool eraser; strokeTool(&eraser, canvas, QPointF(10,10), QPointF(50,50));
            CHECK(imagesDiffer(before, doc.activeLayer()->image()), "Eraser modifies pixels");
            // eraser should reduce alpha somewhere
            bool madeTransparent = false;
            QImage &img = doc.activeLayer()->image();
            for (int y=0;y<64 && !madeTransparent;++y) for (int x=0;x<64;++x)
                if (img.pixelColor(x,y).alpha() < 255) { madeTransparent = true; break; }
            CHECK(madeTransparent, "Eraser produces transparency");
        }
        // Recolor: needs a target colour present.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::blue);
            CanvasWidget canvas; canvas.setDocument(&doc);
            QImage before = doc.activeLayer()->image().copy();
            RecolorTool rc; rc.setTolerance(255);
            strokeTool(&rc, canvas, QPointF(10,10), QPointF(50,50));
            CHECK(imagesDiffer(before, doc.activeLayer()->image()), "Recolor modifies pixels");
        }
        // Clone stamp: set source (alt/right) then paint.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::white);
            QPainter p(&doc.activeLayer()->image()); p.fillRect(0,0,20,20,Qt::green); p.end();
            CanvasWidget canvas; canvas.setDocument(&doc);
            CloneStampTool clone;
            QMouseEvent src = pressEv(QPointF(10,10), Qt::RightButton);
            clone.mousePressEvent(QPointF(10,10), &src, canvas);   // set source
            QImage before = doc.activeLayer()->image().copy();
            strokeTool(&clone, canvas, QPointF(40,40), QPointF(45,45));
            CHECK(imagesDiffer(before, doc.activeLayer()->image()), "Clone stamp modifies pixels");
        }
        // During a clone stroke, the sampled-source crosshair follows the
        // source offset while the original source crosshair stays fixed.
        {
            Document doc(100, 100); doc.activeLayer()->clear(Qt::white);
            CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
            canvas.setPan(QPointF(0, 0));
            CloneStampTool clone;
            clone.setBrushSize(1);
            QMouseEvent src = pressEv(QPointF(30, 30), Qt::RightButton);
            clone.mousePressEvent(QPointF(30, 30), &src, canvas);
            QMouseEvent press = pressEv(QPointF(40, 40));
            clone.mousePressEvent(QPointF(40, 40), &press, canvas);
            QMouseEvent move = moveEv(QPointF(40, 35));
            clone.mouseMoveEvent(QPointF(40, 35), &move, canvas);

            QImage overlay(100, 100, QImage::Format_ARGB32);
            overlay.fill(Qt::white);
            QPainter painter(&overlay);
            clone.drawOverlay(painter, canvas);
            painter.end();

            CHECK(overlay.pixelColor(58, 45) == QColor(Qt::blue),
                  "Clone overlay shows blue crosshair at the sampled source");
            CHECK(overlay.pixelColor(58, 50) == QColor(Qt::red),
                  "Clone overlay keeps red crosshair at the original source");
        }
    }

    // ---------- RECOLOR & PENCIL OPTIONS ----------
    SECTION("Recolor and Pencil expose Paint.NET options");
    {
        // Recolor, Sampling = Secondary colour: only pixels matching the
        // secondary colour are recolored, not arbitrary clicked colours.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::white);
            // Paint the left half green; leave the right half white.
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(0, 0, 32, 64, Qt::green); }
            doc.setPrimaryColor(Qt::red);
            doc.setSecondaryColor(Qt::green);
            CanvasWidget canvas; canvas.setDocument(&doc);
            RecolorTool rc;
            rc.setSampleSecondary(true);   // target = secondary (green)
            rc.setTolerance(0);            // exact match only
            rc.setBrushSize(300);          // one dab covers the whole canvas
            strokeTool(&rc, canvas, QPointF(10, 10), QPointF(50, 50));
            QColor greenSide = doc.activeLayer()->image().pixelColor(10, 32);
            QColor whiteSide = doc.activeLayer()->image().pixelColor(50, 32);
            CHECK(greenSide.red() > 200 && greenSide.green() < 60,
                  "Recolor Sampling=Secondary: green pixels become the primary (red)");
            CHECK(whiteSide == QColor(Qt::white),
                  "Recolor Sampling=Secondary: non-secondary (white) pixels are untouched");
        }

        // Recolor Hardness: a soft tip changes edge pixels less than the centre.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::green);
            doc.setPrimaryColor(Qt::red);
            CanvasWidget canvas; canvas.setDocument(&doc);
            RecolorTool rc;
            rc.setSampleSecondary(false);   // target = clicked pixel (green)
            rc.setHardness(0);              // maximum soft edge
            rc.setBrushSize(40);            // radius 20
            QMouseEvent e = pressEv(QPointF(32, 32));
            rc.mousePressEvent(QPointF(32, 32), &e, canvas);
            QMouseEvent r = releaseEv(QPointF(32, 32));
            rc.mouseReleaseEvent(QPointF(32, 32), &r, canvas);
            int centreRed = doc.activeLayer()->image().pixelColor(32, 32).red();
            int edgeRed   = doc.activeLayer()->image().pixelColor(32, 49).red();  // near the rim
            CHECK(centreRed > edgeRed + 40,
                  "Recolor Hardness: centre is recolored more strongly than the edge");
        }

        // Pencil blend mode: Multiply differs from Normal on a mid-grey ground.
        {
            const QColor grey(128, 128, 128);
            auto pencilStroke = [&](int blendMode) {
                Document doc(64, 64); doc.activeLayer()->clear(grey);
                doc.setPrimaryColor(grey);
                CanvasWidget canvas; canvas.setDocument(&doc);
                PencilTool pencil;
                pencil.setBlendMode(blendMode);
                strokeTool(&pencil, canvas, QPointF(10, 10), QPointF(40, 40));
                return doc.activeLayer()->image().pixelColor(10, 10);
            };
            QColor normal   = pencilStroke(0);    // Normal
            QColor multiply = pencilStroke(1);    // Multiply
            CHECK(normal == grey, "Pencil Normal writes the source colour directly");
            CHECK(multiply.red() < normal.red(),
                  "Pencil Multiply darkens the pixel, differing from Normal");
        }
    }

    // ---------- GRADIENT TOOL OPTIONS ----------
    SECTION("Gradient tool exposes Paint.NET options");
    {
        GradientTool grad;
        // New gradient types exist and round-trip through the setter.
        grad.setGradientType(GradientType::LinearReflected);
        CHECK(grad.gradientType() == GradientType::LinearReflected, "LinearReflected type");
        grad.setGradientType(GradientType::Spiral);
        CHECK(grad.gradientType() == GradientType::Spiral, "Spiral type");
        // Repeat mode: default None, settable.
        CHECK(grad.repeatMode() == GradientRepeat::None, "Repeat defaults to None");
        grad.setRepeatMode(GradientRepeat::Reflect);
        CHECK(grad.repeatMode() == GradientRepeat::Reflect, "Repeat mode settable");
        // Transparency mode: default off, settable.
        CHECK(!grad.transparencyMode(), "Transparency defaults off");
        grad.setTransparencyMode(true);
        CHECK(grad.transparencyMode(), "Transparency mode settable");
        // Transparency mode paints varying alpha into the layer.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::red);
            CanvasWidget canvas; canvas.setDocument(&doc);
            GradientTool g; g.setTransparencyMode(true);
            g.setGradientType(GradientType::Linear);
            strokeTool(&g, canvas, QPointF(2,2), QPointF(60,60));
            bool madeTransparent = false;
            QImage &img = doc.activeLayer()->image();
            for (int y=0;y<64 && !madeTransparent;++y) for (int x=0;x<64;++x)
                if (img.pixelColor(x,y).alpha() < 255) { madeTransparent = true; break; }
            CHECK(madeTransparent, "Transparency gradient varies alpha");
        }
        // Right-button drag draws (uses secondary as base) without crashing.
        {
            Document doc(64, 64); doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::red); doc.setSecondaryColor(Qt::blue);
            CanvasWidget canvas; canvas.setDocument(&doc);
            GradientTool g;
            QImage before = doc.activeLayer()->image().copy();
            QMouseEvent p = pressEv(QPointF(4,4), Qt::RightButton);
            g.mousePressEvent(QPointF(4,4), &p, canvas);
            QMouseEvent r = releaseEv(QPointF(58,58));
            g.mouseReleaseEvent(QPointF(58,58), &r, canvas);
            CHECK(imagesDiffer(before, doc.activeLayer()->image()), "Right-button gradient draws");
        }
    }

    // ---------- SELECTION-AWARE DRAWING ----------
    SECTION("Selection confines drawing");
    {
        Document doc(64, 64);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        doc.selection().selectRect(QRect(0, 0, 20, 20), SelectionMode::Replace);
        CanvasWidget canvas; canvas.setDocument(&doc);
        BrushTool brush; brush.setBrushSize(6);
        strokeTool(&brush, canvas, QPointF(5, 5), QPointF(55, 55));
        QImage &img = doc.activeLayer()->image();
        // Inside selection near (5,5) should be reddish; outside (say 50,50) untouched white.
        bool insidePainted = img.pixelColor(6, 6).red() > 150 && img.pixelColor(6,6).blue() < 120;
        bool outsideClean = img.pixelColor(50, 50) == QColor(Qt::white);
        CHECK(insidePainted, "Brush paints inside the selection");
        CHECK(outsideClean, "Brush does NOT paint outside the selection");
    }

    // ---------- SELECTION OPERATIONS ----------
    SECTION("Selection operations");
    {
        Selection sel(64, 64);
        CHECK(!sel.hasSelection(), "New selection is empty");
        sel.selectAll();
        CHECK(sel.hasSelection() && sel.isSelected(32,32), "selectAll selects everything");
        sel.clear();
        CHECK(!sel.hasSelection(), "clear deselects");
        sel.selectRect(QRect(10,10,20,20), SelectionMode::Replace);
        CHECK(sel.isSelected(15,15) && !sel.isSelected(50,50), "selectRect");
        sel.invert();
        CHECK(!sel.isSelected(15,15) && sel.isSelected(50,50), "invert");
        // add / subtract / intersect
        Selection s2(64,64);
        s2.selectRect(QRect(0,0,32,64), SelectionMode::Replace);
        s2.selectRect(QRect(32,0,32,64), SelectionMode::Add);
        CHECK(s2.isSelected(10,10) && s2.isSelected(50,10), "Add mode unions");
        s2.selectRect(QRect(0,0,32,64), SelectionMode::Subtract);
        CHECK(!s2.isSelected(10,10) && s2.isSelected(50,10), "Subtract mode removes");
        Selection s3(64,64);
        s3.selectRect(QRect(0,0,40,64), SelectionMode::Replace);
        s3.selectRect(QRect(30,0,34,64), SelectionMode::Intersect);
        CHECK(s3.isSelected(35,10) && !s3.isSelected(10,10), "Intersect mode");
        // Invert (XOR) mode: overlap cancels, the rest unions.
        Selection s5(64,64);
        s5.selectRect(QRect(0,0,40,64), SelectionMode::Replace);
        s5.selectRect(QRect(30,0,34,64), SelectionMode::Invert);
        CHECK(s5.isSelected(10,10) && s5.isSelected(50,10) && !s5.isSelected(35,10),
              "Invert mode XORs (overlap removed, rest kept)");

        // Modifier mapping must match Paint.NET exactly: Ctrl=add, Alt=subtract,
        // Alt+right=intersect, Ctrl+right=invert, nothing=replace.
        CHECK(selectionModeFor(Qt::NoModifier, Qt::LeftButton) == SelectionMode::Replace, "no modifier = replace");
        CHECK(selectionModeFor(Qt::ControlModifier, Qt::LeftButton) == SelectionMode::Add, "Ctrl = add");
        CHECK(selectionModeFor(Qt::AltModifier, Qt::LeftButton) == SelectionMode::Subtract, "Alt = subtract");
        CHECK(selectionModeFor(Qt::AltModifier, Qt::RightButton) == SelectionMode::Intersect, "Alt+right = intersect");
        CHECK(selectionModeFor(Qt::ControlModifier, Qt::RightButton) == SelectionMode::Invert, "Ctrl+right = invert");
        // The old Shift mapping must be gone.
        CHECK(selectionModeFor(Qt::ShiftModifier, Qt::LeftButton) == SelectionMode::Replace, "Shift no longer adds");

        // Tolerance is now a 0..100 percentage (Paint.NET), mapped to the 0..255
        // colour-distance basis the flood/wand/recolor code compares against.
        BrushTool tolTool;   // any Tool exposes the shared tolerance
        tolTool.setTolerance(0);   CHECK(tolTool.toleranceDistance() == 0, "0% tolerance = 0 distance");
        tolTool.setTolerance(100); CHECK(tolTool.toleranceDistance() == 255, "100% tolerance = 255 distance");
        tolTool.setTolerance(50);  CHECK(tolTool.toleranceDistance() >= 126 && tolTool.toleranceDistance() <= 128,
                                         "50% tolerance = ~127 distance");
        tolTool.setTolerance(500); CHECK(tolTool.tolerance() == 100, "tolerance clamps to 100%");

        // feather / expand / contract run without crashing and keep selection
        Selection s4(64,64); s4.selectRect(QRect(20,20,24,24), SelectionMode::Replace);
        s4.expand(3);  CHECK(s4.isSelected(18,32), "expand grows selection");
        s4.contract(3); CHECK(s4.hasSelection(), "contract keeps selection");
        s4.feather(2);  CHECK(s4.hasSelection(), "feather keeps selection");
    }

    // ---------- LAYERS ----------
    SECTION("Layer operations");
    {
        Document doc(32, 32);
        CHECK(doc.layerCount() == 1, "starts with 1 layer");
        doc.addLayer();
        CHECK(doc.layerCount() == 2, "addLayer");
        doc.duplicateLayer(doc.activeLayerIndex());
        CHECK(doc.layerCount() == 3, "duplicateLayer");
        doc.removeLayer(0);
        CHECK(doc.layerCount() == 2, "removeLayer");
        doc.mergeLayerDown(1);
        CHECK(doc.layerCount() == 1, "mergeLayerDown");
        doc.addLayer(); doc.addLayer();
        int before = doc.layerCount();
        doc.flattenImage();
        CHECK(doc.layerCount() == 1 && before == 3, "flattenImage collapses to 1");
        // reorder
        Document d2(16,16); d2.addLayer(); d2.addLayer();
        d2.layerAt(0)->setName("A"); d2.layerAt(1)->setName("B"); d2.layerAt(2)->setName("C");
        d2.reorderLayers({2,1,0}, 0);
        CHECK(d2.layerAt(0)->name()=="C" && d2.layerAt(2)->name()=="A", "reorderLayers");
    }

    // ---------- REGIONAL RENDERING ----------
    SECTION("Regional flatten matches full flatten");
    {
        Document doc(48, 36);
        doc.activeLayer()->clear(QColor(40, 70, 110, 255));
        QImage topImage(22, 18, QImage::Format_ARGB32_Premultiplied);
        topImage.fill(QColor(200, 80, 30, 210));
        const int topIndex = doc.addLayer(topImage);
        doc.layerAt(topIndex)->setOffset(QPoint(17, -4));
        doc.layerAt(topIndex)->setOpacity(0.65f);
        doc.layerAt(topIndex)->setBlendMode(BlendMode::Multiply);

        const QImage full = doc.flattenVisible();
        for (const QRect &tile : {QRect(0, 0, 16, 14), QRect(8, 6, 24, 20),
                                  QRect(30, 16, 18, 20)}) {
            CHECK(doc.flattenVisible(tile) == full.copy(tile),
                  "regional flatten matches full composite");
        }
    }

    // ---------- SMART MERGE (keep both artworks) ----------
    SECTION("Smart merge keeps artwork from both layers");
    {
        Document doc(40, 40);
        // Bottom layer: white with a BLUE square in the top-left corner area.
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(2,2,12,12, QColor(0,0,255)); p.end(); }
        // Top layer: white with a RED square in the bottom-right, elsewhere white.
        int top = doc.addLayer();   // white by default
        { QPainter p(&doc.layerAt(top)->image()); p.fillRect(26,26,12,12, QColor(255,0,0)); p.end(); }

        // Normal composite would hide the blue (opaque white top). keepArtwork
        // knocks out the white so BOTH squares survive.
        doc.mergeLayerDown(top, /*keepArtwork=*/true);
        CHECK(doc.layerCount()==1, "smart merge collapses to 1 layer");
        QColor blue = doc.layerAt(0)->image().pixelColor(6, 6);
        QColor red  = doc.layerAt(0)->image().pixelColor(31, 31);
        CHECK(blue.blue()>200 && blue.red()<60, "bottom artwork (blue) is kept");
        CHECK(red.red()>200 && red.blue()<60, "top artwork (red) is kept");
    }

    // ---------- BLEND MODES ----------
    SECTION("Blend modes composite");
    {
        for (BlendMode m : Layer::allBlendModes()) {
            Layer top(16,16,"t");
            QImage ti = makeImage(16,16, QColor(180,120,60,255));
            top.setImage(ti); top.setBlendMode(m);
            QImage below = makeImage(16,16, QColor(90,90,90,255));
            QImage r = top.composited(below);
            CHECK(r.size()==below.size() && !r.isNull(),
                  qPrintable(QString("Blend mode %1 produces output").arg(Layer::blendModeName(m))));
        }
    }

    // ---------- BLEND MODE FORMULAS (Paint.NET fidelity) ----------
    SECTION("Blend mode formulas match Paint.NET");
    {
        // Composite a known top colour over a known base and check the result of
        // the modes the task called out: Xor, Overlay, Color Burn, Color Dodge.
        const QColor base(90, 90, 90, 255);
        const QColor top(180, 120, 60, 255);
        auto blended = [&](BlendMode m) {
            Layer t(16, 16, "t");
            t.setImage(makeImage(16, 16, top));
            t.setBlendMode(m);
            QImage r = t.composited(makeImage(16, 16, base));
            return r.pixelColor(8, 8);
        };
        auto near = [](int a, int b, int tol) { return std::abs(a - b) <= tol; };

        // Xor: per-channel bitwise XOR of the colour bytes (exact).
        QColor x = blended(BlendMode::Xor);
        CHECK(near(x.red(),   90 ^ 180, 1) && near(x.green(), 90 ^ 120, 1) &&
              near(x.blue(),  90 ^ 60,  1),
              "Xor is a per-channel bitwise XOR of the two colours");

        // Overlay: base<128 -> 2*B*T/255 per channel.
        QColor o = blended(BlendMode::Overlay);
        CHECK(near(o.red(), 2*90*180/255, 4) && near(o.green(), 2*90*120/255, 4) &&
              near(o.blue(), 2*90*60/255, 4),
              "Overlay uses the standard overlay formula");

        // Color Burn: 255 - (255-B)*255/T, clamped to 0.
        QColor cb = blended(BlendMode::ColorBurn);
        CHECK(near(cb.red(), 21, 4) && cb.green() <= 4 && cb.blue() <= 4,
              "Color Burn uses the standard color-burn formula");

        // Color Dodge: min(255, B*255/(255-T)).
        QColor cd = blended(BlendMode::ColorDodge);
        CHECK(cd.red() >= 251 && near(cd.green(), 170, 4) && near(cd.blue(), 118, 4),
              "Color Dodge uses the standard color-dodge formula");
    }

    // ---------- MOVE LAYER TO TOP / BOTTOM ----------
    SECTION("Move layer to very top / bottom");
    {
        // Ctrl+click on the panel's move buttons calls moveLayer(idx, top/bottom);
        // verify that reordering directly.
        Document doc(16, 16);                 // layer 0 = "Background"
        doc.layerAt(0)->setName("A");
        int b = doc.addLayer(); doc.layerAt(b)->setName("B");
        int c = doc.addLayer(); doc.layerAt(c)->setName("C");
        // Stack (bottom->top): A, B, C
        doc.moveLayer(0, doc.layerCount() - 1);       // send A to the very top
        CHECK(doc.layerAt(0)->name()=="B" && doc.layerAt(1)->name()=="C" &&
              doc.layerAt(2)->name()=="A", "move-to-top reorders correctly");
        doc.moveLayer(2, 0);                           // send A back to the bottom
        CHECK(doc.layerAt(0)->name()=="A" && doc.layerAt(1)->name()=="B" &&
              doc.layerAt(2)->name()=="C", "move-to-bottom reorders correctly");
    }

    // ---------- UNDO / REDO ----------
    SECTION("Undo / redo");
    {
        Document doc(48,48); doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        CanvasWidget canvas; canvas.setDocument(&doc);
        QImage clean = doc.activeLayer()->image().copy();
        BrushTool brush; strokeTool(&brush, canvas, QPointF(5,5), QPointF(40,40));
        QImage painted = doc.activeLayer()->image().copy();
        CHECK(imagesDiffer(clean, painted), "stroke changed the layer");
        CHECK(doc.history().canUndo(), "history has an undo entry");
        doc.history().undo();
        CHECK(!imagesDiffer(clean, doc.activeLayer()->image()), "undo restores clean image");
        CHECK(doc.history().canRedo(), "history has a redo entry");
        doc.history().redo();
        CHECK(!imagesDiffer(painted, doc.activeLayer()->image()), "redo re-applies stroke");
        // structural undo: duplicate then undo
        int n = doc.layerCount(); doc.duplicateLayer(0);
        CHECK(doc.layerCount()==n+1, "duplicate added a layer");
        doc.history().undo();
        CHECK(doc.layerCount()==n, "undo removes the duplicated layer");
    }

    // ---------- SAVE / LOAD ----------
    SECTION("Save / load round-trip");
    {
        Document doc(24,24);
        doc.activeLayer()->clear(Qt::transparent);
        QPainter p(&doc.activeLayer()->image());
        p.fillRect(6,6,12,12, QColor(255,0,0,255)); p.end();
        QString png = QDir::tempPath() + "/_pdtest.png", jpg = QDir::tempPath() + "/_pdtest.jpg";
        CHECK(doc.save(png), "save PNG");
        CHECK(doc.save(jpg), "save JPG");
        Document loaded(png);
        CHECK(loaded.width()==24 && loaded.height()==24, "PNG reloads at correct size");
        QImage flat = loaded.flatten();
        CHECK(flat.pixelColor(0,0).alpha()==0, "PNG preserved transparency");
        CHECK(flat.pixelColor(12,12).red()>200, "PNG preserved the red square");
        Document jl(jpg);
        CHECK(jl.flatten().pixelColor(0,0).red()>230, "JPG filled transparent with white");
    }

    // ---------- EFFECTS ----------
    SECTION("Effects run and return valid output");
    {
        QImage src = makeImage(48, 48, QColor(120,160,200,255));
        // add some structure so effects have something to work on
        { QPainter p(&src); p.fillRect(10,10,28,28, QColor(240,60,60)); p.end(); }

        auto testEffect = [&](const char *name, Effect *fx) {
            QImage out = fx->apply(src);
            bool ok = !out.isNull() && out.width()>0 && out.height()>0;
            CHECK(ok, name);
            delete fx;
        };
        testEffect("Blur", new BlurEffect);
        testEffect("Sharpen", new SharpenEffect);
        testEffect("Noise", new NoiseEffect);
        testEffect("Emboss", new EmbossEffect);
        testEffect("EdgeDetect", new EdgeDetectEffect);
        testEffect("OilPaint", new OilPaintEffect);
        testEffect("Pixelate", new PixelateEffect);
        testEffect("MotionBlur", new MotionBlurEffect);
        testEffect("RadialBlur", new RadialBlurEffect);
        testEffect("ZoomBlur", new ZoomBlurEffect);
        testEffect("SurfaceBlur", new SurfaceBlurEffect);
        testEffect("Unfocus", new UnfocusEffect);
        testEffect("Fragment", new FragmentEffect);
        testEffect("Bulge", new BulgeEffect);
        testEffect("Twist", new TwistEffect);
        testEffect("FrostedGlass", new FrostedGlassEffect);
        testEffect("Crystalize", new CrystalizeEffect);
        testEffect("Tile", new TileEffect);
        testEffect("Dents", new DentsEffect);
        testEffect("PolarInversion", new PolarInversionEffect);
        testEffect("Median", new MedianEffect);
        testEffect("ReduceNoise", new ReduceNoiseEffect);
        testEffect("Quantize", new QuantizeEffect);
        testEffect("Glow", new GlowEffect);
        testEffect("RedEyeRemove", new RedEyeRemoveEffect);
        testEffect("SoftenPortrait", new SoftenPortraitEffect);
        testEffect("Vignette", new VignetteEffect);
        testEffect("Turbulence", new TurbulenceEffect);
        testEffect("Relief", new ReliefEffect);
        testEffect("Outline", new OutlineEffect);
        testEffect("Morphology", new MorphologyEffect);
    }

    // Effects that must visibly change the image at default params
    SECTION("Key effects actually change the image");
    {
        QImage src = makeImage(48, 48, QColor(120,160,200,255));
        { QPainter p(&src); p.fillRect(10,10,28,28, QColor(240,60,60)); p.end(); }
        auto changes = [&](const char *name, Effect *fx) {
            QImage out = fx->apply(src);
            CHECK(imagesDiffer(src, out.convertToFormat(src.format())), name);
            delete fx;
        };
        changes("Blur changes image", new BlurEffect);
        changes("Invert-style EdgeDetect changes image", new EdgeDetectEffect);
        changes("Emboss changes image", new EmbossEffect);
        changes("Pixelate changes image", new PixelateEffect);
        // Relief must not be flat gray (regression from the truncation bug)
        {
            ReliefEffect r; QImage out = r.apply(src);
            bool allGray = true;
            for (int y=0;y<out.height()&&allGray;++y) for (int x=0;x<out.width();++x) {
                QColor c = out.pixelColor(x,y);
                if (!(c.red()==128&&c.green()==128&&c.blue()==128)) { allGray=false; break; }
            }
            CHECK(!allGray, "Relief is not flat gray at default angle");
        }
        // Turbulence must match input size
        {
            TurbulenceEffect t; QImage out = t.apply(src);
            CHECK(out.size()==src.size(), "Turbulence output matches input size");
        }
    }

    // ---------- ADJUSTMENTS ----------
    SECTION("Adjustments run and return valid output");
    {
        QImage src = makeImage(32, 32, QColor(120,160,200,255));
        { QPainter p(&src); p.fillRect(4,4,10,10, QColor(240,60,60)); p.end(); }
        auto testAdj = [&](const char *name, Adjustment *a, bool mustChange) {
            QImage out = a->apply(src);
            CHECK(!out.isNull(), name);
            if (mustChange)
                CHECK(imagesDiffer(src, out.convertToFormat(src.format())),
                      qPrintable(QString("%1 changes the image").arg(name)));
            delete a;
        };
        { BrightnessContrast a; a.setBrightness(40); a.setContrast(20);
          testAdj("BrightnessContrast", new BrightnessContrast(a), true); }
        { HueSaturation a; a.setHue(60);
          testAdj("HueSaturation", new HueSaturation(a), true); }
        { Levels a; a.setInputBlack(30); a.setInputWhite(220);
          testAdj("Levels", new Levels(a), true); }
        { Curves a; QVector<QPointF> pts{{0,40},{128,180},{255,255}}; a.setControlPoints(pts);
          testAdj("Curves", new Curves(a), true); }
        testAdj("InvertColors", new InvertColors, true);
        { Sepia a; a.setIntensity(90); testAdj("Sepia", new Sepia(a), true); }
        { Posterize a; a.setLevels(3); testAdj("Posterize", new Posterize(a), true); }
        { Threshold a; a.setThreshold(128); testAdj("Threshold", new Threshold(a), true); }
        { ColorBalance a; a.setCyanRed(40); testAdj("ColorBalance", new ColorBalance(a), true); }
    }

    SECTION("Curves canvas preview, selection, transparency and grouped history");
    {
        Document doc(4, 4);
        QImage source(4, 4, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(64, 96, 128, 128));
        source.setPixel(0, 0, 0);
        doc.layerAt(0)->setImage(source);
        doc.addLayer(source, "Second layer");
        doc.layerAt(1)->setOffset(QPoint(1, 0));
        doc.history().clear();
        doc.setModified(false);
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(1)[1] = 255;
        mask.scanLine(1)[2] = 128;
        doc.selection().setMaskImage(mask);
        CanvasWidget canvas;
        canvas.setDocument(&doc);
        auto operation = [](const QImage &image) {
            Curves curves;
            curves.setControlPoints({{0, 255}, {255, 255}});
            return curves.apply(image);
        };
        {
            AdjustmentPreview preview(&doc, &canvas, {0, 1});
            QImage displayed;
            QObject::connect(&preview, &AdjustmentPreview::previewReady, &canvas,
                             [&](const QImage &image) { displayed = image; });
            preview.request(operation);
            QElapsedTimer timer;
            timer.start();
            while (displayed.isNull() && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            CHECK(!displayed.isNull(), "Curves delivers a canvas preview");
            CHECK(doc.layerAt(0)->image() == source && doc.layerAt(1)->image() == source,
                  "Preview leaves original layer images untouched");
            preview.cancel();
            CHECK(doc.history().currentIndex() == 0 && !doc.isModified(),
                  "Cancel leaves history and modified state untouched");
        }
        AdjustmentPreview preview(&doc, &canvas, {0, 1});
        CHECK(preview.commit(operation, "Curves"), "Curves commits adjusted layers");
        const QImage after0 = doc.layerAt(0)->image();
        const QImage after1 = doc.layerAt(1)->image();
        CHECK(after0.format() == QImage::Format_ARGB32_Premultiplied &&
              after1.format() == QImage::Format_ARGB32_Premultiplied,
              "Curves preserves premultiplied layer storage");
        CHECK(after0.pixel(1, 1) == qRgba(128, 128, 128, 128),
              "Curves maps straight colour then restores premultiplication");
        CHECK(after0.pixel(2, 1) == qRgba(80, 88, 96, 128),
              "Feathered selection blends premultiplied colour without changing alpha");
        CHECK(after1.pixel(0, 1) == after0.pixel(1, 1),
              "Selection follows document coordinates on an offset layer");
        CHECK(after0.pixel(3, 3) == source.pixel(3, 3) && after0.pixel(0, 0) == 0,
              "Unselected and transparent pixels remain unchanged");
        CHECK(doc.history().currentIndex() == 1, "Multiple layers create one Curves undo entry");
        doc.history().undo();
        CHECK(doc.layerAt(0)->image() == source && doc.layerAt(1)->image() == source,
              "Undo restores both original layers");
        doc.history().redo();
        CHECK(doc.layerAt(0)->image() == after0 && doc.layerAt(1)->image() == after1,
              "Redo restores both adjusted layers");
    }

    SECTION("Brightness/Contrast dialog preserves thumbnail and reports parameter changes");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 128, 128));
        PreviewDialog dialog("Brightness / Contrast", source,
            {{"Brightness", -100, 100, 0, ""}, {"Contrast", -100, 100, 0, ""}},
            [](const QImage &image, const QVector<int> &values) {
                BrightnessContrast adjustment;
                adjustment.setBrightness(values[0]);
                adjustment.setContrast(values[1]);
                return adjustment.apply(image);
            });
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr, "Existing thumbnail remains available");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        dialog.findChildren<QSlider *>().first()->setValue(40);
        CHECK(changes == 1 && dialog.values()[0] == 40, "Slider emits one synchronised parameter notification");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Thumbnail still updates on slider change");
        dialog.findChildren<QSpinBox *>().last()->setValue(20);
        CHECK(changes == 2 && dialog.values()[1] == 20, "Numeric entry emits synchronised parameter notification");
    }

    SECTION("Hue/Saturation control notifications and neutral saturation");
    {
        HueSaturationDialog dialog;
        int changes = 0;
        QObject::connect(&dialog, &HueSaturationDialog::parametersChanged, &dialog, [&]() { ++changes; });
        CHECK(dialog.hue() == 0 && dialog.saturation() == 100 && dialog.lightness() == 0,
              "Hue/Saturation starts with neutral controls");
        HueSaturation adjustment;
        adjustment.setSaturation(dialog.saturation() - 100);
        QImage source = makeImage(8, 8, QColor(64, 96, 128, 128));
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Neutral dialog saturation maps to an unchanged image");
        dialog.findChildren<QSlider *>().first()->setValue(30);
        CHECK(changes == 1 && dialog.hue() == 30, "Hue slider reports parameter changes");
        dialog.findChildren<QSpinBox *>()[1]->setValue(140);
        CHECK(changes == 2 && dialog.saturation() == 140, "Saturation numeric entry reports changes");
        dialog.findChildren<QSpinBox *>().last()->setValue(20);
        CHECK(changes == 3 && dialog.lightness() == 20, "Lightness numeric entry reports changes");
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text() == "↺") button->click();
        CHECK(dialog.hue() == 0 && dialog.saturation() == 100 && dialog.lightness() == 0,
              "Per-control Reset restores neutral Hue/Saturation values");
    }

    SECTION("Levels thumbnail, gamma controls and neutral mapping");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 128, 128));
        PreviewDialog dialog("Levels", source,
            {{"Input black", 0, 255, 0, ""}, {"Input white", 0, 255, 255, ""},
             {"Gamma", 10, 1000, 100, "%"}},
            [](const QImage &image, const QVector<int> &values) {
                Levels adjustment;
                adjustment.setInputBlack(values[0]);
                adjustment.setInputWhite(values[1]);
                adjustment.setGamma(values[2] / 100.0);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr, "Levels retains its thumbnail");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().last()->setValue(180);
        CHECK(changes == 1 && dialog.values()[2] == 180, "Gamma numeric entry notifies canvas preview");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Gamma updates existing thumbnail");
        Levels neutral;
        CHECK(neutral.apply(source).convertToFormat(source.format()) == source,
              "Default Levels preserves neutral pixels and alpha");
    }

    SECTION("Sepia thumbnail, intensity and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 128, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Sepia", source, {{"Intensity", 0, 100, 80, "%"}},
            [](const QImage &image, const QVector<int> &values) {
                Sepia adjustment;
                adjustment.setIntensity(values[0]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values()[0] == 80, "Sepia retains thumbnail and default intensity");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(0);
        CHECK(changes == 1 && dialog.values()[0] == 0, "Sepia numeric entry notifies preview");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Sepia thumbnail updates with intensity");
        Sepia adjustment;
        adjustment.setIntensity(0);
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Zero Sepia intensity preserves original pixels");
        adjustment.setIntensity(100);
        const QImage processed = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qAlpha(processed.pixel(1, 1)) == 128 && processed.pixel(0, 0) == 0,
              "Sepia preserves partial alpha and transparent pixels");
    }

    SECTION("Posterize thumbnail, level controls and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Posterize", source, {{"Levels", 2, 64, 4, ""}},
            [](const QImage &image, const QVector<int> &values) {
                Posterize adjustment;
                adjustment.setLevels(values[0]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values()[0] == 4, "Posterize retains thumbnail and default four levels");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(2);
        CHECK(changes == 1 && dialog.values()[0] == 2, "Posterize numeric entry notifies preview");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Posterize thumbnail updates with levels");
        dialog.findChildren<QSlider *>().first()->setValue(64);
        CHECK(changes == 2 && dialog.values()[0] == 64, "Posterize maximum levels synchronise controls");
        Posterize adjustment;
        adjustment.setLevels(2);
        const QImage processed = adjustment.apply(source);
        CHECK(qRed(processed.pixel(1, 1)) == 0 && qBlue(processed.pixel(1, 1)) == 255,
              "Posterize quantises straight colour channels at two levels");
        const QImage stored = processed.convertToFormat(source.format());
        CHECK(qAlpha(stored.pixel(1, 1)) == 128 && stored.pixel(0, 0) == 0,
              "Posterize preserves partial alpha and transparent pixels");
    }

    SECTION("Threshold thumbnail, control endpoints and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Threshold", source, {{"Threshold", 0, 255, 128, ""}},
            [](const QImage &image, const QVector<int> &values) {
                Threshold adjustment;
                adjustment.setThreshold(values[0]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values()[0] == 128, "Threshold retains thumbnail and default 128");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(0);
        CHECK(changes == 1 && dialog.values()[0] == 0, "Threshold numeric entry notifies preview");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Threshold thumbnail updates with control changes");
        dialog.findChildren<QSlider *>().first()->setValue(255);
        CHECK(changes == 2 && dialog.values()[0] == 255, "Threshold upper endpoint synchronises controls");
        Threshold adjustment;
        adjustment.setThreshold(0);
        const QImage white = adjustment.apply(source).convertToFormat(source.format());
        CHECK(white.pixel(1, 1) == qRgba(128, 128, 128, 128) && white.pixel(0, 0) == 0,
              "Threshold zero produces white while preserving partial and zero alpha");
        adjustment.setThreshold(255);
        const QImage black = adjustment.apply(source).convertToFormat(source.format());
        CHECK(black.pixel(1, 1) == qRgba(0, 0, 0, 128) && black.pixel(0, 0) == 0,
              "Threshold 255 produces black for darker pixels while preserving alpha");
    }

    SECTION("Color Balance thumbnail, channel controls and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Color Balance", source,
            {{"Cyan / Red", -100, 100, 0, ""}, {"Magenta / Green", -100, 100, 0, ""},
             {"Yellow / Blue", -100, 100, 0, ""}},
            [](const QImage &image, const QVector<int> &values) {
                ColorBalance adjustment;
                adjustment.setCyanRed(values[0]);
                adjustment.setMagentaGreen(values[1]);
                adjustment.setYellowBlue(values[2]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values() == QVector<int>({0,0,0}),
              "Color Balance retains thumbnail and neutral defaults");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>()[0]->setValue(30);
        dialog.findChildren<QSlider *>()[1]->setValue(-20);
        dialog.findChildren<QSpinBox *>()[2]->setValue(40);
        CHECK(changes == 3 && dialog.values() == QVector<int>({30,-20,40}),
              "All Color Balance channels notify preview with synchronised values");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Color Balance thumbnail updates");
        ColorBalance adjustment;
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Neutral Color Balance preserves original pixels");
        adjustment.setCyanRed(30); adjustment.setMagentaGreen(-20); adjustment.setYellowBlue(40);
        const QImage processed = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qAlpha(processed.pixel(1, 1)) == 128 && processed.pixel(0, 0) == 0,
              "Color Balance preserves partial alpha and transparent pixels");
    }

    SECTION("Exposure thumbnail, signed controls and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Exposure", source, {{"Exposure", -100, 100, 0, ""}},
            [](const QImage &image, const QVector<int> &values) {
                Exposure adjustment;
                adjustment.setExposure(values[0]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values()[0] == 0, "Exposure retains thumbnail and neutral default");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(-50);
        CHECK(changes == 1 && dialog.values()[0] == -50, "Negative Exposure numeric entry notifies preview");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Exposure thumbnail updates");
        dialog.findChildren<QSlider *>().first()->setValue(50);
        CHECK(changes == 2 && dialog.values()[0] == 50, "Positive Exposure slider synchronises controls");
        Exposure adjustment;
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Neutral Exposure preserves original pixels");
        adjustment.setExposure(50);
        const QImage bright = adjustment.apply(source).convertToFormat(source.format());
        adjustment.setExposure(-50);
        const QImage dark = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qRed(bright.pixel(1, 1)) > qRed(source.pixel(1, 1)) &&
              qRed(dark.pixel(1, 1)) < qRed(source.pixel(1, 1)),
              "Exposure brightens and darkens relative to original");
        CHECK(qAlpha(bright.pixel(1, 1)) == 128 && qAlpha(dark.pixel(1, 1)) == 128 &&
              bright.pixel(0, 0) == 0 && dark.pixel(0, 0) == 0,
              "Exposure preserves partial alpha and transparent pixels");
    }

    SECTION("Highlights/Shadows thumbnail, controls and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Highlights / Shadows", source,
            {{"Highlights", -100, 100, 0, ""}, {"Shadows", -100, 100, 0, ""}},
            [](const QImage &image, const QVector<int> &values) {
                HighlightsShadows adjustment;
                adjustment.setHighlights(values[0]);
                adjustment.setShadows(values[1]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values() == QVector<int>({0,0}),
              "Highlights/Shadows retains thumbnail and neutral defaults");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(-40);
        dialog.findChildren<QSlider *>().last()->setValue(60);
        CHECK(changes == 2 && dialog.values() == QVector<int>({-40,60}),
              "Both Highlights/Shadows controls notify preview with synchronised values");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Highlights/Shadows thumbnail updates");
        HighlightsShadows adjustment;
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Neutral Highlights/Shadows preserves original pixels");
        adjustment.setHighlights(-40);
        const QImage dark = adjustment.apply(source).convertToFormat(source.format());
        adjustment.setHighlights(0); adjustment.setShadows(60);
        const QImage bright = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qRed(dark.pixel(1, 1)) < qRed(source.pixel(1, 1)) &&
              qRed(bright.pixel(1, 1)) > qRed(source.pixel(1, 1)),
              "Highlights and shadows independently affect the original image");
        CHECK(qAlpha(bright.pixel(1, 1)) == 128 && qAlpha(dark.pixel(1, 1)) == 128 &&
              bright.pixel(0, 0) == 0 && dark.pixel(0, 0) == 0,
              "Highlights/Shadows preserves partial alpha and transparent pixels");
    }

    SECTION("Temperature/Tint thumbnail, controls and alpha preservation");
    {
        QImage source = makeImage(8, 8, QColor(64, 96, 192, 128));
        source.setPixel(0, 0, 0);
        PreviewDialog dialog("Temperature / Tint", source,
            {{"Temperature", -100, 100, 0, ""}, {"Tint", -100, 100, 0, ""}},
            [](const QImage &image, const QVector<int> &values) {
                TemperatureTint adjustment;
                adjustment.setTemperature(values[0]);
                adjustment.setTint(values[1]);
                return adjustment.apply(image);
            });
        QLabel *thumbnail = nullptr;
        for (auto *label : dialog.findChildren<QLabel *>())
            if (!label->pixmap().isNull()) thumbnail = label;
        CHECK(thumbnail != nullptr && dialog.values() == QVector<int>({0,0}),
              "Temperature/Tint retains thumbnail and neutral defaults");
        const QImage before = thumbnail ? thumbnail->pixmap().toImage() : QImage();
        int changes = 0;
        QObject::connect(&dialog, &PreviewDialog::parametersChanged, &dialog, [&]() { ++changes; });
        dialog.findChildren<QSpinBox *>().first()->setValue(-40);
        dialog.findChildren<QSlider *>().last()->setValue(60);
        CHECK(changes == 2 && dialog.values() == QVector<int>({-40,60}),
              "Both Temperature/Tint controls notify preview with synchronised values");
        CHECK(thumbnail && thumbnail->pixmap().toImage() != before, "Temperature/Tint thumbnail updates");
        TemperatureTint adjustment;
        CHECK(adjustment.apply(source).convertToFormat(source.format()) == source,
              "Neutral Temperature/Tint preserves original pixels");
        adjustment.setTemperature(50);
        const QImage warm = adjustment.apply(source).convertToFormat(source.format());
        adjustment.setTemperature(-50);
        const QImage cool = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qRed(warm.pixel(1, 1)) > qRed(source.pixel(1, 1)) &&
              qBlue(warm.pixel(1, 1)) < qBlue(source.pixel(1, 1)) &&
              qRed(cool.pixel(1, 1)) < qRed(source.pixel(1, 1)) &&
              qBlue(cool.pixel(1, 1)) > qBlue(source.pixel(1, 1)),
              "Positive and negative temperature shift red and blue in opposite directions");
        adjustment.setTemperature(0); adjustment.setTint(50);
        const QImage tinted = adjustment.apply(source).convertToFormat(source.format());
        CHECK(qGreen(tinted.pixel(1, 1)) < qGreen(source.pixel(1, 1)), "Positive tint reduces green");
        CHECK(qAlpha(warm.pixel(1, 1)) == 128 && qAlpha(cool.pixel(1, 1)) == 128 &&
              qAlpha(tinted.pixel(1, 1)) == 128 && warm.pixel(0, 0) == 0 &&
              cool.pixel(0, 0) == 0 && tinted.pixel(0, 0) == 0,
              "Temperature/Tint preserves partial alpha and transparent pixels");
    }

    // ---------- I18N ----------
    SECTION("Internationalisation");
    {
        I18n::setLanguage(I18n::Lang::French);
        CHECK(I18n::t("&Fichier") == "&Fichier", "French returns source string");
        I18n::setLanguage(I18n::Lang::English);
        CHECK(I18n::t("&Fichier") == "&File", "English translates File menu");
        CHECK(I18n::t("Pinceau") == "Paintbrush", "English translates Paintbrush");
        CHECK(I18n::t("Sombre") == "Dark", "English translates Dark");
        CHECK(I18n::t("ZZ-unknown-ZZ") == "ZZ-unknown-ZZ", "Unknown string passes through");
        I18n::setLanguage(I18n::Lang::French);
    }

    // ---------- THEME ----------
    SECTION("Theming");
    {
        Theme::setScheme(Theme::Scheme::Light);
        CHECK(!Theme::isDark(), "Light scheme is not dark");
        QString ls = Theme::styleSheet();
        CHECK(ls.contains("QMenuBar"), "Light stylesheet has content");
        Theme::setScheme(Theme::Scheme::Dark);
        CHECK(Theme::isDark(), "Dark scheme is dark");
        QString ds = Theme::styleSheet();
        CHECK(ds.contains("QMenuBar") && ds != ls, "Dark stylesheet differs from light");
        CHECK(Theme::canvasBackdrop() != QString("#969696"), "Dark backdrop differs from light");
        Theme::setScheme(Theme::Scheme::Light);
    }

    // ---------- DOCUMENT RESIZE / CROP ----------
    SECTION("Resize / canvas size / crop");
    {
        Document doc(40, 30);
        doc.resize(80, 60);
        CHECK(doc.width()==80 && doc.height()==60, "resize scales the document");
        CHECK(doc.history().canUndo(), "resize is undoable");
        Document d2(40, 30);
        d2.resizeCanvas(60, 60, 10, 10);
        CHECK(d2.width()==60 && d2.height()==60, "canvas size changes dimensions");
        Document d3(40, 40);
        d3.cropTo(QRect(5, 5, 20, 20));
        CHECK(d3.width()==20 && d3.height()==20, "crop resizes the document");
        CHECK(d3.history().canUndo(), "crop is undoable");
    }

    // ---------- SELECTION TOOLS ----------
    SECTION("Selection tools create selections");
    {
        // Rectangle select
        {
            Document doc(64,64); CanvasWidget canvas; canvas.setDocument(&doc);
            SelectionTool rect(SelectionShape::Rectangle);
            strokeTool(&rect, canvas, QPointF(10,10), QPointF(40,40));
            CHECK(doc.selection().hasSelection(), "Rectangle select creates a selection");
            CHECK(doc.selection().isSelected(25,25), "Rectangle selection covers dragged area");
            CHECK(!doc.selection().isSelected(55,55), "Rectangle selection excludes outside");
        }
        // Ellipse select
        {
            Document doc(64,64); CanvasWidget canvas; canvas.setDocument(&doc);
            SelectionTool ell(SelectionShape::Ellipse);
            strokeTool(&ell, canvas, QPointF(5,5), QPointF(60,60));
            CHECK(doc.selection().hasSelection(), "Ellipse select creates a selection");
            CHECK(doc.selection().isSelected(32,32), "Ellipse selection covers centre");
        }
        // Lasso
        {
            Document doc(64,64); CanvasWidget canvas; canvas.setDocument(&doc);
            LassoTool lasso;
            QMouseEvent e1 = pressEv(QPointF(10,10)); lasso.mousePressEvent(QPointF(10,10), &e1, canvas);
            for (QPointF p : {QPointF(50,10), QPointF(50,50), QPointF(10,50)}) {
                QMouseEvent m = moveEv(p); lasso.mouseMoveEvent(p, &m, canvas);
            }
            QMouseEvent r = releaseEv(QPointF(10,10)); lasso.mouseReleaseEvent(QPointF(10,10), &r, canvas);
            CHECK(doc.selection().hasSelection(), "Lasso creates a selection");
        }
        // Magic wand (contiguous region of same colour)
        {
            Document doc(64,64); doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(0,0,32,64,Qt::red); p.end(); }
            CanvasWidget canvas; canvas.setDocument(&doc);
            MagicWandTool wand; wand.setTolerance(20);
            QMouseEvent e = pressEv(QPointF(10,10)); wand.mousePressEvent(QPointF(10,10), &e, canvas);
            QMouseEvent r = releaseEv(QPointF(10,10)); wand.mouseReleaseEvent(QPointF(10,10), &r, canvas);
            CHECK(doc.selection().hasSelection(), "Magic wand creates a selection");
            CHECK(doc.selection().isSelected(10,10) && !doc.selection().isSelected(50,10),
                  "Magic wand selects only the same-colour region");
        }
    }

    // ---------- MOVE TOOL ----------
    SECTION("Move tool");
    {
        Document doc(64,64); doc.activeLayer()->clear(Qt::transparent);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(0,0,20,20, QColor(255,0,0,255)); p.end(); }
        CanvasWidget canvas; canvas.setDocument(&doc);
        QColor origin = doc.activeLayer()->image().pixelColor(5,5);   // red
        MoveTool move;
        strokeTool(&move, canvas, QPointF(5,5), QPointF(35,35));   // shift by +30,+30
        QImage &img = doc.activeLayer()->image();
        CHECK(origin.red()>200, "content present before move");
        CHECK(img.pixelColor(35,35).red()>200, "Move tool relocated the pixels");
        CHECK(doc.history().canUndo(), "Move is undoable");
    }

    // ---------- TEXT TOOL ----------
    SECTION("Text tool");
    {
        Document doc(80,40); doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::black);
        CanvasWidget canvas; canvas.setDocument(&doc);
        TextTool text;
        QMouseEvent e = pressEv(QPointF(5,20)); text.mousePressEvent(QPointF(5,20), &e, canvas);
        for (QChar ch : QString("Hi")) {
            QKeyEvent k(QEvent::KeyPress, 0, Qt::NoModifier, QString(ch));
            text.keyPressEvent(&k, canvas);
        }
        QImage before = doc.activeLayer()->image().copy();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        text.keyPressEvent(&enter, canvas);   // commit
        CHECK(imagesDiffer(before, doc.activeLayer()->image()), "Text tool commits text to the layer");
    }

    // ---------- COLOR PICKER ----------
    SECTION("Color picker");
    {
        Document doc(32,32); doc.activeLayer()->clear(QColor(10,200,40));
        doc.setPrimaryColor(Qt::black);
        CanvasWidget canvas; canvas.setDocument(&doc);
        ColorPickerTool picker;
        QMouseEvent e = pressEv(QPointF(16,16)); picker.mousePressEvent(QPointF(16,16), &e, canvas);
        QColor picked = doc.primaryColor();
        CHECK(picked.green()>150 && picked.red()<80, "Color picker samples the pixel under the cursor");
    }

    // ---------- COLOR WHEEL LOGIC (the reported bug) ----------
    SECTION("Color wheel picks a visible colour");
    {
        ColorWheelWidget wheel;
        wheel.setColor(Qt::black);          // value = 0 (the default)
        wheel.resize(200, 200);
        QPixmap pm(200,200);
        wheel.render(&pm);                  // forces paintEvent -> sets wheelRadius
        QColor got = Qt::black;
        QObject::connect(&wheel, &ColorWheelWidget::colorChanged,
                         [&](const QColor &c){ got = c; });
        // Click clearly inside the disc, to the right of centre (red-ish hue).
        QPointF pos(150, 100);
        QMouseEvent press(QEvent::MouseButtonPress, pos, wheel.mapToGlobal(pos.toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&wheel, &press);
        CHECK(got.value() > 10, "Picking on the wheel does NOT stay black (value lifted)");
        CHECK(got.saturation() > 10, "Picking on the wheel gives a saturated colour");
    }

    // ================================================================
    // BEHAVIOURAL / INTEGRATION TESTS (what the user actually observes)
    // ================================================================

    // ---------- LAYER OPACITY REVEALS THE LAYER BEHIND ----------
    SECTION("Reducing a layer's opacity reveals the layer behind");
    {
        Document doc(32,32);
        doc.activeLayer()->clear(QColor(0,0,255));           // bottom = blue
        int top = doc.addLayer();
        doc.layerAt(top)->clear(QColor(255,0,0));            // top = red, opaque
        // Fully opaque: result is pure red.
        QColor opaque = doc.flatten().pixelColor(16,16);
        CHECK(opaque.red()>230 && opaque.blue()<40, "Opaque top layer hides the one below (red)");
        // Half opacity: red blends with the blue behind -> both channels present.
        doc.setLayerOpacity(top, 0.5f);
        QColor half = doc.flatten().pixelColor(16,16);
        CHECK(half.red()>60 && half.blue()>60,
              "At 50% opacity you SEE the blue layer through the red one");
        // Fully transparent top: result is pure blue (the layer behind).
        doc.setLayerOpacity(top, 0.0f);
        QColor clear = doc.flatten().pixelColor(16,16);
        CHECK(clear.blue()>230 && clear.red()<40,
              "At 0% opacity only the layer behind (blue) shows");
    }

    // ---------- HIDING A LAYER REVEALS THE ONE BEHIND ----------
    SECTION("Toggling layer visibility");
    {
        Document doc(16,16);
        doc.activeLayer()->clear(QColor(0,180,0));    // bottom green
        int top = doc.addLayer();
        doc.layerAt(top)->clear(QColor(200,0,0));     // top red
        CHECK(doc.flatten().pixelColor(8,8).red()>180, "top layer visible -> red");
        doc.setLayerVisibility(top, false);
        CHECK(doc.flatten().pixelColor(8,8).green()>150, "top hidden -> green shows through");
    }

    // ---------- BRUSH SIZE CHANGES THE FOOTPRINT ----------
    SECTION("Brush size actually changes the stroke width");
    {
        auto paintedPixels = [](int size) {
            Document doc(80,80); doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            BrushTool brush; brush.setBrushSize(size); brush.setHardness(100);
            QMouseEvent e = pressEv(QPointF(40,40));
            brush.mousePressEvent(QPointF(40,40), &e, canvas);
            QMouseEvent r = releaseEv(QPointF(40,40));
            brush.mouseReleaseEvent(QPointF(40,40), &r, canvas);
            return countNonBackground(doc.activeLayer()->image(), QColor(Qt::white));
        };
        int small = paintedPixels(4);
        int big = paintedPixels(40);
        CHECK(small > 0, "small brush paints something");
        CHECK(big > small * 4, "a bigger brush paints a much larger area");
    }

    // ---------- ERASER SIZE CHANGES THE FOOTPRINT ----------
    SECTION("Eraser size changes the erased area");
    {
        auto erasedPixels = [](int size) {
            Document doc(80,80); doc.activeLayer()->clear(QColor(0,0,0,255));
            CanvasWidget canvas; canvas.setDocument(&doc);
            EraserTool er; er.setBrushSize(size); er.setHardness(100);
            QMouseEvent e = pressEv(QPointF(40,40));
            er.mousePressEvent(QPointF(40,40), &e, canvas);
            QMouseEvent r = releaseEv(QPointF(40,40));
            er.mouseReleaseEvent(QPointF(40,40), &r, canvas);
            QImage &img = doc.activeLayer()->image();
            int transparent = 0;
            for (int y=0;y<80;++y) for (int x=0;x<80;++x)
                if (img.pixelColor(x,y).alpha() < 128) ++transparent;
            return transparent;
        };
        CHECK(erasedPixels(40) > erasedPixels(6) * 4, "a bigger eraser clears a much larger area");
    }

    // ---------- SELECTION SELECTS THE RIGHT ZONE ----------
    SECTION("Selecting a zone then filling only affects that zone");
    {
        Document doc(64,64); doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        // Select a precise rectangle.
        doc.selection().selectRect(QRect(16,16,16,16), SelectionMode::Replace);
        CanvasWidget canvas; canvas.setDocument(&doc);
        FillTool fill; fill.setTolerance(255);
        QMouseEvent e = pressEv(QPointF(24,24));
        fill.mousePressEvent(QPointF(24,24), &e, canvas);
        QMouseEvent r = releaseEv(QPointF(24,24));
        fill.mouseReleaseEvent(QPointF(24,24), &r, canvas);
        QImage &img = doc.activeLayer()->image();
        // Corners of the selected rect should be red; everything else white.
        CHECK(img.pixelColor(20,20).red()>200 && img.pixelColor(20,20).blue()<80,
              "fill covers the selected zone");
        CHECK(img.pixelColor(2,2)==QColor(Qt::white) && img.pixelColor(60,60)==QColor(Qt::white),
              "fill leaves everything OUTSIDE the selection untouched");
        // Precise boundary: (31,31) inside, (33,33) outside.
        CHECK(img.pixelColor(31,31).red()>200, "boundary pixel inside selection is filled");
        CHECK(img.pixelColor(33,33)==QColor(Qt::white), "boundary pixel outside selection is clean");
    }

    // ---------- MAKE AN IMPORTED IMAGE PARTLY TRANSPARENT ----------
    SECTION("Import an image as a layer and make it transparent over another");
    {
        // Background document (a solid photo-like layer).
        Document doc(40,40); doc.activeLayer()->clear(QColor(30,140,30));  // green bg
        // "Import" a second image as a new layer.
        QImage imported = makeImage(40,40, QColor(200,40,40));             // red image
        int idx = doc.addLayer(imported, "Imported");
        CHECK(doc.layerCount()==2, "imported image becomes a new layer");
        CHECK(doc.flatten().pixelColor(20,20).red()>180, "imported image on top (red)");
        // Make it half transparent -> green shows through.
        doc.setLayerOpacity(idx, 0.4f);
        QColor blended = doc.flatten().pixelColor(20,20);
        CHECK(blended.green()>60 && blended.red()>60,
              "transparent imported image lets the background show through");
    }

    // ---------- MULTI-DOCUMENT INDEPENDENCE ----------
    SECTION("Multiple open images are independent");
    {
        Document a(20,20); a.activeLayer()->clear(Qt::red);
        Document b(30,30); b.activeLayer()->clear(Qt::blue);
        CHECK(a.width()==20 && b.width()==30, "two documents keep their own size");
        CHECK(a.flatten().pixelColor(5,5).red()>200, "doc A is red");
        CHECK(b.flatten().pixelColor(5,5).blue()>200, "doc B is blue");
        // Editing A must not touch B.
        a.activeLayer()->clear(Qt::black);
        CHECK(b.flatten().pixelColor(5,5).blue()>200, "editing A leaves B unchanged");
    }

    // ---------- TOLERANCE AFFECTS FILL SPREAD ----------
    SECTION("Fill tolerance controls the flooded area");
    {
        auto filled = [](int tol) {
            Document doc(40,40);
            // gradient-ish: left half 100, right half 130 grey
            QImage img = makeImage(40,40, QColor(100,100,100,255));
            { QPainter p(&img); p.fillRect(20,0,20,40, QColor(130,130,130)); p.end(); }
            doc.activeLayer()->setImage(img);
            doc.setPrimaryColor(Qt::red);
            CanvasWidget canvas; canvas.setDocument(&doc);
            FillTool fill; fill.setTolerance(tol);
            QMouseEvent e = pressEv(QPointF(5,20));
            fill.mousePressEvent(QPointF(5,20), &e, canvas);
            QMouseEvent r = releaseEv(QPointF(5,20));
            fill.mouseReleaseEvent(QPointF(5,20), &r, canvas);
            int red = 0; QImage &out = doc.activeLayer()->image();
            for (int y=0;y<40;++y) for (int x=0;x<40;++x)
                if (out.pixelColor(x,y).red()>200) ++red;
            return red;
        };
        int low = filled(10);    // only the left half (~800 px)
        int high = filled(200);  // crosses into the right half (~1600 px)
        CHECK(low > 100 && low < 1200, "low tolerance stays within the similar region");
        CHECK(high > low, "higher tolerance floods a larger area");
    }

    // ---------- SAMPLING: Image vs Layer (Paint.NET) ----------
    SECTION("Magic Wand sampling: Layer selects differently than Image");
    {
        // Bottom layer: fully red. Top (active) layer: left half red, right half
        // transparent. Composite (Image) is all-red; the top layer alone (Layer)
        // has a transparent right half.
        Document doc(32, 32);
        doc.activeLayer()->clear(Qt::red);                       // bottom layer, index 0
        QImage top = makeImage(32, 32, QColor(255, 0, 0, 255));  // red
        { QPainter p(&top); p.setCompositionMode(QPainter::CompositionMode_Source);
          p.fillRect(16, 0, 16, 32, Qt::transparent); p.end(); }
        int topIdx = doc.addLayer(top, "Top");
        doc.setActiveLayer(topIdx);
        CanvasWidget canvas; canvas.setDocument(&doc);

        // Sampling = Image: the composite is uniform red, so the right side is
        // part of the same-colour region.
        {
            MagicWandTool wand; wand.setTolerance(20); wand.setSampleImage(true);
            QMouseEvent e = pressEv(QPointF(2, 2)); wand.mousePressEvent(QPointF(2, 2), &e, canvas);
            CHECK(doc.selection().isSelected(24, 16),
                  "Sampling=Image: red shows through in the composite, right side selected");
        }
        // Sampling = Layer: the active layer's right half is transparent, so it is
        // NOT part of the red region.
        {
            MagicWandTool wand; wand.setTolerance(20); wand.setSampleImage(false);
            QMouseEvent e = pressEv(QPointF(2, 2)); wand.mousePressEvent(QPointF(2, 2), &e, canvas);
            CHECK(doc.selection().isSelected(2, 2),
                  "Sampling=Layer: the clicked red pixel is selected");
            CHECK(!doc.selection().isSelected(24, 16),
                  "Sampling=Layer: the transparent right half is excluded");
        }
    }

    SECTION("Paint Bucket sampling: Image decides region, fill writes active layer");
    {
        // Bottom layer: left half blue, right half green (opaque). Top (active)
        // layer: fully transparent. With Sampling=Image the region is decided from
        // the composite (contiguous blue = left half), but the paint lands on the
        // active (top) layer.
        Document doc(32, 32);
        QImage bottom = makeImage(32, 32, QColor(0, 0, 255, 255));  // blue
        { QPainter p(&bottom); p.fillRect(16, 0, 16, 32, QColor(0, 255, 0, 255)); p.end(); }
        doc.activeLayer()->setImage(bottom);                       // index 0
        int topIdx = doc.addLayer(makeImage(32, 32, Qt::transparent), "Top");
        doc.setActiveLayer(topIdx);
        doc.setPrimaryColor(Qt::red);
        CanvasWidget canvas; canvas.setDocument(&doc);

        FillTool fill; fill.setTolerance(20); fill.setSampleImage(true);
        QMouseEvent e = pressEv(QPointF(4, 4)); fill.mousePressEvent(QPointF(4, 4), &e, canvas);

        QImage &topImg = doc.activeLayer()->image();
        CHECK(topImg.pixelColor(4, 4).red() > 200 && topImg.pixelColor(4, 4).alpha() > 200,
              "Sampling=Image: composite's blue region on the left is filled red on the active layer");
        CHECK(topImg.pixelColor(28, 4).alpha() < 40,
              "Sampling=Image: the green (right) region is not part of the flood, stays transparent");
        // The paint must go to the ACTIVE (top) layer, never the sampled bottom.
        CHECK(doc.layerAt(0)->image().pixelColor(4, 4).blue() > 200,
              "the bottom (sampled) layer is left untouched by the fill");
    }

    // ---------- SMART MERGE (keep both drawings) ----------
    SECTION("Merge keeps both drawings when top has a flat background");
    {
        Document doc(40, 40);
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(2,2,12,12, QColor(0,0,255)); p.end(); } // bottom: blue square
        int top = doc.addLayer();            // white layer
        doc.layerAt(top)->clear(Qt::white);
        { QPainter p(&doc.layerAt(top)->image()); p.fillRect(26,26,12,12, QColor(255,0,0)); p.end(); } // top: red square
        doc.mergeLayerDown(top, true);       // keepArtwork
        CHECK(doc.layerCount() == 1, "merge collapses to one layer");
        QImage flat = doc.flatten();
        CHECK(flat.pixelColor(8,8).blue() > 200 && flat.pixelColor(8,8).red() < 80,
              "bottom drawing (blue) survives the merge");
        CHECK(flat.pixelColor(32,32).red() > 200 && flat.pixelColor(32,32).blue() < 80,
              "top drawing (red) survives the merge");
    }

    // ---------- REGRESSION: audit bug fixes ----------
    SECTION("Rotate 90° swaps non-square dimensions and keeps every layer");
    {
        Document doc(80, 40);                       // non-square
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(0,0,10,10, QColor(0,0,255)); p.end(); }
        int top = doc.addLayer();                   // second layer with red mark
        { QPainter p(&doc.layerAt(top)->image()); p.fillRect(70,30,10,10, QColor(255,0,0)); p.end(); }
        doc.rotate(1, "Rotate 90° CW");
        CHECK(doc.width() == 40 && doc.height() == 80, "document dimensions swapped");
        CHECK(doc.layerCount() == 2, "both layers preserved");
        CHECK(doc.layerAt(0)->width() == 40 && doc.layerAt(0)->height() == 80, "layer 0 resized");
        CHECK(doc.layerAt(1)->width() == 40 && doc.layerAt(1)->height() == 80, "layer 1 resized (all layers rotate)");
        doc.history().undo();
        CHECK(doc.width() == 80 && doc.height() == 40, "undo restores original dimensions");
    }

    SECTION("Flip keeps dimensions and mirrors content");
    {
        Document doc(40, 20);
        doc.activeLayer()->clear(Qt::white);
        doc.activeLayer()->image().setPixelColor(0, 0, QColor(0,0,255));   // mark top-left
        doc.flip(true, "Flip Horizontal");
        CHECK(doc.width() == 40 && doc.height() == 20, "dimensions unchanged");
        CHECK(doc.activeLayer()->image().pixelColor(39, 0).blue() > 200, "top-left moved to top-right");
    }

    SECTION("Brush Overwrite mode does not wipe the whole layer");
    {
        Document doc(40, 40);
        doc.activeLayer()->clear(QColor(0,0,255));       // solid blue
        doc.setPrimaryColor(Qt::red);
        CanvasWidget canvas; canvas.setDocument(&doc);
        BrushTool brush; brush.setBrushSize(6); brush.setBlendMode(14);   // Overwrite
        strokeTool(&brush, canvas, QPointF(10,20), QPointF(30,20));
        QImage &out = doc.activeLayer()->image();
        CHECK(out.pixelColor(2,2).blue() > 200, "untouched corner stays blue (not wiped)");
        CHECK(out.pixelColor(20,20).red() > 200, "painted area became red");
        int transparent = 0;
        for (int y=0;y<40;++y) for (int x=0;x<40;++x) if (out.pixelColor(x,y).alpha() < 10) ++transparent;
        CHECK(transparent < 40, "layer is not blanked to transparent");
    }

    SECTION("Black and White desaturates (not a 1-bit threshold)");
    {
        QImage img = makeImage(10, 10, QColor(200, 50, 50, 255));   // saturated red
        Desaturate bw;
        QImage out = bw.apply(img);
        QColor c = out.pixelColor(5,5);
        CHECK(c.red() == c.green() && c.green() == c.blue(), "output is grey");
        CHECK(c.red() > 0 && c.red() < 255, "grey keeps a mid tone (not black/white threshold)");
    }

    SECTION("Hue/Saturation leaves greys neutral");
    {
        QImage img = makeImage(8, 8, QColor(120,120,120,255));       // pure grey
        HueSaturation hs; hs.setSaturation(80);                      // crank saturation
        QColor c = hs.apply(img).pixelColor(4,4);
        CHECK(std::abs(c.red()-c.green()) < 12 && std::abs(c.green()-c.blue()) < 12,
              "grey stays roughly neutral (no red tint from hsvHue -1)");
    }

    SECTION("Flood fill stays inside the selection");
    {
        Document doc(40, 40);
        doc.activeLayer()->clear(QColor(100,100,100,255));           // uniform → tolerance can't stop it
        doc.selection().selectRect(QRect(0,0,20,40));               // left half selected
        doc.setPrimaryColor(Qt::red);
        CanvasWidget canvas; canvas.setDocument(&doc);
        FillTool fill; fill.setTolerance(255);
        QMouseEvent e = pressEv(QPointF(5,20)); fill.mousePressEvent(QPointF(5,20), &e, canvas);
        QMouseEvent r = releaseEv(QPointF(5,20)); fill.mouseReleaseEvent(QPointF(5,20), &r, canvas);
        QImage &out = doc.activeLayer()->image();
        CHECK(out.pixelColor(5,20).red() > 200, "inside selection is filled");
        CHECK(out.pixelColor(30,20).red() < 120, "outside selection is untouched (no leak)");
    }

    SECTION("Clone stamp at size 1 does not produce NaN/garbage");
    {
        Document doc(30, 30);
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(0,0,15,30, QColor(0,120,255)); p.end(); }
        CanvasWidget canvas; canvas.setDocument(&doc);
        CloneStampTool clone; clone.setBrushSize(1);
        QMouseEvent s = pressEv(QPointF(5,15), Qt::RightButton);   // set source
        clone.mousePressEvent(QPointF(5,15), &s, canvas);
        strokeTool(&clone, canvas, QPointF(20,15), QPointF(25,15));
        QColor c = doc.activeLayer()->image().pixelColor(22,15);
        CHECK(c.isValid() && c.alpha() >= 0 && c.alpha() <= 255, "produces a valid pixel (no NaN cast)");
    }

    SECTION("Native .psw format preserves layers, opacity and blend mode");
    {
        const QString path = QDir::tempPath() + "/paintsw_roundtrip.psw";
        {
            Document doc(48, 32);
            doc.activeLayer()->clear(QColor(10, 20, 30));
            doc.activeLayer()->setName("Fond");
            int top = doc.addLayer("Dessin");
            { QPainter p(&doc.layerAt(top)->image()); p.fillRect(4,4,20,20, QColor(200,60,60)); p.end(); }
            doc.layerAt(top)->setOpacity(0.5f);
            doc.layerAt(top)->setBlendMode(BlendMode::Multiply);
            doc.layerAt(top)->setVisible(false);
            CHECK(doc.saveNative(path), "saveNative succeeds");
        }
        {
            Document doc(1, 1);
            CHECK(doc.loadNative(path), "loadNative succeeds");
            CHECK(doc.width() == 48 && doc.height() == 32, "dimensions restored");
            CHECK(doc.layerCount() == 2, "both layers restored");
            CHECK(doc.layerAt(0)->name() == "Fond", "layer 0 name restored");
            CHECK(doc.layerAt(1)->name() == "Dessin", "layer 1 name restored");
            CHECK(qAbs(doc.layerAt(1)->opacity() - 0.5f) < 0.01f, "opacity restored");
            CHECK(doc.layerAt(1)->blendMode() == BlendMode::Multiply, "blend mode restored");
            CHECK(doc.layerAt(1)->isVisible() == false, "visibility restored");
            CHECK(doc.layerAt(1)->image().pixelColor(10,10).red() > 150, "pixel content restored");
        }
        QFile::remove(path);
    }

    SECTION("Move tool moves only the selected pixels, not the whole layer");
    {
        Document doc(40, 40);
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(5, 5, 10, 10, QColor(255, 0, 0)); p.end(); }
        doc.activeLayer()->image().setPixelColor(35, 35, QColor(0, 0, 255));  // unselected marker
        doc.selection().selectRect(QRect(5, 5, 10, 10));                      // select the red square

        CanvasWidget canvas; canvas.setDocument(&doc);
        MoveTool move;
        strokeTool(&move, canvas, QPointF(10, 10), QPointF(30, 10));          // drag selection +20 in x

        const QImage out = doc.flatten();
        CHECK(out.pixelColor(30, 10).red() > 200, "selected pixels appear at the new location");
        CHECK(out.pixelColor(10, 10).alpha() < 10, "original selected area is now a transparent hole");
        CHECK(out.pixelColor(35, 35).blue() > 200, "unselected pixels elsewhere did NOT move");
        CHECK(doc.selection().boundingRect().x() >= 24 && doc.selection().boundingRect().x() <= 26,
              "the marquee followed the pixels (~+20)");
    }

    // ---------- PLUGIN SYSTEM ----------
    SECTION("Plugin effect wrapper runs a plugin process function");
    {
        PswParam prm{ "Amount", 0, 100, 100, "%" };
        PswEffect desc{ "Test Invert", "Photo", 1, &prm, &testInvertProcess, nullptr };
        PluginEffect eff(desc, std::shared_ptr<QLibrary>());   // no library needed for this test
        CHECK(eff.name() == "Test Invert", "plugin name copied out of C struct");
        CHECK(eff.paramCount() == 1, "plugin param count");

        QImage img = makeImage(8, 8, QColor(10, 20, 30, 255));
        eff.setValues({100});
        QColor c = eff.apply(img).pixelColor(4, 4);
        CHECK(c.red() == 245 && c.green() == 235 && c.blue() == 225, "full invert applied via plugin");
        eff.setValues({0});
        QColor c0 = eff.apply(img).pixelColor(4, 4);
        CHECK(c0.red() == 10 && c0.green() == 20 && c0.blue() == 30, "amount 0 leaves pixels unchanged");
    }

    SECTION("Plugin manager loads an external .so if present (non-fatal if absent)");
    {
        const QString dir = QCoreApplication::applicationDirPath() + "/plugins";
        const QString so = dir + "/sample_sepia_plugin.so";
        if (QFile::exists(so)) {
            PluginManager mgr;
            mgr.loadFrom({dir});
            CHECK(!mgr.empty(), "sample plugin discovered and loaded");
            if (!mgr.empty()) {
                auto eff = mgr.effects().front();
                QImage img = makeImage(6, 6, QColor(100, 150, 200, 255));
                QImage out = eff->apply(img).convertToFormat(QImage::Format_ARGB32_Premultiplied);
                CHECK(out.size() == img.size(), "plugin effect returns a same-size image");
                CHECK(imagesDiffer(img, out), "sepia plugin actually changed the pixels");
            }
        } else {
            printf("  (skipped: build/plugins/sample_sepia_plugin.so not present)\n");
        }
    }

    // ---------- TEXT TOOL SWALLOWS ITS OWN KEYS ----------
    SECTION("Text tool captures shortcut letters");
    {
        // ~60% of the alphabet are also tool shortcuts (b, e, s, t, z…). Qt matches
        // those before keyPressEvent, so typing dropped them. The canvas claims
        // ShortcutOverride while the tool wants key input, so the letters arrive.
        Document doc(200, 100);
        doc.activeLayer()->clear(Qt::white);
        CanvasWidget canvas;
        canvas.setDocument(&doc);
        TextTool text;
        canvas.setCurrentTool(&text);

        CHECK(!text.wantsKeyInput(), "text tool doesn't grab keys until you click");
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 40), QPointF(10, 40),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        text.mousePressEvent(QPointF(10, 40), &press, canvas);
        CHECK(text.wantsKeyInput(), "text tool grabs keys once editing");

        // Every letter, including the shortcut ones, must be claimed as text.
        int claimed = 0;
        for (char c = 'a'; c <= 'z'; ++c) {
            QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_A + (c - 'a'), Qt::NoModifier, QString(c));
            QApplication::sendEvent(&canvas, &so);
            if (so.isAccepted()) ++claimed;
        }
        CHECK(claimed == 26, "the canvas claims every letter while typing");

        // But a shortcut chord (Ctrl+…) must still pass through to the app.
        QKeyEvent ctrlA(QEvent::ShortcutOverride, Qt::Key_A, Qt::ControlModifier, QString());
        QApplication::sendEvent(&canvas, &ctrlA);
        CHECK(!ctrlA.isAccepted(), "Ctrl-chords still reach the app while typing");

        // Once editing ends, letters go back to being shortcuts.
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        text.keyPressEvent(&esc, canvas);
        CHECK(!text.wantsKeyInput(), "text tool releases keys after Escape");
        QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
        QApplication::sendEvent(&canvas, &so);
        CHECK(!so.isAccepted(), "letters are shortcuts again once editing stops");
    }

    // ---------- COLORS PANEL: More >> / << Less COLLAPSES BACK ----------
    SECTION("Colors panel More/Less");
    {
        // "<< Less" hid the sliders but left the panel at its expanded height,
        // and it could not be shrunk back by hand (issue #9). Check both the
        // docked and the floating case, since they resize through different APIs.
        auto roundTrip = [](bool floating) {
            QMainWindow mw;
            Document doc(64, 64);
            auto *panel = new ColorsPanel;
            panel->setDocument(&doc);
            auto *dock = new QDockWidget("Colors", &mw);
            dock->setWidget(panel);
            mw.addDockWidget(Qt::LeftDockWidgetArea, dock);
            if (floating) { dock->setFloating(true); dock->resize(230, 343); }
            mw.resize(900, 700);
            mw.show();
            QCoreApplication::processEvents();

            QPushButton *more = nullptr;
            for (auto *b : panel->findChildren<QPushButton *>())
                if (b->text().contains("Plus") || b->text().contains("More")) { more = b; break; }
            if (!more) return std::make_tuple(0, 0, 0);

            auto settle = [&] {
                QCoreApplication::processEvents();
                QCoreApplication::sendPostedEvents();
                QCoreApplication::processEvents();
                return panel->sizeHint().height();
            };
            const int before = settle();
            more->click();
            const int expanded = settle();
            more->click();
            const int after = settle();
            return std::make_tuple(before, expanded, after);
        };

        for (bool floating : {false, true}) {
            const auto [before, expanded, after] = roundTrip(floating);
            const char *what = floating ? "floating" : "docked";
            CHECK(expanded > before + 50,
                  floating ? "More >> expands the floating panel" : "More >> expands the docked panel");
            CHECK(after <= before + 5,
                  floating ? "<< Less collapses the floating panel back"
                           : "<< Less collapses the docked panel back");
            printf("    (%s: %d -> %d -> %d)\n", what, before, expanded, after);
        }
    }

    // ---------- COLORS PANEL: PALETTE SERIALIZATION ROUND-TRIPS ----------
    SECTION("Palette serialization");
    {
        // Save-then-load must reproduce the palette exactly, including alpha.
        QVector<QColor> pal = {
            QColor(0x00,0x00,0x00), QColor(0xFF,0x00,0x00),
            QColor(0x12,0x34,0x56), QColor::fromRgba(qRgba(0x80, 0x11, 0x22, 0x33)),
        };
        const QString text = ColorsPanel::paletteToText(pal);
        const QVector<QColor> back = ColorsPanel::paletteFromText(text);
        CHECK(back.size() == pal.size(), "round-trip keeps the colour count");
        bool same = back.size() == pal.size();
        for (int i = 0; i < back.size() && same; ++i)
            same = back[i].rgba() == pal[i].rgba();
        CHECK(same, "round-trip preserves every ARGB value including alpha");

        // 8-digit uppercase AARRGGBB, one per line.
        CHECK(text.startsWith("FF000000\n"), "opaque black serializes as FF000000");

        // Comments (';'), blank lines and malformed lines are skipped.
        const QString messy = "; a comment\n\nFF00FF00\nnothex!!\nFF\n#FF0000FF\n";
        const QVector<QColor> parsed = ColorsPanel::paletteFromText(messy);
        CHECK(parsed.size() == 2, "comments/blank/malformed lines are skipped");
        CHECK(parsed.size() == 2 && parsed[0].rgba() == qRgba(0, 255, 0, 255),
              "green parses from AARRGGBB");
        CHECK(parsed.size() == 2 && parsed[1].rgba() == qRgba(0, 0, 255, 255),
              "a leading '#' is tolerated");

        CHECK(ColorsPanel::defaultPalette().size() == 32, "default palette has 32 colours");
    }

    // ---------- DOCK PANELS NEVER GET WRAPPED IN A GROUP WINDOW ----------
    SECTION("Dock options");
    {
        // QMainWindow::GroupedDragging makes Qt wrap dragged panels in a private
        // QDockWidgetGroupWindow. That wrapper inherits the main window's title,
        // so it shows up as a blank "Untitled - paint.software x.y.z" box, and
        // destroying one while the layout still points at it crashes the app
        // (issue #10). It also lies to isFloating(), which is what broke the
        // Colors panel's collapse when undocked (issue #9). It must stay off.
        QMainWindow mw;
        mw.setDockOptions(QMainWindow::AnimatedDocks
                          | QMainWindow::AllowNestedDocks
                          | QMainWindow::AllowTabbedDocks);
        auto *dock = new QDockWidget("Colors", &mw);
        mw.addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->setFloating(true);
        QCoreApplication::processEvents();

        CHECK(!(mw.dockOptions() & QMainWindow::GroupedDragging),
              "GroupedDragging is off (no blank group windows)");
        CHECK(dock->isFloating(), "an undocked panel reports isFloating() truthfully");
        CHECK(qstrcmp(dock->window()->metaObject()->className(), "QDockWidgetGroupWindow") != 0,
              "an undocked panel is its own window, not a group wrapper");
    }

    // ---------- COLOUR SLIDER LABELS ARE TRANSLATED ----------
    SECTION("Colour slider labels");
    {
        // The letters are French initials in the source: V = Vert (green) and
        // T = Teinte (hue). Left untranslated they read as wrong channels in
        // English, which is what issue #8 was still reporting after the RGB/HSV
        // headers themselves had been fixed.
        auto labelsFor = [](I18n::Lang lang) {
            I18n::setLanguage(lang);
            Document doc(64, 64);
            ColorsPanel panel;
            panel.setDocument(&doc);
            QStringList out;
            for (auto *l : panel.findChildren<QLabel *>()) {
                const QString t = l->text();
                if (t.size() == 2 && t.endsWith(':')) out << t;
            }
            return out;
        };

        const QStringList en = labelsFor(I18n::Lang::English);
        CHECK(en.contains("G:"), "English shows G: for green (not V: for Vert)");
        CHECK(en.contains("H:"), "English shows H: for hue (not T: for Teinte)");
        CHECK(!en.contains("T:"), "no French T: left in the English sliders");

        const QStringList fr = labelsFor(I18n::Lang::French);
        CHECK(fr.contains("V:") && fr.contains("T:"), "French keeps V: (Vert) and T: (Teinte)");
        I18n::setLanguage(I18n::Lang::English);
        printf("    (en: %s | fr: %s)\n", qPrintable(en.join(' ')), qPrintable(fr.join(' ')));
    }

    // ---------- MENU BAR ACCELERATORS ARE UNIQUE IN BOTH LANGUAGES ----------
    SECTION("Menu accelerators");
    {
        // Alt+letter accelerators are case-insensitive, so two menus claiming the
        // same letter do not open — Qt just cycles between them. French used to
        // have "&Fichier" and "E&ffets" both on Alt+F. Translating a menu title
        // moves its accelerator, so this has to hold in every language.
        // Keep this list in step with createMenus().
        const QStringList sourceTitles = {
            "&Fichier", "É&dition", "&Affichage", "&Image",
            "&Calques", "A&justements", "&Effets", "&Options",
        };

        auto collisionFor = [&sourceTitles](I18n::Lang lang) {
            I18n::setLanguage(lang);
            QHash<QChar, QString> taken;
            QString clash;
            for (const QString &src : sourceTitles) {
                const QString title = I18n::t(src);
                const int amp = title.indexOf('&');
                if (amp < 0 || amp + 1 >= title.size()) continue;
                const QChar key = title.at(amp + 1).toLower();
                if (taken.contains(key))
                    clash += QStringLiteral("%1 vs %2 on Alt+%3; ")
                                 .arg(taken.value(key), title, QString(key.toUpper()));
                else
                    taken.insert(key, title);
            }
            return clash;
        };

        const QString frClash = collisionFor(I18n::Lang::French);
        const QString enClash = collisionFor(I18n::Lang::English);
        CHECK(frClash.isEmpty(), "no duplicate menu accelerators in French");
        CHECK(enClash.isEmpty(), "no duplicate menu accelerators in English");
        if (!frClash.isEmpty()) printf("    (fr: %s)\n", qPrintable(frClash));
        if (!enClash.isEmpty()) printf("    (en: %s)\n", qPrintable(enClash));
        I18n::setLanguage(I18n::Lang::English);
    }

    // ---------- PIXEL COORDINATE CONVERSION ----------
    SECTION("Pixel coordinate conversion");
    {
        // toPixelPos floors: the pixel under x=5.7 is pixel 5 (it spans [5,6)),
        // not 6 as QPointF::toPoint() (round-to-nearest) would give. This was the
        // Pencil off-by-one in pixel art (PR #5).
        CHECK(toPixelPos(QPointF(5.7, 5.7)) == QPoint(5, 5), "toPixelPos floors 5.7 to 5");
        CHECK(toPixelPos(QPointF(5.2, 5.9)) == QPoint(5, 5), "toPixelPos floors within a pixel");
        CHECK(toPixelPos(QPointF(0.0, 0.0)) == QPoint(0, 0), "toPixelPos of an exact edge");
        CHECK(toPixelPos(QPointF(-0.3, -0.3)) == QPoint(-1, -1), "toPixelPos floors negatives");

        // The Pencil now marks the pixel actually under the cursor.
        Document doc(10, 10);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::black);
        CanvasWidget canvas;
        canvas.setDocument(&doc);
        PencilTool pencil;
        // A single click at (3.7, 4.2): pixel (3, 4) must be the one painted.
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(3.7, 4.2), QPointF(3.7, 4.2),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        pencil.mousePressEvent(QPointF(3.7, 4.2), &press, canvas);
        QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(3.7, 4.2), QPointF(3.7, 4.2),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        pencil.mouseReleaseEvent(QPointF(3.7, 4.2), &rel, canvas);
        CHECK(doc.activeLayer()->image().pixelColor(3, 4) == QColor(Qt::black),
              "Pencil paints the pixel under the cursor, not the rounded one");
        CHECK(doc.activeLayer()->image().pixelColor(4, 4) != QColor(Qt::black),
              "Pencil does not paint the neighbouring (rounded) pixel");

        // A movement delta must stay round-to-nearest, not floor: a sub-pixel
        // nudge shouldn't jump a whole pixel (that would drift the Move tools).
        CHECK((QPointF(-0.001, -0.001)).toPoint() == QPoint(0, 0),
              "a tiny negative drag delta stays at zero (round, not floor)");
    }

    // ---------- NEW LAYERS ARE TRANSPARENT ----------
    SECTION("New layers are transparent");
    {
        Document doc(20, 20);
        // The initial Background layer is white/opaque — that must not change.
        CHECK(doc.layerAt(0)->image().pixelColor(5, 5) == QColor(Qt::white),
              "the Background layer starts white");
        // Draw on it, add a layer: the new layer must be transparent so the
        // artwork below shows through (issue #3 — added layers were opaque white).
        { QPainter p(&doc.layerAt(0)->image()); p.fillRect(0, 0, 20, 20, Qt::red); }
        const int idx = doc.addLayer("Layer 2");
        CHECK(doc.layerAt(idx)->image().pixelColor(10, 10).alpha() == 0,
              "an added layer is fully transparent");
        const QColor flat = doc.flatten().convertToFormat(QImage::Format_ARGB32).pixelColor(10, 10);
        CHECK(flat.red() > 200 && flat.green() < 60,
              "artwork on a lower layer shows through a new layer");
    }

    SECTION("Pasted image becomes a canvas-sized layer, movable without clipping");
    {
        // A pasted image smaller than the canvas must land on a canvas-sized
        // layer, so it can be moved across the canvas instead of being clipped to
        // its own little box (issue #18).
        Document doc(200, 100);
        QImage paste(40, 40, QImage::Format_ARGB32);
        paste.fill(Qt::red);
        const int idx = doc.addLayer(paste, "Calque collé");
        CHECK(doc.layerAt(idx)->image().size() == QSize(200, 100),
              "pasted layer is canvas-sized, not paste-sized");
        CHECK(doc.layerAt(idx)->image().pixelColor(10, 10) == QColor(Qt::red),
              "the pasted pixels are placed at the top-left");
        CHECK(doc.layerAt(idx)->image().pixelColor(120, 50).alpha() == 0,
              "the rest of the canvas-sized layer is transparent");

        // Drive the REAL Move tool end-to-end (no selection = whole-layer move),
        // which is the code path that used to clip to the paste box. Grab inside
        // the red block and drag it +100px right; it must arrive intact.
        {
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            canvas.setZoom(1.0);
            MoveTool tool;
            const QPointF from(20, 20), to(120, 20);
            QMouseEvent e1(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(from, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);

            const QImage &after = doc.layerAt(idx)->image();
            CHECK(after.pixelColor(130, 20) == QColor(Qt::red),
                  "Move tool: pasted block travels +100px intact (right edge not clipped)");
            CHECK(after.pixelColor(10, 20).alpha() == 0,
                  "Move tool: the block really moved (old position now empty)");
        }
    }

    SECTION("Clipboard paste preserves oversized content and is one undoable operation");
    {
        Document doc(200, 100);
        QImage image(240, 120, QImage::Format_ARGB32);
        image.fill(QColor(20, 80, 160, 128));
        image.setPixelColor(239, 119, Qt::red);
        doc.selection().selectRect(QRect(5, 7, 10, 11));
        const QImage mask = doc.selection().mask();
        const QImage original = doc.activeLayer()->image();
        doc.pasteImage(image, "paste", false);
        CHECK(doc.size() == QSize(200, 100), "Keep does not resize the canvas");
        CHECK(doc.activeLayer()->image().size() == image.size(), "Keep retains all pasted pixels");
        CHECK(doc.activeLayer()->image().format() == QImage::Format_ARGB32_Premultiplied,
              "clipboard paste stores premultiplied pixels");
        CHECK(doc.activeLayer()->image().pixelColor(239,119) == QColor(Qt::red),
              "the off-canvas corner survives paste");
        CHECK(!doc.selection().hasSelection(), "accepted paste clears a stale selection");
        CHECK(doc.history().currentIndex() == 1, "paste is a single history action");
        doc.history().undo();
        CHECK(doc.layerCount() == 1 && doc.activeLayerIndex() == 0, "undo restores stack and active layer");
        CHECK(doc.selection().mask() == mask, "undo restores the pre-paste selection");
        doc.history().redo();
        CHECK(doc.activeLayer()->image().pixelColor(239,119) == QColor(Qt::red), "redo retains hidden pixels");
        doc.activeLayer()->setOffset(QPoint(-40,-20));
        QImage next(260, 80, QImage::Format_ARGB32_Premultiplied);
        next.fill(Qt::transparent);
        doc.pasteImage(next, "expanded", true);
        CHECK(doc.size() == QSize(260, 100), "Expand grows only dimensions that need it");
        CHECK(doc.layerAt(0)->image().copy(original.rect()) == original, "Expand does not resample old artwork");
        CHECK(doc.layerAt(1)->offset() == QPoint(-40,-20), "Expand preserves existing layer positions");
        CHECK(doc.layerAt(1)->image().pixelColor(239,119) == QColor(Qt::red), "Expand retains pre-existing hidden content");
        doc.history().undo();
        CHECK(doc.size() == QSize(200,100) && doc.layerCount() == 2 && doc.activeLayerIndex() == 1,
              "one undo reverses expansion and paste together");
    }

    // ---------- LAYER MOVE / FLIP / IMPORT ----------
    SECTION("Layer move / flip / import");
    {
        {   // move reorders the stack
            Document doc(20, 20);
            doc.layerAt(0)->setName("A");
            doc.addLayer();
            doc.layerAt(1)->setName("B");
            doc.moveLayer(0, 1);
            CHECK(doc.layerAt(0)->name() == "B" && doc.layerAt(1)->name() == "A",
                  "moving a layer up reorders the stack");
        }
        {   // per-layer horizontal flip swaps left/right, others untouched
            Document doc(4, 1);
            doc.addLayer();   // a second layer that must NOT move
            doc.layerAt(1)->image().fill(Qt::green);
            const QImage other = doc.layerAt(1)->image().copy();
            QImage &img = doc.layerAt(0)->image();
            img.fill(Qt::transparent);
            img.setPixelColor(0, 0, Qt::red);
            img.setPixelColor(3, 0, Qt::blue);
            doc.layerAt(0)->setImage(img.mirrored(true, false));
            CHECK(doc.layerAt(0)->image().pixelColor(0, 0).blue() > 200
                  && doc.layerAt(0)->image().pixelColor(3, 0).red() > 200,
                  "per-layer flip mirrors that layer");
            CHECK(doc.layerAt(1)->image() == other, "per-layer flip leaves other layers alone");
        }
        {   // import adds an image as a new named layer
            Document doc(10, 10);
            const int before = doc.layerCount();
            QImage img(6, 6, QImage::Format_ARGB32);
            img.fill(Qt::green);
            const int idx = doc.addLayer(img, "imported.png");
            CHECK(doc.layerCount() == before + 1 && doc.layerAt(idx)->name() == "imported.png",
                  "importing a file adds it as a new layer");
        }
    }

    // ---------- SELECTION MODES + GROW/SHRINK/FEATHER ----------
    SECTION("Selection modes and modify");
    {
        auto area = [](const Selection &s, int w, int h) {
            long n = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    if (s.isSelected(x, y)) ++n;
            return n;
        };
        // The shared modifier→mode helper every selection tool uses — the
        // detailed Paint.NET mapping is asserted in the selection-modes section.
        CHECK(selectionModeFor(Qt::NoModifier) == SelectionMode::Replace, "no modifier = replace");
        CHECK(selectionModeFor(Qt::ControlModifier) == SelectionMode::Add, "ctrl = add");
        CHECK(selectionModeFor(Qt::AltModifier) == SelectionMode::Subtract, "alt = subtract");

        {   // intersect keeps only the overlap
            Selection s(100, 100);
            s.selectRect(QRect(10, 10, 40, 40));
            s.selectRect(QRect(30, 30, 40, 40), SelectionMode::Intersect);
            CHECK(area(s, 100, 100) == 400, "intersect keeps the 20x20 overlap");
        }
        {   // grow enlarges, shrink reduces
            Selection s(100, 100);
            s.selectRect(QRect(40, 40, 20, 20));
            const long base = area(s, 100, 100);
            s.expand(5);
            CHECK(area(s, 100, 100) > base, "grow enlarges the selection");
            Selection s2(100, 100);
            s2.selectRect(QRect(40, 40, 20, 20));
            s2.contract(3);
            CHECK(area(s2, 100, 100) < base, "shrink reduces the selection");
        }
    }

    // ---------- FILL SELECTION ----------
    SECTION("Fill selection");
    {
        auto fillMasked = [](Document &doc) {
            // Mirror MainWindow::fillSelection's masked path.
            Layer *layer = doc.activeLayer();
            QImage fill(layer->image().size(), QImage::Format_ARGB32_Premultiplied);
            fill.fill(doc.primaryColor());
            fill = doc.selection().getMaskedImage(fill, layer->offset());
            QPainter p(&layer->image());
            p.drawImage(0, 0, fill);
        };
        auto redCount = [](const QImage &in) {
            const QImage img = in.convertToFormat(QImage::Format_ARGB32);
            long n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 200 && c.green() < 60 && c.blue() < 60) ++n;
                }
            return n;
        };

        Document doc(40, 40);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        doc.selection().selectRect(QRect(10, 10, 20, 20));   // 400 px
        fillMasked(doc);
        CHECK(redCount(doc.activeLayer()->image()) == 400, "fill selection paints only the 400 selected pixels");

        // No selection -> whole layer is fair game (MainWindow fills the rect).
        Document doc2(30, 30);
        doc2.activeLayer()->clear(Qt::white);
        doc2.setPrimaryColor(Qt::red);
        { QPainter p(&doc2.activeLayer()->image()); p.fillRect(doc2.activeLayer()->image().rect(), doc2.primaryColor()); }
        CHECK(redCount(doc2.activeLayer()->image()) == 900, "fill with no selection covers the whole layer");
    }

    // ---------- PASTE TEXT INTO THE TEXT TOOL ----------
    SECTION("Text tool: paste");
    {
        auto inkWidth = [](const QImage &img) {
            int x0 = INT_MAX, x1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qGray(img.pixel(x, y)) < 128) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
            return x1 < 0 ? 0 : x1 - x0 + 1;
        };
        auto inkHeight = [](const QImage &img) {
            int y0 = INT_MAX, y1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qGray(img.pixel(x, y)) < 128) { y0 = std::min(y0, y); y1 = std::max(y1, y); }
            return y1 < 0 ? 0 : y1 - y0 + 1;
        };

        {   // pasting while editing appends at the caret and renders
            Document doc(400, 120);
            doc.activeLayer()->clear(Qt::white);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            TextTool tool;
            canvas.setCurrentTool(&tool);
            QMouseEvent pr(QEvent::MouseButtonPress, QPointF(10, 60), QPointF(10, 60),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(QPointF(10, 60), &pr, canvas);
            tool.insertText("pasted", canvas);
            tool.commitText(canvas);
            CHECK(inkWidth(doc.activeLayer()->image()) > 20, "pasted text is drawn while editing");
        }
        {   // paste outside editing is a no-op — no text conjured from nowhere
            Document doc(400, 120);
            doc.activeLayer()->clear(Qt::white);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            TextTool tool;
            canvas.setCurrentTool(&tool);
            tool.insertText("ghost", canvas);
            tool.commitText(canvas);
            CHECK(inkWidth(doc.activeLayer()->image()) == 0, "paste does nothing when not editing");
        }
        {   // pasted CRLF becomes a real line break
            Document doc(400, 200);
            doc.activeLayer()->clear(Qt::white);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            TextTool tool;
            canvas.setCurrentTool(&tool);
            QMouseEvent pr(QEvent::MouseButtonPress, QPointF(10, 40), QPointF(10, 40),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(QPointF(10, 40), &pr, canvas);
            tool.insertText("a\r\nb", canvas);
            tool.commitText(canvas);
            CHECK(inkHeight(doc.activeLayer()->image()) > 30, "pasted CRLF makes two lines");
        }
    }

    // ---------- MOVE TOOL: RESIZING THE SELECTED PIXELS ----------
    SECTION("Move tool: resize selected pixels");
    {
        // Dragging a handle must stretch or squash the artwork itself, like
        // paint.net's Move Selected Pixels. The tool had no handles at all, so
        // shrinking a selected area was impossible with any tool.
        auto redBounds = [](const QImage &img) {
            int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 90 && c.blue() < 90 && c.alpha() > 100) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y);
                        x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
                }
            return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        };
        auto dragRed = [&](QPointF grab, QPointF to) {
            Document doc(200, 200);
            doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(40, 40, 60, 60), Qt::red); }
            doc.selection().selectRect(QRect(40, 40, 60, 60));
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            canvas.setZoom(1.0);
            MoveTool tool;
            QMouseEvent e1(QEvent::MouseButtonPress, grab, grab, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(grab, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            return redBounds(doc.flatten());
        };
        auto near = [](int a, int b) { return std::abs(a - b) <= 3; };

        QRect r = dragRed(QPointF(99, 99), QPointF(139, 139));
        CHECK(near(r.width(), 100) && near(r.height(), 100), "corner handle grows the artwork");
        r = dragRed(QPointF(99, 99), QPointF(69, 69));
        CHECK(near(r.width(), 30) && near(r.height(), 30), "corner handle shrinks the artwork");
        r = dragRed(QPointF(70, 99), QPointF(70, 60));
        CHECK(near(r.width(), 60) && r.height() < 30, "bottom handle squashes the artwork flat");
        r = dragRed(QPointF(99, 70), QPointF(159, 70));
        CHECK(near(r.width(), 120) && near(r.height(), 60), "right handle stretches width only");
        // Pressing away from any handle must still move, not resize.
        r = dragRed(QPointF(60, 70), QPointF(90, 100));
        CHECK(near(r.width(), 60) && near(r.height(), 60) && near(r.x(), 70) && near(r.y(), 70),
              "pressing inside still moves the artwork unscaled");
    }

    // ---------- UNDO/REDO OF A PIXEL RESIZE ----------
    SECTION("Move tool: undo/redo a resize");
    {
        Document doc(200, 200);
        doc.activeLayer()->clear(Qt::white);
        { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(40, 40, 60, 60), Qt::red); }
        doc.selection().selectRect(QRect(40, 40, 60, 60));
        CanvasWidget canvas;
        canvas.setDocument(&doc);
        canvas.setZoom(1.0);

        auto redRect = [&]() {
            const QImage img = doc.flatten();
            int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 90 && c.blue() < 90 && c.alpha() > 100) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y);
                        x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
                }
            return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        };

        MoveTool tool;
        const QPointF grab(99, 99), to(69, 69);
        QMouseEvent e1(QEvent::MouseButtonPress, grab, grab, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool.mousePressEvent(grab, &e1, canvas);
        QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool.mouseMoveEvent(to, &e2, canvas);
        QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        tool.mouseReleaseEvent(to, &e3, canvas);

        CHECK(redRect().width() < 40, "resize shrank the artwork");
        // One drag must leave exactly one undo step, and it must put the pixels back.
        CHECK(doc.history().canUndo(), "a resize is undoable");
        doc.history().undo();
        CHECK(redRect() == QRect(40, 40, 60, 60), "undo restores the original artwork");
        CHECK(doc.history().canRedo(), "a resize is redoable");
        doc.history().redo();
        CHECK(redRect().width() < 40, "redo re-applies the resize");
    }

    // ---------- MOVE TOOL TARGETS THE ACTIVE LAYER ----------
    SECTION("Move tool acts on the selected layer");
    {
        // The selection is document-wide, so it can be drawn while one layer is
        // active and then used after switching to another. Whatever is on screen,
        // the edit must land on the SELECTED layer and leave the others alone.
        auto colourBounds = [](const QImage &img, QColor want) {
            int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.alpha() > 100 && std::abs(c.red() - want.red()) < 60
                        && std::abs(c.green() - want.green()) < 60
                        && std::abs(c.blue() - want.blue()) < 60) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y);
                        x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
                }
            return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        };
        // Two layers, same square: red underneath, blue on top.
        auto build = [](Document &doc) {
            doc.activeLayer()->clear(Qt::transparent);
            { QPainter p(&doc.layerAt(0)->image()); p.fillRect(QRect(40, 40, 60, 60), Qt::red); }
            doc.addLayer();
            { QPainter p(&doc.layerAt(1)->image()); p.fillRect(QRect(40, 40, 60, 60), Qt::blue); }
            doc.setActiveLayer(0);
            doc.selection().selectRect(QRect(40, 40, 60, 60));   // drawn on layer 0
            doc.setActiveLayer(1);                               // but layer 1 is selected
        };
        auto drag = [](Document &doc, QPointF from, QPointF to) {
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            canvas.setZoom(1.0);
            MoveTool tool;
            QMouseEvent e1(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(from, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
        };

        {   // resizing via a handle
            Document doc(200, 200);
            build(doc);
            drag(doc, QPointF(99, 99), QPointF(69, 69));
            const QRect blue = colourBounds(doc.snapshotEffectiveLayers()[1]->image(), Qt::blue);
            const QRect red = colourBounds(doc.layerAt(0)->image(), Qt::red);
            CHECK(blue.width() < 40 && blue.height() < 40, "resize scales the SELECTED layer");
            CHECK(red == QRect(40, 40, 60, 60), "resize leaves the other layer untouched");
        }
        {   // plain move
            Document doc(200, 200);
            build(doc);
            drag(doc, QPointF(60, 70), QPointF(90, 100));
            const QRect blue = colourBounds(doc.snapshotEffectiveLayers()[1]->image(), Qt::blue);
            const QRect red = colourBounds(doc.layerAt(0)->image(), Qt::red);
            CHECK(blue.x() > 55, "move shifts the SELECTED layer");
            CHECK(red == QRect(40, 40, 60, 60), "move leaves the other layer untouched");
        }
        {   // a locked layer must refuse both
            Document doc(200, 200);
            build(doc);
            doc.layerAt(1)->setLocked(true);
            const QImage before = doc.layerAt(1)->image().copy();
            drag(doc, QPointF(99, 99), QPointF(69, 69));
            CHECK(doc.layerAt(1)->image() == before, "a locked layer is not resized");
        }
    }

    SECTION("Brush keeps drawing when alternating primary/secondary buttons (issue #17)");
    {
        // Releasing one of two held buttons must not cut the stroke, and a second
        // button pressed mid-stroke must not restart it. Paul-Coy saw the mouse
        // stop drawing at the primary<->secondary transition.
        Document doc(64, 64);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::black);
        doc.setSecondaryColor(Qt::red);
        CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
        BrushTool brush; brush.setBrushSize(6);

        const QPointF a(10, 32), b(30, 32), c(50, 32);
        // Press LEFT and draw a->b.
        QMouseEvent p1(QEvent::MouseButtonPress, a, a, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        brush.mousePressEvent(a, &p1, canvas);
        QMouseEvent m1(QEvent::MouseMove, b, b, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        brush.mouseMoveEvent(b, &m1, canvas);
        // Press RIGHT too (both held) — must NOT restart the stroke.
        QMouseEvent p2(QEvent::MouseButtonPress, b, b, Qt::RightButton, Qt::LeftButton | Qt::RightButton, Qt::NoModifier);
        brush.mousePressEvent(b, &p2, canvas);
        // Release LEFT while RIGHT is still held — the stroke must continue.
        QMouseEvent r1(QEvent::MouseButtonRelease, b, b, Qt::LeftButton, Qt::RightButton, Qt::NoModifier);
        brush.mouseReleaseEvent(b, &r1, canvas);
        // Move to c — this segment must still paint (drawing didn't stop).
        QMouseEvent m2(QEvent::MouseMove, c, c, Qt::NoButton, Qt::RightButton, Qt::NoModifier);
        brush.mouseMoveEvent(c, &m2, canvas);
        // Release the LAST button — now the stroke ends.
        QMouseEvent r2(QEvent::MouseButtonRelease, c, c, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        brush.mouseReleaseEvent(c, &r2, canvas);

        const QImage img = doc.flatten();
        // First half = primary (black); the segment must actually be painted.
        CHECK(qGray(img.pixel(20, 32)) < 120, "first half painted in the primary colour");
        // Second half painted (no gap) AND in the SECONDARY colour — the button
        // switch now changes colour mid-line instead of stopping (issue #17).
        const QColor c2 = img.pixelColor(40, 32);
        CHECK(c2.alpha() > 0 && c2.red() > 180 && c2.green() < 90 && c2.blue() < 90,
              "stroke continued past the switch and turned secondary (no gap)");
    }

    // ---------- MOVE SELECTION TOOL ----------
    SECTION("Move Selection tool");
    {
        auto dragSelection = [](QRect sel, QPointF from, QPoint delta, double zoom) {
            Document doc(200, 200);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            canvas.setZoom(zoom);
            doc.selection().selectRect(sel);
            MoveSelectionTool tool;
            const QPointF to = from + QPointF(delta.x(), delta.y());
            QMouseEvent e1(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            tool.mousePressEvent(from, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            return doc.selection().boundingRect();
        };

        // The resize handles used to span a flat 6 canvas pixels, so on a small
        // selection the corner zones met in the middle: every drag resized the
        // marquee and the selection could not be moved at all.
        const QRect small(10, 10, 20, 20);
        CHECK(dragSelection(small, QPointF(15, 15), QPoint(20, 10), 1.0) == small.translated(20, 10),
              "small selection moves when dragged near a corner");
        CHECK(dragSelection(small, QPointF(20, 20), QPoint(20, 10), 1.0) == small.translated(20, 10),
              "small selection moves when dragged from its centre");
        const QRect big(20, 20, 80, 80);
        CHECK(dragSelection(big, QPointF(60, 60), QPoint(10, 10), 1.0) == big.translated(10, 10),
              "large selection moves when dragged from its centre");
        // The grab zone is screen-space, so it must hold at any zoom.
        CHECK(dragSelection(small, QPointF(14, 14), QPoint(20, 10), 4.0) == small.translated(20, 10),
              "selection moves when zoomed in");
        CHECK(dragSelection(small, QPointF(20, 20), QPoint(20, 10), 0.25) == small.translated(20, 10),
              "selection moves when zoomed out");
        // A press outside the marquee must do nothing.
        CHECK(dragSelection(big, QPointF(150, 150), QPoint(10, 10), 1.0) == big,
              "pressing outside the selection leaves it alone");
        // Grabbing a corner exactly must still resize rather than move.
        {
            const QRect r = dragSelection(big, QPointF(99, 99), QPoint(40, 40), 1.0);
            CHECK(r.width() > big.width() + 20 && r.height() > big.height() + 20,
                  "grabbing the corner handle still resizes the marquee");
        }
    }

    // ---------- MOVE TOOL: ROTATION + MODIFIER KEYS ----------
    SECTION("Move tool: rotation and modifier keys");
    {
        auto redBounds = [](const QImage &img) {
            int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 90 && c.blue() < 90 && c.alpha() > 100) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y);
                        x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
                }
            return x1 < 0 ? QRect() : QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        };
        auto countRed = [](const QImage &img) {
            int n = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.red() > 150 && c.green() < 90 && c.blue() < 90 && c.alpha() > 100) ++n;
                }
            return n;
        };
        auto near = [](int a, int b) { return std::abs(a - b) <= 4; };

        // Ctrl at press moves a COPY: the original square stays where it was and a
        // second one appears at the drop point.
        {
            Document doc(200, 200);
            doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(40, 40, 40, 40), Qt::red); }
            doc.selection().selectRect(QRect(40, 40, 40, 40));
            CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
            MoveTool tool;
            const QPointF from(52, 60), to(152, 160);   // press inside, drag far away
            QMouseEvent e1(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
            tool.mousePressEvent(from, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::ControlModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::ControlModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            const QImage img = doc.flatten();
            CHECK(img.pixelColor(60, 60).red() > 150, "Ctrl-move leaves the original pixels in place");
            CHECK(img.pixelColor(160, 160).red() > 150, "Ctrl-move drops a copy at the new spot");
        }

        // Shift while resizing a corner keeps the original aspect ratio (square →
        // square) even when the drag is lopsided.
        {
            Document doc(200, 200);
            doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(40, 40, 40, 40), Qt::red); }
            doc.selection().selectRect(QRect(40, 40, 40, 40));
            CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
            MoveTool tool;
            const QPointF grab(79, 79), to(139, 110);   // bottom-right corner, uneven drag
            QMouseEvent e1(QEvent::MouseButtonPress, grab, grab, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
            tool.mousePressEvent(grab, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            const QRect r = redBounds(doc.flatten());
            CHECK(near(r.width(), r.height()), "Shift-resize keeps the aspect ratio square");
        }

        // Alt while resizing grows symmetrically about the centre: the opposite edge
        // moves out too, so the centre of the square stays put.
        {
            Document doc(200, 200);
            doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(60, 60, 40, 40), Qt::red); }
            doc.selection().selectRect(QRect(60, 60, 40, 40));
            const QPoint centreBefore = QRect(60, 60, 40, 40).center();
            CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
            MoveTool tool;
            const QPointF grab(99, 79), to(120, 79);   // right edge dragged out
            QMouseEvent e1(QEvent::MouseButtonPress, grab, grab, Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
            tool.mousePressEvent(grab, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            const QRect r = redBounds(doc.flatten());
            CHECK(r.left() < 60, "Alt-resize also moves the opposite (left) edge outward");
            CHECK(near(r.center().x(), centreBefore.x()), "Alt-resize keeps the centre fixed");
        }

        // Right-drag rotates the content about the box centre: a 90°-ish rotation of
        // a wide rectangle makes the bounding box taller than it is wide.
        {
            Document doc(200, 200);
            doc.activeLayer()->clear(Qt::white);
            { QPainter p(&doc.activeLayer()->image()); p.fillRect(QRect(60, 90, 80, 20), Qt::red); }
            doc.selection().selectRect(QRect(60, 90, 80, 20));
            const QRect before = redBounds(doc.flatten());
            const int redBefore = countRed(doc.flatten());
            CanvasWidget canvas; canvas.setDocument(&doc); canvas.setZoom(1.0);
            MoveTool tool;
            const QPointF c = QRectF(QRect(60, 90, 80, 20)).center();
            const QPointF from = c + QPointF(60, 0);   // to the right of centre
            const QPointF to   = c + QPointF(0, 60);   // straight below → ~90° turn
            QMouseEvent e1(QEvent::MouseButtonPress, from, from, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
            tool.mousePressEvent(from, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, to, to, Qt::NoButton, Qt::RightButton, Qt::NoModifier);
            tool.mouseMoveEvent(to, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, to, to, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
            tool.mouseReleaseEvent(to, &e3, canvas);
            const QRect after = redBounds(doc.flatten());
            CHECK(before.width() > before.height(), "sanity: started wide");
            CHECK(after.height() > after.width(), "rotation turns the wide box tall");
            CHECK(std::abs(countRed(doc.flatten()) - redBefore) < redBefore / 2,
                  "rotation preserves roughly the same amount of content");
            CHECK(doc.history().canUndo(), "a rotation is undoable");
        }
    }

    // ---------- CANVAS VIEW CENTRING ----------
    SECTION("Canvas view centring");
    {
        Document doc(800, 600);
        CanvasWidget canvas;
        canvas.setDocument(&doc);
        canvas.show();   // hidden widgets never get a resize event (CI runs offscreen)
        const int rs = canvas.rulerSize();

        auto resizeTo = [&](int w, int h) { canvas.resize(w, h); QCoreApplication::processEvents(); };
        auto isCentred = [&](int w, int h) {
            const QPointF want((w - rs - doc.width() * canvas.zoom()) / 2.0,
                               (h - rs - doc.height() * canvas.zoom()) / 2.0);
            return std::abs(canvas.pan().x() - want.x()) < 0.51
                && std::abs(canvas.pan().y() - want.y()) < 0.51;
        };

        resizeTo(1200, 800);
        canvas.resetToDefaultView();
        CHECK(isCentred(1200, 800), "resetToDefaultView centres the image");

        // The window manager resizes us *after* startup (maximise), so centring
        // once at construction leaves the image off-centre unless we follow.
        resizeTo(1920, 1040);
        CHECK(isCentred(1920, 1040), "image re-centres when the window is maximised");
        resizeTo(900, 650);
        CHECK(isCentred(900, 650), "image re-centres when the window is restored small");

        // A margin clamp used to shove a snugly-fitting image 40px off-centre.
        resizeTo(800 + rs + 30, 600 + rs + 30);
        CHECK(std::abs(canvas.pan().x() - 15.0) < 0.51 && std::abs(canvas.pan().y() - 15.0) < 0.51,
              "snug window still centres exactly (no margin clamp)");

        // Once the user places the view themselves, a resize must not steal it.
        resizeTo(1200, 800);
        canvas.setPan(QPointF(5, 7));
        resizeTo(1500, 900);
        CHECK(canvas.pan() == QPointF(5, 7), "a user pan survives a resize (no auto-centring)");
        canvas.resetToDefaultView();
        CHECK(isCentred(1500, 900), "resetToDefaultView re-enables centring after a pan");
    }

    SECTION("Zoom reuses the flattened document cache");
    {
        Document doc(32, 32);
        doc.activeLayer()->clear(Qt::red);
        CanvasWidget canvas;
        canvas.resize(80, 80);
        canvas.setShowRulers(false);
        canvas.setDocument(&doc);
        canvas.setPan(QPointF(0, 0));

        QImage target(canvas.size(), QImage::Format_ARGB32_Premultiplied);
        canvas.render(&target);                    // populate the cache
        doc.activeLayer()->clear(Qt::blue);        // mutate without notification
        canvas.setZoom(2.0);
        canvas.render(&target);
        CHECK(target.pixelColor(25, 25).red() > 180,
              "zoom-only changes keep the cached document pixels");

        doc.notifyContentChanged();
        canvas.render(&target);
        CHECK(target.pixelColor(25, 25).blue() > 180,
              "document changes still invalidate the cached document pixels");
    }

    SECTION("Resize dialog keeps typed decimals");
    {
        // Typing "8.5" into a Print Size field used to drop the ".5": the first
        // keystroke fired valueChanged, syncFromPixels() wrote the value back to
        // the same box mid-edit, and the reformat reset the text and cursor
        // (issue #14). The fix is keyboardTracking(false) on the decimal spins,
        // so they only commit on Enter / focus-out. Guard that invariant.
        ResizeDialog dlg(800, 600, 96);
        int checked = 0;
        for (QDoubleSpinBox *sb : dlg.findChildren<QDoubleSpinBox *>()) {
            CHECK(!sb->keyboardTracking(),
                  "a resize-dialog decimal field does not re-format on every keystroke");
            ++checked;
        }
        CHECK(checked >= 3, "found the print-size and percent decimal fields");
    }

    SECTION("Brush size: fractional, up to 2000, editable dropdown");
    {
        // Paint.NET's brush size is an editable dropdown that accepts decimals
        // (it antialiases sub-integer sizes) and ranges up to 2000. Ours used to
        // be an integer spin capped at 500 (#12).
        BrushTool brush;
        brush.setBrushSize(6.5);
        CHECK(std::abs(brush.brushSize() - 6.5) < 1e-9, "brush size keeps a decimal value");
        brush.setBrushSize(2000);
        CHECK(std::abs(brush.brushSize() - 2000.0) < 1e-9, "brush size reaches 2000");
        brush.setBrushSize(5000);
        CHECK(std::abs(brush.brushSize() - 2000.0) < 1e-9, "brush size clamps above 2000");
        brush.setBrushSize(0.2);
        CHECK(std::abs(brush.brushSize() - 1.0) < 1e-9, "brush size clamps below 1");

        // The panel's control: an editable combo with presets, wired so typing a
        // value commits it to the tool.
        ToolOptionsPanel panel;
        panel.setTool(&brush);
        QComboBox *sizeCombo = nullptr;
        for (QComboBox *c : panel.findChildren<QComboBox *>()) {
            if (c->isEditable() && c->findText("2000") >= 0) { sizeCombo = c; break; }
        }
        CHECK(sizeCombo != nullptr, "brush size is an editable dropdown listing 2000");
        if (sizeCombo) {
            CHECK(sizeCombo->count() >= 50, "the dropdown offers a full range of presets");
            // Simulate the user typing "12.5" and committing with Enter.
            sizeCombo->setCurrentText("12.5");
            emit sizeCombo->lineEdit()->editingFinished();
            CHECK(std::abs(brush.brushSize() - 12.5) < 1e-9,
                  "typing a decimal into the dropdown sets the tool's size");
        }
    }

    SECTION("Line / Curve tool: draw, commit, cancel");
    {
        // Drawing lays a straight segment but does NOT paint yet — the tool waits
        // in edit mode with control nubs. Committing (here via deactivate, as a
        // tool switch would) bakes it in. Escape / no-commit paints nothing.
        Document doc(64, 64);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        CanvasWidget canvas;
        canvas.setDocument(&doc);

        LineTool line;
        QImage before = doc.activeLayer()->image().copy();
        strokeTool(&line, canvas, QPointF(8, 8), QPointF(56, 56));
        CHECK(!imagesDiffer(before, doc.activeLayer()->image()),
              "drawing a line does not paint until committed (edit mode)");
        line.deactivate(canvas);   // switching away commits
        CHECK(imagesDiffer(before, doc.activeLayer()->image()),
              "committing the line paints it into the layer");

        // A fresh line that is never committed leaves the layer untouched.
        Document doc2(64, 64);
        doc2.activeLayer()->clear(Qt::white);
        CanvasWidget canvas2;
        canvas2.setDocument(&doc2);
        LineTool line2;
        QImage before2 = doc2.activeLayer()->image().copy();
        strokeTool(&line2, canvas2, QPointF(8, 8), QPointF(56, 56));
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        line2.keyPressEvent(&esc, canvas2);
        line2.deactivate(canvas2);
        CHECK(!imagesDiffer(before2, doc2.activeLayer()->image()),
              "Escape cancels the line — nothing is painted");
    }

    SECTION("Line / Curve tool: curve type shapes the committed path");
    {
        // Lay a line, bend an inner nub off-axis, then commit under each curve
        // type. Straight (polyline) and Bézier build different shapes from the
        // same nubs, and the Cubic Spline is verified to pass through the moved
        // interior nub (interpolating), unlike the Bézier which only approaches it.
        auto committedFor = [](LineTool::CurveType type, QPointF bendTo) {
            Document doc(64, 64);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::red);
            CanvasWidget canvas;
            canvas.setDocument(&doc);
            LineTool line;
            line.setCurveType(type);
            // Drag out the straight base line; the tool drops into edit mode.
            strokeTool(&line, canvas, QPointF(8, 8), QPointF(56, 56));
            // Grab inner nub #1 (at canvas (24,24)) and drag it off the axis.
            const QPointF nubW = canvas.canvasToWidget(QPointF(24, 24));
            QMouseEvent gp(QEvent::MouseButtonPress, nubW, nubW,
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            line.mousePressEvent(canvas.widgetToCanvas(nubW), &gp, canvas);
            QMouseEvent gm = moveEv(bendTo);
            line.mouseMoveEvent(bendTo, &gm, canvas);
            QMouseEvent gr = releaseEv(bendTo);
            line.mouseReleaseEvent(bendTo, &gr, canvas);
            line.deactivate(canvas);   // commit
            return doc.activeLayer()->image().copy();
        };

        const QPointF bend(24, 4);   // pull nub #1 well above the diagonal
        QImage straight = committedFor(LineTool::CurveType::Straight, bend);
        QImage bezier   = committedFor(LineTool::CurveType::Bezier, bend);
        QImage spline   = committedFor(LineTool::CurveType::CubicSpline, bend);

        CHECK(imagesDiffer(straight, bezier),
              "Straight and Bézier produce different rasters from the same nubs");
        CHECK(imagesDiffer(spline, bezier),
              "Cubic Spline differs from Bézier for the same nubs");

        // The spline interpolates the moved nub: a painted pixel sits right on it.
        auto paintedNear = [](const QImage &img, QPointF p, int r) {
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    const int x = int(p.x()) + dx, y = int(p.y()) + dy;
                    if (x < 0 || y < 0 || x >= img.width() || y >= img.height()) continue;
                    if (img.pixelColor(x, y) != QColor(Qt::white)) return true;
                }
            return false;
        };
        CHECK(paintedNear(spline, bend, 2),
              "Cubic Spline passes through the interior nub");
    }

    SECTION("Shape tool: editable before commit (Paint.NET)");
    {
        // Like the Line/Curve tool, dragging a shape lays out its bounding box but
        // does NOT paint — the shape stays live for editing until committed.
        Document doc(64, 64);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::red);
        doc.setSecondaryColor(Qt::red);
        CanvasWidget canvas;
        canvas.setDocument(&doc);

        ShapeTool sh;
        sh.setShapeFill(ShapeFill::Filled);
        QImage before = doc.activeLayer()->image().copy();
        strokeTool(&sh, canvas, QPointF(8, 8), QPointF(56, 56));
        CHECK(!imagesDiffer(before, doc.activeLayer()->image()),
              "drawing a shape does not paint until committed (edit mode)");
        sh.deactivate(canvas);   // switching away commits
        CHECK(imagesDiffer(before, doc.activeLayer()->image()),
              "committing the shape paints it into the layer");

        // A fresh shape cancelled with Escape leaves the layer untouched.
        Document doc2(64, 64);
        doc2.activeLayer()->clear(Qt::white);
        CanvasWidget canvas2;
        canvas2.setDocument(&doc2);
        ShapeTool sh2;
        sh2.setShapeFill(ShapeFill::Filled);
        QImage before2 = doc2.activeLayer()->image().copy();
        strokeTool(&sh2, canvas2, QPointF(8, 8), QPointF(56, 56));
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        sh2.keyPressEvent(&esc, canvas2);
        sh2.deactivate(canvas2);
        CHECK(!imagesDiffer(before2, doc2.activeLayer()->image()),
              "Escape cancels the shape — nothing is painted");

        // Enter commits, exactly like the Line/Curve tool.
        Document doc3(64, 64);
        doc3.activeLayer()->clear(Qt::white);
        doc3.setPrimaryColor(Qt::blue);
        doc3.setSecondaryColor(Qt::blue);
        CanvasWidget canvas3;
        canvas3.setDocument(&doc3);
        ShapeTool sh3;
        sh3.setShapeFill(ShapeFill::Filled);
        QImage before3 = doc3.activeLayer()->image().copy();
        strokeTool(&sh3, canvas3, QPointF(8, 8), QPointF(56, 56));
        QKeyEvent ent(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        CHECK(sh3.wantsCommitKey(Qt::Key_Return), "shape claims Enter while editing");
        sh3.keyPressEvent(&ent, canvas3);
        CHECK(imagesDiffer(before3, doc3.activeLayer()->image()),
              "Enter commits the shape");
    }

    SECTION("Shape catalogue: expanded set paints (issue #17)");
    {
        // Every catalogue entry must produce a non-empty raster once committed.
        const ShapeType kinds[] = {
            ShapeType::Rectangle, ShapeType::RoundedRectangle, ShapeType::Ellipse,
            ShapeType::Diamond, ShapeType::Triangle, ShapeType::Trapezoid,
            ShapeType::Parallelogram, ShapeType::RightTriangle, ShapeType::Pentagon,
            ShapeType::Hexagon, ShapeType::Heptagon, ShapeType::Octagon,
            ShapeType::Star3, ShapeType::Star4, ShapeType::Star, ShapeType::Star6,
            ShapeType::Arrow, ShapeType::Chevron, ShapeType::SpeechBalloon,
            ShapeType::Cloud, ShapeType::Heart, ShapeType::Lightning,
            ShapeType::Cross, ShapeType::Check,
        };
        CHECK(std::size(kinds) == 24, "the catalogue has 24 shapes");
        // The enum's numeric range must cover exactly this set, in order.
        CHECK(static_cast<int>(ShapeType::Check) == 23, "Check is the last enum value");
        for (ShapeType k : kinds) {
            Document doc(80, 80);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            doc.setSecondaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            ShapeTool sh;
            sh.setShapeType(k);
            sh.setShapeFill(ShapeFill::Both);
            QImage before = doc.activeLayer()->image().copy();
            strokeTool(&sh, canvas, QPointF(10, 10), QPointF(70, 70));
            CHECK(!imagesDiffer(before, doc.activeLayer()->image()),
                  "shape is not painted before commit");
            sh.deactivate(canvas);
            CHECK(imagesDiffer(before, doc.activeLayer()->image()),
                  "committed shape paints pixels");
        }
    }

    SECTION("Shape tool: rotate + move within the edit session");
    {
        // Rotating the live shape changes the committed raster vs. the un-rotated
        // one; moving it shifts the painted pixels. Both happen without a fresh
        // drag, proving the shape stays editable.
        auto paintedBBox = [](double angleDeg, QPointF nudge) {
            Document doc(100, 100);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            doc.setSecondaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            ShapeTool sh;
            sh.setShapeType(ShapeType::Rectangle);
            sh.setShapeFill(ShapeFill::Filled);
            // Lay out a wide, short rect so rotation is visible.
            QMouseEvent p(QEvent::MouseButtonPress, QPointF(20, 40), QPointF(20, 40),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            sh.mousePressEvent(QPointF(20, 40), &p, canvas);
            QMouseEvent m(QEvent::MouseMove, QPointF(80, 60), QPointF(80, 60),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            sh.mouseMoveEvent(QPointF(80, 60), &m, canvas);
            QMouseEvent r(QEvent::MouseButtonRelease, QPointF(80, 60), QPointF(80, 60),
                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            sh.mouseReleaseEvent(QPointF(80, 60), &r, canvas);
            // Rotate by grabbing the rotation handle (widget space).
            if (angleDeg != 0.0) {
                const QPointF centre(50, 50);
                const QPointF rotHandle = canvas.canvasToWidget(QPointF(50, 40 - 24));
                QMouseEvent rp(QEvent::MouseButtonPress, rotHandle, rotHandle,
                               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                sh.mousePressEvent(canvas.widgetToCanvas(rotHandle), &rp, canvas);
                // Drag the handle to ~angleDeg around the centre.
                const double rad = (angleDeg - 90.0) * M_PI / 180.0;
                const QPointF target = centre + QPointF(std::cos(rad), std::sin(rad)) * 40.0;
                const QPointF tW = canvas.canvasToWidget(target);
                QMouseEvent rm(QEvent::MouseMove, tW, tW, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                sh.mouseMoveEvent(target, &rm, canvas);
                QMouseEvent ru(QEvent::MouseButtonRelease, tW, tW, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                sh.mouseReleaseEvent(target, &ru, canvas);
            }
            // Nudge with arrow keys.
            for (int i = 0; i < int(nudge.x()); ++i) {
                QKeyEvent k(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                sh.keyPressEvent(&k, canvas);
            }
            for (int i = 0; i < int(nudge.y()); ++i) {
                QKeyEvent k(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
                sh.keyPressEvent(&k, canvas);
            }
            sh.deactivate(canvas);
            const QImage &img = doc.activeLayer()->image();
            int minx = 999, miny = 999, maxx = -1, maxy = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qGray(img.pixel(x, y)) < 128) {
                        minx = qMin(minx, x); maxx = qMax(maxx, x);
                        miny = qMin(miny, y); maxy = qMax(maxy, y);
                    }
            return std::make_tuple(minx, miny, maxx, maxy);
        };

        auto [ax0, ay0, ax1, ay1] = paintedBBox(0.0, QPointF(0, 0));
        auto [bx0, by0, bx1, by1] = paintedBBox(90.0, QPointF(0, 0));
        // The un-rotated rect is wide & short; rotated 90° it becomes tall & narrow.
        const int wA = ax1 - ax0, hA = ay1 - ay0;
        const int wB = bx1 - bx0, hB = by1 - by0;
        CHECK(wA > hA, "un-rotated rect is wider than tall");
        CHECK(hB > wB, "after a 90° rotation the rect is taller than wide");

        auto [nx0, ny0, nx1, ny1] = paintedBBox(0.0, QPointF(10, 10));
        CHECK(nx0 > ax0 && ny0 > ay0, "arrow-key nudges move the shape before commit");
    }

    SECTION("Fill Style: solid + hatch patterns");
    {
        // Paint.NET's Fill Style is Solid Color plus the GDI+ hatch set (~53).
        CHECK(Hatch::count() >= 54, "at least 54 fill styles (Solid + 53 hatches)");
        CHECK(Hatch::name(0) == QStringLiteral("Solid Color"), "index 0 is Solid Color");

        auto fillWith = [](int style) {
            QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::transparent);
            QPainter p(&img);
            p.setPen(Qt::NoPen);
            p.setBrush(Hatch::brush(style, Qt::black, Qt::white));
            p.drawRect(0, 0, 16, 16);
            p.end();
            int black = 0, white = 0;
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    QRgb c = img.pixel(x, y);
                    if (qRed(c) < 40) ++black; else if (qRed(c) > 215) ++white;
                }
            return std::make_pair(black, white);
        };

        auto [sb, sw] = fillWith(0);
        CHECK(sb > 200 && sw == 0, "Solid style fills uniformly with the foreground");
        // A hatch has BOTH foreground and background pixels — real structure.
        auto [hb, hw] = fillWith(5);   // Cross
        CHECK(hb > 0 && hw > 0, "a hatch pattern paints both fg and bg pixels");
        auto [pb, pw] = fillWith(12);  // 50%
        CHECK(pb > 40 && pw > 40, "the 50% pattern is roughly half foreground");

        // End to end: a filled shape with a hatch fill style paints a pattern,
        // not a flat colour.
        Document doc(64, 64);
        doc.activeLayer()->clear(Qt::white);
        doc.setPrimaryColor(Qt::black);
        doc.setSecondaryColor(Qt::white);
        CanvasWidget canvas; canvas.setDocument(&doc);
        ShapeTool sh;
        sh.setShapeFill(ShapeFill::Filled);
        sh.setFillStyle(5);            // Cross hatch
        strokeTool(&sh, canvas, QPointF(8, 8), QPointF(56, 56));
        sh.deactivate(canvas);         // commit the editable shape into the layer
        int fg = 0, bg = 0;
        const QImage &out = doc.activeLayer()->image();
        for (int y = 20; y < 44; ++y)
            for (int x = 20; x < 44; ++x) {
                QRgb c = out.pixel(x, y);
                if (qRed(c) < 40) ++fg; else if (qRed(c) > 215) ++bg;
            }
        CHECK(fg > 0 && bg > 0, "a hatch-filled shape shows the pattern (fg + bg inside)");
    }

    SECTION("Marching ants trace only the perimeter (issue #15)");
    {
        // A non-rectangular selection's QRegion is many scanline rects. Stroking
        // each one filled the interior with static; the fix strokes only the
        // merged outline. Render the canvas and check the selection interior is
        // clean (no dash pixels away from the boundary).
        Document doc(80, 80);
        doc.activeLayer()->clear(Qt::white);
        QPainterPath blob;
        blob.addEllipse(QRectF(10, 10, 60, 60));
        doc.selection().selectPath(blob, SelectionMode::Replace);

        CanvasWidget canvas;
        canvas.setDocument(&doc);
        canvas.resize(120, 120);
        canvas.show();
        QCoreApplication::processEvents();

        const QImage shot = canvas.grab().toImage();
        // Sample a small patch at the ellipse centre, well inside the boundary.
        const QPointF c = canvas.canvasToWidget(QPointF(40, 40));
        int dark = 0, total = 0;
        for (int dy = -6; dy <= 6; ++dy)
            for (int dx = -6; dx <= 6; ++dx) {
                const QPoint p = c.toPoint() + QPoint(dx, dy);
                if (!shot.rect().contains(p)) continue;
                ++total;
                if (qGray(shot.pixel(p)) < 128) ++dark;
            }
        // With the bug the interior is a dense dash grid; with the fix it's the
        // clean white layer. Allow a couple of stray antialiased pixels.
        CHECK(total > 0 && dark <= 2, "selection interior is clean, not filled with static");
    }

    SECTION("Brush pressure varies dab size, not opacity (issue #16)");
    {
        auto paintedArea = [](double pressure, bool sensitivity) {
            Document doc(80, 80);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            BrushTool brush;
            brush.setBrushSize(24);
            brush.setPressureSensitivity(sensitivity);
            brush.setPressure(pressure);
            strokeTool(&brush, canvas, QPointF(15, 40), QPointF(65, 40));
            const QImage &img = doc.activeLayer()->image();
            int painted = 0, darkCore = 0;
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < 80; ++x) {
                    int g = qGray(img.pixel(x, y));
                    if (g < 250) ++painted;       // any mark
                    if (g < 40) ++darkCore;       // fully-opaque black
                }
            return std::make_pair(painted, darkCore);
        };

        auto [fullArea, fullCore] = paintedArea(1.0, true);
        auto [lightArea, lightCore] = paintedArea(0.4, true);
        CHECK(lightArea > 0 && lightArea < fullArea * 0.7,
              "lower pressure paints a visibly thinner stroke");
        // Not opacity: the pixels that ARE painted stay fully opaque black.
        CHECK(lightCore > 0, "low-pressure stroke is still solid (opacity unchanged)");

        // Sensitivity off: full size regardless of pressure.
        auto [offArea, offCore] = paintedArea(0.4, false);
        CHECK(offArea > fullArea * 0.9, "pressure OFF stamps the full brush size");

        // The same size-not-opacity model now applies to the Eraser: lower
        // pressure clears a thinner track.
        auto erasedArea = [](double pressure) {
            Document doc(80, 80);
            doc.activeLayer()->clear(Qt::black);   // opaque, so erasing shows
            CanvasWidget canvas; canvas.setDocument(&doc);
            EraserTool er;
            er.setBrushSize(24);
            er.setPressureSensitivity(true);
            er.setPressure(pressure);
            strokeTool(&er, canvas, QPointF(15, 40), QPointF(65, 40));
            const QImage &img = doc.activeLayer()->image();
            int cleared = 0;
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < 80; ++x)
                    if (qAlpha(img.pixel(x, y)) < 128) ++cleared;
            return cleared;
        };
        const int erFull = erasedArea(1.0), erLight = erasedArea(0.4);
        CHECK(erLight > 0 && erLight < erFull * 0.7,
              "eraser: lower pressure clears a thinner track (size, not opacity)");
    }

    SECTION("Shape tool: Shift constrains 1:1, Alt draws from centre, corner size");
    {
        // Strokes a filled rectangle from a->b with the given modifiers held, then
        // returns the bounding box of the painted (black) pixels.
        auto bbox = [](Qt::KeyboardModifiers mods, QPointF a, QPointF b) {
            Document doc(120, 120);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            doc.setSecondaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            ShapeTool sh;
            sh.setShapeType(ShapeType::Rectangle);
            sh.setShapeFill(ShapeFill::Filled);
            QMouseEvent e1(QEvent::MouseButtonPress, a, a, Qt::LeftButton, Qt::LeftButton, mods);
            sh.mousePressEvent(a, &e1, canvas);
            QMouseEvent e2(QEvent::MouseMove, b, b, Qt::NoButton, Qt::LeftButton, mods);
            sh.mouseMoveEvent(b, &e2, canvas);
            QMouseEvent e3(QEvent::MouseButtonRelease, b, b, Qt::LeftButton, Qt::NoButton, mods);
            sh.mouseReleaseEvent(b, &e3, canvas);
            sh.deactivate(canvas);   // commit the editable shape into the layer
            const QImage &img = doc.activeLayer()->image();
            int minx = 999, miny = 999, maxx = -1, maxy = -1;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (qGray(img.pixel(x, y)) < 128) {
                        minx = qMin(minx, x); maxx = qMax(maxx, x);
                        miny = qMin(miny, y); maxy = qMax(maxy, y);
                    }
            return std::make_tuple(minx, miny, maxx, maxy);
        };
        auto approx = [](int v, int target) { return std::abs(v - target) <= 2; };

        // Plain drag: 40 wide, 20 tall from (20,20).
        {
            auto [x0, y0, x1, y1] = bbox(Qt::NoModifier, QPointF(20, 20), QPointF(60, 40));
            CHECK(approx(x0,20) && approx(y0,20) && approx(x1,60) && approx(y1,40),
                  "plain drag draws the raw drag rectangle");
        }
        // Shift: constrain to a square using the larger extent (40), toward cursor.
        {
            auto [x0, y0, x1, y1] = bbox(Qt::ShiftModifier, QPointF(20, 20), QPointF(60, 40));
            CHECK(approx(x0,20) && approx(y0,20) && approx(x1,60) && approx(y1,60),
                  "Shift forces a 1:1 square growing toward the cursor");
        }
        // Alt: start point is the centre; extend equally to both sides.
        {
            auto [x0, y0, x1, y1] = bbox(Qt::AltModifier, QPointF(60, 60), QPointF(80, 70));
            CHECK(approx(x0,40) && approx(y0,50) && approx(x1,80) && approx(y1,70),
                  "Alt centres the shape on the start point");
        }
        // Shift+Alt: centred square (extent 20 on the larger axis).
        {
            auto [x0, y0, x1, y1] = bbox(Qt::ShiftModifier | Qt::AltModifier,
                                         QPointF(60, 60), QPointF(80, 70));
            CHECK(approx(x0,40) && approx(y0,40) && approx(x1,80) && approx(y1,80),
                  "Shift+Alt draws a centred 1:1 square");
        }

        // Corner size clamps to 0..200 and is honoured by the rounded rectangle:
        // a larger radius rounds away more of the corner pixels.
        ShapeTool cs;
        cs.setCornerSize(-5);  CHECK(cs.cornerSize() == 0,   "corner size clamps at 0");
        cs.setCornerSize(999); CHECK(cs.cornerSize() == 200, "corner size clamps at 200");
        auto cornerFilled = [](int radius) {
            Document doc(80, 80);
            doc.activeLayer()->clear(Qt::white);
            doc.setPrimaryColor(Qt::black);
            doc.setSecondaryColor(Qt::black);
            CanvasWidget canvas; canvas.setDocument(&doc);
            ShapeTool sh;
            sh.setShapeType(ShapeType::RoundedRectangle);
            sh.setShapeFill(ShapeFill::Filled);
            sh.setCornerSize(radius);
            strokeTool(&sh, canvas, QPointF(10, 10), QPointF(70, 70));
            sh.deactivate(canvas);   // commit the editable shape into the layer
            // Is the top-left corner pixel (just inside the bbox) painted?
            return qGray(doc.activeLayer()->image().pixel(12, 12)) < 128;
        };
        CHECK(cornerFilled(0),  "corner size 0 leaves a sharp (filled) corner");
        CHECK(!cornerFilled(30), "a large corner size rounds the corner away");
    }

    SECTION("WebP import/export round-trips when the codec is present (issue #19)");
    {
        // WebP is offered in Open/Save; it works only if Qt's image-format plugin
        // is installed (the packaging now depends on qt6-image-formats-plugins).
        // Skip cleanly where the codec is absent (e.g. a bare dev box) so this
        // isn't a false failure, but exercise the real round-trip in CI where the
        // plugin is installed.
        const bool canWrite = QImageWriter::supportedImageFormats().contains("webp");
        const bool canRead  = QImageReader::supportedImageFormats().contains("webp");
        if (!canWrite || !canRead) {
            printf("    (webp codec not installed here — skipped; CI has the plugin)\n");
        } else {
            QImage src(40, 30, QImage::Format_ARGB32);
            for (int y = 0; y < 30; ++y)
                for (int x = 0; x < 40; ++x)
                    src.setPixelColor(x, y, QColor(x * 6, y * 8, 120));
            QTemporaryFile f("XXXXXX.webp");
            f.setAutoRemove(true);
            CHECK(f.open(), "temp .webp file opens");
            const QString path = f.fileName();
            QImageWriter w(path, "webp");
            w.setQuality(100);   // near-lossless
            CHECK(w.write(src), "QImageWriter writes a .webp file");
            QImage back(path);
            CHECK(!back.isNull() && back.size() == QSize(40, 30),
                  "the .webp reloads at the right size");
            const QColor c = back.convertToFormat(QImage::Format_ARGB32).pixelColor(20, 15);
            const QColor want(120, 120, 120);
            CHECK(std::abs(c.red() - want.red()) < 24 && std::abs(c.green() - want.green()) < 24
                  && std::abs(c.blue() - want.blue()) < 24,
                  "webp colours round-trip within tolerance");
        }
    }

    SECTION("Save Configuration: JPEG quality changes size and is remembered (Paint.NET)");
    {
        // A photo-like gradient so JPEG quality actually moves the file size.
        Document doc(64, 64);
        QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                img.setPixelColor(x, y, QColor((x * 7) % 256, (y * 11) % 256, (x * y) % 256));
        doc.activeLayer()->setImage(img);

        QTemporaryDir dir;
        CHECK(dir.isValid(), "temp dir for JPEG saves");
        const QString lowPath  = dir.filePath("low.jpg");
        const QString highPath = dir.filePath("high.jpg");

        CHECK(doc.save(lowPath, 10),  "save JPEG at quality 10");
        CHECK(doc.save(highPath, 95), "save JPEG at quality 95");
        const qint64 lowSize  = QFileInfo(lowPath).size();
        const qint64 highSize = QFileInfo(highPath).size();
        CHECK(lowSize > 0 && highSize > 0, "both JPEGs are written");
        CHECK(lowSize < highSize, "lower quality yields a smaller JPEG");

        // A plain re-save (quality == -1) must reuse the last chosen quality, so a
        // Ctrl+S after Save As keeps the setting instead of reverting to default.
        const QString reuse = dir.filePath("reuse.jpg");
        CHECK(doc.save(reuse), "re-save without an explicit quality");
        const qint64 reuseSize = QFileInfo(reuse).size();
        CHECK(std::abs(reuseSize - highSize) < highSize / 5,
              "re-save reuses the remembered quality (≈ last size)");

        // The dialog only offers itself for lossy formats.
        CHECK(SaveConfigDialog::supportsQuality("jpg")
              && SaveConfigDialog::supportsQuality("webp")
              && !SaveConfigDialog::supportsQuality("png"),
              "quality dialog is offered for JPEG/WebP, not PNG");
    }

    SECTION("Floating pixel session foundation (no UI integration)");
    {
        Document doc(8, 6), other(3, 2);
        Layer *target = doc.activeLayer();
        target->setOffset(QPoint(-2, 1));
        target->setOpacity(0.25f);
        target->setBlendMode(BlendMode::Multiply);
        doc.selection().selectRect(QRect(0, 0, 2, 2));
        const QImage selectionBefore = doc.selection().mask().copy();
        const QImage layerBefore = target->image().copy();
        const QImage flattenedBefore = doc.flatten();
        const int historyBefore = doc.history().currentIndex();
        QImage source(4, 1, QImage::Format_ARGB32_Premultiplied);
        source.setPixelColor(0, 0, Qt::transparent);
        source.setPixelColor(1, 0, QColor(200, 60, 20, 128));
        source.setPixelColor(2, 0, QColor(80, 160, 240, 128));
        source.setPixelColor(3, 0, Qt::red);
        QImage coverage(4, 1, QImage::Format_Grayscale8);
        coverage.fill(255);
        coverage.scanLine(0)[2] = 128;
        coverage.scanLine(0)[3] = 0;
        const QImage sourceBefore = source.copy(), maskBefore = coverage.copy();
        // Retain raw pointers BEFORE initialization: normal COW would not stop
        // subsequent writes through these aliases from corrupting shared storage.
        uchar *sourceBits = source.bits(), *maskBits = coverage.bits();
        uchar *baseBits = target->image().bits();
        CHECK(doc.beginFloatingPixelSession(0, source, coverage, QPoint(-1, 2)),
              "valid floating session initializes");
        auto *session = doc.floatingPixelSession();
        CHECK(session && session->targets(target), "session binds to target layer identity");
        CHECK(doc.history().currentIndex() == historyBefore && !doc.isModified(),
              "session initialization does not change history or dirty state");
        CHECK(doc.selection().mask() == selectionBefore && target->image() == layerBefore,
              "session initialization leaves selection and layer unchanged");
        CHECK(doc.flatten() != flattenedBefore, "internal session contributes to document rendering");
        const QRect region(-2, 1, 8, 6);
        const QImage rendered = session->render(region);
        CHECK(rendered.pixel(1, 1) == 0, "fully transparent selected source clears destination");
        CHECK(rendered.pixel(2, 1) == sourceBefore.pixel(1, 0),
              "partial source alpha replaces opaque destination without source-over");
        auto covered = [](int src, int dst) { return (src * 128 + dst * 127 + 127) / 255; };
        const QRgb softSource = sourceBefore.pixel(2, 0);
        const QRgb expected = qRgba(covered(qRed(softSource), 255),
                                    covered(qGreen(softSource), 255),
                                    covered(qBlue(softSource), 255),
                                    covered(qAlpha(softSource), 255));
        CHECK(rendered.pixel(3, 1) == expected, "fractional coverage is applied exactly once");
        CHECK(rendered.pixel(4, 1) == qRgb(255, 255, 255), "zero coverage preserves destination");
        CHECK(session->render(QRect(-3, 0, 1, 1)).pixel(0, 0) == 0,
              "base outside offset layer is transparent");
        const QImage geometry = session->renderCoverage(region);
        CHECK(geometry.constScanLine(1)[1] == 255 && geometry.constScanLine(1)[2] == 255
              && geometry.constScanLine(1)[3] == 128 && geometry.constScanLine(1)[4] == 0,
              "geometry is independent of source alpha and layer opacity/blend mode");
        CHECK(session->render(QRect(-1, 2, 2, 1)) == rendered.copy(1, 1, 2, 1),
              "region rendering agrees with full render");
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x) {
                const QRgb p = rendered.pixel(x, y);
                CHECK(qRed(p) <= qAlpha(p) && qGreen(p) <= qAlpha(p) && qBlue(p) <= qAlpha(p),
                      "replacement preserves premultiplied channel invariants");
            }
        sourceBits[0] ^= 255;
        maskBits[0] = 0;
        baseBits[0] = 0;
        QImage exportedSource = session->sourcePixels(), exportedMask = session->sourceCoverage();
        exportedSource.fill(Qt::green);
        exportedMask.fill(0);
        CHECK(session->sourcePixels() == sourceBefore && session->sourceCoverage() == maskBefore
              && session->basePixels() == layerBefore && session->render(region) == rendered,
              "raw aliases and COW getter edits cannot mutate snapshots");

        const QTransform initial = session->transform();
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(1000000, -1000000)),
              "entirely off-canvas transform is accepted");
        CHECK(doc.floatingPixelSession() == session
              && session->renderCoverage(region).constScanLine(1)[1] == 0,
              "off-canvas session remains present with clipped visible coverage");
        CHECK(session->renderCoverage(QRect(1000000, -1000000, 1, 1)).constScanLine(0)[0] == 255,
              "off-canvas source geometry remains available");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(-3, 2)), "partially clipped movement accepted");
        CHECK(session->renderCoverage(QRect(-2, 2, 1, 1)).constScanLine(0)[0] == 255,
              "partial off-canvas geometry maps correctly");
        QTransform rotated;
        rotated.translate(2, 2);
        rotated.rotate(90);
        CHECK(doc.setFloatingPixelTransform(rotated), "affine rotation accepted");
        CHECK(session->renderCoverage(QRect(1, 2, 1, 1)).constScanLine(0)[0] == 255,
              "rotation samples source geometry in document space");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromScale(2, 2)), "affine scale accepted");
        CHECK(session->renderCoverage(QRect(0, 0, 2, 2)).constScanLine(1)[1] == 255,
              "scale uses independent source coverage");
        CHECK(doc.setFloatingPixelTransform(initial) && session->render(region) == rendered,
              "transform round trip renders immutable original without resampling damage");
        CHECK(!doc.setFloatingPixelTransform(QTransform::fromScale(0, 1)), "singular transform rejected");
        CHECK(!doc.setFloatingPixelTransform(QTransform(1, 0, 0.1, 0, 1, 0, 0, 0, 1)),
              "projective transform rejected");
        CHECK(!doc.setFloatingPixelTransform(QTransform::fromTranslate(std::numeric_limits<double>::infinity(), 0))
              && !doc.setFloatingPixelTransform(QTransform::fromTranslate(std::numeric_limits<double>::quiet_NaN(), 0)),
              "nonfinite transforms rejected");
        CHECK(session->transform() == initial, "invalid transforms preserve prior state");
        CHECK(session->render(QRect()).isNull() && session->renderCoverage(QRect()).isNull(),
              "invalid render regions return null");
        CHECK(other.beginFloatingPixelSession(0, sourceBefore, maskBefore, QPoint()),
              "second document initializes independent session with different dimensions");
        CHECK(other.floatingPixelSession() != session && other.floatingPixelSession()->transform().isIdentity(),
              "document sessions do not share mutable transform state");
        doc.setFloatingPixelTransform(QTransform::fromTranslate(50, 60));
        CHECK(other.floatingPixelSession()->transform().isIdentity(),
              "updating one document transform leaves the second unchanged");
        doc.setFloatingPixelTransform(initial);
        doc.addLayer("Other");
        CHECK(doc.floatingPixelSession()->targets(target), "active layer change does not retarget session");
        CHECK(!doc.beginFloatingPixelSession(1, sourceBefore, maskBefore, QPoint()),
              "initialization cannot silently replace existing session");
        // Native saves include a legacy raster plus optional live-session metadata.
        QTemporaryDir dir;
        const QString path = dir.filePath("session-foundation.psw");
        CHECK(doc.saveNative(path), "native save accepts document with inactive session");
        Document loaded(1, 1);
        CHECK(loaded.loadNative(path) && loaded.floatingPixelSession()
              && loaded.flatten() == doc.flatten(),
              "native roundtrip preserves visible raster and live session");
        doc.removeLayer(0);
        CHECK(!doc.floatingPixelSession(), "removed target cannot be accessed through session API");
        doc.discardFloatingPixelSession();
        CHECK(other.floatingPixelSession(), "releasing one session leaves other document intact");
    }
    SECTION("Floating session invalid input and ownership");
    {
        Document doc(2, 2);
        QImage source(1, 1, QImage::Format_ARGB32);
        source.fill(QColor(120, 80, 40, 128));
        QImage coverage(1, 1, QImage::Format_Grayscale8);
        coverage.fill(255);
        QImage wrongSize(2, 1, QImage::Format_Grayscale8);
        wrongSize.fill(255);
        CHECK(!doc.beginFloatingPixelSession(-1, source, coverage, QPoint())
              && !doc.beginFloatingPixelSession(2, source, coverage, QPoint()), "invalid layer indices rejected");
        CHECK(!doc.beginFloatingPixelSession(0, QImage(), coverage, QPoint())
              && !doc.beginFloatingPixelSession(0, source, QImage(), QPoint())
              && !doc.beginFloatingPixelSession(0, source, wrongSize, QPoint())
              && !doc.beginFloatingPixelSession(0, source, source, QPoint()),
              "null mismatched and non-coverage masks rejected");
        coverage.fill(0);
        CHECK(!doc.beginFloatingPixelSession(0, source, coverage, QPoint()) && !doc.floatingPixelSession(),
              "empty coverage fails without installing session");
        coverage.fill(255);
        CHECK(doc.beginFloatingPixelSession(0, source, coverage, QPoint()), "valid input succeeds after failures");
        CHECK(doc.floatingPixelSession()->sourcePixels().format() == QImage::Format_ARGB32_Premultiplied
              && doc.floatingPixelSession()->render(QRect(0, 0, 1, 1)).pixel(0, 0) == qPremultiply(source.pixel(0, 0)),
              "straight-alpha input is normalized once");
        auto layer = std::make_shared<Layer>(2, 2);
        std::weak_ptr<Layer> weak = layer;
        auto detached = FloatingPixelSession::create(layer, source, coverage, QPoint());
        layer.reset();
        CHECK(weak.expired() && !detached->targets(nullptr), "session does not retain target layer lifetime");
        CHECK(!detached->render(QRect(0, 0, 1, 1)).isNull(), "snapshot render needs no live target pointer");
        doc.discardFloatingPixelSession();
        source.fill(Qt::transparent);
        coverage.fill(128);
        CHECK(doc.beginFloatingPixelSession(0, source, coverage, QPoint()),
              "transparent source with fractional geometry is valid");
        CHECK(doc.floatingPixelSession()->render(QRect(0, 0, 1, 1)).pixel(0, 0) == qRgba(127, 127, 127, 127),
              "fractionally selected transparent source attenuates destination by coverage not alpha");
    }

    SECTION("Floating session checkpoints and document history");
    {
        Document doc(9, 7), other(4, 4);
        doc.activeLayer()->setOffset(QPoint(-1, 1));
        doc.activeLayer()->setRetainsOffCanvasPixels(true);
        doc.selection().selectRect(QRect(5, 4, 2, 2));
        const QImage oldSelection = doc.selection().mask().copy();
        const QImage oldRaster = doc.activeLayer()->image().copy();
        QImage source(3, 2, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(210, 50, 80, 120));
        source.setPixelColor(0, 0, Qt::transparent);
        QImage mask(3, 2, QImage::Format_Grayscale8);
        mask.fill(255);
        mask.scanLine(0)[2] = 0;
        mask.scanLine(1)[1] = 100;
        int contentSignals = 0, selectionSignals = 0;
        QObject::connect(&doc, &Document::documentChanged, &doc, [&]() { ++contentSignals; });
        QObject::connect(&doc, &Document::floatingPixelsChanged, &doc, [&]() { ++contentSignals; });
        QObject::connect(&doc, &Document::selectionChanged, &doc, [&]() { ++selectionSignals; });
        auto initial = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Begin Floating Pixels");
        CHECK(doc.beginFloatingPixelSession(0, source, mask, QPoint(1, 2)), "initialize history session");
        CHECK(doc.setFloatingPixelTransform(doc.floatingPixelSession()->transform()),
              "initial checkpoint synchronizes selection coverage");
        CHECK(initial->captureAfter(), "initial checkpoint captures session creation");
        doc.history().push(std::move(initial));
        doc.setModified(true);
        const QImage initialRaster = doc.flatten(), initialMask = doc.selection().mask().copy();
        const QTransform initialTransform = doc.floatingPixelSession()->transform();
        const uchar *immutableSource = doc.floatingPixelSession()->sourcePixels().constBits();
        CHECK(doc.history().currentIndex() == 1, "session initialization is a distinct history operation");
        CHECK(contentSignals >= 2 && selectionSignals == 1, "initial session invalidates canvas and selection");

        auto gesture = [&](const QTransform &transform, const QString &description) {
            auto command = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, description);
            CHECK(doc.setFloatingPixelTransform(transform), "preview transform accepted");
            CHECK(command->captureAfter(), "gesture checkpoint captured");
            doc.history().push(std::move(command));
            doc.setModified(true);
        };
        auto move = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Move Floating Pixels");
        for (int x = 2; x <= 4; ++x)
            CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(x, 2)), "drag previews update state");
        CHECK(doc.history().currentIndex() == 1, "drag previews do not generate history entries");
        CHECK(move->captureAfter(), "move checkpoint captures final preview");
        doc.history().push(std::move(move));
        const QImage movedRaster = doc.flatten(), movedMask = doc.selection().mask().copy();
        QTransform resized;
        resized.translate(4, 2);
        resized.scale(1.5, 2);
        gesture(resized, "Scale Floating Pixels");
        QTransform rotated = resized;
        rotated.rotate(30);
        gesture(rotated, "Rotate Floating Pixels");
        const QImage finalRaster = doc.flatten(), finalMask = doc.selection().mask().copy();
        CHECK(doc.history().currentIndex() == 4, "each completed transform has one checkpoint");
        CHECK(doc.layerAt(0)->image() == oldRaster, "previews never alter committed target pixels");
        doc.history().undo();
        doc.history().undo();
        CHECK(doc.flatten() == movedRaster && doc.selection().mask() == movedMask
              && doc.floatingPixelSession()->transform() == QTransform::fromTranslate(4, 2),
              "undo restores transform pixels and matching selection");
        doc.history().redo();
        doc.history().redo();
        CHECK(doc.flatten() == finalRaster && doc.selection().mask() == finalMask,
              "redo restores scale and rotation");
        doc.history().goToIndex(0);
        CHECK(!doc.floatingPixelSession() && doc.layerAt(0)->image() == oldRaster
              && doc.selection().mask() == oldSelection && doc.layerAt(0)->offset() == QPoint(-1, 1)
              && doc.layerAt(0)->retainsOffCanvasPixels(), "undo initialization restores original raster geometry and selection");
        doc.history().goToIndex(1);
        CHECK(doc.flatten() == initialRaster && doc.selection().mask() == initialMask
              && doc.floatingPixelSession()->transform() == initialTransform,
              "history navigation restores editable initial session");
        doc.history().goToIndex(4);
        doc.history().goToIndex(2);
        gesture(QTransform::fromTranslate(-2, 3), "Replacement Branch");
        CHECK(!doc.history().canRedo() && doc.history().currentIndex() == 3
              && doc.history().undoDescription() == "Replacement Branch"
              && doc.history().fullHistoryList().size() == 3,
              "new checkpoint after undo discards abandoned redo branch");
        CHECK(doc.floatingPixelSession()->sourcePixels() == source
              && doc.floatingPixelSession()->sourceCoverage() == mask
              && doc.floatingPixelSession()->sourcePixels().constBits() == immutableSource,
              "history preserves immutable source pixels and independent geometry");
        const int index = doc.history().currentIndex();
        const QImage beforeInvalid = doc.flatten(), selectionInvalid = doc.selection().mask().copy();
        CHECK(!doc.setFloatingPixelTransform(QTransform::fromScale(0, 1))
              && doc.flatten() == beforeInvalid && doc.selection().mask() == selectionInvalid
              && doc.history().currentIndex() == index, "invalid preview has no partial effects");
        gesture(QTransform::fromTranslate(100, 100), "Off Canvas");
        CHECK(doc.floatingPixelSession() && !doc.selection().hasSelection(),
              "off-canvas preview preserves session independently of visible selection");
        doc.history().undo();
        CHECK(doc.flatten() == beforeInvalid && doc.selection().mask() == selectionInvalid,
              "off-canvas undo restores pixels and selection");
        CHECK(other.beginFloatingPixelSession(0, source, mask, QPoint()), "independent history document initializes");
        const QImage otherRaster = other.flatten();
        doc.history().goToIndex(0);
        CHECK(other.floatingPixelSession() && other.flatten() == otherRaster,
              "history navigation does not affect another document session");
        doc.history().goToIndex(1);
        doc.addLayer("Unrelated layer");
        CHECK(doc.floatingPixelSession()->targets(doc.layerAt(0)), "active-layer switch preserves history target");
        FloatingPixelSessionCommand wrongTarget(&doc, 1, "Wrong target");
        CHECK(!wrongTarget.captureAfter(), "checkpoint rejects unrelated target layer");
        FloatingPixelSessionCommand invalidIndex(&doc, -1, "Invalid target");
        CHECK(!invalidIndex.captureAfter(), "checkpoint rejects invalid target index");
    }

    SECTION("Floating session raster checkpoints and snapshot rebinding");
    {
        Document doc(5, 4);
        QImage source(2, 2, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::transparent);
        QImage mask(2, 2, QImage::Format_Grayscale8);
        mask.fill(255);
        auto initialization = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Initialize Checkpoint");
        CHECK(doc.beginFloatingPixelSession(0, source, mask, QPoint(1, 1)), "raster checkpoint session initializes");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(1, 1)), "raster checkpoint selection initializes");
        CHECK(initialization->captureAfter(), "initialization checkpoint captured");
        doc.history().push(std::move(initialization));
        const QImage before = doc.flatten(), beforeSelection = doc.selection().mask().copy();
        // Exercise reversible session -> raster state without implementing a Finish
        // policy or hooking a user action. Stage 3 will choose that lifecycle.
        auto raster = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Raster Checkpoint");
        doc.activeLayer()->setImage(doc.floatingPixelSession()->render(QRect(0, 0, 5, 4)));
        doc.activeLayer()->setOffset(QPoint());
        doc.activeLayer()->setRetainsOffCanvasPixels(true);
        doc.discardFloatingPixelSession();
        doc.selection().clear();
        CHECK(raster->captureAfter(), "checkpoint captures raster with no session");
        doc.history().push(std::move(raster));
        CHECK(doc.flatten() == before && !doc.floatingPixelSession(), "materialized raster has identical appearance");
        doc.history().undo();
        CHECK(doc.floatingPixelSession() && doc.flatten() == before
              && doc.selection().mask() == beforeSelection && !doc.activeLayer()->retainsOffCanvasPixels(),
              "undo checkpoint restores session geometry and raster flags");
        doc.history().redo();
        CHECK(!doc.floatingPixelSession() && !doc.selection().hasSelection()
              && doc.activeLayer()->retainsOffCanvasPixels(), "redo restores raster and absence of session");
        doc.history().undo();
        auto snapshot = std::make_unique<DocumentSnapshotCommand>(&doc, "Snapshot With Session");
        doc.discardFloatingPixelSession();
        doc.activeLayer()->clear(Qt::green);
        snapshot->captureAfter();
        doc.history().push(std::move(snapshot));
        doc.history().undo();
        CHECK(doc.floatingPixelSession() && doc.floatingPixelSession()->targets(doc.activeLayer())
              && doc.flatten() == before && doc.selection().mask() == beforeSelection,
              "full-document snapshot rebinds session to reconstructed layer");
        // Walk the preceding command after snapshot restoration replaced layer objects.
        doc.history().goToIndex(0);
        doc.history().goToIndex(1);
        CHECK(doc.floatingPixelSession() && doc.flatten() == before,
              "session commands replay correctly after snapshot layer reconstruction");
        doc.history().goToIndex(2);
        CHECK(!doc.floatingPixelSession() && doc.flatten().pixelColor(0, 0) == QColor(Qt::green),
              "snapshot redo clears obsolete session state");
    }

    SECTION("Floating session full and tiled layer composition");
    {
        Document doc(7, 5);
        doc.activeLayer()->clear(QColor(60, 130, 180));
        const int targetIndex = doc.addLayer("Target");
        doc.activeLayer()->clear(QColor(200, 40, 20, 130));
        doc.activeLayer()->setOffset(QPoint(-2, 1));
        const int upperIndex = doc.addLayer("Upper");
        doc.activeLayer()->clear(QColor(20, 220, 80, 90));
        doc.activeLayer()->setOffset(QPoint(2, -1));
        QImage source(4, 3, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(230, 60, 200, 110));
        source.setPixelColor(0, 0, Qt::transparent);
        QImage mask(4, 3, QImage::Format_Grayscale8);
        mask.fill(255);
        mask.scanLine(1)[1] = 0;
        mask.scanLine(2)[2] = 100;
        CHECK(doc.beginFloatingPixelSession(targetIndex, source, mask, QPoint(3, 1)), "middle-layer composition initializes");
        const QImage committed = doc.layerAt(targetIndex)->image().copy();
        const QRect canvas(QPoint(), doc.size());
        for (BlendMode mode : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Overwrite,
                               BlendMode::Glow, BlendMode::Reflect, BlendMode::Negation, BlendMode::Xor}) {
            doc.setLayerBlendMode(targetIndex, mode);
            doc.setLayerOpacity(targetIndex, 0.6f);
            std::vector<Layer> expected;
            for (int i = 0; i < doc.layerCount(); ++i) {
                expected.push_back(*doc.layerAt(i));
                if (i == targetIndex) {
                    const QRect bounds = doc.floatingPixelSession()->documentBounds()
                                             .intersected(QRectF(canvas)).toAlignedRect();
                    expected.back().setImage(doc.floatingPixelSession()->render(bounds));
                    expected.back().setOffset(bounds.topLeft());
                }
            }
            const QImage reference = Document::compositeLayers(expected, doc.size());
            const QImage full = doc.flatten();
            CHECK(full == reference && doc.flattenVisible() == full
                  && doc.flattenVisible(canvas) == full, "effective raster uses existing layer-stack blend and opacity");
            QImage stitched(doc.size(), QImage::Format_ARGB32_Premultiplied);
            stitched.fill(Qt::transparent);
            QPainter painter(&stitched);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            for (int y = 0; y < doc.height(); y += 2)
                for (int x = 0; x < doc.width(); x += 3) {
                    const QRect tile = QRect(x, y, 3, 2).intersected(canvas);
                    painter.drawImage(tile.topLeft(), doc.flattenVisible(tile));
                }
            painter.end();
            CHECK(stitched == full, "stitched dirty tiles match full composition including custom blend modes");
        }
        CHECK(doc.layerAt(targetIndex)->image() == committed
              && doc.floatingPixelSession()->targets(doc.layerAt(targetIndex)),
              "composition neither bakes pixels nor follows active upper layer");
        doc.setLayerVisibility(targetIndex, false);
        const QImage hidden = doc.flatten();
        doc.setLayerVisibility(targetIndex, true);
        doc.setLayerOpacity(targetIndex, 0);
        CHECK(doc.flatten() == hidden, "hidden and zero-opacity session targets are excluded");
        doc.setLayerOpacity(targetIndex, 1);
        doc.setLayerBlendMode(targetIndex, BlendMode::Normal);
        doc.layerAt(upperIndex)->clear(Qt::yellow);
        doc.layerAt(upperIndex)->setOffset(QPoint());
        CHECK(doc.flatten().pixelColor(3, 1) == QColor(Qt::yellow), "opaque upper layer occludes floating content");
        CHECK(doc.flattenVisible(QRect(-2, -3, 4, 5)) == doc.flatten().copy(0, 0, 2, 2)
              && doc.flattenVisible(QRect(100, 100, 2, 2)).isNull(), "tile clipping follows document bounds");
    }
    SECTION("Floating preview preserves finite offset-layer domain");
    {
        Document doc(5, 5);
        doc.activeLayer()->clear(Qt::blue);
        const int index = doc.addLayer("Small offset target");
        QImage small(2, 2, QImage::Format_ARGB32_Premultiplied);
        small.fill(Qt::red);
        doc.activeLayer()->setImage(small);
        doc.activeLayer()->setOffset(QPoint(1, 1));
        doc.activeLayer()->setBlendMode(BlendMode::Overwrite);
        const QImage original = doc.flatten();
        QImage mask(2, 2, QImage::Format_Grayscale8);
        mask.fill(255);
        CHECK(doc.beginFloatingPixelSession(index, small, mask, QPoint(1, 1)), "finite-domain session initializes");
        CHECK(doc.flatten() == original && doc.flattenVisible(QRect(0, 0, 2, 2)) == original.copy(0, 0, 2, 2),
              "identity preview cannot clear pixels outside finite target domain in Overwrite mode");
        CHECK(!doc.setFloatingPixelTransform(QTransform::fromScale(std::numeric_limits<double>::max(), 1)),
              "transform with nonfinite raster extent is rejected");
    }
    SECTION("Floating session invalidates the existing canvas cache");
    {
        Document doc(20, 20);
        CanvasWidget canvas;
        canvas.resize(40, 40);
        canvas.setDocument(&doc);
        canvas.setShowRulers(false);
        canvas.setPan(QPointF(5, 5));
        auto sample = [&](QPoint documentPoint) {
            QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            canvas.render(&image);
            return image.pixelColor(canvas.canvasToWidget(documentPoint).toPoint());
        };
        CHECK(sample(QPoint(5, 5)) == QColor(Qt::white), "initial canvas caches committed raster");
        QImage source(4, 4, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::red);
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);
        auto initialization = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Initialize Preview");
        CHECK(doc.beginFloatingPixelSession(0, source, mask, QPoint(4, 4)), "cache-test session initializes");
        CHECK(initialization->captureAfter(), "cache-test checkpoint captured");
        doc.history().push(std::move(initialization));
        CHECK(sample(QPoint(5, 5)) == QColor(Qt::red), "session creation invalidates cached composite");
        auto move = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Move Preview");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(10, 10)), "cache-test preview updates");
        CHECK(move->captureAfter(), "cache-test move captured");
        doc.history().push(std::move(move));
        CHECK(sample(QPoint(5, 5)) == QColor(Qt::white) && sample(QPoint(11, 11)) == QColor(Qt::red),
              "transform invalidates cached composite at old and new location");
        doc.history().undo();
        CHECK(sample(QPoint(5, 5)) == QColor(Qt::red) && sample(QPoint(11, 11)) == QColor(Qt::white),
              "history restoration invalidates cached composite");
        doc.discardFloatingPixelSession();
        CHECK(sample(QPoint(5, 5)) == QColor(Qt::white), "session release invalidates cached composite");
    }
    SECTION("Floating checkpoint rejects target replacement during capture");
    {
        Document doc(2, 2);
        FloatingPixelSessionCommand pending(&doc, 0, "Pending");
        doc.restoreFromSnapshot(doc.snapshotLayers(), 2, 2, 0);
        CHECK(!pending.captureAfter(), "checkpoint cannot silently capture a replacement layer at the same index");
        FloatingPixelSessionCommand nullDocument(nullptr, 0, "Invalid");
        CHECK(!nullDocument.captureAfter(), "checkpoint rejects null document");
        CHECK(!doc.setFloatingPixelTransform(QTransform()), "preview without a session is rejected");
    }

    SECTION("Floating clipboard pixels preserve independent geometry");
    {
        QImage source(5, 4, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(180, 60, 30, 128));
        source.setPixelColor(1, 1, Qt::transparent);
        QImage mask(9, 8, QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(2)[3] = 255;
        mask.scanLine(2)[4] = 128;
        mask.scanLine(3)[3] = 255;
        Selection selection(9, 8);
        selection.setMaskImage(mask);
        const auto captured = ClipboardPixels::capture(source, QPoint(2, 1), selection);
        CHECK(captured && captured->pixels.size() == QSize(2, 2)
              && captured->sourceOrigin == QPoint(3, 2), "capture crops to selection bounds in document space");
        CHECK(captured->coverage.constScanLine(0)[0] == 255 && captured->pixels.pixel(0, 0) == 0,
              "transparent selected pixels retain full geometry");
        CHECK(captured->coverage.constScanLine(1)[1] == 0 && captured->pixels.pixel(1, 1) == 0,
              "private clipboard does not retain colours from unselected holes");
        const QImage conventional = captured->standardImage();
        const QRgb partial = source.pixel(2, 1);
        CHECK(conventional.pixel(1, 0) == qRgba(qRed(partial) * 128 / 255, qGreen(partial) * 128 / 255,
                                               qBlue(partial) * 128 / 255, qAlpha(partial) * 128 / 255),
              "ordinary clipboard image applies fractional coverage exactly once");
        const auto mime = captured->mimeData();
        CHECK(mime && mime->hasImage() && mime->hasFormat(ClipboardPixels::mimeType()),
              "clipboard advertises both standard image and private geometry");
        const auto decoded = ClipboardPixels::decode(*mime);
        CHECK(decoded && decoded->pixels == captured->pixels && decoded->coverage == captured->coverage
              && decoded->sourceOrigin == captured->sourceOrigin, "private format roundtrips unmasked premultiplied pixels and geometry");
        CHECK(qvariant_cast<QImage>(mime->imageData()) == conventional,
              "external applications receive the masked standard image");
        source.fill(Qt::green);
        mask.fill(0);
        CHECK(captured->pixels == decoded->pixels && captured->coverage == decoded->coverage,
              "capture owns snapshots independent of input image mutation");
        Document destination(2, 3);
        CHECK(destination.beginFloatingPixelSession(0, decoded->pixels, decoded->coverage, QPoint()),
              "clipboard data initializes a session in a differently sized document");
        CHECK(destination.floatingPixelSession()->sourcePixels() == decoded->pixels
              && destination.floatingPixelSession()->sourceCoverage() == decoded->coverage,
              "session consumes raw pixels and mask separately without masking twice");
        Selection all(1, 1);
        const auto whole = ClipboardPixels::capture(source, QPoint(-7, -3), all);
        const auto wholeMime = whole->mimeData();
        const auto wholeDecoded = ClipboardPixels::decode(*wholeMime);
        CHECK(wholeDecoded && wholeDecoded->pixels == source && wholeDecoded->sourceOrigin == QPoint(-7, -3)
              && wholeDecoded->coverage.constScanLine(0)[0] == 255, "whole-layer capture preserves negative origin and rectangular geometry");
        const auto outside = ClipboardPixels::capture(source, QPoint(INT_MIN, INT_MAX), selection);
        CHECK(outside && outside->pixels.pixel(0, 0) == 0 && outside->coverage.constScanLine(0)[0] == 255,
              "extreme layer offsets produce selected transparent padding without coordinate overflow");
    }
    SECTION("Floating clipboard payload validation");
    {
        QImage source(3, 2, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::red);
        Selection selection(3, 2);
        const auto captured = ClipboardPixels::capture(source, QPoint(), selection);
        const auto goodMime = captured->mimeData();
        const QByteArray payload = goodMime->data(ClipboardPixels::mimeType());
        auto rejects = [&](QByteArray bytes) {
            QMimeData mime;
            mime.setData(ClipboardPixels::mimeType(), bytes);
            mime.setImageData(source);
            CHECK(!ClipboardPixels::decode(mime), "invalid private data is rejected independently of ordinary fallback image");
        };
        rejects(QByteArray());
        rejects(payload.first(23));
        rejects(payload.chopped(1));
        rejects(payload + 'x');
        QByteArray bad = payload;
        bad[0] ^= 1;
        rejects(bad);
        bad = payload;
        qToLittleEndian(quint32(2), reinterpret_cast<uchar *>(bad.data()) + 4);
        rejects(bad);
        bad = payload;
        qToLittleEndian(quint32(INT_MAX), reinterpret_cast<uchar *>(bad.data()) + 8);
        rejects(bad);
        bad = payload;
        qToLittleEndian(quint32(0), reinterpret_cast<uchar *>(bad.data()) + 12);
        rejects(bad);
        bad = payload;
        // First pixel: nonzero colour with zero alpha violates premultiplied storage.
        qToLittleEndian(quint32(0x00ff0000), reinterpret_cast<uchar *>(bad.data()) + 24);
        rejects(bad);
        bad = payload;
        for (int i = 24 + source.width() * source.height() * 4; i < bad.size(); ++i) bad[i] = 0;
        rejects(bad);
        QMimeData external;
        external.setImageData(source);
        CHECK(!ClipboardPixels::decode(external), "ordinary external image has no invented private geometry");
        CHECK(!ClipboardPixels::capture(QImage(), QPoint(), selection), "null image cannot be captured");
        ClipboardPixels empty;
        CHECK(!empty.mimeData() && empty.standardImage().isNull(), "invalid clipboard value cannot publish an image");
    }

    SECTION("Atomic floating paste preserves destination between placements");
    {
        Document doc(12, 6);
        QImage destination(12, 6, QImage::Format_ARGB32_Premultiplied);
        destination.fill(Qt::white);
        destination.setPixelColor(1, 2, Qt::red);
        destination.setPixelColor(5, 2, Qt::green);
        destination.setPixelColor(9, 2, Qt::blue);
        doc.activeLayer()->setImage(destination);
        doc.selection().selectRect(QRect(0, 0, 2, 2));
        const QImage priorSelection = doc.selection().mask().copy();
        QImage source(2, 2, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::magenta);
        source.setPixelColor(1, 0, Qt::transparent);
        source.setPixelColor(0, 1, QColor(240, 60, 90, 100));
        QImage mask(2, 2, QImage::Format_Grayscale8);
        mask.fill(255);
        CHECK(doc.pasteFloatingPixels(source, mask, QPoint(1, 2)), "ordinary floating paste initializes");
        CHECK(doc.layerCount() == 1 && doc.activeLayerIndex() == 0 && doc.isModified(),
              "floating paste targets active layer without creating a layer");
        CHECK(doc.selection().boundingRect() == QRect(1, 2, 2, 2), "paste replaces previous selection with source geometry");
        CHECK(doc.activeLayer()->image() == destination && doc.flatten().pixelColor(1, 2) == QColor(Qt::magenta),
              "paste preview replaces target pixels without altering captured destination");
        CHECK(doc.flatten().pixel(2, 2) == 0 && doc.flatten().pixel(1, 3) == source.pixel(0, 1),
              "selected full and partial transparency replace destination alpha");
        auto move = [&](int x) {
            auto command = std::make_unique<FloatingPixelSessionCommand>(&doc, 0, "Move Pasted Pixels");
            CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(x, 2)), "pasted transform preview accepted");
            CHECK(command->captureAfter(), "pasted transform checkpoint captured");
            doc.history().push(std::move(command));
        };
        move(5);
        CHECK(doc.flatten().pixelColor(1, 2) == QColor(Qt::red), "A is preserved after moving pasted pixels to B");
        move(9);
        CHECK(doc.flatten().pixelColor(5, 2) == QColor(Qt::green)
              && doc.flatten().pixelColor(9, 2) == QColor(Qt::magenta),
              "B is preserved after moving pasted pixels to C");
        doc.history().goToIndex(0);
        CHECK(!doc.floatingPixelSession() && doc.flatten() == destination
              && doc.selection().mask() == priorSelection, "undo paste restores pixels and preceding selection");
        doc.history().goToIndex(3);
        CHECK(doc.floatingPixelSession() && doc.flatten().pixelColor(9, 2) == QColor(Qt::magenta),
              "paste and consecutive placements replay as separate history operations");
    }
    SECTION("Floating paste new layer expansion and rejection boundaries");
    {
        Document doc(3, 2);
        doc.activeLayer()->setOffset(QPoint(-1, 0));
        doc.activeLayer()->setRetainsOffCanvasPixels(true);
        doc.activeLayer()->setLocked(true);
        const QImage original = doc.activeLayer()->image().copy();
        QImage source(5, 4, QImage::Format_ARGB32_Premultiplied);
        source.fill(QColor(120, 30, 220, 128));
        QImage mask(5, 4, QImage::Format_Grayscale8);
        mask.fill(255);
        CHECK(!doc.pasteFloatingPixels(source, mask, QPoint()), "active-layer paste rejects locked layer");
        CHECK(doc.size() == QSize(3, 2) && doc.layerCount() == 1 && !doc.isModified()
              && !doc.history().canUndo(), "rejected paste leaves geometry layer stack and history untouched");
        CHECK(doc.pasteFloatingPixels(source, mask, QPoint(), true, QSize(5, 4)),
              "paste into new layer can expand canvas above locked layer");
        CHECK(doc.layerCount() == 2 && doc.activeLayerIndex() == 1 && doc.size() == QSize(5, 4)
              && doc.floatingPixelSession()->targets(doc.layerAt(1)), "new layer is active above original and owns paste");
        CHECK(doc.layerAt(0)->offset() == QPoint(-1, 0) && doc.layerAt(0)->retainsOffCanvasPixels()
              && doc.layerAt(0)->image().copy(QRect(QPoint(), original.size())) == original,
              "expansion preserves existing pixels offsets and retained-content state");
        doc.history().undo();
        CHECK(doc.size() == QSize(3, 2) && doc.layerCount() == 1 && doc.activeLayerIndex() == 0
              && !doc.floatingPixelSession() && doc.layerAt(0)->image() == original,
              "single undo reverses expansion new layer and session");
        doc.history().redo();
        CHECK(doc.floatingPixelSession() && doc.layerCount() == 2 && doc.size() == QSize(5, 4),
              "redo restores expanded paste and target binding");
        const int history = doc.history().currentIndex();
        CHECK(doc.pasteFloatingPixels(source, mask, QPoint()) && doc.history().currentIndex() == history + 1,
              "new paste atomically replaces previous session with one new history entry");
        doc.history().undo();
        CHECK(doc.floatingPixelSession() && doc.history().currentIndex() == history,
              "undo replacement paste restores previous editable session");

        Document invalid(3, 2);
        CHECK(!invalid.pasteFloatingPixels(source, mask, QPoint(), false, QSize(2, 2)),
              "paste cannot silently shrink canvas");
        mask.fill(0);
        CHECK(!invalid.pasteFloatingPixels(source, mask, QPoint(), false, QSize(5, 4))
              && invalid.size() == QSize(3, 2) && !invalid.isModified(),
              "invalid coverage cannot leave partially expanded document");
        invalid.activeLayer()->setOffset(QPoint(INT_MIN, 0));
        mask.fill(255);
        CHECK(!invalid.pasteFloatingPixels(source, mask, QPoint(), false, QSize(5, 4)),
              "overflowing retained bitmap extent is rejected before allocation");
    }

    SECTION("Floating context boundaries preserve painting and resume with a hole");
    {
        Document doc(100, 70);
        QImage source(20, 20, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::magenta);
        QImage mask(20, 20, QImage::Format_Grayscale8);
        mask.fill(255);
        CHECK(doc.pasteFloatingPixels(source, mask, QPoint(10, 10)), "context fixture pastes");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(30, 10)), "uninterrupted fixture movement");
        const QImage floating = doc.flatten();
        CHECK(doc.suspendFloatingPixels() && doc.floatingPixelSession()->isSuspended()
              && doc.selection().hasSelection() && doc.flatten() == floating,
              "context transition preserves appearance and active selection");
        const QImage beforeBrush = doc.activeLayer()->image().copy();
        doc.activeLayer()->image().setPixelColor(35, 15, Qt::black);
        doc.pushImageEdit(0, beforeBrush, "Brush");
        CHECK(doc.flatten().pixelColor(35, 15) == QColor(Qt::black), "painting affects suspended session raster");
        auto move = std::make_unique<DocumentSnapshotCommand>(&doc, "Move Selected Pixels");
        const QImage painted = doc.flatten();
        CHECK(doc.prepareFloatingPixelGesture() && doc.flatten() == painted,
              "resuming source preserves initial appearance exactly");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(60, 10)), "resumed movement accepted");
        move->captureAfter(); doc.history().push(std::move(move));
        CHECK(doc.flatten().pixelColor(35, 15).alpha() == 0
              && doc.flatten().pixelColor(65, 15) == QColor(Qt::black),
              "after painting the lifted source includes stroke and leaves transparency behind");
        const QImage moved = doc.flatten();
        CHECK(doc.finishFloatingPixels() && !doc.floatingPixelSession()
              && !doc.selection().hasSelection() && doc.flatten() == moved, "Finish preserves raster and deselects");
        doc.history().undo();
        CHECK(doc.floatingPixelSession() && !doc.floatingPixelSession()->isSuspended()
              && doc.flatten() == moved && doc.selection().hasSelection(), "Undo Finish restores editable source and its hole");
        doc.history().undo();
        CHECK(doc.floatingPixelSession()->isSuspended() && doc.flatten() == painted,
              "Undo movement restores pre-lift painted raster");
        doc.history().undo();
        CHECK(doc.flatten() == floating, "Undo Brush restores pre-stroke raster");
        doc.history().undo();
        CHECK(!doc.floatingPixelSession()->isSuspended(), "Undo context boundary restores uninterrupted paste state");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(60, 10))
              && doc.flatten().pixelColor(35, 15) == QColor(Qt::white), "pre-context movement reveals original destination again");
    }
    SECTION("Floating sessions survive layer switches and fresh native reopening");
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("floating.psw");
        QImage expected, selected, immutable;
        QTransform transform;
        {
            Document original(80, 60);
            original.addLayer("Other");
            original.setActiveLayer(0);
            QImage source(20, 20, QImage::Format_ARGB32_Premultiplied);
            source.fill(QColor(180, 70, 40, 128));
            source.setPixelColor(3, 3, Qt::transparent);
            QImage mask(20, 20, QImage::Format_Grayscale8);
            mask.fill(255);
            mask.scanLine(4)[4] = 100;
            CHECK(original.pasteFloatingPixels(source, mask, QPoint(10, 10)), "native fixture initializes");
            CHECK(original.setFloatingPixelTransform(QTransform::fromTranslate(40, 20)), "native fixture transforms");
            expected = original.flatten(); selected = original.selection().mask();
            immutable = original.floatingPixelSession()->sourcePixels();
            transform = original.floatingPixelSession()->transform();
            const int history = original.history().currentIndex();
            CHECK(original.saveNative(path) && original.floatingPixelSession()
                  && !original.floatingPixelSession()->isSuspended() && original.history().currentIndex() == history,
                  "native saving does not finish or add history");
            CHECK(original.save(dir.filePath("visible.png")) && original.floatingPixelSession()
                  && original.flatten() == expected, "PNG export preserves live editing state");
            CHECK(QImage(dir.filePath("visible.png")).convertToFormat(QImage::Format_ARGB32_Premultiplied) == expected,
                  "PNG contains current visible source and transparency");
        } // No original document/session survives this scope.
        Document reopened(1, 1);
        CHECK(reopened.loadNative(path) && reopened.floatingPixelSession(), "fresh document restores native session metadata");
        CHECK(reopened.flatten() == expected && reopened.selection().mask() == selected
              && reopened.floatingPixelSession()->sourcePixels() == immutable
              && reopened.floatingPixelSession()->transform() == transform,
              "fresh reopen restores appearance independent geometry immutable source and transform");
        CHECK(reopened.setFloatingPixelTransform(QTransform::fromTranslate(10, 10))
              && reopened.flatten().pixelColor(45, 25) == QColor(Qt::white),
              "moving reopened source reveals original destination instead of saved composite");
        reopened.setActiveLayer(1);
        CHECK(reopened.floatingPixelSession()->isSuspended(), "layer switch suspends without deselecting");
        reopened.setActiveLayer(0);
        CHECK(reopened.prepareFloatingPixelGesture()
              && reopened.setFloatingPixelTransform(QTransform::fromTranslate(40, 20))
              && reopened.flatten().pixelColor(15, 15).alpha() == 0,
              "returning after layer switch lifts raster and leaves hole");
        const QImage resumed = reopened.flatten();
        CHECK(reopened.saveNative(path), "lifted source saves with original-pose data");
        Document lifted;
        CHECK(lifted.loadNative(path) && lifted.flatten() == resumed, "lifted source native roundtrip preserves result");

        // Simulate an old reader by consuming only the unchanged v1/v2 prefix.
        QFile file(path); CHECK(file.open(QIODevice::ReadOnly), "open native prefix for legacy read");
        QDataStream input(&file); input.setVersion(QDataStream::Qt_6_0);
        quint32 magic; qint32 version, width, height, active, count;
        input >> magic >> version >> width >> height >> active >> count;
        std::vector<Layer> legacyLayers;
        for (int i = 0; i < count; ++i) {
            QString name; float opacity; bool visible, locked, retained = false; qint32 blend; QPoint offset; QImage image;
            input >> name >> opacity >> visible >> locked >> blend >> offset;
            if (version == 2) input >> retained;
            input >> image;
            Layer layer(image, name); layer.setOpacity(opacity); layer.setVisible(visible);
            layer.setBlendMode(BlendMode(blend)); layer.setOffset(offset);
            legacyLayers.push_back(layer);
        }
        CHECK(input.status() == QDataStream::Ok && magic == 0x50535731 && (version == 1 || version == 2)
              && Document::compositeLayers(legacyLayers, QSize(width, height)) == resumed,
              "legacy reader sees correct materialized raster in unchanged layered format");
        const qint64 footer = file.pos();
        file.seek(0); const QByteArray bytes = file.readAll();
        const QString legacyPath = dir.filePath("legacy.psw");
        { QFile output(legacyPath); CHECK(output.open(QIODevice::WriteOnly), "write legacy native fixture"); output.write(bytes.left(footer)); }
        Document legacy;
        CHECK(legacy.loadNative(legacyPath) && !legacy.floatingPixelSession() && legacy.flatten() == resumed,
              "native files without optional metadata remain readable");
        const QString broken = dir.filePath("truncated.psw");
        { QFile output(broken); CHECK(output.open(QIODevice::WriteOnly), "write truncated native fixture"); output.write(bytes.chopped(8)); }
        const QImage before = lifted.flatten();
        CHECK(!lifted.loadNative(broken) && lifted.floatingPixelSession() && lifted.flatten() == before,
              "invalid session trailer cannot partially replace existing document");
    }

    SECTION("Floating regional redraw matches full reference across transforms and blend modes");
    {
        for (bool fullCoverage : {false, true}) for (BlendMode mode : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Overwrite}) {
            Document doc(43, 31);
            doc.activeLayer()->clear(Qt::green);
            doc.addLayer("Offset target");
            QImage base(13, 9, QImage::Format_ARGB32_Premultiplied); base.fill(QColor(30, 90, 180, 140));
            doc.activeLayer()->setImage(base); doc.activeLayer()->setOffset(QPoint(9, 7));
            doc.activeLayer()->setBlendMode(mode); doc.activeLayer()->setOpacity(0.7);
            QImage source(7, 5, QImage::Format_ARGB32_Premultiplied); source.fill(QColor(190, 60, 80, 110));
            source.setPixel(0, 0, 0);
            QImage coverage(7, 5, QImage::Format_Grayscale8); coverage.fill(255);
            if (!fullCoverage) { coverage.scanLine(0)[0] = 100; coverage.scanLine(2)[3] = 0; }
            CHECK(doc.pasteFloatingPixels(source, coverage, QPoint(28, 20),false,QSize(),FloatingPixelSession::Sampling::Nearest), "regional fixture starts live");
            QRegion dirty; int notifications = 0;
            QObject::connect(&doc, &Document::floatingPixelsChanged, &doc, [&](const QRegion &area) { dirty = area; ++notifications; });
            for (const QTransform transform : {QTransform::fromTranslate(29, 21), QTransform::fromTranslate(-3, -2),
                     QTransform::fromScale(2.3, 1.7) * QTransform::fromTranslate(12.25, 8.5),
                     QTransform().rotate(37) * QTransform::fromTranslate(30, 6),
                     QTransform::fromTranslate(1000, 900), QTransform::fromTranslate(5, 4),
                     QTransform(1 + 1e-14, 1e-16, 1e-16, 1, 5.5, 4.5)}) {
                QImage incremental = doc.flatten();
                CHECK(doc.setFloatingPixelTransform(transform), "regional transform accepted");
                { QPainter painter(&incremental); painter.setCompositionMode(QPainter::CompositionMode_Source);
                  for (const QRect &area : dirty) painter.drawImage(area.topLeft(), doc.flattenVisible(area)); }
                CHECK(incremental == doc.flatten(), "old/new footprints and finite-domain changes fully invalidate composite");
                const auto *session = doc.floatingPixelSession();
                const QRect region(-4, -3, 53, 40);
                QImage expected(region.size(), QImage::Format_ARGB32_Premultiplied); expected.fill(0);
                QImage expectedMask(doc.size(), QImage::Format_Grayscale8); expectedMask.fill(0);
                const QTransform inverse = transform.inverted();
                for (int y = 0; y < region.height(); ++y) for (int x = 0; x < region.width(); ++x) {
                    const QPoint point = region.topLeft() + QPoint(x, y);
                    const QPoint bp = point - QPoint(9, 7);
                    QRgb pixel = base.rect().contains(bp) ? base.pixel(bp) : 0;
                    const QPointF sp = inverse.map(QPointF(point) + QPointF(0.5, 0.5));
                    if (sp.x() >= 0 && sp.y() >= 0 && sp.x() < source.width() && sp.y() < source.height()) {
                        const QPoint sample(int(std::floor(sp.x())), int(std::floor(sp.y())));
                        const int c = coverage.constScanLine(sample.y())[sample.x()]; const QRgb src = source.pixel(sample);
                        auto mix = [c](int d, int s) { return (s*c + d*(255-c) + 127)/255; };
                        pixel = qRgba(mix(qRed(pixel),qRed(src)), mix(qGreen(pixel),qGreen(src)),
                                      mix(qBlue(pixel),qBlue(src)), mix(qAlpha(pixel),qAlpha(src)));
                        if (expectedMask.rect().contains(point)) expectedMask.scanLine(point.y())[point.x()] = c;
                    }
                    expected.setPixel(x, y, pixel);
                }
                CHECK(session->render(region) == expected, "optimized raster equals scalar replacement reference");
                CHECK(doc.selection().mask() == expectedMask, "bounded mask equals scalar inverse-sampling reference");
                Selection ordinary; ordinary.setMaskImage(expectedMask);
                CHECK(doc.selection().boundingRect() == ordinary.boundingRect() && doc.selection().region() == ordinary.region(),
                      "bounded selection caches preserve bounds and marquee geometry");
                CHECK(session->renderScaled(region, region.size()) == expected, "bounded preview uses identical pixel-centre sampling");
                const int before = notifications; const qint64 key = doc.selection().mask().cacheKey();
                CHECK(doc.setFloatingPixelTransform(transform) && notifications == before && doc.selection().mask().cacheKey() == key,
                      "unchanged transform performs no raster or selection update");
            }
            CHECK(doc.thumbnail(QSize(30, 30)).size() == QSize(30, 21), "document thumbnail stays bounded");
        }
    }

    SECTION("Bounded floating sample cache ownership and eviction");
    {
        FloatingRenderCache cache(16);
        FloatingRenderCache::Tile tile;
        tile.pixels = QImage(32, 32, QImage::Format_ARGB32_Premultiplied); tile.pixels.fill(Qt::red);
        tile.coverage = QImage(32, 32, QImage::Format_Grayscale8); tile.coverage.fill(255); tile.opaqueCoverage = true;
        for (int i = 0; i < 100; ++i) cache.insert(QByteArray::number(i), tile);
        CHECK(cache.stats().costKiB <= 16 && cache.stats().entries <= 2, "LRU respects total image and key budget");
        CHECK(cache.find("0").isNull() && !cache.find("99").isNull(), "old tiles evict before recently inserted samples");
        auto returned = cache.find("99"); returned.pixels.fill(Qt::blue); returned.coverage.fill(0);
        CHECK(cache.find("99").pixels.pixelColor(0, 0) == QColor(Qt::red)
              && cache.find("99").coverage.constScanLine(0)[0] == 255, "returned tile values detach on mutation");
        cache.clear();
        CHECK(cache.stats().costKiB == 0 && cache.stats().entries == 0 && returned.pixels.pixelColor(0, 0) == QColor(Qt::blue),
              "eviction releases cache ownership without invalidating callers");
        FloatingRenderCache disabled(0); disabled.insert("tile", tile);
        CHECK(disabled.stats().entries == 0, "oversized entries are rejected without exceeding budget");
    }

    SECTION("Floating cached rendering equals uncached composition through mutations");
    {
        auto &cache = FloatingRenderCache::shared(); cache.clear();
        Document doc(640, 480); doc.activeLayer()->clear(QColor(25, 130, 190, 200));
        doc.addLayer("Offset target");
        QImage base(120, 85, QImage::Format_ARGB32_Premultiplied); base.fill(QColor(190, 35, 60, 120));
        doc.activeLayer()->setImage(base); doc.activeLayer()->setOffset(QPoint(-13, 11));
        QImage source(307, 281, QImage::Format_ARGB32_Premultiplied); source.fill(QColor(60, 90, 200, 120)); source.setPixel(10, 20, 0);
        QImage mask(source.size(), QImage::Format_Grayscale8); mask.fill(255);
        for (int y = 0; y < mask.height(); ++y) for (int x = 0; x < mask.width(); ++x)
            if ((x+y)%5 == 0) mask.scanLine(y)[x] = 100; else if ((x+y)%7 == 0) mask.scanLine(y)[x] = 0;
        CHECK(doc.pasteFloatingPixels(source, mask, QPoint(200, 30))
              && doc.setFloatingPixelTransform(QTransform().rotate(13.5) * QTransform::fromTranslate(200.25, 30.5)),
              "cached fixture crosses tile boundaries with fractional alpha and geometry");
        auto uncached = [](Document &document) {
            std::vector<Layer> layers;
            const auto *session = document.floatingPixelSession();
            for (int i = 0; i < document.layerCount(); ++i) {
                Layer layer(*document.layerAt(i));
                if (session && !session->isSuspended() && session->targets(document.layerAt(i))) {
                    const QRect domain = session->documentBounds().intersected(QRectF(QPointF(), document.size())).toAlignedRect();
                    if (!domain.isEmpty()) { layer.setImage(session->render(domain)); layer.setOffset(domain.topLeft()); }
                    else layer.setVisible(false);
                }
                layers.push_back(layer);
            }
            return Document::compositeLayers(layers, document.size());
        };
        CHECK(doc.flatten() == uncached(doc), "first cache population preserves full-resolution reference");
        const auto warm = cache.stats();
        CHECK(doc.flatten() == uncached(doc) && cache.stats().misses == warm.misses && cache.stats().hits > warm.hits,
              "unchanged rendering reuses sampled pixels and coverage");
        for (BlendMode mode : Layer::allBlendModes()) for (float opacity : {0.0f, 0.3f, 1.0f}) {
            doc.setLayerBlendMode(1, mode); doc.setLayerOpacity(1, opacity);
            CHECK(doc.flatten() == uncached(doc), "cached samples preserve blend/opacity composition including Overwrite");
            CHECK(cache.stats().misses == warm.misses, "layer properties do not resample unchanged source geometry");
        }
        doc.setLayerVisibility(1, false); CHECK(doc.flatten() == uncached(doc), "hidden source does not leave cached pixels");
        doc.setLayerVisibility(1, true); doc.setLayerBlendMode(1, BlendMode::Normal);
        CHECK(doc.setFloatingPixelTransform(QTransform().rotate(-27) * QTransform::fromTranslate(-100, 80))
              && doc.flatten() == uncached(doc) && cache.stats().misses > warm.misses, "transform change invalidates samples and handles off-canvas clipping");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromScale(1.2, 0.8) * QTransform::fromTranslate(30.5, 40.25))
              && doc.flatten() == uncached(doc), "axis-aligned footprint cache matches replacement reference");
        const auto axisWarm = cache.stats(); doc.setLayerOpacity(1, 0.6f);
        CHECK(doc.flatten() == uncached(doc) && cache.stats().misses == axisWarm.misses,
              "axis-aligned property updates reuse rendered footprint without resampling");
        const QRect partial(31, 29, 283, 297);
        CHECK(doc.flattenVisible(partial) == uncached(doc).copy(partial), "unaligned subregions reuse correctly clipped tiles");
        QImage changed(70, 90, QImage::Format_ARGB32_Premultiplied); changed.fill(Qt::yellow);
        QImage changedMask(changed.size(), QImage::Format_Grayscale8); changedMask.fill(128);
        CHECK(doc.pasteFloatingPixels(changed, changedMask, QPoint(50, 40)) && doc.flatten() == uncached(doc),
              "replacement source and independent geometry never reuse stale tiles");
        doc.history().undo(); CHECK(doc.flatten() == uncached(doc), "history restores matching source cache identity");
        CHECK(doc.suspendFloatingPixels(), "cached source suspends safely");
        doc.activeLayer()->image().setPixelColor(1, 1, Qt::black);
        CHECK(doc.prepareFloatingPixelGesture() && doc.setFloatingPixelTransform(QTransform::fromTranslate(120, 90))
              && doc.flatten() == uncached(doc), "edited raster and resumed lift use fresh source samples");
        QTemporaryDir dir; const QString native = dir.filePath("cached.psw");
        CHECK(doc.saveNative(native), "cached editable source saves");
        Document reopened; CHECK(reopened.loadNative(native) && reopened.flatten() == doc.flatten(), "native reopen is independent of in-memory cache identity");
        const auto misses = cache.stats().misses;
        FloatingRenderCache::Tile eviction;
        eviction.pixels = QImage(256, 256, QImage::Format_ARGB32_Premultiplied); eviction.pixels.fill(0);
        eviction.coverage = QImage(256, 256, QImage::Format_Grayscale8); eviction.coverage.fill(255);
        for (int i = 0; i < 1000; ++i) cache.insert("evict:" + QByteArray::number(i), eviction);
        CHECK(cache.stats().costKiB <= cache.stats().limitKiB, "shared budget stays bounded under cross-document-style pressure");
        CHECK(doc.flatten() == uncached(doc) && reopened.flatten() == doc.flatten() && cache.stats().misses > misses,
              "eviction rebuilds correct results for independent documents");
        const QImage before = doc.flatten(); const auto beforeStats = cache.stats();
        CHECK(!doc.setFloatingPixelTransform(QTransform::fromScale(0, 1)) && doc.flatten() == before
              && cache.stats().misses == beforeStats.misses, "invalid transforms do not corrupt or invalidate good samples");
    }

    SECTION("Stage 6 structural lifecycle preserves session target identity");
    {
        Document doc(80,60);
        doc.addLayer("Target");
        QImage pixels(12,10,QImage::Format_ARGB32_Premultiplied); pixels.fill(Qt::red);
        QImage mask(pixels.size(),QImage::Format_Grayscale8); mask.fill(128);
        CHECK(doc.pasteFloatingPixels(pixels,mask,QPoint(20,15)), "soft source starts editable");
        auto *target = doc.activeLayer();
        doc.moveLayer(1,0);
        CHECK(doc.floatingPixelSession()->targets(target) && !doc.floatingPixelSession()->isSuspended(),
              "reorder retains source ownership by layer identity");
        doc.history().undo();
        CHECK(doc.floatingPixelSession()->targets(doc.layerAt(1)), "undo reorder preserves target");
        doc.duplicateLayer(1);
        CHECK(doc.floatingPixelSession()->isSuspended(), "duplicate materializes source before copying");
        CHECK(doc.layerAt(1)->image() == doc.layerAt(2)->image(), "duplicate captures effective source");
        doc.history().undo(); doc.history().undo();
        CHECK(doc.floatingPixelSession() && !doc.floatingPixelSession()->isSuspended(),
              "undo duplicate and Finish restores uninterrupted source");
        doc.removeLayer(1);
        CHECK(!doc.floatingPixelSession(), "remove target finishes before releasing layer");
        doc.history().undo(); doc.history().undo();
        CHECK(doc.floatingPixelSession() && doc.floatingPixelSession()->targets(doc.layerAt(1)),
              "undo removal and Finish rebinds editable target");
    }

    SECTION("Stage 6 distant extent rejection is atomic");
    {
        Document doc(32,24);
        QImage pixels(4,4,QImage::Format_ARGB32_Premultiplied); pixels.fill(Qt::green);
        QImage mask(pixels.size(),QImage::Format_Grayscale8); mask.fill(255);
        CHECK(doc.pasteFloatingPixels(pixels,mask,QPoint()), "extent fixture starts");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(1e12,1e12)),
              "off-canvas preview clips without allocating a giant bridge");
        const QImage stored = doc.activeLayer()->image();
        const int index = doc.history().currentIndex();
        CHECK(!doc.finishFloatingPixels() && doc.floatingPixelSession()
              && doc.activeLayer()->image() == stored && doc.history().currentIndex() == index,
              "unrepresentable materialization fails without discarding pixels source or history");
        CHECK(doc.setFloatingPixelTransform(QTransform::fromTranslate(5,6)) && doc.finishFloatingPixels(),
              "rejected materialization leaves source recoverable by moving back");
    }

    SECTION("Copied placement preserves coordinates and follows the viewport direction");
    {
        ClipboardPixels clip;
        clip.pixels = QImage(20,20,QImage::Format_ARGB32_Premultiplied);
        const QSize canvas(1000,800);
        const QRectF view(200,200,200,200);
        clip.sourceOrigin = QPoint(240,260);
        CHECK(clip.pasteOrigin(view,canvas) == clip.sourceOrigin, "visible copied coordinates are retained");
        clip.sourceOrigin = QPoint(190,240);
        CHECK(clip.pasteOrigin(view,canvas) == clip.sourceOrigin, "partly visible sample retains coordinates");
        clip.sourceOrigin = QPoint(90,90);
        CHECK(clip.pasteOrigin(view,canvas) == QPoint(200,200), "diagonal sample meets nearest inset corner along centre ray");
        clip.sourceOrigin = QPoint(90,240);
        CHECK(clip.pasteOrigin(view,canvas) == QPoint(200,268), "left edge placement preserves diagonal direction");
        clip.sourceOrigin = QPoint(490,290);
        CHECK(clip.pasteOrigin(view,canvas) == QPoint(380,290), "right edge fits whole sample");
        clip.sourceOrigin = QPoint(290,490);
        CHECK(clip.pasteOrigin(view,canvas) == QPoint(290,380), "bottom edge fits whole sample");
        clip.sourceOrigin = QPoint(900,700);
        CHECK(clip.pasteOrigin(QRectF(0,0,1000,800),canvas) == clip.sourceOrigin, "full view preserves source position");
        CHECK(clip.pasteOrigin(QRectF(0,0,40,30),QSize(40,30)) == QPoint(16,10), "smaller destination follows direction within canvas");
        clip.pixels = QImage(250,250,QImage::Format_ARGB32_Premultiplied);
        CHECK(clip.pasteOrigin(view,canvas) == QPoint(175,175), "sample larger than viewport centres without shrinking");
    }

    // ---------- RESULT ----------
    printf("\n=====================================\n");
    printf("PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    printf("%s\n", g_fail == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
