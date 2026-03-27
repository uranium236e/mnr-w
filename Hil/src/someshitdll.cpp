#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "../include/someshitdll.h"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

const wchar_t* const kStateProp = L"SOMESHITDLL_ATTACH_STATE";

HHOOK g_keyboard_hook = nullptr;
bool g_super_down = false;
std::vector<HWND> g_attached_hwnds;

struct AttachState {
    WNDPROC old_proc = nullptr;
    bool snapping = false;
    bool moving = false;
    bool resizing = false;
    POINT snap_start_screen{};
    DWORD snap_start_tick = 0;
    RECT drag_start_rect{};
    POINT drag_start_screen{};
    int drag_quadrant = 0;
    int drag_moved_sq = 0;
    HCURSOR cursors[4] = { nullptr, nullptr, nullptr, nullptr }; // NW, NE, SE, SW
    HCURSOR move_cursor = nullptr;
    bool owns_cursors = false;
    bool intercept_super = true;
    bool enable_move = true;
    bool enable_resize = true;
    bool enable_snap = true;
    bool enable_close = true;
};

void apply_borderless(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    LONG_PTR ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex_style |= WS_EX_APPWINDOW;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

bool try_apply_system_backdrop(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return false;
    }

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto set_attr =
        reinterpret_cast<DwmSetWindowAttributeFn>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
    if (!set_attr) {
        FreeLibrary(dwm);
        return false;
    }

    // DWMWA_SYSTEMBACKDROP_TYPE (38) + DWMSBT_TRANSIENTWINDOW (3)
    const DWORD kDWMWA_SYSTEMBACKDROP_TYPE = 38;
    const int kDWMSBT_TRANSIENTWINDOW = 3;
    const int backdrop = kDWMSBT_TRANSIENTWINDOW;
    const HRESULT hr = set_attr(hwnd, kDWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    FreeLibrary(dwm);
    return SUCCEEDED(hr);
}

bool try_apply_accent_acrylic(HWND hwnd, unsigned int tint_argb) {
    struct ACCENT_POLICY {
        int AccentState;
        int AccentFlags;
        int GradientColor;
        int AnimationId;
    };
    struct WINDOWCOMPOSITIONATTRIBDATA {
        int Attrib;
        PVOID pvData;
        SIZE_T cbData;
    };

    constexpr int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;
    constexpr int WCA_ACCENT_POLICY = 19;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return false;
    }

    using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
    auto set_wca = reinterpret_cast<SetWindowCompositionAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetWindowCompositionAttribute")));
    if (!set_wca) {
        return false;
    }

    ACCENT_POLICY policy{};
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.AccentFlags = 2;
    policy.GradientColor = static_cast<int>(tint_argb);

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);

    return set_wca(hwnd, &data) != FALSE;
}

bool try_apply_dwm_blur(HWND hwnd) {
    MARGINS margins = { -1, -1, -1, -1 };
    (void)DwmExtendFrameIntoClientArea(hwnd, &margins);

    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = TRUE;
    blur.hRgnBlur = nullptr;
    return SUCCEEDED(DwmEnableBlurBehindWindow(hwnd, &blur));
}

bool apply_acrylic_best_effort(HWND hwnd, unsigned int tint_argb) {
    if (try_apply_system_backdrop(hwnd)) {
        return true;
    }
    if (try_apply_accent_acrylic(hwnd, tint_argb)) {
        return true;
    }
    return try_apply_dwm_blur(hwnd);
}

bool get_client_rect(HWND hwnd, RECT* out_rect) {
    if (!out_rect) {
        return false;
    }
    RECT rect{};
    if (!GetClientRect(hwnd, &rect)) {
        return false;
    }
    *out_rect = rect;
    return true;
}

int quadrant_from_client_point(HWND hwnd, POINT client_pt) {
    RECT rc{};
    if (!get_client_rect(hwnd, &rc)) {
        return 0;
    }
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const bool left = client_pt.x < w / 2;
    const bool top = client_pt.y < h / 2;
    if (left && top) return 0;      // NW
    if (!left && top) return 1;     // NE
    if (!left && !top) return 2;    // SE
    return 3;                       // SW
}

bool is_attached_foreground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) {
        return false;
    }
    for (HWND hwnd : g_attached_hwnds) {
        if (hwnd == fg) {
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM w_param, LPARAM l_param) {
    if (code != HC_ACTION) {
        return CallNextHookEx(g_keyboard_hook, code, w_param, l_param);
    }

    const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    const bool key_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
    const bool key_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);
    if (!info) {
        return CallNextHookEx(g_keyboard_hook, code, w_param, l_param);
    }

    const bool is_super_key = (info->vkCode == VK_LWIN || info->vkCode == VK_RWIN);
    if (!is_super_key) {
        return CallNextHookEx(g_keyboard_hook, code, w_param, l_param);
    }

    // Only intercept for an attached foreground window.
    if (!is_attached_foreground()) {
        return CallNextHookEx(g_keyboard_hook, code, w_param, l_param);
    }

    if (key_down) {
        g_super_down = true;
    }
    if (key_up) {
        g_super_down = false;
    }

    // Swallow Win key so Start doesn't open.
    return 1;
}

void ensure_keyboard_hook_installed() {
    if (g_keyboard_hook) {
        return;
    }
    g_keyboard_hook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandleW(nullptr), 0);
}

void maybe_uninstall_keyboard_hook() {
    if (!g_keyboard_hook) {
        return;
    }
    if (!g_attached_hwnds.empty()) {
        return;
    }
    UnhookWindowsHookEx(g_keyboard_hook);
    g_keyboard_hook = nullptr;
    g_super_down = false;
}

void snap_window_to_quadrant(HWND hwnd, int quadrant) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return;
    }

    const RECT work = mi.rcWork;
    const int full_w = std::max(1, static_cast<int>(work.right - work.left));
    const int full_h = std::max(1, static_cast<int>(work.bottom - work.top));
    const int half_w = full_w / 2;
    const int half_h = full_h / 2;

    int x = work.left;
    int y = work.top;
    if (quadrant == 1 || quadrant == 2) x = work.left + half_w;
    if (quadrant == 2 || quadrant == 3) y = work.top + half_h;

    SetWindowPos(hwnd, HWND_TOP, x, y, half_w, half_h, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
}

bool rotate_bgra_90(const std::vector<std::uint8_t>& src, int src_w, int src_h,
                    std::vector<std::uint8_t>* dst, int* dst_w, int* dst_h) {
    if (!dst || !dst_w || !dst_h || src_w <= 0 || src_h <= 0) {
        return false;
    }
    dst->assign(static_cast<std::size_t>(src_w * src_h * 4), 0);
    *dst_w = src_h;
    *dst_h = src_w;
    for (int y = 0; y < src_h; ++y) {
        for (int x = 0; x < src_w; ++x) {
            const int nx = src_h - 1 - y;
            const int ny = x;
            const std::size_t si = static_cast<std::size_t>((y * src_w + x) * 4);
            const std::size_t di = static_cast<std::size_t>((ny * (*dst_w) + nx) * 4);
            (*dst)[di + 0] = src[si + 0];
            (*dst)[di + 1] = src[si + 1];
            (*dst)[di + 2] = src[si + 2];
            (*dst)[di + 3] = src[si + 3];
        }
    }
    return true;
}

bool load_bgra32_from_wic(const wchar_t* path, std::vector<std::uint8_t>* out_pixels, int* out_w, int* out_h) {
    if (!path || !path[0] || !out_pixels || !out_w || !out_h) {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0 || w > 512 || h > 512) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || !converter) {
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        return false;
    }

    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    out_pixels->assign(static_cast<std::size_t>(h) * stride, 0);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(out_pixels->size()), out_pixels->data());

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();

    if (FAILED(hr)) {
        return false;
    }

    *out_w = static_cast<int>(w);
    *out_h = static_cast<int>(h);
    return true;
}

HCURSOR create_cursor_from_bgra(const std::vector<std::uint8_t>& pixels, int w, int h, int hot_x, int hot_y) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* dib_bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !dib_bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }

    std::memcpy(dib_bits, pixels.data(), pixels.size());

    const int mask_stride = ((w + 31) / 32) * 4;
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(mask_stride * h), 0x00);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t pi = static_cast<std::size_t>((y * w + x) * 4 + 3);
            const std::uint8_t a = pixels[pi];
            if (a == 0) {
                const int byte_index = y * mask_stride + (x / 8);
                const int bit = 7 - (x % 8);
                mask[static_cast<std::size_t>(byte_index)] |= static_cast<std::uint8_t>(1u << bit);
            }
        }
    }

    HBITMAP mono = CreateBitmap(w, h, 1, 1, mask.data());
    if (!mono) {
        DeleteObject(color);
        return nullptr;
    }

    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(std::clamp(hot_x, 0, w - 1));
    info.yHotspot = static_cast<DWORD>(std::clamp(hot_y, 0, h - 1));
    info.hbmColor = color;
    info.hbmMask = mono;

    HCURSOR cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(color);
    DeleteObject(mono);
    return cursor;
}

bool build_quadrant_cursors(AttachState* state, const SomeshitDllConfig& cfg) {
    if (!state) {
        return false;
    }

    if (!cfg.enable_custom_cursors || !cfg.cursor_image_path || !cfg.cursor_image_path[0]) {
        state->cursors[0] = LoadCursorA(nullptr, IDC_SIZENWSE);
        state->cursors[1] = LoadCursorA(nullptr, IDC_SIZENESW);
        state->cursors[2] = LoadCursorA(nullptr, IDC_SIZENWSE);
        state->cursors[3] = LoadCursorA(nullptr, IDC_SIZENESW);
        state->owns_cursors = false;
        state->move_cursor = LoadCursorA(nullptr, IDC_SIZEALL);
        return true;
    }

    std::vector<std::uint8_t> base_pixels;
    int w = 0, h = 0;
    if (!load_bgra32_from_wic(cfg.cursor_image_path, &base_pixels, &w, &h)) {
        // Fall back to system cursors instead of failing the attach.
        state->cursors[0] = LoadCursorA(nullptr, IDC_SIZENWSE);
        state->cursors[1] = LoadCursorA(nullptr, IDC_SIZENESW);
        state->cursors[2] = LoadCursorA(nullptr, IDC_SIZENWSE);
        state->cursors[3] = LoadCursorA(nullptr, IDC_SIZENESW);
        state->owns_cursors = false;
        state->move_cursor = LoadCursorA(nullptr, IDC_SIZEALL);
        return true;
    }

    std::vector<std::uint8_t> pixels = base_pixels;
    int cw = w, ch = h;

    auto make_hot = [&](int quadrant, int width, int height, int* out_x, int* out_y) {
        switch (quadrant) {
        case 0: *out_x = 0; *out_y = 0; break;                  // NW
        case 1: *out_x = width - 1; *out_y = 0; break;          // NE
        case 2: *out_x = width - 1; *out_y = height - 1; break; // SE
        default:*out_x = 0; *out_y = height - 1; break;         // SW
        }
    };

    for (int q = 0; q < 4; ++q) {
        pixels = base_pixels;
        cw = w;
        ch = h;
        if (cfg.rotate_cursor_quadrants) {
            for (int r = 0; r < q; ++r) {
                std::vector<std::uint8_t> rotated;
                int rw = 0, rh = 0;
                if (!rotate_bgra_90(pixels, cw, ch, &rotated, &rw, &rh)) {
                    return false;
                }
                pixels.swap(rotated);
                cw = rw;
                ch = rh;
            }
        }

        int hot_x = 0, hot_y = 0;
        make_hot(q, cw, ch, &hot_x, &hot_y);
        state->cursors[q] = create_cursor_from_bgra(pixels, cw, ch, hot_x, hot_y);
        if (!state->cursors[q]) {
            // Fall back to system cursors instead of failing the attach.
            state->cursors[0] = LoadCursorA(nullptr, IDC_SIZENWSE);
            state->cursors[1] = LoadCursorA(nullptr, IDC_SIZENESW);
            state->cursors[2] = LoadCursorA(nullptr, IDC_SIZENWSE);
            state->cursors[3] = LoadCursorA(nullptr, IDC_SIZENESW);
            state->owns_cursors = false;
            state->move_cursor = LoadCursorA(nullptr, IDC_SIZEALL);
            return true;
        }
    }

    state->owns_cursors = true;
    state->move_cursor = LoadCursorA(nullptr, IDC_SIZEALL);
    return true;
}

LRESULT CALLBACK someshitdll_wndproc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AttachState*>(GetPropW(hwnd, kStateProp));
    if (!state || !state->old_proc) {
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }

    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (state->enable_close && g_super_down && (w_param == 'W')) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    }
    case WM_NCDESTROY: {
        RemovePropW(hwnd, kStateProp);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(state->old_proc));
        if (state->owns_cursors) {
            for (HCURSOR& cursor : state->cursors) {
                if (cursor) {
                    DestroyCursor(cursor);
                    cursor = nullptr;
                }
            }
        }
        g_attached_hwnds.erase(std::remove(g_attached_hwnds.begin(), g_attached_hwnds.end(), hwnd), g_attached_hwnds.end());
        maybe_uninstall_keyboard_hook();
        delete state;
        break;
    }
    case WM_CAPTURECHANGED: {
        state->snapping = false;
        state->moving = false;
        state->resizing = false;
        state->drag_moved_sq = 0;
        break;
    }
    case WM_LBUTTONDOWN: {
        if (!state->enable_move) {
            break;
        }
        if (!g_super_down) {
            break;
        }
        state->moving = true;
        state->resizing = false;
        state->snapping = false;
        state->drag_moved_sq = 0;
        GetWindowRect(hwnd, &state->drag_start_rect);
        GetCursorPos(&state->drag_start_screen);
        SetCapture(hwnd);
        if (state->move_cursor) {
            SetCursor(state->move_cursor);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (!g_super_down) {
            break;
        }
        if (state->enable_resize) {
            state->resizing = true;
            state->moving = false;
            state->snapping = false;
            state->drag_moved_sq = 0;
            state->snap_start_tick = GetTickCount();
            GetWindowRect(hwnd, &state->drag_start_rect);
            GetCursorPos(&state->drag_start_screen);
            POINT client{ GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param) };
            state->drag_quadrant = quadrant_from_client_point(hwnd, client);
            SetCapture(hwnd);
            if (state->drag_quadrant >= 0 && state->drag_quadrant < 4 && state->cursors[state->drag_quadrant]) {
                SetCursor(state->cursors[state->drag_quadrant]);
            }
            return 0;
        }
        if (state->enable_snap) {
            state->snapping = true;
            state->snap_start_tick = GetTickCount();
            POINT screen{};
            GetCursorPos(&screen);
            state->snap_start_screen = screen;
            state->drag_start_screen = screen;
            state->drag_moved_sq = 0;
            SetCapture(hwnd);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (!state->moving && !state->resizing && !state->snapping) {
            break;
        }

        POINT screen{};
        GetCursorPos(&screen);
        const int dx = static_cast<int>(screen.x - state->drag_start_screen.x);
        const int dy = static_cast<int>(screen.y - state->drag_start_screen.y);
        const int moved_sq = dx * dx + dy * dy;
        state->drag_moved_sq = std::max(state->drag_moved_sq, moved_sq);

        if (state->moving) {
            const int x = state->drag_start_rect.left + dx;
            const int y = state->drag_start_rect.top + dy;
            const int w = std::max(64, static_cast<int>(state->drag_start_rect.right - state->drag_start_rect.left));
            const int h = std::max(64, static_cast<int>(state->drag_start_rect.bottom - state->drag_start_rect.top));
            SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOOWNERZORDER);
            if (state->move_cursor) {
                SetCursor(state->move_cursor);
            }
            return 0;
        }

        if (state->resizing) {
            RECT r = state->drag_start_rect;
            const int min_w = 220;
            const int min_h = 160;
            switch (state->drag_quadrant) {
            case 0: // NW
                r.left += dx;
                r.top += dy;
                break;
            case 1: // NE
                r.right += dx;
                r.top += dy;
                break;
            case 2: // SE
                r.right += dx;
                r.bottom += dy;
                break;
            default: // SW
                r.left += dx;
                r.bottom += dy;
                break;
            }
            if (r.right - r.left < min_w) {
                if (state->drag_quadrant == 0 || state->drag_quadrant == 3) {
                    r.left = r.right - min_w;
                } else {
                    r.right = r.left + min_w;
                }
            }
            if (r.bottom - r.top < min_h) {
                if (state->drag_quadrant == 0 || state->drag_quadrant == 1) {
                    r.top = r.bottom - min_h;
                } else {
                    r.bottom = r.top + min_h;
                }
            }
            SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
            if (state->drag_quadrant >= 0 && state->drag_quadrant < 4 && state->cursors[state->drag_quadrant]) {
                SetCursor(state->cursors[state->drag_quadrant]);
            }
            return 0;
        }

        if (state->snapping) {
            POINT client{ GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param) };
            const int q = quadrant_from_client_point(hwnd, client);
            if (q >= 0 && q < 4 && state->cursors[q]) {
                SetCursor(state->cursors[q]);
                return 0;
            }
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!state->moving) {
            break;
        }
        state->moving = false;
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONUP: {
        if (state->resizing) {
            state->resizing = false;
            ReleaseCapture();
            return 0;
        }

        if (state->snapping) {
            state->snapping = false;
            ReleaseCapture();
            // Only snap after a real drag, not a single click.
            if (state->drag_moved_sq >= 20 * 20) {
                POINT client{ GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param) };
                const int q = quadrant_from_client_point(hwnd, client);
                snap_window_to_quadrant(hwnd, q);
            }
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        if (state->snapping || state->moving || state->resizing) {
            return TRUE;
        }
        break;
    }
    default:
        break;
    }

    return CallWindowProcW(state->old_proc, hwnd, msg, w_param, l_param);
}

bool attach_wndproc(HWND hwnd, const SomeshitDllConfig& cfg) {
    if (GetPropW(hwnd, kStateProp) != nullptr) {
        return true; // already attached
    }

    auto* state = new (std::nothrow) AttachState();
    if (!state) {
        return false;
    }

    if (!build_quadrant_cursors(state, cfg)) {
        delete state;
        return false;
    }

    state->intercept_super = cfg.enable_super_intercept != 0;
    state->enable_move = cfg.enable_super_move != 0;
    state->enable_resize = cfg.enable_super_resize != 0;
    state->enable_snap = cfg.enable_quadrant_snap != 0;
    state->enable_close = cfg.enable_super_close != 0;

    SetPropW(hwnd, kStateProp, state);
    state->old_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(someshitdll_wndproc)));
    if (!state->old_proc) {
        RemovePropW(hwnd, kStateProp);
        delete state;
        return false;
    }
    return true;
}

SomeshitDllConfig with_defaults(const SomeshitDllConfig* cfg) {
    SomeshitDllConfig out{};
    out.enable_borderless = 1;
    out.enable_acrylic = 1;
    out.enable_quadrant_snap = 1;
    out.enable_super_intercept = 1;
    out.enable_super_move = 1;
    out.enable_super_resize = 1;
    out.enable_super_close = 1;
    out.enable_custom_cursors = 0;
    out.cursor_image_path = nullptr;
    out.rotate_cursor_quadrants = 1;
    out.acrylic_tint_argb = 0xAA202020u;
    if (cfg) {
        out = *cfg;
        if (out.acrylic_tint_argb == 0) {
            out.acrylic_tint_argb = 0xAA202020u;
        }
    }
    return out;
}

}  // namespace

SOMESHITDLL_API int SomeshitDllAttach(HWND hwnd, const SomeshitDllConfig* config) {
    if (!hwnd || !IsWindow(hwnd)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }

    const SomeshitDllConfig cfg = with_defaults(config);

    if (cfg.enable_borderless) {
        apply_borderless(hwnd);
    }
    if (cfg.enable_acrylic) {
        (void)apply_acrylic_best_effort(hwnd, cfg.acrylic_tint_argb);
    }
    if (cfg.enable_quadrant_snap) {
        const bool wants_custom = cfg.enable_custom_cursors && cfg.cursor_image_path && cfg.cursor_image_path[0];
        HRESULT hr = S_OK;
        bool co_ok = true;
        if (wants_custom) {
            hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            co_ok = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
        const bool ok = attach_wndproc(hwnd, cfg);
        if (wants_custom && SUCCEEDED(hr)) {
            CoUninitialize();
        }
        if (!(co_ok && ok)) {
            SetLastError(ok ? ERROR_GEN_FAILURE : ERROR_INVALID_FUNCTION);
            return 0;
        }
        SetLastError(0);
        if (cfg.enable_super_intercept) {
            if (std::find(g_attached_hwnds.begin(), g_attached_hwnds.end(), hwnd) == g_attached_hwnds.end()) {
                g_attached_hwnds.push_back(hwnd);
            }
            ensure_keyboard_hook_installed();
        }
        return 1;
    }

    SetLastError(0);
    return 1;
}

#else

SOMESHITDLL_API int SomeshitDllAttach(void*, const SomeshitDllConfig*) {
    return 1;
}

#endif
