#include "clipboardpixels.h"
#include "selection.h"
#include <QtEndian>
#include <cstring>
#include <limits>

namespace {
constexpr quint32 magic = 0x50535058; // PSPX
constexpr quint32 version = 1;
constexpr int headerSize = 24;

qint64 payloadSize(int width, int height) {
    // Restrict to the representable wire size before multiplying. QByteArray
    // input length must match exactly before decode allocates either image.
    if (width <= 0 || height <= 0) return 0;
    const qint64 count = qint64(width) * height;
    if (count > (std::numeric_limits<int>::max() - headerSize) / 5) return 0;
    return headerSize + count * 5;
}
}

const char *ClipboardPixels::mimeType() {
    return "application/x-paint-software-pixels-v1";
}

bool ClipboardPixels::isValid() const {
    if (pixels.isNull() || coverage.isNull() || pixels.size() != coverage.size()
        || pixels.format() != QImage::Format_ARGB32_Premultiplied
        || coverage.format() != QImage::Format_Grayscale8) return false;
    for (int y = 0; y < coverage.height(); ++y)
        for (int x = 0; x < coverage.width(); ++x)
            if (coverage.constScanLine(y)[x]) return true;
    return false;
}

std::optional<ClipboardPixels> ClipboardPixels::capture(const QImage &image, QPoint imageOffset,
                                                       const Selection &selection) {
    if (image.isNull()) return std::nullopt;
    ClipboardPixels result;
    result.clippingMode = selection.clippingMode();
    const QImage normalized = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (normalized.isNull()) return std::nullopt;
    if (selection.hasSelection()) {
        const QRect bounds = selection.boundingRect();
        if (bounds.isEmpty()) return std::nullopt;
        // Selected transparent space survives independently of image alpha.
        // Use wide coordinates: offsets loaded from a file can be INT_MIN.
        result.pixels = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
        if (result.pixels.isNull()) return std::nullopt;
        result.pixels.fill(Qt::transparent);
        const qint64 sourceX = qint64(bounds.x()) - imageOffset.x();
        const qint64 left = qMax(qint64(0), -sourceX);
        const qint64 right = qMin(qint64(bounds.width()), qint64(normalized.width()) - sourceX);
        if (left < right)
            for (int y = 0; y < bounds.height(); ++y) {
                const qint64 sourceY = qint64(bounds.y()) + y - imageOffset.y();
                if (sourceY < 0 || sourceY >= normalized.height()) continue;
                std::memcpy(result.pixels.scanLine(y) + left * 4,
                            normalized.constScanLine(int(sourceY)) + (sourceX + left) * 4,
                            size_t(right - left) * 4);
            }
        result.coverage = selection.clippingMask().copy(bounds);
        result.sourceOrigin = bounds.topLeft();
        if (result.coverage.isNull()) return std::nullopt;
        // Do not put unselected colours inside holes onto the private clipboard.
        // Fractional coverage retains the unmasked colour for one later multiply.
        for (int y = 0; y < result.pixels.height(); ++y) {
            auto *line = reinterpret_cast<QRgb *>(result.pixels.scanLine(y));
            const uchar *mask = result.coverage.constScanLine(y);
            for (int x = 0; x < result.pixels.width(); ++x)
                if (!mask[x]) line[x] = 0;
        }
    } else {
        result.pixels = normalized.copy();
        result.coverage = QImage(image.size(), QImage::Format_Grayscale8);
        result.coverage.fill(255);
        result.sourceOrigin = imageOffset;
    }
    if (!result.isValid()) return std::nullopt;
    return result;
}

QImage ClipboardPixels::standardImage() const {
    if (!isValid()) return {};
    Selection selection(pixels.width(), pixels.height());
    selection.setMaskImage(coverage);
    return selection.getMaskedImage(pixels);
}

QByteArray ClipboardPixels::encode() const {
    if (!isValid()) return {};
    const qint64 size = payloadSize(pixels.width(), pixels.height());
    if (!size) return {};
    QByteArray result(int(size), Qt::Uninitialized);
    auto *bytes = reinterpret_cast<uchar *>(result.data());
    const quint32 fields[] = {magic, version, quint32(pixels.width()), quint32(pixels.height()),
                              quint32(sourceOrigin.x()), quint32(sourceOrigin.y())};
    for (int i = 0; i < 6; ++i) qToLittleEndian(fields[i], bytes + i * 4);
    // Rows have no wire padding; colour words are explicitly little endian.
    uchar *pixelBytes = bytes + headerSize;
    uchar *maskBytes = pixelBytes + qint64(pixels.width()) * pixels.height() * 4;
    for (int y = 0; y < pixels.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x)
            qToLittleEndian(quint32(line[x]), pixelBytes + (qint64(y) * pixels.width() + x) * 4);
        std::memcpy(maskBytes + qint64(y) * pixels.width(), coverage.constScanLine(y), pixels.width());
    }
    return result;
}

std::unique_ptr<QMimeData> ClipboardPixels::mimeData() const {
    const QByteArray encoded = encode();
    if (encoded.isEmpty()) return nullptr;
    const QImage image = standardImage();
    if (image.isNull()) return nullptr;
    auto mime = std::make_unique<QMimeData>();
    mime->setImageData(image);
    mime->setData(mimeType(), encoded);
    mime->setData("application/x-paint-software-clipping", clippingMode == SelectionClippingMode::Pixelated ? "pixelated" : "antialiased");
    return mime;
}

std::optional<ClipboardPixels> ClipboardPixels::decode(const QMimeData &mime) {
    const QByteArray data = mime.data(mimeType());
    if (data.size() < headerSize) return std::nullopt;
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
    if (qFromLittleEndian<quint32>(bytes) != magic
        || qFromLittleEndian<quint32>(bytes + 4) != version) return std::nullopt;
    const quint32 width = qFromLittleEndian<quint32>(bytes + 8);
    const quint32 height = qFromLittleEndian<quint32>(bytes + 12);
    if (width > quint32(std::numeric_limits<int>::max())
        || height > quint32(std::numeric_limits<int>::max())) return std::nullopt;
    const qint64 size = payloadSize(int(width), int(height));
    if (!size || size != data.size()) return std::nullopt;
    ClipboardPixels result;
    const QByteArray clipping = mime.data("application/x-paint-software-clipping");
    result.clippingMode = clipping == "pixelated" ? SelectionClippingMode::Pixelated : SelectionClippingMode::Antialiased;
    result.pixels = QImage(int(width), int(height), QImage::Format_ARGB32_Premultiplied);
    result.coverage = QImage(int(width), int(height), QImage::Format_Grayscale8);
    if (result.pixels.isNull() || result.coverage.isNull()) return std::nullopt;
    result.coverage.fill(0);
    result.sourceOrigin = QPoint(qFromLittleEndian<qint32>(bytes + 16), qFromLittleEndian<qint32>(bytes + 20));
    const uchar *pixelBytes = bytes + headerSize;
    const uchar *maskBytes = pixelBytes + qint64(width) * height * 4;
    for (int y = 0; y < int(height); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.pixels.scanLine(y));
        for (int x = 0; x < int(width); ++x) {
            const QRgb pixel = qFromLittleEndian<quint32>(pixelBytes + (qint64(y) * width + x) * 4);
            if (qRed(pixel) > qAlpha(pixel) || qGreen(pixel) > qAlpha(pixel)
                || qBlue(pixel) > qAlpha(pixel)) return std::nullopt;
            line[x] = pixel;
        }
        std::memcpy(result.coverage.scanLine(y), maskBytes + qint64(y) * width, width);
    }
    if (!result.isValid()) return std::nullopt;
    return result;
}

QPoint ClipboardPixels::pasteOrigin(const QRectF &visibleCanvas, const QSize &canvasSize) const {
    const QRectF canvas(QPointF(), canvasSize);
    const QRectF visible = visibleCanvas.intersected(canvas);
    QPointF origin = sourceOrigin;
    if (!visible.isEmpty() && !visible.intersects(QRectF(origin, pixels.size()))) {
        const QPointF half(pixels.width()/2.0, pixels.height()/2.0);
        const QPointF centre = visible.center();
        const QPointF direction = origin + half - centre;
        // Inset by half the sample so the whole sample fits when possible.
        // A sample larger than the view is centred on that axis.
        const double extentX = qMax(0.0, (visible.width()-pixels.width())/2.0);
        const double extentY = qMax(0.0, (visible.height()-pixels.height())/2.0);
        double fraction = 1.0;
        if (direction.x() != 0) fraction = qMin(fraction, extentX/qAbs(direction.x()));
        if (direction.y() != 0) fraction = qMin(fraction, extentY/qAbs(direction.y()));
        origin = centre + direction*fraction - half;
    }
    QPoint result = origin.toPoint();
    result.setX(qBound(0, result.x(), qMax(0, canvasSize.width()-pixels.width())));
    result.setY(qBound(0, result.y(), qMax(0, canvasSize.height()-pixels.height())));
    return result;
}
