#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32

#include "../include/hil_shell_api.h"

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct Rect {
    float x;
    float y;
    float w;
    float h;
};

enum class ShellMode {
    RootPicker,
    Boot,
    Greeter,
    Desktop,
    Launcher,
    SystemMenu
};

enum class MenuAction {
    FocusTerminal,
    FocusEditor,
    FocusBrowser,
    FocusRoot,
    FocusToolchain,
    LockShell,
    RebuildRoot,
    ToggleMonocle,
    QuitShell
};

struct MenuEntry {
    std::string title;
    std::string detail;
    MenuAction action;
};

enum class FolderEntryKind {
    MountCurrent,
    Parent,
    Drive,
    Directory
};

struct FolderEntry {
    FolderEntryKind kind;
    std::string label;
    std::string detail;
    std::string path;
};

struct ClientPane {
    std::string key;
    std::string title;
    std::string subtitle;
    Color accent;
    int workspace;
    bool floating;
    Rect current;
    Rect target;
    Rect floating_rect;
};

struct AppState {
    GLFWwindow* window = nullptr;
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    int workspace_count = 5;
    int active_workspace = 0;
    int focused_client = -1;
    bool split_vertical = true;
    bool monocle = false;
    bool show_help = true;
    bool capture_system_keys = true;
    bool close_user_apps_first = true;
    ShellMode mode = ShellMode::Boot;
    double mode_started_at = 0.0;
    GLuint font_base = 0;
    float font_char_width = 9.4f;
    float font_line_height = 22.0f;
    std::array<unsigned char, GLFW_KEY_LAST + 1> key_locks{};
    HilEnvironmentInfo env{};
    std::vector<ClientPane> clients;
    std::vector<std::string> boot_lines;
    std::vector<MenuEntry> launcher_entries;
    std::vector<MenuEntry> system_entries;
    std::string mount_root_path;
    std::string session_user = "guest";
    std::string login_input;
    std::string toast;
    double toast_until = 0.0;
    int launcher_index = 0;
    int system_index = 0;
    int closed_app_count = 0;
    std::vector<std::string> closed_apps;
    std::string host_username;
    std::string shell_prompt;
    std::string shell_cwd;
    std::string vfs_cwd = "/home/guest";
    std::string browser_url;
    std::vector<std::string> terminal_lines;
    std::string terminal_input;
    std::string picker_path;
    std::vector<FolderEntry> picker_places;
    int picker_place_index = 0;
    bool picker_focus_places = true;
    std::vector<FolderEntry> picker_entries;
    int picker_index = 0;
    bool allow_exit = false;
};

std::string g_last_error;
HHOOK g_keyboard_hook = nullptr;
bool g_capture_system_keys = false;
bool g_super_down = false;
constexpr unsigned int kVulkan10 = (1u << 22);
constexpr float kApproxGlyphWidth = 9.4f;
using PFN_vkEnumerateInstanceVersion = long(__stdcall*)(unsigned int*);

bool terminal_accepts_input(const AppState& state);
std::string lower_copy(std::string value);
bool start_session_from_mount_root(AppState& state, bool close_user_apps_first);
bool terminate_visible_user_apps(AppState& state);
void seed_clients(AppState& state);
void setup_menus(AppState& state);
void switch_mode(AppState& state, ShellMode mode);

template <typename T>
T proc_as(FARPROC proc) {
    T function = nullptr;
    std::memcpy(&function, &proc, sizeof(function));
    return function;
}

void set_last_error(const std::string& message) {
    g_last_error = message;
}

Color rgba(float r, float g, float b, float a) {
    return Color{ r, g, b, a };
}

Rect make_rect(float x, float y, float w, float h) {
    return Rect{ x, y, w, h };
}

float lerp(float from, float to, float amount) {
    return from + (to - from) * amount;
}

unsigned int vk_major(unsigned int version) {
    return version >> 22;
}

unsigned int vk_minor(unsigned int version) {
    return (version >> 12) & 0x3ffu;
}

unsigned int vk_patch(unsigned int version) {
    return version & 0xfffu;
}

std::string compact_path(const std::string& path, std::size_t max_len) {
    if (path.size() <= max_len) {
        return path;
    }
    if (max_len < 10) {
        return path.substr(0, max_len);
    }
    return path.substr(0, max_len / 2) + "..." + path.substr(path.size() - max_len / 2 + 3);
}

std::size_t max_text_chars_for_width(const AppState& state, float width) {
    const float glyph_width = state.font_char_width > 1.0f ? state.font_char_width : kApproxGlyphWidth;
    if (width <= glyph_width) {
        return 1;
    }
    return static_cast<std::size_t>(std::floor(width / glyph_width));
}

std::string fit_text_to_width(const AppState& state, const std::string& text, float width) {
    return compact_path(text, max_text_chars_for_width(state, width));
}

std::vector<std::string> wrap_text_to_width(const AppState& state, const std::string& text, float width, std::size_t max_lines) {
    std::vector<std::string> lines;
    const std::size_t max_chars = std::max<std::size_t>(1, max_text_chars_for_width(state, width));
    if (text.empty()) {
        lines.push_back("");
        return lines;
    }

    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t line_end = std::min(cursor + max_chars, text.size());
        if (line_end < text.size()) {
            const std::size_t split = text.find_last_of(" /\\-_", line_end);
            if (split != std::string::npos && split > cursor + max_chars / 3) {
                line_end = split + 1;
            }
        }

        std::string line = text.substr(cursor, line_end - cursor);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())) != 0) {
            line.pop_back();
        }
        if (line.empty()) {
            line = text.substr(cursor, std::min(max_chars, text.size() - cursor));
            line_end = cursor + line.size();
        }
        lines.push_back(line);
        cursor = line_end;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }

        if (max_lines != 0 && lines.size() >= max_lines && cursor < text.size()) {
            lines.back() = compact_path(lines.back() + " " + text.substr(cursor), max_chars);
            break;
        }
    }

    if (lines.empty()) {
        lines.push_back("");
    }
    return lines;
}

std::string normalize_root_path(const char* raw_path) {
    std::string path = (raw_path && raw_path[0]) ? raw_path : "";
    std::replace(path.begin(), path.end(), '/', '\\');

    while (path.size() > 3 && !path.empty() && (path.back() == '\\' || path.back() == '/')) {
        path.pop_back();
    }

    return path;
}

std::string join_path(const std::string& base, const std::string& child) {
    if (base.empty()) {
        return child;
    }
    if (child.empty()) {
        return base;
    }
    if (base.back() == '\\' || base.back() == '/') {
        return base + child;
    }
    return base + "\\" + child;
}

bool ensure_directory_tree(const std::string& path) {
    const int result = SHCreateDirectoryExA(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_FILE_EXISTS || result == ERROR_ALREADY_EXISTS;
}

bool directory_exists(const std::string& path) {
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool write_text_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }

    stream << contents;
    return stream.good();
}

void copy_string(char* destination, std::size_t destination_size, const char* source) {
    if (!destination || destination_size == 0) {
        return;
    }
    std::snprintf(destination, destination_size, "%s", source ? source : "");
}

std::string wide_to_utf8(const wchar_t* value) {
    if (!value || !value[0]) {
        return "";
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return "";
    }

    std::vector<char> buffer(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer.data(), needed, nullptr, nullptr);
    return std::string(buffer.data());
}

std::string current_directory_utf8() {
    char buffer[MAX_PATH];
    const DWORD written = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(buffer)), buffer);
    if (written == 0 || written >= sizeof(buffer)) {
        return ".";
    }
    return std::string(buffer);
}

std::string environment_utf8(const char* name, const char* fallback) {
    char buffer[512];
    const DWORD written = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (written == 0 || written >= sizeof(buffer)) {
        return fallback ? std::string(fallback) : std::string();
    }
    return std::string(buffer);
}

std::string make_absolute_path(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    char buffer[MAX_PATH];
    const DWORD written = GetFullPathNameA(path.c_str(), static_cast<DWORD>(sizeof(buffer)), buffer, nullptr);
    if (written == 0 || written >= sizeof(buffer)) {
        return path;
    }
    return std::string(buffer);
}

std::string shellish_path(const std::string& absolute_path) {
    std::string normalized = absolute_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');

    std::string user_profile = environment_utf8("USERPROFILE", "");
    std::replace(user_profile.begin(), user_profile.end(), '\\', '/');
    if (!user_profile.empty() && normalized.rfind(user_profile, 0) == 0) {
        normalized.replace(0, user_profile.size(), "~");
    }
    return normalized;
}

std::string make_shell_prompt(const std::string& username, const std::string& cwd_path) {
    const char marker = username.empty() ? 'u' : static_cast<char>(std::tolower(static_cast<unsigned char>(username.front())));
    return "@" + std::string(1, marker) + " --" + (username.empty() ? std::string("user") : username) + " (" + shellish_path(cwd_path) + ") $";
}

std::string make_shell_prompt_virtual(const std::string& username, const std::string& vfs_path) {
    const char marker = username.empty() ? 'u' : static_cast<char>(std::tolower(static_cast<unsigned char>(username.front())));
    return "@" + std::string(1, marker) + " --" + (username.empty() ? std::string("user") : username) + " (" + vfs_path + ") $";
}

std::vector<std::string> split_command_line(const std::string& line) {
    std::vector<std::string> parts;
    std::string current;
    bool in_quotes = false;

    for (char ch : line) {
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string normalize_vfs_path(std::string path) {
    if (path.empty()) {
        return "/";
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }

    std::vector<std::string> segments;
    std::string segment;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        const char ch = i < path.size() ? path[i] : '/';
        if (ch == '/') {
            if (segment.empty() || segment == ".") {
                segment.clear();
                continue;
            }
            if (segment == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
                segment.clear();
                continue;
            }
            segments.push_back(segment);
            segment.clear();
            continue;
        }
        segment.push_back(ch);
    }

    std::string normalized = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        normalized += segments[i];
        if (i + 1 < segments.size()) {
            normalized.push_back('/');
        }
    }
    return normalized;
}

std::string join_vfs(const std::string& base, const std::string& child) {
    if (child.empty()) {
        return normalize_vfs_path(base);
    }
    if (!child.empty() && child.front() == '/') {
        return normalize_vfs_path(child);
    }
    std::string combined = base;
    if (combined.empty()) {
        combined = "/";
    }
    if (combined.back() != '/') {
        combined.push_back('/');
    }
    combined += child;
    return normalize_vfs_path(combined);
}

bool resolve_vfs_path(const AppState& state, const std::string& input, bool require_directory,
                      std::string* out_vfs_path, std::string* out_abs_path) {
    if (state.mount_root_path.empty() || !directory_exists(state.mount_root_path)) {
        return false;
    }

    const std::string vfs = normalize_vfs_path(join_vfs(state.vfs_cwd, input));
    std::string relative = vfs;
    if (!relative.empty() && relative.front() == '/') {
        relative.erase(relative.begin());
    }
    std::replace(relative.begin(), relative.end(), '/', '\\');

    const std::string abs = join_path(state.mount_root_path, relative);
    const DWORD attributes = GetFileAttributesA(abs.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (require_directory && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }

    if (out_vfs_path) {
        *out_vfs_path = vfs;
    }
    if (out_abs_path) {
        *out_abs_path = abs;
    }
    return true;
}

std::vector<std::string> list_directory_entries(const std::string& abs_dir) {
    std::vector<std::string> names;
    WIN32_FIND_DATAA find_data{};
    HANDLE find = FindFirstFileA(join_path(abs_dir, "*").c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        if (std::strcmp(find_data.cFileName, ".") == 0 || std::strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        std::string name = find_data.cFileName;
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            name.push_back('/');
        }
        names.push_back(name);
    } while (FindNextFileA(find, &find_data));
    FindClose(find);

    std::sort(names.begin(), names.end(), [](const std::string& left, const std::string& right) {
        return lower_copy(left) < lower_copy(right);
    });
    return names;
}

bool is_drive_root(const std::string& path) {
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
           (path[2] == '\\' || path[2] == '/') && path.find_first_not_of("\\/", 3) == std::string::npos;
}

std::string parent_directory(const std::string& path) {
    if (path.empty() || is_drive_root(path)) {
        return path;
    }

    std::size_t end = path.size();
    while (end > 0 && (path[end - 1] == '\\' || path[end - 1] == '/')) {
        --end;
    }
    const std::size_t slash = path.find_last_of("\\/", end - 1);
    if (slash == std::string::npos) {
        return path;
    }
    if (slash == 2 && path.size() >= 3 && path[1] == ':') {
        return path.substr(0, 3);
    }
    return path.substr(0, slash);
}

std::vector<std::string> logical_drives() {
    char buffer[512] = {};
    const DWORD written = GetLogicalDriveStringsA(static_cast<DWORD>(sizeof(buffer)), buffer);
    std::vector<std::string> drives;
    if (written == 0 || written >= sizeof(buffer)) {
        return drives;
    }

    for (const char* cursor = buffer; *cursor != '\0'; cursor += std::strlen(cursor) + 1) {
        drives.push_back(std::string(cursor));
    }
    return drives;
}

void append_unique_folder_entry(std::vector<FolderEntry>& entries, std::set<std::string>& seen_paths,
                                FolderEntryKind kind, const std::string& label,
                                const std::string& detail, const std::string& path) {
    if (path.empty() || !directory_exists(path)) {
        return;
    }

    const std::string normalized_path = lower_copy(make_absolute_path(path));
    if (!seen_paths.insert(normalized_path).second) {
        return;
    }

    entries.push_back(FolderEntry{ kind, label, detail, make_absolute_path(path) });
}

void refresh_root_picker(AppState& state) {
    if (state.picker_path.empty() || !directory_exists(state.picker_path)) {
        state.picker_path = make_absolute_path(state.shell_cwd);
    }

    state.picker_places.clear();
    std::set<std::string> place_paths;
    append_unique_folder_entry(
        state.picker_places,
        place_paths,
        FolderEntryKind::MountCurrent,
        "Boot Current Folder",
        shellish_path(state.picker_path),
        state.picker_path);

    const std::string home = environment_utf8("USERPROFILE", "");
    append_unique_folder_entry(state.picker_places, place_paths, FolderEntryKind::Directory, "Home", shellish_path(home), home);
    append_unique_folder_entry(state.picker_places, place_paths, FolderEntryKind::Directory, "Desktop", "~/Desktop", join_path(home, "Desktop"));
    append_unique_folder_entry(state.picker_places, place_paths, FolderEntryKind::Directory, "Documents", "~/Documents", join_path(home, "Documents"));
    append_unique_folder_entry(state.picker_places, place_paths, FolderEntryKind::Directory, "Downloads", "~/Downloads", join_path(home, "Downloads"));
    append_unique_folder_entry(state.picker_places, place_paths, FolderEntryKind::Directory, "Workspace", shellish_path(state.shell_cwd), state.shell_cwd);
    for (const std::string& drive : logical_drives()) {
        append_unique_folder_entry(
            state.picker_places,
            place_paths,
            FolderEntryKind::Drive,
            "Drive " + drive.substr(0, 2),
            drive,
            drive);
    }

    state.picker_entries.clear();
    const std::string parent = parent_directory(state.picker_path);
    if (!parent.empty() && parent != state.picker_path) {
        state.picker_entries.push_back(FolderEntry{
            FolderEntryKind::Parent,
            "Parent Directory",
            shellish_path(parent),
            parent
        });
    }

    std::vector<std::string> directories;
    WIN32_FIND_DATAA find_data{};
    HANDLE find = FindFirstFileA(join_path(state.picker_path, "*").c_str(), &find_data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            if (std::strcmp(find_data.cFileName, ".") == 0 || std::strcmp(find_data.cFileName, "..") == 0) {
                continue;
            }
            directories.push_back(find_data.cFileName);
        } while (FindNextFileA(find, &find_data));
        FindClose(find);
    }

    std::sort(directories.begin(), directories.end(), [](const std::string& left, const std::string& right) {
        return lower_copy(left) < lower_copy(right);
    });

    for (const std::string& directory : directories) {
        state.picker_entries.push_back(FolderEntry{
            FolderEntryKind::Directory,
            directory,
            shellish_path(join_path(state.picker_path, directory)),
            join_path(state.picker_path, directory)
        });
    }

    state.picker_place_index = state.picker_places.empty()
        ? 0
        : std::clamp(state.picker_place_index, 0, static_cast<int>(state.picker_places.size()) - 1);
    state.picker_index = state.picker_entries.empty()
        ? 0
        : std::clamp(state.picker_index, 0, static_cast<int>(state.picker_entries.size()) - 1);
    if (state.picker_entries.empty()) {
        state.picker_focus_places = true;
    }
}

void current_time_strings(char* date_buffer, std::size_t date_size, char* time_buffer, std::size_t time_size) {
    SYSTEMTIME local{};
    GetLocalTime(&local);
    std::snprintf(date_buffer, date_size, "%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    std::snprintf(time_buffer, time_size, "%02u:%02u", local.wHour, local.wMinute);
}

LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM w_param, LPARAM l_param) {
    if (code == HC_ACTION && g_capture_system_keys) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
        const bool key_down = (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN);
        const bool key_up = (w_param == WM_KEYUP || w_param == WM_SYSKEYUP);

        if (info->vkCode == VK_LWIN || info->vkCode == VK_RWIN) {
            if (key_down) {
                g_super_down = true;
            }
            if (key_up) {
                g_super_down = false;
            }
            return 1;
        }

        if (key_down) {
            const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            const bool ctrl_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (alt_down && (info->vkCode == VK_TAB || info->vkCode == VK_ESCAPE)) {
                return 1;
            }
            if (alt_down && info->vkCode == VK_F4) {
                return 1;
            }
            if (ctrl_down && info->vkCode == VK_ESCAPE) {
                return 1;
            }
        }
    }

    return CallNextHookEx(g_keyboard_hook, code, w_param, l_param);
}

bool install_keyboard_capture(bool enable) {
    g_capture_system_keys = enable;
    g_super_down = false;
    if (!enable) {
        return true;
    }

    g_keyboard_hook = SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandleA(nullptr), 0);
    return g_keyboard_hook != nullptr;
}

void uninstall_keyboard_capture() {
    if (g_keyboard_hook) {
        UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
    g_capture_system_keys = false;
    g_super_down = false;
}

bool probe_vulkan_runtime(unsigned int* out_version) {
    if (out_version) {
        *out_version = 0;
    }

    HMODULE module = LoadLibraryW(L"vulkan-1.dll");
    if (!module) {
        return false;
    }

    unsigned int version = kVulkan10;
    const auto enumerate_instance_version = proc_as<PFN_vkEnumerateInstanceVersion>(
        GetProcAddress(module, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version) {
        unsigned int detected_version = 0;
        if (enumerate_instance_version(&detected_version) == 0) {
            version = detected_version;
        }
    }

    FreeLibrary(module);

    if (out_version) {
        *out_version = version;
    }
    return true;
}

void gather_environment_info(HilEnvironmentInfo* out_info) {
    if (!out_info) {
        return;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    glfwGetVersion(
        &out_info->glfw_version_major,
        &out_info->glfw_version_minor,
        &out_info->glfw_version_rev);

    BOOL composition_enabled = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&composition_enabled))) {
        out_info->dwm_composition_enabled = composition_enabled ? 1 : 0;
    }

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if (mode) {
            out_info->primary_monitor_width = mode->width;
            out_info->primary_monitor_height = mode->height;
            out_info->primary_monitor_refresh_hz = mode->refreshRate;
        }
    }

    out_info->vulkan_runtime_available = probe_vulkan_runtime(&out_info->vulkan_api_version) ? 1 : 0;
}

bool key_pressed_once(AppState& state, int key) {
    if (key < 0 || key > GLFW_KEY_LAST) {
        return false;
    }

    const int current_state = glfwGetKey(state.window, key);
    if (current_state == GLFW_PRESS) {
        if (!state.key_locks[static_cast<std::size_t>(key)]) {
            state.key_locks[static_cast<std::size_t>(key)] = 1;
            return true;
        }
    } else {
        state.key_locks[static_cast<std::size_t>(key)] = 0;
    }

    return false;
}

bool super_combo_pressed_once(AppState& state, int key, bool require_alt = false, bool require_shift = false) {
    const bool alt_down = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!g_super_down) {
        return false;
    }
    if (require_alt != alt_down) {
        return false;
    }
    if (require_shift && !shift_down) {
        return false;
    }
    if (!require_shift && shift_down && key != GLFW_KEY_TAB) {
        return false;
    }
    return key_pressed_once(state, key);
}

void set_toast(AppState& state, const std::string& message) {
    state.toast = message;
    state.toast_until = glfwGetTime() + 2.8;
}

void switch_mode(AppState& state, ShellMode mode) {
    state.mode = mode;
    state.mode_started_at = glfwGetTime();
    state.launcher_index = 0;
    state.system_index = 0;
}

bool initialize_mount_root(AppState& state) {
    std::vector<std::string> directories = {
        "bin", "dev", "etc", "home", "home\\guest", "mnt", "opt", "proc", "root",
        "run", "srv", "tmp", "toolchain", "toolchain\\omcc", "usr", "usr\\bin",
        "usr\\include", "usr\\lib", "var", "var\\log", "workspace"
    };

    for (const std::string& relative : directories) {
        if (!ensure_directory_tree(join_path(state.mount_root_path, relative))) {
            set_last_error("Unable to create mount-root directory: " + relative);
            return false;
        }
    }

    const std::string os_release =
        "NAME=\"Hil Omarchy Shell\"\n"
        "ID=hil\n"
        "PRETTY_NAME=\"Hil Omarchy Shell\"\n"
        "VARIANT=\"mounted-root session\"\n";
    const std::string motd =
        "welcome to hil shell\n"
        "mounted root accepted\n"
        "omarchy-inspired session is ready\n";
    const std::string profile =
        "export ROOT=/\n"
        "export SHELL=hsh\n"
        "alias cc=/usr/bin/cc\n";
    const std::string readme =
        "This is a Windows-hosted pseudo root.\n"
        "The compositor is the fullscreen shell, not a real Linux desktop stack.\n"
        "Use /workspace/hello.c as the starter project.\n";
    const std::string hello_c =
        "#include <stdio.h>\n\n"
        "int main(void) {\n"
        "    puts(\"hello from the mounted root\");\n"
        "    return 0;\n"
        "}\n";
    const std::string cc_cmd =
        "@echo off\r\n"
        "set GCC=C:\\msys64\\mingw64\\bin\\gcc.exe\r\n"
        "if exist \"%GCC%\" (\r\n"
        "  \"%GCC%\" %*\r\n"
        ") else (\r\n"
        "  echo gcc shim failed: %%GCC%% not found\r\n"
        "  exit /b 1\r\n"
        ")\r\n";
    const std::string toolchain_readme =
        "omcc toolchain notes\n"
        "- /usr/bin/cc.cmd forwards to the host MSYS2 gcc\n"
        "- headers live under /usr/include\n"
        "- build output is still handled by Windows tools\n";

    struct FileSpec {
        const char* relative;
        const char* contents;
    };

    const FileSpec files[] = {
        { "etc\\os-release", os_release.c_str() },
        { "etc\\motd", motd.c_str() },
        { "home\\guest\\.shellrc", profile.c_str() },
        { "home\\guest\\README.txt", readme.c_str() },
        { "workspace\\hello.c", hello_c.c_str() },
        { "usr\\bin\\cc.cmd", cc_cmd.c_str() },
        { "toolchain\\omcc\\README.txt", toolchain_readme.c_str() }
    };

    for (const FileSpec& file : files) {
        if (!write_text_file(join_path(state.mount_root_path, file.relative), file.contents)) {
            set_last_error("Unable to write mount-root file: " + std::string(file.relative));
            return false;
        }
    }

    state.boot_lines = {
        "root accepted :: " + compact_path(state.mount_root_path, 58),
        "seeded /usr /etc /home /workspace",
        "installed cc shim at /usr/bin/cc.cmd",
        "warming compositor, launcher, and control menu"
    };

    return true;
}

bool start_session_from_mount_root(AppState& state, bool close_user_apps_first) {
    if (state.mount_root_path.empty()) {
        set_last_error("No mount-root folder is selected.");
        return false;
    }

    state.mount_root_path = make_absolute_path(state.mount_root_path);
    state.browser_url = "file:///" + shellish_path(state.mount_root_path) + "/home/guest/README.txt";
    state.terminal_lines = {
        "mounted root :: " + compact_path(state.mount_root_path, 54),
        "browser, editor, and toolchain panes ready",
        "type `shutdown` to exit the shell"
    };
    state.terminal_input.clear();
    state.boot_lines.clear();

    if (!initialize_mount_root(state)) {
        return false;
    }

    if (close_user_apps_first) {
        terminate_visible_user_apps(state);
    }

    seed_clients(state);
    setup_menus(state);
    switch_mode(state, ShellMode::Boot);
    return true;
}

struct ProcessInfo {
    DWORD pid;
    DWORD parent_pid;
    std::string exe_name;
};

struct WindowInfo {
    DWORD pid;
    HWND hwnd;
};

BOOL CALLBACK enum_visible_windows_proc(HWND hwnd, LPARAM l_param) {
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    const LONG_PTR ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return TRUE;
    }

    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(l_param);
    windows->push_back(WindowInfo{ pid, hwnd });
    return TRUE;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_protected_process_name(const std::string& exe_name) {
    static const char* const kProtectedNames[] = {
        "system",
        "registry",
        "smss.exe",
        "csrss.exe",
        "wininit.exe",
        "services.exe",
        "lsass.exe",
        "svchost.exe",
        "fontdrvhost.exe",
        "winlogon.exe",
        "dwm.exe",
        "sihost.exe",
        "taskhostw.exe",
        "ctfmon.exe",
        "conhost.exe",
        "explorer.exe",
        "shellexperiencehost.exe",
        "startmenuexperiencehost.exe",
        "searchhost.exe",
        "searchapp.exe",
        "textinputhost.exe",
        "applicationframehost.exe",
        "lockapp.exe",
        "widgets.exe",
        "powershell.exe",
        "pwsh.exe",
        "cmd.exe",
        "windowsterminal.exe"
    };

    for (const char* protected_name : kProtectedNames) {
        if (exe_name == protected_name) {
            return true;
        }
    }
    return false;
}

std::vector<ProcessInfo> snapshot_processes() {
    std::vector<ProcessInfo> processes;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            processes.push_back(ProcessInfo{
                entry.th32ProcessID,
                entry.th32ParentProcessID,
                lower_copy(wide_to_utf8(entry.szExeFile))
            });
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

const ProcessInfo* find_process_info(const std::vector<ProcessInfo>& processes, DWORD pid) {
    for (const ProcessInfo& process : processes) {
        if (process.pid == pid) {
            return &process;
        }
    }
    return nullptr;
}

std::set<DWORD> protected_process_chain(const std::vector<ProcessInfo>& processes) {
    std::set<DWORD> protected_pids;
    DWORD pid = GetCurrentProcessId();

    while (pid != 0) {
        if (!protected_pids.insert(pid).second) {
            break;
        }
        const ProcessInfo* info = find_process_info(processes, pid);
        if (!info || info->parent_pid == pid) {
            break;
        }
        pid = info->parent_pid;
    }

    return protected_pids;
}

std::vector<HWND> windows_for_pid(const std::vector<WindowInfo>& windows, DWORD pid) {
    std::vector<HWND> matches;
    for (const WindowInfo& window : windows) {
        if (window.pid == pid && IsWindow(window.hwnd)) {
            matches.push_back(window.hwnd);
        }
    }
    return matches;
}

std::string query_process_image_path(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return "";
    }

    std::vector<char> buffer(2048, '\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL ok = QueryFullProcessImageNameA(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!ok || size == 0) {
        return "";
    }
    return lower_copy(std::string(buffer.data(), size));
}

std::string windows_directory_path() {
    char buffer[MAX_PATH] = {};
    const UINT size = GetWindowsDirectoryA(buffer, static_cast<UINT>(sizeof(buffer)));
    if (size == 0 || size >= sizeof(buffer)) {
        return "";
    }
    return lower_copy(std::string(buffer, size));
}

bool path_is_within_root(const std::string& path, const std::string& root) {
    if (path.empty() || root.empty()) {
        return false;
    }

    std::string normalized_path = lower_copy(path);
    std::string normalized_root = lower_copy(root);
    if (!normalized_root.empty() && normalized_root.back() != '\\' && normalized_root.back() != '/') {
        normalized_root.push_back('\\');
    }

    return normalized_path == lower_copy(root) || normalized_path.rfind(normalized_root, 0) == 0;
}

bool terminate_visible_user_apps(AppState& state) {
    const std::vector<ProcessInfo> processes = snapshot_processes();
    const std::set<DWORD> protected_pids = protected_process_chain(processes);

    std::vector<WindowInfo> windows;
    EnumWindows(enum_visible_windows_proc, reinterpret_cast<LPARAM>(&windows));

    DWORD current_session_id = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id);

    const std::string windows_dir = windows_directory_path();
    std::set<DWORD> candidate_pids;
    for (const WindowInfo& window : windows) {
        candidate_pids.insert(window.pid);
    }

    for (const ProcessInfo& process : processes) {
        if (process.pid == 0 || protected_pids.count(process.pid) != 0) {
            continue;
        }
        if (is_protected_process_name(process.exe_name)) {
            continue;
        }

        DWORD process_session_id = 0;
        if (!ProcessIdToSessionId(process.pid, &process_session_id) || process_session_id != current_session_id) {
            continue;
        }

        if (candidate_pids.count(process.pid) != 0) {
            continue;
        }

        const std::string image_path = query_process_image_path(process.pid);
        if (image_path.empty() || path_is_within_root(image_path, windows_dir)) {
            continue;
        }

        candidate_pids.insert(process.pid);
    }

    state.closed_app_count = 0;
    state.closed_apps.clear();

    for (DWORD pid : candidate_pids) {
        if (protected_pids.count(pid) != 0) {
            continue;
        }

        const ProcessInfo* info = find_process_info(processes, pid);
        if (!info) {
            continue;
        }
        if (is_protected_process_name(info->exe_name)) {
            continue;
        }

        DWORD process_session_id = 0;
        if (!ProcessIdToSessionId(pid, &process_session_id) || process_session_id != current_session_id) {
            continue;
        }

        const std::vector<HWND> target_windows = windows_for_pid(windows, pid);
        if (target_windows.empty()) {
            const std::string image_path = query_process_image_path(pid);
            if (image_path.empty() || path_is_within_root(image_path, windows_dir)) {
                continue;
            }
        }

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (!process) {
            continue;
        }

        for (HWND hwnd : target_windows) {
            DWORD_PTR ignored = 0;
            SendMessageTimeoutA(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 700, &ignored);
        }

        DWORD wait_status = WaitForSingleObject(process, target_windows.empty() ? 300 : 1800);
        bool forced = false;
        if (wait_status == WAIT_TIMEOUT) {
            if (TerminateProcess(process, 0)) {
                forced = true;
                wait_status = WaitForSingleObject(process, 1800);
            }
        }

        if (wait_status == WAIT_OBJECT_0) {
            ++state.closed_app_count;
            std::string label = info->exe_name;
            if (forced) {
                label += " (forced)";
            }
            state.closed_apps.push_back(label);
        }

        CloseHandle(process);
    }

    if (state.closed_app_count == 0) {
        state.boot_lines.insert(state.boot_lines.begin(), "prelaunch cleanup :: no disposable user apps found");
    } else {
        state.boot_lines.insert(state.boot_lines.begin(), "prelaunch cleanup :: closed " + std::to_string(state.closed_app_count) + " user app(s)");
        if (!state.closed_apps.empty()) {
            std::string sample = state.closed_apps.front();
            for (std::size_t i = 1; i < state.closed_apps.size() && i < 3; ++i) {
                sample += ", " + state.closed_apps[i];
            }
            if (state.closed_apps.size() > 3) {
                sample += ", ...";
            }
            state.boot_lines.insert(state.boot_lines.begin() + 1, "targets :: " + sample);
        }
    }

    return true;
}

void draw_quad(const Rect& rect, const Color& color) {
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(rect.x, rect.y);
    glVertex2f(rect.x + rect.w, rect.y);
    glVertex2f(rect.x + rect.w, rect.y + rect.h);
    glVertex2f(rect.x, rect.y + rect.h);
    glEnd();
}

void draw_outline(const Rect& rect, const Color& color, float width) {
    glColor4f(color.r, color.g, color.b, color.a);
    glLineWidth(width);
    glBegin(GL_LINE_LOOP);
    glVertex2f(rect.x, rect.y);
    glVertex2f(rect.x + rect.w, rect.y);
    glVertex2f(rect.x + rect.w, rect.y + rect.h);
    glVertex2f(rect.x, rect.y + rect.h);
    glEnd();
}

void draw_text(const AppState& state, float x, float y, const Color& color, const std::string& text) {
    if (!state.font_base || text.empty()) {
        return;
    }

    glColor4f(color.r, color.g, color.b, color.a);
    glRasterPos2f(x, y);
    glListBase(state.font_base - 32u);
    glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE, text.c_str());
}

bool create_bitmap_font(AppState& state) {
    HDC dc = wglGetCurrentDC();
    if (!dc) {
        set_last_error("Unable to access the current device context.");
        return false;
    }

    state.font_base = glGenLists(96);
    if (!state.font_base) {
        set_last_error("Unable to allocate OpenGL font display lists.");
        return false;
    }

    HFONT font = CreateFontA(
        -18,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_TT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        FF_DONTCARE,
        "Consolas");
    if (!font) {
        set_last_error("Unable to create the shell UI font.");
        return false;
    }

    HGDIOBJ old_font = SelectObject(dc, font);

    TEXTMETRICA metrics{};
    if (GetTextMetricsA(dc, &metrics)) {
        state.font_line_height = static_cast<float>(metrics.tmHeight + metrics.tmExternalLeading);
    }
    SIZE sample_size{};
    if (GetTextExtentPoint32A(dc, "MMMMMMMMMM", 10, &sample_size) && sample_size.cx > 0) {
        state.font_char_width = static_cast<float>(sample_size.cx) / 10.0f;
    }

    const BOOL ok = wglUseFontBitmapsA(dc, 32, 96, state.font_base);
    SelectObject(dc, old_font);
    DeleteObject(font);

    if (!ok) {
        set_last_error("Unable to bake text glyphs into the OpenGL context.");
        return false;
    }

    return true;
}

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) {
        return;
    }

    state->width = width;
    state->height = height;
}

void window_close_callback(GLFWwindow* window) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }

    if (!state->allow_exit) {
        // Ignore Alt+F4 / window close; exit only via `shutdown` or explicit system menu action.
        glfwSetWindowShouldClose(window, GLFW_FALSE);
        set_toast(*state, "use shutdown / omarchy menu to exit");
        return;
    }

    glfwSetWindowShouldClose(window, GLFW_TRUE);
}

void char_callback(GLFWwindow* window, unsigned int codepoint) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) {
        return;
    }
    if (codepoint < 32 || codepoint > 126) {
        return;
    }

    if (state->mode == ShellMode::Greeter) {
        if (state->login_input.size() < 24) {
            state->login_input.push_back(static_cast<char>(codepoint));
        }
        return;
    }

    if (terminal_accepts_input(*state) && state->terminal_input.size() < 64) {
        state->terminal_input.push_back(static_cast<char>(codepoint));
    }
}

void apply_dwm_style(HWND hwnd, bool enable_blur) {
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    LONG_PTR ex_style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex_style |= WS_EX_APPWINDOW;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style);

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    if (enable_blur) {
        DWM_BLURBEHIND blur{};
        blur.dwFlags = DWM_BB_ENABLE;
        blur.fEnable = TRUE;
        blur.hRgnBlur = nullptr;
        DwmEnableBlurBehindWindow(hwnd, &blur);
    }
}

bool init_glfw() {
    if (glfwInit() == GLFW_FALSE) {
        set_last_error("GLFW failed to initialize.");
        return false;
    }
    return true;
}

void configure_window_hints(bool hidden) {
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, hidden ? GLFW_FALSE : GLFW_TRUE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
}

bool init_gl_context(GLFWwindow* window, HilEnvironmentInfo* out_info) {
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glewExperimental = GL_TRUE;
    const GLenum glew_status = glewInit();
    glGetError();
    if (glew_status != GLEW_OK) {
        set_last_error(reinterpret_cast<const char*>(glewGetErrorString(glew_status)));
        return false;
    }

    if (out_info) {
        gather_environment_info(out_info);
        copy_string(
            out_info->renderer_name,
            sizeof(out_info->renderer_name),
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    }

    return true;
}

void seed_clients(AppState& state) {
    const int span = std::max(1, state.workspace_count);
    const std::string root_label = compact_path(state.mount_root_path, 42);

    state.clients = {
        { "tty0", "tty0", state.shell_prompt, rgba(0.94f, 0.51f, 0.41f, 1.0f), 0 % span, false, {}, {}, make_rect(0.56f, 0.16f, 0.28f, 0.30f) },
        { "editor", "/workspace/hello.c", "starter project from mounted root", rgba(0.26f, 0.77f, 0.70f, 1.0f), 0 % span, false, {}, {}, make_rect(0.58f, 0.18f, 0.28f, 0.32f) },
        { "walker", "walker", "launcher overlay and command search", rgba(0.96f, 0.76f, 0.33f, 1.0f), 0 % span, true, {}, {}, make_rect(0.54f, 0.15f, 0.30f, 0.26f) },
        { "toolchain", "/usr/bin/cc", "gcc shim inside pseudo root", rgba(0.40f, 0.63f, 0.95f, 1.0f), 1 % span, false, {}, {}, make_rect(0.54f, 0.18f, 0.31f, 0.35f) },
        { "rootfs", "rootfs", root_label, rgba(0.58f, 0.88f, 0.42f, 1.0f), 1 % span, true, {}, {}, make_rect(0.51f, 0.16f, 0.33f, 0.38f) },
        { "browser", "browser", state.browser_url, rgba(0.84f, 0.50f, 0.94f, 1.0f), 2 % span, false, {}, {}, make_rect(0.57f, 0.20f, 0.28f, 0.33f) },
        { "logs", "journalctl", "/var/log boot trace", rgba(0.96f, 0.61f, 0.21f, 1.0f), 2 % span, false, {}, {}, make_rect(0.60f, 0.20f, 0.27f, 0.31f) },
        { "menu", "omarchy menu", "style, lock, rebuild root, quit", rgba(0.31f, 0.74f, 0.93f, 1.0f), 3 % span, true, {}, {}, make_rect(0.56f, 0.18f, 0.28f, 0.28f) }
    };

    state.focused_client = state.clients.empty() ? -1 : 0;
}

std::vector<int> visible_clients(const AppState& state) {
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(state.clients.size()); ++i) {
        if (state.clients[static_cast<std::size_t>(i)].workspace == state.active_workspace) {
            indices.push_back(i);
        }
    }
    return indices;
}

void clamp_focus(AppState& state) {
    const std::vector<int> indices = visible_clients(state);
    if (indices.empty()) {
        state.focused_client = -1;
        return;
    }

    if (std::find(indices.begin(), indices.end(), state.focused_client) == indices.end()) {
        state.focused_client = indices.front();
    }
}

void focus_relative(AppState& state, int delta) {
    const std::vector<int> indices = visible_clients(state);
    if (indices.empty()) {
        state.focused_client = -1;
        return;
    }

    auto current = std::find(indices.begin(), indices.end(), state.focused_client);
    int position = 0;
    if (current != indices.end()) {
        position = static_cast<int>(current - indices.begin());
    }

    position += delta;
    if (position < 0) {
        position = static_cast<int>(indices.size()) - 1;
    }
    if (position >= static_cast<int>(indices.size())) {
        position = 0;
    }

    state.focused_client = indices[static_cast<std::size_t>(position)];
}

int find_client(const AppState& state, const std::string& key) {
    for (int i = 0; i < static_cast<int>(state.clients.size()); ++i) {
        if (state.clients[static_cast<std::size_t>(i)].key == key) {
            return i;
        }
    }
    return -1;
}

void focus_client_by_key(AppState& state, const std::string& key) {
    const int index = find_client(state, key);
    if (index >= 0) {
        state.focused_client = index;
        state.active_workspace = state.clients[static_cast<std::size_t>(index)].workspace;
    }
}

bool terminal_accepts_input(const AppState& state) {
    if (state.mode != ShellMode::Desktop) {
        return false;
    }
    if (state.focused_client < 0 || state.focused_client >= static_cast<int>(state.clients.size())) {
        return false;
    }
    return state.clients[static_cast<std::size_t>(state.focused_client)].key == "tty0";
}

std::string trim_copy(const std::string& value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

void push_terminal_line(AppState& state, const std::string& line) {
    state.terminal_lines.push_back(line);
    if (state.terminal_lines.size() > 240) {
        state.terminal_lines.erase(state.terminal_lines.begin(), state.terminal_lines.begin() + (state.terminal_lines.size() - 240));
    }
}

void execute_terminal_command(AppState& state) {
    const std::string line = trim_copy(state.terminal_input);
    push_terminal_line(state, state.shell_prompt + (line.empty() ? "" : " " + line));

    if (line.empty()) {
        state.terminal_input.clear();
        return;
    }

    const std::vector<std::string> argv = split_command_line(line);
    const std::string cmd = argv.empty() ? "" : lower_copy(argv.front());

    auto arg = [&](std::size_t index) -> std::string {
        if (index >= argv.size()) {
            return "";
        }
        return argv[index];
    };

    if (cmd == "help" || cmd == "?") {
        push_terminal_line(state, "commands: help, clear, pwd, cd, ls, cat, open, shutdown");
        push_terminal_line(state, "notes: paths are inside the mounted root (pseudo fs)");
        state.terminal_input.clear();
        return;
    }

    if (cmd == "clear") {
        state.terminal_lines.clear();
        state.terminal_input.clear();
        return;
    }

    if (cmd == "shutdown") {
        push_terminal_line(state, "shutting down shell...");
        set_toast(state, "shutdown");
        state.allow_exit = true;
        state.terminal_input.clear();
        glfwSetWindowShouldClose(state.window, GLFW_TRUE);
        return;
    }

    if (state.mount_root_path.empty()) {
        push_terminal_line(state, "error: no mounted root (select a root first)");
        state.terminal_input.clear();
        return;
    }

    if (cmd == "pwd") {
        push_terminal_line(state, state.vfs_cwd);
        state.terminal_input.clear();
        return;
    }

    if (cmd == "cd") {
        const std::string target = arg(1).empty() ? "/home/guest" : arg(1);
        std::string resolved_vfs;
        std::string resolved_abs;
        if (!resolve_vfs_path(state, target, true, &resolved_vfs, &resolved_abs)) {
            push_terminal_line(state, "cd: no such directory: " + target);
            state.terminal_input.clear();
            return;
        }
        state.vfs_cwd = resolved_vfs;
        state.shell_prompt = make_shell_prompt_virtual(state.host_username, state.vfs_cwd);
        state.terminal_input.clear();
        return;
    }

    if (cmd == "ls") {
        const std::string target = arg(1);
        std::string resolved_vfs;
        std::string resolved_abs;
        if (!resolve_vfs_path(state, target.empty() ? "." : target, true, &resolved_vfs, &resolved_abs)) {
            push_terminal_line(state, "ls: no such directory: " + (target.empty() ? "." : target));
            state.terminal_input.clear();
            return;
        }

        push_terminal_line(state, resolved_vfs + ":");
        const std::vector<std::string> entries = list_directory_entries(resolved_abs);
        if (entries.empty()) {
            push_terminal_line(state, "(empty)");
        } else {
            std::string row;
            for (const std::string& entry : entries) {
                if (!row.empty()) {
                    row += "  ";
                }
                row += entry;
                if (row.size() > 96) {
                    push_terminal_line(state, row);
                    row.clear();
                }
            }
            if (!row.empty()) {
                push_terminal_line(state, row);
            }
        }
        state.terminal_input.clear();
        return;
    }

    if (cmd == "cat") {
        const std::string target = arg(1);
        if (target.empty()) {
            push_terminal_line(state, "cat: missing file operand");
            state.terminal_input.clear();
            return;
        }

        std::string resolved_abs;
        if (!resolve_vfs_path(state, target, false, nullptr, &resolved_abs)) {
            push_terminal_line(state, "cat: no such file: " + target);
            state.terminal_input.clear();
            return;
        }

        std::ifstream stream(resolved_abs, std::ios::binary);
        if (!stream) {
            push_terminal_line(state, "cat: unable to open: " + target);
            state.terminal_input.clear();
            return;
        }

        std::string file_line;
        int emitted = 0;
        while (std::getline(stream, file_line)) {
            push_terminal_line(state, file_line);
            if (++emitted > 80) {
                push_terminal_line(state, "(output truncated)");
                break;
            }
        }
        state.terminal_input.clear();
        return;
    }

    if (cmd == "open") {
        const std::string target = arg(1);
        if (target.empty()) {
            push_terminal_line(state, "open: missing path");
            state.terminal_input.clear();
            return;
        }
        std::string resolved_abs;
        if (!resolve_vfs_path(state, target, false, nullptr, &resolved_abs)) {
            push_terminal_line(state, "open: no such path: " + target);
            state.terminal_input.clear();
            return;
        }
        state.browser_url = "file:///" + shellish_path(resolved_abs);
        push_terminal_line(state, "opened in browser pane: " + target);
        state.terminal_input.clear();
        return;
    }

    push_terminal_line(state, "unknown command: " + cmd);
    push_terminal_line(state, "try: help");
    state.terminal_input.clear();
}

void rotate_workspace_clients(AppState& state) {
    const std::vector<int> indices = visible_clients(state);
    if (indices.size() < 2) {
        return;
    }

    ClientPane front = state.clients[static_cast<std::size_t>(indices.front())];
    for (std::size_t i = 1; i < indices.size(); ++i) {
        state.clients[static_cast<std::size_t>(indices[i - 1])] =
            state.clients[static_cast<std::size_t>(indices[i])];
    }
    state.clients[static_cast<std::size_t>(indices.back())] = front;
    state.focused_client = indices.front();
}

void close_focused_client(AppState& state) {
    if (state.focused_client < 0 || state.focused_client >= static_cast<int>(state.clients.size())) {
        return;
    }

    const std::string key = state.clients[static_cast<std::size_t>(state.focused_client)].key;
    if (key == "tty0") {
        set_toast(state, "tty0 is pinned");
        return;
    }

    state.clients.erase(state.clients.begin() + state.focused_client);
    if (state.focused_client >= static_cast<int>(state.clients.size())) {
        state.focused_client = static_cast<int>(state.clients.size()) - 1;
    }
    clamp_focus(state);
    set_toast(state, "closed pane");
}

void toggle_focused_floating(AppState& state) {
    if (state.focused_client < 0 || state.focused_client >= static_cast<int>(state.clients.size())) {
        return;
    }
    auto& client = state.clients[static_cast<std::size_t>(state.focused_client)];
    client.floating = !client.floating;
}

void setup_menus(AppState& state) {
    state.launcher_entries = {
        { "Terminal", "focus tty0 and shell pane", MenuAction::FocusTerminal },
        { "Editor", "focus /workspace/hello.c", MenuAction::FocusEditor },
        { "Browser", "focus browser/doc pane", MenuAction::FocusBrowser },
        { "Mounted Root", "inspect the mounted pseudo root", MenuAction::FocusRoot },
        { "Toolchain", "jump to /usr/bin/cc", MenuAction::FocusToolchain }
    };

    state.system_entries = {
        { "Lock Shell", "return to the greeter", MenuAction::LockShell },
        { "Rebuild Root", "reseed /usr, /etc, /home, and /workspace", MenuAction::RebuildRoot },
        { "Toggle Monocle", "flip monocle layout mode", MenuAction::ToggleMonocle },
        { "Quit Shell", "close the compositor window", MenuAction::QuitShell }
    };
}

void compute_layout(AppState& state) {
    const float left_pad = 34.0f;
    const float top_pad = 86.0f;
    const float right_pad = 34.0f;
    const float bottom_pad = 42.0f;
    const float gap = 18.0f;

    clamp_focus(state);
    const std::vector<int> indices = visible_clients(state);
    if (indices.empty()) {
        return;
    }

    std::vector<int> tiled;
    std::vector<int> floating;
    for (int index : indices) {
        if (state.clients[static_cast<std::size_t>(index)].floating) {
            floating.push_back(index);
        } else {
            tiled.push_back(index);
        }
    }

    const float inner_x = left_pad;
    const float inner_y = top_pad;
    const float inner_w = std::max(240.0f, static_cast<float>(state.width) - left_pad - right_pad);
    const float inner_h = std::max(240.0f, static_cast<float>(state.height) - top_pad - bottom_pad);

    if (state.monocle && state.focused_client >= 0) {
        for (int index : indices) {
            auto& client = state.clients[static_cast<std::size_t>(index)];
            if (index == state.focused_client) {
                client.target = make_rect(inner_x, inner_y, inner_w, inner_h);
            } else {
                client.target = make_rect(inner_x + inner_w + 120.0f, inner_y, 0.0f, 0.0f);
            }
        }
    } else if (!tiled.empty()) {
        if (tiled.size() == 1) {
            state.clients[static_cast<std::size_t>(tiled.front())].target = make_rect(inner_x, inner_y, inner_w, inner_h);
        } else if (state.split_vertical) {
            const float master_w = std::floor(inner_w * 0.57f);
            const float stack_w = inner_w - master_w - gap;
            state.clients[static_cast<std::size_t>(tiled.front())].target = make_rect(inner_x, inner_y, master_w, inner_h);

            const std::size_t stack_count = tiled.size() - 1;
            const float stack_h = (inner_h - gap * static_cast<float>(stack_count - 1)) / static_cast<float>(stack_count);
            float cursor_y = inner_y;
            for (std::size_t i = 1; i < tiled.size(); ++i) {
                state.clients[static_cast<std::size_t>(tiled[i])].target =
                    make_rect(inner_x + master_w + gap, cursor_y, stack_w, stack_h);
                cursor_y += stack_h + gap;
            }
        } else {
            const float master_h = std::floor(inner_h * 0.56f);
            const float stack_h = inner_h - master_h - gap;
            state.clients[static_cast<std::size_t>(tiled.front())].target = make_rect(inner_x, inner_y, inner_w, master_h);

            const std::size_t stack_count = tiled.size() - 1;
            const float stack_w = (inner_w - gap * static_cast<float>(stack_count - 1)) / static_cast<float>(stack_count);
            float cursor_x = inner_x;
            for (std::size_t i = 1; i < tiled.size(); ++i) {
                state.clients[static_cast<std::size_t>(tiled[i])].target =
                    make_rect(cursor_x, inner_y + master_h + gap, stack_w, stack_h);
                cursor_x += stack_w + gap;
            }
        }
    }

    for (int index : floating) {
        auto& client = state.clients[static_cast<std::size_t>(index)];
        client.target = make_rect(
            inner_x + client.floating_rect.x * inner_w,
            inner_y + client.floating_rect.y * inner_h,
            std::max(180.0f, client.floating_rect.w * inner_w),
            std::max(120.0f, client.floating_rect.h * inner_h));
    }

    for (int index : indices) {
        auto& client = state.clients[static_cast<std::size_t>(index)];
        if (client.current.w <= 0.0f || client.current.h <= 0.0f) {
            client.current = client.target;
        }
    }
}

void animate_layout(AppState& state, float dt) {
    const float smoothing = 1.0f - std::exp(-dt * 10.0f);
    for (auto& client : state.clients) {
        client.current.x = lerp(client.current.x, client.target.x, smoothing);
        client.current.y = lerp(client.current.y, client.target.y, smoothing);
        client.current.w = lerp(client.current.w, client.target.w, smoothing);
        client.current.h = lerp(client.current.h, client.target.h, smoothing);
    }
}

void execute_menu_action(AppState& state, MenuAction action) {
    switch (action) {
    case MenuAction::FocusTerminal:
        focus_client_by_key(state, "tty0");
        set_toast(state, "focused tty0");
        break;
    case MenuAction::FocusEditor:
        focus_client_by_key(state, "editor");
        set_toast(state, "focused /workspace/hello.c");
        break;
    case MenuAction::FocusBrowser:
        focus_client_by_key(state, "browser");
        set_toast(state, "focused browser");
        break;
    case MenuAction::FocusRoot:
        focus_client_by_key(state, "rootfs");
        set_toast(state, "opened mounted root");
        break;
    case MenuAction::FocusToolchain:
        focus_client_by_key(state, "toolchain");
        set_toast(state, "opened /usr/bin/cc");
        break;
    case MenuAction::LockShell:
        switch_mode(state, ShellMode::Greeter);
        set_toast(state, "shell locked");
        break;
    case MenuAction::RebuildRoot:
        if (initialize_mount_root(state)) {
            set_toast(state, "root rebuilt");
        } else {
            set_toast(state, "root rebuild failed");
        }
        break;
    case MenuAction::ToggleMonocle:
        state.monocle = !state.monocle;
        set_toast(state, state.monocle ? "monocle on" : "monocle off");
        break;
    case MenuAction::QuitShell:
        state.allow_exit = true;
        glfwSetWindowShouldClose(state.window, GLFW_TRUE);
        break;
    }
}

bool activate_root_picker_entry(AppState& state, const FolderEntry& entry, bool close_user_apps_first) {
    if (entry.kind == FolderEntryKind::MountCurrent) {
        state.mount_root_path = state.picker_path;
        if (!start_session_from_mount_root(state, close_user_apps_first)) {
            set_toast(state, HilGetLastError());
            refresh_root_picker(state);
            return false;
        }
        return true;
    }

    state.picker_path = entry.path;
    refresh_root_picker(state);
    state.picker_focus_places = false;
    return true;
}

void handle_root_picker_input(AppState& state, bool close_user_apps_first) {
    if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
        set_toast(state, "use tab to boot or omarchy menu to exit");
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_LEFT) || key_pressed_once(state, GLFW_KEY_H)) {
        if (!state.picker_places.empty()) {
            state.picker_focus_places = true;
        } else {
            const std::string parent = parent_directory(state.picker_path);
            if (!parent.empty() && parent != state.picker_path) {
                state.picker_path = parent;
                refresh_root_picker(state);
            }
        }
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_RIGHT) || key_pressed_once(state, GLFW_KEY_L)) {
        if (!state.picker_entries.empty()) {
            state.picker_focus_places = false;
        }
        return;
    }

    auto move_selection = [&](int delta) {
        std::vector<FolderEntry>& active_entries = state.picker_focus_places ? state.picker_places : state.picker_entries;
        int& active_index = state.picker_focus_places ? state.picker_place_index : state.picker_index;
        if (!active_entries.empty()) {
            const int count = static_cast<int>(active_entries.size());
            active_index = (active_index + delta + count) % count;
        }
    };
    if (key_pressed_once(state, GLFW_KEY_DOWN) || key_pressed_once(state, GLFW_KEY_J)) {
        move_selection(1);
    }
    if (key_pressed_once(state, GLFW_KEY_UP) || key_pressed_once(state, GLFW_KEY_K)) {
        move_selection(-1);
    }
    if (key_pressed_once(state, GLFW_KEY_BACKSPACE)) {
        const std::string parent = parent_directory(state.picker_path);
        if (!parent.empty() && parent != state.picker_path) {
            state.picker_path = parent;
            refresh_root_picker(state);
        }
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_R)) {
        refresh_root_picker(state);
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_TAB)) {
        state.mount_root_path = state.picker_path;
        if (!start_session_from_mount_root(state, close_user_apps_first)) {
            set_toast(state, HilGetLastError());
            refresh_root_picker(state);
        }
        return;
    }
    if (!key_pressed_once(state, GLFW_KEY_ENTER)) {
        return;
    }

    if (state.picker_focus_places) {
        if (state.picker_places.empty()) {
            return;
        }
        activate_root_picker_entry(state, state.picker_places[static_cast<std::size_t>(state.picker_place_index)], close_user_apps_first);
        return;
    }

    if (state.picker_entries.empty()) {
        return;
    }

    activate_root_picker_entry(state, state.picker_entries[static_cast<std::size_t>(state.picker_index)], close_user_apps_first);
}

void handle_boot_input(AppState& state, double now) {
    if (now - state.mode_started_at > 1.8 || key_pressed_once(state, GLFW_KEY_ENTER)) {
        switch_mode(state, ShellMode::Greeter);
    }
}

void handle_greeter_input(AppState& state) {
    if (key_pressed_once(state, GLFW_KEY_BACKSPACE) && !state.login_input.empty()) {
        state.login_input.pop_back();
    }

    if (key_pressed_once(state, GLFW_KEY_ENTER)) {
        if (!state.login_input.empty()) {
            state.session_user = state.login_input;
        }
        switch_mode(state, ShellMode::Desktop);
        set_toast(state, "session started for " + state.session_user);
    }

    if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
        // Keep the session alive; ESC just backs out to root selection.
        refresh_root_picker(state);
        switch_mode(state, ShellMode::RootPicker);
    }
}

void handle_launcher_input(AppState& state) {
    if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
        switch_mode(state, ShellMode::Desktop);
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_DOWN) || key_pressed_once(state, GLFW_KEY_J)) {
        state.launcher_index = (state.launcher_index + 1) % static_cast<int>(state.launcher_entries.size());
    }
    if (key_pressed_once(state, GLFW_KEY_UP) || key_pressed_once(state, GLFW_KEY_K)) {
        state.launcher_index = (state.launcher_index + static_cast<int>(state.launcher_entries.size()) - 1) %
                               static_cast<int>(state.launcher_entries.size());
    }
    if (key_pressed_once(state, GLFW_KEY_ENTER)) {
        execute_menu_action(state, state.launcher_entries[static_cast<std::size_t>(state.launcher_index)].action);
        switch_mode(state, ShellMode::Desktop);
    }
}

void handle_system_menu_input(AppState& state) {
    if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
        switch_mode(state, ShellMode::Desktop);
        return;
    }
    if (key_pressed_once(state, GLFW_KEY_DOWN) || key_pressed_once(state, GLFW_KEY_J)) {
        state.system_index = (state.system_index + 1) % static_cast<int>(state.system_entries.size());
    }
    if (key_pressed_once(state, GLFW_KEY_UP) || key_pressed_once(state, GLFW_KEY_K)) {
        state.system_index = (state.system_index + static_cast<int>(state.system_entries.size()) - 1) %
                             static_cast<int>(state.system_entries.size());
    }
    if (key_pressed_once(state, GLFW_KEY_ENTER)) {
        execute_menu_action(state, state.system_entries[static_cast<std::size_t>(state.system_index)].action);
        if (state.mode == ShellMode::SystemMenu) {
            switch_mode(state, ShellMode::Desktop);
        }
    }
}

void handle_desktop_input(AppState& state) {
    if (super_combo_pressed_once(state, GLFW_KEY_SPACE, false, false) || super_combo_pressed_once(state, GLFW_KEY_D, false, false)) {
        switch_mode(state, ShellMode::Launcher);
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_SPACE, true, false)) {
        switch_mode(state, ShellMode::SystemMenu);
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_Q, false, false)) {
        close_focused_client(state);
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_Q, false, true)) {
        switch_mode(state, ShellMode::SystemMenu);
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_L, false, false)) {
        switch_mode(state, ShellMode::Greeter);
        set_toast(state, "locked");
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_R, false, true)) {
        if (initialize_mount_root(state)) {
            set_toast(state, "root rebuilt");
        } else {
            set_toast(state, "root rebuild failed");
        }
        return;
    }
    if (super_combo_pressed_once(state, GLFW_KEY_ENTER)) {
        focus_client_by_key(state, "tty0");
        set_toast(state, "tty0 focused");
    }
    if (super_combo_pressed_once(state, GLFW_KEY_E)) {
        focus_client_by_key(state, "editor");
        set_toast(state, "editor focused");
    }
    if (super_combo_pressed_once(state, GLFW_KEY_B)) {
        focus_client_by_key(state, "browser");
        set_toast(state, "browser focused");
    }
    if (super_combo_pressed_once(state, GLFW_KEY_T)) {
        focus_client_by_key(state, "toolchain");
        set_toast(state, "toolchain focused");
    }
    if (super_combo_pressed_once(state, GLFW_KEY_R)) {
        focus_client_by_key(state, "rootfs");
        set_toast(state, "mounted root focused");
    }

    if (terminal_accepts_input(state)) {
        if (key_pressed_once(state, GLFW_KEY_BACKSPACE) && !state.terminal_input.empty()) {
            state.terminal_input.pop_back();
        }
        if (key_pressed_once(state, GLFW_KEY_ENTER)) {
            execute_terminal_command(state);
        }
        if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
            focus_relative(state, 1);
            set_toast(state, "focus moved");
        }
        return;
    }

    if (key_pressed_once(state, GLFW_KEY_ESCAPE)) {
        // ESC should not quit; keep it as a no-op with feedback.
        set_toast(state, "esc disabled (use shutdown to exit)");
    }
    if (key_pressed_once(state, GLFW_KEY_Q)) {
        set_toast(state, "q disabled (use shutdown to exit)");
    }
    if (key_pressed_once(state, GLFW_KEY_TAB) || key_pressed_once(state, GLFW_KEY_L) || key_pressed_once(state, GLFW_KEY_RIGHT)) {
        focus_relative(state, 1);
    }
    if (key_pressed_once(state, GLFW_KEY_H) || key_pressed_once(state, GLFW_KEY_LEFT)) {
        focus_relative(state, -1);
    }
    if (key_pressed_once(state, GLFW_KEY_J) || key_pressed_once(state, GLFW_KEY_DOWN)) {
        focus_relative(state, 1);
    }
    if (key_pressed_once(state, GLFW_KEY_K) || key_pressed_once(state, GLFW_KEY_UP)) {
        focus_relative(state, -1);
    }
    if (key_pressed_once(state, GLFW_KEY_F)) {
        toggle_focused_floating(state);
    }
    if (key_pressed_once(state, GLFW_KEY_M)) {
        state.monocle = !state.monocle;
    }
    if (key_pressed_once(state, GLFW_KEY_S)) {
        state.split_vertical = !state.split_vertical;
    }
    if (key_pressed_once(state, GLFW_KEY_R)) {
        rotate_workspace_clients(state);
    }
    if (key_pressed_once(state, GLFW_KEY_G)) {
        state.show_help = !state.show_help;
    }

    for (int workspace = 0; workspace < std::min(9, state.workspace_count); ++workspace) {
        if (super_combo_pressed_once(state, GLFW_KEY_1 + workspace, false, false)) {
            state.active_workspace = workspace;
            clamp_focus(state);
        }
        if (super_combo_pressed_once(state, GLFW_KEY_1 + workspace, false, true)) {
            if (state.focused_client >= 0 && state.focused_client < static_cast<int>(state.clients.size())) {
                state.clients[static_cast<std::size_t>(state.focused_client)].workspace = workspace;
                state.active_workspace = workspace;
                clamp_focus(state);
                set_toast(state, "moved pane to workspace " + std::to_string(workspace + 1));
            }
        }
    }
}

void draw_background(const AppState& state, float tick, bool dimmed) {
    glBegin(GL_QUADS);
    glColor4f(0.03f, 0.05f, 0.08f, dimmed ? 0.94f : 0.88f);
    glVertex2f(0.0f, 0.0f);
    glColor4f(0.06f, 0.08f, 0.13f, dimmed ? 0.95f : 0.90f);
    glVertex2f(static_cast<float>(state.width), 0.0f);
    glColor4f(0.10f, 0.07f, 0.15f, dimmed ? 0.96f : 0.92f);
    glVertex2f(static_cast<float>(state.width), static_cast<float>(state.height));
    glColor4f(0.05f, 0.07f, 0.10f, dimmed ? 0.95f : 0.90f);
    glVertex2f(0.0f, static_cast<float>(state.height));
    glEnd();

    const float pulse = 0.5f + 0.5f * std::sin(tick * 0.6f);
    for (int i = 0; i < state.width; i += 64) {
        draw_quad(make_rect(static_cast<float>(i), 0.0f, 1.0f, static_cast<float>(state.height)),
                  rgba(1.0f, 1.0f, 1.0f, 0.02f + 0.03f * pulse));
    }
    for (int i = 0; i < state.height; i += 52) {
        draw_quad(make_rect(0.0f, static_cast<float>(i), static_cast<float>(state.width), 1.0f),
                  rgba(1.0f, 1.0f, 1.0f, 0.01f + 0.02f * pulse));
    }
}

void draw_top_bar(const AppState& state) {
    draw_quad(make_rect(20.0f, 18.0f, static_cast<float>(state.width) - 40.0f, 48.0f), rgba(0.07f, 0.09f, 0.13f, 0.86f));
    draw_outline(make_rect(20.0f, 18.0f, static_cast<float>(state.width) - 40.0f, 48.0f), rgba(0.22f, 0.28f, 0.38f, 0.95f), 1.0f);

    char date_buf[32];
    char time_buf[32];
    current_time_strings(date_buf, sizeof(date_buf), time_buf, sizeof(time_buf));

    draw_text(state, 38.0f, 49.0f, rgba(0.96f, 0.97f, 1.0f, 1.0f), "omarchy shell :: mounted root session");
    draw_text(state, static_cast<float>(state.width) - 200.0f, 49.0f, rgba(0.80f, 0.85f, 0.93f, 1.0f), time_buf);

    float cursor_x = 388.0f;
    for (int workspace = 0; workspace < state.workspace_count; ++workspace) {
        const bool active = workspace == state.active_workspace;
        const Rect badge = make_rect(cursor_x, 27.0f, 42.0f, 30.0f);
        draw_quad(badge, active ? rgba(0.22f, 0.49f, 0.95f, 0.92f) : rgba(0.11f, 0.14f, 0.20f, 0.88f));
        draw_outline(badge, active ? rgba(0.70f, 0.82f, 1.0f, 0.96f) : rgba(0.24f, 0.29f, 0.39f, 0.90f), 1.0f);
        char label[8];
        std::snprintf(label, sizeof(label), "%d", workspace + 1);
        draw_text(state, cursor_x + 15.0f, 47.0f, rgba(0.98f, 0.99f, 1.0f, 1.0f), label);
        cursor_x += 50.0f;
    }

    char runtime_line[256];
    if (state.env.vulkan_runtime_available) {
        std::snprintf(
            runtime_line,
            sizeof(runtime_line),
            "gl %s | vk %u.%u.%u | root %s",
            state.env.renderer_name,
            vk_major(state.env.vulkan_api_version),
            vk_minor(state.env.vulkan_api_version),
            vk_patch(state.env.vulkan_api_version),
            compact_path(state.mount_root_path, 22).c_str());
    } else {
        std::snprintf(
            runtime_line,
            sizeof(runtime_line),
            "gl %s | vk missing | root %s",
            state.env.renderer_name,
            compact_path(state.mount_root_path, 24).c_str());
    }
    draw_text(state, 38.0f, 76.0f, rgba(0.72f, 0.77f, 0.86f, 1.0f), runtime_line);
}

void draw_terminal_surface(const AppState& state, const ClientPane& client) {
    const Rect inner = make_rect(client.current.x + 10.0f, client.current.y + 40.0f, client.current.w - 20.0f, client.current.h - 50.0f);
    draw_quad(inner, rgba(0.03f, 0.05f, 0.07f, 0.96f));

    float y = inner.y + 24.0f;
    const std::size_t visible_line_count = state.terminal_lines.size() > 5 ? 5 : state.terminal_lines.size();
    const std::size_t start = state.terminal_lines.size() > visible_line_count ? state.terminal_lines.size() - visible_line_count : 0;

    for (std::size_t i = start; i < state.terminal_lines.size(); ++i) {
        const bool prompt_line = state.terminal_lines[i].rfind(state.shell_prompt, 0) == 0;
        draw_text(state, inner.x + 12.0f, y, prompt_line ? rgba(0.54f, 0.92f, 0.62f, 1.0f) : rgba(0.74f, 0.80f, 0.88f, 1.0f), state.terminal_lines[i]);
        y += 24.0f;
    }

    draw_text(state, inner.x + 12.0f, y + 4.0f, rgba(0.54f, 0.92f, 0.62f, 1.0f), state.shell_prompt + (state.terminal_input.empty() ? " _" : " " + state.terminal_input + "_"));
}

void draw_browser_surface(const AppState& state, const ClientPane& client) {
    const Rect toolbar = make_rect(client.current.x + 10.0f, client.current.y + 40.0f, client.current.w - 20.0f, 34.0f);
    draw_quad(toolbar, rgba(0.11f, 0.12f, 0.18f, 0.94f));

    draw_quad(make_rect(toolbar.x + 12.0f, toolbar.y + 10.0f, 10.0f, 10.0f), rgba(0.95f, 0.43f, 0.40f, 0.95f));
    draw_quad(make_rect(toolbar.x + 28.0f, toolbar.y + 10.0f, 10.0f, 10.0f), rgba(0.97f, 0.78f, 0.33f, 0.95f));
    draw_quad(make_rect(toolbar.x + 44.0f, toolbar.y + 10.0f, 10.0f, 10.0f), rgba(0.42f, 0.87f, 0.56f, 0.95f));

    const Rect address = make_rect(toolbar.x + 70.0f, toolbar.y + 6.0f, toolbar.w - 84.0f, 22.0f);
    draw_quad(address, rgba(0.16f, 0.18f, 0.26f, 0.96f));
    draw_outline(address, rgba(0.31f, 0.36f, 0.48f, 0.90f), 1.0f);
    draw_text(state, address.x + 10.0f, address.y + 16.0f, rgba(0.83f, 0.87f, 0.95f, 1.0f), state.browser_url);

    const Rect page = make_rect(client.current.x + 10.0f, client.current.y + 78.0f, client.current.w - 20.0f, client.current.h - 88.0f);
    draw_quad(page, rgba(0.94f, 0.95f, 0.98f, 0.98f));
    draw_text(state, page.x + 14.0f, page.y + 28.0f, rgba(0.14f, 0.16f, 0.21f, 1.0f), "Hil Browser");
    draw_text(state, page.x + 14.0f, page.y + 54.0f, rgba(0.28f, 0.31f, 0.39f, 1.0f), "Mounted root overview");
    draw_text(state, page.x + 14.0f, page.y + 84.0f, rgba(0.33f, 0.36f, 0.43f, 1.0f), ("root :: " + compact_path(state.mount_root_path, 38)).c_str());
    draw_text(state, page.x + 14.0f, page.y + 108.0f, rgba(0.33f, 0.36f, 0.43f, 1.0f), "docs :: /home/guest/README.txt");
    draw_text(state, page.x + 14.0f, page.y + 132.0f, rgba(0.33f, 0.36f, 0.43f, 1.0f), "toolchain :: /usr/bin/cc.cmd");
}

void draw_root_surface(const AppState& state, const ClientPane& client) {
    const Rect inner = make_rect(client.current.x + 10.0f, client.current.y + 40.0f, client.current.w - 20.0f, client.current.h - 50.0f);
    draw_quad(inner, rgba(0.08f, 0.11f, 0.09f, 0.94f));
    draw_text(state, inner.x + 12.0f, inner.y + 24.0f, rgba(0.86f, 0.95f, 0.89f, 1.0f), compact_path(state.mount_root_path, 38));
    draw_text(state, inner.x + 12.0f, inner.y + 50.0f, rgba(0.72f, 0.83f, 0.74f, 1.0f), "bin  etc  home  usr  var  workspace");
    draw_text(state, inner.x + 12.0f, inner.y + 74.0f, rgba(0.72f, 0.83f, 0.74f, 1.0f), "toolchain  tmp  proc  opt  mnt");
}

void draw_client(const AppState& state, const ClientPane& client, bool focused) {
    if (client.current.w <= 0.0f || client.current.h <= 0.0f) {
        return;
    }

    draw_quad(make_rect(client.current.x + 8.0f, client.current.y + 10.0f, client.current.w, client.current.h),
              rgba(0.0f, 0.0f, 0.0f, focused ? 0.28f : 0.18f));
    draw_quad(client.current, rgba(0.08f, 0.10f, 0.14f, 0.88f));
    draw_quad(make_rect(client.current.x, client.current.y, client.current.w, 30.0f),
              rgba(client.accent.r, client.accent.g, client.accent.b, focused ? 0.96f : 0.76f));
    draw_outline(client.current,
                 focused ? rgba(0.93f, 0.96f, 1.0f, 0.98f) : rgba(0.25f, 0.31f, 0.40f, 0.92f),
                 focused ? 2.0f : 1.0f);

    draw_text(state, client.current.x + 12.0f, client.current.y + 21.0f, rgba(0.06f, 0.08f, 0.12f, 1.0f), client.title);

    if (client.key == "tty0") {
        draw_terminal_surface(state, client);
    } else if (client.key == "browser") {
        draw_browser_surface(state, client);
    } else if (client.key == "rootfs") {
        draw_root_surface(state, client);
    } else {
        draw_text(state, client.current.x + 12.0f, client.current.y + 54.0f, rgba(0.76f, 0.80f, 0.88f, 1.0f), client.subtitle);
        draw_text(state, client.current.x + 12.0f, client.current.y + 80.0f, rgba(0.54f, 0.59f, 0.68f, 1.0f), client.floating ? "floating pane" : "tiled pane");
        if (focused) {
            draw_text(state, client.current.x + 12.0f, client.current.y + 106.0f, rgba(0.88f, 0.93f, 1.0f, 1.0f), "focused");
        }
    }
}

void draw_help(const AppState& state) {
    if (!state.show_help) {
        return;
    }

    const Rect panel = make_rect(34.0f, static_cast<float>(state.height) - 186.0f, 710.0f, 138.0f);
    draw_quad(panel, rgba(0.06f, 0.08f, 0.12f, 0.86f));
    draw_outline(panel, rgba(0.24f, 0.29f, 0.38f, 0.92f), 1.0f);

    draw_text(state, panel.x + 14.0f, panel.y + 24.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), "hotkeys");
    draw_text(state, panel.x + 14.0f, panel.y + 52.0f, rgba(0.74f, 0.79f, 0.88f, 1.0f), "super+d launcher  |  super+alt+space omarchy menu  |  super+enter tty0");
    draw_text(state, panel.x + 14.0f, panel.y + 78.0f, rgba(0.74f, 0.79f, 0.88f, 1.0f), "super+1..9 workspace  |  super+shift+1..9 move pane  |  super+l lock");
    draw_text(state, panel.x + 14.0f, panel.y + 104.0f, rgba(0.74f, 0.79f, 0.88f, 1.0f), "h/j/k/l focus  |  super+q close pane  |  f float  |  m monocle  |  r rotate  |  g help");
}

void draw_toast(const AppState& state) {
    if (state.toast.empty() || glfwGetTime() > state.toast_until) {
        return;
    }

    const Rect toast = make_rect(static_cast<float>(state.width) - 360.0f, static_cast<float>(state.height) - 84.0f, 320.0f, 44.0f);
    draw_quad(toast, rgba(0.10f, 0.13f, 0.19f, 0.92f));
    draw_outline(toast, rgba(0.37f, 0.54f, 0.92f, 0.96f), 1.0f);
    draw_text(state, toast.x + 14.0f, toast.y + 29.0f, rgba(0.94f, 0.97f, 1.0f, 1.0f), state.toast);
}

void draw_root_picker(const AppState& state, double now) {
    draw_background(state, static_cast<float>(now), true);

    char date_buf[32];
    char time_buf[32];
    current_time_strings(date_buf, sizeof(date_buf), time_buf, sizeof(time_buf));

    const float card_w = std::min(static_cast<float>(state.width) - 80.0f, 1120.0f);
    const float card_h = std::min(static_cast<float>(state.height) - 80.0f, 650.0f);
    const Rect card = make_rect(static_cast<float>(state.width) * 0.5f - card_w * 0.5f,
                                static_cast<float>(state.height) * 0.5f - card_h * 0.5f,
                                card_w,
                                card_h);
    draw_quad(card, rgba(0.06f, 0.08f, 0.11f, 0.90f));
    draw_outline(card, rgba(0.29f, 0.34f, 0.43f, 0.96f), 1.0f);
    const float line_h = std::max(18.0f, state.font_line_height);

    draw_text(state, card.x + 28.0f, card.y + 42.0f, rgba(0.97f, 0.98f, 1.0f, 1.0f), "omarchy shell session chooser");
    draw_text(state, card.x + 28.0f, card.y + 42.0f + line_h, rgba(0.75f, 0.80f, 0.88f, 1.0f), "pick a root folder, then boot the fullscreen mounted-root session");
    draw_text(state, card.x + card.w - 140.0f, card.y + 42.0f, rgba(0.91f, 0.94f, 1.0f, 1.0f), time_buf);
    draw_text(state, card.x + card.w - 240.0f, card.y + 42.0f + line_h, rgba(0.72f, 0.77f, 0.85f, 1.0f), date_buf);

    const Rect path_box = make_rect(card.x + 24.0f, card.y + 98.0f, card.w - 48.0f, 46.0f);
    draw_quad(path_box, rgba(0.11f, 0.13f, 0.19f, 0.94f));
    draw_outline(path_box, rgba(0.34f, 0.49f, 0.84f, 0.96f), 1.0f);
    draw_text(state, path_box.x + 14.0f, path_box.y + 16.0f, rgba(0.72f, 0.77f, 0.86f, 1.0f), "current path");
    draw_text(state, path_box.x + 14.0f, path_box.y + 16.0f + line_h, rgba(0.94f, 0.97f, 1.0f, 1.0f), fit_text_to_width(state, shellish_path(state.picker_path), path_box.w - 28.0f));

    const float column_top = path_box.y + path_box.h + 16.0f;
    const float footer_h = 108.0f;
    const Rect places_box = make_rect(card.x + 24.0f, column_top, 300.0f, card.y + card.h - footer_h - column_top - 16.0f);
    const Rect browser_box = make_rect(places_box.x + places_box.w + 16.0f, column_top, card.x + card.w - 24.0f - (places_box.x + places_box.w + 16.0f), places_box.h);
    draw_quad(places_box, rgba(0.08f, 0.10f, 0.15f, 0.88f));
    draw_outline(places_box, rgba(0.23f, 0.28f, 0.37f, 0.92f), 1.0f);
    draw_quad(browser_box, rgba(0.08f, 0.10f, 0.15f, 0.84f));
    draw_outline(browser_box, rgba(0.23f, 0.27f, 0.36f, 0.92f), 1.0f);
    draw_text(state, places_box.x + 16.0f, places_box.y + 22.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), state.picker_focus_places ? "quick places  [focused]" : "quick places");
    draw_text(state, browser_box.x + 16.0f, browser_box.y + 22.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), state.picker_focus_places ? "folder browser" : "folder browser  [focused]");

    const float place_row_h = line_h * 2.0f + 16.0f;
    const float place_rows_y = places_box.y + 38.0f;
    const int visible_places = std::max(1, static_cast<int>((places_box.h - 52.0f) / place_row_h));
    const int place_start = state.picker_places.empty() ? 0 : std::max(0, std::min(state.picker_place_index - visible_places / 2, static_cast<int>(state.picker_places.size()) - visible_places));
    const int place_end = std::min(static_cast<int>(state.picker_places.size()), place_start + visible_places);
    float place_y = place_rows_y;
    for (int i = place_start; i < place_end; ++i) {
        const bool selected = i == state.picker_place_index;
        const bool focused = selected && state.picker_focus_places;
        const FolderEntry& entry = state.picker_places[static_cast<std::size_t>(i)];
        const Rect row = make_rect(places_box.x + 12.0f, place_y, places_box.w - 24.0f, place_row_h - 6.0f);
        draw_quad(row, selected ? rgba(0.16f, 0.22f, 0.36f, focused ? 0.98f : 0.84f) : rgba(0.09f, 0.10f, 0.15f, 0.76f));
        draw_outline(row, focused ? rgba(0.52f, 0.69f, 1.0f, 0.98f) : rgba(0.21f, 0.26f, 0.35f, 0.88f), selected ? 2.0f : 1.0f);
        draw_text(state, row.x + 12.0f, row.y + 18.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), fit_text_to_width(state, entry.label, row.w - 24.0f));
        draw_text(state, row.x + 12.0f, row.y + 18.0f + line_h, rgba(0.72f, 0.77f, 0.86f, 1.0f), fit_text_to_width(state, entry.detail, row.w - 24.0f));
        place_y += place_row_h;
    }

    if (state.picker_places.empty()) {
        draw_text(state, places_box.x + 16.0f, places_box.y + 62.0f, rgba(0.72f, 0.77f, 0.86f, 1.0f), "no quick places available");
    }

    const float browser_row_h = line_h * 2.0f + 16.0f;
    const float browser_rows_y = browser_box.y + 38.0f;
    const int visible_entries = std::max(1, static_cast<int>((browser_box.h - 52.0f) / browser_row_h));
    const int entry_start = state.picker_entries.empty() ? 0 : std::max(0, std::min(state.picker_index - visible_entries / 2, static_cast<int>(state.picker_entries.size()) - visible_entries));
    const int entry_end = std::min(static_cast<int>(state.picker_entries.size()), entry_start + visible_entries);
    float entry_y = browser_rows_y;
    for (int i = entry_start; i < entry_end; ++i) {
        const bool selected = i == state.picker_index;
        const bool focused = selected && !state.picker_focus_places;
        const FolderEntry& entry = state.picker_entries[static_cast<std::size_t>(i)];
        const Rect row = make_rect(browser_box.x + 12.0f, entry_y, browser_box.w - 24.0f, browser_row_h - 6.0f);
        draw_quad(row, selected ? rgba(0.16f, 0.22f, 0.36f, focused ? 0.98f : 0.84f) : rgba(0.09f, 0.10f, 0.15f, 0.72f));
        draw_outline(row, focused ? rgba(0.52f, 0.69f, 1.0f, 0.98f) : rgba(0.19f, 0.24f, 0.33f, 0.86f), selected ? 2.0f : 1.0f);

        std::string prefix = "[dir]";
        if (entry.kind == FolderEntryKind::Parent) {
            prefix = "[..]";
        } else if (entry.kind == FolderEntryKind::Drive) {
            prefix = "[drv]";
        }

        draw_text(state, row.x + 12.0f, row.y + 18.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), fit_text_to_width(state, prefix + " " + entry.label, row.w - 24.0f));
        draw_text(state, row.x + 12.0f, row.y + 18.0f + line_h, rgba(0.72f, 0.77f, 0.86f, 1.0f), fit_text_to_width(state, entry.detail, row.w - 24.0f));
        entry_y += browser_row_h;
    }

    if (state.picker_entries.empty()) {
        draw_text(state, browser_box.x + 16.0f, browser_box.y + 62.0f, rgba(0.72f, 0.77f, 0.86f, 1.0f), "this folder has no child directories");
        draw_text(state, browser_box.x + 16.0f, browser_box.y + 62.0f + line_h, rgba(0.72f, 0.77f, 0.86f, 1.0f), "press tab to boot this folder or backspace to go up");
    }

    const FolderEntry* selected_entry = nullptr;
    if (state.picker_focus_places && state.picker_place_index >= 0 && state.picker_place_index < static_cast<int>(state.picker_places.size())) {
        selected_entry = &state.picker_places[static_cast<std::size_t>(state.picker_place_index)];
    } else if (!state.picker_focus_places && state.picker_index >= 0 && state.picker_index < static_cast<int>(state.picker_entries.size())) {
        selected_entry = &state.picker_entries[static_cast<std::size_t>(state.picker_index)];
    }

    const Rect footer = make_rect(card.x + 24.0f, card.y + card.h - footer_h, card.w - 48.0f, footer_h - 16.0f);
    draw_quad(footer, rgba(0.10f, 0.12f, 0.18f, 0.92f));
    draw_outline(footer, rgba(0.23f, 0.28f, 0.38f, 0.92f), 1.0f);
    draw_text(state, footer.x + 14.0f, footer.y + 18.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), "boot target");
    const std::string target_path = selected_entry ? selected_entry->path : state.picker_path;
    const std::vector<std::string> target_lines = wrap_text_to_width(state, shellish_path(target_path), footer.w - 28.0f, 2);
    float footer_text_y = footer.y + 18.0f + line_h;
    for (const std::string& line : target_lines) {
        draw_text(state, footer.x + 14.0f, footer_text_y, rgba(0.82f, 0.88f, 0.96f, 1.0f), line);
        footer_text_y += line_h;
    }

    draw_text(state, footer.x + 14.0f, footer.y + footer.h - 22.0f, rgba(0.74f, 0.79f, 0.88f, 1.0f),
              "left/right switch pane  |  up/down move  |  enter open  |  tab boot current folder  |  backspace parent  |  esc quit");
    draw_toast(state);
}

void draw_boot(const AppState& state, double now) {
    draw_background(state, static_cast<float>(now), false);
    draw_text(state, 54.0f, 114.0f, rgba(0.97f, 0.98f, 1.0f, 1.0f), "hil :: omarchy-inspired session");
    draw_text(state, 54.0f, 142.0f, rgba(0.72f, 0.79f, 0.89f, 1.0f), "accepting mount root, seeding pseudo fs, capturing shell keys");

    const Rect panel = make_rect(54.0f, 182.0f, 820.0f, 188.0f);
    draw_quad(panel, rgba(0.06f, 0.08f, 0.12f, 0.86f));
    draw_outline(panel, rgba(0.24f, 0.29f, 0.38f, 0.92f), 1.0f);

    float y = panel.y + 30.0f;
    for (const std::string& line : state.boot_lines) {
        draw_text(state, panel.x + 16.0f, y, rgba(0.78f, 0.83f, 0.91f, 1.0f), line);
        y += 28.0f;
    }

    const float progress = static_cast<float>(std::clamp((now - state.mode_started_at) / 1.8, 0.0, 1.0));
    draw_quad(make_rect(panel.x, panel.y + panel.h - 16.0f, panel.w, 6.0f), rgba(1.0f, 1.0f, 1.0f, 0.08f));
    draw_quad(make_rect(panel.x, panel.y + panel.h - 16.0f, panel.w * progress, 6.0f), rgba(0.35f, 0.62f, 0.96f, 0.95f));
}

void draw_greeter(const AppState& state, double now) {
    draw_background(state, static_cast<float>(now), true);

    char date_buf[32];
    char time_buf[32];
    current_time_strings(date_buf, sizeof(date_buf), time_buf, sizeof(time_buf));

    const Rect card = make_rect(static_cast<float>(state.width) * 0.5f - 270.0f, static_cast<float>(state.height) * 0.5f - 180.0f, 540.0f, 330.0f);
    draw_quad(card, rgba(0.06f, 0.08f, 0.11f, 0.88f));
    draw_outline(card, rgba(0.28f, 0.34f, 0.44f, 0.94f), 1.0f);

    draw_text(state, card.x + 24.0f, card.y + 42.0f, rgba(0.97f, 0.98f, 1.0f, 1.0f), "omarchy shell");
    draw_text(state, card.x + 24.0f, card.y + 72.0f, rgba(0.76f, 0.81f, 0.89f, 1.0f), "hyprland flavor, windows host");
    draw_text(state, card.x + 24.0f, card.y + 118.0f, rgba(0.90f, 0.94f, 1.0f, 1.0f), time_buf);
    draw_text(state, card.x + 114.0f, card.y + 118.0f, rgba(0.73f, 0.78f, 0.86f, 1.0f), date_buf);
    draw_text(state, card.x + 24.0f, card.y + 158.0f, rgba(0.80f, 0.85f, 0.93f, 1.0f), "mounted root");
    draw_text(state, card.x + 24.0f, card.y + 184.0f, rgba(0.95f, 0.97f, 1.0f, 1.0f), compact_path(state.mount_root_path, 52));

    const Rect input = make_rect(card.x + 24.0f, card.y + 216.0f, card.w - 48.0f, 46.0f);
    draw_quad(input, rgba(0.10f, 0.12f, 0.18f, 0.92f));
    draw_outline(input, rgba(0.35f, 0.54f, 0.92f, 0.96f), 1.0f);

    const std::string user_line = state.login_input.empty() ? "guest" : state.login_input;
    draw_text(state, input.x + 16.0f, input.y + 30.0f, rgba(0.96f, 0.98f, 1.0f, 1.0f), user_line);
    draw_text(state, card.x + 24.0f, card.y + 294.0f, rgba(0.72f, 0.77f, 0.85f, 1.0f), "type a user label, press enter to start, escape to quit");
}

void draw_menu_overlay(const AppState& state, const std::string& title, const std::string& subtitle,
                       const std::vector<MenuEntry>& entries, int selected_index) {
    draw_quad(make_rect(0.0f, 0.0f, static_cast<float>(state.width), static_cast<float>(state.height)),
              rgba(0.0f, 0.0f, 0.0f, 0.34f));

    struct WrappedMenuEntry {
        std::vector<std::string> title_lines;
        std::vector<std::string> detail_lines;
        float height = 0.0f;
    };

    const float card_w = std::clamp(static_cast<float>(state.width) * 0.58f, 520.0f, static_cast<float>(state.width) - 120.0f);
    const float row_text_width = card_w - 68.0f;
    const float line_h = std::max(18.0f, state.font_line_height);
    std::vector<WrappedMenuEntry> wrapped_entries;
    wrapped_entries.reserve(entries.size());
    float total_rows_h = 0.0f;
    for (const MenuEntry& entry : entries) {
        WrappedMenuEntry wrapped;
        wrapped.title_lines = wrap_text_to_width(state, entry.title, row_text_width, 1);
        wrapped.detail_lines = wrap_text_to_width(state, entry.detail, row_text_width, 2);
        wrapped.height = 18.0f + static_cast<float>(wrapped.title_lines.size() + wrapped.detail_lines.size()) * line_h + 18.0f;
        total_rows_h += wrapped.height + 10.0f;
        wrapped_entries.push_back(wrapped);
    }

    const float card_h = std::clamp(110.0f + total_rows_h + 24.0f, 240.0f, static_cast<float>(state.height) - 120.0f);
    const Rect card = make_rect(static_cast<float>(state.width) * 0.5f - card_w * 0.5f,
                                static_cast<float>(state.height) * 0.5f - card_h * 0.5f,
                                card_w,
                                card_h);
    draw_quad(card, rgba(0.06f, 0.08f, 0.11f, 0.94f));
    draw_outline(card, rgba(0.29f, 0.35f, 0.44f, 0.96f), 1.0f);
    draw_text(state, card.x + 24.0f, card.y + 40.0f, rgba(0.97f, 0.98f, 1.0f, 1.0f), fit_text_to_width(state, title, card.w - 48.0f));
    draw_text(state, card.x + 24.0f, card.y + 40.0f + line_h, rgba(0.75f, 0.80f, 0.88f, 1.0f), fit_text_to_width(state, subtitle, card.w - 48.0f));

    float y = card.y + 108.0f;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool selected = static_cast<int>(i) == selected_index;
        const WrappedMenuEntry& wrapped = wrapped_entries[i];
        const Rect row = make_rect(card.x + 18.0f, y - 22.0f, card.w - 36.0f, wrapped.height);
        draw_quad(row, selected ? rgba(0.15f, 0.21f, 0.34f, 0.96f) : rgba(0.09f, 0.10f, 0.15f, 0.82f));
        draw_outline(row, selected ? rgba(0.39f, 0.57f, 0.95f, 0.96f) : rgba(0.21f, 0.26f, 0.35f, 0.88f), 1.0f);
        float text_y = row.y + 20.0f;
        for (const std::string& line : wrapped.title_lines) {
            draw_text(state, row.x + 16.0f, text_y, rgba(0.96f, 0.98f, 1.0f, 1.0f), line);
            text_y += line_h;
        }
        for (const std::string& line : wrapped.detail_lines) {
            draw_text(state, row.x + 16.0f, text_y, rgba(0.72f, 0.77f, 0.86f, 1.0f), line);
            text_y += line_h;
        }
        y += wrapped.height + 10.0f;
    }
}

void draw_desktop(const AppState& state, double now) {
    draw_background(state, static_cast<float>(now), false);
    draw_top_bar(state);

    const std::vector<int> indices = visible_clients(state);
    for (int index : indices) {
        draw_client(state, state.clients[static_cast<std::size_t>(index)], index == state.focused_client);
    }

    if (indices.empty()) {
        draw_text(state, 54.0f, 132.0f, rgba(0.93f, 0.96f, 1.0f, 1.0f), "empty workspace");
        draw_text(state, 54.0f, 158.0f, rgba(0.71f, 0.76f, 0.84f, 1.0f), "switch workspaces with 1-9 or pull up walker with super+space");
    }

    draw_help(state);
    draw_toast(state);

    if (state.mode == ShellMode::Launcher) {
        draw_menu_overlay(state, "launcher", "pick a pane or command target", state.launcher_entries, state.launcher_index);
    } else if (state.mode == ShellMode::SystemMenu) {
        draw_menu_overlay(state, "omarchy menu", "session control, rebuilds, and lock", state.system_entries, state.system_index);
    }
}

void render(AppState& state, double now) {
    glViewport(0, 0, state.width, state.height);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<double>(state.width), static_cast<double>(state.height), 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    switch (state.mode) {
    case ShellMode::RootPicker:
        draw_root_picker(state, now);
        break;
    case ShellMode::Boot:
        draw_boot(state, now);
        break;
    case ShellMode::Greeter:
        draw_greeter(state, now);
        break;
    case ShellMode::Desktop:
    case ShellMode::Launcher:
    case ShellMode::SystemMenu:
        draw_desktop(state, now);
        break;
    }
}

void update_state(AppState& state, double now) {
    if (!state.toast.empty() && now > state.toast_until) {
        state.toast.clear();
    }

    switch (state.mode) {
    case ShellMode::RootPicker:
        handle_root_picker_input(state, state.close_user_apps_first);
        break;
    case ShellMode::Boot:
        handle_boot_input(state, now);
        break;
    case ShellMode::Greeter:
        handle_greeter_input(state);
        break;
    case ShellMode::Desktop:
        handle_desktop_input(state);
        break;
    case ShellMode::Launcher:
        handle_desktop_input(state);
        handle_launcher_input(state);
        break;
    case ShellMode::SystemMenu:
        handle_desktop_input(state);
        handle_system_menu_input(state);
        break;
    }
}

}  // namespace

int HilSmokeTest(HilEnvironmentInfo* out_info) {
    set_last_error("");

    if (!init_glfw()) {
        return 1;
    }

    configure_window_hints(true);
    GLFWwindow* window = glfwCreateWindow(96, 96, "hil-smoke", nullptr, nullptr);
    if (!window) {
        set_last_error("GLFW could not create a hidden probe window.");
        glfwTerminate();
        return 1;
    }

    int status = 0;
    if (!init_gl_context(window, out_info)) {
        status = 1;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return status;
}

int HilRunShell(const HilLaunchConfig* config) {
    set_last_error("");

    if (!init_glfw()) {
        return 1;
    }

    configure_window_hints(false);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
    int monitor_x = 0;
    int monitor_y = 0;
    if (monitor) {
        glfwGetMonitorPos(monitor, &monitor_x, &monitor_y);
    }

    const int width = (config && config->preferred_width > 0) ? config->preferred_width : (mode ? mode->width : 1600);
    const int height = (config && config->preferred_height > 0) ? config->preferred_height : (mode ? mode->height : 900);

    GLFWwindow* window = glfwCreateWindow(width, height, "Hil Omarchy Shell", nullptr, nullptr);
    if (!window) {
        set_last_error("GLFW could not create the main shell window.");
        glfwTerminate();
        return 1;
    }

    AppState state{};
    state.window = window;
    state.hwnd = glfwGetWin32Window(window);
    state.workspace_count = std::max(1, config ? config->workspace_count : 5);
    state.active_workspace = std::clamp(config ? config->start_workspace : 0, 0, state.workspace_count - 1);
    state.capture_system_keys = !config || config->capture_system_keys != 0;
    state.close_user_apps_first = !config || config->close_user_apps_first != 0;
    state.mount_root_path = normalize_root_path(config ? config->mount_root_path : nullptr);
    state.host_username = environment_utf8("USERNAME", "binay");
    state.shell_cwd = current_directory_utf8();
    state.shell_prompt = make_shell_prompt(state.host_username, state.shell_cwd);
    state.browser_url = "file:///" + shellish_path(state.shell_cwd) + "/docs";

    glfwSetWindowUserPointer(window, &state);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCharCallback(window, char_callback);
    glfwSetWindowCloseCallback(window, window_close_callback);
    glfwSetWindowPos(window, monitor_x, monitor_y);

    if (!init_gl_context(window, &state.env)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    if (!create_bitmap_font(state)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
    state.width = framebuffer_width;
    state.height = framebuffer_height;

    apply_dwm_style(state.hwnd, !config || config->enable_blur != 0);
    SetWindowPos(state.hwnd, HWND_TOPMOST, monitor_x, monitor_y, width, height, SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    if (!state.mount_root_path.empty()) {
        if (!start_session_from_mount_root(state, state.close_user_apps_first)) {
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
    } else {
        state.picker_path = make_absolute_path(state.shell_cwd);
        refresh_root_picker(state);
        switch_mode(state, ShellMode::RootPicker);
    }

    if (!install_keyboard_capture(state.capture_system_keys)) {
        set_last_error("Unable to install the low-level keyboard hook.");
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    glfwFocusWindow(window);

    double last_tick = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        update_state(state, now);

        if (state.mode == ShellMode::Desktop || state.mode == ShellMode::Launcher || state.mode == ShellMode::SystemMenu) {
            compute_layout(state);
        }

        const float dt = static_cast<float>(std::clamp(now - last_tick, 0.0, 0.1));
        last_tick = now;
        animate_layout(state, dt);
        render(state, now);
        glfwSwapBuffers(window);
    }

    uninstall_keyboard_capture();

    if (state.font_base) {
        glDeleteLists(state.font_base, 96);
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

const char* HilGetLastError() {
    return g_last_error.c_str();
}
