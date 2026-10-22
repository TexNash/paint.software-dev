#pragma once

#include "selection.h"
#include <QImage>
#include <QMimeData>
#include <QPoint>
#include <QRectF>
#include <memory>
#include <optional>

class Selection;

// Clipboard value, not a session: pixels are unmasked, geometry is independent.
// The ordinary image representation applies coverage once for other applications.
struct ClipboardPixels {
    QImage pixels;
    QImage coverage;
    QPoint sourceOrigin;
    SelectionClippingMode clippingMode = SelectionClippingMode::Antialiased;

    static const char *mimeType();
    static std::optional<ClipboardPixels> capture(const QImage &image, QPoint imageOffset,
                                                 const Selection &selection);
    // Read only our private format. Callers retain their existing ordinary-image
    // and local-file fallbacks when it is absent or invalid.
    static std::optional<ClipboardPixels> decode(const QMimeData &mime);
    std::unique_ptr<QMimeData> mimeData() const;
    QImage standardImage() const;
    // Preserve copied document coordinates while visible; otherwise move along
    // the line toward the visible canvas centre until the sample fits the view.
    QPoint pasteOrigin(const QRectF &visibleCanvas, const QSize &canvasSize) const;

private:
    bool isValid() const;
    QByteArray encode() const;
};
