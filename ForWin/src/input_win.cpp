#include "input.h"
#include <Qt>
#include <QHash>
#include <QDebug>
#include <QPoint>
#include <cmath>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool InputController::available(QString *reason) const {
    Q_UNUSED(reason); return true;
}

void InputController::key(int code, bool down) {
    int vk = 0;
    if ((code >= Qt::Key_A && code <= Qt::Key_Z) || (code >= Qt::Key_0 && code <= Qt::Key_9)) vk = code;
    else if (code >= Qt::Key_F1 && code <= Qt::Key_F12) vk = VK_F1 + code - Qt::Key_F1;
    else {
        static const QHash<int,int> keys = {{Qt::Key_Shift,VK_SHIFT},{Qt::Key_Control,VK_CONTROL},
            {Qt::Key_Alt,VK_MENU},{Qt::Key_Meta,VK_LWIN},{Qt::Key_Return,VK_RETURN},{Qt::Key_Enter,VK_RETURN},
            {Qt::Key_Space,VK_SPACE},{Qt::Key_Tab,VK_TAB},{Qt::Key_Escape,VK_ESCAPE},{Qt::Key_Backspace,VK_BACK},
            {Qt::Key_Delete,VK_DELETE},{Qt::Key_Left,VK_LEFT},{Qt::Key_Right,VK_RIGHT},{Qt::Key_Up,VK_UP},
            {Qt::Key_Down,VK_DOWN},{Qt::Key_Home,VK_HOME},{Qt::Key_End,VK_END},{Qt::Key_PageUp,VK_PRIOR},
            {Qt::Key_PageDown,VK_NEXT},{Qt::Key_Minus,VK_OEM_MINUS},{Qt::Key_Equal,VK_OEM_PLUS},
            {Qt::Key_Comma,VK_OEM_COMMA},{Qt::Key_Period,VK_OEM_PERIOD},{Qt::Key_Slash,VK_OEM_2},
            {Qt::Key_Semicolon,VK_OEM_1},{Qt::Key_Apostrophe,VK_OEM_7},{Qt::Key_BracketLeft,VK_OEM_4},
            {Qt::Key_BracketRight,VK_OEM_6},{Qt::Key_Backslash,VK_OEM_5},{Qt::Key_QuoteLeft,VK_OEM_3}};
        vk = keys.value(code);
    }
    if (vk) {
        INPUT e{}; e.type = INPUT_KEYBOARD; e.ki.wVk = WORD(vk);
        e.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        if (vk >= VK_PRIOR && vk <= VK_DELETE) e.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (SendInput(1,&e,sizeof(e))!=1) qWarning("Windows keyboard injection failed: %lu",GetLastError());
    }
}
void InputController::button(int code, bool down) {
    DWORD flag = code == 1 ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
        : code == 2 ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
        : (down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
    INPUT e{}; e.type = INPUT_MOUSE; e.mi.dwFlags = flag; SendInput(1,&e,sizeof(e));
}

namespace {
void absoluteMouse(const QPoint &p,DWORD buttonFlag=0) {
    // A single absolute SendInput record places and clicks atomically. Separate
    // SetCursorPos/button calls can be overridden by VM mouse integration.
    const int left=GetSystemMetrics(SM_XVIRTUALSCREEN),top=GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width=GetSystemMetrics(SM_CXVIRTUALSCREEN),height=GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width<=0 || height<=0) return;
    INPUT e{}; e.type=INPUT_MOUSE;
    e.mi.dx=LONG(std::floor((p.x()-left+0.5)*65536.0/width));
    e.mi.dy=LONG(std::floor((p.y()-top+0.5)*65536.0/height));
    e.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|buttonFlag;
    if (SendInput(1,&e,sizeof(e))!=1) qWarning("Windows mouse injection failed: %lu",GetLastError());
}
}
void InputController::movePointer(const QPoint &p) { absoluteMouse(p); }
void InputController::pointerButton(const QPoint &p,int code,bool down) {
    DWORD flag=code==1 ? (down?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP)
        : code==2 ? (down?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_RIGHTUP)
        : (down?MOUSEEVENTF_MIDDLEDOWN:MOUSEEVENTF_MIDDLEUP);
    absoluteMouse(p,flag);
}
void InputController::scrollWheel(int ticks) {
        INPUT native{}; native.type = INPUT_MOUSE; native.mi.dwFlags = MOUSEEVENTF_WHEEL;
        native.mi.mouseData = DWORD(ticks * WHEEL_DELTA); SendInput(1,&native,sizeof(native));

}
