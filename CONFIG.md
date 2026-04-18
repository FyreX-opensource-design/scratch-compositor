# Stackcomp configuration

This file documents the **INI-style** configuration read at compositor startup. The format is line-oriented: sections in square brackets, `key = value` pairs, `#` comments, and blank lines ignored.

## Where the file is loaded from

1. **`stackcomp -c PATH` / `stackcomp --config PATH`** — highest priority when given.
2. **`$STACKCOMP_CONFIG`** — if set to a readable path.
3. **Default search** (first file that exists and is readable):
   - `$XDG_CONFIG_HOME/stackcomp/config`
   - `~/.config/stackcomp/config`

If no file is found, or the file defines **no** `[bind]` entries, **built-in defaults** are used (Super+Return runs `${TERMINAL:-foot}` via `sh -c`, Super+Shift+Q closes the focused client, Super+Escape quits, Super+t toggles stack/tile layout).

A starting point for your own file is **`stackcomp.conf.example`** in this repository.

---

## Section `[bind]`

Each `[bind]` block describes **one** shortcut. Start a new `[bind]` section for each binding.

| Key | Required | Meaning |
|-----|----------|---------|
| **`mods`** | Yes | Modifier mask. Tokens separated by `+`, comma, or space (case-insensitive). Recognized tokens: **`shift`**, **`ctrl`** / **`control`**, **`alt`** / **`meta`**, **`super`** / **`mod`** / **`logo`** / **`win`** / **`mod4`**. |
| **`key`** | Yes | Keysym name passed to **xkb_keysym_from_name** (case-insensitive), for example `Return`, `Escape`, `Q`, `Left`, `F1`. |
| **`action`** | Yes | What to do when the chord matches (see **Actions** below). |
| **`command`** | For some actions | Shell command line for **`exec`**, or parameter for **`tile_move`** / **`tile_grid_move`** as documented below. |
| **`when`** | No | If set, **`/bin/sh -c '…'`** is run **on every key press** before the bind is considered; **exit status 0** means the bind is active. Non-zero skips the bind. The shell sees `STACKCOMP_LAYOUT` as `stack`, `tile`, or `scroll`. |

Bindings are matched using modifier and keysym sampled **before** the compositor updates XKB state from the key event, so the chord matches what the user pressed.

### Layout-aware `when` example

```ini
[bind]
mods = Super+Shift
key = Right
action = scroll_next
when = [ "$STACKCOMP_LAYOUT" = scroll ]

[bind]
mods = Super+Shift
key = Right
action = tile_right
when = [ "$STACKCOMP_LAYOUT" = tile ]
```

### Security

**`command`** (for `exec`) and **`when`** are executed with **`posix_spawnp("sh", "-c", …)`**. Treat the config file as **trusted code**; do not point untrusted users at writable config paths.

---

## Section `[hooks]` (optional)

A single **`[hooks]`** block may define shell snippets run at lifecycle points. Each value is passed to **`/bin/sh -c`** (same trust model as **`exec`**).

| Key | When it runs |
|-----|----------------|
| **`startup`** / **`on_startup`** | After the compositor has started the backend and set **`WAYLAND_DISPLAY`** (async: stackcomp does not wait for the shell to exit). |
| **`shutdown`** / **`on_shutdown`** | When the compositor is exiting after a normal session (**after** `wl_display_run` returns). Stackcomp **waits** for this process to exit before tearing down the display. |
| **`reload`** / **`on_reload`** | After a successful **config reload** (IPC **`reload config`** or **`reload`**, or **`stackcomp --reload-config`**). Runs under the **new** config (async). |

If there is no config file path (defaults-only startup) and no default file on disk, **`reload config`** cannot resolve a path and fails with a log message.

---

## Section `[layout_anim]` (optional)

A single block **`[layout_anim]`** (alias **`[layout_animation]`**) configures **scene-node** easing when tiled or scroll-layout windows **move** to new cells or scroll positions. Client **configure / resize** is unchanged; only translation is smoothed. If the section is omitted, easing is **on** with the defaults below.

| Key | Default | Meaning |
|-----|---------|---------|
| **`enabled`** / **`enable`** | `yes` | `yes` / `true` / `1` / `on` or `no` / `false` / `0` / `off`. When off, positions snap immediately on every arrange. |
| **`lambda`** / **`speed`** | `15` | Exponential approach rate per second (higher = snappier). Allowed range **0.5–120**. |
| **`epsilon`** / **`snap`** | `0.35` | Pixel distance below which a window snaps to its target. Allowed range **0.05–64**. |

Changes apply after **config reload** (or compositor restart).

---

## Section `[decoration]` (optional)

Optional defaults for **xdg-decoration** (see **`[decoration_rule]`** below). If omitted, the compositor defaults to **stripping** client-side title bars in **tile** and **scroll** layouts, for clients that support the protocol.

| Key | Default | Meaning |
|-----|---------|---------|
| **`strip_in_tile_scroll`** / **`default_strip`** / **`strip`** | `yes` | If **yes**, windows that do **not** match a `[decoration_rule]` use **server-side** decoration mode in tile/scroll (clients hide their own title bar; stackcomp does not draw SSD, so the window appears frameless). If **no**, those windows keep **client-side** decorations in tile/scroll. |

**Stack** layout and **tile-float** windows always use **client-side** decorations (normal title bars), regardless of this section.

---

## Section `[decoration_rule]` (optional)

POSIX **extended** regex rules, same style as **`[tile_rule]`**: optional **`app_id`** and/or **`title`** (both must match when both are present). **First matching rule wins** in file order.

| Key | Meaning |
|-----|---------|
| **`app_id`** / **`app-id`** | Regex matched against the XDG `app_id`. |
| **`title`** | Regex matched against the window title. |
| **`strip`** | `yes` or `no` — same as “strip client title bar in tile/scroll” for this match. If omitted in a rule block, it defaults to **yes**. |

Use **`strip = no`** on specific apps (e.g. video players) to keep their client decorations while tiled.

---

## Actions reference

Unless noted, tiling-related actions are **no-ops** when not in **tile** layout, when there is **no focused** toplevel, when the focused surface is **unmapped**, or when the focused window is a **tile float** (floating within tile mode).

### General

| `action` | Aliases | `command` | Effect |
|----------|---------|-----------|--------|
| **`quit`** | `exit` | — | Terminate the compositor. |
| **`close`** | `kill` | — | Send close to the focused XDG toplevel. |
| **`exec`** | `spawn` | **Required** | Run `command` under `/bin/sh -c`. |
| **`layout_toggle`** | `toggle_layout` | — | Switch between stack and tile layout (scroll switches to stack). |
| **`layout_tile`** | `tile` | — | Force **tile** layout. |
| **`layout_stack`** | `stack` | — | Force **stack** layout. |
| **`layout_scroll`** | `scroll` | — | Force **scroll** layout (niri-like horizontal strip). |

### Sort order (linear index along the tiled list)

These move the **focused tiled** window along the internal sort order (row-major grid order, not “same row only”).

| `action` | Aliases | `command` | Effect |
|----------|---------|-----------|--------|
| **`tile_swap_prev`** | `tile_prev`, **`tile_left`**, `scroll_prev`, `scroll_left` | — | One step toward the **start** of the sort list. |
| **`tile_swap_next`** | `tile_next`, **`tile_right`**, `scroll_next`, `scroll_right` | — | One step toward the **end** of the sort list. |
| **`tile_to_first`** | `tile_first`, `tile_begin` | — | Bubble to the **first** slot in sort order. |
| **`tile_to_last`** | `tile_last`, `tile_end` | — | Bubble to the **last** slot in sort order. |
| **`tile_move`** | `tile_shift` | Optional signed integer; default **`1`** | Move **N** steps along sort order (positive = toward end, negative = toward start). |

### Grid moves (geometry: same row or same column)

The tile grid uses a fixed column count derived from the number of tiled windows (see compositor notes). **`tile_up` / `tile_down`** move one row within the **same column**; **`tile_grid_move`** can move **left/right** on the **same row**, or **up/down** in the **same column**, including multi-step forms.

| `action` | Aliases | `command` | Effect |
|----------|---------|-----------|--------|
| **`tile_grid_up`** | `tile_up` | — | Swap with the cell **one row up** (same column). |
| **`tile_grid_down`** | `tile_down` | — | Swap with the cell **one row down** (same column). |
| **`tile_column_top`** | `tile_col_top`, `tile_grid_top` | — | Bubble to the **top** of the current column. |
| **`tile_column_bottom`** | `tile_col_bottom`, `tile_grid_bottom` | — | Bubble to the **bottom** of the current column. |
| **`tile_grid_move`** | `tile_row_move` | **Required** | See **`tile_grid_move` `command=`** below. |

#### `tile_grid_move` `command=`

Exactly one of the following forms (after trimming surrounding space):

1. **Direction only:** `left`, `right`, `up`, `down`, `top`, `bottom` — one step, or column edge for **`top`** / **`bottom`**.
2. **Direction and count:** `DIR N` where **N** is an integer **≥ 1** (e.g. `left 3`, `up 2`). **`left`** / **`right`** move on the **same row**; **`up`** / **`down`** on the **same column**.
3. **Legacy vertical:** a **bare signed integer** (whole `command=` value is only a number, optional leading sign). Positive = **down**, negative = **up**, in grid rows.

Invalid or trailing junk after a valid prefix causes the bind line to be rejected at load time.

---

## Section `[tile_rule]` (optional)

Rules control how individual XDG toplevels behave in **tile** layout. **First matching rule in file order wins.**

| Key | Meaning |
|-----|---------|
| **`app_id`** | POSIX **extended** regex matched against the window’s **app_id** (Wayland). |
| **`title`** | POSIX **extended** regex matched against the window’s **title**. |
| **`mode`** | **`tile`** / **`tiled`** (default): normal grid cell. **`float`** / **`floating`**: not placed in the grid; shown centered when mapped and raised on focus in tile mode. |
| **`order`** | Integer; **lower** values sort **earlier** in the tile list among tiled windows. |

If both **`app_id`** and **`title`** are set on a rule, **both** must match. At least one of **`app_id`** or **`title`** must be present for a flushed rule to apply.

---

## Related: IPC and CLI (not config syntax)

From another terminal you can send one-line commands to a running instance (Unix socket under **`$XDG_RUNTIME_DIR/stackcomp-ipc.sock`** when IPC is enabled), for example:

- **`layout stack`**, **`layout tile`**, **`layout scroll`**, **`layout toggle`**
- **`tile move …`** (same semantics as sort-order actions above)
- **`scroll …`** or **`scroll move …`** — **`prev`** / **`left`**, **`next`** / **`right`**, or a **signed integer** (viewport steps in scroll layout; no-op if not in scroll layout)
- **`tile grid …`** (same forms as **`tile_grid_move` `command=`**)
- **`reload config`** or **`reload`** — re-read the config file (same path as at startup, or the default path), replace keybinds and tile rules, refresh tile layout if applicable, then run the new file’s **`reload`** hook from **`[hooks]`**.

The **`stackcomp`** binary also accepts **`--layout`**, **`--scroll`** (same as **`--layout scroll`**), **`--tile-move`**, **`--scroll-move`**, **`--tile-grid`**, and **`--reload-config`** for scripting; see **`COMPOSITOR.md`** for behavior when no compositor is listening.

---

## See also

- **`COMPOSITOR.md`** — compositor scope, architecture notes, build and run.
- **`PROTOCOLS.md`** — Wayland globals, wlroots vs portal expectations, unsupported protocols.
- **`stackcomp.conf.example`** — commented sample file in the repo.
