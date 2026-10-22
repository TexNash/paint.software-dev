#include "canvaswidget.h"
#include "core/document.h"
#include "tools/tool.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QEvent>
#include <QTabletEvent>
#include <QSettings>
#include <cmath>
#include <cstring>

bool CanvasWidget::event(QEvent *event)
{
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse
        || event->type() == QEvent::Hide) cancelSelectionDrag();

    // Track when the stylus is within range of the tablet. While it hovers there
    // (in proximity but tip not touching), nothing draws — not even a barrel/
    // right-button press, which otherwise painted a hover blob (issue #17). The
    // gate is only armed while the pen is in proximity, so the mouse is never
    // affected.
    if (event->type() == QEvent::TabletEnterProximity) { m_penInProximity = true; return true; }
    if (event->type() == QEvent::TabletLeaveProximity) { m_penInProximity = false; m_penInContact = false; return true; }

    // Qt matches single-key shortcuts (B, E, S…) before the key reaches
    // keyPressEvent, so while the Text tool is typing those letters would trigger
    // a tool switch and never be typed — ~60% of the alphabet vanished. Accepting
    // ShortcutOverride tells Qt to deliver the key as a normal press instead.
    if (event->type() == QEvent::ShortcutOverride && m_currentTool) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const bool ctrlAlt = ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        if ((m_currentTool->wantsKeyInput() && !ctrlAlt)
            || m_currentTool->wantsCommitKey(ke->key())
            || (m_document && m_document->floatingPixelSession() && ke->key() == Qt::Key_Escape)) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

CanvasWidget::CanvasWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_TabletTracking);

    connect(&m_marchingTimer, &QTimer::timeout, this, &CanvasWidget::updateMarchingAnts);
    m_marchingTimer.start(150);
    m_selectionPanTimer.setInterval(16);
    m_selectionPanTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_selectionPanTimer, &QTimer::timeout, this, &CanvasWidget::panSelectionDrag);
    m_floatingMoveTimer.setSingleShot(true);
    m_floatingMoveTimer.setInterval(16);
    connect(&m_floatingMoveTimer, &QTimer::timeout, this, &CanvasWidget::flushFloatingMove);
}

void CanvasWidget::setDocument(Document *doc) {
    cancelSelectionDrag();
    flushFloatingMove();
    if (m_document) disconnect(&m_document->history(), nullptr, this, nullptr);
    if (m_document && m_document != doc && m_currentTool)
        m_currentTool->deactivate(*this);
    if (m_document) disconnect(m_document, nullptr, this, nullptr);
    m_previewImage = QImage();
    m_outlineMask = QImage();
    m_selectionOutlineKey = -1;
    m_document = doc;
    if (doc) {
        connect(&doc->history(), &HistoryManager::historyAboutToChange, this, &CanvasWidget::cancelSelectionDrag);
        connect(&doc->history(), &HistoryManager::historyChanged, this, [this]() {
            m_floatingMoveTimer.stop();
            m_pendingFloatingMove = false;
        });
        connect(doc, &Document::floatingPixelsChanged, this, [this](const QRegion &regions) {
            for (const QRect &region : regions) updateCanvasRegion(region);
            update(); // overlay/handles can also extend beyond the canvas
        });
        connect(doc, &Document::floatingContextChanging, this, [this]() {
            flushFloatingMove();
            if (m_currentTool && m_currentTool->type() == ToolType::Move) m_currentTool->deactivate(*this);
        });
        connect(doc, &Document::layerAppearanceChanged, this, [this](int index) {
            const auto *layer = m_document->layerAt(index);
            if (!layer) return;
            const auto *session = m_document->floatingPixelSession();
            const QRectF domain = session && !session->isSuspended() && session->targets(layer)
                ? session->documentBounds() : QRectF(layer->offset(), layer->image().size());
            // Visibility/blend/opacity affect the whole finite layer domain,
            // including transparent pixels in Overwrite, not just the source.
            updateCanvasRegion(domain.intersected(QRectF(QPointF(), m_document->size())).toAlignedRect());
        });
        connect(doc, &Document::documentChanged, this, [this]() {
            m_cacheValid = false;
            update();
        });
        connect(doc, &Document::layersChanged, this, [this]() {
            m_cacheValid = false;
            update();
        });
        connect(doc, &Document::selectionChanged, this, [this]() { update(); });
    }
    m_cacheValid = false;
    update();
}

bool CanvasWidget::setCurrentTool(Tool *tool) {
    if (tool != m_currentTool) cancelSelectionDrag();
    flushFloatingMove();
    if (m_currentTool && m_currentTool != tool) {
        m_currentTool->deactivate(*this);   // commit any pending edit before switching
        if (tool && m_document && !m_document->suspendFloatingPixels()) return false;
    }
    m_currentTool = tool;
    return true;
}

void CanvasWidget::setZoom(double zoom) {
    m_zoom = qBound(0.01, zoom, 64.0);
    emit zoomChanged(m_zoom);
    update();
}

void CanvasWidget::setShowRulers(bool show) {
    m_showRulers = show;
    update();
}

bool CanvasWidget::showRulers() const {
    return m_showRulers;
}

void CanvasWidget::zoomIn() {
    double newZoom = m_zoom;
    if (m_zoom < 1.0) newZoom = m_zoom * 1.5;
    else if (m_zoom < 2.0) newZoom = m_zoom + 0.25;
    else if (m_zoom < 8.0) newZoom = m_zoom + 1.0;
    else newZoom = m_zoom * 1.5;
    setZoom(newZoom);
}

void CanvasWidget::zoomOut() {
    double newZoom = m_zoom;
    if (m_zoom > 8.0) newZoom = m_zoom / 1.5;
    else if (m_zoom > 2.0) newZoom = m_zoom - 1.0;
    else if (m_zoom > 1.0) newZoom = m_zoom - 0.25;
    else newZoom = m_zoom / 1.5;
    setZoom(newZoom);
}

void CanvasWidget::zoomToFit() {
    if (!m_document) return;
    int rs = m_showRulers ? 20 : 0;
    double zx = (double)(width() - 20 - rs) / m_document->width();
    double zy = (double)(height() - 20 - rs) / m_document->height();
    setZoom(std::min(zx, zy));
    m_pan = QPointF(
        (width() - rs - m_document->width() * m_zoom) / 2.0,
        (height() - rs - m_document->height() * m_zoom) / 2.0
    );
    update();
}

void CanvasWidget::zoomToRect(const QRect &canvasRect) {
    if (!m_document || canvasRect.isEmpty()) return;
    const int rs = m_showRulers ? 20 : 0;
    const double margin = 24.0;
    const double zx = (width() - rs - margin * 2) / canvasRect.width();
    const double zy = (height() - rs - margin * 2) / canvasRect.height();
    setZoom(std::min(zx, zy));
    // Centre the rectangle in the viewport.
    m_pan = QPointF(
        (width() - rs - canvasRect.width() * m_zoom) / 2.0 - canvasRect.x() * m_zoom,
        (height() - rs - canvasRect.height() * m_zoom) / 2.0 - canvasRect.y() * m_zoom
    );
    m_autoCenter = false;   // framed on a specific rect, not on the whole image
    update();
}

void CanvasWidget::zoomToActual() {
    setZoom(1.0);
    if (m_document) {
        int rs = m_showRulers ? 20 : 0;
        m_pan = QPointF(
            (width() - rs - m_document->width()) / 2.0,
            (height() - rs - m_document->height()) / 2.0
        );
    }
    update();
}

void CanvasWidget::centerView() {
    if (!m_document) return;

    // paint.net opens with the canvas centred, not pinned to the top-left. The
    // image is painted at m_pan + (rs, rs), so centring it inside the area right
    // of the rulers means panning by half the leftover space.
    // No margin clamp here: it would push a snugly-fitting image off-centre, and
    // an image larger than the viewport is better centred (overflowing evenly)
    // than pinned to the left.
    const int rs = m_showRulers ? 20 : 0;
    const qreal imgW = m_document->width() * m_zoom;
    const qreal imgH = m_document->height() * m_zoom;
    m_pan = QPointF((width() - rs - imgW) / 2.0, (height() - rs - imgH) / 2.0);
    update();
}

void CanvasWidget::resetToDefaultView() {
    if (!m_document) return;

    const int rs = m_showRulers ? 20 : 0;
    const qreal margin = 40.0;
    const qreal availableWidth = std::max(1.0, width() - rs - margin * 2.0);
    const qreal availableHeight = std::max(1.0, height() - rs - margin * 2.0);
    const qreal fitZoom = std::min(availableWidth / m_document->width(), availableHeight / m_document->height());

    setZoom(std::min(1.0, fitZoom));
    m_autoCenter = true;   // follow the window again until the user pans
    centerView();
}

QPointF CanvasWidget::widgetToCanvas(const QPointF &widgetPos) const {
    int rs = m_showRulers ? 20 : 0;
    return (widgetPos - QPointF(rs, rs) - m_pan) / m_zoom;
}

QPointF CanvasWidget::canvasToWidget(const QPointF &canvasPos) const {
    int rs = m_showRulers ? 20 : 0;
    return canvasPos * m_zoom + m_pan + QPointF(rs, rs);
}

void CanvasWidget::updateCanvas() {
    m_cacheValid = false;
    update();
}

void CanvasWidget::updateCanvasRegion(const QRect &documentRect) {
    if (!m_document || documentRect.isEmpty()) return;
    const QRect dirty = documentRect.intersected(QRect(QPoint(), m_document->size()));
    if (dirty.isEmpty()) return;
    m_dirtyRender += dirty;
    const QRectF widgetRect(canvasToWidget(dirty.topLeft()),
                            QSizeF(dirty.size()) * m_zoom);
    update(widgetRect.toAlignedRect().adjusted(-2, -2, 2, 2));
}

void CanvasWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 2.0);

    // Backdrop around the image (paint.net's "editing window"), scheme-aware
    painter.fillRect(rect(), m_backdrop);

    if (!m_document) return;

    // Ruler offset
    int rs = m_showRulers ? 20 : 0;

    // Canvas area
    QRectF canvasRect(m_pan.x() + rs, m_pan.y() + rs,
                      m_document->width() * m_zoom,
                      m_document->height() * m_zoom);

    // Checkerboard for transparency
    drawCheckerboard(painter, canvasRect.toRect());

    // Render document
    if (m_previewImage.isNull()) {
        if (!m_cacheValid) {
            m_cachedRender = m_document->flattenVisible();
            m_cacheValid = true;
        } else if (!m_dirtyRender.isEmpty()) {
            QPainter cachePainter(&m_cachedRender);
            cachePainter.setCompositionMode(QPainter::CompositionMode_Source);
            for (const QRect &dirty : m_dirtyRender)
                cachePainter.drawImage(dirty.topLeft(), m_document->flattenVisible(dirty));
        }
        m_dirtyRender = QRegion();
    }
    painter.drawImage(canvasRect, m_previewImage.isNull() ? m_cachedRender : m_previewImage);

    // Grid at high zoom
    if (m_showGrid && m_zoom >= 4.0) {
        painter.setPen(QPen(QColor(0, 0, 0, 40), 1));
        double startX = m_pan.x() + rs;
        double startY = m_pan.y() + rs;
        for (int x = 0; x <= m_document->width(); ++x) {
            double wx = startX + x * m_zoom;
            painter.drawLine(QPointF(wx, startY), QPointF(wx, startY + m_document->height() * m_zoom));
        }
        for (int y = 0; y <= m_document->height(); ++y) {
            double wy = startY + y * m_zoom;
            painter.drawLine(QPointF(startX, wy), QPointF(startX + m_document->width() * m_zoom, wy));
        }
    }

    // Canvas border
    painter.setPen(QPen(Qt::darkGray, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(canvasRect.adjusted(-1, -1, 0, 0));

    // Selection marching ants
    if (m_document->selection().hasSelection()) {
        drawSelectionMarching(painter);
    }

    // Tool overlay
    if (m_currentTool) {
        m_currentTool->drawOverlay(painter, *this);
    }

    // Rulers (drawn on top of everything, like Paint.NET)
    if (m_showRulers && m_document) {
        const int rulerSize = 20;
        QFont rulerFont("Sans", 7);
        painter.setFont(rulerFont);

        // Theme-aware colours: light strip on light backdrop, dark on dark.
        const bool dark = m_backdrop.lightness() < 110;
        const QColor rulerBg   = dark ? QColor(58, 58, 58)   : QColor(255, 255, 255);
        const QColor rulerLine = dark ? QColor(90, 90, 90)   : QColor(180, 180, 180);
        const QColor tickCol   = dark ? QColor(190, 190, 190) : QColor(100, 100, 100);
        const QColor textCol   = dark ? QColor(220, 220, 220) : QColor(60, 60, 60);

        // Backgrounds
        painter.fillRect(QRect(rulerSize, 0, width() - rulerSize, rulerSize), rulerBg);
        painter.fillRect(QRect(0, rulerSize, rulerSize, height() - rulerSize), rulerBg);
        painter.fillRect(QRect(0, 0, rulerSize, rulerSize), rulerBg);
        painter.setPen(QPen(rulerLine, 1));
        painter.drawLine(rulerSize, rulerSize, width(), rulerSize);
        painter.drawLine(rulerSize, rulerSize, rulerSize, height());
        painter.drawRect(QRect(0, 0, rulerSize, rulerSize));

        // Pixels-per-unit for the chosen measurement unit.
        const double pxPerUnit = (m_unit == Unit::Inches)      ? m_dpi
                               : (m_unit == Unit::Centimeters) ? m_dpi / 2.54
                                                               : 1.0;
        // On-screen pixels per unit, used to pick a readable tick step.
        const double screenPerUnit = pxPerUnit * m_zoom;
        // Choose a "nice" major step (in units) so labels are ~60px apart.
        auto niceStep = [&](double minScreen) -> double {
            static const double steps[] = {0.1, 0.25, 0.5, 1, 2, 5, 10, 25, 50, 100, 200, 500, 1000};
            for (double s : steps)
                if (s * screenPerUnit >= minScreen) return s;
            return 2000.0;
        };
        const double majorStep = niceStep(58.0);
        const double smallStep = majorStep / 5.0;   // 5 minor ticks per major

        // Label formatting: pixels are integers, in/cm show up to 2 decimals.
        auto fmt = [&](double unitVal) -> QString {
            if (m_unit == Unit::Pixels) return QString::number(qRound(unitVal));
            QString s = QString::number(unitVal, 'f', 2);
            while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) s.chop(1);
            return s;
        };

        // Visible canvas range, in the chosen unit (ruler is "infinite":
        // negative values before the origin, values beyond the image).
        const double leftU  = ((-m_pan.x()) / m_zoom) / pxPerUnit;
        const double rightU = ((width()  - rulerSize - m_pan.x()) / m_zoom) / pxPerUnit;
        const double topU   = ((-m_pan.y()) / m_zoom) / pxPerUnit;
        const double botU   = ((height() - rulerSize - m_pan.y()) / m_zoom) / pxPerUnit;

        auto unitToWx = [&](double u) { return m_pan.x() + rulerSize + u * pxPerUnit * m_zoom; };
        auto unitToWy = [&](double u) { return m_pan.y() + rulerSize + u * pxPerUnit * m_zoom; };
        auto isMultiple = [](double v, double step) {
            return std::abs(v / step - std::round(v / step)) < 1e-6;
        };

        painter.setPen(QPen(tickCol, 1));
        const double startX = std::floor(leftU / smallStep) * smallStep;
        for (double u = startX; u <= rightU + smallStep; u += smallStep) {
            const double wx = unitToWx(u);
            if (wx < rulerSize || wx > width()) continue;
            if (isMultiple(u, majorStep)) {
                painter.setPen(QPen(tickCol, 1));
                painter.drawLine(QPointF(wx, 0), QPointF(wx, rulerSize));
                painter.setPen(textCol);
                painter.drawText(QPointF(wx + 2, rulerSize - 4), fmt(u));
            } else {
                painter.setPen(QPen(tickCol, 1));
                painter.drawLine(QPointF(wx, rulerSize * 0.7), QPointF(wx, rulerSize));
            }
        }

        const double startY = std::floor(topU / smallStep) * smallStep;
        for (double u = startY; u <= botU + smallStep; u += smallStep) {
            const double wy = unitToWy(u);
            if (wy < rulerSize || wy > height()) continue;
            if (isMultiple(u, majorStep)) {
                painter.setPen(QPen(tickCol, 1));
                painter.drawLine(QPointF(0, wy), QPointF(rulerSize, wy));
                painter.setPen(textCol);
                painter.save();
                painter.translate(rulerSize - 4, wy + 2);
                painter.rotate(90);
                painter.drawText(0, 0, fmt(u));
                painter.restore();
            } else {
                painter.setPen(QPen(tickCol, 1));
                painter.drawLine(QPointF(rulerSize * 0.7, wy), QPointF(rulerSize, wy));
            }
        }

        // Cursor position indicator on the rulers.
        painter.setPen(QPen(QColor(255, 60, 60, 200), 1));
        QPoint cursorPos = mapFromGlobal(QCursor::pos());
        if (cursorPos.x() >= rulerSize && cursorPos.x() < width())
            painter.drawLine(cursorPos.x(), 0, cursorPos.x(), rulerSize);
        if (cursorPos.y() >= rulerSize && cursorPos.y() < height())
            painter.drawLine(0, cursorPos.y(), rulerSize, cursorPos.y());
    }
}

void CanvasWidget::reloadSettings() {
    QSettings s("PaintDali", "PaintDali");
    m_checkerBrightness = s.value("canvas/checkerBrightness", 80).toInt();
    update();
}

void CanvasWidget::drawCheckerboard(QPainter &painter, const QRect &rect) {
    const int gridSize = 8;
    // Brightness slider (Settings > Canvas) scales the checkerboard: 100 = white
    // squares, lower values darken both tones.
    const int base = qBound(0, 155 + m_checkerBrightness, 255);
    const int alt = qBound(0, base - 51, 255);
    QColor c1(base, base, base);
    QColor c2(alt, alt, alt);

    painter.save();
    painter.setClipRect(rect);
    // At 1:1/high zoom most of a large document can be offscreen. Never walk
    // all those invisible checker squares for a local brush update.
    const QRect visible = rect.intersected(this->rect());
    int startX = visible.left() - (visible.left() % (gridSize * 2));
    int startY = visible.top() - (visible.top() % (gridSize * 2));

    for (int y = startY; y <= visible.bottom(); y += gridSize) {
        for (int x = startX; x <= visible.right(); x += gridSize) {
            bool dark = ((x / gridSize) + (y / gridSize)) % 2;
            painter.fillRect(x, y, gridSize, gridSize, dark ? c2 : c1);
        }
    }
    painter.restore();
}

void CanvasWidget::drawSelectionMarching(QPainter &painter) {
    if (!m_document) return;

    const Selection &sel = m_document->selection();
    // The mask/outline does not change when the pointer or dash phase moves.
    // Rebuilding it on every paint scans the full canvas and simplifies thousands
    // of scanline rectangles, overwhelming otherwise-local brush updates.
    if (m_selectionOutlineKey != sel.mask().cacheKey()) {
        const auto *session = m_document->floatingPixelSession();
        const QRect bounds = sel.boundingRect();
        const QPoint delta = bounds.topLeft() - m_outlineBounds.topLeft();
        bool translated = session && !session->isSuspended() && !m_outlineMask.isNull()
            && bounds.size() == m_outlineBounds.size() && m_outlineBounds.translated(delta) == bounds;
        // Compare raster coverage rather than assuming floating-point affine
        // translations produce identical boundary pixels. This preserves the
        // exact clipped marching outline while avoiding costly path rebuilding.
        for (int y = 0; translated && y < bounds.height(); ++y)
            translated = std::memcmp(sel.mask().constScanLine(bounds.y() + y) + bounds.x(),
                                      m_outlineMask.constScanLine(m_outlineBounds.y() + y) + m_outlineBounds.x(),
                                      size_t(bounds.width())) == 0;
        if (translated) m_selectionOutline.translate(delta);
        else {
            QPainterPath path; path.addRegion(sel.region());
            m_selectionOutline = path.simplified();
        }
        m_outlineBounds = bounds;
        m_outlineMask = sel.mask(); // COW reference, not another full raster copy
        m_selectionOutlineKey = sel.mask().cacheKey();
    }
    if (m_selectionOutline.isEmpty()) return;

    painter.save();
    int rs = m_showRulers ? 20 : 0;
    painter.translate(m_pan + QPointF(rs, rs));
    painter.scale(m_zoom, m_zoom);

    QPen pen1(Qt::white, 1.0 / m_zoom);
    QPen pen2(Qt::black, 1.0 / m_zoom, Qt::DashLine);
    QVector<qreal> dashPattern;
    dashPattern << 4 << 4;
    pen2.setDashPattern(dashPattern);
    pen2.setDashOffset(m_marchingOffset);

    // Build ONE outline for the whole region. Adding each region rect as its own
    // subpath and stroking it outlined every internal scanline rectangle, which
    // filled non-rectangular selections (ellipse, lasso) with static instead of
    // just tracing the perimeter (issue #15). simplified() merges the rects so
    // only the true boundary is stroked.
    painter.setPen(pen1);
    painter.drawPath(m_selectionOutline);
    painter.setPen(pen2);
    painter.drawPath(m_selectionOutline);
    painter.restore();
}

void CanvasWidget::updateMarchingAnts() {
    m_marchingOffset = (m_marchingOffset + 1) % 8;
    if (m_document && m_document->selection().hasSelection())
        update();
}

void CanvasWidget::mousePressEvent(QMouseEvent *event) {
    flushFloatingMove();
    // Pan with the middle button, or with the left button while Space is held —
    // Paint.NET's shortcut. Alt is deliberately NOT a pan modifier: it is the
    // selection-subtract key and the two would collide.
    if (event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton && m_spaceDown)) {
        cancelSelectionDrag();
        m_isPanning = true;
        m_lastPanPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    // Shape selection tools use either button to draw. Other tools retain the
    // context menu on bare right-click; combination gestures always reach tools.
    if (event->button() == Qt::RightButton && event->modifiers() == Qt::NoModifier
        && m_document && m_document->selection().hasSelection()
        && !(m_currentTool && (m_currentTool->type() == ToolType::Move
            || m_currentTool->type() == ToolType::RectSelection
            || m_currentTool->type() == ToolType::EllipseSelection
            || m_currentTool->type() == ToolType::LassoSelection))) {
        QPoint canvasPos = widgetToCanvas(event->position()).toPoint();
        if (m_document->selection().isSelected(canvasPos.x(), canvasPos.y())) {
            emit selectionContextMenuRequested(event->globalPosition().toPoint());
            event->accept();
            return;
        }
    }

    // A stylus that is only hovering (tip not touching) still makes Qt synthesise
    // mouse events — including button presses from the barrel button — that
    // painted a hover blob (issue #17). Drop any press while the pen is not in
    // contact, whether the event is synthesised OR arrives while the pen is in
    // proximity. Real mouse presses (pen away, not synthesised) are never gated.
    const bool fromPen = m_penInProximity || event->source() != Qt::MouseEventNotSynthesized;
    if (fromPen && !m_penInContact)
        return;

    if (m_currentTool && m_document) {
        const auto type = m_currentTool->type();
        const bool selecting = type == ToolType::RectSelection || type == ToolType::EllipseSelection
            || type == ToolType::LassoSelection || type == ToolType::MagicWand || type == ToolType::MoveSelection;
        if (selecting && !m_document->finishFloatingPixels(false)) return;
        if (type != ToolType::Move && !selecting && m_document->floatingPixelSession()) {
            if (!m_document->suspendFloatingPixels()) return;
            if (!m_document->selection().hasSelection() && type != ToolType::Pan && type != ToolType::Zoom
                && type != ToolType::ColorPicker) return;
        }
        setFocus(Qt::MouseFocusReason);   // so Enter/Escape reach the tool (Line/Curve commit)
        // A real mouse press (not synthesised from a stylus) always means full
        // pressure. Without this, a low-pressure stylus stroke left m_pressure
        // stuck near 0 and the mouse brush kept drawing faint until restart (#16).
        // But NEVER reset while the pen tip is in contact: on some drivers the
        // stylus press (tip or barrel button) is reported as a non-synthesised
        // mouse event, and resetting to 1.0 then made the first primary dab a
        // full-size blob and killed pressure on the secondary (barrel) stroke
        // (issue #17). During contact the tablet drives the pressure.
        if (event->source() == Qt::MouseEventNotSynthesized && !m_penInContact) {
            m_pressure = 1.0;
            m_currentTool->setPressure(1.0);
        }
        QPointF canvasPos = widgetToCanvas(event->position());
        m_currentTool->mousePressEvent(canvasPos, event, *this);
        if (m_currentTool->selectionGestureActive() && type != ToolType::MoveSelection) {
            m_selectionPointer = event->position();
            m_selectionButtons = event->buttons();
            m_selectionModifiers = event->modifiers();
            m_selectionPanTimer.start();
        }
        if (!m_currentTool->updatesCanvasRegion()
            && !(m_currentTool->type() == ToolType::Move && m_document->floatingPixelSession())) m_cacheValid = false;
        update();
    }
}

void CanvasWidget::mouseMoveEvent(QMouseEvent *event) {
    m_selectionPointer = event->position();
    m_selectionButtons = event->buttons();
    m_selectionModifiers = event->modifiers();
    if (m_isPanning) {
        QPoint delta = event->pos() - m_lastPanPos;
        m_pan += QPointF(delta);
        m_autoCenter = false;   // the user placed the view; leave it alone
        m_lastPanPos = event->pos();
        update();
        return;
    }

    QPointF canvasPos = widgetToCanvas(event->position());
    emit cursorPositionChanged(canvasPos.toPoint());

    // A hovering stylus (tip not touching) must not draw, even if it synthesises
    // move events with a button held (issue #17). The cursor position above is
    // still updated so the status bar tracks the hover.
    const bool fromPen = m_penInProximity || event->source() != Qt::MouseEventNotSynthesized;
    if (fromPen && !m_penInContact)
        return;

    if (m_currentTool && m_document && m_currentTool->type() == ToolType::Move && m_document->floatingPixelSession()) {
        if (event->buttons() != Qt::NoButton) {
            m_floatingMovePosition = canvasPos;
            m_floatingMoveButtons = event->buttons();
            m_floatingMoveModifiers = event->modifiers();
            m_pendingFloatingMove = true;
            // Never restart: bound latency while replacing stale queued positions.
            if (!m_floatingMoveTimer.isActive()) m_floatingMoveTimer.start();
        }
        return;
    }
    if (m_currentTool && m_document) {
        m_currentTool->mouseMoveEvent(canvasPos, event, *this);
        if (!m_currentTool->updatesCanvasRegion()
            && !(m_currentTool->type() == ToolType::Move && m_document->floatingPixelSession())) m_cacheValid = false;
        update();
    }
}

void CanvasWidget::flushFloatingMove() {
    m_floatingMoveTimer.stop();
    if (!m_pendingFloatingMove) return;
    m_pendingFloatingMove = false;
    if (!m_document || !m_document->floatingPixelSession() || !m_currentTool || m_currentTool->type() != ToolType::Move) return;
    const QPointF widget = canvasToWidget(m_floatingMovePosition);
    QMouseEvent event(QEvent::MouseMove, widget, widget, Qt::NoButton, m_floatingMoveButtons, m_floatingMoveModifiers);
    m_currentTool->mouseMoveEvent(m_floatingMovePosition, &event, *this);
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent *event) {
    if (m_currentTool && m_document && m_currentTool->type() == ToolType::Move
        && m_document->floatingPixelSession()
        && (event->button() == Qt::LeftButton || event->button() == Qt::RightButton)) {
        // The release position is authoritative even if the last native move
        // was compressed or the frame timer has not fired yet.
        m_floatingMovePosition = widgetToCanvas(event->position());
        m_floatingMoveButtons = event->button();
        m_floatingMoveModifiers = event->modifiers();
        m_pendingFloatingMove = true;
    }
    flushFloatingMove();
    if (m_isPanning && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        m_isPanning = false;
        setCursor(Qt::ArrowCursor);
        return;
    }

    if (m_currentTool && m_document) {
        m_currentTool->mouseReleaseEvent(widgetToCanvas(event->position()), event, *this);
        if (!m_currentTool->selectionGestureActive()) m_selectionPanTimer.stop();
        if (!m_currentTool->updatesCanvasRegion()
            && !(m_currentTool->type() == ToolType::Move && m_document->floatingPixelSession())) m_cacheValid = false;
        update();
        emit canvasModified();
    }
}

void CanvasWidget::wheelEvent(QWheelEvent *event) {
    QPointF oldCanvasPos = widgetToCanvas(event->position());
    double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    setZoom(m_zoom * factor);

    QPointF newWidgetPos = canvasToWidget(oldCanvasPos);
    m_pan += event->position() - newWidgetPos;
    m_autoCenter = false;   // wheel zoom anchors on the cursor; keep that anchor
    update();
}

void CanvasWidget::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape && m_currentTool && m_currentTool->selectionGestureActive()) {
        cancelSelectionDrag();
        event->accept();
        return;
    }
    if (m_document && m_document->floatingPixelSession()
        && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !(m_currentTool && m_currentTool->wantsKeyInput())) {
        if (m_currentTool) m_currentTool->deactivate(*this);
        m_document->finishFloatingPixels();
        event->accept();
        return;
    }
    // Space arms pan-drag for any tool (unless the Text tool is capturing keys).
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()
        && !(m_currentTool && m_currentTool->wantsKeyInput())) {
        if (!m_spaceDown) {
            m_spaceDown = true;
            if (!m_isPanning) setCursor(Qt::OpenHandCursor);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Delete && m_document && m_document->selection().hasSelection()) {
        emit deleteSelectionRequested();
        event->accept();
        return;
    }
    // Backspace fills the selection (paint.net). Handled here, like Delete, so it
    // fires reliably whenever the canvas has focus — but not while the Text tool
    // is typing, where Backspace must delete a character instead.
    if (event->key() == Qt::Key_Backspace && m_document
        && !(m_currentTool && m_currentTool->wantsKeyInput())) {
        emit fillSelectionRequested();
        event->accept();
        return;
    }

    if (m_currentTool) {
        m_currentTool->keyPressEvent(event, *this);
        m_cacheValid = false;
        update();  // reflect tool state (e.g. text being typed) immediately
    }
    QWidget::keyPressEvent(event);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat() && m_spaceDown) {
        m_spaceDown = false;
        if (!m_isPanning && m_currentTool) setCursor(m_currentTool->cursor());
        event->accept();
        return;
    }
    if (m_currentTool) {
        m_currentTool->keyReleaseEvent(event, *this);
        update();
    }
    QWidget::keyReleaseEvent(event);
}

void CanvasWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    // The startup view has to be centred whatever the final window size turns out
    // to be: the window manager maximises us *after* the first layout pass, so
    // centring only at construction leaves the image off-centre. Re-centring on
    // every resize until the user pans keeps it right, maximised or restored.
    if (m_autoCenter) centerView();
}

void CanvasWidget::tabletEvent(QTabletEvent *event) {
    // Record pressure, then ignore so Qt still synthesises the compatibility
    // mouse press/move/release that actually drive the tool. Accepting here would
    // swallow those and the stylus would update pressure but never draw.
    m_pressure = event->pressure() > 0.0 ? event->pressure() : m_pressure;
    if (m_currentTool)
        m_currentTool->setPressure(m_pressure);
    if (!m_tabletSeen) {
        m_tabletSeen = true;
        emit tabletDetected();   // reveal the Pressure toggle, like Paint.NET
    }

    // Track whether the tip is actually touching. The pen draws ONLY on contact;
    // while it hovers (pressure 0) — even with a barrel/right button held — it
    // must not paint. mousePressEvent/mouseMoveEvent drop the synthesised mouse
    // events that arrive during a hover (issue #17, and the stray dab at the
    // start of a stroke). A tip release ends contact.
    if (event->type() == QEvent::TabletRelease)
        m_penInContact = false;
    else
        m_penInContact = event->pressure() > 0.0;

    // Ignore so Qt still synthesises the compatibility mouse events that drive
    // the tools; the contact gate above decides whether they actually draw.
    event->ignore();
}

void CanvasWidget::cancelSelectionDrag() {
    m_selectionPanTimer.stop();
    m_selectionButtons = Qt::NoButton;
    if (m_currentTool) m_currentTool->cancelSelectionGesture(*this);
    update();
}

void CanvasWidget::panSelectionDrag() {
    if (!m_document || !m_currentTool || !m_currentTool->selectionGestureActive()
        || m_selectionButtons == Qt::NoButton) {
        cancelSelectionDrag();
        return;
    }
    const double rs = rulerSize();
    auto step = [](double pointer, double low, double high, double imageLow, double imageHigh) {
        if (pointer < low && imageLow < low)
            return qMin(qBound(1.0, (low - pointer) * 0.2, 24.0), low - imageLow);
        if (pointer > high && imageHigh > high)
            return -qMin(qBound(1.0, (pointer - high) * 0.2, 24.0), imageHigh - high);
        return 0.0;
    };
    const QPointF delta(step(m_selectionPointer.x(), rs, width(), rs + m_pan.x(),
                             rs + m_pan.x() + m_document->width() * m_zoom),
                        step(m_selectionPointer.y(), rs, height(), rs + m_pan.y(),
                             rs + m_pan.y() + m_document->height() * m_zoom));
    if (delta.isNull()) return;
    setPan(m_pan + delta);
    QMouseEvent move(QEvent::MouseMove, m_selectionPointer, mapToGlobal(m_selectionPointer.toPoint()),
                     Qt::NoButton, m_selectionButtons, m_selectionModifiers);
    const QPointF pos = widgetToCanvas(m_selectionPointer);
    m_currentTool->mouseMoveEvent(pos, &move, *this);
    emit cursorPositionChanged(toPixelPos(pos));
}
