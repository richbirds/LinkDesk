#pragma once
#include <QJsonObject>
#include <QRect>
#include <QSet>
class InputController {
public:
    bool available(QString *reason = nullptr) const;
    void apply(const QJsonObject &event, const QRect &screen);
    void releaseAll();
    ~InputController() { releaseAll(); }
private:
    QSet<int> m_keys;
    QSet<int> m_buttons;
    void movePointer(const QPoint &position);
    void pointerButton(const QPoint &position,int button,bool down);
    void scrollWheel(int ticks);
    void key(int qtKey, bool down);
    void button(int button, bool down);
};
