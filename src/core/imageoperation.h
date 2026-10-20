#pragma once
#include "selection.h"
#include <functional>
using ImageOperation = std::function<QImage(const QImage &)>;
// Pure, size-preserving operation in layer coordinates; preserves selection coverage.
QImage adjustedImage(const QImage &source, const QPoint &offset,
                     const Selection &selection, const ImageOperation &operation);
