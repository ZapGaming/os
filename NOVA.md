# ZapOS Nova

Nova is the desktop direction for the `chatgpt/zapos` branch. It deliberately replaces the legacy window-manager look with a focused, native command-center interface.

## Design direction

- Dark spatial wallpaper with restrained cyan, blue, purple, and green accents
- Persistent system header with live uptime, SMP, and disk state
- Workspace navigation instead of a traditional imitation desktop
- A system overview built around live kernel information
- Dedicated full-height terminal experience
- Architecture view explaining what is truly running
- Keyboard-accessible command palette
- Consistent rounded surfaces, spacing, hierarchy, and status language

## Controls

| Action | Input |
|---|---|
| Open command palette | Backtick (`) |
| Switch from palette | 1, 2, or 3 |
| Select workspace | Sidebar click |
| Run a terminal command | Enter |
| Edit terminal input | Typing / Backspace |
| Exit palette | Escape or Backtick |

## Workspaces

### Overview

A live command center showing task count, architecture, privilege model, scheduler activity, and fast navigation into the terminal or system view.

### Terminal

The existing native ZapOS shell is presented as a first-class workspace rather than another tiny floating window. Commands still execute through `shell_execute()`, and output from launched ELF processes continues to route back into the terminal.

### System

A concise visual map of the boot path, kernel, memory model, GUI, networking stack, and runtime support.

## Compatibility

Nova keeps the existing compositor ABI:

- `gui_init()`
- `gui_run()`
- `gui_blit_fullscreen()`
- `terminal_route_output()`

This means the kernel startup path, DOOM fullscreen takeover, syscall output routing, scheduler, framebuffer, FAT32 filesystem, and native shell remain compatible.

The original `gui/compositor.c` remains in the repository as a reference for the browser, file manager, and earlier desktop implementation. The branch Makefile excludes it and builds `gui/nova_compositor.c` as the active shell.

## Next engineering targets

1. Move the browser and file manager out of the legacy monolith into standalone applications.
2. Add a lightweight application registry so Nova can launch native workspaces dynamically.
3. Add keyboard focus and proper key routing between workspaces.
4. Persist Nova settings to FAT32.
5. Add an in-OS theme editor and wallpaper controls.
6. Introduce partial redraw regions to reduce full-frame work.
