#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QImageReader>
#include <QTimer>
#include <QDialog>
#include <QCommandLinkButton>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <iostream>
#include "mainwindow.h"
#include "canvas/canvaswidget.h"
#include "core/document.h"
#include "i18n.h"
#include "tools/magicwandtool.h"
#include "tools/filltool.h"
#include "tools/brushtool.h"
#include "tools/penciltool.h"
#include "tools/erasertool.h"
#include "tools/recolortool.h"
#include "tools/shapetool.h"
#include "tools/linetool.h"
#include "tools/texttool.h"
#include "tools/gradienttool.h"
#include "tools/clonestamptool.h"
#include "tools/colorpickertool.h"
#include "tools/movetool.h"


static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; } } while (0)
static void invoke(MainWindow &w, const char *slot) {
    CHECK(QMetaObject::invokeMethod(&w, slot, Qt::DirectConnection));
}
static QImage fixture(int w = 40, int h = 30) {
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(17, 96, 201, 128));
    image.setPixelColor(w - 1, h - 1, Qt::red);
    return image;
}
static void paths() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.path() + QString::fromUtf8("/copied image é猫.png");
    const QImage image = fixture();
    CHECK(image.save(path));
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    CHECK(canvas != nullptr);
    auto *doc = canvas->document();
    QApplication::clipboard()->setText(path);
    invoke(w, "paste");
    CHECK(doc->layerCount() == 2);
    CHECK(doc->activeLayer()->image().pixelColor(39, 29) == QColor(Qt::red));
    CHECK(doc->activeLayer()->image().size() == doc->size());
}
static void cancelPrompt() {
    for (int choice : {2, 3, 4}) {
        MainWindow w;
        auto *canvas = w.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        doc->selection().selectRect(QRect(4, 6, 20, 30));
        const QImage before = doc->flatten();
        const QImage mask = doc->selection().mask();
        auto *tool = canvas->currentTool();
        const int active = doc->activeLayerIndex();
        const int history = doc->history().currentIndex();
        const bool modified = doc->isModified();
        bool seen = false;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            CHECK(dlg != nullptr);
            if (!dlg) return;
            seen = true;
            CHECK(dlg->windowTitle() == "Paste");
            const auto rows = dlg->findChildren<QCommandLinkButton *>();
            CHECK(rows.size() == 3);
            if (rows.size() == 3) {
                CHECK(rows[0]->text() == "Expand canvas");
                CHECK(rows[0]->description() == "Automatically expands the canvas to fit the image being pasted.");
                CHECK(rows[1]->text() == "Keep canvas size");
                CHECK(rows[1]->description() == "Does not expand the canvas. You will have to move the pasted image around to make sure that the part you want is within the canvas boundaries.");
                CHECK(rows[2]->text() == "Cancel");
                CHECK(rows[2]->description() == "Cancels the paste action.");
                CHECK(rows[1]->height() < 100); // wrapped explanation without oversized blank rows
                CHECK(rows[0]->hasFocus());
                const QImage focused = rows[0]->grab().toImage().copy(0, 0, rows[0]->width(), 3);
                rows[0]->clearFocus();
                const QImage unfocused = rows[0]->grab().toImage().copy(0, 0, rows[0]->width(), 3);
                CHECK(focused != unfocused); // visible full-row focus indication
                rows[0]->setFocus();
            }
            bool message = false, thumbnail = false;
            for (auto *label : dlg->findChildren<QLabel *>()) {
                if (label->text() == "The image being pasted is larger than the canvas size. What do you want to do?") message = true;
                if (label->size() == QSize(120,80) && !label->pixmap().isNull()) thumbnail = true;
            }
            CHECK(message); CHECK(thumbnail);
            // The modal must run before any document mutation.
            CHECK(doc->flatten() == before); CHECK(doc->layerCount() == 1);
            if (choice == 2 && rows.size() == 3) rows[2]->click();
            else if (choice == 3) {
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(dlg, &escape);
            } else dlg->close();
        });
        timer.start(30);
        QApplication::clipboard()->setImage(fixture(doc->width() + 40, doc->height() + 30));
        invoke(w, choice == 2 ? "paste" : "pasteIntoNewLayer");
        timer.stop();
        CHECK(seen);
        CHECK(doc->flatten() == before); CHECK(doc->size() == before.size());
        CHECK(doc->layerCount() == 1); CHECK(doc->activeLayerIndex() == active);
        CHECK(doc->history().currentIndex() == history); CHECK(doc->isModified() == modified);
        CHECK(doc->selection().mask() == mask); CHECK(canvas->currentTool() == tool);
    }
}
static void choosePaste(MainWindow &w, const QImage &image, int choice, const char *slot = "paste") {
    bool seen = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        CHECK(dlg != nullptr);
        if (!dlg) return;
        seen = true;
        const auto rows = dlg->findChildren<QCommandLinkButton *>();
        CHECK(rows.size() == 3);
        if (rows.size() == 3) rows[choice]->click(); else dlg->reject();
    });
    QApplication::clipboard()->setImage(image);
    timer.start(30);
    invoke(w, slot);
    timer.stop();
    CHECK(seen);
}
static void expandPaste() {
    for (QSize size : {QSize(840, 500), QSize(700, 630), QSize(840, 630)}) {
        MainWindow w;
        auto *canvas = w.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        doc->activeLayer()->image().setPixelColor(6, 8, Qt::green);
        doc->selection().selectRect(QRect(3, 4, 10, 12));
        const QImage before = doc->layerAt(0)->image();
        const QImage mask = doc->selection().mask();
        const QSize original = doc->size();
        const QImage pasted = fixture(size.width(), size.height());
        choosePaste(w, pasted, 0, "pasteIntoNewLayer");
        CHECK(doc->size() == QSize(qMax(original.width(), size.width()), qMax(original.height(), size.height())));
        CHECK(doc->layerCount() == 2);
        CHECK(doc->layerAt(0)->image().copy(before.rect()) == before);
        CHECK(doc->activeLayer()->image().pixelColor(size.width()-1, size.height()-1) == QColor(Qt::red));
        CHECK(doc->history().currentIndex() == 1);
        CHECK(!doc->selection().hasSelection());
        CHECK(canvas->currentTool()->type() == ToolType::Move);
        doc->history().undo();
        CHECK(doc->size() == original); CHECK(doc->layerCount() == 1);
        CHECK(doc->layerAt(0)->image() == before);
        CHECK(doc->selection().mask() == mask);
        doc->history().redo();
        CHECK(doc->layerCount() == 2);
        CHECK(doc->activeLayerIndex() == 1);
        CHECK(!doc->selection().hasSelection());
    }
}
static void drag(CanvasWidget *canvas, QPoint start, QPoint delta) {
    auto *tool = canvas->currentTool();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start), QPointF(start), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(start+delta), QPointF(start+delta), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(start+delta), QPointF(start+delta), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    tool->mousePressEvent(start, &press, *canvas);
    tool->mouseMoveEvent(start+delta, &move, *canvas);
    tool->mouseReleaseEvent(start+delta, &release, *canvas);
}
static void matchedCanvasRetention() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    const QImage image = fixture(840,630);
    choosePaste(window, image, 1);
    choosePaste(window, fixture(840,630), 0);
    doc->setActiveLayer(1);
    CHECK(doc->size() == image.size());
    CHECK(doc->activeLayer()->image() == image);
    // Save before the first move: neither bitmap size nor origin identifies Keep.
    QTemporaryDir directory;
    CHECK(doc->saveNative(directory.path()+"/matched.psw"));
    Document loaded;
    CHECK(loaded.loadNative(directory.path()+"/matched.psw"));
    loaded.setActiveLayer(1);
    CanvasWidget loadedCanvas;
    MoveTool move;
    loadedCanvas.setDocument(&loaded);
    loadedCanvas.setCurrentTool(&move);
    for (CanvasWidget *target : {canvas, &loadedCanvas}) {
        auto *document = target->document();
        for (QPoint delta : {QPoint(50,40), QPoint(-50,-40)}) {
            // Repeat after returning to zero so retention cannot depend on the
            // original drag offset either.
            for (int repeat = 0; repeat < 2; ++repeat) {
                drag(target, QPoint(300,200), delta);
                CHECK(document->activeLayer()->image() == image);
                CHECK(document->activeLayer()->offset() == delta);
                drag(target, QPoint(300,200), -delta);
                CHECK(document->activeLayer()->image() == image);
                CHECK(document->activeLayer()->offset().isNull());
                document->history().undo();
                CHECK(document->activeLayer()->offset() == delta);
                CHECK(document->activeLayer()->image() == image);
                document->history().undo();
                CHECK(document->activeLayer()->offset().isNull());
                CHECK(document->activeLayer()->image() == image);
                document->history().redo(); document->history().redo();
                CHECK(document->activeLayer()->image() == image);
                CHECK(document->activeLayer()->offset().isNull());
            }
        }
    }
    // Expansion snapshots, duplicates, Layer copy/assignment and add/remove
    // history must retain the same movement behavior.
    Document copies;
    CHECK(copies.loadNative(directory.path()+"/matched.psw"));
    copies.setActiveLayer(1);
    copies.duplicateLayer(1);
    copies.history().undo(); copies.history().redo();
    Layer assigned(1,1);
    assigned = *copies.activeLayer();
    copies.insertLayerDirect(3, std::make_shared<Layer>(assigned));
    copies.removeLayer(3);
    copies.history().undo();
    CanvasWidget copyCanvas;
    MoveTool copyMove;
    copyCanvas.setDocument(&copies);
    copyCanvas.setCurrentTool(&copyMove);
    for (int index : {1,2,3}) {
        copies.setActiveLayer(index);
        drag(&copyCanvas, QPoint(300,200), QPoint(50,40));
        drag(&copyCanvas, QPoint(300,200), QPoint(-50,-40));
        CHECK(copies.activeLayer()->image() == image);
        CHECK(copies.activeLayer()->offset().isNull());
    }
    Document snapshot(800,600);
    snapshot.pasteImage(image,"Keep",false);
    snapshot.pasteImage(image,"Expand",true);
    snapshot.history().undo(); snapshot.history().redo();
    snapshot.setActiveLayer(1);
    copyCanvas.setDocument(&snapshot);
    drag(&copyCanvas, QPoint(300,200), QPoint(50,40));
    drag(&copyCanvas, QPoint(300,200), QPoint(-50,-40));
    CHECK(snapshot.activeLayer()->image() == image);
}

static void retainedMove() {
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    const QImage image = fixture(840, 630);
    choosePaste(w, image, 1);
    drag(canvas, QPoint(300, 200), QPoint(-50, -40));
    CHECK(doc->activeLayer()->image() == image);
    CHECK(doc->activeLayer()->offset() == QPoint(-50, -40));
    CHECK(doc->flatten().pixelColor(789, 589) == QColor(Qt::red));
    drag(canvas, QPoint(200, 200), QPoint(20, 15));
    CHECK(doc->activeLayer()->image() == image);
    CHECK(doc->activeLayer()->offset() == QPoint(-30, -25));
    doc->history().undo();
    CHECK(doc->activeLayer()->offset() == QPoint(-50, -40));
    CHECK(doc->activeLayer()->image() == image);
    doc->history().undo();
    CHECK(doc->activeLayer()->offset().isNull());
    doc->history().redo(); doc->history().redo();
    CHECK(doc->activeLayer()->offset() == QPoint(-30, -25));
    CHECK(doc->activeLayer()->image() == image);
    QTemporaryDir dir;
    CHECK(doc->saveNative(dir.path()+"/retained.psw"));
    Document loaded;
    CHECK(loaded.loadNative(dir.path()+"/retained.psw"));
    CHECK(loaded.size() == doc->size());
    CHECK(loaded.activeLayer()->image() == image);
    CHECK(loaded.activeLayer()->offset() == QPoint(-30,-25));
    CHECK(loaded.flatten() == doc->flatten());
    const QRect tile(10, 10, 30, 40);
    CHECK(doc->flattenVisible(tile) == doc->flatten().copy(tile));
}
static void keepPaste() {
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    const QSize original = doc->size();
    const QImage image = fixture(840, 630);
    choosePaste(w, image, 1);
    CHECK(doc->size() == original);
    CHECK(doc->activeLayer()->image() == image);
    CHECK(doc->activeLayer()->image().format() == QImage::Format_ARGB32_Premultiplied);
    CHECK(doc->history().currentIndex() == 1);
    doc->history().undo();
    CHECK(doc->layerCount() == 1); CHECK(doc->activeLayerIndex() == 0);
    doc->history().redo();
    CHECK(doc->activeLayer()->image() == image);
}
static void retainedWand() {
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    choosePaste(w, fixture(840, 630), 1);
    drag(canvas, QPoint(200,200), QPoint(-50,-40));
    MagicWandTool wand;
    wand.setTolerance(0);
    wand.setSampleImage(false);
    wand.setGlobal(true);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(789,589), QPointF(789,589), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    wand.mousePressEvent(QPoint(789,589), &press, *canvas);
    CHECK(doc->selection().mask().size() == doc->size());
    CHECK(doc->selection().boundingRect() == QRect(789,589,1,1));
    CHECK(doc->selection().isSelected(789,589));
}
static void retainedFill() {
    for (bool sampleImage : {false, true}) {
        MainWindow w;
        auto *canvas = w.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        const QImage image = fixture(840, 630);
        choosePaste(w, image, 1);
        drag(canvas, QPoint(200,200), QPoint(-50,-40));
        FillTool fill;
        fill.setTolerance(0); fill.setGlobal(true); fill.setSampleImage(sampleImage);
        doc->setPrimaryColor(Qt::green);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(789,589), QPointF(789,589), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        fill.mousePressEvent(QPoint(789,589), &press, *canvas);
        CHECK(doc->activeLayer()->image().pixelColor(839,629) == QColor(Qt::green));
        CHECK(doc->flatten().pixelColor(789,589) == QColor(Qt::green));
        CHECK(doc->activeLayer()->image().pixelColor(0,0) == image.pixelColor(0,0));
        doc->history().undo();
        CHECK(doc->activeLayer()->image() == image);
        CHECK(doc->activeLayer()->offset() == QPoint(-50,-40));
        doc->history().redo();
        CHECK(doc->flatten().pixelColor(789,589) == QColor(Qt::green));
    }
}
static std::unique_ptr<Tool> paintingTool(ToolType type) {
    switch (type) {
    case ToolType::Brush: return std::make_unique<BrushTool>();
    case ToolType::Pencil: return std::make_unique<PencilTool>();
    case ToolType::Eraser: return std::make_unique<EraserTool>();
    case ToolType::Recolor: return std::make_unique<RecolorTool>();
    case ToolType::Shape: return std::make_unique<ShapeTool>();
    case ToolType::Line: return std::make_unique<LineTool>();
    case ToolType::Text: return std::make_unique<TextTool>();
    case ToolType::Gradient: return std::make_unique<GradientTool>();
    case ToolType::CloneStamp: return std::make_unique<CloneStampTool>();
    default: return std::make_unique<ColorPickerTool>();
    }
}
static void paintGesture(Tool *tool, CanvasWidget &canvas) {
    tool->setBrushSize(8);
    const QPointF from(100,100), to(160,130);
    if (tool->type() == ToolType::CloneStamp) {
        QMouseEvent source(QEvent::MouseButtonPress, QPointF(105,95), QPointF(105,95), Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        tool->mousePressEvent(QPointF(105,95), &source, canvas);
    }
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    tool->mousePressEvent(from, &press, canvas);
    if (tool->type() == ToolType::Text) {
        QKeyEvent text(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "XX");
        tool->keyPressEvent(&text, canvas);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        tool->keyPressEvent(&enter, canvas);
    }
    tool->mouseMoveEvent(to, &move, canvas);
    tool->mouseReleaseEvent(to, &release, canvas);
    tool->deactivate(canvas);
}
static void retainedPainting() {
    for (const auto type : {ToolType::Brush, ToolType::Pencil, ToolType::Eraser,
         ToolType::Recolor, ToolType::Shape, ToolType::Line, ToolType::Text,
         ToolType::Gradient, ToolType::CloneStamp, ToolType::ColorPicker}) {
        for (bool selected : {false, true}) {
            MainWindow w;
            auto *canvas = w.findChild<CanvasWidget *>();
            auto *doc = canvas->document();
            QImage image = fixture(840,630);
            { QPainter p(&image); p.fillRect(145,125,20,20,Qt::red); }
            choosePaste(w, image, 1);
            drag(canvas,QPoint(300,200),QPoint(-50,-40));
            Document reference(800,600);
            const int idx = reference.addLayer("reference");
            { QPainter p(&reference.layerAt(idx)->image()); p.drawImage(QPoint(-50,-40),image); }
            CanvasWidget referenceCanvas;
            referenceCanvas.setDocument(&reference);
            if (selected) {
                doc->selection().selectRect(QRect(90,80,70,70));
                reference.selection().selectRect(QRect(90,80,70,70));
            } else {
                // The finite retained bitmap moves with its own bounds. The
                // reference canvas-buffer edit is restricted to that same domain.
                reference.selection().selectRect(image.rect().translated(-50,-40)
                                                    .intersected(QRect(QPoint(),doc->size())));
            }
            doc->setPrimaryColor(Qt::green); reference.setPrimaryColor(Qt::green);
            doc->setSecondaryColor(Qt::red); reference.setSecondaryColor(Qt::red);
            auto actualTool = paintingTool(type), expectedTool = paintingTool(type);
            const QImage before = doc->activeLayer()->image();
            paintGesture(actualTool.get(),*canvas);
            paintGesture(expectedTool.get(),referenceCanvas);
            const int failBefore = failures;
            CHECK(doc->flatten() == reference.flatten());
            CHECK(doc->primaryColor() == reference.primaryColor());
            CHECK(doc->activeLayer()->image().size() == image.size());
            CHECK(doc->activeLayer()->offset() == QPoint(-50,-40));
            CHECK(doc->activeLayer()->image().pixelColor(0,0) == before.pixelColor(0,0));
            if (failures != failBefore) std::cerr << "  retained tool " << int(type) << " selected=" << selected << "\n";
        }
    }
}
static void retainedSelectedMove() {
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    QImage image = fixture(840,630);
    { QPainter p(&image); p.fillRect(145,125,20,20,Qt::red); }
    choosePaste(w,image,1);
    drag(canvas,QPoint(300,200),QPoint(-50,-40));
    doc->selection().selectRect(QRect(95,85,20,20));
    drag(canvas,QPoint(105,95),QPoint(25,20));
    CHECK(doc->activeLayer()->image().pixelColor(175,155) == QColor(Qt::red));
    CHECK(doc->activeLayer()->image().pixelColor(150,130).alpha() == 0);
    CHECK(doc->activeLayer()->image().pixelColor(839,629) == QColor(Qt::red));
    CHECK(doc->activeLayer()->offset() == QPoint(-50,-40));
    doc->history().undo();
    CHECK(doc->activeLayer()->image() == image);
    doc->history().redo();
    CHECK(doc->flatten().pixelColor(125,115) == QColor(Qt::red));
}
static void noPrompt(MainWindow &w, const char *slot) {
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        CHECK(dlg == nullptr);
        if (dlg) dlg->reject();
    });
    timer.start(30);
    invoke(w, slot);
    timer.stop();
}
static void newImagePath() {
    QTemporaryDir dir;
    const QString path = dir.path() + QString::fromUtf8("/new é猫 image.png");
    const QImage image = fixture(940, 730);
    CHECK(image.save(path));
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *original = canvas->document();
    QApplication::clipboard()->setText(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    noPrompt(w, "pasteIntoNewImage");
    CHECK(canvas->document() != original);
    CHECK(original->layerCount() == 1); CHECK(original->size() == QSize(800, 600));
    CHECK(canvas->document()->size() == image.size());
    CHECK(canvas->document()->activeLayer()->image() == image);
    CHECK(!canvas->document()->history().canUndo());
}
static void clipboardVariants() {
    QTemporaryDir dir;
    const QString path = dir.path() + QString::fromUtf8("/file with spaces é猫.png");
    const QString webp = dir.path() + "/image.webp";
    const QString bad = dir.path() + "/not-an-image.txt";
    const QImage image = fixture();
    CHECK(image.save(path));
    CHECK(QImageReader::supportedImageFormats().contains("webp"));
    CHECK(image.save(webp, "WEBP"));
    { QFile file(bad); CHECK(file.open(QIODevice::WriteOnly)); file.write("not an image"); }
    for (int variant = 0; variant < 7; ++variant) {
        MainWindow w;
        auto *canvas = w.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        auto *mime = new QMimeData;
        switch (variant) {
        case 0: mime->setImageData(image); break;
        case 1: mime->setUrls({QUrl::fromLocalFile(path)}); break;
        case 2: mime->setData("text/uri-list", QUrl::fromLocalFile(path).toEncoded()+"\r\n"); break;
        case 3: mime->setText(path); break;
        case 4: mime->setText(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)); break;
        case 5: mime->setUrls({QUrl("https://example.invalid/a.png"), QUrl::fromLocalFile(bad),
                             QUrl::fromLocalFile(dir.path()+"/missing.png"), QUrl::fromLocalFile(path), QUrl::fromLocalFile(webp)}); break;
        case 6: mime->setText(webp); break;
        }
        QApplication::clipboard()->setMimeData(mime);
        noPrompt(w, variant % 2 ? "pasteIntoNewLayer" : "paste");
        CHECK(doc->layerCount() == 2);
        const QImage expected = variant == 6 ? QImage(webp) : image;
        CHECK(doc->activeLayer()->image().copy(expected.rect()) == expected.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        CHECK(doc->activeLayer()->image().size() == doc->size());
    }
    for (const QString &invalid : {QString("ordinary text"), QString("./relative.png"),
            dir.path()+"/missing.png", bad, dir.path(), QString("https://example.invalid/image.png"),
            QString("file://remote.invalid/image.png"), QString("$(touch /should-not-exist)")}) {
        MainWindow w;
        auto *canvas = w.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        const QImage before = doc->flatten();
        const int history = doc->history().currentIndex();
        const bool modified = doc->isModified();
        QApplication::clipboard()->setText(invalid);
        noPrompt(w,"paste");
        CHECK(doc->flatten() == before); CHECK(doc->layerCount() == 1);
        CHECK(doc->history().currentIndex() == history); CHECK(doc->isModified() == modified);
        noPrompt(w,"pasteIntoNewImage");
        CHECK(canvas->document() == doc);
    }
    // Image data takes priority over an accompanying file-manager reference.
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *mime = new QMimeData;
    const QImage native = fixture(50,40);
    mime->setImageData(native); mime->setUrls({QUrl::fromLocalFile(path)});
    QApplication::clipboard()->setMimeData(mime);
    noPrompt(w,"paste");
    CHECK(canvas->document()->activeLayer()->image().copy(native.rect()) == native);
    for (QSize size : {QSize(800,600), QSize(100,600), QSize(800,100), QSize(40,30)}) {
        const QImage next = fixture(size.width(), size.height());
        QApplication::clipboard()->setImage(next);
        noPrompt(w,"paste");
        CHECK(canvas->document()->size() == QSize(800,600));
        CHECK(canvas->document()->activeLayer()->image().copy(next.rect()) == next);
    }
}
static void typingTextPaste() {
    QTemporaryDir dir;
    const QString path = dir.path()+"/typing image.png";
    CHECK(fixture().save(path));
    MainWindow w;
    auto *canvas = w.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    CHECK(QMetaObject::invokeMethod(&w,"selectTool",Qt::DirectConnection,Q_ARG(ToolType,ToolType::Text)));
    auto *tool = canvas->currentTool();
    QMouseEvent press(QEvent::MouseButtonPress,QPointF(80,80),QPointF(80,80),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    tool->mousePressEvent(QPointF(80,80),&press,*canvas);
    CHECK(tool->wantsKeyInput());
    const QImage before = doc->activeLayer()->image();
    QApplication::clipboard()->setText(path);
    noPrompt(w,"paste");
    CHECK(canvas->currentTool() == tool); CHECK(doc->layerCount() == 1);
    CHECK(doc->activeLayer()->image() == before);
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    tool->keyPressEvent(&enter,*canvas);
    CHECK(!tool->wantsKeyInput());
    CHECK(doc->activeLayer()->image() != before);
    CHECK(doc->history().currentIndex() == 1);
}
static void pendingToolPaste() {
    for (ToolType type : {ToolType::Line, ToolType::Shape}) {
        MainWindow w;
        auto *canvas=w.findChild<CanvasWidget *>();
        auto *doc=canvas->document();
        CHECK(QMetaObject::invokeMethod(&w,"selectTool",Qt::DirectConnection,Q_ARG(ToolType,type)));
        auto *tool=canvas->currentTool();
        QMouseEvent press(QEvent::MouseButtonPress,QPointF(20,20),QPointF(20,20),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove,QPointF(80,60),QPointF(80,60),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,QPointF(80,60),QPointF(80,60),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        tool->mousePressEvent(QPointF(20,20),&press,*canvas);
        tool->mouseMoveEvent(QPointF(80,60),&move,*canvas);
        tool->mouseReleaseEvent(QPointF(80,60),&release,*canvas);
        const QImage pasted=fixture(840,630);
        choosePaste(w,pasted,1);
        CHECK(doc->activeLayer()->image()==pasted);
        CHECK(canvas->currentTool()->type()==ToolType::Move);
        CHECK(doc->layerAt(0)->image().pixelColor(20,20)!=QColor(Qt::white));
    }
}
int main(int argc, char **argv) {
    // Real MainWindow integration must never read/write a developer's UI state
    // or autosave recovery files, including when this executable is run alone.
    QTemporaryDir settings;
    if (!settings.isValid()) return 2;
    for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
        const QString path = settings.path() + '/' + QString::fromLatin1(name);
        QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(name, path.toUtf8());
    }
    QApplication app(argc, argv);
    QSettings("PaintDali", "PaintDali").setValue("ui/language", "en");
    I18n::setLanguage(I18n::Lang::English);
    paths();
    cancelPrompt();
    expandPaste();
    keepPaste();
    retainedMove();
    matchedCanvasRetention();
    newImagePath();
    retainedWand();
    retainedFill();
    retainedPainting();
    retainedSelectedMove();
    clipboardVariants();
    typingTextPaste();
    pendingToolPaste();
    std::cout << checks << " paste checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
