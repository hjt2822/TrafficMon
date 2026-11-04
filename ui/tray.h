/* Tray icon and native popup menu. One icon, one owner window. */
#ifndef TM_TRAY_H
#define TM_TRAY_H

#include <windows.h>

#define TM_TRAY_CALLBACK (WM_APP + 1)
#define TM_TRAY_OPEN     40001
#define TM_TRAY_EXIT     40002
#define TM_TRAY_MINI     40003

int tm_tray_add(HWND owner, HICON icon, const wchar_t* tip);
void tm_tray_update(const wchar_t* tip);
void tm_tray_remove(void);

/* Shows the tray menu at the cursor and posts WM_COMMAND to owner. miniMode
 * decides whether the mini-window toggle reads "轻量模式" or "返回主窗口".
 * programMenu, opacityMenu and skinMenu are submenus owned by the caller and
 * attached here; the function takes ownership of all three. */
void tm_tray_menu(HWND owner, int miniMode, HMENU programMenu, HMENU opacityMenu,
                  HMENU skinMenu);

#endif /* TM_TRAY_H */