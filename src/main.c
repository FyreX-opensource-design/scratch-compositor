#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"
#include "server.h"

static void focus_toplevel(struct comp_server *server, struct comp_toplevel *toplevel);
static struct wlr_output *primary_wlr_output(struct comp_server *server);
static void process_cursor_motion(struct comp_server *server, uint32_t time_msec);
static void begin_move(struct comp_server *server, struct comp_toplevel *view, bool swallow_left_release);
static void ipc_fini(struct comp_server *server);
static bool ipc_init(struct comp_server *server);
static bool server_reload_config(struct comp_server *server);
static struct comp_toplevel **tile_sorted_views(struct comp_server *server, size_t *n_out);
static int tile_sorted_index(struct comp_toplevel **arr, size_t n, struct comp_toplevel *v);
static void toplevel_apply_decoration_mode(struct comp_toplevel *view);

/** True after `wlr_backend_start` so shutdown hook runs only for a real session. */
static bool compositor_session_active;
/** Verbose XDG lifecycle logs (off by default). Enable with `STACKCOMP_DEBUG_XDG=1`. */
static bool xdg_debug_logs_enabled;

static void log_xdg_state(const char *tag, struct comp_toplevel *view)
{
	if (!xdg_debug_logs_enabled || !view || !view->xdg_toplevel || !view->xdg_toplevel->base) {
		return;
	}
	struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
	wlr_log(WLR_INFO,
		"xdgdbg:%s app_id='%s' title='%s' mapped=%d initialized=%d initial_commit=%d layout=%d",
		tag,
		view->xdg_toplevel->app_id ? view->xdg_toplevel->app_id : "",
		view->xdg_toplevel->title ? view->xdg_toplevel->title : "",
		xdg->surface->mapped, xdg->initialized, xdg->initial_commit, (int)view->server->layout);
}

static struct comp_output *comp_output_from_wlr(struct comp_server *server, struct wlr_output *wlr_out)
{
	if (!wlr_out) {
		return NULL;
	}
	struct comp_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_out) {
			return o;
		}
	}
	return NULL;
}

static void scroll_sync_to_focused(struct comp_server *server)
{
	if (server->layout != COMP_LAYOUT_SCROLL || !server->focused_toplevel) {
		return;
	}
	size_t n = 0;
	struct comp_toplevel **arr = tile_sorted_views(server, &n);
	if (!arr || n == 0) {
		free(arr);
		return;
	}
	const int idx = tile_sorted_index(arr, n, server->focused_toplevel);
	if (idx >= 0) {
		server->scroll_index = idx;
	}
	free(arr);
}

/** Seconds since CLOCK_MONOTONIC epoch (for layout animation delta time). */
static uint64_t timespec_to_ns(const struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

static bool layout_anim_effective(const struct comp_server *server)
{
	return server->config && server->config->layout_anim_enabled;
}

/** Returns true if any tiled view still needs another frame to reach its target. */
static bool layout_anim_tick(struct comp_server *server, const struct timespec *now)
{
	if (server->layout != COMP_LAYOUT_TILE && server->layout != COMP_LAYOUT_SCROLL) {
		return false;
	}
	if (!layout_anim_effective(server)) {
		return false;
	}
	const double lambda = server->config->layout_anim_lambda;
	const double eps = server->config->layout_anim_epsilon;
	const uint64_t now_ns = timespec_to_ns(now);
	if (server->layout_anim_last_ns == 0) {
		server->layout_anim_last_ns = now_ns;
		return false;
	}
	double dt = (double)(now_ns - server->layout_anim_last_ns) / 1e9;
	server->layout_anim_last_ns = now_ns;
	if (dt <= 0.0) {
		return false;
	}
	if (dt > 0.1) {
		dt = 0.1;
	}
	const double k = 1.0 - exp(-lambda * dt);
	bool any = false;
	struct comp_toplevel *v;
	wl_list_for_each(v, &server->toplevels, link) {
		if (!v->layout_anim_tracked) {
			continue;
		}
		if (!v->xdg_toplevel->base->surface->mapped || v->tile_float) {
			continue;
		}
		if (server->grab == COMP_GRAB_MOVE && v == server->grabbed_toplevel) {
			continue;
		}
		const double dx = (double)v->layout_tgt_x - v->layout_anim_x;
		const double dy = (double)v->layout_tgt_y - v->layout_anim_y;
		if (fabs(dx) < eps && fabs(dy) < eps) {
			if (v->scene_tree->node.x != v->layout_tgt_x || v->scene_tree->node.y != v->layout_tgt_y) {
				wlr_scene_node_set_position(&v->scene_tree->node, v->layout_tgt_x, v->layout_tgt_y);
				any = true;
			}
			v->layout_anim_x = (double)v->layout_tgt_x;
			v->layout_anim_y = (double)v->layout_tgt_y;
			continue;
		}
		v->layout_anim_x += dx * k;
		v->layout_anim_y += dy * k;
		const int ix = (int)lround(v->layout_anim_x);
		const int iy = (int)lround(v->layout_anim_y);
		wlr_scene_node_set_position(&v->scene_tree->node, ix, iy);
		any = true;
	}
	return any;
}

/** After targets change, ensure outputs repaint even if no client buffer update occurs yet. */
static void layout_anim_kick_outputs(struct comp_server *server)
{
	if (server->layout != COMP_LAYOUT_TILE && server->layout != COMP_LAYOUT_SCROLL) {
		return;
	}
	if (!layout_anim_effective(server)) {
		return;
	}
	const double eps = server->config->layout_anim_epsilon;
	struct comp_toplevel *v;
	wl_list_for_each(v, &server->toplevels, link) {
		if (!v->layout_anim_tracked || !v->xdg_toplevel->base->surface->mapped || v->tile_float) {
			continue;
		}
		if (server->grab == COMP_GRAB_MOVE && v == server->grabbed_toplevel) {
			continue;
		}
		if (fabs((double)v->layout_tgt_x - v->layout_anim_x) > eps ||
		    fabs((double)v->layout_tgt_y - v->layout_anim_y) > eps) {
			struct comp_output *o;
			wl_list_for_each(o, &server->outputs, link) {
				wlr_output_schedule_frame(o->wlr_output);
			}
			return;
		}
	}
}

/* Hit surface under (lx, ly); sx/sy outputs are surface-local coords when non-NULL. */
static struct wlr_surface *surface_at(struct comp_server *server, double lx, double ly,
	double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(buf);
	return ss ? ss->surface : NULL;
}

static struct comp_toplevel *toplevel_at(struct comp_server *server, double lx, double ly, double *sx, double *sy)
{
	struct wlr_surface *surf = surface_at(server, lx, ly, sx, sy);
	if (!surf) {
		return NULL;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surf);
	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->xdg_toplevel->base->surface == root) {
			return t;
		}
	}
	return NULL;
}

static void output_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_output *output = wl_container_of(listener, output, frame);
	struct comp_server *server = output->server;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct wlr_output *clock_out = primary_wlr_output(server);
	const bool run_layout_anim = clock_out && output->wlr_output == clock_out;
	const bool layout_anim = run_layout_anim && layout_anim_tick(server, &now);

	if (!wlr_scene_output_needs_frame(output->scene_output) && !layout_anim) {
		return;
	}

	if (!wlr_scene_output_commit(output->scene_output, NULL)) {
		if (layout_anim) {
			struct comp_output *o;
			wl_list_for_each(o, &server->outputs, link) {
				wlr_output_schedule_frame(o->wlr_output);
			}
		}
		return;
	}
	wlr_scene_output_send_frame_done(output->scene_output, &now);

	if (layout_anim) {
		struct comp_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			wlr_output_schedule_frame(o->wlr_output);
		}
	}
}

static struct wlr_output *primary_wlr_output(struct comp_server *server)
{
	if (wl_list_empty(&server->outputs)) {
		return NULL;
	}
	if (server->cursor) {
		struct wlr_output *at = wlr_output_layout_output_at(server->output_layout, server->cursor->x,
			server->cursor->y);
		if (at) {
			return at;
		}
	}
	struct comp_output *co = wl_container_of(server->outputs.next, co, link);
	return co->wlr_output;
}

static void layer_shell_arrange(struct comp_server *server)
{
	static const enum zwlr_layer_shell_v1_layer layer_order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
	};
	struct comp_output *out;
	wl_list_for_each(out, &server->outputs, link) {
		struct wlr_box full;
		wlr_output_layout_get_box(server->output_layout, out->wlr_output, &full);
		struct wlr_box usable = full;
		for (size_t li = 0; li < sizeof(layer_order) / sizeof(layer_order[0]); li++) {
			struct comp_layer *layer;
			wl_list_for_each(layer, &server->layers, link) {
				if (layer->layer_surface->output != out->wlr_output) {
					continue;
				}
				/* Before the first ack, `current` may not match the client's layer yet. */
				const enum zwlr_layer_shell_v1_layer lyr = layer->layer_surface->configured
					? layer->layer_surface->current.layer
					: layer->layer_surface->pending.layer;
				if (lyr != layer_order[li]) {
					continue;
				}
				/* wlroots sets initialized on first surface commit; configure before that asserts. */
				if (!layer->layer_surface->initialized) {
					continue;
				}
				/* Must configure unmapped surfaces too: clients map only after the first configure. */
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &full, &usable);
			}
		}
		out->layer_workarea = usable;
	}
	if ((server->layout == COMP_LAYOUT_TILE || server->layout == COMP_LAYOUT_SCROLL) &&
	    server->grab != COMP_GRAB_MOVE) {
		server_arrange_toplevels(server);
	}
}

static void comp_layer_map(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_layer *layer = wl_container_of(listener, layer, map);
	layer_shell_arrange(layer->server);
}

static void comp_layer_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_layer *layer = wl_container_of(listener, layer, unmap);
	layer_shell_arrange(layer->server);
}

static void comp_layer_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_layer *layer = wl_container_of(listener, layer, commit);
	if (layer->layer_surface->initial_commit) {
		layer_shell_arrange(layer->server);
		return;
	}
	if (layer->layer_surface->pending.committed) {
		layer_shell_arrange(layer->server);
	}
}

static void comp_layer_new_popup(struct wl_listener *listener, void *data)
{
	struct comp_layer *layer = wl_container_of(listener, layer, new_popup);
	struct wlr_xdg_popup *popup = data;
	struct comp_output *out = comp_output_from_wlr(layer->server, layer->layer_surface->output);
	struct wlr_box box;
	if (out) {
		box = out->layer_workarea;
	} else {
		wlr_output_layout_get_box(layer->server->output_layout, layer->layer_surface->output, &box);
	}
	/* wlroots 0.19: unconstrain may schedule configure; popup base must be initialized. */
	if (popup->base && popup->base->initialized) {
		wlr_xdg_popup_unconstrain_from_box(popup, &box);
	} else if (xdg_debug_logs_enabled) {
		wlr_log(WLR_INFO, "xdgdbg:layer-popup skip unconstrain before initialized");
	}
	wlr_scene_xdg_surface_create(layer->scene_layer->tree, popup->base);
}

static void comp_layer_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_layer *layer = wl_container_of(listener, layer, destroy);
	wl_list_remove(&layer->map.link);
	wl_list_remove(&layer->unmap.link);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->new_popup.link);
	wl_list_remove(&layer->destroy.link);
	wl_list_remove(&layer->link);
	free(layer);
}

static void layer_shell_new_surface(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, layer_shell_new_surface);
	struct wlr_layer_surface_v1 *wlr_layer = data;

	if (wl_list_empty(&server->outputs)) {
		wlr_layer_surface_v1_destroy(wlr_layer);
		return;
	}
	if (!wlr_layer->output) {
		wlr_layer->output = primary_wlr_output(server);
	}

	const enum zwlr_layer_shell_v1_layer lyr = wlr_layer->pending.layer;
	if ((size_t)lyr >= sizeof(server->layer_trees) / sizeof(server->layer_trees[0])) {
		wlr_layer_surface_v1_destroy(wlr_layer);
		return;
	}

	struct comp_layer *layer = calloc(1, sizeof(*layer));
	if (!layer) {
		wlr_layer_surface_v1_destroy(wlr_layer);
		return;
	}
	layer->server = server;
	layer->layer_surface = wlr_layer;
	layer->scene_layer = wlr_scene_layer_surface_v1_create(server->layer_trees[lyr], wlr_layer);
	if (!layer->scene_layer) {
		free(layer);
		wlr_layer_surface_v1_destroy(wlr_layer);
		return;
	}

	layer->destroy.notify = comp_layer_destroy;
	wl_signal_add(&wlr_layer->events.destroy, &layer->destroy);
	layer->map.notify = comp_layer_map;
	wl_signal_add(&wlr_layer->surface->events.map, &layer->map);
	layer->unmap.notify = comp_layer_unmap;
	wl_signal_add(&wlr_layer->surface->events.unmap, &layer->unmap);
	layer->commit.notify = comp_layer_commit;
	wl_signal_add(&wlr_layer->surface->events.commit, &layer->commit);
	layer->new_popup.notify = comp_layer_new_popup;
	wl_signal_add(&wlr_layer->events.new_popup, &layer->new_popup);

	wl_list_insert(&server->layers, &layer->link);
}

static void output_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_output *output = wl_container_of(listener, output, commit);
	struct comp_server *srv = output->server;
	layer_shell_arrange(srv);
}

static void output_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_output *output = wl_container_of(listener, output, destroy);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->commit.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	wlr_scene_output_destroy(output->scene_output);
	wlr_output_layout_remove(output->server->output_layout, output->wlr_output);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct comp_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
	struct wlr_output_layout_output *lout = wlr_output_layout_add_auto(server->output_layout, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, lout, output->scene_output);

	float scale = wlr_output->scale;
	if (!wlr_xcursor_manager_load(server->cursor_mgr, scale)) {
		wlr_log(WLR_ERROR, "Cannot load XCursor theme for scale %f", scale);
	}

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->commit.notify = output_commit;
	wl_signal_add(&wlr_output->events.commit, &output->commit);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	wl_list_insert(&server->outputs, &output->link);
	layer_shell_arrange(server);
}

static uint32_t tile_user_key_gen;

static void toplevel_refresh_tile_props(struct comp_toplevel *view)
{
	if (!view->server->config) {
		view->tile_float = false;
		view->tile_order = 0;
		return;
	}
	comp_config_tile_props_for_toplevel(view->server->config, view->xdg_toplevel->app_id,
		view->xdg_toplevel->title, &view->tile_float, &view->tile_order);
}

static void server_refresh_all_tile_props(struct comp_server *server)
{
	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		toplevel_refresh_tile_props(t);
	}
}

static void xdg_decoration_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, xdg_decoration_destroy);
	wl_list_remove(&view->xdg_decoration_request_mode.link);
	wl_list_remove(&view->xdg_decoration_destroy.link);
	view->xdg_decoration = NULL;
}

static void xdg_decoration_handle_request_mode(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, xdg_decoration_request_mode);
	toplevel_apply_decoration_mode(view);
}

static void toplevel_apply_decoration_mode(struct comp_toplevel *view)
{
	if (!view->xdg_decoration) {
		return;
	}
	/* wlr_xdg_toplevel_decoration_v1_set_mode schedules configure; wlroots asserts if !initialized. */
	if (!view->xdg_toplevel->base->initialized) {
		return;
	}
	struct comp_server *server = view->server;
	enum wlr_xdg_toplevel_decoration_v1_mode mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	if (server->layout == COMP_LAYOUT_STACK) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	} else if (view->tile_float) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	} else if (comp_config_decoration_prefer_server_side_tile_scroll(server->config, view->xdg_toplevel->app_id,
			 view->xdg_toplevel->title)) {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	} else {
		mode = WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	}
	wlr_xdg_toplevel_decoration_v1_set_mode(view->xdg_decoration, mode);
}

void server_sync_xdg_decorations(struct comp_server *server)
{
	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		toplevel_apply_decoration_mode(t);
	}
}

static void xdg_new_toplevel_decoration(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, new_xdg_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	struct comp_toplevel *view = NULL;
	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t->xdg_toplevel == deco->toplevel) {
			view = t;
			break;
		}
	}
	if (!view) {
		return;
	}
	if (view->xdg_decoration) {
		wl_list_remove(&view->xdg_decoration_destroy.link);
		wl_list_remove(&view->xdg_decoration_request_mode.link);
		view->xdg_decoration = NULL;
	}
	view->xdg_decoration = deco;
	view->xdg_decoration_destroy.notify = xdg_decoration_handle_destroy;
	wl_signal_add(&deco->events.destroy, &view->xdg_decoration_destroy);
	view->xdg_decoration_request_mode.notify = xdg_decoration_handle_request_mode;
	wl_signal_add(&deco->events.request_mode, &view->xdg_decoration_request_mode);
	toplevel_apply_decoration_mode(view);
}

static void toplevel_handle_set_title(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, set_title);
	toplevel_refresh_tile_props(view);
	if ((view->server->layout == COMP_LAYOUT_TILE || view->server->layout == COMP_LAYOUT_SCROLL) &&
	    view->xdg_toplevel->base->surface->mapped) {
		server_arrange_toplevels(view->server);
	}
	server_sync_xdg_decorations(view->server);
}

static void toplevel_handle_set_app_id(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, set_app_id);
	toplevel_refresh_tile_props(view);
	if ((view->server->layout == COMP_LAYOUT_TILE || view->server->layout == COMP_LAYOUT_SCROLL) &&
	    view->xdg_toplevel->base->surface->mapped) {
		server_arrange_toplevels(view->server);
	}
	server_sync_xdg_decorations(view->server);
}

static int cmp_toplevel_tile_order(const void *va, const void *vb)
{
	const struct comp_toplevel *a = *(const struct comp_toplevel *const *)va;
	const struct comp_toplevel *b = *(const struct comp_toplevel *const *)vb;
	if (a->tile_order < b->tile_order) {
		return -1;
	}
	if (a->tile_order > b->tile_order) {
		return 1;
	}
	if (a->tile_user_key < b->tile_user_key) {
		return -1;
	}
	if (a->tile_user_key > b->tile_user_key) {
		return 1;
	}
	return 0;
}

static void tile_swap_sort_keys(struct comp_toplevel *a, struct comp_toplevel *b)
{
	uint32_t k = a->tile_user_key;
	a->tile_user_key = b->tile_user_key;
	b->tile_user_key = k;
	int o = a->tile_order;
	a->tile_order = b->tile_order;
	b->tile_order = o;
}

/** Caller frees; returns NULL when no tiled mapped non-float views. */
static struct comp_toplevel **tile_sorted_views(struct comp_server *server, size_t *n_out)
{
	*n_out = 0;
	size_t n_tile = 0;
	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->xdg_toplevel->base->surface->mapped || t->tile_float) {
			continue;
		}
		n_tile++;
	}
	if (n_tile == 0) {
		return NULL;
	}
	struct comp_toplevel **arr = calloc(n_tile, sizeof(*arr));
	if (!arr) {
		return NULL;
	}
	size_t i = 0;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->xdg_toplevel->base->surface->mapped || t->tile_float) {
			continue;
		}
		arr[i++] = t;
	}
	qsort(arr, n_tile, sizeof(*arr), cmp_toplevel_tile_order);
	*n_out = n_tile;
	return arr;
}

static int tile_sorted_index(struct comp_toplevel **arr, size_t n, struct comp_toplevel *v)
{
	for (size_t i = 0; i < n; i++) {
		if (arr[i] == v) {
			return (int)i;
		}
	}
	return -1;
}

static void toplevel_unmap(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, unmap);
	if (view->server->focused_toplevel == view) {
		view->server->focused_toplevel = NULL;
		wlr_seat_keyboard_notify_clear_focus(view->server->seat);
	}
	if (view->server->grabbed_toplevel == view) {
		view->server->grabbed_toplevel = NULL;
		view->server->grab = COMP_GRAB_NONE;
		view->server->swallow_left_release = false;
	}
	if ((view->server->layout == COMP_LAYOUT_TILE || view->server->layout == COMP_LAYOUT_SCROLL) &&
	    view->server->grab != COMP_GRAB_MOVE) {
		server_arrange_toplevels(view->server);
	}
}

static void toplevel_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, destroy);
	if (view->xdg_decoration) {
		wl_list_remove(&view->xdg_decoration_destroy.link);
		wl_list_remove(&view->xdg_decoration_request_mode.link);
		view->xdg_decoration = NULL;
	}
	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_app_id.link);
	wl_list_remove(&view->link);
	if (view->server->focused_toplevel == view) {
		view->server->focused_toplevel = NULL;
	}
	if (view->server->grabbed_toplevel == view) {
		view->server->grabbed_toplevel = NULL;
		view->server->grab = COMP_GRAB_NONE;
		view->server->swallow_left_release = false;
	}
	struct comp_server *srv = view->server;
	free(view);
	if ((srv->layout == COMP_LAYOUT_TILE || srv->layout == COMP_LAYOUT_SCROLL) &&
	    srv->grab != COMP_GRAB_MOVE) {
		server_arrange_toplevels(srv);
	}
}

static void toplevel_commit(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_surface *xdg = view->xdg_toplevel->base;
	log_xdg_state("commit", view);
	/* wlroots 0.19 asserts if we schedule configure before initialized. */
	if (xdg->initial_commit && xdg->initialized) {
		log_xdg_state("commit:set_size0x0", view);
		wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
	}
	toplevel_apply_decoration_mode(view);
}

static void toplevel_map(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, map);
	log_xdg_state("map", view);
	struct wlr_box *geo = &view->xdg_toplevel->base->geometry;

	struct wlr_output *out = wlr_output_layout_output_at(
		view->server->output_layout, view->server->cursor->x, view->server->cursor->y);
	struct wlr_box obox = {0, 0, 800, 600};
	struct comp_output *co = comp_output_from_wlr(view->server, out);
	if (co) {
		obox = co->layer_workarea;
	} else if (out) {
		wlr_output_layout_get_box(view->server->output_layout, out, &obox);
	}
	toplevel_refresh_tile_props(view);
	if (view->server->layout == COMP_LAYOUT_TILE || view->server->layout == COMP_LAYOUT_SCROLL) {
		if (view->tile_float) {
			int x = obox.x + (obox.width - geo->width) / 2;
			int y = obox.y + (obox.height - geo->height) / 2;
			wlr_scene_node_set_position(&view->scene_tree->node, x, y);
			wlr_scene_node_raise_to_top(&view->scene_tree->node);
			focus_toplevel(view->server, view);
			server_arrange_toplevels(view->server);
			server_sync_xdg_decorations(view->server);
			return;
		}
		focus_toplevel(view->server, view);
		server_arrange_toplevels(view->server);
		server_sync_xdg_decorations(view->server);
		return;
	}
	int x = obox.x + (obox.width - geo->width) / 2;
	int y = obox.y + (obox.height - geo->height) / 2;
	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	wlr_scene_node_raise_to_top(&view->scene_tree->node);
	focus_toplevel(view->server, view);
	server_sync_xdg_decorations(view->server);
}

static void toplevel_request_move(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_toplevel *view = wl_container_of(listener, view, request_move);
	begin_move(view->server, view, false);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
}

static void xdg_shell_new_toplevel(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, xdg_shell_new_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct comp_toplevel *view = calloc(1, sizeof(*view));
	view->server = server;
	view->xdg_toplevel = xdg_toplevel;
	view->scene_tree = wlr_scene_xdg_surface_create(server->windows_tree, xdg_toplevel->base);
	assert(view->scene_tree);
	view->scene_tree->node.data = view;
	view->tile_user_key = ++tile_user_key_gen;
	view->tile_float = false;
	view->tile_order = 0;

	view->set_title.notify = toplevel_handle_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
	view->set_app_id.notify = toplevel_handle_set_app_id;
	wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);

	view->map.notify = toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->request_move.notify = toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
	wl_list_insert(server->toplevels.prev, &view->link);
}

static void focus_toplevel(struct comp_server *server, struct comp_toplevel *toplevel)
{
	if (!toplevel || !toplevel->xdg_toplevel->base->surface->mapped) {
		return;
	}
	if (!toplevel->xdg_toplevel->base->initialized) {
		log_xdg_state("focus:skip-not-initialized", toplevel);
		return;
	}
	struct comp_toplevel *prev = server->focused_toplevel;
	if (prev == toplevel) {
		return;
	}
	if (prev && prev->xdg_toplevel->base->initialized) {
		log_xdg_state("focus:deactivate-prev", prev);
		wlr_xdg_toplevel_set_activated(prev->xdg_toplevel, false);
	}
	log_xdg_state("focus:activate-new", toplevel);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	if (server->layout == COMP_LAYOUT_STACK ||
	    ((server->layout == COMP_LAYOUT_TILE || server->layout == COMP_LAYOUT_SCROLL) && toplevel->tile_float)) {
		wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	}
	server->focused_toplevel = toplevel;
	scroll_sync_to_focused(server);

	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kbd = wlr_seat_get_keyboard(seat);
	struct wlr_surface *surf = toplevel->xdg_toplevel->base->surface;
	if (kbd) {
		wlr_seat_keyboard_notify_enter(seat, surf, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surf, NULL, 0, NULL);
	}
}

static void begin_move(struct comp_server *server, struct comp_toplevel *view, bool swallow_left_release)
{
	server->grab = COMP_GRAB_MOVE;
	server->grabbed_toplevel = view;
	server->swallow_left_release = swallow_left_release;
	server->grab_cursor_x = server->cursor->x;
	server->grab_cursor_y = server->cursor->y;
	server->grab_view_x = view->scene_tree->node.x;
	server->grab_view_y = view->scene_tree->node.y;
}

static void tile_grid_dims(size_t n, int *cols_out, int *rows_out)
{
	/* ~square grid: ceil(sqrt(n)) columns so both axes get space (not only full-height strips). */
	int cols = 1;
	while ((size_t)cols * (size_t)cols < n) {
		cols++;
	}
	int rows = (int)((n + (size_t)cols - 1) / (size_t)cols);
	*cols_out = cols;
	*rows_out = rows;
}

void server_arrange_toplevels(struct comp_server *server)
{
	if (server->layout != COMP_LAYOUT_TILE && server->layout != COMP_LAYOUT_SCROLL) {
		return;
	}
	struct wlr_box box;
	if (wl_list_length(&server->outputs) == 1) {
		struct comp_output *o = wl_container_of(server->outputs.next, o, link);
		box = o->layer_workarea;
	} else {
		wlr_output_layout_get_box(server->output_layout, NULL, &box);
	}
	if (box.width <= 0 || box.height <= 0) {
		return;
	}
	const bool anim_on = layout_anim_effective(server);
	size_t n_tile = 0;
	struct comp_toplevel **arr = tile_sorted_views(server, &n_tile);
	if (!arr) {
		struct comp_toplevel *u;
		wl_list_for_each(u, &server->toplevels, link) {
			u->layout_anim_tracked = false;
		}
		return;
	}

	struct comp_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (!t->xdg_toplevel->base->surface->mapped) {
			continue;
		}
		if (t->tile_float || !t->xdg_toplevel->base->initialized) {
			t->layout_anim_tracked = false;
		}
	}

	if (server->layout == COMP_LAYOUT_SCROLL) {
		if (server->scroll_index < 0) {
			server->scroll_index = 0;
		}
		if (server->scroll_index >= (int)n_tile) {
			server->scroll_index = (int)n_tile - 1;
		}
		for (size_t j = 0; j < n_tile; j++) {
			struct comp_toplevel *v = arr[j];
			if (!v->xdg_toplevel->base->initialized) {
				log_xdg_state("arrange-scroll:skip-not-initialized", v);
				continue;
			}
			const int rel = (int)j - server->scroll_index;
			const int x = box.x + rel * box.width;
			const int y = box.y;
			v->layout_tgt_x = x;
			v->layout_tgt_y = y;
			log_xdg_state("arrange-scroll:set_size", v);
			wlr_xdg_toplevel_set_size(v->xdg_toplevel, box.width, box.height);
			if (!anim_on || !v->layout_anim_tracked) {
				v->layout_anim_x = (double)x;
				v->layout_anim_y = (double)y;
				wlr_scene_node_set_position(&v->scene_tree->node, x, y);
				v->layout_anim_tracked = true;
			}
		}
		layout_anim_kick_outputs(server);
		free(arr);
		return;
	}
	int cols, rows;
	tile_grid_dims(n_tile, &cols, &rows);
	const int cell_w = box.width / cols;
	const int cell_h = box.height / rows;

	for (size_t j = 0; j < n_tile; j++) {
		struct comp_toplevel *v = arr[j];
		if (!v->xdg_toplevel->base->initialized) {
			log_xdg_state("arrange-tile:skip-not-initialized", v);
			continue;
		}
		const int col = (int)(j % (size_t)cols);
		const int row = (int)(j / (size_t)cols);
		const int x = box.x + col * cell_w;
		const int y = box.y + row * cell_h;
		const int w = (col == cols - 1) ? (box.x + box.width - x) : cell_w;
		const int h = (row == rows - 1) ? (box.y + box.height - y) : cell_h;
		v->layout_tgt_x = x;
		v->layout_tgt_y = y;
		log_xdg_state("arrange-tile:set_size", v);
		wlr_xdg_toplevel_set_size(v->xdg_toplevel, w, h);
		if (!anim_on || !v->layout_anim_tracked) {
			v->layout_anim_x = (double)x;
			v->layout_anim_y = (double)y;
			wlr_scene_node_set_position(&v->scene_tree->node, x, y);
			v->layout_anim_tracked = true;
		}
	}
	layout_anim_kick_outputs(server);
	free(arr);
}

void server_tile_move_focused_n(struct comp_server *server, int steps)
{
	if ((server->layout != COMP_LAYOUT_TILE && server->layout != COMP_LAYOUT_SCROLL) || steps == 0) {
		return;
	}
	struct comp_toplevel *f = server->focused_toplevel;
	if (!f || f->tile_float || !f->xdg_toplevel->base->surface->mapped) {
		return;
	}

	const int sig = steps > 0 ? 1 : -1;
	const int nabs = steps > 0 ? steps : -steps;
	for (int c = 0; c < nabs; c++) {
		size_t n = 0;
		struct comp_toplevel **sorted = tile_sorted_views(server, &n);
		if (!sorted || n < 2) {
			free(sorted);
			return;
		}
		const int i = tile_sorted_index(sorted, n, f);
		if (i < 0) {
			free(sorted);
			return;
		}
		const int j = i + sig;
		if (j < 0 || j >= (int)n) {
			free(sorted);
			return;
		}
		tile_swap_sort_keys(f, sorted[(size_t)j]);
		free(sorted);
	}
	server_arrange_toplevels(server);
}

void server_scroll_move(struct comp_server *server, int steps)
{
	if (server->layout != COMP_LAYOUT_SCROLL || steps == 0) {
		return;
	}
	size_t n = 0;
	struct comp_toplevel **arr = tile_sorted_views(server, &n);
	if (!arr || n == 0) {
		free(arr);
		return;
	}
	int idx = server->scroll_index + steps;
	if (idx < 0) {
		idx = 0;
	} else if (idx >= (int)n) {
		idx = (int)n - 1;
	}
	server->scroll_index = idx;
	free(arr);
	server_arrange_toplevels(server);
}

void server_tile_move_focused_edge(struct comp_server *server, bool to_first)
{
	if (server->layout != COMP_LAYOUT_TILE && server->layout != COMP_LAYOUT_SCROLL) {
		return;
	}
	struct comp_toplevel *f = server->focused_toplevel;
	if (!f || f->tile_float || !f->xdg_toplevel->base->surface->mapped) {
		return;
	}
	for (;;) {
		size_t n = 0;
		struct comp_toplevel **sorted = tile_sorted_views(server, &n);
		if (!sorted || n < 2) {
			free(sorted);
			return;
		}
		const int i = tile_sorted_index(sorted, n, f);
		if (i < 0) {
			free(sorted);
			return;
		}
		const int j = to_first ? i - 1 : i + 1;
		if (j < 0 || j >= (int)n) {
			free(sorted);
			break;
		}
		tile_swap_sort_keys(f, sorted[(size_t)j]);
		free(sorted);
	}
	server_arrange_toplevels(server);
}

void server_tile_move_focused_grid_vert(struct comp_server *server, int steps)
{
	if (server->layout != COMP_LAYOUT_TILE || steps == 0) {
		return;
	}
	struct comp_toplevel *f = server->focused_toplevel;
	if (!f || f->tile_float || !f->xdg_toplevel->base->surface->mapped) {
		return;
	}
	const int sig = steps > 0 ? 1 : -1;
	const int nabs = steps > 0 ? steps : -steps;
	for (int c = 0; c < nabs; c++) {
		size_t n = 0;
		struct comp_toplevel **sorted = tile_sorted_views(server, &n);
		if (!sorted || n < 2) {
			free(sorted);
			return;
		}
		int cols, rows;
		tile_grid_dims(n, &cols, &rows);
		(void)rows;
		const int i = tile_sorted_index(sorted, n, f);
		if (i < 0) {
			free(sorted);
			return;
		}
		const int j = sig > 0 ? i + cols : i - cols;
		if (j < 0 || j >= (int)n) {
			free(sorted);
			return;
		}
		tile_swap_sort_keys(f, sorted[(size_t)j]);
		free(sorted);
	}
	server_arrange_toplevels(server);
}

void server_tile_move_focused_grid_vert_edge(struct comp_server *server, bool to_top)
{
	if (server->layout != COMP_LAYOUT_TILE) {
		return;
	}
	struct comp_toplevel *f = server->focused_toplevel;
	if (!f || f->tile_float || !f->xdg_toplevel->base->surface->mapped) {
		return;
	}
	for (;;) {
		size_t n = 0;
		struct comp_toplevel **sorted = tile_sorted_views(server, &n);
		if (!sorted || n < 2) {
			free(sorted);
			return;
		}
		int cols, rows;
		tile_grid_dims(n, &cols, &rows);
		(void)rows;
		const int i = tile_sorted_index(sorted, n, f);
		if (i < 0) {
			free(sorted);
			return;
		}
		int j;
		if (to_top) {
			if (i < cols) {
				free(sorted);
				break;
			}
			j = i - cols;
		} else {
			if (i + cols >= (int)n) {
				free(sorted);
				break;
			}
			j = i + cols;
		}
		tile_swap_sort_keys(f, sorted[(size_t)j]);
		free(sorted);
	}
	server_arrange_toplevels(server);
}

void server_tile_move_focused_grid_horiz(struct comp_server *server, int steps)
{
	if (server->layout != COMP_LAYOUT_TILE || steps == 0) {
		return;
	}
	struct comp_toplevel *f = server->focused_toplevel;
	if (!f || f->tile_float || !f->xdg_toplevel->base->surface->mapped) {
		return;
	}
	const int sig = steps > 0 ? 1 : -1;
	const int nabs = steps > 0 ? steps : -steps;
	for (int c = 0; c < nabs; c++) {
		size_t n = 0;
		struct comp_toplevel **sorted = tile_sorted_views(server, &n);
		if (!sorted || n < 2) {
			free(sorted);
			return;
		}
		int cols, rows;
		tile_grid_dims(n, &cols, &rows);
		(void)rows;
		const int i = tile_sorted_index(sorted, n, f);
		if (i < 0) {
			free(sorted);
			return;
		}
		const int j = i + sig;
		if (j < 0 || j >= (int)n) {
			free(sorted);
			return;
		}
		if (j / cols != i / cols) {
			free(sorted);
			return;
		}
		tile_swap_sort_keys(f, sorted[(size_t)j]);
		free(sorted);
	}
	server_arrange_toplevels(server);
}

void server_tile_grid_run_command(struct comp_server *server, const char *cmd_in)
{
	char buf[256];
	if (!cmd_in) {
		return;
	}
	strncpy(buf, cmd_in, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';
	char *s = buf;
	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
		s++;
	}
	char *end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	if (!s[0]) {
		return;
	}

	if (!strcasecmp(s, "up")) {
		server_tile_move_focused_grid_vert(server, -1);
		return;
	}
	if (!strcasecmp(s, "down")) {
		server_tile_move_focused_grid_vert(server, 1);
		return;
	}
	if (!strcasecmp(s, "left")) {
		server_tile_move_focused_grid_horiz(server, -1);
		return;
	}
	if (!strcasecmp(s, "right")) {
		server_tile_move_focused_grid_horiz(server, 1);
		return;
	}
	if (!strcasecmp(s, "top")) {
		server_tile_move_focused_grid_vert_edge(server, true);
		return;
	}
	if (!strcasecmp(s, "bottom")) {
		server_tile_move_focused_grid_vert_edge(server, false);
		return;
	}

	char dir[32];
	int steps = 0;
	const int n = sscanf(s, "%31s %d", dir, &steps);
	if (n == 2 && steps > 0) {
		if (!strcasecmp(dir, "left")) {
			server_tile_move_focused_grid_horiz(server, -steps);
		} else if (!strcasecmp(dir, "right")) {
			server_tile_move_focused_grid_horiz(server, steps);
		} else if (!strcasecmp(dir, "up")) {
			server_tile_move_focused_grid_vert(server, -steps);
		} else if (!strcasecmp(dir, "down")) {
			server_tile_move_focused_grid_vert(server, steps);
		} else {
			wlr_log(WLR_INFO, "tile grid: unknown direction '%s'", dir);
		}
		return;
	}
	if (n == 2 && steps <= 0) {
		wlr_log(WLR_INFO, "tile grid: count after direction must be ≥1");
		return;
	}

	char *ep = NULL;
	const long v = strtol(s, &ep, 10);
	if (ep != s && (!ep || !*ep)) {
		server_tile_move_focused_grid_vert(server, (int)v);
		return;
	}
	wlr_log(WLR_INFO, "tile grid: unknown '%s'", s);
}

void server_set_layout(struct comp_server *server, enum comp_layout layout)
{
	if (server->layout == layout) {
		return;
	}
	server->layout = layout;
	if (layout == COMP_LAYOUT_TILE || layout == COMP_LAYOUT_SCROLL) {
		server_refresh_all_tile_props(server);
		if (layout == COMP_LAYOUT_SCROLL) {
			server->scroll_index = 0;
			scroll_sync_to_focused(server);
		}
		server_arrange_toplevels(server);
	} else {
		struct comp_toplevel *v;
		wl_list_for_each(v, &server->toplevels, link) {
			v->layout_anim_tracked = false;
		}
		server->layout_anim_last_ns = 0;
		if (server->focused_toplevel) {
			wlr_scene_node_raise_to_top(&server->focused_toplevel->scene_tree->node);
		}
	}
	server_sync_xdg_decorations(server);
	comp_config_sync_layout_env(server->layout);
}

void server_toggle_layout(struct comp_server *server)
{
	if (server->layout == COMP_LAYOUT_SCROLL) {
		server_set_layout(server, COMP_LAYOUT_STACK);
		return;
	}
	server_set_layout(server, server->layout == COMP_LAYOUT_TILE
		? COMP_LAYOUT_STACK : COMP_LAYOUT_TILE);
}

static bool ipc_socket_path(char *out, size_t out_sz)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !rt[0]) {
		return false;
	}
	const int n = snprintf(out, out_sz, "%s/stackcomp-ipc.sock", rt);
	if (n < 0 || (size_t)n >= out_sz) {
		return false;
	}
	return true;
}

/* Returns 0 if the line was delivered to a listening stackcomp, -1 otherwise. */
static int ipc_client_send_line(const char *line)
{
	char path[108];
	if (!ipc_socket_path(path, sizeof(path))) {
		return -1;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	socklen_t slen = (socklen_t)offsetof(struct sockaddr_un, sun_path) +
		(socklen_t)strlen(addr.sun_path);
	if (connect(fd, (struct sockaddr *)&addr, slen) < 0) {
		close(fd);
		return -1;
	}
	const size_t len = strlen(line);
	if (write(fd, line, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void ipc_process_line(struct comp_server *server, char *line)
{
	while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n') {
		line++;
	}
	if (!line[0]) {
		return;
	}
	char *end = line + strlen(line);
	while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';

	if (!strncmp(line, "layout ", 7)) {
		const char *rest = line + 7;
		while (*rest == ' ' || *rest == '\t') {
			rest++;
		}
		if (!strcmp(rest, "toggle")) {
			server_toggle_layout(server);
		} else if (!strcmp(rest, "tile")) {
			server_set_layout(server, COMP_LAYOUT_TILE);
		} else if (!strcmp(rest, "scroll")) {
			server_set_layout(server, COMP_LAYOUT_SCROLL);
		} else if (!strcmp(rest, "stack")) {
			server_set_layout(server, COMP_LAYOUT_STACK);
		} else {
			wlr_log(WLR_INFO, "ipc: unknown layout subcommand '%s'", rest);
		}
		return;
	}
	if (!strncmp(line, "tile move ", 10)) {
		const char *rest = line + 10;
		while (*rest == ' ' || *rest == '\t') {
			rest++;
		}
		if (!strcmp(rest, "prev")) {
			server_tile_move_focused_n(server, -1);
		} else if (!strcmp(rest, "next")) {
			server_tile_move_focused_n(server, 1);
		} else if (!strcmp(rest, "first")) {
			server_tile_move_focused_edge(server, true);
		} else if (!strcmp(rest, "last")) {
			server_tile_move_focused_edge(server, false);
		} else {
			char *end = NULL;
			long v = strtol(rest, &end, 10);
			if (end != rest && (!end || !*end)) {
				server_tile_move_focused_n(server, (int)v);
			} else {
				wlr_log(WLR_INFO, "ipc: unknown tile move '%s'", rest);
			}
		}
		return;
	}
	if (!strncmp(line, "tile grid ", 10)) {
		const char *rest = line + 10;
		while (*rest == ' ' || *rest == '\t') {
			rest++;
		}
		server_tile_grid_run_command(server, rest);
		return;
	}
	if (!strncmp(line, "scroll ", 7)) {
		const char *rest = line + 7;
		while (*rest == ' ' || *rest == '\t') {
			rest++;
		}
		if (!strncmp(rest, "move ", 5)) {
			rest += 5;
			while (*rest == ' ' || *rest == '\t') {
				rest++;
			}
		}
		if (!strcmp(rest, "prev") || !strcmp(rest, "left")) {
			server_scroll_move(server, -1);
		} else if (!strcmp(rest, "next") || !strcmp(rest, "right")) {
			server_scroll_move(server, 1);
		} else {
			char *end = NULL;
			long v = strtol(rest, &end, 10);
			if (end != rest && (!end || !*end)) {
				server_scroll_move(server, (int)v);
			} else {
				wlr_log(WLR_INFO, "ipc: unknown scroll '%s'", rest);
			}
		}
		return;
	}
	if (!strcasecmp(line, "reload config") || !strcasecmp(line, "reload")) {
		if (!server_reload_config(server)) {
			wlr_log(WLR_ERROR, "ipc: config reload failed");
		}
		return;
	}
	wlr_log(WLR_INFO, "ipc: unknown command '%s'", line);
}

static bool server_reload_config(struct comp_server *server)
{
	const char *path = server->config_path;
	char fallback[PATH_MAX];
	if (!path || !path[0]) {
		if (!comp_config_default_path(fallback, sizeof(fallback))) {
			wlr_log(WLR_ERROR, "reload: no config path and no default config file");
			return false;
		}
		path = fallback;
	}
	struct comp_config *new_cfg = NULL;
	if (!comp_config_load(path, &new_cfg)) {
		return false;
	}
	struct comp_config *old = server->config;
	server->config = new_cfg;
	comp_config_free(old);
	if (server->layout == COMP_LAYOUT_TILE || server->layout == COMP_LAYOUT_SCROLL) {
		server_refresh_all_tile_props(server);
		server_arrange_toplevels(server);
	}
	server_sync_xdg_decorations(server);
	comp_config_run_reload(server->config);
	wlr_log(WLR_INFO, "reload: loaded config from %s", path);
	return true;
}

static int ipc_on_listen(int fd, uint32_t mask, void *data)
{
	(void)mask;
	struct comp_server *server = data;
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			wlr_log_errno(WLR_ERROR, "ipc accept");
		}
		return 0;
	}
	char buf[512];
	ssize_t n = read(c, buf, sizeof(buf) - 1);
	close(c);
	if (n <= 0) {
		return 0;
	}
	buf[n] = '\0';
	ipc_process_line(server, buf);
	return 0;
}

static bool ipc_init(struct comp_server *server)
{
	if (!ipc_socket_path(server->ipc_socket_path, sizeof(server->ipc_socket_path))) {
		wlr_log(WLR_ERROR, "ipc: XDG_RUNTIME_DIR is not set or path too long");
		server->ipc_socket_path[0] = '\0';
		return false;
	}
	unlink(server->ipc_socket_path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: socket");
		return false;
	}
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strncpy(addr.sun_path, server->ipc_socket_path, sizeof(addr.sun_path) - 1);
	socklen_t slen = (socklen_t)offsetof(struct sockaddr_un, sun_path) + (socklen_t)strlen(addr.sun_path);
	if (bind(fd, (struct sockaddr *)&addr, slen) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: bind %s", server->ipc_socket_path);
		close(fd);
		server->ipc_socket_path[0] = '\0';
		return false;
	}
	if (listen(fd, 8) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: listen");
		close(fd);
		unlink(server->ipc_socket_path);
		server->ipc_socket_path[0] = '\0';
		return false;
	}
	server->ipc_listen_fd = fd;
	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
	server->ipc_event_source = wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, ipc_on_listen, server);
	if (!server->ipc_event_source) {
		wlr_log(WLR_ERROR, "ipc: wl_event_loop_add_fd failed");
		close(fd);
		unlink(server->ipc_socket_path);
		server->ipc_socket_path[0] = '\0';
		server->ipc_listen_fd = -1;
		return false;
	}
	wlr_log(WLR_INFO, "ipc: listening on %s", server->ipc_socket_path);
	return true;
}

static void ipc_fini(struct comp_server *server)
{
	if (server->ipc_event_source) {
		wl_event_source_remove(server->ipc_event_source);
		server->ipc_event_source = NULL;
	}
	if (server->ipc_listen_fd >= 0) {
		close(server->ipc_listen_fd);
		server->ipc_listen_fd = -1;
	}
	if (server->ipc_socket_path[0]) {
		unlink(server->ipc_socket_path);
		server->ipc_socket_path[0] = '\0';
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_keyboard *kbd = wl_container_of(listener, kbd, destroy);
	wl_list_remove(&kbd->destroy.link);
	wl_list_remove(&kbd->key.link);
	wl_list_remove(&kbd->modifiers.link);
	free(kbd);
}

/* wlr_keyboard_notify_key() re-emits keyboard->events.key; ignore nested calls. */
static int keyboard_key_dispatch_depth;

static void keyboard_handle_key(struct wl_listener *listener, void *data)
{
	if (keyboard_key_dispatch_depth > 0) {
		return;
	}
	keyboard_key_dispatch_depth++;

	struct comp_keyboard *kbd = wl_container_of(listener, kbd, key);
	struct wlr_keyboard_key_event *event = data;
	struct wlr_keyboard *wlr_kbd = wlr_keyboard_from_input_device(kbd->dev);

	uint32_t mods_filtered = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	if (wlr_kbd->xkb_state && wlr_kbd->keymap) {
		mods_filtered = wlr_keyboard_get_modifiers(wlr_kbd) & COMP_BIND_MOD_FILTER;
		/* wlroots uses Linux evdev codes; XKB uses evdev + 8. Never call xkb with a
		 * keycode outside this keymap's range (libxkbcommon can fault otherwise). */
		xkb_keycode_t xkbc = (xkb_keycode_t)((uint64_t)event->keycode + 8u);
		xkb_keycode_t lo = xkb_keymap_min_keycode(wlr_kbd->keymap);
		xkb_keycode_t hi = xkb_keymap_max_keycode(wlr_kbd->keymap);
		if (xkbc >= lo && xkbc <= hi) {
			sym = xkb_state_key_get_one_sym(wlr_kbd->xkb_state, xkbc);
		}
	}

	const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	if (pressed) {
		const char *kdbg = getenv("STACKCOMP_LOG_KEYS");
		if (kdbg && kdbg[0] && strcmp(kdbg, "0") != 0 && sym != XKB_KEY_NoSymbol) {
			char name[128];
			if (xkb_keysym_get_name(sym, name, sizeof(name)) < 0) {
				snprintf(name, sizeof(name), "(bad)");
			}
			wlr_log(WLR_INFO, "keyboard: keysym=%s mods=0x%x", name, mods_filtered);
		}
	}

	wlr_seat_set_keyboard(kbd->server->seat, wlr_kbd);
	wlr_keyboard_notify_key(wlr_kbd, event);
	if (pressed && comp_config_try_bindings(kbd->server->config, kbd->server, pressed, mods_filtered, sym)) {
		keyboard_key_dispatch_depth--;
		return;
	}
	wlr_seat_keyboard_notify_key(kbd->server->seat, event->time_msec, event->keycode, event->state);
	keyboard_key_dispatch_depth--;
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_keyboard *kbd = wl_container_of(listener, kbd, modifiers);
	struct wlr_keyboard *wlr_kbd = wlr_keyboard_from_input_device(kbd->dev);
	wlr_seat_set_keyboard(kbd->server->seat, wlr_kbd);
	wlr_seat_keyboard_notify_modifiers(kbd->server->seat, &wlr_kbd->modifiers);
}

static void server_new_input(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *dev = data;

	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: {
		struct wlr_keyboard *wlr_kbd = wlr_keyboard_from_input_device(dev);
		struct comp_keyboard *kbd = calloc(1, sizeof(*kbd));
		if (!kbd) {
			wlr_log(WLR_ERROR, "Out of memory allocating keyboard state");
			return;
		}
		kbd->server = server;
		kbd->dev = dev;

		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *map = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!map) {
			wlr_log(WLR_ERROR, "Failed to compile XKB keymap");
			exit(1);
		}
		wlr_keyboard_set_keymap(wlr_kbd, map);
		xkb_keymap_unref(map);
		xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(wlr_kbd, 25, 600);

		kbd->destroy.notify = keyboard_handle_destroy;
		wl_signal_add(&dev->events.destroy, &kbd->destroy);
		kbd->key.notify = keyboard_handle_key;
		wl_signal_add(&wlr_kbd->events.key, &kbd->key);
		kbd->modifiers.notify = keyboard_handle_modifiers;
		wl_signal_add(&wlr_kbd->events.modifiers, &kbd->modifiers);

		wlr_seat_set_keyboard(server->seat, wlr_kbd);
		wlr_seat_set_capabilities(server->seat,
			WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
		/* Do not wlr_cursor_attach_input_device(keyboard): only pointer/touch/tablet. */
		break;
	}
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(server->cursor, dev);
		wlr_seat_set_capabilities(server->seat,
			WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
		break;
	default:
		break;
	}
}

static void process_cursor_motion(struct comp_server *server, uint32_t time_msec)
{
	if (server->grab == COMP_GRAB_MOVE && server->grabbed_toplevel) {
		struct comp_toplevel *v = server->grabbed_toplevel;
		double dx = server->cursor->x - server->grab_cursor_x;
		double dy = server->cursor->y - server->grab_cursor_y;
		wlr_scene_node_set_position(&v->scene_tree->node,
			server->grab_view_x + (int)dx, server->grab_view_y + (int)dy);
	}

	double sx, sy;
	struct wlr_surface *surface = surface_at(server, server->cursor->x, server->cursor->y, &sx, &sy);

	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		return;
	}
	wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
}

static void server_cursor_motion(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(server->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	process_cursor_motion(server, ev->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(server->cursor, &ev->pointer->base, ev->x, ev->y);
	process_cursor_motion(server, ev->time_msec);
}

static void layer_surface_try_keyboard_focus_click(struct comp_server *server, double lx, double ly)
{
	double sx, sy;
	struct wlr_surface *surf = surface_at(server, lx, ly, &sx, &sy);
	if (!surf) {
		return;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surf);
	struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (!ls || !ls->surface->mapped) {
		return;
	}
	const enum zwlr_layer_surface_v1_keyboard_interactivity ki = ls->current.keyboard_interactive;
	const bool top_or_overlay = ls->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
		ls->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	if (ki == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		return;
	}
	if (ki == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE && !top_or_overlay) {
		return;
	}
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kbd = wlr_seat_get_keyboard(seat);
	if (kbd) {
		wlr_seat_keyboard_notify_enter(seat, ls->surface, kbd->keycodes, kbd->num_keycodes, &kbd->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, ls->surface, NULL, 0, NULL);
	}
}

static void server_cursor_button(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *ev = data;

	uint32_t mods = 0;
	struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
	if (kbd) {
		mods = wlr_keyboard_get_modifiers(kbd);
	}

	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED && server->grab == COMP_GRAB_MOVE) {
		struct comp_toplevel *dragged = server->grabbed_toplevel;
		server->grab = COMP_GRAB_NONE;
		server->grabbed_toplevel = NULL;
		if ((server->layout == COMP_LAYOUT_TILE || server->layout == COMP_LAYOUT_SCROLL) && dragged) {
			double sx, sy;
			struct comp_toplevel *drop =
				toplevel_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
			if (drop && drop != dragged && !dragged->tile_float && !drop->tile_float) {
				tile_swap_sort_keys(dragged, drop);
			}
			if (dragged->layout_anim_tracked) {
				dragged->layout_anim_x = (double)dragged->scene_tree->node.x;
				dragged->layout_anim_y = (double)dragged->scene_tree->node.y;
			}
			server_arrange_toplevels(server);
		}
		if (ev->button == BTN_LEFT && server->swallow_left_release) {
			server->swallow_left_release = false;
			process_cursor_motion(server, ev->time_msec);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);
		process_cursor_motion(server, ev->time_msec);
		return;
	}

	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && ev->button == BTN_LEFT &&
	    (mods & WLR_MODIFIER_LOGO)) {
		double sx, sy;
		struct comp_toplevel *v = toplevel_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
		if (v) {
			begin_move(server, v, true);
			focus_toplevel(server, v);
			return;
		}
	}

	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED && ev->button == BTN_LEFT) {
		double sx, sy;
		struct comp_toplevel *v = toplevel_at(server, server->cursor->x, server->cursor->y, &sx, &sy);
		if (v) {
			focus_toplevel(server, v);
		} else {
			layer_surface_try_keyboard_focus_click(server, server->cursor->x, server->cursor->y);
		}
	}

	wlr_seat_pointer_notify_button(server->seat, ev->time_msec, ev->button, ev->state);
}

static void server_cursor_axis(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(server->seat, ev->time_msec, ev->orientation,
		ev->delta, ev->delta_discrete, ev->source, ev->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data)
{
	(void)data;
	struct comp_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
}

static void seat_request_cursor(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, seat_request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	wlr_cursor_set_surface(server->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data)
{
	struct comp_server *server = wl_container_of(listener, server, seat_request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(server->seat, ev->source, ev->serial);
}

bool server_init(struct comp_server *server)
{
	server->wl_display = wl_display_create();
	if (!server->wl_display) {
		return false;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);

	server->backend = wlr_backend_autocreate(loop, &server->session);
	if (!server->backend) {
		return false;
	}
	server->renderer = wlr_renderer_autocreate(server->backend);
	if (!server->renderer) {
		return false;
	}
	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

	server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (!server->allocator) {
		return false;
	}

	struct wl_display *dpy = server->wl_display;
	server->compositor = wlr_compositor_create(dpy, 6, server->renderer);
	server->subcompositor = wlr_subcompositor_create(dpy);
	server->data_device_mgr = wlr_data_device_manager_create(dpy);
	server->output_layout = wlr_output_layout_create(dpy);
	server->xdg_output_manager = wlr_xdg_output_manager_v1_create(dpy, server->output_layout);
	if (!server->xdg_output_manager) {
		return false;
	}

	server->screencopy_manager = wlr_screencopy_manager_v1_create(dpy);
	if (!server->screencopy_manager) {
		wlr_log(WLR_ERROR, "Failed to create wlr_screencopy_manager_v1");
		return false;
	}

	server->scene = wlr_scene_create();
	server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
		wlr_scene_tree_create(&server->scene->tree);
	server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
	server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
		wlr_scene_tree_create(&server->scene->tree);
	server->windows_tree = wlr_scene_tree_create(&server->scene->tree);
	server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&server->scene->tree);
	server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
		wlr_scene_tree_create(&server->scene->tree);

	server->xdg_shell = wlr_xdg_shell_create(dpy, 3);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);
	server->xdg_shell_new_toplevel.notify = xdg_shell_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->xdg_shell_new_toplevel);

	server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(dpy);
	if (!server->xdg_decoration_manager) {
		wlr_log(WLR_ERROR, "Failed to create wlr_xdg_decoration_manager_v1");
		return false;
	}
	server->new_xdg_decoration.notify = xdg_new_toplevel_decoration;
	wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->new_xdg_decoration);

	server->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
	server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	if (!server->cursor_mgr) {
		return false;
	}

	server->layer_shell = wlr_layer_shell_v1_create(dpy, 4);
	if (!server->layer_shell) {
		return false;
	}
	wl_list_init(&server->layers);
	server->layer_shell_new_surface.notify = layer_shell_new_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->layer_shell_new_surface);

	server->seat = wlr_seat_create(dpy, "seat0");
	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);
	server->seat_request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->seat_request_cursor);
	server->seat_request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->seat_request_set_selection);

	wl_list_init(&server->outputs);
	wl_list_init(&server->toplevels);
	server->ipc_listen_fd = -1;
	server->ipc_socket_path[0] = '\0';
	server->grab = COMP_GRAB_NONE;
	if (server->ipc_enabled && !ipc_init(server)) {
		wlr_log(WLR_ERROR, "ipc: disabled (initialization failed)");
		server->ipc_enabled = false;
	}
	return true;
}

static void server_finish(struct comp_server *server)
{
	if (compositor_session_active && server->config) {
		comp_config_run_shutdown(server->config);
	}
	compositor_session_active = false;
	ipc_fini(server);
	comp_config_free(server->config);
	server->config = NULL;
	free(server->config_path);
	server->config_path = NULL;
}

int main(int argc, char **argv)
{
	wlr_log_init(WLR_INFO, NULL);
	{
		const char *e = getenv("STACKCOMP_DEBUG_XDG");
		xdg_debug_logs_enabled = e && e[0] && strcmp(e, "0") != 0;
	}
	/*
	 * Some parent processes leave SIGCHLD ignored; the kernel then auto-reaps
	 * children and waitpid() in when= / shutdown hooks fails with ECHILD.
	 */
	if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
		wlr_log_errno(WLR_ERROR, "signal(SIGCHLD, SIG_DFL)");
		return 1;
	}

	const char *cfg_path = getenv("STACKCOMP_CONFIG");
	char cfg_buf[PATH_MAX];
	enum comp_layout initial_layout = COMP_LAYOUT_STACK;
	bool layout_from_argv = false;
	bool tile_move_from_argv = false;
	char tile_move_line[64];
	bool tile_grid_from_argv = false;
	char tile_grid_line[64];
	bool scroll_move_from_argv = false;
	char scroll_move_line[64];
	bool no_ipc = false;
	bool reload_config_from_argv = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
			if (i + 1 >= argc) {
				wlr_log(WLR_ERROR, "Missing path after %s", argv[i]);
				return 1;
			}
			cfg_path = argv[++i];
		} else if (!strcmp(argv[i], "--layout")) {
			if (i + 1 >= argc) {
				wlr_log(WLR_ERROR, "Missing value after --layout");
				return 1;
			}
			const char *v = argv[++i];
			if (!strcasecmp(v, "tile")) {
				initial_layout = COMP_LAYOUT_TILE;
			} else if (!strcasecmp(v, "scroll")) {
				initial_layout = COMP_LAYOUT_SCROLL;
			} else if (!strcasecmp(v, "stack")) {
				initial_layout = COMP_LAYOUT_STACK;
			} else {
				wlr_log(WLR_ERROR, "Unknown --layout %s (use stack, tile, or scroll)", v);
				return 1;
			}
			layout_from_argv = true;
		} else if (!strcmp(argv[i], "--scroll")) {
			initial_layout = COMP_LAYOUT_SCROLL;
			layout_from_argv = true;
		} else if (!strcmp(argv[i], "--tile-move")) {
			if (i + 1 >= argc) {
				wlr_log(WLR_ERROR, "Missing value after --tile-move");
				return 1;
			}
			const char *v = argv[++i];
			if (!strcasecmp(v, "prev") || !strcasecmp(v, "left")) {
				snprintf(tile_move_line, sizeof(tile_move_line), "tile move prev\n");
			} else if (!strcasecmp(v, "next") || !strcasecmp(v, "right")) {
				snprintf(tile_move_line, sizeof(tile_move_line), "tile move next\n");
			} else if (!strcasecmp(v, "first")) {
				snprintf(tile_move_line, sizeof(tile_move_line), "tile move first\n");
			} else if (!strcasecmp(v, "last")) {
				snprintf(tile_move_line, sizeof(tile_move_line), "tile move last\n");
			} else {
				char *end = NULL;
				(void)strtol(v, &end, 10);
				if (!end || end == v || *end) {
					wlr_log(WLR_ERROR,
						"Unknown --tile-move %s (use prev, next, left, right, first, last, or a signed integer)",
						v);
					return 1;
				}
				snprintf(tile_move_line, sizeof(tile_move_line), "tile move %s\n", v);
			}
			tile_move_from_argv = true;
		} else if (!strcmp(argv[i], "--tile-grid")) {
			if (i + 1 >= argc) {
				wlr_log(WLR_ERROR, "Missing value after --tile-grid");
				return 1;
			}
			const char *v = argv[++i];
			if (i + 1 < argc) {
				const char *v2 = argv[i + 1];
				char *e2 = NULL;
				const long cnt = strtol(v2, &e2, 10);
				if (e2 != v2 && *e2 == '\0' && cnt > 0 &&
				    (!strcasecmp(v, "left") || !strcasecmp(v, "right") || !strcasecmp(v, "up") ||
				     !strcasecmp(v, "down"))) {
					snprintf(tile_grid_line, sizeof(tile_grid_line), "tile grid %s %s\n", v, v2);
					i++;
					tile_grid_from_argv = true;
					continue;
				}
			}
			if (!strcasecmp(v, "up") || !strcasecmp(v, "down") || !strcasecmp(v, "left") ||
			    !strcasecmp(v, "right") || !strcasecmp(v, "top") || !strcasecmp(v, "bottom")) {
				snprintf(tile_grid_line, sizeof(tile_grid_line), "tile grid %s\n", v);
			} else {
				char *end = NULL;
				(void)strtol(v, &end, 10);
				if (!end || end == v || *end) {
					wlr_log(WLR_ERROR,
						"Unknown --tile-grid %s (use up, down, left, right, top, bottom, DIR COUNT, or a signed integer)",
						v);
					return 1;
				}
				snprintf(tile_grid_line, sizeof(tile_grid_line), "tile grid %s\n", v);
			}
			tile_grid_from_argv = true;
		} else if (!strcmp(argv[i], "--scroll-move")) {
			if (i + 1 >= argc) {
				wlr_log(WLR_ERROR, "Missing value after --scroll-move");
				return 1;
			}
			const char *v = argv[++i];
			if (!strcasecmp(v, "prev") || !strcasecmp(v, "left")) {
				snprintf(scroll_move_line, sizeof(scroll_move_line), "scroll prev\n");
			} else if (!strcasecmp(v, "next") || !strcasecmp(v, "right")) {
				snprintf(scroll_move_line, sizeof(scroll_move_line), "scroll next\n");
			} else {
				char *end = NULL;
				(void)strtol(v, &end, 10);
				if (!end || end == v || *end) {
					wlr_log(WLR_ERROR,
						"Unknown --scroll-move %s (use prev, next, left, right, or a signed integer)",
						v);
					return 1;
				}
				snprintf(scroll_move_line, sizeof(scroll_move_line), "scroll %s\n", v);
			}
			scroll_move_from_argv = true;
		} else if (!strcmp(argv[i], "--ipc")) {
			/* IPC is default-on when XDG_RUNTIME_DIR is set; flag kept for scripts. */
		} else if (!strcmp(argv[i], "--no-ipc")) {
			no_ipc = true;
		} else if (!strcmp(argv[i], "--reload-config")) {
			reload_config_from_argv = true;
		} else {
			wlr_log(WLR_ERROR, "Unknown argument: %s", argv[i]);
			return 1;
		}
	}
	if (reload_config_from_argv) {
		if (ipc_client_send_line("reload config\n") != 0) {
			wlr_log(WLR_ERROR, "No running stackcomp or IPC failed for --reload-config");
			return 1;
		}
		wlr_log(WLR_INFO, "Sent reload config to running stackcomp");
		return 0;
	}
	if (tile_move_from_argv) {
		if (ipc_client_send_line(tile_move_line) != 0) {
			wlr_log(WLR_ERROR, "No running stackcomp or IPC failed for --tile-move");
			return 1;
		}
		wlr_log(WLR_INFO, "Sent tile move to running stackcomp via IPC");
	}
	if (tile_grid_from_argv) {
		if (ipc_client_send_line(tile_grid_line) != 0) {
			wlr_log(WLR_ERROR, "No running stackcomp or IPC failed for --tile-grid");
			return 1;
		}
		wlr_log(WLR_INFO, "Sent tile grid to running stackcomp via IPC");
	}
	if (scroll_move_from_argv) {
		if (ipc_client_send_line(scroll_move_line) != 0) {
			wlr_log(WLR_ERROR, "No running stackcomp or IPC failed for --scroll-move");
			return 1;
		}
		wlr_log(WLR_INFO, "Sent scroll move to running stackcomp via IPC");
	}
	if (layout_from_argv) {
		char line[48];
		const char *layout_word = initial_layout == COMP_LAYOUT_TILE ? "tile" :
			(initial_layout == COMP_LAYOUT_SCROLL ? "scroll" : "stack");
		snprintf(line, sizeof(line), "layout %s\n", layout_word);
		if (ipc_client_send_line(line) == 0) {
			wlr_log(WLR_INFO, "Applied layout to running stackcomp via IPC");
			return 0;
		}
	}
	if (tile_move_from_argv || tile_grid_from_argv || scroll_move_from_argv) {
		return 0;
	}
	if (!cfg_path && comp_config_default_path(cfg_buf, sizeof(cfg_buf))) {
		cfg_path = cfg_buf;
	}

	struct comp_config *cfg = NULL;
	if (!comp_config_load(cfg_path, &cfg)) {
		wlr_log(WLR_ERROR, "Failed to load keybind config");
		return 1;
	}

	char ipc_probe[108];
	struct comp_server server = {0};
	server.config = cfg;
	server.layout = initial_layout;
	comp_config_sync_layout_env(server.layout);
	server.ipc_enabled = !no_ipc && ipc_socket_path(ipc_probe, sizeof(ipc_probe));
	if (cfg_path && cfg_path[0]) {
		server.config_path = strdup(cfg_path);
	}
	if (!server_init(&server)) {
		wlr_log(WLR_ERROR, "Failed to initialize compositor");
		free(server.config_path);
		server.config_path = NULL;
		comp_config_free(cfg);
		return 1;
	}

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "Unable to add Wayland socket");
		server_finish(&server);
		wl_display_destroy_clients(server.wl_display);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "Failed to start backend");
		server_finish(&server);
		wl_display_destroy_clients(server.wl_display);
		wl_display_destroy(server.wl_display);
		return 1;
	}
	compositor_session_active = true;
	setenv("WAYLAND_DISPLAY", socket, true);
	wlr_log(WLR_INFO, "Running compositor on WAYLAND_DISPLAY=%s", socket);
	comp_config_run_startup(server.config);

	wl_display_run(server.wl_display);

	server_finish(&server);
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
