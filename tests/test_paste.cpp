#include <QApplication>
#include <QMessageBox>
#include <QFileDialog>
#include <QMenuBar>
#include <QMenu>
#include <QLayout>
#include "core/floatingrendercache.h"
#include "panels/imagelistbar.h"
#include "panels/layerspanel.h"
#include "dialogs/layerpropertiesdialog.h"
#include "core/adjustmentpreview.h"
#include <QListWidget>
#include <QToolButton>
#include <QElapsedTimer>
#include <algorithm>
#include <numeric>
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
#include <QPushButton>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineF>
#include <QEnterEvent>
#include <QStyleFactory>
#include <QStyle>
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include "dialogs/pastedialog.h"
#include "dialogs/settingsdialog.h"
#include "theme.h"
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
#include "core/clipboardpixels.h"
#include "dialogs/previewdialog.h"


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
    CHECK(doc->layerCount() == 1);
    CHECK(doc->floatingPixelSession());
    CHECK(doc->flatten().pixelColor(39, 29) == QColor(Qt::red));
    CHECK(doc->activeLayer()->image().size() == doc->size());
}

static void utilityPanelPreferences() {
    QSettings settings("PaintDali", "PaintDali");
    settings.remove("ui/followUtilityPanels");
    settings.remove("ui/hideUtilityPanelsOnDeactivate");

    SettingsDialog dialog;
    auto *follow = dialog.findChild<QCheckBox *>("followUtilityPanels");
    auto *hide = dialog.findChild<QCheckBox *>("hideUtilityPanelsOnDeactivate");
    CHECK(follow != nullptr);
    CHECK(hide != nullptr);
    if (!follow || !hide) {
        settings.remove("ui/followUtilityPanels");
        settings.remove("ui/hideUtilityPanelsOnDeactivate");
        return;
    }

    CHECK(!follow->isChecked());
    CHECK(!hide->isChecked());
    int changes = 0;
    QObject::connect(&dialog, &SettingsDialog::panelBehaviorChanged,
                     [&changes]() { ++changes; });
    follow->setChecked(true);
    hide->setChecked(true);
    CHECK(settings.value("ui/followUtilityPanels").toBool());
    CHECK(settings.value("ui/hideUtilityPanelsOnDeactivate").toBool());
    CHECK(changes == 2);

    // The opt-in auto-hide must restore visible panels, but not panels the user
    // had already hidden themselves.
    settings.setValue("ui/hideUtilityPanelsOnDeactivate", false);
    MainWindow window;
    auto *tools = window.findChild<QDockWidget *>("ToolsDock");
    auto *colors = window.findChild<QDockWidget *>("ColorsDock");
    CHECK(tools != nullptr);
    CHECK(colors != nullptr);
    if (!tools || !colors) {
        settings.remove("ui/followUtilityPanels");
        settings.remove("ui/hideUtilityPanelsOnDeactivate");
        return;
    }
    window.show();
    tools->show();
    colors->hide();
    QApplication::processEvents();
    CHECK(tools->window()->isVisible());
    CHECK(!colors->isVisible());

    const bool followInvoked = QMetaObject::invokeMethod(
        &window, "repositionFloatingUtilityWindows", Qt::DirectConnection);
    CHECK(followInvoked);
    QRect mainFrame = window.frameGeometry();
    CHECK(tools->window()->frameGeometry().topLeft() == mainFrame.topLeft() + QPoint(65, 164));
    window.move(mainFrame.topLeft() + QPoint(40, 30));
    QApplication::processEvents();
    const bool followAfterMove = QMetaObject::invokeMethod(
        &window, "repositionFloatingUtilityWindows", Qt::DirectConnection);
    CHECK(followAfterMove);
    mainFrame = window.frameGeometry();
    CHECK(tools->window()->frameGeometry().topLeft() == mainFrame.topLeft() + QPoint(65, 164));

    settings.setValue("ui/hideUtilityPanelsOnDeactivate", true);
    const bool inactiveInvoked = QMetaObject::invokeMethod(
        &window, "onApplicationStateChanged", Qt::DirectConnection,
        Q_ARG(Qt::ApplicationState, Qt::ApplicationInactive));
    CHECK(inactiveInvoked);
    CHECK(!tools->window()->isVisible());

    const bool activeInvoked = QMetaObject::invokeMethod(
        &window, "onApplicationStateChanged", Qt::DirectConnection,
        Q_ARG(Qt::ApplicationState, Qt::ApplicationActive));
    CHECK(activeInvoked);
    CHECK(tools->window()->isVisible());
    CHECK(!colors->isVisible());
    settings.remove("ui/followUtilityPanels");
    settings.remove("ui/hideUtilityPanelsOnDeactivate");
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
            const auto rows = dlg->findChildren<QPushButton *>();
            CHECK(rows.size() == 3);
            if (rows.size() == 3) {
                CHECK(rows[0]->text() == "Expand canvas");
                CHECK(rows[0]->accessibleDescription() == "Automatically expands the canvas to fit the image being pasted.");
                CHECK(rows[1]->text() == "Keep canvas size");
                CHECK(rows[1]->accessibleDescription() == "Does not expand the canvas. You will have to move the pasted image around to make sure that the part you want is within the canvas boundaries.");
                CHECK(rows[2]->text() == "Cancel");
                CHECK(rows[2]->accessibleDescription() == "Cancels the paste action.");
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
// Retained-raster regressions explicitly use New Layer + Finish. Live floating
// Paste is exercised separately below; do not depend on the former Ctrl+V bug.
static void choosePaste(MainWindow &w, const QImage &image, int choice,
                        const char *slot = "pasteIntoNewLayer", bool finish = true) {
    bool seen = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        CHECK(dlg != nullptr);
        if (!dlg) return;
        seen = true;
        const auto rows = dlg->findChildren<QPushButton *>();
        CHECK(rows.size() == 3);
        if (rows.size() == 3) rows[choice]->click(); else dlg->reject();
    });
    QApplication::clipboard()->setImage(image);
    timer.start(30);
    invoke(w, slot);
    timer.stop();
    CHECK(seen);
    if (finish) invoke(w, "deselectAll");
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
        choosePaste(w, pasted, 0, "pasteIntoNewLayer", false);
        CHECK(doc->size() == QSize(qMax(original.width(), size.width()), qMax(original.height(), size.height())));
        CHECK(doc->layerCount() == 2);
        CHECK(doc->layerAt(0)->image().copy(before.rect()) == before);
        CHECK(doc->snapshotEffectiveLayers()[1]->image().pixelColor(size.width()-1, size.height()-1) == QColor(Qt::red));
        CHECK(doc->history().currentIndex() == 1);
        CHECK(doc->selection().hasSelection());
        CHECK(canvas->currentTool()->type() == ToolType::Move);
        doc->history().undo();
        CHECK(doc->size() == original); CHECK(doc->layerCount() == 1);
        CHECK(doc->layerAt(0)->image() == before);
        CHECK(doc->selection().mask() == mask);
        doc->history().redo();
        CHECK(doc->layerCount() == 2);
        CHECK(doc->activeLayerIndex() == 1);
        CHECK(doc->selection().hasSelection());
    }
}
static void drag(CanvasWidget *canvas, QPoint start, QPoint delta,
                 Qt::KeyboardModifiers modifiers = Qt::NoModifier, Qt::MouseButton button = Qt::LeftButton) {
    auto *tool = canvas->currentTool();
    // Existing drag fixtures move artwork; pivot gestures are tested explicitly.
    if (tool->type() == ToolType::Move && button == Qt::LeftButton) {
        const auto *session = canvas->document()->floatingPixelSession();
        const QRect rect = session ? session->sourceBounds().toAlignedRect() : canvas->document()->selection().boundingRect();
        const QPointF pivot = session ? session->rotationPivot() : QRectF(rect).center();
        if (!rect.isEmpty() && QLineF(start,pivot).length() <= SelHandles::pivotRadius(rect,canvas->zoom()))
            start.rx() -= qMax(1,rect.width()/5);
    }
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start), QPointF(start), button, button, modifiers);
    QMouseEvent move(QEvent::MouseMove, QPointF(start+delta), QPointF(start+delta), Qt::NoButton, button, modifiers);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(start+delta), QPointF(start+delta), button, Qt::NoButton, modifiers);
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
    CHECK(doc->history().currentIndex() == 2);
    doc->history().undo(); // Finish
    doc->history().undo();
    CHECK(doc->layerCount() == 1); CHECK(doc->activeLayerIndex() == 0);
    doc->history().redo();
    doc->history().redo(); // Finish
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
    CHECK(doc->flatten().pixelColor(125,115) == QColor(Qt::red));
    const auto effective = doc->snapshotEffectiveLayers()[doc->activeLayerIndex()];
    CHECK(effective->image().pixelColor(QPoint(100,90)-effective->offset()).alpha() == 0);
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
        CHECK(doc->layerCount() == (variant % 2 ? 2 : 1));
        const QImage expected = variant == 6 ? QImage(webp) : image;
        CHECK(doc->snapshotEffectiveLayers()[doc->activeLayerIndex()]->image().copy(expected.rect()) == expected.convertToFormat(QImage::Format_ARGB32_Premultiplied));
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
    CHECK(canvas->document()->snapshotEffectiveLayers()[0]->image().copy(native.rect()) == native);
    for (QSize size : {QSize(800,600), QSize(100,600), QSize(800,100), QSize(40,30)}) {
        const QImage next = fixture(size.width(), size.height());
        QApplication::clipboard()->setImage(next);
        noPrompt(w,"paste");
        CHECK(canvas->document()->size() == QSize(800,600));
        CHECK(canvas->document()->snapshotEffectiveLayers()[0]->image().copy(next.rect()) == next);
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
static void fullCanvasView(CanvasWidget *canvas) {
    canvas->resize(1000, 800);
    canvas->setShowRulers(false);
    canvas->setZoom(1);
    canvas->setPan(QPointF());
}

static void floatingPasteGestures() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    fullCanvasView(canvas);
    QImage image = fixture(80, 60);
    image.setPixelColor(10, 10, Qt::transparent);
    doc->activeLayer()->image().setPixelColor(110, 90, Qt::green);
    const QImage before = doc->flatten();
    QApplication::clipboard()->setImage(image);
    noPrompt(window, "paste");
    CHECK(doc->layerCount() == 1 && doc->activeLayerIndex() == 0);
    CHECK(doc->floatingPixelSession() && !doc->floatingPixelSession()->isSuspended());
    CHECK(canvas->currentTool()->type() == ToolType::Move);
    CHECK(doc->selection().boundingRect() == image.rect());
    CHECK(doc->flatten().copy(image.rect()) == image);
    const uchar *source = doc->floatingPixelSession()->sourcePixels().constBits();
    drag(canvas, QPoint(40, 30), QPoint(100, 80));
    CHECK(doc->flatten().pixel(110, 90) == 0);
    drag(canvas, QPoint(140, 110), QPoint(140, 20));
    CHECK(doc->flatten().pixelColor(110, 90) == QColor(Qt::green));
    CHECK(doc->flatten().pixel(250, 110) == 0);
    CHECK(doc->floatingPixelSession()->sourcePixels().constBits() == source);
    CHECK(doc->history().currentIndex() == 3);

    const QRect frame = doc->floatingPixelSession()->sourceBounds().toAlignedRect();
    drag(canvas, frame.bottomRight(), QPoint(40, 30));
    CHECK(doc->floatingPixelSession()->sourceBounds().width() > frame.width());
    CHECK(doc->floatingPixelSession()->sourcePixels().constBits() == source);
    CHECK(doc->history().undoDescription() == "Resize Selected Pixels");
    invoke(window, "undo");
    CHECK(doc->floatingPixelSession()->sourceBounds().toAlignedRect() == frame);
    const QPoint centre = QRectF(frame).center().toPoint();
    drag(canvas, centre + QPoint(60, 0), QPoint(-60, 60), Qt::NoModifier, Qt::RightButton);
    CHECK(qAbs(doc->floatingPixelSession()->sourceBounds().width() - frame.height()) < 0.001);
    CHECK(qAbs(doc->floatingPixelSession()->sourceBounds().height() - frame.width()) < 0.001);
    CHECK(doc->history().undoDescription() == "Rotate Selection");
    invoke(window, "undo");
    const int history = doc->history().currentIndex();
    CHECK(doc->history().canRedo());
    const QImage noOp = doc->flatten();
    drag(canvas, centre, QPoint(), Qt::ControlModifier);
    CHECK(doc->history().currentIndex() == history && doc->history().canRedo());
    CHECK(doc->flatten() == noOp && doc->floatingPixelSession()->sourcePixels().constBits() == source);
    drag(canvas, centre, QPoint(120, 0), Qt::ControlModifier);
    CHECK(doc->flatten().pixelColor(319, 159) == QColor(Qt::red));
    CHECK(doc->flatten().pixelColor(439, 159) == QColor(Qt::red));
    invoke(window, "undo");
    CHECK(doc->flatten().pixelColor(439, 159) == QColor(Qt::white));
    invoke(window, "redo");
    const QImage visible = doc->flatten();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(canvas, &escape);
    CHECK(!doc->floatingPixelSession() && !doc->selection().hasSelection());
    CHECK(doc->flatten() == visible);
    invoke(window, "undo");
    CHECK(doc->floatingPixelSession() && doc->flatten() == visible);
    const double oldX = doc->floatingPixelSession()->transform().dx();
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::ControlModifier);
    QApplication::sendEvent(canvas, &right);
    CHECK(doc->floatingPixelSession()->transform().dx() == oldX + 10);
    CHECK(!doc->history().canRedo());
    invoke(window, "deselectAll");
    CHECK(!doc->floatingPixelSession() && !doc->selection().hasSelection());
    doc->history().goToIndex(0);
    CHECK(doc->flatten() == before && doc->layerCount() == 1);
}

static void cropWorkflow() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    fullCanvasView(canvas);
    const QSize originalSize = doc->size();
    QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);
    QApplication::clipboard()->setImage(image);
    noPrompt(window, "paste");
    drag(canvas, QPoint(40,30), QPoint(100,80));
    const QRect selection = doc->selection().boundingRect();
    const int history = doc->history().currentIndex();
    invoke(window, "cropToSelection");
    CHECK(doc->size() == selection.size());
    CHECK(!doc->selection().hasSelection() && !doc->floatingPixelSession());
    CHECK(doc->history().currentIndex() == history + 2);
    invoke(window, "undo");
    CHECK(doc->size() == originalSize && doc->floatingPixelSession());
    CHECK(doc->floatingPixelSession()->isSuspended());
    drag(canvas, QPoint(140,110), QPoint(100,0));
    CHECK(doc->flatten().pixelColor(140,110).alpha() == 0);
    CHECK(doc->flatten().pixelColor(240,110) == QColor(Qt::red));
    invoke(window, "undo"); // movement
    invoke(window, "undo"); // Finish
    CHECK(doc->floatingPixelSession() && !doc->floatingPixelSession()->isSuspended());
    drag(canvas, QPoint(140,110), QPoint(100,0));
    CHECK(doc->flatten().pixelColor(140,110) == QColor(Qt::white));

    // Crop also settles a legacy selected-pixel gesture without migrating it.
    doc->finishFloatingPixels();
    doc->selection().selectRect(QRect(200,80,80,60));
    auto *tool = canvas->currentTool();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(240,110), QPointF(240,110),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPointF(260,120), QPointF(260,120),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    tool->mousePressEvent(QPointF(240,110), &press, *canvas);
    tool->mouseMoveEvent(QPointF(260,120), &move, *canvas);
    const QRect movedSelection = doc->selection().boundingRect();
    const QImage beforeCrop = doc->flatten().copy(movedSelection);
    invoke(window, "cropToSelection");
    CHECK(doc->size() == movedSelection.size() && doc->flatten() == beforeCrop);
    invoke(window, "undo");
    CHECK(doc->selection().boundingRect() == movedSelection && doc->floatingPixelSession());

    MainWindow queuedWindow;
    auto *queuedCanvas = queuedWindow.findChild<CanvasWidget *>();
    auto *queuedDoc = queuedCanvas->document();
    fullCanvasView(queuedCanvas);
    QApplication::clipboard()->setImage(image);
    noPrompt(queuedWindow, "paste");
    auto send = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF widget = queuedCanvas->canvasToWidget(point);
        QMouseEvent event(type, widget, widget, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(queuedCanvas, &event);
    };
    send(QEvent::MouseButtonPress, QPoint(24,30), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(784,30), Qt::NoButton, Qt::LeftButton);
    CHECK(queuedDoc->floatingPixelSession()->transform() == QTransform());
    invoke(queuedWindow, "cropToSelection");
    CHECK(queuedDoc->size() == QSize(40,60)); // latest pose crosses the right canvas edge
    CHECK(queuedDoc->flatten().pixelColor(20,30) == QColor(Qt::red));
    CHECK(queuedDoc->history().currentIndex() == 4); // Paste, Move, Finish, Crop
    invoke(queuedWindow, "undo");
    CHECK(queuedDoc->selection().boundingRect() == QRect(760,0,40,60));
    queuedCanvas->flushFloatingMove();
    CHECK(queuedDoc->selection().boundingRect() == QRect(760,0,40,60));
}

static void floatingResamplingControls() {
    using Sampling=FloatingPixelSession::Sampling;
    MainWindow window;
    auto *canvas=window.findChild<CanvasWidget *>(); fullCanvasView(canvas);
    auto *doc=canvas->document();
    QApplication::clipboard()->setImage(fixture(80,60)); noPrompt(window,"paste");
    auto *combo=window.findChild<QComboBox *>("floatingResampling");
    CHECK(combo && combo->count()==3 && combo->currentIndex()==int(Sampling::Bicubic));
    CHECK(doc->floatingPixelSession()->sampling()==Sampling::Bicubic);
    CHECK(doc->setFloatingPixelTransform(QTransform().rotate(17)*QTransform::fromTranslate(80.25,60.5)));
    const QImage source=doc->floatingPixelSession()->sourcePixels();
    const QImage smooth=doc->flatten();
    const int history=doc->history().currentIndex();
    combo->setCurrentIndex(int(Sampling::Nearest));
    CHECK(doc->floatingPixelSession()->sampling()==Sampling::Nearest && doc->flatten()!=smooth);
    CHECK(doc->history().currentIndex()==history+1);
    invoke(window,"undo");
    CHECK(combo->currentIndex()==int(Sampling::Bicubic) && doc->flatten()==smooth);
    invoke(window,"redo");
    CHECK(combo->currentIndex()==int(Sampling::Nearest));
    CHECK(doc->floatingPixelSession()->sourcePixels().cacheKey()==source.cacheKey());
    combo->setCurrentIndex(int(Sampling::Bilinear));
    CHECK(doc->floatingPixelSession()->sampling()==Sampling::Bilinear);
    invoke(window,"copy");
    const auto copied=ClipboardPixels::decode(*QApplication::clipboard()->mimeData());
    CHECK(copied && copied->coverage==doc->floatingPixelSession()->renderCoverage(doc->selection().boundingRect()));
    QTemporaryDir dir;
    const QString path=dir.filePath("filtered.psw");
    CHECK(doc->saveNative(path));
    MainWindow reopened;
    reopened.openFile(path);
    auto *restored=reopened.findChild<CanvasWidget *>()->document();
    CHECK(restored->floatingPixelSession()->sampling()==Sampling::Bilinear
          && reopened.findChild<QComboBox *>("floatingResampling")->currentIndex()==int(Sampling::Bilinear));
    // A preference change with no session applies to the next paste/lift.
    invoke(window,"deselectAll");
    combo->setCurrentIndex(int(Sampling::Nearest));
    QApplication::clipboard()->setImage(fixture(80,60)); noPrompt(window,"paste");
    CHECK(canvas->document()->floatingPixelSession()->sampling()==Sampling::Nearest);
}

static void floatingContextAndClipboard() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    fullCanvasView(canvas);
    QImage image(80, 60, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::magenta);
    QApplication::clipboard()->setImage(image);
    noPrompt(window, "paste");
    drag(canvas, QPoint(40, 30), QPoint(100, 80));
    CHECK(QMetaObject::invokeMethod(&window, "selectTool", Qt::DirectConnection, Q_ARG(ToolType, ToolType::Brush)));
    CHECK(doc->floatingPixelSession()->isSuspended() && doc->selection().hasSelection());
    auto *brush = canvas->currentTool();
    brush->setBrushSize(5);
    brush->setHardness(100);
    doc->setPrimaryColor(Qt::black);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(115.5, 95.5), QPointF(115.5, 95.5), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(115.5, 95.5), QPointF(115.5, 95.5), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    brush->mousePressEvent(QPointF(115.5, 95.5), &press, *canvas);
    brush->mouseReleaseEvent(QPointF(115.5, 95.5), &release, *canvas);
    CHECK(doc->flatten().pixelColor(115, 95) == QColor(Qt::black));
    CHECK(QMetaObject::invokeMethod(&window, "selectTool", Qt::DirectConnection, Q_ARG(ToolType, ToolType::Move)));
    drag(canvas, QPoint(140, 110), QPoint(140, 20));
    CHECK(doc->flatten().pixel(115, 95) == 0);
    CHECK(doc->flatten().pixelColor(255, 115) == QColor(Qt::black));

    // Copy uses the live source, not the committed raster hidden beneath it.
    invoke(window, "copy");
    const auto copied = ClipboardPixels::decode(*QApplication::clipboard()->mimeData());
    CHECK(copied && copied->pixels.pixelColor(15, 15) == QColor(Qt::black));
    MainWindow target;
    auto *targetCanvas = target.findChild<CanvasWidget *>();
    fullCanvasView(targetCanvas);
    auto *targetDoc = targetCanvas->document();
    targetDoc->addLayer("Target");
    targetDoc->activeLayer()->clear(Qt::blue);
    noPrompt(target, "paste");
    CHECK(targetDoc->layerCount() == 2 && targetDoc->activeLayerIndex() == 1);
    CHECK(targetDoc->floatingPixelSession()->sourcePixels() == copied->pixels);
    CHECK(targetDoc->floatingPixelSession()->sourceCoverage() == copied->coverage);
    const int history = targetDoc->history().currentIndex();
    const QImage visible = targetDoc->flatten();
    const QTransform transform = targetDoc->floatingPixelSession()->transform();
    QTimer::singleShot(0, [&]() {
        auto *dialog = qobject_cast<PreviewDialog *>(QApplication::activeModalWidget());
        CHECK(dialog != nullptr);
        if (dialog) dialog->reject();
    });
    CHECK(QMetaObject::invokeMethod(&target, "applyAdjustment", Qt::DirectConnection, Q_ARG(int, 0)));
    CHECK(targetDoc->history().currentIndex() == history && targetDoc->flatten() == visible);
    CHECK(!targetDoc->floatingPixelSession()->isSuspended() && targetDoc->floatingPixelSession()->transform() == transform);
    targetDoc->setPrimaryColor(Qt::green);
    invoke(target, "fillSelection");
    CHECK(targetDoc->floatingPixelSession()->isSuspended());
    CHECK(targetDoc->flatten().pixelColor(targetDoc->selection().boundingRect().topLeft() + QPoint(20,20)) == QColor(Qt::green));
    invoke(target, "undo");
    CHECK(targetDoc->floatingPixelSession() && !targetDoc->floatingPixelSession()->isSuspended()
          && targetDoc->flatten() == visible);
    invoke(target, "cut");
    CHECK(!targetDoc->floatingPixelSession() && !targetDoc->selection().hasSelection());
    invoke(target, "undo");
    CHECK(targetDoc->floatingPixelSession() && targetDoc->flatten() == visible);
    noPrompt(target, "pasteIntoNewLayer");
    CHECK(targetDoc->layerCount() == 3 && targetDoc->activeLayerIndex() == 2);
    CHECK(targetDoc->floatingPixelSession()->targets(targetDoc->layerAt(2)));
}

// Stage 6 acceptance audit: placement is document-space and independent of
// clipboard geometry, while each document retains its own editable source.
static void floatingPlacementAndSoftClipboard() {
    MainWindow source;
    auto *canvas = source.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    canvas->resize(480, 360);
    canvas->setZoom(4.0);
    canvas->setPan(QPointF(-400, -280));
    const QRect view = canvas->rect().adjusted(canvas->rulerSize(), canvas->rulerSize(), 0, 0);
    QPoint expected = canvas->widgetToCanvas(view.topLeft()).toPoint();
    expected.setX(qBound(0, expected.x(), doc->width()-40));
    expected.setY(qBound(0, expected.y(), doc->height()-30));
    CHECK(expected.x() > 0 && expected.y() > 0);
    QApplication::clipboard()->setImage(fixture());
    noPrompt(source, "paste");
    CHECK(doc->floatingPixelSession()->transform() == QTransform::fromTranslate(expected.x(), expected.y()));
    invoke(source, "undo");
    CHECK(!doc->floatingPixelSession());
    invoke(source, "redo");
    CHECK(doc->floatingPixelSession()->transform() == QTransform::fromTranslate(expected.x(), expected.y()));

    MainWindow target;
    auto *targetCanvas = target.findChild<CanvasWidget *>();
    fullCanvasView(targetCanvas);
    auto *other = targetCanvas->document();
    // An irregular soft mask includes selected fully transparent source pixels.
    QImage pixels = fixture(); pixels.setPixel(12, 10, 0);
    QImage coverage(pixels.size(), QImage::Format_Grayscale8); coverage.fill(0);
    for (int y=3; y<26; ++y) for (int x=3; x<35; ++x)
        if ((x+y)%7) coverage.scanLine(y)[x] = uchar(32 + (x*17+y*11)%224);
    coverage.scanLine(10)[12] = 255;
    CHECK(doc->pasteFloatingPixels(pixels, coverage, QPoint(10, 15)));
    const QImage originalSource = doc->floatingPixelSession()->sourcePixels();
    const QImage originalCoverage = doc->floatingPixelSession()->sourceCoverage();
    invoke(source, "copy");
    const auto copied = ClipboardPixels::decode(*QApplication::clipboard()->mimeData());
    const QRect selected(3,3,32,23);
    CHECK(copied && copied->coverage == coverage.copy(selected));
    CHECK(copied && copied->pixels.pixel(9,7) == 0
          && copied->pixels.pixel(1,1) == pixels.pixel(4,4));
    noPrompt(target, "paste");
    CHECK(other->floatingPixelSession()->sourceCoverage() == coverage.copy(selected));
    CHECK(other->floatingPixelSession()->sourcePixels().pixel(9,7) == 0);
    CHECK(other->setFloatingPixelTransform(QTransform().rotate(19)*QTransform::fromTranslate(80,60)));
    CHECK(doc->floatingPixelSession()->sourcePixels() == originalSource
          && doc->floatingPixelSession()->sourceCoverage() == originalCoverage
          && doc->floatingPixelSession()->transform() == QTransform::fromTranslate(10,15));
    const QImage appearance = other->flatten();
    QTemporaryDir dir;
    CHECK(other->saveNative(dir.filePath("soft.psw")));
    Document reopened;
    CHECK(reopened.loadNative(dir.filePath("soft.psw")) && reopened.flatten() == appearance);
    CHECK(reopened.floatingPixelSession()->sourceCoverage() == coverage.copy(selected));
    const QImage beforeSelectAll = other->selection().mask();
    invoke(target, "selectAll");
    CHECK(!other->floatingPixelSession());
    invoke(target, "undo");
    CHECK(!other->floatingPixelSession() && other->selection().mask() == beforeSelectAll);
    invoke(target, "undo"); // Finish precedes the separate Select All history entry.
    CHECK(other->floatingPixelSession() && other->flatten() == appearance);
    CHECK(QMetaObject::invokeMethod(&target, "selectTool", Qt::DirectConnection,
                                   Q_ARG(ToolType, ToolType::MoveSelection)));
    CHECK(other->floatingPixelSession()->isSuspended());
    const QImage beforeOutline = other->flatten();
    const QRect outline = other->selection().boundingRect();
    QPoint grab = outline.center();
    bool found = false;
    for (int y=outline.top()+8; y<outline.bottom()-8 && !found; ++y)
        for (int x=outline.left()+8; x<outline.right()-8; ++x)
            if (other->selection().isSelected(x,y)
                && SelHandles::at(outline,QPointF(x,y),targetCanvas->zoom()) == SelHandles::Handle::None) {
                grab = QPoint(x,y); found = true; break;
            }
    CHECK(found);
    const QPointF start = targetCanvas->canvasToWidget(grab);
    const QPointF end = start + QPointF(8*targetCanvas->zoom(), 5*targetCanvas->zoom());
    QMouseEvent press(QEvent::MouseButtonPress,start,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(targetCanvas,&press);
    QMouseEvent move(QEvent::MouseMove,end,end,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(targetCanvas,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,end,end,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(targetCanvas,&release);
    CHECK(!other->floatingPixelSession() && other->flatten() == beforeOutline);
    CHECK(other->selection().boundingRect() == outline.translated(8,5));
    CHECK(QMetaObject::invokeMethod(&target, "selectTool", Qt::DirectConnection,
                                   Q_ARG(ToolType, ToolType::Move)));
    drag(targetCanvas,other->selection().boundingRect().center(),QPoint(10,0));
    CHECK(other->floatingPixelSession() && other->floatingPixelSession()->targets(other->activeLayer()));
}

static void copiedPastePlacement() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    fullCanvasView(canvas);
    QImage pixels = fixture(20,20);
    QImage mask(pixels.size(),QImage::Format_Grayscale8); mask.fill(255);
    CHECK(doc->pasteFloatingPixels(pixels,mask,QPoint(350,250)));
    invoke(window,"copy");
    noPrompt(window,"paste");
    CHECK(doc->floatingPixelSession()->transform() == QTransform::fromTranslate(350,250));
    // Pan the copied area entirely out of view, then paste without recopying.
    canvas->resize(400,300); canvas->setZoom(4); canvas->setPan(QPointF(-2000,-1400));
    const QRect view = canvas->rect().adjusted(canvas->rulerSize(),canvas->rulerSize(),0,0);
    const QRectF visible(canvas->widgetToCanvas(view.topLeft()),canvas->widgetToCanvas(view.bottomRight()));
    const auto copied = ClipboardPixels::decode(*QApplication::clipboard()->mimeData());
    CHECK(copied.has_value());
    if (!copied) return;
    const QPoint expected = copied->pasteOrigin(visible,doc->size());
    CHECK(expected != QPoint(350,250));
    noPrompt(window,"pasteIntoNewLayer");
    const QTransform placed = QTransform::fromTranslate(expected.x(),expected.y());
    CHECK(doc->floatingPixelSession()->transform() == placed && doc->layerCount() == 2);
    invoke(window,"undo");
    CHECK(doc->layerCount() == 1 && doc->floatingPixelSession()->transform() == QTransform::fromTranslate(350,250));
    invoke(window,"redo");
    CHECK(doc->layerCount() == 2 && doc->floatingPixelSession()->transform() == placed);
}

static void floatingCoalescing() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>(); fullCanvasView(canvas);
    auto *doc = canvas->document();
    QApplication::clipboard()->setImage(fixture(80, 60)); noPrompt(window, "paste");
    int updates = 0;
    QObject::connect(doc, &Document::floatingPixelsChanged, doc, [&](const QRegion &area) { if (!area.isEmpty()) ++updates; });
    auto send = [&](QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons) {
        const QPointF widget = canvas->canvasToWidget(point);
        QMouseEvent event(type, widget, widget, button, buttons, Qt::NoModifier); QApplication::sendEvent(canvas, &event);
    };
    const qint64 initialMask = doc->selection().mask().cacheKey();
    send(QEvent::MouseMove, QPoint(40, 30), Qt::NoButton, Qt::NoButton);
    send(QEvent::MouseButtonPress, QPoint(30, 30), Qt::LeftButton, Qt::LeftButton);
    canvas->flushFloatingMove();
    CHECK(updates == 0 && doc->selection().mask().cacheKey() == initialMask);
    for (int i = 1; i <= 200; ++i) send(QEvent::MouseMove, QPoint(30+i, 130), Qt::NoButton, Qt::LeftButton);
    CHECK(updates == 0); // positions wait for one frame; old positions do no work
    canvas->flushFloatingMove();
    CHECK(updates == 1 && doc->floatingPixelSession()->transform() == QTransform::fromTranslate(200, 100));
    const qint64 movedMask = doc->selection().mask().cacheKey();
    for (int i = 0; i < 20; ++i) { send(QEvent::MouseMove, QPoint(230, 130), Qt::NoButton, Qt::LeftButton); canvas->flushFloatingMove(); }
    CHECK(updates == 1 && doc->selection().mask().cacheKey() == movedMask);
    send(QEvent::MouseMove, QPoint(240, 140), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPoint(250, 150), Qt::LeftButton, Qt::NoButton);
    CHECK(updates == 2 && doc->floatingPixelSession()->transform() == QTransform::fromTranslate(220, 120));
    CHECK(doc->history().currentIndex() == 2);
    invoke(window, "undo");
    CHECK(doc->floatingPixelSession()->transform() == QTransform());
    invoke(window, "redo");
    CHECK(doc->floatingPixelSession()->transform() == QTransform::fromTranslate(220, 120));
    send(QEvent::MouseButtonPress, QPoint(240, 140), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(250, 150), Qt::NoButton, Qt::LeftButton);
    doc->history().undo(); // history panel/direct navigation discards pending input
    canvas->flushFloatingMove();
    CHECK(doc->floatingPixelSession()->transform() == QTransform());
    // Exercise the canvas's actual dirty-render cache, including jumps off-canvas.
    QImage partial(canvas->size(), QImage::Format_ARGB32_Premultiplied), full = partial;
    canvas->render(&partial);
    for (const QTransform transform : {QTransform::fromTranslate(180, 90),
             QTransform::fromScale(1.7, 0.8) * QTransform::fromTranslate(250, 170),
             QTransform().rotate(31) * QTransform::fromTranslate(-20, -15), QTransform::fromTranslate(2000, 2000)}) {
        CHECK(doc->setFloatingPixelTransform(transform));
        canvas->render(&partial);
        canvas->updateCanvas(); canvas->render(&full);
        CHECK(partial == full);
    }
    CHECK(doc->finishFloatingPixels());
    doc->activeLayer()->clear(Qt::white);
    doc->selection().selectRect(QRect(20,20,80,60));
    doc->history().clear();
    updates = 0;
    const qint64 initialOrdinaryMask = doc->selection().mask().cacheKey();
    send(QEvent::MouseButtonPress,QPoint(40,40),Qt::LeftButton,Qt::LeftButton);
    CHECK(doc->floatingPixelSession());
    const QImage lifted = doc->floatingPixelSession()->sourcePixels();
    send(QEvent::MouseMove,QPoint(40,40),Qt::NoButton,Qt::LeftButton);
    canvas->flushFloatingMove();
    CHECK(updates == 0 && doc->selection().mask().cacheKey() == initialOrdinaryMask);
    for (int i = 1; i <= 200; ++i)
        send(QEvent::MouseMove,QPoint(40+i,140),Qt::NoButton,Qt::LeftButton);
    CHECK(updates == 0);
    canvas->flushFloatingMove();
    CHECK(updates == 1 && doc->floatingPixelSession()->sourcePixels().cacheKey() == lifted.cacheKey());
    const qint64 ordinaryMask = doc->selection().mask().cacheKey();
    for (int i = 0; i < 20; ++i) {
        send(QEvent::MouseMove,QPoint(240,140),Qt::NoButton,Qt::LeftButton);
        canvas->flushFloatingMove();
    }
    CHECK(updates == 1 && doc->selection().mask().cacheKey() == ordinaryMask);
    send(QEvent::MouseButtonRelease,QPoint(250,150),Qt::LeftButton,Qt::NoButton);
    CHECK(doc->history().currentIndex() == 1);
    invoke(window,"undo");
    CHECK(!doc->floatingPixelSession() && doc->selection().boundingRect() == QRect(20,20,80,60));
}

static void floatingNativeAndDocumentSwitch() {
    QTemporaryDir dir;
    const QString native = dir.filePath("live.psw"), second = dir.filePath("second.png");
    CHECK(fixture(100, 90).save(second));
    {
        MainWindow window;
        auto *canvas = window.findChild<CanvasWidget *>();
        fullCanvasView(canvas);
        auto *first = canvas->document();
        QApplication::clipboard()->setImage(fixture(80, 60));
        noPrompt(window, "paste");
        CHECK(first->saveNative(native));
        const QImage visible = first->flatten();
        const QTransform transform = first->floatingPixelSession()->transform();
        window.openFile(second);
        auto *other = canvas->document();
        CHECK(other != first);
        CHECK(QMetaObject::invokeMethod(&window, "selectTool", Qt::DirectConnection, Q_ARG(ToolType, ToolType::Pencil)));
        other->activeLayer()->image().setPixelColor(1, 1, Qt::black);
        window.openFile(native);
        CHECK(canvas->document() == first && canvas->currentTool()->type() == ToolType::Move);
        CHECK(first->floatingPixelSession() && !first->floatingPixelSession()->isSuspended());
        CHECK(first->floatingPixelSession()->transform() == transform && first->flatten() == visible);
        CHECK(other->activeLayer()->image().pixelColor(1, 1) == QColor(Qt::black));
    }
    MainWindow reopened;
    reopened.openFile(native);
    auto *canvas = reopened.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    fullCanvasView(canvas);
    CHECK(doc->floatingPixelSession() && canvas->currentTool()->type() == ToolType::Move);
    CHECK(doc->selection().hasSelection() && !doc->history().canUndo());
    drag(canvas, QPoint(40, 30), QPoint(100, 80));
    CHECK(doc->flatten().pixelColor(20, 20) == QColor(Qt::white));
    CHECK(doc->history().currentIndex() == 1 && doc->isModified());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(canvas, &enter);
    CHECK(!doc->floatingPixelSession() && !doc->selection().hasSelection());
    invoke(reopened, "undo");
    CHECK(doc->floatingPixelSession());
}

static void floatingLayerAppearance() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>(); fullCanvasView(canvas);
    auto *doc = canvas->document(); doc->activeLayer()->clear(Qt::green);
    doc->addLayer("Offset");
    QImage base(100, 80, QImage::Format_ARGB32_Premultiplied); base.fill(Qt::blue);
    doc->activeLayer()->setImage(base); doc->activeLayer()->setOffset(QPoint(160, 140));
    QApplication::clipboard()->setImage(fixture(80, 60)); noPrompt(window, "paste");
    QImage incremental(canvas->size(), QImage::Format_ARGB32_Premultiplied), full = incremental;
    canvas->render(&incremental);
    auto *list = window.findChild<LayersPanel *>()->findChild<QListWidget *>();
    auto *row = list->item(0);
    auto *tab = window.findChild<ImageListBar *>()->findChild<QToolButton *>();
    int structural = 0;
    QObject::connect(doc, &Document::layersChanged, doc, [&]() { ++structural; });
    const auto warm = FloatingRenderCache::shared().stats();
    for (BlendMode mode : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Overwrite, BlendMode::Reflect}) {
        for (float opacity : {0.0f, 0.25f, 1.0f}) {
            doc->setLayerBlendMode(1, mode); doc->setLayerOpacity(1, opacity);
            canvas->render(&incremental); canvas->updateCanvas(); canvas->render(&full);
            CHECK(incremental == full); // full layer domain, including the bridge beyond the source
            CHECK(list->item(0) == row && window.findChild<ImageListBar *>()->findChild<QToolButton *>() == tab);
        }
    }
    CHECK(structural == 0 && FloatingRenderCache::shared().stats().misses == warm.misses);
    doc->setLayerVisibility(1, false); canvas->render(&incremental); canvas->updateCanvas(); canvas->render(&full);
    CHECK(incremental == full && row->checkState() == Qt::Unchecked);
    doc->setLayerVisibility(1, true);
    doc->setLayerBlendMode(1, BlendMode::Normal); doc->setLayerOpacity(1, 0.7f);
    const QImage before = doc->flatten(); const QImage mask = doc->selection().mask();
    const int history = doc->history().currentIndex();
    QTimer::singleShot(0, [&]() {
        auto *dialog = qobject_cast<LayerPropertiesDialog *>(QApplication::activeModalWidget()); CHECK(dialog);
        if (!dialog) return;
        CHECK(QMetaObject::invokeMethod(dialog, "previewChanged", Qt::DirectConnection,
              Q_ARG(int, 80), Q_ARG(bool, false), Q_ARG(int, int(BlendMode::Multiply))));
        canvas->render(&incremental); canvas->updateCanvas(); canvas->render(&full); CHECK(incremental == full);
        dialog->reject();
    });
    CHECK(QMetaObject::invokeMethod(window.findChild<LayersPanel *>(), "layerDoubleClicked", Qt::DirectConnection, Q_ARG(int, 1)));
    CHECK(doc->flatten() == before && doc->selection().mask() == mask && doc->history().currentIndex() == history);
    CHECK(!doc->floatingPixelSession()->isSuspended() && FloatingRenderCache::shared().stats().misses == warm.misses);
    AdjustmentPreview preview(doc, canvas, {1});
    doc->setLayerOpacity(1, 0.4f);
    CHECK(!preview.commit([](const QImage &image) { QImage changed = image; changed.fill(Qt::red); return changed; }, "stale"));
    CHECK(doc->history().currentIndex() == history); // property changes invalidate worker snapshots too
}

static void pasteDialogStyles() {
    const QString originalStyle = QApplication::style()->objectName();
    const QString originalSheet = qApp->styleSheet();
    const QPalette originalPalette = qApp->palette();
    const Theme::Scheme originalScheme = Theme::scheme();
    for (const QString &key : QStyleFactory::keys()) {
        std::cout << "Paste dialog style: " << key.toStdString() << '\n';
        for (Theme::Scheme scheme : {Theme::Scheme::Light,Theme::Scheme::Dark}) {
            qApp->setStyleSheet(QString());
            QApplication::setStyle(QStyleFactory::create(key));
            Theme::setScheme(scheme);
            qApp->setStyleSheet(Theme::styleSheet());
            for (I18n::Lang lang : {I18n::Lang::English,I18n::Lang::French}) {
                I18n::setLanguage(lang);
                PasteDialog dialog(fixture(840,630));
                dialog.show();
                dialog.activateWindow();
                QApplication::processEvents();
                const auto rows = dialog.findChildren<QPushButton *>();
                CHECK(rows.size() == 3);
                if (rows.size() != 3) continue;
                CHECK(rows[0]->text() == (lang == I18n::Lang::English ? "Expand canvas" : "Agrandir le canevas"));
                CHECK(rows[1]->text() == (lang == I18n::Lang::English ? "Keep canvas size" : "Conserver la taille du canevas"));
                CHECK(rows[2]->text() == (lang == I18n::Lang::English ? "Cancel" : "Annuler"));
                CHECK(rows[0]->isDefault() && rows[0]->hasFocus());
                const QStringList descriptions = lang == I18n::Lang::English
                    ? QStringList{"Automatically expands the canvas to fit the image being pasted.",
                        "Does not expand the canvas. You will have to move the pasted image around to make sure that the part you want is within the canvas boundaries.",
                        "Cancels the paste action."}
                    : QStringList{"Agrandit automatiquement le canevas pour contenir l'image à coller.",
                        "N'agrandit pas le canevas. Vous devrez déplacer l'image collée pour que la partie souhaitée se trouve dans les limites du canevas.",
                        "Annule l'action de collage."};
                for (int index=0;index<rows.size();++index) {
                    CHECK(rows[index]->accessibleDescription() == descriptions[index]);
                    bool heading = false, explanation = false;
                    for (auto *label : rows[index]->findChildren<QLabel *>()) {
                        heading |= label->text() == rows[index]->text();
                        explanation |= label->text() == descriptions[index];
                    }
                    CHECK(heading && explanation);
                }
                for (auto *row : rows) {
                    auto *accessible = QAccessible::queryAccessibleInterface(row);
                    CHECK(accessible && accessible->text(QAccessible::Name) == row->text());
                    CHECK(accessible && accessible->text(QAccessible::Description) == row->accessibleDescription());
                    CHECK(row->heightForWidth(300) >= row->heightForWidth(480));
                    row->clearFocus();
                    QEvent leave(QEvent::Leave);
                    QApplication::sendEvent(row,&leave);
                    const QImage idle = row->grab().toImage().copy(0,0,row->width(),3);
                    row->setFocus();
                    const QImage focused = row->grab().toImage().copy(0,0,row->width(),3);
                    CHECK(focused != idle);
                    row->clearFocus();
                    QEnterEvent enter(QPointF(2,2),QPointF(2,2),QPointF(row->mapToGlobal(QPoint(2,2))));
                    QApplication::sendEvent(row,&enter);
                    const QImage hovered = row->grab().toImage().copy(0,0,row->width(),3);
                    CHECK(hovered != idle);
                    QApplication::sendEvent(row,&leave);
                }
                rows[0]->setFocus();
                QKeyEvent tab(QEvent::KeyPress,Qt::Key_Tab,Qt::NoModifier);
                QApplication::sendEvent(&dialog,&tab);
                CHECK(rows[1]->hasFocus());
                QKeyEvent press(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier);
                QKeyEvent release(QEvent::KeyRelease,Qt::Key_Space,Qt::NoModifier);
                QApplication::sendEvent(rows[1],&press);
                QApplication::sendEvent(rows[1],&release);
                CHECK(dialog.result() == PasteDialog::Keep && !dialog.isVisible());
                dialog.show();
                QApplication::processEvents();
                rows[0]->setFocus();
                QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
                QApplication::sendEvent(&dialog,&enter);
                CHECK(dialog.result() == PasteDialog::Expand && !dialog.isVisible());
                dialog.show();
                QApplication::processEvents();
                const QPointF hit(rows[2]->width()-5,rows[2]->height()/2);
                QMouseEvent down(QEvent::MouseButtonPress,hit,hit,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                QMouseEvent up(QEvent::MouseButtonRelease,hit,hit,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(rows[2],&down);
                QApplication::sendEvent(rows[2],&up);
                CHECK(dialog.result() == PasteDialog::Cancel && !dialog.isVisible());
            }
        }
    }
    qApp->setStyleSheet(QString());
    QApplication::setStyle(QStyleFactory::create(originalStyle));
    qApp->setPalette(originalPalette);
    Theme::setScheme(originalScheme);
    qApp->setStyleSheet(originalSheet);
    I18n::setLanguage(I18n::Lang::English);
}

// Opt-in benchmark: the same real MainWindow/event/render path before and after
// optimization. No timing assertions in the functional regression suite.
static void benchmarkFloating() {
    for (QSize patch : {QSize(300, 200), QSize(2000, 1500)}) {
        MainWindow window;
        auto *canvas = window.findChild<CanvasWidget *>();
        auto *doc = canvas->document();
        doc->resize(4000, 3000);
        fullCanvasView(canvas);
        canvas->setZoom(0.2);
        QApplication::clipboard()->setImage(fixture(patch.width(), patch.height()));
        noPrompt(window, "paste");
        QImage screen(canvas->size(), QImage::Format_ARGB32_Premultiplied);
        canvas->render(&screen);
        auto send = [&](QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
            const QPointF widget = canvas->canvasToWidget(point);
            QMouseEvent event(type, widget, widget, button, buttons, Qt::NoModifier);
            QApplication::sendEvent(canvas, &event);
        };
        QPointF start(patch.width() / 2, patch.height() / 2);
        QElapsedTimer timer; timer.start();
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        canvas->render(&screen);
        const double press = timer.nsecsElapsed() / 1e6;
        std::vector<double> frames;
        for (int i = 1; i <= 8; ++i) {
            timer.restart();
            send(QEvent::MouseMove, start + QPointF(i * 15, i * 10), Qt::NoButton, Qt::LeftButton);
            // Deliver a scheduled frame, and include its work in the measurement.
            QEventLoop frame; QTimer::singleShot(17, &frame, &QEventLoop::quit); frame.exec();
            canvas->render(&screen);
            frames.push_back(timer.nsecsElapsed() / 1e6);
        }
        timer.restart();
        for (int i = 0; i < 20; ++i) {
            send(QEvent::MouseMove, start + QPointF(120, 80), Qt::NoButton, Qt::LeftButton);
            canvas->render(&screen);
        }
        const double stationary = timer.nsecsElapsed() / 1e6;
        send(QEvent::MouseButtonRelease, start + QPointF(120, 80), Qt::LeftButton, Qt::NoButton);
        std::sort(frames.begin(), frames.end());
        std::cout << "BENCH 4000x3000 patch=" << patch.width() << 'x' << patch.height()
                  << " press_ms=" << press << " frame_mean_ms=" << std::accumulate(frames.begin(), frames.end(), 0.0) / frames.size() << " frame_median_ms=" << frames[frames.size()/2]
                  << " frame_max_ms=" << frames.back() << " stationary_20_ms=" << stationary << std::endl;
        auto measure = [&](const char *operation, QPointF anchor, QPointF delta, Qt::MouseButton button) {
            send(QEvent::MouseButtonPress, anchor, button, button);
            std::vector<double> times;
            for (int i = 1; i <= 3; ++i) {
                timer.restart();
                send(QEvent::MouseMove, anchor + delta * i, Qt::NoButton, button);
                QEventLoop frame; QTimer::singleShot(17, &frame, &QEventLoop::quit); frame.exec();
                canvas->render(&screen);
                times.push_back(timer.nsecsElapsed() / 1e6);
            }
            send(QEvent::MouseButtonRelease, anchor + delta * 3, button, Qt::NoButton);
            std::sort(times.begin(), times.end());
            std::cout << "BENCH " << operation << " patch=" << patch.width() << 'x' << patch.height()
                      << " mean_ms=" << std::accumulate(times.begin(), times.end(), 0.0) / times.size() << " median_ms=" << times[1] << " max_ms=" << times.back() << std::endl;
        };
        measure("resize", doc->floatingPixelSession()->sourceBounds().toAlignedRect().bottomRight(), QPointF(20, 15), Qt::LeftButton);
        const QRectF bounds = doc->floatingPixelSession()->sourceBounds();
        measure("rotate", bounds.center() + QPointF(bounds.width()/3, 0), QPointF(-4, 12), Qt::RightButton);
    }
}

// Phase timings complement the event/render benchmark; kept opt-in so CI has
// no wall-clock pass/fail thresholds. Use identical builds for comparisons.
static void benchmarkStage4A(FloatingPixelSession::Sampling sampling=FloatingPixelSession::Sampling::Bicubic) {
    for (bool layered : {false, true}) {
        MainWindow window;
        auto *canvas = window.findChild<CanvasWidget *>(); auto *doc = canvas->document();
        CHECK(QMetaObject::invokeMethod(&window,"selectTool",Qt::DirectConnection,Q_ARG(ToolType,ToolType::Move)));
        window.findChild<QComboBox *>("floatingResampling")->setCurrentIndex(int(sampling));
        doc->resize(4000, 3000); fullCanvasView(canvas); canvas->setZoom(0.2);
        if (layered) {
            doc->addLayer("Target"); doc->activeLayer()->setOffset(QPoint(-25, 17));
            doc->addLayer("Upper"); doc->activeLayer()->clear(QColor(30, 120, 90, 70));
            doc->activeLayer()->setBlendMode(BlendMode::Multiply); doc->setActiveLayer(1);
        }
        QImage coverage(2000, 1500, QImage::Format_Grayscale8); coverage.fill(255);
        if (layered) { Selection shape(2000, 1500); shape.selectEllipse(QRect(0, 0, 1999, 1499)); coverage = shape.mask(); }
        auto mime = ClipboardPixels{fixture(2000, 1500), coverage, QPoint()}.mimeData();
        QApplication::clipboard()->setMimeData(mime.release()); noPrompt(window, "paste");
        doc->setFloatingPixelTransform(QTransform().rotate(17) * QTransform::fromTranslate(700, 250));
        QImage screen(canvas->size(), QImage::Format_ARGB32_Premultiplied); canvas->render(&screen);
        const auto *session = doc->floatingPixelSession(); const QRect canvasRect(QPoint(), doc->size());
        const QRect footprint = session->sourceBounds().intersected(QRectF(canvasRect)).toAlignedRect();
        auto measure = [&](const char *phase, auto operation) {
            std::vector<double> samples;
            for (int i = 0; i < 12; ++i) { QElapsedTimer timer; timer.start(); operation(i); samples.push_back(timer.nsecsElapsed()/1e6); }
            const double mean = std::accumulate(samples.begin(), samples.end(), 0.0)/samples.size();
            std::sort(samples.begin(), samples.end());
            std::cout << "PROFILE mode=" << int(sampling) << " layers=" << doc->layerCount() << " shape=" << (layered ? "ellipse" : "rectangle")
                      << " phase=" << phase << " mean_ms=" << mean << " p95_ms=" << samples.back()
                      << " max_ms=" << samples.back() << std::endl;
        };
        QImage result;
        measure("source_sampling", [&](int) { result = session->render(footprint); });
        measure("mask_generation", [&](int) { result = session->renderCoverage(canvasRect); });
        measure("marquee_geometry", [&](int) { QPainterPath path; path.addRegion(doc->selection().region()); path = path.simplified(); });
        const auto effective = doc->snapshotEffectiveLayers(); std::vector<Layer> layers;
        for (const auto &layer : effective) layers.push_back(*layer);
        measure("compositing_only", [&](int) { result = Document::compositeLayers(layers, doc->size()); });
        measure("document_thumbnail", [&](int) { result = doc->thumbnail(QSize(30, 30)); });
        measure("layer_thumbnail", [&](int) { result = session->renderScaled(canvasRect, QSize(40, 30)); });
        measure("opacity_invalidation", [&](int i) { doc->setLayerOpacity(doc->activeLayerIndex(), 0.2f + i*0.05f); });
        const auto opacityStart = FloatingRenderCache::shared().stats();
        measure("opacity_frame", [&](int i) { doc->setLayerOpacity(doc->activeLayerIndex(), 0.2f + i*0.05f); canvas->render(&screen); });
        const auto cacheBefore = FloatingRenderCache::shared().stats();
        std::cout << "CACHE opacity hits=" << cacheBefore.hits - opacityStart.hits << " misses=" << cacheBefore.misses - opacityStart.misses
                  << " cost_kib=" << cacheBefore.costKiB << " limit_kib=" << cacheBefore.limitKiB << std::endl;
        const QTransform original = session->transform();
        measure("transform_frame", [&](int i) { doc->setFloatingPixelTransform(original * QTransform::fromTranslate(i*9, i*3)); canvas->render(&screen); });
        if (QCoreApplication::arguments().contains("--benchmark-stage4c")) {
            const auto reduced=QTransform::fromScale(0.25,0.25)*QTransform::fromTranslate(700,250);
            doc->setFloatingPixelTransform(reduced);
            measure("reduction_frame", [&](int i) { doc->setFloatingPixelTransform(reduced*QTransform::fromTranslate(i*9,i*3)); canvas->render(&screen); });
        }
    }
}

static void thumbnailCloseWorkflow() {
    QTemporaryDir dir;
    const QString path = dir.path() + "/close.png";
    CHECK(fixture().save(path));
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    const QString firstPath = dir.path() + "/first.png";
    CHECK(fixture().save(firstPath));
    window.openFile(firstPath);
    auto *first = canvas->document();
    window.openFile(path);
    auto *loaded = canvas->document();
    auto *bar = window.findChild<ImageListBar *>();
    auto closeAt = [&](int index) {
        const auto tabs = bar->findChildren<QToolButton *>(QString(), Qt::FindDirectChildrenOnly);
        // Old buttons are deleted later after a document-list rebuild.
        for (auto *tab : tabs) {
            if (tab->property("imageIndex").toInt() == index && bar->layout()->indexOf(tab) >= 0) {
                tab->findChild<QToolButton *>("imageCloseButton")->click();
                return;
            }
        }
        CHECK(false);
    };
    auto answer = [&](QMessageBox::StandardButton choice) {
        QTimer::singleShot(0, [&, choice]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            CHECK(box != nullptr);
            if (box) box->button(choice)->click();
        });
    };
    loaded->setModified(true);
    qobject_cast<QToolButton *>(bar->layout()->itemAt(0)->widget())->click();
    CHECK(canvas->document() == first);
    answer(QMessageBox::Cancel);
    closeAt(1);
    CHECK(canvas->document() == loaded && loaded->isModified());
    // A failed write must leave the modified document open.
    CHECK(QFile::remove(path));
    CHECK(QDir().mkdir(path));
    QTimer::singleShot(0, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        CHECK(box != nullptr);
        QTimer::singleShot(0, [&]() {
            auto *warning = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            CHECK(warning != nullptr);
            if (warning) warning->accept();
        });
        if (box) box->button(QMessageBox::Save)->click();
    });
    closeAt(1);
    CHECK(canvas->document() == loaded && loaded->isModified());
    CHECK(QDir().rmdir(path));
    answer(QMessageBox::Save);
    closeAt(1);
    CHECK(canvas->document() == first);
    window.openFile(path);
    loaded = canvas->document();
    loaded->setModified(true);
    answer(QMessageBox::Discard);
    closeAt(1);
    CHECK(canvas->document() == first);
    closeAt(0);
    first = canvas->document();
    first->setModified(true);
    QTimer::singleShot(0, [&]() {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        CHECK(box != nullptr);
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            CHECK(dialog != nullptr);
            if (dialog) dialog->reject();
        });
        if (box) box->button(QMessageBox::Save)->click();
    });
    closeAt(0);
    CHECK(canvas->document() == first && first->isModified());
    first->setModified(false);
    closeAt(0);
    CHECK(canvas->document() && canvas->document()->size() == QSize(800, 600));
    QMenu *edit = nullptr;
    for (auto *action : window.menuBar()->actions())
        if (action->text().remove('&') == "Edit") edit = action->menu();
    CHECK(edit != nullptr);
    if (edit) {
        const auto actions = edit->actions();
        CHECK(actions.last()->text() == "Settings...");
        CHECK(actions[actions.size() - 2]->isSeparator());
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<SettingsDialog *>(QApplication::activeModalWidget());
            CHECK(dialog != nullptr);
            if (dialog) dialog->reject();
        });
        actions.last()->trigger();
    }
}

static void selectionHistoryWorkflow() {
    MainWindow window;
    auto *canvas = window.findChild<CanvasWidget *>();
    auto *doc = canvas->document();
    invoke(window, "selectAll");
    CHECK(doc->selection().hasSelection());
    CHECK(doc->history().currentIndex() == 1);
    const QImage selected = doc->selection().mask();
    auto triggerShortcut = [&](const QKeySequence &sequence) {
        for (auto *action : window.findChildren<QAction *>()) {
            if (action->shortcuts().contains(sequence)) {
                action->trigger();
                return;
            }
        }
        CHECK(false);
    };
    triggerShortcut(QKeySequence("Ctrl+D"));
    CHECK(!doc->selection().hasSelection());
    CHECK(doc->history().currentIndex() == 2);
    doc->history().undo();
    CHECK(doc->selection().mask() == selected);
    doc->history().redo();
    CHECK(!doc->selection().hasSelection());
    doc->history().undo();
    triggerShortcut(QKeySequence("Return"));
    CHECK(!doc->selection().hasSelection());
    CHECK(doc->history().currentIndex() == 2);
    doc->history().undo();
    CHECK(doc->selection().mask() == selected);
    invoke(window, "invertSelection");
    CHECK(!doc->selection().hasSelection());
    doc->history().goToIndex(0);
    CHECK(!doc->selection().hasSelection());
    doc->history().goToIndex(1);
    CHECK(doc->selection().mask() == selected);
    // Selection history must interleave correctly with actual image edits.
    doc->activeLayer()->image().fill(Qt::blue);
    const QImage beforePixels = doc->activeLayer()->image();
    QKeyEvent erase(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(canvas, &erase);
    doc->history().undo();
    CHECK(doc->activeLayer()->image() == beforePixels);
    CHECK(doc->selection().mask() == selected);
    // Finishing pasted pixels already owns its selection state and needs no extra Deselect.
    QImage pixels(8, 8, QImage::Format_ARGB32_Premultiplied); pixels.fill(Qt::red);
    QImage mask(8, 8, QImage::Format_Grayscale8); mask.fill(255);
    CHECK(doc->pasteFloatingPixels(pixels, mask, QPoint(3, 4)));
    const int index = doc->history().currentIndex();
    invoke(window, "deselectAll");
    CHECK(!doc->floatingPixelSession() && !doc->selection().hasSelection());
    CHECK(doc->history().currentIndex() == index + 1);
    doc->history().undo();
    CHECK(doc->floatingPixelSession() && doc->selection().hasSelection());
    // The selected tool exposes the mode control after floating-context changes.
    CHECK(QMetaObject::invokeMethod(&window, "selectTool", Qt::DirectConnection,
                                    Q_ARG(ToolType, ToolType::RectSelection)));
    auto *modes = window.findChild<QWidget *>("selectionCombineGroup");
    CHECK(modes != nullptr);
    if (modes) CHECK(!modes->isHidden());
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
    if (app.arguments().contains("--benchmark-floating")) { benchmarkFloating(); return failures ? 1 : 0; }
    if (app.arguments().contains("--benchmark-stage4a")) { benchmarkStage4A(); return failures ? 1 : 0; }
    if (app.arguments().contains("--benchmark-stage4c")) {
        for (auto mode:{FloatingPixelSession::Sampling::Nearest,FloatingPixelSession::Sampling::Bilinear,FloatingPixelSession::Sampling::Bicubic}) benchmarkStage4A(mode);
        return failures ? 1 : 0;
    }
    selectionHistoryWorkflow();
    thumbnailCloseWorkflow();
    pasteDialogStyles();
    utilityPanelPreferences();
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
    floatingPasteGestures();
    floatingContextAndClipboard();
    floatingResamplingControls();
    cropWorkflow();
    floatingNativeAndDocumentSwitch();
    floatingCoalescing();
    floatingPlacementAndSoftClipboard();
    copiedPastePlacement();
    floatingLayerAppearance();
    std::cout << checks << " paste checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
