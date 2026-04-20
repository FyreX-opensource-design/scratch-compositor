#pragma once

#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

struct comp_server;

void comp_config_sync_shell_env(struct comp_server *server);

enum comp_keybind_action {
	COMP_KEYBIND_NONE = 0,
	COMP_KEYBIND_QUIT,
	COMP_KEYBIND_CLOSE,
	COMP_KEYBIND_EXEC,
	COMP_KEYBIND_LAYOUT_TOGGLE,
	COMP_KEYBIND_LAYOUT_TILE,
	COMP_KEYBIND_LAYOUT_STACK,
	COMP_KEYBIND_LAYOUT_SCROLL,
	COMP_KEYBIND_TILE_SWAP_PREV,
	COMP_KEYBIND_TILE_SWAP_NEXT,
	COMP_KEYBIND_SCROLL_PREV,
	COMP_KEYBIND_SCROLL_NEXT,
	COMP_KEYBIND_TILE_TO_FIRST,
	COMP_KEYBIND_TILE_TO_LAST,
	/** Optional command= signed integer (default 1): shift N steps in sort order (+ end, − start). */
	COMP_KEYBIND_TILE_MOVE,
	COMP_KEYBIND_TILE_GRID_UP,
	COMP_KEYBIND_TILE_GRID_DOWN,
	COMP_KEYBIND_TILE_TO_COLUMN_TOP,
	COMP_KEYBIND_TILE_TO_COLUMN_BOTTOM,
	/**
	 * Required command=: `left|right|up|down|top|bottom`, optional count (`up 2`, `left 3`),
	 * or legacy bare signed integer (vertical only, + down / − up).
	 */
	COMP_KEYBIND_TILE_GRID_MOVE,
	/** command= workspace number 1..COMP_WORKSPACE_COUNT (see server.h). */
	COMP_KEYBIND_WORKSPACE_GOTO,
	COMP_KEYBIND_WORKSPACE_NEXT,
	COMP_KEYBIND_WORKSPACE_PREV,
	/** command= target workspace 1..COMP_WORKSPACE_COUNT. */
	COMP_KEYBIND_WORKSPACE_MOVE,
};

struct comp_keybind {
	uint32_t mods;
	xkb_keysym_t keysym;
	enum comp_keybind_action action;
	char *command;
	char *when_shell;
};

/** Tiling placement rule: first matching rule in file order wins. */
struct comp_tile_rule {
	regex_t app_id_re;
	regex_t title_re;
	bool have_app_id;
	bool have_title;
	/* mode=float: not placed in the tile grid (floating on top). mode=tile: normal grid cell. */
	bool float_in_tile;
	/* Lower values are placed earlier (left/top) among tiled windows. */
	int order;
};

/** xdg-decoration: first matching rule in file order wins (see `[decoration_rule]`). */
struct comp_decoration_rule {
	regex_t app_id_re;
	regex_t title_re;
	bool have_app_id;
	bool have_title;
	/** If true, compositor prefers server-side mode in tile/scroll (client hides CSD). */
	bool prefer_server_side;
};

struct comp_config {
	struct comp_keybind *binds;
	size_t n_binds;
	struct comp_tile_rule *tile_rules;
	size_t n_tile_rules;
	struct comp_decoration_rule *decoration_rules;
	size_t n_decoration_rules;
	/** When no `[decoration_rule]` matches, use this for tile/scroll (default true = hide client title bars). */
	bool decoration_strip_default;
	/** Optional `sh -c` snippets from `[hooks]` (trusted like exec). */
	char *hook_startup;
	char *hook_shutdown;
	char *hook_reload;
	/** Tile/scroll scene position easing (see `[layout_anim]` in CONFIG.md). */
	bool layout_anim_enabled;
	double layout_anim_lambda;
	double layout_anim_epsilon;
};

#define COMP_BIND_MOD_FILTER \
	(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

bool comp_config_default_path(char *out, size_t out_len);

bool comp_config_load(const char *path, struct comp_config **cfg_out);

void comp_config_free(struct comp_config *cfg);

/** Run hook under `sh -c` (async; does not wait). */
void comp_config_run_startup(const struct comp_config *cfg);
void comp_config_run_reload(const struct comp_config *cfg);
/** Run hook under `sh -c` and wait for exit (use for compositor shutdown). */
void comp_config_run_shutdown(const struct comp_config *cfg);

bool comp_keybind_when_ok(const struct comp_keybind *bind);

/**
 * Run keybinds for a physical key. `mods_filtered` and `sym` must be sampled
 * from the keyboard *before* wlr_keyboard_notify_key() updates XKB state.
 */
bool comp_config_try_bindings(struct comp_config *cfg, struct comp_server *server,
	bool key_pressed, uint32_t mods_filtered, xkb_keysym_t sym);

/**
 * Resolve tile_rule settings for a toplevel from WM app_id and title (may be NULL).
 * Uses the first matching [tile_rule] in config file order.
 */
void comp_config_tile_props_for_toplevel(const struct comp_config *cfg, const char *app_id,
	const char *title, bool *out_float_in_tile, int *out_order);

/**
 * Whether xdg-decoration should use server-side mode in tile/scroll for this app (hide client title bar).
 * Stack layout and tile-float windows always use client-side decorations regardless.
 */
bool comp_config_decoration_prefer_server_side_tile_scroll(const struct comp_config *cfg, const char *app_id,
	const char *title);
