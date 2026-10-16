#pragma once
#include <QJsonObject>
#include <QRect>
#include <QSet>
class InputController {
public:
    bool available(QString *reason = nullptr) const;
    void apply(const QJsonObject &event, const QRect &screen);
    void releaseAll();
    void releaseKnown(const QSet<int> &keys,const QSet<int> &buttons) { m_keys.unite(keys);m_buttons.unite(buttons);releaseAll(); }
#ifdef Q_OS_WIN
    static bool nativeKeySupported(int code);
    static void clearNativeError();
    static unsigned long nativeError();
#endif
    ~InputController() { releaseAll(); }
private:
    QSet<int> m_keys;
    QSet<int> m_buttons;
#ifdef Q_OS_WIN
    // Bypass native caption tracking and stale VM non-client hit tests.
    quintptr m_captionWindow=0;
    int m_captionHit=0;
#endif
    void movePointer(const QPoint &position);
    void pointerButton(const QPoint &position,int button,bool down);
    void scrollWheel(int ticks);
    void key(int qtKey, bool down);
    void button(int button, bool down);
};
