#include "stdafx.h"
#include "SettingsDialogTheme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <memory>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {
constexpr wchar_t kThemeProperty[] = L"MPCVR.SettingsDialogTheme";
constexpr UINT_PTR kThemeSubclass = 1;

struct DialogTheme {
    COLORREF background = RGB(32, 32, 32);
    COLORREF text = RGB(255, 255, 255);
    COLORREF control = RGB(47, 47, 47);
    COLORREF border = RGB(72, 72, 72);
    COLORREF disabled = RGB(128, 128, 128);
    COLORREF accent = RGB(0, 120, 215);
    HBRUSH backgroundBrush = nullptr;
    HBRUSH controlBrush = nullptr;

    ~DialogTheme() {
        if (backgroundBrush) DeleteObject(backgroundBrush);
        if (controlBrush) DeleteObject(controlBrush);
    }
};

void FillColor(HDC dc, const RECT& rect, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Outline(HDC dc, const RECT& rect, COLORREF color)
{
    SetDCBrushColor(dc, color);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void Stroke(HDC dc, COLORREF color, int width, const POINT* points, int count)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, pen);
    Polyline(dc, points, count);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void PaintControl(HWND hwnd, HDC dc, DialogTheme& theme)
{
    RECT rect;
    GetClientRect(hwnd, &rect);
    const int saved = SaveDC(dc);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const COLORREF textColor = enabled ? theme.text : theme.disabled;
    SetTextColor(dc, textColor);
    wchar_t text[1024] = {};
    GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
    wchar_t className[32] = {};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const bool focused = GetFocus() == hwnd;
    const bool showFocus = focused && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    const UINT textFlags = (SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
    POINT cursor;
    GetCursorPos(&cursor);
    ScreenToClient(hwnd, &cursor);
    const bool hot = enabled && PtInRect(&rect, cursor);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    const int unit = std::max<LONG>(1, metrics.tmHeight / 12);

    if (!_wcsicmp(className, WC_COMBOBOXW)) {
        FillColor(dc, rect, theme.control);
        Outline(dc, rect, focused ? theme.accent : theme.border);
        const int arrowWidth = GetSystemMetrics(SM_CXVSCROLL);
        RECT label = rect;
        label.left += 4 * unit;
        label.right -= arrowWidth;
        DrawTextW(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        const int x = rect.right - arrowWidth / 2;
        const int y = (rect.bottom - 2 * unit) / 2;
        const POINT arrow[] = {{x - 3 * unit, y}, {x, y + 3 * unit}, {x + 3 * unit, y}};
        Stroke(dc, textColor, unit, arrow, 3);
    } else if ((style & BS_TYPEMASK) == BS_GROUPBOX) {
        RECT border = rect;
        border.top += metrics.tmHeight / 2;
        Outline(dc, border, theme.border);
        RECT label = {8 * unit, 0, rect.right, metrics.tmHeight};
        DrawTextW(dc, text, -1, &label, DT_CALCRECT | DT_SINGLELINE | textFlags);
        label.right += 3 * unit;
        FillColor(dc, label, theme.background);
        DrawTextW(dc, text, -1, &label, DT_SINGLELINE | textFlags);
    } else if ((style & BS_TYPEMASK) == BS_AUTOCHECKBOX || (style & BS_TYPEMASK) == BS_CHECKBOX) {
        FillColor(dc, rect, theme.background);
        const int size = std::max<LONG>(13 * unit, metrics.tmHeight - 2 * unit);
        const int y = (rect.bottom - size) / 2;
        RECT box = {0, y, size, y + size};
        const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
        FillColor(dc, box, checked && enabled ? theme.accent : theme.control);
        Outline(dc, box, hot ? theme.text : theme.disabled);
        if (checked) {
            const POINT check[] = {{3 * unit, y + size / 2}, {size / 2 - unit, y + size - 4 * unit}, {size - 3 * unit, y + 3 * unit}};
            Stroke(dc, enabled ? theme.text : theme.disabled, 2 * unit, check, 3);
        }
        RECT label = rect;
        label.left = size + 5 * unit;
        DrawTextW(dc, text, -1, &label, DT_SINGLELINE | DT_VCENTER | textFlags);
        if (showFocus) DrawFocusRect(dc, &label);
    } else {
        const bool pressed = SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED;
        FillColor(dc, rect, pressed ? theme.border : (hot ? RGB(60, 60, 60) : theme.control));
        Outline(dc, rect, focused || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON ? theme.accent : theme.border);
        DrawTextW(dc, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | textFlags);
        if (showFocus) {
            InflateRect(&rect, -3 * unit, -3 * unit);
            DrawFocusRect(dc, &rect);
        }
    }
    RestoreDC(dc, saved);
}

LRESULT CALLBACK ControlProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR id, DWORD_PTR data)
{
    auto& theme = *reinterpret_cast<DialogTheme*>(data);
    if (message == WM_CTLCOLORLISTBOX) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, theme.text);
        SetBkColor(dc, theme.control);
        return reinterpret_cast<LRESULT>(theme.controlBrush);
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        PaintControl(hwnd, dc, theme);
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_PRINTCLIENT) {
        PaintControl(hwnd, reinterpret_cast<HDC>(wParam), theme);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ControlProc, id);
    const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
    switch (message) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        [[fallthrough]];
    }
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT:
    case WM_UPDATEUISTATE:
    case BM_SETCHECK:
    case BM_SETSTATE:
    case BM_SETSTYLE:
    case CB_SETCURSEL:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    }
    return result;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR id, DWORD_PTR data)
{
    auto* theme = reinterpret_cast<DialogTheme*>(data);
    switch (message) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const bool input = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
        SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lParam)) ? theme->text : theme->disabled);
        SetBkColor(dc, input ? theme->control : theme->background);
        return reinterpret_cast<LRESULT>(input ? theme->controlBrush : theme->backgroundBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, theme->backgroundBrush);
        return 1;
    }
    case WM_NCDESTROY:
        RemovePropW(hwnd, kThemeProperty);
        RemoveWindowSubclass(hwnd, DialogProc, id);
        delete theme;
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
} // namespace

void InitializeSettingsDialogTheme(HWND dialog)
{
    if (GetPropW(dialog, kThemeProperty)) return;
    HIGHCONTRASTW contrast = {sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
            && (contrast.dwFlags & HCF_HIGHCONTRASTON)) return;
    HWND parent = GetParent(dialog);
    if (!parent) return;
    auto theme = std::make_unique<DialogTheme>();
    if (const auto* inherited = reinterpret_cast<DialogTheme*>(GetPropW(parent, kThemeProperty))) {
        theme->background = inherited->background;
        theme->text = inherited->text;
    } else {
        // The host owns the COM property-page palette. Ask its normal paint
        // handler instead of reading MPC-HC preferences or changing app themes.
        HDC dc = GetDC(parent);
        if (!dc) return;
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessageW(parent, WM_CTLCOLORDLG,
            reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(parent)));
        LOGBRUSH description = {};
        const bool solid = brush && GetObjectW(brush, sizeof(description), &description)
            && description.lbStyle == BS_SOLID;
        theme->background = solid ? description.lbColor : GetBkColor(dc);
        theme->text = GetTextColor(dc);
        ReleaseDC(parent, dc);
    }
    const int brightness = GetRValue(theme->background) + GetGValue(theme->background) + GetBValue(theme->background);
    if (brightness >= 3 * 128) return;
    if (theme->text == RGB(0, 0, 0)) theme->text = RGB(255, 255, 255);
    theme->backgroundBrush = CreateSolidBrush(theme->background);
    theme->controlBrush = CreateSolidBrush(theme->control);
    if (!theme->backgroundBrush || !theme->controlBrush) return;
    if (!SetPropW(dialog, kThemeProperty, theme.get())) return;
    if (!SetWindowSubclass(dialog, DialogProc, kThemeSubclass, reinterpret_cast<DWORD_PTR>(theme.get()))) {
        RemovePropW(dialog, kThemeProperty);
        return;
    }
    for (HWND child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t className[32] = {};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (!_wcsicmp(className, WC_BUTTONW) || !_wcsicmp(className, WC_COMBOBOXW)) {
            // Dialog controls intentionally overlap group-box sibling windows.
            // WS_CLIPSIBLINGS would let a higher group box clip away their
            // entire normal paint region, even though WM_PRINT looks correct.
            SetWindowSubclass(child, ControlProc, kThemeSubclass, reinterpret_cast<DWORD_PTR>(theme.get()));
            if (!_wcsicmp(className, WC_COMBOBOXW)) {
                COMBOBOXINFO combo = {sizeof(combo)};
                if (GetComboBoxInfo(child, &combo) && combo.hwndList) {
                    SetWindowTheme(combo.hwndList, L"DarkMode_Explorer", nullptr);
                }
            }
        } else if (!_wcsicmp(className, WC_STATICW)) {
            // Native themed statics can override WM_CTLCOLOR text colors,
            // including disabled labels. These only need the parent's palette.
            SetWindowTheme(child, L"", L"");
        } else {
            SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        }
    }
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(dialog, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DwmSetWindowAttribute(dialog, DWMWA_CAPTION_COLOR, &theme->background, sizeof(theme->background));
    DwmSetWindowAttribute(dialog, DWMWA_TEXT_COLOR, &theme->text, sizeof(theme->text));
    theme.release(); // Owned by the dialog subclass until WM_NCDESTROY.
}
