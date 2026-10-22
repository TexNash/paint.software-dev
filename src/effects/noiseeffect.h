#pragma once
#include "effect.h"
#include <QRandomGenerator>

class NoiseEffect : public Effect {
public:
    QString name() const override { return "Add Noise"; }
    QImage apply(const QImage &input) override;
    int intensity() const { return m_intensity; }
    void setIntensity(int i) { m_intensity = qBound(1, i, 100); }
    quint32 seed() const { return m_seed; }
    void setSeed(quint32 seed) { m_seed = seed; }
private:
    quint32 m_seed = QRandomGenerator::global()->generate();
    int m_intensity = 30;
};
