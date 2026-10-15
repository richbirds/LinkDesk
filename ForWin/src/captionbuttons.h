#pragma once
#include <windows.h>
#include <oleacc.h>

// Qt can return HTCAPTION for standard DWM caption buttons when queried with
// WM_NCHITTEST. Use the actual Windows accessibility rectangles instead.
inline int captionButtonAt(HWND window,POINT point) {
    TITLEBARINFOEX info{}; info.cbSize=sizeof(info); DWORD_PTR ignored=0;
    if(SendMessageTimeoutW(window,WM_GETTITLEBARINFOEX,0,reinterpret_cast<LPARAM>(&info),
                           SMTO_ABORTIFHUNG,100,&ignored)) {
        const int indices[]={2,3,5},hits[]={HTMINBUTTON,HTMAXBUTTON,HTCLOSE};
        for(int i=0;i<3;++i) {
            const auto state=info.rgstate[indices[i]];
            if(!(state&(STATE_SYSTEM_UNAVAILABLE|STATE_SYSTEM_INVISIBLE|STATE_SYSTEM_OFFSCREEN))
                && PtInRect(&info.rgrect[indices[i]],point)) return hits[i];
        }
    }
    return HTNOWHERE;
}
