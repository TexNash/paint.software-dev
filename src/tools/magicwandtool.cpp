#include "magicwandtool.h"
#include "canvas/canvaswidget.h"
#include "core/document.h"

void MagicWandTool::mousePressEvent(const QPointF &canvasPos, QMouseEvent *event, CanvasWidget &canvas) {
    // Left = combine per modifiers; right only with a modifier (Alt+right =
    // intersect, Ctrl+right = invert). Bare right-click falls through to the
    // canvas context menu.
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    if (event->button() == Qt::RightButton && event->modifiers() == Qt::NoModifier) return;
    auto *doc = canvas.document();
    if (!doc) return;

    QPoint pos = toPixelPos(canvasPos);
    // Sampling=Image reads the composite of visible layers; Sampling=Layer reads
    // only the active layer (Paint.NET). Fall back to the composite if there is
    // no active layer.
    QImage image;
    if (m_sampleImage) {
        image = doc->flattenVisible();
    } else if (auto *layer = doc->activeLayer()) {
        // Selection masks are document-sized. Sample the visible document-space
        // portion, never the retained oversized buffer (which could overrun a
        // contiguous selection's mask), and account for the layer's position.
        image = QImage(doc->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        { QPainter painter(&image); painter.drawImage(layer->offset(), layer->image()); }
        image = image.convertToFormat(QImage::Format_ARGB32);
    } else {
        image = doc->flattenVisible();
    }
    if (!image.valid(pos)) return;

    const Selection before = doc->selection();
    const SelectionMode mode = selectionModeFor(event->modifiers(), event->button(), selectionMode());

    if (m_global) {
        // Select all pixels of the same color globally
        QRgb targetColor = image.pixel(pos);
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        int tr = qRed(targetColor), tg = qGreen(targetColor), tb = qBlue(targetColor), ta = qAlpha(targetColor);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                QRgb c = image.pixel(x, y);
                int dr = qRed(c) - tr, dg = qGreen(c) - tg, db = qBlue(c) - tb, da = qAlpha(c) - ta;
                int dist = static_cast<int>(std::sqrt(dr*dr + dg*dg + db*db + da*da));
                if (dist <= toleranceDistance())
                    mask.scanLine(y)[x] = 255;
            }
        }
        // Apply mask as selection
        QPainterPath path;
        for (int y = 0; y < mask.height(); ++y) {
            const uchar *line = mask.constScanLine(y);
            const int w = mask.width();
            int start = -1;
            for (int x = 0; x < w; ++x) {
                if (line[x] > 0 && start == -1) start = x;
                else if (line[x] == 0 && start != -1) {
                    path.addRect(start, y, x - start, 1);
                    start = -1;
                }
            }
            if (start != -1) path.addRect(start, y, w - start, 1);
        }
        doc->selection().selectPath(path, mode);
    } else {
        doc->selection().selectByColor(image, pos, toleranceDistance(), mode);
    }

    doc->recordSelectionChange(before, "Magic Wand");
}

void MagicWandTool::mouseMoveEvent(const QPointF &, QMouseEvent *, CanvasWidget &) {}
void MagicWandTool::mouseReleaseEvent(const QPointF &, QMouseEvent *, CanvasWidget &) {}
