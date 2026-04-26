#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_input_device;
struct wlr_cursor;
struct wlr_data_device_manager;
struct wlr_output;
struct wlr_output_layout;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_output;
struct wlr_scene_output_layout;
struct wlr_scene_tree;
struct wlr_session;
struct wlr_seat;
struct wlr_subcompositor;
struct wlr_xcursor_manager;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;
struct wlr_xdg_output_manager_v1;
struct wlr_screencopy_manager_v1;
struct wlr_xdg_decoration_manager_v1;
struct wlr_xdg_toplevel_decoration_v1;

struct comp_config;

/** Number of virtual workspaces (indices 0 .. COUNT-1; keybinds/IPC use 1..COUNT). */
#define COMP_WORKSPACE_COUNT 9

enum comp_layout {
	COMP_LAYOUT_STACK = 0,
	COMP_LAYOUT_TILE,
	COMP_LAYOUT_SCROLL,
};

void comp_config_sync_layout_env(enum comp_layout layout);

enum comp_grab {
	COMP_GRAB_NONE,
	COMP_GRAB_MOVE,
};

struct comp_server;

struct comp_output {
	struct wl_list link;
	struct comp_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	/** Layout coords: full output box minus layer-shell exclusive zones (updated in layer_shell_arrange). */
	struct wlr_box layer_workarea;
	/** Scroll column index per workspace (scroll layout); independent per physical output. */
	int workspace_scroll_slot[COMP_WORKSPACE_COUNT];
	struct wl_listener frame;
	struct wl_listener commit;
	struct wl_listener destroy;
};

/** One zwlr_layer_surface_v1 client; lives on server->layers. */
struct comp_layer {
	struct wl_list link;
	struct comp_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_listener destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener new_popup;
};

struct comp_toplevel {
	struct wl_list link;
	struct comp_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	/** Tie-break for tiling sort; swapped with another toplevel on Super+drag drop (with tile_order). */
	uint32_t tile_user_key;
	bool tile_float;
	int tile_order;
	/** Tile/scroll layout target position (scene node lerps here when layout_anim_tracked). */
	int layout_tgt_x, layout_tgt_y;
	double layout_anim_x, layout_anim_y;
	bool layout_anim_tracked;
	/** Virtual workspace index 0 .. COMP_WORKSPACE_COUNT-1. */
	int workspace;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
	struct wlr_xdg_toplevel_decoration_v1 *xdg_decoration;
	struct wl_listener xdg_decoration_destroy;
	struct wl_listener xdg_decoration_request_mode;
};

struct comp_keyboard {
	struct wl_listener destroy;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct comp_server *server;
	struct wlr_input_device *dev;
};

struct comp_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_session *session;
	struct wlr_compositor *compositor;
	struct wlr_subcompositor *subcompositor;
	struct wlr_data_device_manager *data_device_mgr;
	struct wlr_output_layout *output_layout;
	struct wlr_xdg_output_manager_v1 *xdg_output_manager;
	struct wlr_screencopy_manager_v1 *screencopy_manager;
	struct wlr_scene *scene;
	/** Scene stacking (back to front): background, layout+outputs, bottom, windows, top, overlay. */
	struct wlr_scene_tree *layer_trees[4];
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *windows_tree;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_list layers;
	struct wl_listener layer_shell_new_surface;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_seat *seat;
	struct wl_listener new_output;
	struct wl_listener new_input;
	struct wl_listener xdg_shell_new_toplevel;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener new_xdg_decoration;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	struct wl_listener seat_request_cursor;
	struct wl_listener seat_request_set_selection;
	struct wl_list outputs;
	struct wl_list toplevels;
	enum comp_layout layout;
	struct comp_toplevel *focused_toplevel;
	int current_workspace;
	enum comp_grab grab;
	struct comp_toplevel *grabbed_toplevel;
	double grab_cursor_x, grab_cursor_y;
	int grab_view_x, grab_view_y;
	bool swallow_left_release;
	struct comp_config *config;
	/** Path used for the last successful load; used by `reload config` IPC. */
	char *config_path;
	bool ipc_enabled;
	struct wl_event_source *ipc_event_source;
	int ipc_listen_fd;
	char ipc_socket_path[108];
	/** CLOCK_MONOTONIC ns; used for layout position easing in tile/scroll. */
	uint64_t layout_anim_last_ns;
};

bool server_init(struct comp_server *server);

void server_set_layout(struct comp_server *server, enum comp_layout layout);
void server_toggle_layout(struct comp_server *server);
void server_arrange_toplevels(struct comp_server *server);
/** Re-apply xdg-decoration mode for every toplevel (layout / tile-float / config rules). */
void server_sync_xdg_decorations(struct comp_server *server);

/** Show only toplevels on current_workspace; layer-shell unchanged. */
void server_workspace_apply_visibility(struct comp_server *server);
/** Switch to workspace index `idx` (0 .. COMP_WORKSPACE_COUNT-1). */
void server_workspace_go(struct comp_server *server, int idx);
/** Rotate current workspace by `delta` (e.g. +1 / -1), wrapping. */
void server_workspace_relative(struct comp_server *server, int delta);
/** Move focused toplevel to workspace `target` (0-based). */
void server_workspace_move_focused(struct comp_server *server, int target);

/** Move focused tiled window by `steps` in sort order (+ toward end, − toward start). No-op if not tiled/focused. */
void server_tile_move_focused_n(struct comp_server *server, int steps);
/** Move focused tiled window to first or last slot in sort order. */
void server_tile_move_focused_edge(struct comp_server *server, bool to_first);
/** Move scroll viewport by N slots in scroll layout (does not reorder). */
void server_scroll_move(struct comp_server *server, int steps);

/** Move by grid rows: +steps toward bottom neighbor, −steps toward top (row-major layout). */
void server_tile_move_focused_grid_vert(struct comp_server *server, int steps);
/** Move focused window to top or bottom of its tile column. */
void server_tile_move_focused_grid_vert_edge(struct comp_server *server, bool to_top);
/** Move by grid columns on the same row: +steps right, −steps left. */
void server_tile_move_focused_grid_horiz(struct comp_server *server, int steps);

/**
 * Parse `tile grid …` remainder: `left|right|up|down|top|bottom`, optional `DIR N` (N≥1),
 * or legacy bare signed integer (vertical steps only).
 */
void server_tile_grid_run_command(struct comp_server *server, const char *cmd);
