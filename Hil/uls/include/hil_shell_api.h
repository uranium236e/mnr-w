#ifndef HIL_SHELL_API_H
#define HIL_SHELL_API_H

#ifdef _WIN32
#if defined(HIL_SHELL_BUILD)
#define HIL_SHELL_API extern "C" __declspec(dllexport)
#else
#define HIL_SHELL_API extern "C" __declspec(dllimport)
#endif
#else
#define HIL_SHELL_API extern "C"
#endif

#define HIL_MAX_RENDERER_NAME 128

struct HilLaunchConfig {
    int preferred_width;
    int preferred_height;
    int start_workspace;
    int workspace_count;
    int enable_blur;
    int capture_system_keys;
    int close_user_apps_first;
    const char* mount_root_path;
};

struct HilEnvironmentInfo {
    int glfw_version_major;
    int glfw_version_minor;
    int glfw_version_rev;
    int dwm_composition_enabled;
    int vulkan_runtime_available;
    unsigned int vulkan_api_version;
    int primary_monitor_width;
    int primary_monitor_height;
    int primary_monitor_refresh_hz;
    char renderer_name[HIL_MAX_RENDERER_NAME];
};

HIL_SHELL_API int HilRunShell(const HilLaunchConfig* config);
HIL_SHELL_API int HilSmokeTest(HilEnvironmentInfo* out_info);
HIL_SHELL_API const char* HilGetLastError();

#endif
