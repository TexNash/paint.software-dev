#include "previewdialog.h"

#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QPushButton>

PreviewDialog::PreviewDialog(const QString &title, QVector<Param> params, QWidget *parent)
    : QDialog(parent), m_params(std::move(params)) {
    setupUi(title);
}

void PreviewDialog::setupUi(const QString &title) {
    setWindowTitle(title);
    auto *root = new QVBoxLayout(this);

    auto *grid = new QGridLayout;
    grid->setColumnStretch(1, 1);
    for (int i = 0; i < m_params.size(); ++i) {
        const Param &p = m_params[i];
        auto *lab = new QLabel(p.label);
        auto *slider = new QSlider(Qt::Horizontal);
        slider->setRange(p.min, p.max);
        slider->setValue(p.value);
        slider->setMinimumWidth(180);
        auto *spin = new QSpinBox;
        spin->setRange(p.min, p.max);
        spin->setValue(p.value);
        if (!p.suffix.isEmpty()) spin->setSuffix(p.suffix);

        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), slider, &QSlider::setValue);
        connect(slider, &QSlider::valueChanged, this, [this]() {
            emit parametersChanged();
        });

        grid->addWidget(lab, i, 0);
        grid->addWidget(slider, i, 1);
        grid->addWidget(spin, i, 2);
        m_sliders.push_back(slider);
        m_spins.push_back(spin);
    }
    root->addLayout(grid);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Reset);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked, this, [this]() {
        for (int i = 0; i < m_params.size(); ++i)
            m_sliders[i]->setValue(m_params[i].value);
    });
}

QVector<int> PreviewDialog::values() const {
    QVector<int> v;
    v.reserve(m_spins.size());
    for (auto *s : m_spins) v.push_back(s->value());
    return v;
}
