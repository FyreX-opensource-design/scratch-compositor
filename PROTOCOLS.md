# Wayland protocols and desktop integration (stackcomp)

This document lists what **stackcomp** actually advertises and handles today, what **wlroots** adds implicitly when you call its helpers, and what is **not** implemented. It is derived from `src/main.c`, `meson.build`, and wlroots 0.19 behavior.

**Important distinction:** **xdg-desktop-portal** (settings, file chooser, screen cast, etc.) talks to implementations over **D-Bus** and, for some features, expects the **Wayland compositor** to expose specific **Wayland protocol** extensions (often **wlroots**-specific unstable ones). Stackcomp currently exposes a **minimal** set of globals; many portal and “desktop shell” features will **not** work until matching protocols are added.

---

## Explicitly created in `server_init` (your code)

These `wlr_*_create` calls register the corresponding **Wayland globals** (names clients see):

| Global (typical client name) | wlroots API | Notes |
|------------------------------|-------------|--------|
| **`wl_compositor`** | `wlr_compositor_create(dpy, 6, …)` | Version **6**. Surfaces and buffer attachment. |
| **`wl_subcompositor`** | `wlr_subcompositor_create` | Subsurfaces. |
| **`wl_data_device_manager`** | `wlr_data_device_manager_create` | Clipboard and drag-and-drop plumbing; selection is wired from the seat’s `request_set_selection`. |
| **`wl_output`** | Via **backend** / output layout | Physical outputs; layout uses `wlr_output_layout` + `wlr_scene_attach_output_layout`. |
| **`zxdg_output_manager_v1`** (xdg-output-unstable) | `wlr_xdg_output_manager_v1_create(dpy, output_layout)` | Logical output geometry for clients (e.g. **waybar** binds this after layer-shell). |
| **`xdg_wm_base`** (XDG shell) | `wlr_xdg_shell_create(dpy, 3)` | Version **3**. Toplevels (and popups are handled inside wlroots’ XDG implementation). This compositor only **tracks toplevels** for layout/focus. |
| **`zwlr_layer_shell_v1`** (wlr-layer-shell unstable) | `wlr_layer_shell_v1_create(dpy, 4)` | Version **4**. Desktop “shell” surfaces (panels, wallpapers, overlays) via **`wlr_scene_layer_surface_v1`**. Scene order (back → front): **background**, **output layout**, **bottom**, **windows**, **top**, **overlay** (bottom panels sit above the desktop but below XDG windows). **`wlr_scene_layer_surface_v1_configure`** drives exclusive zones; per-output **`layer_workarea`** feeds **tile** layout (single output) and initial **stack** window placement. **Keyboard:** pointer click on a layer surface can move seat keyboard focus for **on-demand** and (top/overlay) **exclusive** interactivity; otherwise keyboard stays on the focused XDG toplevel. |
| **`wl_seat`** | `wlr_seat_create(dpy, "seat0")` | Pointer + keyboard only (see below). |
| **`wl_shm`** / **`linux_dmabuf`** (and related buffer support) | `wlr_renderer_init_wl_display(renderer, dpy)` | wlroots registers what the renderer needs for clients to attach buffers. Exact globals depend on the renderer/backend (common: `wl_shm`, `zwp_linux_dmabuf_v1`). |

**Not created anywhere in this repo:** foreign toplevel management, output management, screencopy, export-dmabuf (for portal capture), gamma, idle inhibit, decoration protocols, presentation time, virtual keyboard/pointer, tablet/touch protocol objects on the seat, etc.

---

## Seat capabilities (`WL_SEAT_CAPABILITY_*`)

`wlr_seat_set_capabilities` is set to **pointer and keyboard only**. There is **no** `WL_SEAT_CAPABILITY_TOUCH` or tablet extension registration in this codebase, and no `wlr_cursor_attach_input_device` for non-pointer devices.

---

## Input events forwarded to clients

From the seat and cursor wiring in `main.c`:

- Keyboard: keymap, enter, key, modifiers (with compositor-side keybind handling before `wlr_seat_keyboard_notify_key`).
- Pointer: enter, motion, button, axis, frame; client cursor surface via `request_set_cursor`.
- Primary selection: **not** wired (no `wlr_primary_selection_v1_device_manager` or equivalent).

---

## `wayland-protocols` in this project

`meson.build` only runs **wayland-scanner** on **stable `xdg-shell`** to generate the server header. No other `wayland-protocols` XML files are built or linked. There are **no** extra stable or staging protocols vendored beyond what wlroots pulls in internally.

---

## xdg-desktop-portal and “wlr protocols”

Tools like **xdg-desktop-portal-wlr** (or similar) usually expect compositor support for things like:

| Need (examples) | Typical Wayland / wlroots side | In stackcomp? |
|-----------------|----------------------------------|---------------|
| Screen / window capture | `zwlr_screencopy_unstable_v1`, sometimes export-dmabuf | **No** |
| Inhibit shortcuts / idle | `zwp_keyboard_shortcuts_inhibit_v1`, `ext_idle_notification_v1` / `idle_inhibit` | **No** |
| Output layout for screen sharing / color | `xdg_wm_base` + full scene; `zxdg_output_manager_v1` for logical geometry | **Yes** xdg-output manager (wlroots) |
| Settings / dark mode (portal) | Often **D-Bus** + optional compositor hooks | **No** compositor hook |
| File open/save (portal) | Mostly **D-Bus** + GTK/Qt | May work **without** extra Wayland globals |
| PipeWire screen share from portal | Usually **screencopy** + dmabuf path | **No** |

So: **do not expect** `xdg-desktop-portal-wlr` screen sharing, global shortcuts inhibit, or similar **compositor-dependent** portal features to work against stock stackcomp today.

---

## Summary tables

### Implemented (directly or via wlroots as above)

- **Core:** compositor, subcompositor, SHM / dmabuf (via renderer), outputs, seat (keyboard + pointer), data device manager.
- **Shell:** **XDG Shell** toplevels (v3), **xdg-output** manager (logical geometry), **wlr-layer-shell** (v4) for panels/wallpaper.
- **Rendering:** scene graph, output layout, basic frame loop.

### Not implemented (common gaps)

- **wlr unstable / ecosystem:** `foreign-toplevel-management`, `output-management`, `screencopy`, `export-dmabuf` (portal), `gamma-control`, `idle-inhibit`, `server-decoration` / `xdg-decoration`, `primary-selection`, `pointer-constraints`, `relative-pointer`, `tablet`, `touch` on seat, `presentation-time`, `fractional-scale` (explicit), `cursor-shape` (wp), `xdg-activation`, `text-input` / `input-method`, `security-context`, etc.

### If you extend stackcomp

Typical order of work for “more apps work”:

1. **wlr-screencopy** (+ dmabuf path) — portal screen capture.
2. **xdg-decoration** or SSD — consistent window frames.
3. **Primary selection** — middle-click paste across clients.
4. **Tablet / touch** — register devices and seat capabilities.

Each addition needs new globals, usually extra `wayland-protocols` or `wlroots` unstable headers, and event handling; portal pieces still need a **portal implementation** configured in the session.

---

The **`wlr-layer-shell-unstable-v1.xml`** file under **`protocols/`** in this repository is a vendored copy (for `wayland-scanner`); it tracks the upstream **wlr-protocols** definition.

## See also

- **`COMPOSITOR.md`** — feature scope and build.
- **wlroots** [README / wiki](https://gitlab.freedesktop.org/wlroots/wlroots) for protocol modules available in 0.19.
