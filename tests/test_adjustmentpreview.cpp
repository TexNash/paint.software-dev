#include "core/adjustmentpreview.h"
#include "canvas/canvaswidget.h"
#include "adjustments/curves.h"
#include "dialogs/curvesdialog.h"
#include <QMouseEvent>
#include <QPushButton>
#include "mainwindow.h"
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <iostream>
#include <cstdlib>

static void check(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
static bool until(const std::function<bool()> &predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QApplication::processEvents(); QThread::msleep(1);
    }
    return predicate();
}
static ImageOperation curve(int lift) {
    return [lift](const QImage &image) {
        Curves c; c.setControlPoints({{0, double(lift)}, {255,255}}); return c.apply(image);
    };
}
static QImage sample(QSize size = {8,8}) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(80,120,160,128));
    image.setPixel(0,0,0);
    return image;
}
int main(int argc, char **argv) {
    QTemporaryDir settings;
    check(settings.isValid(),"temporary settings directory");
    for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
        const QString path = settings.path() + '/' + QString::fromLatin1(name);
        QDir().mkpath(path);
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        qputenv(name,path.toUtf8());
    }
    QApplication app(argc, argv);
    Document doc(8,8);
    doc.layerAt(0)->setImage(sample());
    CanvasWidget canvas; canvas.setDocument(&doc); canvas.resize(160,160);
    canvas.show(); canvas.zoomToFit(); QApplication::processEvents();
    const QImage canvasBefore = canvas.grab().toImage();
    const QImage original = doc.layerAt(0)->image();
    const auto initial = doc.history().currentIndex();
    const bool modified = doc.isModified();
    {
        AdjustmentPreview preview(&doc,&canvas,{0});
        int frames = 0; QImage displayed;
        QObject::connect(&preview,&AdjustmentPreview::previewReady,&app,[&](const QImage &image) {
            check(QThread::currentThread() == app.thread(),"GUI-thread delivery");
            displayed = image; ++frames;
        });
        preview.request(curve(30));
        check(until([&]{return frames == 1;}),"first live frame");
        check(displayed == curve(30)(original).convertToFormat(original.format()),"preview colour and alpha");
        preview.request(curve(70));
        check(until([&]{return frames == 2;}),"second live frame");
        check(displayed == curve(70)(original).convertToFormat(original.format()),"non-cumulative processing");
        check(doc.layerAt(0)->image() == original,"preview leaves document unchanged");
        check(canvas.grab().toImage() != canvasBefore,"main canvas displays override");
        preview.cancel();
        check(canvas.grab().toImage() == canvasBefore,"Cancel clears canvas override");
        preview.request(curve(90));
        QApplication::processEvents();
        check(frames == 2,"cancel rejects requests");
        check(doc.history().currentIndex() == initial && doc.isModified() == modified,"cancel preserves state");
    }
    // Exercise the actual curve editor's add/move/remove/reset notifications.
    {
        CurvesDialog dialog;
        AdjustmentPreview session(&doc,&canvas,{0});
        int frames = 0;
        QObject::connect(&session,&AdjustmentPreview::previewReady,&app,[&](const QImage &){++frames;});
        QObject::connect(&dialog,&CurvesDialog::curveChanged,&session,[&]() {
            const auto points = dialog.controlPoints();
            session.request([points](const QImage &im) { Curves c; c.setControlPoints(points); return c.apply(im); });
        });
        QWidget *editor = nullptr;
        for (auto *widget : dialog.findChildren<QWidget *>())
            if (widget->size() == QSize(256,256)) editor = widget;
        check(editor,"curve editor found");
        auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type,point,point,button,buttons,Qt::NoModifier);
            QApplication::sendEvent(editor,&event);
        };
        mouse(QEvent::MouseButtonPress,{128,100},Qt::LeftButton,Qt::LeftButton);
        check(until([&]{return frames==1;}),"adding point previews");
        mouse(QEvent::MouseMove,{140,70},Qt::NoButton,Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease,{140,70},Qt::LeftButton,Qt::NoButton);
        check(until([&]{return frames==2;}),"moving point previews");
        mouse(QEvent::MouseButtonPress,{140,70},Qt::RightButton,Qt::RightButton);
        check(until([&]{return frames==3;}),"removing point previews");
        check(dialog.controlPoints().size()==2,"point removed");
        for (auto *button : dialog.findChildren<QPushButton *>())
            if (button->text().contains("initialiser") || button->text() == "Reset") button->click();
        check(until([&]{return frames==4;}),"Reset previews");
        QObject::connect(&dialog,&QDialog::rejected,&session,&AdjustmentPreview::cancel);
        dialog.reject();
        check(doc.layerAt(0)->image()==original && doc.history().currentIndex()==initial,"dialog Cancel leaves document intact");
    }
    // Feathered selection in document coordinates on an offset, transparent layer.
    doc.layerAt(0)->setOffset({2,1});
    QImage mask(8,8,QImage::Format_Grayscale8); mask.fill(0);
    mask.scanLine(3)[4] = 128;
    mask.scanLine(3)[5] = 255;
    doc.selection().setMaskImage(mask);
    QImage expected = adjustedImage(original,{2,1},doc.selection(),curve(70));
    QImage full = curve(70)(original).convertToFormat(original.format());
    QRgb src = original.pixel(2,2), fx = full.pixel(2,2), half = expected.pixel(2,2);
    check(qRed(half) == (qRed(src)*127+qRed(fx)*128)/255,"feather coverage");
    check(expected.pixel(3,2) == full.pixel(3,2),"offset selected pixel");
    check(expected.pixel(1,2) == original.pixel(1,2),"unselected pixel unchanged");
    check(qAlpha(half) == qAlpha(src) && expected.pixel(0,0) == 0,"alpha preserved");
    {
        AdjustmentPreview preview(&doc,&canvas,{0});
        check(preview.commit(curve(70),"Curves"),"commit succeeds before preview completes");
        check(doc.layerAt(0)->image() == expected,"commit selection matches calculation");
    }
    check(doc.history().currentIndex() == initial+1,"one undo entry");
    doc.history().undo();
    check(doc.layerAt(0)->image() == original,"undo exact original");
    {
        AdjustmentPreview preview(&doc,&canvas,{0}); preview.request(curve(20)); preview.cancel();
    }
    check(doc.history().canRedo(),"Cancel preserves redo");
    doc.history().redo(); check(doc.layerAt(0)->image() == expected,"redo exact adjusted image");

    doc.selection().clear();
    doc.addLayer(sample(),"second");
    doc.layerAt(1)->setOpacity(0.6f); doc.layerAt(1)->setBlendMode(BlendMode::Multiply);
    auto before0 = doc.layerAt(0)->image(), before1 = doc.layerAt(1)->image();
    int historyBefore = doc.history().currentIndex();
    {
        AdjustmentPreview preview(&doc,&canvas,{0,1});
        QImage displayed;
        QObject::connect(&preview,&AdjustmentPreview::previewReady,&app,[&](const QImage &im){displayed=im;});
        preview.request(curve(40));
        check(until([&]{return !displayed.isNull();}),"multilayer preview");
        check(preview.commit(curve(40),"Curves"),"multilayer commit");
        check(displayed == doc.flattenVisible(),"preview matches actual blend/opacity/offset composite");
    }
    check(doc.history().currentIndex() == historyBefore+1,"all layers single undo");
    auto after0 = doc.layerAt(0)->image(), after1 = doc.layerAt(1)->image();
    doc.history().undo();
    check(doc.layerAt(0)->image()==before0 && doc.layerAt(1)->image()==before1,"all layers undone together");
    doc.history().redo();
    check(doc.layerAt(0)->image()==after0 && doc.layerAt(1)->image()==after1,"all layers redone together");
    {
        AdjustmentPreview preview(&doc,&canvas,{0,1});
        int count = doc.history().currentIndex();
        check(!preview.commit([](const QImage &im){return im;},"Identity"),"identity no commit");
        check(count == doc.history().currentIndex(),"identity no history");
    }
    // Running work must never deliver after cancel or session destruction.
    auto entered = std::make_shared<std::atomic_bool>(false);
    auto release = std::make_shared<std::atomic_bool>(false);
    ImageOperation blocked = [entered,release](const QImage &im) {
        entered->store(true);
        while (!release->load()) QThread::msleep(1);
        return curve(60)(im);
    };
    int late = 0;
    auto *preview = new AdjustmentPreview(&doc,&canvas,{0});
    QObject::connect(preview,&AdjustmentPreview::previewReady,&app,[&](const QImage &){++late;});
    preview->request(blocked);
    check(until([&]{return entered->load();}),"worker started");
    preview->cancel(); delete preview; release->store(true);
    QThreadPool::globalInstance()->waitForDone(); QApplication::processEvents();
    check(late == 0,"no callback after destruction");
    entered->store(false); release->store(false);
    {
        AdjustmentPreview session(&doc,&canvas,{0});
        QObject::connect(&session,&AdjustmentPreview::previewReady,&app,[&](const QImage &){++late;});
        session.request(blocked);
        check(until([&]{return entered->load();}),"cancel worker started");
        session.cancel(); release->store(true);
        QThreadPool::globalInstance()->waitForDone(); QApplication::processEvents();
        check(late==0,"no late frame after Cancel with session alive");
    }
    // Latest pending request wins, with older work displayed only before newer work.
    entered->store(false); release->store(false);
    {
        AdjustmentPreview session(&doc,&canvas,{0});
        QImage displayed; int frames = 0;
        QObject::connect(&session,&AdjustmentPreview::previewReady,&app,[&](const QImage &im){displayed=im;++frames;});
        session.request(blocked);
        check(until([&]{return entered->load();}),"blocked generation started");
        session.request(curve(10)); session.request(curve(90)); release->store(true);
        check(until([&]{return frames == 2;}),"latest pending frame delivered");
        std::vector<Layer> layers{*doc.layerAt(0),*doc.layerAt(1)};
        layers[0].setImage(curve(90)(after0));
        check(displayed == Document::compositeLayers(layers,doc.size()),"no stale replacement");
        doc.notifyContentChanged();
        check(!session.commit(curve(20),"invalid"),"external document change invalidates session");
    }
    // Actual MainWindow modal-dialog wiring: reject, immediate OK, grouped undo.
    {
        MainWindow window;
        auto *view = window.findChild<CanvasWidget *>();
        check(view,"MainWindow canvas");
        auto *document = view->document();
        document->activeLayer()->setImage(sample());
        auto before = document->activeLayer()->image();
        int index = document->history().currentIndex();
        auto editDialog = [&window](bool accept) {
            QTimer::singleShot(0,&window,[&window,accept]() {
                auto *dialog = window.findChild<CurvesDialog *>();
                check(dialog,"MainWindow opens Curves dialog");
                QWidget *editor = nullptr;
                for (auto *widget : dialog->findChildren<QWidget *>())
                    if (widget->size() == QSize(256,256)) editor = widget;
                check(editor,"MainWindow curve editor");
                QMouseEvent event(QEvent::MouseButtonPress,QPointF(128,40),QPointF(128,40),
                                  Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                QApplication::sendEvent(editor,&event);
                if (accept) dialog->accept(); else dialog->reject();
            });
            check(QMetaObject::invokeMethod(&window,"applyAdjustment",Qt::DirectConnection,Q_ARG(int,3)),"invoke Curves adjustment");
        };
        editDialog(false);
        check(document->activeLayer()->image()==before && document->history().currentIndex()==index,"MainWindow Cancel");
        editDialog(true);
        auto after = document->activeLayer()->image();
        check(after!=before && document->history().currentIndex()==index+1,"MainWindow immediate OK commits once");
        document->history().undo(); check(document->activeLayer()->image()==before,"MainWindow undo");
        document->history().redo(); check(document->activeLayer()->image()==after,"MainWindow redo");
    }
    std::cout << "Adjustment preview regressions passed\n";
}
