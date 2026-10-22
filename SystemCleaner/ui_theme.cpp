// Module: ui_theme.cpp - Modern Win32 UI skin (custom painting, layout, DPI)
//
// Pure Win32 + GDI/GDI+. No third-party dependencies. The tab control is kept
// as the page container (so control IDs / tests stay valid) but its native
// drawing is suppressed; navigation lives in a custom-drawn sidebar.

#define NOMINMAX

#include "common.h"
#include "ui_theme.h"

#include <gdiplus.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Gdiplus;

// ====================== Design tokens ======================

namespace {

constexpr COLORREF kBg = RGB(244, 246, 250);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kBorder = RGB(230, 234, 241);
constexpr COLORREF kBorderStrong = RGB(208, 214, 224);
constexpr COLORREF kTextPrimary = RGB(16, 24, 40);
constexpr COLORREF kTextSecondary = RGB(102, 112, 133);
constexpr COLORREF kTextTertiary = RGB(152, 162, 179);
constexpr COLORREF kAccent = RGB(37, 99, 235);
constexpr COLORREF kAccentHover = RGB(29, 78, 216);
constexpr COLORREF kAccentPressed = RGB(30, 64, 175);
constexpr COLORREF kAccentSoft = RGB(234, 241, 254);
constexpr COLORREF kAccentSoftStrong = RGB(219, 232, 254);
constexpr COLORREF kSuccess = RGB(22, 163, 74);
constexpr COLORREF kSuccessHover = RGB(21, 128, 61);
constexpr COLORREF kSuccessPressed = RGB(22, 101, 52);
constexpr COLORREF kSuccessSoft = RGB(232, 247, 238);
constexpr COLORREF kDanger = RGB(220, 38, 38);
constexpr COLORREF kDangerSoft = RGB(254, 243, 242);
constexpr COLORREF kDangerBorder = RGB(241, 184, 181);
constexpr COLORREF kWarning = RGB(181, 71, 8);
constexpr COLORREF kWarningSoft = RGB(255, 247, 237);
constexpr COLORREF kSidebarTop = RGB(11, 21, 38);
constexpr COLORREF kSidebarBottom = RGB(14, 29, 51);
constexpr COLORREF kNavHover = RGB(18, 34, 57);
constexpr COLORREF kNavActive = RGB(23, 46, 82);
constexpr COLORREF kNavText = RGB(166, 181, 203);
constexpr COLORREF kNavTextActive = RGB(248, 250, 252);
constexpr COLORREF kNavIcon = RGB(126, 147, 178);
constexpr COLORREF kNavIconActive = RGB(147, 197, 253);
constexpr COLORREF kFooterCard = RGB(16, 31, 54);
constexpr COLORREF kFooterBorder = RGB(29, 46, 72);
constexpr COLORREF kSidebarBorder = RGB(24, 39, 62);
constexpr COLORREF kRowSelected = RGB(234, 241, 254);
constexpr COLORREF kDisabledFill = RGB(233, 237, 243);
constexpr COLORREF kDisabledBorder = RGB(226, 231, 239);

// Page metadata (icon glyphs from Segoe MDL2 Assets)
struct PageInfo {
    const wchar_t* glyph;
    const wchar_t* title;
    const wchar_t* subtitle;
};

const PageInfo kPages[7] = {
    { L"\xE74D", L"垃圾清理",   L"扫描并清理系统与浏览器产生的临时文件" },
    { L"\xE945", L"开机启动项", L"管理随系统启动的程序与启动文件夹" },
    { L"\xE700", L"右键菜单",   L"浏览并整理文件与文件夹的右键菜单项" },
    { L"\xE8F1", L"注册表清理", L"扫描无效注册表残留，删除前自动备份" },
    { L"\xE774", L"网络工具",   L"网络测速、Hosts 编辑与一键修复" },
    { L"\xE7B8", L"强力卸载",   L"卸载软件并清理卸载残留与用户数据" },
    { L"\xE90F", L"实用工具",   L"文件粉碎、进程管理与磁盘清理" },
};

// ====================== State ======================

int g_dpi = 96;
bool g_gdiplusReady = false;
bool g_admin = false;
HWND g_hMain = NULL;
HWND g_hSidebar = NULL;
HWND g_hNavTip = NULL;
int g_navHover = -1;

struct FontSet {
    HFONT title = NULL;
    HFONT subtitle = NULL;
    HFONT brand = NULL;
    HFONT brandSub = NULL;
    HFONT nav = NULL;
    HFONT body = NULL;
    HFONT strong = NULL;
    HFONT caption = NULL;
    HFONT button = NULL;
    HFONT iconLarge = NULL;
    HFONT iconEmpty = NULL;
    HFONT iconNav = NULL;
    HFONT iconSmall = NULL;
    HFONT mono = NULL;
    HFONT speed = NULL;
    HFONT section = NULL;
};

FontSet g_f;

// ====================== Small helpers ======================

inline Color GpColor(COLORREF c, BYTE alpha = 255) {
    return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

int ClampInt(int value, int lo, int hi) {
    return value < lo ? lo : (value > hi ? hi : value);
}

int QueryWindowDpi(HWND hWnd) {
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn fn = NULL;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) fn = (GetDpiForWindowFn)GetProcAddress(user32, "GetDpiForWindow");
    }
    UINT dpi = (fn && hWnd) ? fn(hWnd) : 0;
    if (dpi == 0) {
        HDC dc = GetDC(NULL);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(NULL, dc);
        }
    }
    return dpi ? (int)dpi : 96;
}

HFONT MakeFont(int tenthsOfPt, int weight, const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(tenthsOfPt, g_dpi, 720);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

void DestroyFontSet(FontSet& fonts) {
    HFONT* all[] = { &fonts.title, &fonts.subtitle, &fonts.brand, &fonts.brandSub,
                     &fonts.nav, &fonts.body, &fonts.strong, &fonts.caption, &fonts.button,
                     &fonts.iconLarge, &fonts.iconEmpty, &fonts.iconNav, &fonts.iconSmall, &fonts.mono,
                     &fonts.speed, &fonts.section };
    for (HFONT* f : all) {
        if (*f) { DeleteObject(*f); *f = NULL; }
    }
}

void CreateFontSet() {
    const wchar_t* ui = L"Segoe UI";
    g_f.title = MakeFont(185, FW_SEMIBOLD, ui);
    g_f.subtitle = MakeFont(90, FW_NORMAL, ui);
    g_f.brand = MakeFont(130, FW_SEMIBOLD, ui);
    g_f.brandSub = MakeFont(85, FW_NORMAL, ui);
    g_f.nav = MakeFont(100, FW_NORMAL, ui);
    g_f.body = MakeFont(95, FW_NORMAL, ui);
    g_f.strong = MakeFont(95, FW_SEMIBOLD, ui);
    g_f.caption = MakeFont(85, FW_NORMAL, ui);
    g_f.button = MakeFont(90, FW_SEMIBOLD, ui);
    g_f.iconLarge = MakeFont(150, FW_NORMAL, L"Segoe MDL2 Assets");
    g_f.iconEmpty = MakeFont(320, FW_NORMAL, L"Segoe MDL2 Assets");
    g_f.iconNav = MakeFont(120, FW_NORMAL, L"Segoe MDL2 Assets");
    g_f.iconSmall = MakeFont(90, FW_NORMAL, L"Segoe MDL2 Assets");
    g_f.mono = MakeFont(95, FW_NORMAL, L"Consolas");
    g_f.speed = MakeFont(115, FW_SEMIBOLD, ui);
    g_f.section = MakeFont(100, FW_SEMIBOLD, ui);
}

bool EnsureGdiplus() {
    if (g_gdiplusReady) return true;
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, NULL) == Ok) {
        g_gdiplusReady = true;
    }
    return g_gdiplusReady;
}

void BuildRoundPath(GraphicsPath& path, const RectF& r, float radius) {
    float d = radius * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void FillRound(Graphics& g, const RectF& r, float radius, const Color& color) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath path;
    BuildRoundPath(path, r, radius);
    SolidBrush brush(color);
    g.FillPath(&brush, &path);
}

void StrokeRound(Graphics& g, const RectF& r, float radius, const Color& color, float width = 1.0f) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath path;
    BuildRoundPath(path, r, radius);
    Pen pen(color, width);
    g.DrawPath(&pen, &path);
}

void DrawCardShadow(Graphics& g, const RectF& r, float radius) {
    for (int i = 3; i >= 1; --i) {
        RectF shadow(r.X - (float)i, r.Y - (float)i + 2.0f,
                     r.Width + i * 2.0f, r.Height + i * 2.0f);
        FillRound(g, shadow, radius + i, Color(5, 15, 23, 42));
    }
}

void DrawTextGdi(HDC dc, const std::wstring& text, RECT rc, HFONT font,
                 COLORREF color, UINT format) {
    if (!font) return;
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    int oldMode = SetBkMode(dc, TRANSPARENT);
    COLORREF oldColor = SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, format | DT_NOPREFIX);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    if (!font || text.empty()) return 0;
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &size);
    SelectObject(dc, oldFont);
    return size.cx;
}

typedef std::function<void(HDC, Graphics&)> ShapePainter;
typedef std::function<void(HDC)> TextPainter;

void BufferedPaint(HDC target, const RECT& rc, const ShapePainter& shapes, const TextPainter& text = nullptr) {
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC mem = CreateCompatibleDC(target);
    if (!mem) return;
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (!bitmap) { DeleteDC(mem); return; }
    HGDIOBJ oldBitmap = SelectObject(mem, bitmap);

    bool shapesDone = false;
    if (EnsureGdiplus()) {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetCompositingQuality(CompositingQualityHighQuality);
        shapes(mem, g);
        g.Flush();
        shapesDone = true;
    }
    if (!shapesDone) {
        HBRUSH brush = CreateSolidBrush(kBg);
        FillRect(mem, &rc, brush);
        DeleteObject(brush);
    }
    if (text) text(mem);

    BitBlt(target, rc.left, rc.top, width, height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

// ====================== Sidebar ======================

const wchar_t* kSidebarClass = L"SC_Sidebar";
bool g_sidebarClassRegistered = false;

bool SidebarExpanded() {
    if (!g_hSidebar) return true;
    RECT rc;
    GetClientRect(g_hSidebar, &rc);
    return (rc.right - rc.left) >= UiScale(140);
}

int NavTop() {
    return UiScale(112);
}

int NavItemHeight() {
    return UiScale(42);
}

int NavItemGap() {
    return UiScale(6);
}

RECT NavItemRect(int index) {
    RECT rc = {};
    if (!g_hSidebar) return rc;
    GetClientRect(g_hSidebar, &rc);
    int expanded = SidebarExpanded();
    int padX = UiScale(expanded ? 12 : 10);
    int h = NavItemHeight();
    int top = NavTop() + index * (h + NavItemGap());
    RECT item = { padX, top, rc.right - padX, top + h };
    return item;
}

int NavHitTest(POINT pt) {
    for (int i = 0; i < 7; ++i) {
        RECT r = NavItemRect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

int CurrentPage() {
    if (!g_hTab) return 0;
    int page = TabCtrl_GetCurSel(g_hTab);
    return (page >= 0 && page < 7) ? page : 0;
}

void CreateNavTooltip(HWND hMain) {
    if (g_hNavTip) return;
    g_hNavTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hMain, NULL, g_hInst, NULL);
    if (!g_hNavTip) return;
    SetWindowPos(g_hNavTip, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    for (int i = 0; i < 7; ++i) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_hSidebar;
        ti.uId = (UINT_PTR)(i + 1);
        ti.lpszText = (LPWSTR)kPages[i].title;
        ti.rect = NavItemRect(i);
        SendMessageW(g_hNavTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

void UpdateNavTooltips() {
    if (!g_hNavTip || !g_hSidebar) return;
    bool expanded = SidebarExpanded();
    RECT empty = { 0, 0, 0, 0 };
    for (int i = 0; i < 7; ++i) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = g_hSidebar;
        ti.uId = (UINT_PTR)(i + 1);
        ti.rect = expanded ? empty : NavItemRect(i);
        SendMessageW(g_hNavTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

void RequestPage(int index) {
    if (!g_hMain || index < 0 || index >= 7) return;
    PostMessageW(g_hMain, WM_APP_UI_NAV, (WPARAM)index, 0);
}

void PaintSidebar(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    bool expanded = SidebarExpanded();
    int page = CurrentPage();
    int hover = g_navHover;
    bool focused = GetFocus() == hWnd;
    bool admin = g_admin;

    BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
        // Background gradient
        LinearGradientBrush bg(Point(0, 0), Point(0, rc.bottom),
            GpColor(kSidebarTop), GpColor(kSidebarBottom));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

        // Subtle accent glow at the top
        SolidBrush softGlow(Color(9, 59, 130, 246));
        g.FillEllipse(&softGlow, (float)-UiScale(60), (float)-UiScale(90),
            (float)UiScale(260), (float)UiScale(220));

        int logoSize = UiScale(36);
        int logoX = expanded ? UiScale(20) : (rc.right - logoSize) / 2;
        int logoY = UiScale(24);
        RectF logoRect((float)logoX, (float)logoY, (float)logoSize, (float)logoSize);
        LinearGradientBrush logoBrush(Point(logoX, logoY),
            Point(logoX + logoSize, logoY + logoSize),
            GpColor(RGB(59, 130, 246)), GpColor(RGB(29, 78, 216)));
        GraphicsPath logoPath;
        BuildRoundPath(logoPath, logoRect, (float)UiScale(11));
        g.FillPath(&logoBrush, &logoPath);

        // Divider under the brand block
        SolidBrush divider(GpColor(kSidebarBorder));
        g.FillRectangle(&divider, (float)UiScale(16), (float)UiScale(92),
            (float)(rc.right - UiScale(32)), 1.0f);

        // Nav items
        for (int i = 0; i < 7; ++i) {
            RECT item = NavItemRect(i);
            bool selected = (i == page);
            bool hot = (i == hover);
            RectF rr((float)item.left, (float)item.top,
                (float)(item.right - item.left), (float)(item.bottom - item.top));

            if (selected) {
                FillRound(g, rr, (float)UiScale(10), GpColor(kNavActive));
            } else if (hot) {
                FillRound(g, rr, (float)UiScale(10), GpColor(kNavHover));
            }

            if (selected) {
                float barW = (float)UiScale(3);
                float barH = (float)UiScale(18);
                RectF bar((float)UiScale(4), rr.Y + (rr.Height - barH) / 2.0f, barW, barH);
                FillRound(g, bar, barW / 2.0f, GpColor(RGB(96, 165, 250)));
            }

            if (focused && selected) {
                StrokeRound(g, RectF(rr.X + 0.5f, rr.Y + 0.5f, rr.Width - 1.0f, rr.Height - 1.0f),
                    (float)UiScale(10), Color(70, 147, 197, 253), 1.0f);
            }
        }

        // Footer card
        if (expanded) {
            int cardH = UiScale(64);
            int cardLeft = UiScale(12);
            int cardTop = rc.bottom - UiScale(34) - cardH;
            RectF card((float)cardLeft, (float)cardTop,
                (float)(rc.right - cardLeft * 2), (float)cardH);
            FillRound(g, card, (float)UiScale(12), GpColor(kFooterCard));
            StrokeRound(g, RectF(card.X + 0.5f, card.Y + 0.5f, card.Width - 1.0f, card.Height - 1.0f),
                (float)UiScale(12), GpColor(admin ? kFooterBorder : RGB(58, 48, 30)));

            int iconSize = UiScale(28);
            RectF iconRect(card.X + UiScale(12), card.Y + (cardH - iconSize) / 2.0f,
                (float)iconSize, (float)iconSize);
            FillRound(g, iconRect, (float)UiScale(8),
                admin ? Color(36, 22, 163, 74) : Color(36, 245, 158, 11));
            // Status dot
            SolidBrush dot(admin ? GpColor(RGB(74, 222, 128)) : GpColor(RGB(251, 191, 36)));
            g.FillEllipse(&dot, card.X + card.Width - (float)UiScale(18),
                card.Y + (float)UiScale(14), (float)UiScale(6), (float)UiScale(6));
        } else {
            int iconSize = UiScale(28);
            RectF iconRect((rc.right - iconSize) / 2.0f, (float)(rc.bottom - UiScale(70)),
                (float)iconSize, (float)iconSize);
            FillRound(g, iconRect, (float)UiScale(8),
                admin ? Color(36, 22, 163, 74) : Color(36, 245, 158, 11));
        }
    }, [&](HDC dc) {
        // Brand text
        if (expanded) {
            int textX = UiScale(66);
            RECT brandRect = { textX, UiScale(25), rc.right - UiScale(12), UiScale(46) };
            DrawTextGdi(dc, L"SystemCleaner", brandRect, g_f.brand, kWhite,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT subRect = { textX, UiScale(45), rc.right - UiScale(12), UiScale(64) };
            DrawTextGdi(dc, L"系统清理工具箱", subRect, g_f.brandSub, RGB(124, 141, 166),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // Logo glyph
        int logoSize = UiScale(36);
        int logoX = expanded ? UiScale(20) : (rc.right - logoSize) / 2;
        int logoY = UiScale(24);
        RECT logoGlyphRect = { logoX, logoY, logoX + logoSize, logoY + logoSize };
        DrawTextGdi(dc, L"\xE74D", logoGlyphRect, g_f.iconNav, kWhite,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Nav items
        for (int i = 0; i < 7; ++i) {
            RECT item = NavItemRect(i);
            bool selected = (i == page);
            COLORREF iconColor = selected ? kNavIconActive : kNavIcon;
            COLORREF textColor = selected ? kNavTextActive : kNavText;

            if (expanded) {
                RECT iconRect = { item.left + UiScale(14), item.top,
                                  item.left + UiScale(36), item.bottom };
                DrawTextGdi(dc, kPages[i].glyph, iconRect, g_f.iconNav, iconColor,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                RECT textRect = { item.left + UiScale(43), item.top,
                                  item.right - UiScale(8), item.bottom };
                DrawTextGdi(dc, kPages[i].title, textRect, g_f.nav, textColor,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            } else {
                DrawTextGdi(dc, kPages[i].glyph, item, g_f.iconNav, iconColor,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        // Footer text
        if (expanded) {
            int cardH = UiScale(64);
            int cardLeft = UiScale(12);
            int cardTop = rc.bottom - UiScale(34) - cardH;
            int iconSize = UiScale(28);
            int iconX = cardLeft + UiScale(12);
            int iconY = cardTop + (cardH - iconSize) / 2;

            RECT lockRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
            DrawTextGdi(dc, admin ? L"\xE72E" : L"\xE7BA", lockRect, g_f.iconSmall,
                admin ? RGB(134, 239, 172) : RGB(253, 230, 138),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT titleRect = { cardLeft + UiScale(50), cardTop + UiScale(10),
                               rc.right - UiScale(26), cardTop + UiScale(34) };
            DrawTextGdi(dc, admin ? L"管理员模式" : L"标准权限", titleRect, g_f.strong,
                RGB(231, 238, 249), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT descRect = { cardLeft + UiScale(50), cardTop + UiScale(31),
                              rc.right - UiScale(26), cardTop + UiScale(54) };
            DrawTextGdi(dc, admin ? L"权限已授予" : L"部分功能受限", descRect, g_f.brandSub,
                RGB(127, 146, 172), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT verRect = { 0, rc.bottom - UiScale(28), rc.right, rc.bottom - UiScale(10) };
            DrawTextGdi(dc, L"SystemCleaner 1.0", verRect, g_f.brandSub,
                RGB(84, 102, 126), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int iconSize = UiScale(28);
            int iconX = (rc.right - iconSize) / 2;
            int iconY = rc.bottom - UiScale(70);
            RECT iconRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
            DrawTextGdi(dc, admin ? L"\xE72E" : L"\xE7BA", iconRect, g_f.iconSmall,
                admin ? RGB(134, 239, 172) : RGB(253, 230, 138),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    });
}

LRESULT CALLBACK SidebarProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintSidebar(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hover = NavHitTest(pt);
        if (hover != g_navHover) {
            g_navHover = hover;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_navHover != -1) {
            g_navHover = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        return 0;
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = NavHitTest(pt);
        if (index >= 0) RequestPage(index);
        return 0;
    }
    case WM_KEYDOWN: {
        int page = CurrentPage();
        if (wParam == VK_UP) {
            RequestPage((page + 6) % 7);
            return 0;
        }
        if (wParam == VK_DOWN) {
            RequestPage((page + 1) % 7);
            return 0;
        }
        if (wParam == VK_HOME) { RequestPage(0); return 0; }
        if (wParam == VK_END) { RequestPage(6); return 0; }
        break;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

void CreateSidebar(HWND hMain) {
    if (g_hSidebar) return;
    if (!g_sidebarClassRegistered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SidebarProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = kSidebarClass;
        if (RegisterClassExW(&wc)) g_sidebarClassRegistered = true;
    }
    g_hSidebar = CreateWindowExW(0, kSidebarClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0, 0, 10, 10, hMain, NULL, g_hInst, NULL);
    CreateNavTooltip(hMain);
}

// ====================== Owner-drawn buttons ======================

const wchar_t* kHoverProp = L"SC_BtnHover";

enum class BtnRole { Primary, Success, Danger, Secondary, Ghost };

BtnRole RoleForButton(HWND hButton) {
    int id = GetDlgCtrlID(hButton);

    wchar_t text[64] = { 0 };
    GetWindowTextW(hButton, text, 64);

    switch (id) {
    case 1003: case 5001: case 4001: case 4004: case 7002:
        return BtnRole::Primary;
    case 1004: case 5002:
        return BtnRole::Success;
    case 2002: case 3002: case 7004: case 6002:
        return BtnRole::Danger;
    case 4002: case 4003: case 4005:
        return BtnRole::Secondary;
    case 6013:
        if (wcsstr(text, L"扫描") || wcsstr(text, L"刷新")) return BtnRole::Primary;
        return BtnRole::Secondary;
    case 6014:
        if (wcsstr(text, L"删除") || wcsstr(text, L"结束")) return BtnRole::Danger;
        if (wcsstr(text, L"修复")) return BtnRole::Success;
        return BtnRole::Secondary;
    default:
        return BtnRole::Secondary;
    }
}

bool ButtonHovered(HWND hButton) {
    return GetPropW(hButton, kHoverProp) != NULL;
}

LRESULT CALLBACK ButtonSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (!ButtonHovered(hWnd)) {
            SetPropW(hWnd, kHoverProp, (HANDLE)1);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (ButtonHovered(hWnd)) {
            RemovePropW(hWnd, kHoverProp);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemovePropW(hWnd, kHoverProp);
        RemoveWindowSubclass(hWnd, ButtonSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}

} // namespace

// ---- Public API implementations ----

bool UiDrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON) return false;

    HWND hButton = dis->hwndItem;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = GetFocus() == hButton;
    bool hovered = ButtonHovered(hButton) && !disabled;
    BtnRole role = RoleForButton(hButton);

    wchar_t label[128] = { 0 };
    GetWindowTextW(hButton, label, 128);

    COLORREF fill = kWhite;
    COLORREF border = kBorderStrong;
    COLORREF text = RGB(52, 64, 84);

    if (disabled) {
        fill = kDisabledFill;
        border = kDisabledBorder;
        text = kTextTertiary;
        hovered = false;
    } else {
        switch (role) {
        case BtnRole::Primary:
            fill = pressed ? kAccentPressed : (hovered ? kAccentHover : kAccent);
            border = fill;
            text = kWhite;
            break;
        case BtnRole::Success:
            fill = pressed ? kSuccessPressed : (hovered ? kSuccessHover : kSuccess);
            border = fill;
            text = kWhite;
            break;
        case BtnRole::Danger:
            fill = pressed ? RGB(254, 228, 226) : (hovered ? kDangerSoft : kWhite);
            border = pressed ? RGB(249, 168, 164) : kDangerBorder;
            text = RGB(180, 35, 24);
            break;
        case BtnRole::Secondary:
            fill = pressed ? RGB(242, 244, 247) : (hovered ? RGB(248, 250, 252) : kWhite);
            border = kBorderStrong;
            text = RGB(52, 64, 84);
            break;
        case BtnRole::Ghost:
            fill = pressed ? RGB(226, 231, 239) : (hovered ? RGB(233, 237, 244) : kBg);
            border = fill;
            text = RGB(71, 84, 103);
            break;
        }
    }

    float radius = (float)UiScale(8);
    int bw = rc.right - rc.left;
    int bh = rc.bottom - rc.top;

    BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
        // Toolbar buttons sit on the page background
        SolidBrush bg(GpColor(kBg));
        g.FillRectangle(&bg, 0, 0, bw, bh);

        RectF r(0.5f, 0.5f, (float)bw - 1.0f, (float)bh - 1.0f);
        FillRound(g, r, radius, GpColor(fill));
        if (role != BtnRole::Primary && role != BtnRole::Success && role != BtnRole::Ghost) {
            StrokeRound(g, r, radius, GpColor(border), 1.0f);
        } else if (role == BtnRole::Ghost && hovered) {
            StrokeRound(g, r, radius, Color(0, 0, 0, 0), 1.0f);
        }

        if (focused && !disabled) {
            COLORREF ring = (role == BtnRole::Primary || role == BtnRole::Success)
                ? RGB(255, 255, 255) : kAccent;
            BYTE alpha = (role == BtnRole::Primary || role == BtnRole::Success) ? 150 : 255;
            StrokeRound(g, RectF(r.X + 2.0f, r.Y + 2.0f, r.Width - 4.0f, r.Height - 4.0f),
                radius - 2.0f, GpColor(ring, alpha), 1.0f);
        }
    }, [&](HDC dc) {
        RECT textRect = { 0, 0, bw, bh };
        DrawTextGdi(dc, label, textRect, g_f.button, text,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    });

    return true;
}

void StyleButton(HWND hButton) {
    if (!hButton) return;
    LONG_PTR style = GetWindowLongPtrW(hButton, GWL_STYLE);
    LONG_PTR type = style & BS_TYPEMASK;
    if (type == BS_PUSHBUTTON) {
        style = (style & ~(LONG_PTR)BS_TYPEMASK) | BS_OWNERDRAW;
        SetWindowLongPtrW(hButton, GWL_STYLE, style);
        SetWindowPos(hButton, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    SendMessageW(hButton, WM_SETFONT, (WPARAM)g_f.button, FALSE);
    SetWindowSubclass(hButton, ButtonSubclassProc, 1, 0);
}

// ====================== Edit chrome ======================

bool g_editSubclassed = false;

void UpdateEditRegion(HWND hWnd) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    bool hosts = (hWnd == g_hHostsEdit);
    int radius = UiScale(hosts ? 10 : 8);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2);
    if (rgn) SetWindowRgn(hWnd, rgn, TRUE);
}

// 单行 EDIT 默认把文字画在顶部，34px 高的输入框会明显贴顶。
// 这三类输入框改成无自动换行的多行 EDIT，再用 EM_SETRECTNP 把首行垂直居中。
bool IsCenteredEdit(HWND hWnd) {
    return hWnd && hWnd != g_hHostsEdit &&
        (hWnd == g_hShredPath || hWnd == g_hToolPathEdit || hWnd == g_hUninstallSearch);
}

void UpdateEditTextRect(HWND hWnd) {
    if (!IsCenteredEdit(hWnd)) return;
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.bottom <= 0) return;

    HFONT font = (HFONT)SendMessageW(hWnd, WM_GETFONT, 0, 0);
    HDC dc = GetDC(hWnd);
    TEXTMETRICW tm = {};
    if (dc) {
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : NULL;
        GetTextMetricsW(dc, &tm);
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(hWnd, dc);
    }
    int lineH = tm.tmHeight + tm.tmExternalLeading;
    if (lineH <= 0) lineH = UiScale(18);
    int top = (rc.bottom - lineH) / 2;
    if (top < 0) top = 0;
    RECT fmt = { 0, top, rc.right, top + lineH };
    SendMessageW(hWnd, EM_SETRECTNP, 0, (LPARAM)&fmt);
}

const wchar_t* EditPlaceholder(HWND hWnd) {
    if (hWnd == g_hShredPath) return L"选择要粉碎的文件";
    if (hWnd == g_hToolPathEdit) return L"输入或选择要处理的文件夹";
    if (hWnd == g_hUninstallSearch) return L"搜索软件名称";
    return nullptr;
}

// 原生 EDIT 会把整个矩形客户区涂白，只描圆角边框仍会在四角留下白色方块。
// 这里先把圆角外侧遮成页面底色，再描边并绘制搜索图标/占位提示。
void PaintEditChrome(HWND hWnd, HDC dc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    if (!EnsureGdiplus()) return;

    bool focused = GetFocus() == hWnd;
    COLORREF border = focused ? kAccent : kBorder;
    float radius = (float)UiScale(hWnd == g_hHostsEdit ? 10 : 8);
    RectF r(0.5f, 0.5f, (float)rc.right - 1.0f, (float)rc.bottom - 1.0f);

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    GraphicsPath outside;
    outside.AddRectangle(RectF(0.0f, 0.0f, (float)rc.right, (float)rc.bottom));
    GraphicsPath pill;
    BuildRoundPath(pill, r, radius);
    outside.AddPath(&pill, FALSE);
    outside.SetFillMode(FillModeAlternate);
    SolidBrush page(GpColor(kBg));
    g.FillPath(&page, &outside);

    StrokeRound(g, r, radius, GpColor(border), focused ? 1.5f : 1.0f);
    g.Flush();

    if (GetDlgCtrlID(hWnd) == 7008) {
        RECT iconRect = { UiScale(9), 0, UiScale(27), rc.bottom };
        DrawTextGdi(dc, L"\xE721", iconRect, g_f.iconSmall, kTextTertiary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const wchar_t* hint = EditPlaceholder(hWnd);
    if (hint && GetWindowTextLengthW(hWnd) == 0 && !focused) {
        int leftMargin = (GetDlgCtrlID(hWnd) == 7008) ? UiScale(32) : UiScale(12);
        RECT hintRect = { leftMargin, 0, rc.right - UiScale(10), rc.bottom };
        HFONT font = (HFONT)SendMessageW(hWnd, WM_GETFONT, 0, 0);
        DrawTextGdi(dc, hint, hintRect, font ? font : g_f.body, kTextTertiary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// 打印/截图时原生 EDIT 会把绘制裁剪区留在传入的 DC 上，必须先清除再画 chrome。
// 画完不能恢复原裁剪区：恢复时原生 EDIT 会把圆角外侧重新刷白，四角又露出白方块。
void PaintEditChromeUnclipped(HWND hWnd, HDC dc) {
    SelectClipRgn(dc, NULL);
    PaintEditChrome(hWnd, dc);
}

LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (message) {
    case WM_CHAR:
        // 多行 EDIT 不能吃掉回车，否则单行输入框会被换行
        if (IsCenteredEdit(hWnd) && (wParam == L'\r' || wParam == L'\n')) return 0;
        break;
    case WM_SIZE: {
        UpdateEditRegion(hWnd);
        // 必须先让原生 EDIT 处理 WM_SIZE：它会在内部重算格式矩形，
        // 先设置的 EM_SETRECTNP 会被它覆盖掉。
        LRESULT result = DefSubclassProc(hWnd, message, wParam, lParam);
        UpdateEditTextRect(hWnd);
        if (hWnd == g_hHostsEdit) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            rc.left += UiScale(12);
            rc.right -= UiScale(14);
            rc.top += UiScale(10);
            rc.bottom -= UiScale(10);
            SendMessageW(hWnd, EM_SETRECTNP, 0, (LPARAM)&rc);
        }
        return result;
    }
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(hWnd, message, wParam, lParam);
        HDC dc = GetDC(hWnd);
        if (dc) {
            PaintEditChrome(hWnd, dc);
            ReleaseDC(hWnd, dc);
        }
        return result;
    }
    case WM_PRINTCLIENT: {
        LRESULT result = DefSubclassProc(hWnd, message, wParam, lParam);
        PaintEditChromeUnclipped(hWnd, (HDC)wParam);
        return result;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, EditSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}

void StyleEdit(HWND hEdit) {
    if (!hEdit) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hEdit, GWL_EXSTYLE);
    if (exStyle & WS_EX_CLIENTEDGE) {
        SetWindowLongPtrW(hEdit, GWL_EXSTYLE, exStyle & ~(LONG_PTR)WS_EX_CLIENTEDGE);
    }
    LONG_PTR style = GetWindowLongPtrW(hEdit, GWL_STYLE);
    if (style & WS_BORDER) style &= ~(LONG_PTR)WS_BORDER;
    SetWindowLongPtrW(hEdit, GWL_STYLE, style);
    SetWindowPos(hEdit, NULL, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    SendMessageW(hEdit, WM_SETFONT,
        (WPARAM)(hEdit == g_hHostsEdit ? g_f.mono : g_f.body), FALSE);

    int leftMargin = (GetDlgCtrlID(hEdit) == 7008) ? UiScale(32) : UiScale(12);
    if (hEdit == g_hHostsEdit) leftMargin = UiScale(12);
    SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELONG(leftMargin, UiScale(10)));

    if (!GetWindowLongPtrW(hEdit, GWLP_USERDATA)) {
        SetWindowLongPtrW(hEdit, GWLP_USERDATA, 1);
        SetWindowSubclass(hEdit, EditSubclassProc, 1, 0);
    }
    UpdateEditRegion(hEdit);
    UpdateEditTextRect(hEdit);
}

// ====================== Progress bar ======================

LRESULT CALLBACK ProgressSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        PBRANGE range = {};
        SendMessageW(hWnd, PBM_GETRANGE, FALSE, (LPARAM)&range);
        int pos = (int)SendMessageW(hWnd, PBM_GETPOS, 0, 0);
        int span = range.iHigh - range.iLow;
        float ratio = 0.0f;
        if (span > 0) {
            ratio = (float)(pos - range.iLow) / (float)span;
            ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
        }

        BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
            SolidBrush bg(GpColor(kBg));
            g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

            float h = (float)(rc.bottom - rc.top);
            float radius = h / 2.0f;
            FillRound(g, RectF(0, 0, (float)rc.right, h), radius, GpColor(RGB(226, 232, 240)));

            float fillW = rc.right * ratio;
            if (fillW > 0.5f) {
                LinearGradientBrush fill(Point(0, 0), Point(rc.right, 0),
                    GpColor(RGB(59, 130, 246)), GpColor(kAccent));
                GraphicsPath path;
                BuildRoundPath(path, RectF(0, 0, fillW, h), radius);
                g.FillPath(&fill, &path);
            }
        });

        EndPaint(hWnd, &ps);
        return 0;
    }
    case PBM_SETPOS:
    case PBM_SETRANGE:
    case PBM_SETRANGE32:
    case PBM_DELTAPOS:
    case PBM_SETMARQUEE: {
        LRESULT result = DefSubclassProc(hWnd, message, wParam, lParam);
        InvalidateRect(hWnd, NULL, FALSE);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, ProgressSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}

void StyleProgress(HWND hProgress) {
    if (!hProgress) return;
    SetWindowTheme(hProgress, L"", L"");
    SetWindowSubclass(hProgress, ProgressSubclassProc, 1, 0);
}

// ====================== List views ======================

bool IsProjectList(HWND hList) {
    return hList && (hList == g_hCleanList || hList == g_hStartupList ||
        hList == g_hContextMenuList || hList == g_hRegCleanList ||
        hList == g_hUninstallList || hList == g_hToolListView);
}

HIMAGELIST g_rowHeightImages = NULL;
int g_rowHeightPixels = 0;

void ApplyRowHeightImages() {
    int target = UiScale(30);
    if (g_rowHeightImages && g_rowHeightPixels == target) return;
    HIMAGELIST next = ImageList_Create(1, target, ILC_COLOR32, 1, 1);
    if (!next) return;
    HWND lists[] = { g_hCleanList, g_hStartupList, g_hContextMenuList,
                     g_hRegCleanList, g_hUninstallList, g_hToolListView };
    for (HWND h : lists) {
        if (h) ListView_SetImageList(h, next, LVSIL_SMALL);
    }
    if (g_rowHeightImages) ImageList_Destroy(g_rowHeightImages);
    g_rowHeightImages = next;
    g_rowHeightPixels = target;
}

void StyleListView(HWND hList) {
    if (!hList) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hList, GWL_EXSTYLE);
    if (exStyle & WS_EX_CLIENTEDGE) {
        SetWindowLongPtrW(hList, GWL_EXSTYLE, exStyle & ~(LONG_PTR)WS_EX_CLIENTEDGE);
        SetWindowPos(hList, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    DWORD ex = (DWORD)ListView_GetExtendedListViewStyle(hList);
    ex &= ~LVS_EX_GRIDLINES;
    ex |= LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP;
    ListView_SetExtendedListViewStyle(hList, ex);

    SetWindowTheme(hList, L"Explorer", NULL);
    ListView_SetBkColor(hList, kWhite);
    ListView_SetTextBkColor(hList, kWhite);
    ListView_SetTextColor(hList, kTextPrimary);
    SendMessageW(hList, WM_SETFONT, (WPARAM)g_f.body, FALSE);
    ApplyRowHeightImages();

    HWND header = ListView_GetHeader(hList);
    if (header) {
        SendMessageW(header, WM_SETFONT, (WPARAM)g_f.strong, FALSE);
        SetWindowTheme(header, L"Explorer", NULL);
    }
}

struct ListEmptyState {
    const wchar_t* glyph;
    const wchar_t* title;
    const wchar_t* hint;
};

bool GetListEmptyState(HWND hList, ListEmptyState& info) {
    if (hList == g_hStartupList) {
        info = { L"\xE945", L"没有找到开机启动项", L"点击「刷新扫描」重新检测" };
        return true;
    }
    if (hList == g_hContextMenuList) {
        info = { L"\xE700", L"没有找到右键菜单项", L"点击「刷新扫描」重新检测" };
        return true;
    }
    if (hList == g_hRegCleanList) {
        info = { L"\xE8F1", L"尚未扫描注册表", L"点击「扫描垃圾」查找无效的注册表残留" };
        return true;
    }
    if (hList == g_hUninstallList) {
        info = { L"\xE774", L"尚未加载软件列表", L"点击「刷新扫描」加载已安装软件与驱动" };
        return true;
    }
    if (hList == g_hToolListView) {
        switch (g_toolFunction) {
        case 2:
            info = { L"\xE8B7", L"还没有扫描结果", L"选择文件夹后点击「开始扫描」查找重复文件" };
            return true;
        case 3:
            info = { L"\xE9D9", L"正在读取进程列表", L"点击「刷新列表」重新加载" };
            return true;
        case 4:
            info = { L"\xE71B", L"还没有扫描结果", L"选择文件夹后点击「扫描失效」检查快捷方式" };
            return true;
        case 5:
            info = { L"\xE8B7", L"还没有扫描结果", L"选择文件夹与文件年龄后点击「开始扫描」" };
            return true;
        default:
            return false;
        }
    }
    return false;
}

void DrawListEmptyState(HWND hList, HDC dc) {
    ListEmptyState info = {};
    if (!GetListEmptyState(hList, info) || !EnsureGdiplus()) return;

    RECT rc;
    GetClientRect(hList, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    int headerH = 0;
    HWND header = ListView_GetHeader(hList);
    if (header) {
        RECT hr;
        GetWindowRect(header, &hr);
        headerH = hr.bottom - hr.top;
    }

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush white(GpColor(kWhite));
    g.FillRectangle(&white, 0, 0, width, height);

    int chip = UiScale(64);
    int cx = width / 2;
    int cy = headerH + (height - headerH) / 2 - UiScale(12);
    RectF chipRect((float)(cx - chip / 2), (float)(cy - chip / 2),
                   (float)chip, (float)chip);
    FillRound(g, chipRect, (float)UiScale(18), GpColor(RGB(238, 243, 251)));
    g.Flush();

    RECT glyphRect = { cx - chip / 2, cy - chip / 2, cx + chip / 2, cy + chip / 2 };
    DrawTextGdi(dc, info.glyph, glyphRect, g_f.iconEmpty, RGB(148, 172, 208),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int textTop = cy + chip / 2;
    RECT titleRect = { 0, textTop + UiScale(14), width, textTop + UiScale(36) };
    DrawTextGdi(dc, info.title, titleRect, g_f.section, kTextSecondary,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT hintRect = { 0, textTop + UiScale(36), width, textTop + UiScale(58) };
    DrawTextGdi(dc, info.hint, hintRect, g_f.caption, kTextTertiary,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

bool UiHandleNotify(LPARAM lParam, LRESULT* result) {
    if (!result) return false;
    LPNMHDR hdr = (LPNMHDR)lParam;
    if (!hdr) return false;

    if (hdr->code == NM_CUSTOMDRAW && IsProjectList(hdr->hwndFrom)) {
        LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)lParam;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            if (ListView_GetItemCount(hdr->hwndFrom) == 0) {
                DrawListEmptyState(hdr->hwndFrom, cd->nmcd.hdc);
                *result = CDRF_SKIPDEFAULT;
                return true;
            }
            *result = CDRF_NOTIFYITEMDRAW;
            return true;
        case CDDS_ITEMPREPAINT: {
            int item = (int)cd->nmcd.dwItemSpec;
            bool selected = (ListView_GetItemState(hdr->hwndFrom, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            cd->clrText = kTextPrimary;
            cd->clrTextBk = selected ? kRowSelected : kWhite;
            *result = CDRF_NOTIFYSUBITEMDRAW;
            return true;
        }
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            int item = (int)cd->nmcd.dwItemSpec;
            bool selected = (ListView_GetItemState(hdr->hwndFrom, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            cd->clrText = kTextPrimary;
            cd->clrTextBk = selected ? kRowSelected : kWhite;
            *result = CDRF_DODEFAULT;
            return true;
        }
        default:
            break;
        }
    }
    return false;
}

// ====================== Layout ======================

void SetColumns(HWND hList, int count, const int* widths) {
    if (!hList) return;
    HWND header = ListView_GetHeader(hList);
    int available = header ? Header_GetItemCount(header) : 0;
    int limit = count < available ? count : available;
    for (int i = 0; i < limit; ++i) {
        ListView_SetColumnWidth(hList, i, widths[i]);
    }
}

void LayoutListColumns(const RECT& page) {
    int listW = page.right - page.left - UiScale(UiMetrics::CardPadding) * 2
        - GetSystemMetrics(SM_CXVSCROLL);
    if (listW < UiScale(200)) listW = UiScale(200);

    int cw[5] = { 0 };
    if (g_hCleanList && ListView_GetColumnWidth(g_hCleanList, 0) > 0) {
        int name = ClampInt(listW * 19 / 100, UiScale(150), UiScale(260));
        int size = ClampInt(listW * 13 / 100, UiScale(96), UiScale(150));
        int count = UiScale(76);
        cw[0] = name;
        cw[1] = listW - name - size - count - UiScale(8);
        if (cw[1] < UiScale(160)) cw[1] = UiScale(160);
        cw[2] = size;
        cw[3] = count;
        SetColumns(g_hCleanList, 4, cw);
    }
    if (g_hStartupList && ListView_GetColumnWidth(g_hStartupList, 0) > 0) {
        int name = ClampInt(listW * 21 / 100, UiScale(150), UiScale(280));
        int loc = ClampInt(listW * 22 / 100, UiScale(150), UiScale(300));
        int status = UiScale(86);
        cw[0] = name;
        cw[1] = listW - name - loc - status - UiScale(8);
        if (cw[1] < UiScale(180)) cw[1] = UiScale(180);
        cw[2] = loc;
        cw[3] = status;
        SetColumns(g_hStartupList, 4, cw);
    }
    if (g_hContextMenuList && ListView_GetColumnWidth(g_hContextMenuList, 0) > 0) {
        int name = ClampInt(listW * 22 / 100, UiScale(160), UiScale(280));
        int type = UiScale(112);
        int status = UiScale(86);
        cw[0] = name;
        cw[1] = type;
        cw[2] = listW - name - type - status - UiScale(8);
        if (cw[2] < UiScale(180)) cw[2] = UiScale(180);
        cw[3] = status;
        SetColumns(g_hContextMenuList, 4, cw);
    }
    if (g_hRegCleanList && ListView_GetColumnWidth(g_hRegCleanList, 0) > 0) {
        int category = UiScale(112);
        cw[0] = ClampInt(listW * 45 / 100, UiScale(240), UiScale(560));
        cw[1] = category;
        cw[2] = listW - cw[0] - category - UiScale(8);
        if (cw[2] < UiScale(180)) cw[2] = UiScale(180);
        SetColumns(g_hRegCleanList, 3, cw);
    }
    if (g_hUninstallList && ListView_GetColumnWidth(g_hUninstallList, 0) > 0) {
        int name = ClampInt(listW * 23 / 100, UiScale(180), UiScale(300));
        int version = UiScale(90);
        int type = UiScale(84);
        int size = UiScale(84);
        cw[0] = name;
        cw[1] = version;
        cw[2] = type;
        cw[3] = size;
        cw[4] = listW - name - version - type - size - UiScale(8);
        if (cw[4] < UiScale(160)) cw[4] = UiScale(160);
        SetColumns(g_hUninstallList, 5, cw);
    }
    if (g_hToolListView) {
        UiStretchLastColumn(g_hToolListView);
    }
}

void UiStretchLastColumn(HWND hList) {
    if (!hList) return;
    HWND header = ListView_GetHeader(hList);
    if (!header) return;
    int count = Header_GetItemCount(header);
    if (count <= 0) return;
    RECT rc;
    GetClientRect(hList, &rc);
    int avail = rc.right - rc.left;
    int sum = 0;
    for (int i = 0; i < count; ++i) {
        sum += ListView_GetColumnWidth(hList, i);
    }
    if (avail > sum + UiScale(8)) {
        int last = ListView_GetColumnWidth(hList, count - 1);
        ListView_SetColumnWidth(hList, count - 1, last + (avail - sum));
    }
}

void UiPlaceListViewInCard(HWND hList, const RECT& pageRect, int extraBottom) {
    if (!hList) return;
    int pad = UiScale(UiMetrics::CardPadding);
    int top = pageRect.top + UiScale(UiMetrics::CardTop) + pad;
    int bottom = pageRect.bottom - UiScale(16) - pad - extraBottom;
    int height = bottom - top;
    if (height < UiScale(60)) height = UiScale(60);
    SetWindowPos(hList, NULL, pageRect.left + pad, top,
        pageRect.right - pageRect.left - pad * 2, height, SWP_NOZORDER);
}

void PlaceButton(int id, int x, int y, int w, int h) {
    HWND hwnd = GetDlgItem(g_hTab, id);
    if (hwnd) SetWindowPos(hwnd, NULL, x, y, w, h, SWP_NOZORDER);
}

void PlaceButtonsLeft(const int* ids, const int* widths, int count, int x, int y, int h, int gap) {
    for (int i = 0; i < count; ++i) {
        PlaceButton(ids[i], x, y, widths[i], h);
        x += widths[i] + gap;
    }
}

void LayoutPages(const RECT& page) {
    const int top = page.top;
    const int right = page.right;
    const int gap = UiScale(8);
    const int btnH = UiScale(UiMetrics::ButtonHeight);

    // ---------- 垃圾清理 ----------
    {
        int ids[] = { 1001, 1002, 1006 };
        int widths[] = { UiScale(64), UiScale(64), UiScale(78) };
        PlaceButtonsLeft(ids, widths, 3, page.left, top, btnH, gap);
    }
    {
        int rightIds[] = { 1005, 1003, 1004 };
        int rightWidths[] = { UiScale(76), UiScale(104), UiScale(104) };
        int x = right - rightWidths[0] - gap - rightWidths[1] - gap - rightWidths[2];
        for (int i = 0; i < 3; ++i) {
            PlaceButton(rightIds[i], x, top, rightWidths[i], btnH);
            x += rightWidths[i] + gap;
        }
    }
    {
        int pad = UiScale(UiMetrics::CardPadding);
        UiPlaceListViewInCard(g_hCleanList, page, UiScale(20));
        SetWindowPos(g_hCleanProgress, NULL, page.left + pad, page.bottom - UiScale(36),
            page.right - page.left - pad * 2, UiScale(6), SWP_NOZORDER);
    }

    // ---------- 开机启动项 ----------
    {
        int ids[] = { 2001, 2002, 2003 };
        int widths[] = { UiScale(96), UiScale(96), UiScale(96) };
        PlaceButtonsLeft(ids, widths, 3, page.left, top, btnH, gap);
    }
    {
        int comboW = UiScale(130);
        int labelW = UiScale(64);
        int labelX = right - comboW - gap - labelW;
        SetWindowPos(GetDlgItem(g_hTab, 2006), NULL, labelX, top + UiScale(8),
            labelW, UiScale(18), SWP_NOZORDER);
        UiCenterComboInRow(GetDlgItem(g_hTab, 2005), right - comboW, comboW, top, btnH, 200);
    }
    UiPlaceListViewInCard(g_hStartupList, page, 0);

    // ---------- 右键菜单 ----------
    {
        int ids[] = { 3001, 3002, 3003 };
        int widths[] = { UiScale(96), UiScale(96), UiScale(96) };
        PlaceButtonsLeft(ids, widths, 3, page.left, top, btnH, gap);
    }
    {
        int comboW = UiScale(130);
        int labelW = UiScale(64);
        int labelX = right - comboW - gap - labelW;
        SetWindowPos(GetDlgItem(g_hTab, 3006), NULL, labelX, top + UiScale(8),
            labelW, UiScale(18), SWP_NOZORDER);
        UiCenterComboInRow(GetDlgItem(g_hTab, 3005), right - comboW, comboW, top, btnH, 200);
    }
    UiPlaceListViewInCard(g_hContextMenuList, page, 0);

    // ---------- 注册表清理 ----------
    {
        int ids[] = { 5001, 5002 };
        int widths[] = { UiScale(104), UiScale(104) };
        PlaceButtonsLeft(ids, widths, 2, page.left, top, btnH, gap);
    }
    UiPlaceListViewInCard(g_hRegCleanList, page, 0);

    // ---------- 强力卸载 ----------
    {
        int ids[] = { 7001, 7002 };
        int widths[] = { UiScale(88), UiScale(88) };
        PlaceButtonsLeft(ids, widths, 2, page.left, top, btnH, gap);
        SetWindowPos(GetDlgItem(g_hTab, 7003), NULL, page.left + UiScale(204),
            top + UiScale(8), UiScale(120), UiScale(20), SWP_NOZORDER);
    }
    {
        int forceW = UiScale(96);
        int searchW = UiScale(168);
        int searchLabelW = UiScale(30);
        int comboW = UiScale(86);
        int typeLabelW = UiScale(30);

        int forceX = right - forceW;
        int searchX = forceX - UiScale(12) - searchW;
        int searchLabelX = searchX - UiScale(6) - searchLabelW;
        int comboX = searchLabelX - UiScale(8) - comboW;
        int typeLabelX = comboX - UiScale(6) - typeLabelW;

        SetWindowPos(GetDlgItem(g_hTab, 7010), NULL, typeLabelX, top + UiScale(8),
            typeLabelW, UiScale(18), SWP_NOZORDER);
        UiCenterComboInRow(g_hUninstallFilter, comboX, comboW, top, btnH, 200);
        SetWindowPos(GetDlgItem(g_hTab, 7007), NULL, searchLabelX, top + UiScale(8),
            searchLabelW, UiScale(18), SWP_NOZORDER);
        SetWindowPos(g_hUninstallSearch, NULL, searchX, top + UiScale(2),
            searchW, UiScale(30), SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 7004), NULL, forceX, top, forceW, btnH, SWP_NOZORDER);
    }
    UiPlaceListViewInCard(g_hUninstallList, page, 0);

    // ---------- 网络工具 ----------
    {
        int ids[] = { 4001, 4002, 4003 };
        int widths[] = { UiScale(104), UiScale(96), UiScale(116) };
        PlaceButtonsLeft(ids, widths, 3, page.left, top, btnH, gap);
    }
    SetWindowPos(g_hSpeedResult, NULL, page.left + UiScale(16), top + UiScale(64),
        right - page.left - UiScale(32), UiScale(86), SWP_NOZORDER);
    SetWindowPos(g_hSpeedProgress, NULL, page.left, top + UiScale(178),
        right - page.left, UiScale(8), SWP_NOZORDER);
    SetWindowPos(GetDlgItem(g_hTab, 4006), NULL, page.left, top + UiScale(196),
        UiScale(220), UiScale(22), SWP_NOZORDER);
    {
        int saveW = UiScale(104);
        int defaultW = UiScale(96);
        int saveX = right - saveW;
        int defaultX = saveX - gap - defaultW;
        SetWindowPos(GetDlgItem(g_hTab, 4004), NULL, saveX, top + UiScale(192),
            saveW, btnH, SWP_NOZORDER);
        SetWindowPos(GetDlgItem(g_hTab, 4005), NULL, defaultX, top + UiScale(192),
            defaultW, btnH, SWP_NOZORDER);
    }
    {
        int editTop = top + UiScale(240);
        int editH = page.bottom - editTop - UiScale(28);
        if (editH < UiScale(80)) editH = UiScale(80);
        SetWindowPos(g_hHostsEdit, NULL, page.left + UiScale(12), editTop,
            right - page.left - UiScale(24), editH, SWP_NOZORDER);
    }

    // ---------- 实用工具（由 tools.cpp 负责内部布局） ----------
    LayoutToolControls(page);

    LayoutListColumns(page);
}

// ====================== Main window painting ======================

void PaintMainWindow(HWND hWnd, HDC hdc) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    int sideW = g_hSidebar ? (SidebarExpanded() ? UiScale(UiMetrics::SidebarWidth)
                                                : UiScale(UiMetrics::SidebarWidthCompact)) : 0;
    int contentX = sideW + UiScale(UiMetrics::ContentMarginX);
    int page = CurrentPage();
    const PageInfo& info = kPages[page];
    int statusTop = rc.bottom - UiScale(UiMetrics::StatusBarHeight);
    bool admin = g_admin;

    std::wstring adminLabel = admin ? L"管理员模式" : L"标准权限";

    BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
        SolidBrush bg(GpColor(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);

        // Header icon chip
        float chip = (float)UiScale(40);
        RectF chipRect((float)contentX, (float)UiScale(22), chip, chip);
        FillRound(g, chipRect, (float)UiScale(11), GpColor(kAccentSoft));

        // Status bar
        SolidBrush line(GpColor(kBorder));
        g.FillRectangle(&line, (float)sideW, (float)statusTop,
            (float)(rc.right - sideW), 1.0f);

        // Admin pill
        int pillH = UiScale(26);
        int pillW = UiScale(116);
        RectF pill((float)(rc.right - UiScale(24) - pillW),
            (float)(statusTop + (UiScale(UiMetrics::StatusBarHeight) - pillH) / 2),
            (float)pillW, (float)pillH);
        FillRound(g, pill, pill.Height / 2.0f, GpColor(admin ? kSuccessSoft : kWarningSoft));
    }, [&](HDC dc) {
        // Header texts
        RECT iconRect = { contentX, UiScale(22), contentX + UiScale(40), UiScale(62) };
        DrawTextGdi(dc, info.glyph, iconRect, g_f.iconLarge, kAccent,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        int textX = contentX + UiScale(54);
        RECT titleRect = { textX, UiScale(18), rc.right - UiScale(24), UiScale(46) };
        DrawTextGdi(dc, info.title, titleRect, g_f.title, kTextPrimary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT subRect = { textX, UiScale(46), rc.right - UiScale(24), UiScale(68) };
        DrawTextGdi(dc, info.subtitle, subRect, g_f.subtitle, kTextSecondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Status admin pill text
        int pillH = UiScale(26);
        int pillW = UiScale(116);
        int pillX = rc.right - UiScale(24) - pillW;
        int pillY = statusTop + (UiScale(UiMetrics::StatusBarHeight) - pillH) / 2;
        RECT lockRect = { pillX + UiScale(12), pillY, pillX + UiScale(30), pillY + pillH };
        DrawTextGdi(dc, admin ? L"\xE72E" : L"\xE7BA", lockRect, g_f.iconSmall,
            admin ? kSuccess : kWarning, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT labelRect = { pillX + UiScale(30), pillY, pillX + pillW - UiScale(8), pillY + pillH };
        DrawTextGdi(dc, adminLabel, labelRect, g_f.caption,
            admin ? RGB(21, 128, 61) : kWarning, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    });
}

// ====================== Page host painting (cards) ======================

void DrawCard(Graphics& g, RECT r, float radius) {
    RectF rr((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
    DrawCardShadow(g, rr, radius);
    FillRound(g, rr, radius, GpColor(kWhite));
    StrokeRound(g, RectF(rr.X + 0.5f, rr.Y + 0.5f, rr.Width - 1.0f, rr.Height - 1.0f),
        radius, GpColor(kBorder), 1.0f);
}

void PaintPageHost(HWND hTab, HDC hdc) {
    RECT rc;
    GetClientRect(hTab, &rc);
    RECT page = rc;
    TabCtrl_AdjustRect(hTab, FALSE, &page);
    int current = CurrentPage();
    float radius = (float)UiScale(UiMetrics::CardRadius);
    int cardTop = page.top + UiScale(UiMetrics::CardTop);
    int cardBottom = page.bottom - UiScale(16);

    std::vector<RECT> cards;
    if (current == 4) {
        RECT speed = { page.left, page.top + UiScale(50), page.right, page.top + UiScale(166) };
        RECT hosts = { page.left, page.top + UiScale(228), page.right, cardBottom };
        cards.push_back(speed);
        cards.push_back(hosts);
    } else if (current == 6) {
        int topOffset = (g_toolFunction == 1) ? UiMetrics::CardTop : UiMetrics::CardTop + 40;
        RECT toolCard = { page.left, page.top + UiScale(topOffset), page.right, cardBottom };
        cards.push_back(toolCard);
    } else {
        RECT mainCard = { page.left, cardTop, page.right, cardBottom };
        cards.push_back(mainCard);
    }

    bool shredHint = (current == 6 && g_toolFunction == 0 && !cards.empty());
    int hintCx = 0, hintCy = 0, hintChip = UiScale(72);
    if (shredHint) {
        hintCx = (cards[0].left + cards[0].right) / 2;
        hintCy = (cards[0].top + cards[0].bottom) / 2 - UiScale(30);
    }

    BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
        SolidBrush bg(GpColor(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        for (const RECT& r : cards) {
            DrawCard(g, r, radius);
        }
        if (shredHint) {
            RectF chip((float)(hintCx - hintChip / 2), (float)(hintCy - hintChip / 2),
                       (float)hintChip, (float)hintChip);
            FillRound(g, chip, (float)UiScale(20), GpColor(kAccentSoft));
        }
    }, [&](HDC dc) {
        if (!shredHint) return;
        RECT glyphRect = { hintCx - hintChip / 2, hintCy - hintChip / 2,
                           hintCx + hintChip / 2, hintCy + hintChip / 2 };
        DrawTextGdi(dc, L"\xE74D", glyphRect, g_f.iconEmpty, kAccent,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        int textTop = hintCy + hintChip / 2;
        RECT titleRect = { 0, textTop + UiScale(16), rc.right, textTop + UiScale(40) };
        DrawTextGdi(dc, L"选择要粉碎的文件", titleRect, g_f.section, kTextSecondary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT hintRect = { 0, textTop + UiScale(40), rc.right, textTop + UiScale(64) };
        DrawTextGdi(dc, L"粉碎会覆写文件内容，操作不可撤销", hintRect, g_f.caption, kTextTertiary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    });
}

// ====================== Chrome sync / install ======================

void UiSyncChrome(HWND hMain) {
    if (!hMain) return;
    InvalidateRect(hMain, NULL, FALSE);
    if (g_hSidebar) InvalidateRect(g_hSidebar, NULL, FALSE);
    if (g_hTab) InvalidateRect(g_hTab, NULL, FALSE);
}

void ApplyDwm(HWND hMain) {
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hMain, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF caption = RGB(246, 247, 250);
    DwmSetWindowAttribute(hMain, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    COLORREF captionText = RGB(16, 24, 40);
    DwmSetWindowAttribute(hMain, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
}

void StyleStatic(HWND hStatic) {
    if (!hStatic) return;
    LONG_PTR exStyle = GetWindowLongPtrW(hStatic, GWL_EXSTYLE);
    if (exStyle & WS_EX_CLIENTEDGE) {
        SetWindowLongPtrW(hStatic, GWL_EXSTYLE, exStyle & ~(LONG_PTR)WS_EX_CLIENTEDGE);
        SetWindowPos(hStatic, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    HFONT font = g_f.caption;
    if (hStatic == g_hSpeedResult) font = g_f.speed;
    if (GetDlgCtrlID(hStatic) == 4006) font = g_f.section;
    SendMessageW(hStatic, WM_SETFONT, (WPARAM)font, FALSE);
}

void UpdateComboRegion(HWND hCombo) {
    RECT rc;
    GetClientRect(hCombo, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    int radius = UiScale(8);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1,
        radius * 2, radius * 2);
    if (rgn) SetWindowRgn(hCombo, rgn, TRUE);
}

void PaintCombo(HWND hCombo, HDC hdc) {
    RECT rc;
    GetClientRect(hCombo, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    bool enabled = IsWindowEnabled(hCombo) != FALSE;
    bool active = GetFocus() == hCombo ||
        SendMessageW(hCombo, CB_GETDROPPEDSTATE, 0, 0) != 0;
    COLORREF fill = enabled ? kWhite : kDisabledFill;
    COLORREF border = active ? kAccent : kBorder;

    BufferedPaint(hdc, rc, [&](HDC, Graphics& g) {
        // 圆角外侧先铺页面底色：整块填白会在四角露出白色方块
        SolidBrush page(GpColor(kBg));
        g.FillRectangle(&page, 0, 0, rc.right, rc.bottom);
        RectF rr(0.5f, 0.5f, (float)rc.right - 1.0f, (float)rc.bottom - 1.0f);
        FillRound(g, rr, (float)UiScale(8), GpColor(fill));
        StrokeRound(g, rr, (float)UiScale(8), GpColor(border), active ? 1.5f : 1.0f);
    }, [&](HDC dc) {
        wchar_t text[256] = { 0 };
        int len = GetWindowTextW(hCombo, text, 256);
        RECT textRect = { UiScale(12), 0, rc.right - UiScale(28), rc.bottom };
        DrawTextGdi(dc, std::wstring(text, len > 0 ? len : 0), textRect, g_f.body,
            enabled ? kTextPrimary : kTextTertiary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT arrowRect = { rc.right - UiScale(26), 0, rc.right - UiScale(8), rc.bottom };
        DrawTextGdi(dc, L"\xE70D", arrowRect, g_f.iconSmall,
            enabled ? kTextSecondary : kTextTertiary,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    });
}

LRESULT CALLBACK ComboSubclassProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR uIdSubclass, DWORD_PTR) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        UpdateComboRegion(hWnd);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hWnd, &ps);
        PaintCombo(hWnd, dc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintCombo(hWnd, (HDC)wParam);
        return 0;
    case WM_PRINT:
        if (lParam & PRF_CLIENT) PaintCombo(hWnd, (HDC)wParam);
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    case CB_SETCURSEL: {
        LRESULT result = DefSubclassProc(hWnd, message, wParam, lParam);
        InvalidateRect(hWnd, NULL, FALSE);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, ComboSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hWnd, message, wParam, lParam);
}

void StyleCombo(HWND hCombo) {
    if (!hCombo) return;
    SendMessageW(hCombo, WM_SETFONT, (WPARAM)g_f.body, FALSE);
    SetWindowTheme(hCombo, L"Explorer", NULL);
    SendMessageW(hCombo, CB_SETITEMHEIGHT, (WPARAM)-1, UiScale(26));
    SendMessageW(hCombo, CB_SETITEMHEIGHT, 0, UiScale(26));

    LONG_PTR style = GetWindowLongPtrW(hCombo, GWL_STYLE);
    if (style & WS_BORDER) {
        SetWindowLongPtrW(hCombo, GWL_STYLE, style & ~(LONG_PTR)WS_BORDER);
        SetWindowPos(hCombo, NULL, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    if (!GetWindowLongPtrW(hCombo, GWLP_USERDATA)) {
        SetWindowLongPtrW(hCombo, GWLP_USERDATA, 1);
        SetWindowSubclass(hCombo, ComboSubclassProc, 1, 0);
    }
    UpdateComboRegion(hCombo);
    InvalidateRect(hCombo, NULL, TRUE);
}

void UiCenterComboInRow(HWND hCombo, int x, int width, int rowTop, int rowHeight, int dropHeight) {
    if (!hCombo) return;
    RECT rc;
    GetWindowRect(hCombo, &rc);
    int comboH = rc.bottom - rc.top;
    int y = rowTop + (rowHeight - comboH) / 2;
    if (y < rowTop) y = rowTop;
    SetWindowPos(hCombo, NULL, x, y, width, UiScale(dropHeight), SWP_NOZORDER);
}

void StyleAllChildControls() {
    if (!g_hTab) return;
    for (HWND h = GetWindow(g_hTab, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
        wchar_t cls[64] = { 0 };
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"Button") == 0) {
            LONG_PTR type = GetWindowLongPtrW(h, GWL_STYLE) & BS_TYPEMASK;
            if (type == BS_AUTOCHECKBOX) {
                SendMessageW(h, WM_SETFONT, (WPARAM)g_f.body, FALSE);
                SetWindowTheme(h, L"Explorer", NULL);
            } else {
                StyleButton(h);
            }
        } else if (wcscmp(cls, L"SysListView32") == 0) {
            StyleListView(h);
        } else if (wcscmp(cls, L"Edit") == 0) {
            StyleEdit(h);
        } else if (wcscmp(cls, L"ComboBox") == 0) {
            StyleCombo(h);
        } else if (wcscmp(cls, L"Static") == 0) {
            StyleStatic(h);
        } else if (wcscmp(cls, L"msctls_progress32") == 0) {
            StyleProgress(h);
        }
    }

    if (g_hStatusText) SendMessageW(g_hStatusText, WM_SETFONT, (WPARAM)g_f.caption, FALSE);
    if (g_hShredPath) StyleEdit(g_hShredPath);
    if (g_hToolPathEdit) StyleEdit(g_hToolPathEdit);
    if (g_hUninstallSearch) StyleEdit(g_hUninstallSearch);
}

// ====================== Public API ======================

void UiInitProcessDpi() {
    typedef BOOL(WINAPI* SetDpiContextFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetDpiContextFn setContext =
            (SetDpiContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if (setContext((HANDLE)-4)) {
                g_dpi = QueryWindowDpi(NULL);
                return;
            }
        }
        typedef BOOL(WINAPI* SetAwareFn)(void);
        SetAwareFn setAware = (SetAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
        if (setAware) setAware();
    }
    g_dpi = QueryWindowDpi(NULL);
}

int UiScale(int px) {
    return MulDiv(px, g_dpi, 96);
}

int UiCurrentDpi() {
    return g_dpi;
}

void UiInstall(HWND hMain) {
    g_hMain = hMain;
    g_dpi = QueryWindowDpi(hMain);
    DestroyFontSet(g_f);
    CreateFontSet();
    EnsureGdiplus();
    g_admin = IsRunAsAdmin();

    ApplyDwm(hMain);
    CreateSidebar(hMain);
    TabCtrl_SetCurSel(g_hTab, 0);
    StyleAllChildControls();
    UpdateNavTooltips();
    UiSyncChrome(hMain);
}

void UiOnGetMinMaxInfo(MINMAXINFO* mmi) {
    if (!mmi) return;
    mmi->ptMinTrackSize.x = UiScale(980);
    mmi->ptMinTrackSize.y = UiScale(640);
}

void UiOnDpiChanged(HWND hMain, WPARAM wParam, LPARAM lParam) {
    int dpi = (int)LOWORD(wParam);
    if (dpi > 0) g_dpi = dpi;
    DestroyFontSet(g_f);
    CreateFontSet();
    StyleAllChildControls();
    RECT* suggested = (RECT*)lParam;
    if (suggested) {
        SetWindowPos(hMain, NULL, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    UiLayoutMainWindow(hMain);
    UiSyncChrome(hMain);
}

void UiLayoutMainWindow(HWND hMain) {
    if (!hMain) return;
    RECT rc;
    GetClientRect(hMain, &rc);
    int width = rc.right;
    int height = rc.bottom;
    if (width <= 0 || height <= 0) return;

    int dpi = QueryWindowDpi(hMain);
    if (dpi != g_dpi) {
        g_dpi = dpi;
        DestroyFontSet(g_f);
        CreateFontSet();
        StyleAllChildControls();
    }

    bool compact = width < UiScale(1160);
    int sideW = UiScale(compact ? UiMetrics::SidebarWidthCompact : UiMetrics::SidebarWidth);
    if (g_hSidebar) {
        SetWindowPos(g_hSidebar, NULL, 0, 0, sideW, height, SWP_NOZORDER);
    }

    int contentX = sideW + UiScale(UiMetrics::ContentMarginX);
    int tabTop = UiScale(UiMetrics::HeaderHeight);
    int statusH = UiScale(UiMetrics::StatusBarHeight);
    int tabWidth = width - contentX - UiScale(UiMetrics::ContentMarginX);
    int tabHeight = height - tabTop - statusH;
    if (tabWidth < UiScale(300)) tabWidth = UiScale(300);
    if (tabHeight < UiScale(200)) tabHeight = UiScale(200);

    if (g_hTab) {
        SetWindowPos(g_hTab, NULL, contentX, tabTop, tabWidth, tabHeight, SWP_NOZORDER);
        RECT page;
        GetClientRect(g_hTab, &page);
        TabCtrl_AdjustRect(g_hTab, FALSE, &page);
        LayoutPages(page);
    }

    if (g_hStatusText) {
        int rightReserve = UiScale(170);
        SetWindowPos(g_hStatusText, NULL, contentX,
            height - statusH + UiScale(16),
            tabWidth - rightReserve, UiScale(20), SWP_NOZORDER);
    }

    UpdateNavTooltips();
    InvalidateRect(hMain, NULL, TRUE);
    UiSyncChrome(hMain);
}

void UiPaintMainWindow(HWND hMain, HDC hdc) {
    if (hdc) {
        PaintMainWindow(hMain, hdc);
        return;
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hMain, &ps);
    PaintMainWindow(hMain, dc);
    EndPaint(hMain, &ps);
}

void UiPaintPageHost(HWND hTab, HDC hdc) {
    if (hdc) {
        PaintPageHost(hTab, hdc);
        return;
    }
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hTab, &ps);
    PaintPageHost(hTab, dc);
    EndPaint(hTab, &ps);
}

HBRUSH UiCtlColor(UINT message, HDC hdc, HWND hCtl) {
    switch (message) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        COLORREF color = kTextSecondary;
        if (hCtl == g_hSpeedResult) color = kTextPrimary;
        else if (hCtl == g_hToolInfoLabel && g_toolFunction == 0) color = RGB(180, 83, 9);
        else if (GetDlgCtrlID(hCtl) == 4006) color = kTextPrimary;
        SetTextColor(hdc, color);

        // 文本会变化的静态控件必须用不透明背景擦除旧内容，
        // 空画刷 + 透明背景会让 WM_SETTEXT 后新旧两段文字重叠。
        if (hCtl == g_hStatusText || hCtl == g_hToolInfoLabel) {
            static HBRUSH statusBg = CreateSolidBrush(kBg);
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kBg);
            return statusBg;
        }
        if (hCtl == g_hSpeedResult) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kWhite);
            return (HBRUSH)GetStockObject(WHITE_BRUSH);
        }

        SetBkMode(hdc, TRANSPARENT);
        return (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        SetTextColor(hdc, kTextPrimary);
        SetBkColor(hdc, kWhite);
        return (HBRUSH)GetStockObject(WHITE_BRUSH);
    }
    default:
        return NULL;
    }
}
