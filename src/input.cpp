#include "input.h"
#include <QPoint>
#include <cmath>
void InputController::apply(const QJsonObject &e, const QRect &screen) {
    const auto action = e.value("action").toString();
    if (action == "release") { releaseAll(); return; }
    if (action == "key") {
        if (!e.value("key").isDouble() || !e.value("down").isBool()) return;
        int k = e.value("key").toInt(); bool down = e.value("down").toBool();
        if (down) { if (m_keys.size() >= 32 || m_keys.contains(k)) return; m_keys.insert(k); }
        else { if (!m_keys.remove(k)) return; }
        key(k, down); return;
    }
    if (action == "move" || action == "button") {
        if (!e.value("x").isDouble() || !e.value("y").isDouble() || screen.isEmpty()) return;
        double x = e.value("x").toDouble(-1), y = e.value("y").toDouble(-1);
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return;
        QPoint p(screen.x() + qRound(x * (screen.width()-1)), screen.y() + qRound(y * (screen.height()-1)));
        if (action == "button") {
            int b = e.value("button").toInt(); if (b < 1 || b > 3 || !e.value("down").isBool()) return;
            bool down = e.value("down").toBool();
            if (down) { if (m_buttons.contains(b)) return; m_buttons.insert(b); }
            else { if (!m_buttons.remove(b)) return; }
            pointerButton(p,b,down);
        } else movePointer(p);
    } else if (action == "scroll") {
        const int ticks = qBound(-10,e.value("ticks").toInt(),10);
        if (!ticks) return;
        scrollWheel(ticks);
    }
}
void InputController::releaseAll() {
#ifdef Q_OS_WIN
    m_captionWindow=0; m_captionHit=0;
#endif
    auto keys = m_keys; auto buttons = m_buttons;
    for (int k : keys) { m_keys.remove(k); key(k,false); }
    for (int b : buttons) { m_buttons.remove(b); button(b,false); }
}
