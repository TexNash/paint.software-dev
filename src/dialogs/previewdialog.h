#pragma once

#include <QDialog>
#include <QVector>

class QLabel;
class QSlider;
class QSpinBox;

// Reusable parameter controls driving a canvas preview via parametersChanged().
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
    PreviewDialog(const QString &title, QVector<Param> params, QWidget *parent = nullptr);

    QVector<int> values() const;

signals:
    void parametersChanged();

private:
    void setupUi(const QString &title);

    QVector<Param> m_params;
    QVector<QSlider *> m_sliders;
    QVector<QSpinBox *> m_spins;
};
