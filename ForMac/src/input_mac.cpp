#include "input.h"
#include <Qt>
#include <QHash>
#include <QPoint>
#include <cmath>
#include <ApplicationServices/ApplicationServices.h>

bool InputController::available(QString *reason) const {
    bool ok = AXIsProcessTrusted();
    if (!ok && reason) *reason = QStringLiteral("请在系统设置 → 隐私与安全性 → 辅助功能中允许 LinkDesk，然后重新开启控制。");
    return ok;
}

void InputController::key(int code, bool down) {
    static const QHash<int, int> keys = {
        {Qt::Key_A,0},{Qt::Key_S,1},{Qt::Key_D,2},{Qt::Key_F,3},{Qt::Key_H,4},{Qt::Key_G,5},
        {Qt::Key_Z,6},{Qt::Key_X,7},{Qt::Key_C,8},{Qt::Key_V,9},{Qt::Key_B,11},{Qt::Key_Q,12},
        {Qt::Key_W,13},{Qt::Key_E,14},{Qt::Key_R,15},{Qt::Key_Y,16},{Qt::Key_T,17},
        {Qt::Key_1,18},{Qt::Key_2,19},{Qt::Key_3,20},{Qt::Key_4,21},{Qt::Key_6,22},{Qt::Key_5,23},
        {Qt::Key_Equal,24},{Qt::Key_9,25},{Qt::Key_7,26},{Qt::Key_Minus,27},{Qt::Key_8,28},
        {Qt::Key_0,29},{Qt::Key_BracketRight,30},{Qt::Key_O,31},{Qt::Key_U,32},{Qt::Key_BracketLeft,33},
        {Qt::Key_I,34},{Qt::Key_P,35},{Qt::Key_Return,36},{Qt::Key_Enter,36},{Qt::Key_L,37},
        {Qt::Key_J,38},{Qt::Key_Apostrophe,39},{Qt::Key_K,40},{Qt::Key_Semicolon,41},
        {Qt::Key_Backslash,42},{Qt::Key_Comma,43},{Qt::Key_Slash,44},{Qt::Key_N,45},
        {Qt::Key_M,46},{Qt::Key_Period,47},{Qt::Key_Tab,48},{Qt::Key_Space,49},{Qt::Key_QuoteLeft,50},
        {Qt::Key_Backspace,51},{Qt::Key_Escape,53},{Qt::Key_Meta,55},{Qt::Key_Shift,56},
        {Qt::Key_Alt,58},{Qt::Key_Control,59},{Qt::Key_F1,122},{Qt::Key_F2,120},{Qt::Key_F3,99},
        {Qt::Key_F4,118},{Qt::Key_F5,96},{Qt::Key_F6,97},{Qt::Key_F7,98},{Qt::Key_F8,100},
        {Qt::Key_F9,101},{Qt::Key_F10,109},{Qt::Key_F11,103},{Qt::Key_F12,111},
        {Qt::Key_Home,115},{Qt::Key_End,119},{Qt::Key_PageUp,116},{Qt::Key_PageDown,121},
        {Qt::Key_Delete,117},{Qt::Key_Left,123},{Qt::Key_Right,124},{Qt::Key_Down,125},{Qt::Key_Up,126}};
    if (!keys.contains(code)) return;
    auto event = CGEventCreateKeyboardEvent(nullptr, CGKeyCode(keys.value(code)), down);
    if (!event) return;
    CGEventFlags flags = 0;
    if (m_keys.contains(Qt::Key_Shift)) flags |= kCGEventFlagMaskShift;
    if (m_keys.contains(Qt::Key_Control)) flags |= kCGEventFlagMaskControl;
    if (m_keys.contains(Qt::Key_Alt)) flags |= kCGEventFlagMaskAlternate;
    if (m_keys.contains(Qt::Key_Meta)) flags |= kCGEventFlagMaskCommand;
    CGEventSetFlags(event, flags);
    CGEventPost(kCGHIDEventTap, event); CFRelease(event);
}
void InputController::button(int code, bool down) {
    auto current = CGEventCreate(nullptr);
    if (!current) return;
    auto point = CGEventGetLocation(current); CFRelease(current);
    CGMouseButton b = code == 1 ? kCGMouseButtonLeft : code == 2 ? kCGMouseButtonRight : kCGMouseButtonCenter;
    CGEventType t = code == 1 ? (down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp)
        : code == 2 ? (down ? kCGEventRightMouseDown : kCGEventRightMouseUp)
        : (down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp);
    auto e = CGEventCreateMouseEvent(nullptr, t, point, b);
    if (e) { CGEventPost(kCGHIDEventTap,e); CFRelease(e); }
}

void InputController::movePointer(const QPoint &p) {
        auto type = m_buttons.contains(1) ? kCGEventLeftMouseDragged
            : m_buttons.contains(2) ? kCGEventRightMouseDragged
            : m_buttons.contains(3) ? kCGEventOtherMouseDragged : kCGEventMouseMoved;
        auto b = m_buttons.contains(2) ? kCGMouseButtonRight : m_buttons.contains(3) ? kCGMouseButtonCenter : kCGMouseButtonLeft;
        auto native = CGEventCreateMouseEvent(nullptr,type,CGPointMake(p.x(),p.y()),b);
        if (native) { CGEventPost(kCGHIDEventTap,native); CFRelease(native); }

}
void InputController::scrollWheel(int ticks) {
        auto native = CGEventCreateScrollWheelEvent(nullptr,kCGScrollEventUnitLine,1,ticks);
        if (native) { CGEventPost(kCGHIDEventTap,native); CFRelease(native); }

}

void InputController::pointerButton(const QPoint &p,int b,bool down) { movePointer(p); button(b,down); }
