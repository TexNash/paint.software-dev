#pragma once

#include <QDialog>
#include <QVector>
#include <QImage>
#include <functional>

class QLabel;
class QSlider;
class QSpinBox;

// Reusable parameter controls with an optional embedded image preview.
// Parameters-only callers can drive a separate canvas preview via parametersChanged().
class PreviewDialog : public QDialog {
    Q_OBJECT
public:
    struct Param {
        QString label;
        int min;
        int max;
        int value;
        QString suffix;   // optional, e.g. "%"
    };
    using Renderer = std::function<QImage(const QImage &, const QVector<int> &)>;

    PreviewDialog(const QString &title, QVector<Param> params, QWidget *parent = nullptr);

    PreviewDialog(const QString &title, const QImage &source,
                  QVector<Param> params, Renderer renderer, QWidget *parent = nullptr);

    QVector<int> values() const;

signals:
    void parametersChanged();

private:
    void setupUi(const QString &title);
    void updatePreview();

    QImage m_previewSource;   // downscaled source, for a responsive preview
    Renderer m_renderer;
    QVector<Param> m_params;
    QVector<QSlider *> m_sliders;
    QVector<QSpinBox *> m_spins;
    QLabel *m_preview = nullptr;
};
