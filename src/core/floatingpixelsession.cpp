#include "floatingpixelsession.h"
#include "layer.h"
#include "floatingrendercache.h"
#include <QDataStream>
#include <QIODevice>
#include <cmath>
#include <QPainter>
#include <cstring>
#include <limits>
#include <vector>
#include <algorithm>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace {
bool integerTranslation(const QTransform &transform) {
    return transform.type() <= QTransform::TxTranslate
        && transform.dx() == std::floor(transform.dx()) && transform.dy() == std::floor(transform.dy())
        && transform.dx() >= std::numeric_limits<int>::min() && transform.dx() <= std::numeric_limits<int>::max()
        && transform.dy() >= std::numeric_limits<int>::min() && transform.dy() <= std::numeric_limits<int>::max();
}
}


std::unique_ptr<FloatingPixelSession> FloatingPixelSession::create(
    const std::shared_ptr<Layer> &target, const QImage &source,
    const QImage &coverage, QPoint sourceOrigin, Sampling sampling, SelectionClippingMode clipping) {
    if (!validSampling(sampling) || !target || source.isNull() || coverage.isNull()
        || source.size() != coverage.size()
        || coverage.format() != QImage::Format_Grayscale8 || target->image().isNull())
        return nullptr;
    bool selected = false;
    for (int y = 0; y < coverage.height() && !selected; ++y)
        for (int x = 0; x < coverage.width(); ++x)
            if (coverage.constScanLine(y)[x] > (clipping == SelectionClippingMode::Pixelated ? 127 : 0)) { selected = true; break; }
    if (!selected) return nullptr;

    auto session = std::unique_ptr<FloatingPixelSession>(new FloatingPixelSession);
    // Deep copies also isolate externally backed images and previously obtained
    // writable bits() pointers, which implicit sharing alone cannot protect against.
    session->m_source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied).copy();
    session->m_unclippedCoverage = coverage.copy();
    session->m_coverage = Selection::clippedMask(session->m_unclippedCoverage, clipping);
    session->m_clippingMode = clipping;
    session->m_base = target->image().copy();
    if (session->m_source.isNull() || session->m_coverage.isNull() || session->m_base.isNull())
        return nullptr;
    session->m_fullCoverage = true;
    for (int y = 0; y < coverage.height() && session->m_fullCoverage; ++y)
        for (int x = 0; x < coverage.width(); ++x)
            if (session->m_coverage.constScanLine(y)[x] != 255) { session->m_fullCoverage = false; break; }
    session->m_sampling = sampling;
    session->m_target = target;
    session->m_baseOffset = target->offset();
    session->setTransform(QTransform::fromTranslate(sourceOrigin.x(), sourceOrigin.y()));
    return session;
}

bool FloatingPixelSession::targets(const Layer *layer) const {
    const auto target = m_target.lock();
    return target && target.get() == layer;
}

bool FloatingPixelSession::setTransform(const QTransform &transform) {
    if (!transform.isAffine()) return false;
    for (double value : {transform.m11(), transform.m12(), transform.m21(),
                         transform.m22(), transform.dx(), transform.dy()})
        if (!std::isfinite(value)) return false;
    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible) return false;
    for (double value : {inverse.m11(), inverse.m12(), inverse.m21(),
                         inverse.m22(), inverse.dx(), inverse.dy()})
        if (!std::isfinite(value)) return false;
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), m_source.size()))
                              .united(QRectF(m_baseOffset, m_base.size()));
    for (double value : {bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                         bounds.right(), bounds.bottom()})
        if (!std::isfinite(value)) return false;
    if (m_rotationPivot) {
        const QPointF pivot = transform.map(*m_rotationPivot);
        if (!std::isfinite(pivot.x()) || !std::isfinite(pivot.y())) return false;
    }
    m_transform = transform;
    m_inverse = inverse;
    return true;
}

QRectF FloatingPixelSession::documentBounds() const {
    return samplingBounds()
        .united(QRectF(m_baseOffset, m_base.size()));
}

bool FloatingPixelSession::sourcePoint(QPointF documentPoint, QPoint &sourcePoint) const {
    const QPointF point = m_inverse.map(documentPoint);
    // Check floating-point bounds before narrowing; huge off-canvas transforms
    // must not overflow integer coordinates.
    if (!(point.x() >= 0 && point.y() >= 0
          && point.x() < m_source.width() && point.y() < m_source.height())) return false;
    sourcePoint = QPoint(int(std::floor(point.x())), int(std::floor(point.y())));
    return true;
}

QRgb FloatingPixelSession::replaceCoveredPixel(QRgb destination, QRgb source, int coverage) {
    // Encapsulated coverage policy. Both operands are premultiplied; this is
    // coverage interpolation, NOT straight-colour math or source-over blending.
    // Apply selection coverage once, including to source alpha.
    auto channel = [coverage](int dst, int src) {
        return (src * coverage + dst * (255 - coverage) + 127) / 255;
    };
    return qRgba(channel(qRed(destination), qRed(source)),
                 channel(qGreen(destination), qGreen(source)),
                 channel(qBlue(destination), qBlue(source)),
                 channel(qAlpha(destination), qAlpha(source)));
}

QRectF FloatingPixelSession::samplingBounds() const {
    // The source frame is a geometric boundary, not a zero-padded colour image.
    // toAlignedRect includes every intersecting pixel cell, including fractional
    // edges. Clamped reconstruction cannot contribute outside this domain.
    return sourceBounds();
}

int FloatingPixelSession::filterLevel() const {
    if (m_sampling == Sampling::Nearest) return 0;
    // Minimum singular value: only prefilter when BOTH axes are being reduced.
    // This avoids turning one-axis reductions into a blur along the other axis.
    const long double a=m_inverse.m11(), b=m_inverse.m12(), c=m_inverse.m21(), d=m_inverse.m22();
    const long double trace=a*a+b*b+c*c+d*d, det=a*d-b*c;
    const long double maximum=std::sqrt((trace+std::sqrt(std::max(0.0L,trace*trace-4*det*det)))/2);
    const long double minimum=maximum>0 ? std::abs(det)/maximum : 0;
    int level=0;
    int extent=qMax(m_source.width(),m_source.height());
    while (level<29 && extent>1 && minimum>=std::ldexp(2.0L,level)) {
        ++level; extent=(extent+1)/2;
    }
    return level;
}

std::pair<QImage,QImage> FloatingPixelSession::filteredImages(int &factor,bool allowBuild) const {
    QImage pixels=m_source, mask=m_coverage;
    factor=1;
    for (int level=1;level<=filterLevel();++level) {
        QByteArray key; QDataStream stream(&key,QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << quint8(2) << m_source.cacheKey() << m_coverage.cacheKey() << level;
        auto reduced=FloatingRenderCache::shared().find(key);
        if (reduced.isNull()) {
            if (!allowBuild) return {pixels,mask};
            const QSize size((pixels.width()+1)/2,(pixels.height()+1)/2);
            reduced.pixels=QImage(size,QImage::Format_ARGB32_Premultiplied);
            reduced.coverage=QImage(size,QImage::Format_Grayscale8);
            if (reduced.isNull() || reduced.coverage.isNull()) return {};
            for (int y=0;y<size.height();++y) {
                auto *out=reinterpret_cast<QRgb *>(reduced.pixels.scanLine(y));
                auto *coverage=reduced.coverage.scanLine(y);
                for (int x=0;x<size.width();++x) {
                    int selected=0, red=0, green=0, blue=0, alpha=0, count=0;
                    for (int dy=0;dy<2 && 2*y+dy<pixels.height();++dy) {
                        const auto *in=reinterpret_cast<const QRgb *>(pixels.constScanLine(2*y+dy));
                        const auto *m=mask.constScanLine(2*y+dy);
                        for (int dx=0;dx<2 && 2*x+dx<pixels.width();++dx) {
                            const int c=m[2*x+dx]; const QRgb p=in[2*x+dx];
                            ++count;
                            selected+=c; red+=c*((p>>16)&255); green+=c*((p>>8)&255);
                            blue+=c*(p&255); alpha+=c*(p>>24);
                        }
                    }
                    coverage[x]=uchar((selected+count/2)/count);
                    out[x]=selected ? qRgba((red+selected/2)/selected,(green+selected/2)/selected,
                                            (blue+selected/2)/selected,(alpha+selected/2)/selected) : 0;
                }
            }
            FloatingRenderCache::shared().insert(key,reduced);
        }
        pixels=reduced.pixels; mask=reduced.coverage; factor*=2;
    }
    return {pixels,mask};
}

FloatingPixelSession::FilterView FloatingPixelSession::filterView(const QImage &pixels,const QImage &mask,int factor) const {
    return {reinterpret_cast<const QRgb *>(pixels.constBits()),mask.constBits(),
            pixels.bytesPerLine()/4,mask.bytesPerLine(),pixels.width(),pixels.height(),
            m_sampling == Sampling::Bicubic ? 4 : 2,
            double(m_source.width())/factor,double(m_source.height())/factor,
            (std::abs(m_inverse.m11())+std::abs(m_inverse.m21()))/factor,
            (std::abs(m_inverse.m12())+std::abs(m_inverse.m22()))/factor,
            m_inverse.m11()/(2*double(factor)),m_inverse.m12()/(2*double(factor)),
            m_inverse.m21()/(2*double(factor)),m_inverse.m22()/(2*double(factor))};
}

double FloatingPixelSession::geometricCoverage(double sx,double sy,const FilterView &input) {
    const double hx=input.footprintX/2, hy=input.footprintY/2;
    if (!(sx>-hx && sy>-hy && sx<input.extentX+hx && sy<input.extentY+hy)) return 0;
    if (sx>=hx && sy>=hy && sx<=input.extentX-hx && sy<=input.extentY-hy) return 1;
    // Clip the inverse-mapped destination pixel against the source frame.
    // Pixel-relative vertices avoid cancellation at large document coordinates.
    struct Point { double x,y; };
    Point a[12]={{-input.ux-input.vx,-input.uy-input.vy},{input.ux-input.vx,input.uy-input.vy},
                 {input.ux+input.vx,input.uy+input.vy},{-input.ux+input.vx,-input.uy+input.vy}};
    Point b[12];
    const double denominator=4*std::abs(input.ux*input.vy-input.uy*input.vx);
    // Extremely ill-conditioned finite transforms may under/overflow area.
    const auto overlap=[](double point,double extent,double footprint) {
        if (!(footprint>0)) return point>=0 && point<extent ? 1.0 : 0.0;
        const double left=std::max(0.0,point-footprint/2), right=std::min(extent,point+footprint/2);
        return std::max(0.0,std::min(1.0,(right-left)/footprint));
    };
    if (!(denominator>0) || !std::isfinite(denominator)
        || !std::isfinite(input.footprintX) || !std::isfinite(input.footprintY))
        return overlap(sx,input.extentX,input.footprintX)*overlap(sy,input.extentY,input.footprintY);
    int count=4;
    for (int edge=0;edge<4 && count;++edge) {
        const bool horizontal=edge<2, lower=edge%2==0;
        const double limit=edge==0 ? -sx : edge==1 ? input.extentX-sx : edge==2 ? -sy : input.extentY-sy;
        auto coordinate=[horizontal](Point p) { return horizontal ? p.x : p.y; };
        auto inside=[&](Point p) { return lower ? coordinate(p)>=limit : coordinate(p)<=limit; };
        int next=0; Point previous=a[count-1]; bool wasInside=inside(previous);
        for (int i=0;i<count;++i) {
            const Point current=a[i]; const bool isInside=inside(current);
            if (isInside!=wasInside) {
                const double t=(limit-coordinate(previous))/(coordinate(current)-coordinate(previous));
                b[next++]={previous.x+t*(current.x-previous.x),previous.y+t*(current.y-previous.y)};
            }
            if (isInside) b[next++]=current;
            previous=current; wasInside=isInside;
        }
        count=next; for (int i=0;i<count;++i) a[i]=b[i];
    }
    double area=0;
    for (int i=0;i<count;++i) { const Point p=a[i], q=a[(i+1)%count]; area+=p.x*q.y-p.y*q.x; }
    return std::max(0.0,std::min(1.0,std::abs(area)/(2*denominator)));
}

void FloatingPixelSession::filteredPoint(double sxPoint,double syPoint,QRgb &pixel,uchar &coverage,const FilterView &input) {
    pixel = 0; coverage = 0;
    const int count=input.count;
    const double geometric=geometricCoverage(sxPoint,syPoint,input);
    if (!(geometric>0)) return;
    sxPoint=std::max(0.5,std::min(double(input.width)-0.5,sxPoint));
    syPoint=std::max(0.5,std::min(double(input.height)-0.5,syPoint));
    const double px = sxPoint-0.5, py = syPoint-0.5;
    const int ix = int(std::floor(px)), iy = int(std::floor(py));
    const double fx = px-ix, fy = py-iy;
    double wx[4], wy[4];
    auto weights = [count](double t, double *w) {
        if (count == 2) { w[0] = 1-t; w[1] = t; return; }
        // Catmull-Rom cubic convolution (a=-0.5), interpolating at pixel centres.
        const double t2=t*t, t3=t2*t;
        w[0] = -0.5*t+t2-0.5*t3; w[1] = 1-2.5*t2+1.5*t3;
        w[2] = 0.5*t+2*t2-1.5*t3; w[3] = -0.5*t2+0.5*t3;
    };
    weights(fx,wx); weights(fy,wy);
    const int left=ix-(count == 4), top=iy-(count == 4);
    float selected=0;
    float channels[4]={}; // Blue, green, red, alpha; valid on either byte order.
#if defined(__SSE2__)
    __m128 accumulated=_mm_setzero_ps();
    const __m128i zero=_mm_setzero_si128();
#endif
    for (int y=0;y<count;++y) {
        const int sy=std::max(0,std::min(input.height-1,top+y));
        const auto *pixels=input.pixels+sy*input.pixelStride;
        const uchar *mask=input.mask+sy*input.maskStride;
        for (int x=0;x<count;++x) {
            const int sx=std::max(0,std::min(input.width-1,left+x));
            const float weight=float(wx[x]*wy[y]*mask[sx]);
            const QRgb p=pixels[sx];
            selected+=weight;
#if defined(__SSE2__)
            // Four independent premultiplied channels share one kernel weight.
            // SSE2 is baseline on x86-64; other platforms use the scalar path.
            const auto bytes=_mm_cvtsi32_si128(int(p));
            const auto words=_mm_unpacklo_epi8(bytes,zero);
            const auto values=_mm_cvtepi32_ps(_mm_unpacklo_epi16(words,zero));
            accumulated=_mm_add_ps(accumulated,_mm_mul_ps(values,_mm_set1_ps(weight)));
#else
            const float values[4]={float(p&255),float((p>>8)&255),float((p>>16)&255),float(p>>24)};
            for (int c=0;c<4;++c) channels[c]+=weight*values[c];
#endif
        }
    }
#if defined(__SSE2__)
    _mm_storeu_ps(channels,accumulated);
#endif
    // Coverage-weighted premultiplied colours avoid bleeding unselected pixels
    // into the shape. Geometry still survives fully transparent source pixels.
    const double covered=selected*geometric;
    coverage=uchar(covered <= 0 ? 0 : covered >= 255 ? 255 : int(covered+0.5));
    if (!coverage || selected<=0) return;
    const double inverse=1.0/selected;
    const double sampledAlpha=channels[3]*inverse;
    const int a=sampledAlpha <= 0 ? 0 : sampledAlpha >= 255 ? 255 : int(sampledAlpha+0.5);
    auto channel=[inverse,a](double c) { c*=inverse; return c <= 0 ? 0 : c >= a ? a : int(c+0.5); };
    pixel=(unsigned(a)<<24)|(unsigned(channel(channels[2]))<<16)|(unsigned(channel(channels[1]))<<8)|unsigned(channel(channels[0]));
}

uchar FloatingPixelSession::clippedCoverage(QPointF local, uchar filtered) const {
    if (m_clippingMode == SelectionClippingMode::Antialiased) return filtered;
    if (local.x() < 0 || local.y() < 0 || local.x() >= m_coverage.width() || local.y() >= m_coverage.height()) return 0;
    return m_coverage.constScanLine(int(local.y()))[int(local.x())] > 127 ? 255 : 0;
}

QImage FloatingPixelSession::renderBase(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    const bool originalLiftPose = !m_liftOriginal.isNull() && m_transform == m_liftTransform;
    const QImage &base = originalLiftPose ? m_liftOriginal : m_base;
    result.fill(Qt::transparent);
    {
        QPainter painter(&result);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(QPointF(double(m_baseOffset.x()) - documentRegion.x(),
                                 double(m_baseOffset.y()) - documentRegion.y()), base);
    }
    return result;
}

QImage FloatingPixelSession::render(const QRect &documentRegion) const {
    QImage result = renderBase(documentRegion);
    if (result.isNull() || (!m_liftOriginal.isNull() && m_transform == m_liftTransform)) return result;
    // Intersect before narrowing to QRect: distant off-canvas transforms need
    // no sampling and must not overflow integer coordinates.
    const QRect affected = samplingBounds().intersected(QRectF(documentRegion)).toAlignedRect().intersected(documentRegion);
    if (!affected.isEmpty() && integerTranslation(m_transform)) {
        const int sx = int(double(affected.x()) - m_transform.dx());
        const int sy = int(double(affected.y()) - m_transform.dy());
        for (int y = 0; y < affected.height(); ++y) {
            auto *destination = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y))
                + affected.x() - documentRegion.x();
            const auto *source = reinterpret_cast<const QRgb *>(m_source.constScanLine(sy + y)) + sx;
            const uchar *coverage = m_coverage.constScanLine(sy + y) + sx;
            if (m_fullCoverage) std::memcpy(destination, source, size_t(affected.width()) * 4);
            else for (int x = 0; x < affected.width(); ++x) {
                if (coverage[x] == 255) destination[x] = source[x];
                else if (coverage[x]) destination[x] = replaceCoveredPixel(destination[x], source[x], coverage[x]);
            }
        }
        return result;
    }
    if (affected.isEmpty()) return result;
    if (m_sampling != Sampling::Nearest) {
        QImage pixels;
        const QImage coverage = sample(affected, &pixels);
        if (coverage.isNull() || pixels.isNull()) return {};
        for (int y = 0; y < affected.height(); ++y) {
            auto *out = reinterpret_cast<QRgb *>(result.scanLine(affected.y()-documentRegion.y()+y)) + affected.x()-documentRegion.x();
            const auto *in = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
            const auto *mask = coverage.constScanLine(y);
            for (int x = 0; x < affected.width(); ++x)
                if (mask[x]) out[x] = replaceCoveredPixel(out[x],in[x],mask[x]);
        }
        return result;
    }
    const auto *pixels = reinterpret_cast<const QRgb *>(m_source.constBits());
    const uchar *mask = m_coverage.constBits();
    const qsizetype pixelStride = m_source.bytesPerLine() / 4, maskStride = m_coverage.bytesPerLine();
    // Match QTransform::map's classified affine paths, including its fuzzy
    // identity/axis classification, while keeping getters out of pixel loops.
    const auto type = m_inverse.type();
    const double a = type <= QTransform::TxTranslate ? 1 : m_inverse.m11();
    const double d = type <= QTransform::TxTranslate ? 1 : m_inverse.m22();
    const double b = type <= QTransform::TxScale ? 0 : m_inverse.m12();
    const double c = type <= QTransform::TxScale ? 0 : m_inverse.m21();
    const double tx = m_inverse.dx(), ty = m_inverse.dy();
    const int sourceWidth = m_source.width(), sourceHeight = m_source.height();
    if (b == 0 && c == 0 && !affected.isEmpty()) {
        std::vector<int> columns(size_t(affected.width()), -1);
        for (int x = 0; x < affected.width(); ++x) {
            const double sx = (double(affected.x()) + x + 0.5) * a + tx;
            if (sx >= 0 && sx < sourceWidth) columns[x] = int(sx);
        }
        for (int y = 0; y < affected.height(); ++y) {
            const double sy = (double(affected.y()) + y + 0.5) * d + ty;
            if (!(sy >= 0 && sy < sourceHeight)) continue;
            const auto *source = pixels + int(sy) * pixelStride;
            const auto *coverage = mask + int(sy) * maskStride;
            auto *destination = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y))
                + affected.x() - documentRegion.x();
            for (int x = 0; x < affected.width(); ++x) {
                const int sx = columns[x];
                if (sx < 0) continue;
                const int value = m_fullCoverage ? 255 : coverage[sx];
                if (value == 255) destination[x] = source[sx];
                else if (value) destination[x] = replaceCoveredPixel(destination[x], source[sx], value);
            }
        }
        return result;
    }
    for (int y = 0; y < affected.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(affected.y() - documentRegion.y() + y));
        const double py = double(affected.y()) + y + 0.5, yc = py * c, yd = py * d;
        for (int x = 0; x < affected.width(); ++x) {
            const double px = double(affected.x()) + x + 0.5;
            const double sx = px * a + yc + tx, sy = px * b + yd + ty;
            if (sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight) {
                const int ix = int(sx), iy = int(sy);
                const int coverage = mask[iy * maskStride + ix];
                auto &pixel = line[affected.x() - documentRegion.x() + x];
                const QRgb source = pixels[iy * pixelStride + ix];
                if (coverage == 255) pixel = source;
                else if (coverage) pixel = replaceCoveredPixel(pixel, source, coverage);
            }
        }
    }
    return result;
}

QImage FloatingPixelSession::renderCoverage(const QRect &documentRegion) const {
    return sample(documentRegion, nullptr);
}

QImage FloatingPixelSession::sample(const QRect &documentRegion, QImage *pixels) const {
    if (documentRegion.isEmpty()) return {};
    QImage result(documentRegion.size(), QImage::Format_Grayscale8);
    if (result.isNull()) return {};
    result.fill(0);
    if (pixels) {
        *pixels = QImage(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
        if (pixels->isNull()) return {};
        pixels->fill(0);
    }
    const auto *sourcePixels = reinterpret_cast<const QRgb *>(m_source.constBits());
    const qsizetype pixelStride = m_source.bytesPerLine() / 4;
    const QRect affected = samplingBounds().intersected(QRectF(documentRegion)).toAlignedRect().intersected(documentRegion);
    if (affected.isEmpty()) return result;
    if (!affected.isEmpty() && integerTranslation(m_transform)) {
        const int sx = int(double(affected.x()) - m_transform.dx());
        const int sy = int(double(affected.y()) - m_transform.dy());
        for (int y = 0; y < affected.height(); ++y) {
            if (pixels) std::memcpy(pixels->scanLine(affected.y() - documentRegion.y() + y) + qsizetype(affected.x() - documentRegion.x()) * 4,
                                    m_source.constScanLine(sy + y) + qsizetype(sx) * 4, size_t(affected.width()) * 4);
            std::memcpy(result.scanLine(affected.y() - documentRegion.y() + y) + affected.x() - documentRegion.x(),
                        m_coverage.constScanLine(sy + y) + sx, size_t(affected.width()));
        }
        return result;
    }
    if (m_sampling != Sampling::Nearest) {
        int factor=1;
        const auto input=filteredImages(factor);
        if (input.first.isNull() || input.second.isNull()) return {};
        const auto view=filterView(input.first,input.second,factor);
        const auto type=m_inverse.type();
        const double a=(type <= QTransform::TxTranslate ? 1 : m_inverse.m11())/factor;
        const double d=(type <= QTransform::TxTranslate ? 1 : m_inverse.m22())/factor;
        const double b=(type <= QTransform::TxScale ? 0 : m_inverse.m12())/factor;
        const double c=(type <= QTransform::TxScale ? 0 : m_inverse.m21())/factor;
        const double tx=m_inverse.dx()/factor, ty=m_inverse.dy()/factor;
        for (int y = affected.top(); !affected.isEmpty() && y <= affected.bottom(); ++y) {
            uchar *mask = result.scanLine(y-documentRegion.y());
            auto *out = pixels ? reinterpret_cast<QRgb *>(pixels->scanLine(y-documentRegion.y())) : nullptr;
            for (int x = affected.left(); x <= affected.right(); ++x) {
                QRgb pixel; uchar coverage;
                filteredPoint((double(x)+0.5)*a+(double(y)+0.5)*c+tx,
                              (double(x)+0.5)*b+(double(y)+0.5)*d+ty,pixel,coverage,view);
                coverage = clippedCoverage(m_inverse.map(QPointF(double(x)+0.5, double(y)+0.5)), coverage);
                mask[x-documentRegion.x()] = coverage;
                if (out) out[x-documentRegion.x()] = pixel;
            }
        }
        return result;
    }
    const uchar *mask = m_coverage.constBits();
    const qsizetype stride = m_coverage.bytesPerLine();
    // Match QTransform::map's classified affine paths, including its fuzzy
    // identity/axis classification, while keeping getters out of pixel loops.
    const auto type = m_inverse.type();
    const double a = type <= QTransform::TxTranslate ? 1 : m_inverse.m11();
    const double d = type <= QTransform::TxTranslate ? 1 : m_inverse.m22();
    const double b = type <= QTransform::TxScale ? 0 : m_inverse.m12();
    const double c = type <= QTransform::TxScale ? 0 : m_inverse.m21();
    const double tx = m_inverse.dx(), ty = m_inverse.dy();
    const int sourceWidth = m_source.width(), sourceHeight = m_source.height();
    if (b == 0 && c == 0 && !affected.isEmpty()) {
        std::vector<int> columns(size_t(affected.width()), -1);
        int first = affected.width(), last = -1;
        for (int x = 0; x < affected.width(); ++x) {
            const double sx = (double(affected.x()) + x + 0.5) * a + tx;
            if (sx >= 0 && sx < sourceWidth) { columns[x] = int(sx); first = qMin(first, x); last = x; }
        }
        for (int y = 0; y < affected.height(); ++y) {
            const double sy = (double(affected.y()) + y + 0.5) * d + ty;
            if (!(sy >= 0 && sy < sourceHeight)) continue;
            auto *destination = result.scanLine(affected.y() - documentRegion.y() + y) + affected.x() - documentRegion.x();
            const auto *source = mask + int(sy) * stride;
            if (pixels) {
                auto *out = reinterpret_cast<QRgb *>(pixels->scanLine(affected.y() - documentRegion.y() + y)) + affected.x() - documentRegion.x();
                const auto *in = sourcePixels + int(sy) * pixelStride;
                for (int x = first; x <= last; ++x) out[x] = in[columns[x]];
            }
            if (m_fullCoverage && last >= first) std::memset(destination + first, 255, size_t(last - first + 1));
            else for (int x = first; x <= last; ++x) destination[x] = source[columns[x]];
        }
        return result;
    }
    for (int y = 0; y < affected.height(); ++y) {
        uchar *line = result.scanLine(affected.y() - documentRegion.y() + y);
        auto *out = pixels ? reinterpret_cast<QRgb *>(pixels->scanLine(affected.y() - documentRegion.y() + y)) : nullptr;
        const double py = double(affected.y()) + y + 0.5, yc = py * c, yd = py * d;
        for (int x = 0; x < affected.width(); ++x) {
            const double px = double(affected.x()) + x + 0.5;
            const double sx = px * a + yc + tx, sy = px * b + yd + ty;
            if (sx >= 0 && sy >= 0 && sx < sourceWidth && sy < sourceHeight) {
                line[affected.x() - documentRegion.x() + x] = mask[int(sy) * stride + int(sx)];
                if (out) out[affected.x() - documentRegion.x() + x] = sourcePixels[int(sy) * pixelStride + int(sx)];
            }
        }
    }
    return result;
}

QImage FloatingPixelSession::transformedSource(const QRect &documentRegion) const {
    if (documentRegion.isEmpty()) return {};
    if (m_sampling != Sampling::Nearest) {
        QImage pixels;
        const QImage coverage=sample(documentRegion,&pixels);
        if (coverage.isNull()) return {};
        for (int y=0;y<pixels.height();++y) {
            auto *line=reinterpret_cast<QRgb *>(pixels.scanLine(y));
            const auto *mask=coverage.constScanLine(y);
            for (int x=0;x<pixels.width();++x) if (!mask[x]) line[x]=0;
        }
        return pixels;
    }
    QImage result(documentRegion.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    result.fill(Qt::transparent);
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            QPoint source;
            if (sourcePoint(QPointF(double(documentRegion.x()) + x + 0.5,
                                   double(documentRegion.y()) + y + 0.5), source)
                && m_coverage.constScanLine(source.y())[source.x()]) line[x] = m_source.pixel(source);
        }
    }
    return result;
}

QImage FloatingPixelSession::renderScaled(const QRect &region, QSize outputSize) const {
    if (region.isEmpty() || outputSize.isEmpty()) return {};
    QImage result(outputSize, QImage::Format_ARGB32_Premultiplied);
    if (result.isNull()) return {};
    const bool originalLiftPose = !m_liftOriginal.isNull() && m_transform == m_liftTransform;
    const QImage &base = originalLiftPose ? m_liftOriginal : m_base;
    int factor=1;
    const auto input=m_sampling != Sampling::Nearest && !originalLiftPose
        ? filteredImages(factor,false) : std::pair<QImage,QImage>();
    if (m_sampling != Sampling::Nearest && !originalLiftPose && input.first.isNull()) return {};
    const auto view=input.first.isNull() ? FilterView{} : filterView(input.first,input.second,factor);
    for (int y = 0; y < result.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            const QPointF point(region.x() + (x + 0.5) * region.width() / outputSize.width(),
                                region.y() + (y + 0.5) * region.height() / outputSize.height());
            const double bx = std::floor(point.x()) - m_baseOffset.x();
            const double by = std::floor(point.y()) - m_baseOffset.y();
            QRgb pixel = bx >= 0 && by >= 0 && bx < base.width() && by < base.height()
                ? base.pixel(int(bx), int(by)) : 0;
            QPoint source;
            if (!originalLiftPose && m_sampling != Sampling::Nearest) {
                QRgb sampled; uchar coverage;
                const QPointF local=m_inverse.map(point)/factor;
                filteredPoint(local.x(),local.y(),sampled,coverage,view);
                pixel = replaceCoveredPixel(pixel,sampled,clippedCoverage(m_inverse.map(point), coverage));
            } else if (!originalLiftPose && sourcePoint(point, source))
                pixel = replaceCoveredPixel(pixel, m_source.pixel(source), m_coverage.constScanLine(source.y())[source.x()]);
            line[x] = pixel;
        }
    }
    return result;
}
