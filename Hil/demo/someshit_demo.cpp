#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32

#include <windows.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <cmath>

#include "../include/someshitdll.h"

static void api_error_callback(int code, const char* message) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, message ? message : "(null)");
}

int main() {
    glfwSetErrorCallback(api_error_callback);
    if (glfwInit() == GLFW_FALSE) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(960, 540, "someshitdll demo", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    const GLenum glew_status = glewInit();
    if (glew_status != GLEW_OK) {
        std::fprintf(stderr, "glewInit failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    HWND hwnd = glfwGetWin32Window(window);

    SomeshitDllConfig cfg{};
    cfg.enable_borderless = 1;
    cfg.enable_acrylic = 1;
    cfg.enable_quadrant_snap = 1;
    cfg.enable_super_intercept = 1;
    cfg.enable_super_move = 1;
    cfg.enable_super_resize = 1;
    cfg.enable_super_close = 1;
    cfg.enable_custom_cursors = 0;
    cfg.rotate_cursor_quadrants = 1;
    cfg.acrylic_tint_argb = 0xAA202020u;

    const int ok = SomeshitDllAttach(hwnd, &cfg);
    const DWORD err = GetLastError();
    std::printf("SomeshitDllAttach => %d (GetLastError=%lu)\n", ok, static_cast<unsigned long>(err));

    while (glfwWindowShouldClose(window) == GLFW_FALSE) {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(window, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);

        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(fbw), static_cast<double>(fbh), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        const float t = static_cast<float>(glfwGetTime());
        // Clear fully transparent so the OS acrylic/backdrop can show through.
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Tinted acrylic "filter" overlay (client-side).
        const float pulse = 0.5f + 0.5f * std::sin(t * 0.7f);
        const float a = 0.14f + 0.08f * pulse;
        glBegin(GL_QUADS);
        glColor4f(0.08f, 0.11f, 0.16f, a);
        glVertex2f(0.0f, 0.0f);
        glColor4f(0.10f, 0.08f, 0.18f, a);
        glVertex2f(static_cast<float>(fbw), 0.0f);
        glColor4f(0.06f, 0.09f, 0.12f, a);
        glVertex2f(static_cast<float>(fbw), static_cast<float>(fbh));
        glColor4f(0.07f, 0.10f, 0.14f, a);
        glVertex2f(0.0f, static_cast<float>(fbh));
        glEnd();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
