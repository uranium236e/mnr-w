# Hil Omarchy Shell

This project builds a Windows DLL plus host EXE that present an Omarchy-inspired fullscreen shell with:

- a boot screen
- an in-shell TUI root selector that feels more like a display manager than a file dialog
- a greeter / lock-style login card
- a conservative prelaunch cleanup pass for visible user apps
- a pseudo Linux-like root mounted onto a chosen folder
- a GLEW + OpenGL compositor surface in a borderless GLFW window
- a small launcher (`walker`) and system menu (`omarchy menu`)
- a low-level keyboard hook that suppresses the Windows key while the shell is active
- a terminal pane styled after your current prompt and a browser-style pane for mounted-root docs

## Build

```powershell
powershell.exe -ExecutionPolicy Bypass -File scripts\build.ps1
```

## Smoke Test

```powershell
powershell.exe -ExecutionPolicy Bypass -File scripts\build.ps1 -RunSmokeTest
```

## Run

```powershell
build\hil_host.exe --root omarchy_root
```

If `--root` is omitted, the shell opens an in-WM display-manager style root selector after launch and lets you browse to the folder you want to mount.

## Controls

- `Enter` on the greeter starts the session
- in the root selector: `Up/Down` or `j/k` move, `Enter` opens or mounts, `Tab` mounts the current folder, `Backspace` goes to the parent folder
- `Super+Space` opens `walker`
- `Super+Alt+Space` opens the `omarchy menu`
- `Super+Enter` focuses `tty0`
- `Super+E` focuses the editor pane
- `Super+B` focuses the browser pane
- `Super+T` focuses the toolchain pane
- `Super+R` focuses the mounted-root pane
- type `shutdown` in `tty0` and press `Enter` to exit the shell
- `1-9` switches workspace
- `h/j/k/l` or arrows move focus
- `f` toggles floating
- `m` toggles monocle
- `s` swaps the split direction
- `r` rotates panes within the active workspace
- `g` hides or shows the help card
- `Esc` or `q` quits

## Root Layout

When the shell accepts a root folder it creates a seeded pseudo filesystem:

- `/etc/os-release`
- `/etc/motd`
- `/home/guest/.shellrc`
- `/workspace/hello.c`
- `/usr/bin/cc.cmd`
- `/toolchain/omcc/README.txt`

`/usr/bin/cc.cmd` is a shim that forwards to `C:\msys64\mingw64\bin\gcc.exe` when available.

## Notes

- This is an in-process shell, not a real Windows desktop replacement or Linux display manager.
- The prelaunch cleanup only targets visible desktop apps in the current session and skips a protected allowlist for core Windows processes and terminals.
- The Vulkan part is currently a runtime probe surfaced in the UI and smoke test, not a full Vulkan renderer.
