# Stackcomp — compositor notes

Working document for the wlroots-based **stacking** compositor in this repo. Update this file as features land.

## Current scope (implemented)

- **Stacking / floating**: XDG toplevels are scene-graph siblings; focus raises a window (`wlr_scene_node_raise_to_top`) in stack mode.
- **Tiling mode**: `COMP_LAYOUT_TILE` lays out mapped XDG toplevels in a **roughly square grid** (≈ `ceil(sqrt(n))` columns) inside each physical output’s **`layer_workarea`** (layer-shell exclusive zones subtracted per head). Windows are assigned to an output by the **center** of their geometry in layout coordinates (fallback: primary / cursor output). Uses `wlr_xdg_toplevel_set_size` + scene node position.
- **Scroll mode**: `COMP_LAYOUT_SCROLL` is a niri-like horizontal strip **per output**: one tiled window per that output’s workarea width, with a **per-workspace, per-output** scroll slot; focus / IPC scroll steps apply to the focused window’s output (or the primary output when none focused).
- **Workspaces**: nine virtual desktops (`1`..`9`); new XDG toplevels attach to the current workspace. Only the active workspace’s toplevels are shown; tile/scroll arrange and scroll index are per workspace. Keybinds / IPC: `workspace`, `workspace_next`, `workspace_prev`, `workspace_move`; env `STACKCOMP_WORKSPACE` for `when=`. Panels can bind **`ext_workspace_manager_v1`** (**`ext-workspace-v1`**) to list workspaces and call **`activate`** (see **`PROTOCOLS.md`**); **waybar** and other bars need a workspace module compiled for this protocol, not Sway IPC. Example Waybar (**`ext/workspaces`**): **`config/waybar-stackcomp.jsonc`** + **`config/waybar-stackcomp.css`** — run with **`waybar -c …/waybar-stackcomp.jsonc -s …/waybar-stackcomp.css`** on **`$WAYLAND_DISPLAY`** after stackcomp starts (Waybar must include the ext-workspaces module; distro packages vary).
- **wlroots 0.19**, scene graph (`wlr_scene_*`), **`xdg-shell`**, **`wlr-layer-shell` v4** (panels / overlays via `wlr_scene_layer_surface_v1`), **`zwlr_screencopy_manager_v1`** (via `wlr_screencopy_manager_v1_create` for tools like **grim**), core seat, pointer, keyboard, outputs via `wlr_output_layout`. **Wayland / portal protocol surface** is still small overall; see **`PROTOCOLS.md`** for details and portal expectations.
- **Keybind config** (INI-style): repeated `[bind]` sections with `mods`, `key`, `action`, optional `command` (for `exec`), optional `when` (shell predicate, evaluated **on each key press**; exit 0 = bind active). Optional **`[hooks]`** section: **`startup`**, **`shutdown`**, **`reload`** shell strings (`sh -c`). See **`CONFIG.md`** for full syntax and every action; **`stackcomp.conf.example`** is a commented starter file. Modifiers and keysym are read **before** `wlr_keyboard_notify_key()` so XKB state matches user chord; `exec` / `when` / hooks use `posix_spawnp("sh", …)` instead of `fork` for safer interaction with wlroots’ threads.
  - **Actions** (summary): `quit`, `close`, `exec`, layout toggles, tile **sort** moves (`tile_left` / `tile_right`, `tile_first` / `tile_last`, `tile_move`), tile **grid** moves (`tile_up` / `tile_down`, column top/bottom, `tile_grid_move` with `command=`). Details and `command=` rules are in **`CONFIG.md`**. Built-in defaults include **Super+t** for `layout_toggle` when no config file is found, the file cannot be read, or it defines no binds.
  - **Search path:** optional `-c` / `--config` (overrides `$STACKCOMP_CONFIG`); otherwise `$STACKCOMP_CONFIG` if set; otherwise the first readable file among `$XDG_CONFIG_HOME/stackcomp/config` and `~/.config/stackcomp/config`. If no file is found or the file defines no binds, built-in defaults apply (`exec` uses `${TERMINAL:-foot}` via `sh -c`).
- **`[tile_rule]`** (optional): tiling placement for XDG toplevels. **`app_id=`** and/or **`title=`** hold **POSIX extended regex** (see `regcomp(3)`); if both are set, **both** must match. **`mode=`** `tile` (default) or `float` (window is not placed in the grid; centered on map, raised on focus). **`order=`** integer: **lower** values sort **earlier** in the tile grid (stable tie-break: map order). Rules are tried **in file order**; the first match applies. Rules are re-evaluated on **`set_title` / `set_app_id`**, and all views are refreshed when switching to tile layout.
- **Pointer**: `Super+drag` moves a window (unchanged; not yet in config file).
- **CLI:** `--layout stack|tile|scroll` sets the layout for a **new** compositor, or if another stackcomp is already listening on the IPC socket, sends `layout …` to it and exits (no second instance). **`--scroll`** is shorthand for **`--layout scroll`**. **`--tile-move`** `prev|next|left|right|first|last|<signed int>` and **`--tile-grid`** `up|down|left|right|top|bottom`, optional **`DIR COUNT`** as two argv tokens (e.g. `stackcomp --tile-grid left 3`), or a **signed integer** for legacy vertical-only steps, send IPC and **exit** (exits **1** if nothing is listening—unlike `--layout` / `--scroll`, they do not start the compositor). **`--scroll-move`** `prev|next|left|right|<signed int>` sends **`scroll …`** over IPC the same way (viewport steps; **1** if nothing is listening). **`--workspace`** `1`..`9` or **`next`** / **`prev`**, and **`--workspace-move`** `N`, send the matching **`workspace …`** IPC lines (**1** if nothing is listening). **`--reload-config`** sends **`reload config`** over IPC and exits (**1** if nothing is listening). **`--no-ipc`** disables the Unix socket on this instance. IPC is **on by default** whenever `XDG_RUNTIME_DIR` is set (socket: `$XDG_RUNTIME_DIR/stackcomp-ipc.sock`). The **`--ipc`** flag is still accepted for compatibility and has no extra effect when IPC would already be enabled.
- **IPC commands** (text, newline optional): `layout toggle`, `layout tile`, `layout stack`, `layout scroll`; **`tile move prev|next|first|last`** or **`tile move <N>`** (sort-order steps); **`scroll prev|next|left|right|<N>`** or the same with an optional **`move`** token (e.g. **`scroll move next`**); **`tile grid`** with the same forms as **`tile_grid_move`** `command=` (e.g. `tile grid left`, `tile grid up 2`, legacy `tile grid -1`); **`reload config`** or **`reload`** to reload the config file and run the new **`[hooks]` `reload=`** script. Example: `echo 'layout tile' | nc -U "$XDG_RUNTIME_DIR/stackcomp-ipc.sock"`.

## Architecture (intended)

| Layer | Role |
|--------|------|
| **Backend** | `wlr_backend_autocreate` (DRM + libinput, or nested X11/Wayland). |
| **Scene** | Root scene → layer trees (background, windows, overlays, cursor policy). Stacking = z-order among window nodes. |
| **Layout** | `wlr_output_layout` + `wlr_scene_attach_output_layout`; optional tiling controller mutates window geometry instead of free drag. |
| **Input** | `wlr_cursor` + `wlr_seat`; keybindings handled in compositor before `wlr_seat_keyboard_notify_key` to clients. |
| **Config** | Eventually loaded at startup + optional reload (see below). |

## Roadmap (your plans)

### 1. Tiling mode

- Add an explicit **layout mode** enum: `STACK`, `TILE`, (optional `MONOCLE`, `FLOAT` per-workspace).
- Tiling controller computes target `wlr_box` per visible toplevel and applies `wlr_xdg_toplevel_set_size` + `wlr_scene_node_set_position` (or a dedicated scene subtree per workspace).
- Preserve **floating override** per window (e.g. dialogs, pinned floats) via tags or user command.
- **Suggested binding**: `Super+T` toggle stack/tile for focused output or workspace.

### 2. Keybind config with shell-based conditionals

- **Implemented:** INI-style `[bind]` blocks (see above). Actions today: `quit`, `close`, `exec` / `spawn`.
- **`when`:** shell snippet via `when=…`; evaluated on **every key press** (dynamic; small latency per match). Later: optional cache or reload-only evaluation.
- **Next:** more actions (`workspace`, `toggle_layout`, …), config reload signal, richer keysym/mod parsing.
- **Security:** `when` and `exec` run under `/bin/sh -c`; treat config as trusted code.

### 3. Plugin system (flags, IPC, scripting)

Three tiers (not mutually exclusive):

| Mechanism | Use case |
|-----------|----------|
| **Binary flags** | `--load-plugin /path/to/foo.so` for native C plugins with a stable C ABI (versioned struct of callbacks). |
| **IPC** | Unix socket + JSON or msgpack: `stackcomp-ipc workspace 3`, events stream for external bars/panels. |

**Bars and workspace UIs:** **`--workspace`** / **`--workspace-move`** and the text IPC socket remain for scripts. **`ext-workspace-v1`** is implemented for standard Wayland clients. **Sway/i3-style bar IPC** (JSON) is still unimplemented if you need drop-in **sway** bar compatibility.
| **Embedded scripting** | Lua or Python in-process for rapid policy (focus follows mouse, dynamic rules). Isolate with clear API surface; consider subprocess + IPC if crashes are unacceptable. |

**Plugin API sketch**: hooks for `on_output_add`, `on_view_map`, `on_key`, `on_idle`, `decorate(view)`, etc., plus registration of custom actions for the config layer.

### 4. Theming (GTK / Qt, mainly server-side)

Wayland clients cannot read arbitrary GTK/Qt theme files for **other** clients; theming is compositor-driven or client-driven.

- **Server-side levers**: background color or image, **server-side decorations** (SSD) if implemented, **fractional scale**, **cursor theme** (`wlr_xcursor_manager`), optional **color management** / **night light** via gamma or color transforms (wlroots exposes pieces of this over time).
- **GTK/Qt alignment**: document env vars (`GTK_THEME`, `QT_QPA_PLATFORMTHEME`, `XCURSOR_THEME`, `XCURSOR_SIZE`) set from compositor config or a wrapper session script; optionally expose **settings portal** or **xdg-desktop-portal-wlr** integration so “dark/light” toggles propagate.
- **Consistent chrome**: if SSD, draw title bars using a small internal theme format (colors, font, radius) shared with panel docs.
- **wlroots** protcals implementation

## Build & run

`meson setup build && meson compile -C build`. Install with `meson install -C build` (or distro packaging): **`stackcomp`** goes to the prefix **`bindir`**, and **`data/stackcomp.desktop`** is installed as **`share/wayland-sessions/stackcomp.desktop`** so greetd, SDDM, and similar greeters can list a **Stackcomp** Wayland session.

```bash
meson setup build && meson compile -C build
./build/stackcomp
./build/stackcomp --layout tile
./build/stackcomp --scroll
```

Nested (Wayland): `WAYLAND_DISPLAY=wayland-1 ./build/stackcomp`

**Build deps:** wlroots 0.19, `wayland-server`, `wayland-protocols` (for `xdg-shell` codegen), `xkbcommon`, plus wlroots’ normal graphics stack (DRM, libinput, etc.) for a DRM session.

Runtime is the same as other wlroots compositors: prefer a **TTY login** or a **nested** Wayland/X11 session for first tests.

## Naming

Placeholder binary name: **stackcomp**. Rename when you pick a final product name.
