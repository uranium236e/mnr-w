#ifndef SOMESHITDLL_H
#define SOMESHITDLL_H

#ifdef _WIN32
#include <windows.h>
#if defined(SOMESHITDLL_BUILD)
#define SOMESHITDLL_API extern "C" __declspec(dllexport)
#else
#define SOMESHITDLL_API extern "C" __declspec(dllimport)
#endif
#else
#define SOMESHITDLL_API extern "C"
#endif

// Single-call attach helper. Call this after you have created your GLFW window
// and have the Win32 HWND (via glfwGetWin32Window).
//
// What it does (Windows):
// - removes standard window decorations (borderless popup)
// - applies acrylic/blur-ish background effect (best-effort by OS version)
// - enables Super + RightClick "quadrant snap" (4-way) with optional custom cursors
//
// Non-Windows: no-op success.

struct SomeshitDllConfig {
    int enable_borderless;       // default 1
    int enable_acrylic;          // default 1
    int enable_quadrant_snap;    // default 1
    int enable_super_intercept;  // default 1 (swallow Win key when attached window is foreground)
    int enable_super_move;       // default 1 (Win + LeftDrag moves window)
    int enable_super_resize;     // default 1 (Win + RightDrag resizes window)
    int enable_super_close;      // default 1 (Win + W posts WM_CLOSE)
    int enable_custom_cursors;   // default 0
    const wchar_t* cursor_image_path; // optional: PNG/ICO. Used as the NW cursor base.
    int rotate_cursor_quadrants; // default 1: rotate cursor_image by 0/90/180/270.
    unsigned int acrylic_tint_argb; // default 0xAA202020 (Win10 accent fallback)
};

// Returns non-zero on success. On failure, returns 0.
SOMESHITDLL_API int SomeshitDllAttach(HWND hwnd, const SomeshitDllConfig* config);

#endif
