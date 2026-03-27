#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "hil_shell_api.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

using HilRunShellFn = int (*)(const HilLaunchConfig*);
using HilSmokeTestFn = int (*)(HilEnvironmentInfo*);
using HilGetLastErrorFn = const char* (*)();

unsigned int vk_major(unsigned int version) {
    return version >> 22;
}

unsigned int vk_minor(unsigned int version) {
    return (version >> 12) & 0x3ffu;
}

unsigned int vk_patch(unsigned int version) {
    return version & 0xfffu;
}

template <typename T>
T load_symbol(HMODULE module, const char* name) {
    FARPROC raw = GetProcAddress(module, name);
    T function = nullptr;
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

int print_error(HMODULE module, const char* prefix) {
    const auto get_last_error = load_symbol<HilGetLastErrorFn>(module, "HilGetLastError");
    const char* message = get_last_error ? get_last_error() : "unknown error";
    std::fprintf(stderr, "%s: %s\n", prefix, message && message[0] ? message : "unknown error");
    return 1;
}

void print_help() {
    std::puts("hil_host.exe [--smoke-test] [--root PATH]");
    std::puts("  default: launch the Omarchy-inspired fullscreen shell");
    std::puts("  --smoke-test: verify the DLL and print runtime capabilities");
    std::puts("  --root PATH: mount and seed the pseudo root at PATH");
    std::puts("  if --root is omitted, the shell opens an in-WM display-manager style root selector");
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke_test = false;
    const char* root_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke-test") == 0) {
            smoke_test = true;
        } else if (std::strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
    }

    HMODULE module = LoadLibraryA("hil_shell.dll");
    if (!module) {
        std::fputs("Unable to load hil_shell.dll from the current directory.\n", stderr);
        return 1;
    }

    const auto run_shell = load_symbol<HilRunShellFn>(module, "HilRunShell");
    const auto smoke = load_symbol<HilSmokeTestFn>(module, "HilSmokeTest");
    if (!run_shell || !smoke) {
        std::fputs("hil_shell.dll is missing one or more exported entry points.\n", stderr);
        FreeLibrary(module);
        return 1;
    }

    int status = 0;
    if (smoke_test) {
        HilEnvironmentInfo info{};
        status = smoke(&info);
        if (status != 0) {
            status = print_error(module, "Smoke test failed");
        } else {
            std::printf("glfw %d.%d.%d\n", info.glfw_version_major, info.glfw_version_minor, info.glfw_version_rev);
            std::printf("primary monitor %dx%d @ %dhz\n", info.primary_monitor_width, info.primary_monitor_height, info.primary_monitor_refresh_hz);
            std::printf("dwm composition %s\n", info.dwm_composition_enabled ? "on" : "off");
            std::printf("opengl renderer %s\n", info.renderer_name);
            if (info.vulkan_runtime_available) {
                std::printf("vulkan runtime %u.%u.%u\n", vk_major(info.vulkan_api_version), vk_minor(info.vulkan_api_version), vk_patch(info.vulkan_api_version));
            } else {
                std::puts("vulkan runtime unavailable");
            }
        }
    } else {
        HilLaunchConfig config{};
        config.workspace_count = 5;
        config.enable_blur = 1;
        config.preferred_width = 0;
        config.preferred_height = 0;
        config.start_workspace = 0;
        config.capture_system_keys = 1;
        config.close_user_apps_first = 1;
        config.mount_root_path = root_path;

        status = run_shell(&config);
        if (status != 0) {
            status = print_error(module, "Shell failed");
        }
    }

    FreeLibrary(module);
    return status;
}
