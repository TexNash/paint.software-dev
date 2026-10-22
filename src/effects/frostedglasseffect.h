#pragma once
#include "effect.h"
#include <QRandomGenerator>

class FrostedGlassEffect : public Effect {
public:
    QString name() const override { return "Frosted Glass"; }
    QImage apply(const QImage &input) override;

    int amount() const { return m_amount; }
    void setAmount(int a) { m_amount = qMax(0, a); }

    quint32 seed() const { return m_seed; }
    void setSeed(quint32 seed) { m_seed = seed; }
private:
    quint32 m_seed = QRandomGenerator::global()->generate();
    int m_amount = 3;
};
