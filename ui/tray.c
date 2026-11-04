#include "tray.h"

#include <string.h>

static NOTIFYICONDATAW g_icon;
static int g_added = 0;

int tm_tray_add(HWND owner, HICON icon, const wchar_t* tip) {
    if (!owner) return 0;
    memset(&g_icon, 0, sizeof(g_icon));
    g_icon.cbSize = sizeof(g_icon);
    g_icon.hWnd = owner;
    g_icon.uID = 1;
    g_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_icon.uCallbackMessage = TM_TRAY_CALLBACK;
    g_icon.hIcon = icon;
    if (tip) lstrcpynW(g_icon.szTip, tip, (int)(sizeof(g_icon.szTip) / sizeof(g_icon.szTip[0])));
    g_added = Shell_NotifyIconW(NIM_ADD, &g_icon) ? 1 : 0;
    if (g_added) {
        g_icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_icon);
    }
    return g_added;
}

void tm_tray_update(const wchar_t* tip) {
    if (!g_added || !tip) return;
    lstrcpynW(g_icon.szTip, tip, (int)(sizeof(g_icon.szTip) / sizeof(g_icon.szTip[0])));
    g_icon.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_icon);
}

void tm_tray_remove(void) {
    if (!g_added) return;
    Shell_NotifyIconW(NIM_DELETE, &g_icon);
    g_added = 0;
}

void tm_tray_menu(HWND owner, int miniMode, HMENU programMenu, HMENU opacityMenu,
                  HMENU skinMenu) {
    if (!owner) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        if (programMenu) DestroyMenu(programMenu);
        if (opacityMenu) DestroyMenu(opacityMenu);
        if (skinMenu) DestroyMenu(skinMenu);
        return;
    }
    AppendMenuW(menu, MF_STRING, TM_TRAY_OPEN, L"打开主窗口");
    AppendMenuW(menu, MF_STRING, TM_TRAY_MINI, miniMode ? L"返回主窗口" : L"轻量模式");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (programMenu) AppendMenuW(menu, MF_POPUP, (UINT_PTR)programMenu, L"流量监控");
    if (opacityMenu) AppendMenuW(menu, MF_POPUP, (UINT_PTR)opacityMenu, L"透明度");
    if (skinMenu) AppendMenuW(menu, MF_POPUP, (UINT_PTR)skinMenu, L"主题");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, TM_TRAY_EXIT, L"退出");
    SetMenuDefaultItem(menu, TM_TRAY_OPEN, FALSE);

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(owner);
    const UINT command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                              cursor.x, cursor.y, 0, owner, NULL);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (command) SendMessageW(owner, WM_COMMAND, command, 0);
}
