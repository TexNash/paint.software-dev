#pragma once

#include "selection.h"
#include <QImage>
#include <QRect>
#include <QTransform>
#include <memory>
#include <optional>
#include <utility>

class Layer;

// Pixel state only: does not modify its target layer. Document/history own lifecycle.
// Source pixels are UNMASKED premultiplied pixels; coverage is independent alpha.
class FloatingPixelSession {
public:
    enum class Sampling { Nearest, Bilinear, Bicubic };
    static bool validSampling(Sampling mode) { return mode >= Sampling::Nearest && mode <= Sampling::Bicubic; }
    static std::unique_ptr<FloatingPixelSession> create(
        const std::shared_ptr<Layer> &target, const QImage &source,
        const QImage &coverage, QPoint sourceOrigin, Sampling sampling = Sampling::Nearest,
        SelectionClippingMode clipping = SelectionClippingMode::Antialiased);

    SelectionClippingMode clippingMode() const { return m_clippingMode; }
    bool targets(const Layer *layer) const;
    // Return values use QImage copy-on-write. No mutable source references escape.
    QImage sourcePixels() const { return m_source; }
    QImage sourceCoverage() const { return m_coverage; }
    QImage basePixels() const { return m_base; }
    QPoint baseOffset() const { return m_baseOffset; }
    QTransform transform() const { return m_transform; }
    Sampling sampling() const { return m_sampling; }
    QPointF rotationPivot() const {
        return m_transform.map(m_rotationPivot.value_or(QRectF(QPointF(), m_source.size()).center()));
    }
    bool hasCustomRotationPivot() const { return m_rotationPivot.has_value(); }
    bool isSuspended() const { return m_suspended; }
    QRectF sourceBounds() const { return m_transform.mapRect(QRectF(QPointF(), m_source.size())); }
    QRectF samplingBounds() const;
    // Absolute source-local -> document transform. Invalid updates leave state intact.
    bool setTransform(const QTransform &transform);
    // Finite raster domain enclosing the captured base and transformed source.
    // Keep this domain when compositing modes where transparent pixels matter.
    QRectF documentBounds() const;

    // Explicit document-space region, independent of canvas bounds.
    QImage render(const QRect &documentRegion) const;
    // Bounded preview sampling; never materializes the full document.
    QImage renderScaled(const QRect &documentRegion, QSize outputSize) const;
    QImage renderCoverage(const QRect &documentRegion) const;
    QImage transformedSource(const QRect &documentRegion) const;

private:
    friend class Document; // Rebind checkpoint copies to restored layer instances.
    FloatingPixelSession() = default;
    QImage renderBase(const QRect &documentRegion) const;
    QImage sample(const QRect &documentRegion, QImage *pixels) const;
    bool sourcePoint(QPointF documentPoint, QPoint &sourcePoint) const;
    int filterLevel() const;
    std::pair<QImage,QImage> filteredImages(int &factor,bool allowBuild = true) const;
    struct FilterView {
        const QRgb *pixels;
        const uchar *mask;
        qsizetype pixelStride, maskStride;
        int width, height, count;
        double extentX,extentY,footprintX,footprintY;
        double ux,uy,vx,vy;
    };
    FilterView filterView(const QImage &pixels,const QImage &mask,int factor) const;
    static double geometricCoverage(double sx,double sy,const FilterView &input);
    static void filteredPoint(double sx,double sy,QRgb &pixel,uchar &coverage,const FilterView &input);
    static QRgb replaceCoveredPixel(QRgb destination, QRgb source, int coverage);

    uchar clippedCoverage(QPointF local, uchar filtered) const;
    SelectionClippingMode m_clippingMode = SelectionClippingMode::Antialiased;
    std::weak_ptr<Layer> m_target;
    QImage m_source, m_coverage, m_base;
    QImage m_unclippedCoverage;
    // A context boundary starts a new lift from the edited raster. At the lift's
    // original pose, preserve that raster exactly, including fractional edges.
    QImage m_liftOriginal;
    QTransform m_liftTransform;
    bool m_suspended = false;
    bool m_fullCoverage = false;
    QPoint m_baseOffset;
    QTransform m_transform, m_inverse;
    Sampling m_sampling = Sampling::Nearest;
    // Source-local UI geometry. History copies it; native reopen resets it,
    // matching the reference. It neither changes pixels nor invalidates caches.
    std::optional<QPointF> m_rotationPivot;
};
