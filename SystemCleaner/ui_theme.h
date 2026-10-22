// Module: ui_theme.h - Modern Win32 UI skin: colors, layout, and custom painting

#pragma once

#include <windows.h>

// Sidebar navigation posts this to the main window (wParam = page index)
#define WM_APP_UI_NAV (WM_APP + 40)

// Design tokens (logical pixels at 96 DPI)
namespace UiMetrics {
    constexpr int SidebarWidth = 214;
    constexpr int SidebarWidthCompact = 72;
    constexpr int ContentMarginX = 28;
    constexpr int HeaderHeight = 88;
    constexpr int StatusBarHeight = 52;
    constexpr int ButtonHeight = 34;
    constexpr int CardRadius = 14;
    constexpr int CardPadding = 12;
    constexpr int ToolbarHeight = 34;
    constexpr int CardTop = 48;
}

// Process / per-monitor DPI setup. Call before creating the main window.
void UiInitProcessDpi();
int UiScale(int px);
int UiCurrentDpi();

// Called once after all page controls exist.
void UiInstall(HWND hMain);

// Layout + painting entry points
void UiLayoutMainWindow(HWND hMain);
void UiPaintMainWindow(HWND hMain, HDC hdc);
void UiPaintPageHost(HWND hTab, HDC hdc);
void UiSyncChrome(HWND hMain);
void UiOnDpiChanged(HWND hMain, WPARAM wParam, LPARAM lParam);
void UiOnGetMinMaxInfo(MINMAXINFO* mmi);

// Combo boxes clamp their closed height; center them in a toolbar row instead
// of top-aligning them against 34px buttons/edit boxes.
void UiCenterComboInRow(HWND hCombo, int x, int width, int rowTop, int rowHeight, int dropHeight);

// Owner-draw / colors / custom draw
bool UiDrawOwnerButton(const DRAWITEMSTRUCT* dis);
HBRUSH UiCtlColor(UINT message, HDC hdc, HWND hCtl);
bool UiHandleNotify(LPARAM lParam, LRESULT* result);

// Places a list view inside the standard white content card.
void UiPlaceListViewInCard(HWND hList, const RECT& pageRect, int extraBottom);
void UiStretchLastColumn(HWND hList);
