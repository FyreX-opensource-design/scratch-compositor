#include "ext_workspace.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "ext-workspace-v1-protocol.h"
#include "server.h"

struct comp_ext_workspace_session {
	struct wl_list link;
	struct comp_server *server;
	struct wl_resource *manager;
	struct wl_resource *group;
	struct wl_resource *handles[COMP_WORKSPACE_COUNT];
	bool on_sessions_list;
};

struct comp_ext_ws_handle_ud {
	struct comp_ext_workspace_session *session;
	int index;
};

struct ext_out_track {
	struct wl_list link;
	struct wlr_output *wlr;
	struct comp_server *server;
	struct wl_listener bind;
	struct wl_listener destroy;
};

static struct wl_global *workspace_global;
static struct wl_list sessions;
static struct wl_list output_tracks;

static struct wl_resource *output_resource_for_client(struct wlr_output *wlr_out, struct wl_client *client)
{
	struct wl_resource *r;
	wl_resource_for_each(r, &wlr_out->resources) {
		if (wl_resource_get_client(r) == client) {
			return r;
		}
	}
	return NULL;
}

static void session_send_workspace_states(struct comp_ext_workspace_session *s)
{
	for (int i = 0; i < COMP_WORKSPACE_COUNT; i++) {
		struct wl_resource *wsr = s->handles[i];
		if (!wsr) {
			continue;
		}
		const uint32_t st = s->server->current_workspace == i ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0;
		ext_workspace_handle_v1_send_state(wsr, st);
	}
	/*
	 * ext-workspace-v1: clients should treat handle updates as atomic once they
	 * receive manager.done (same as the initial burst). Waybar's ext/workspaces
	 * refreshes active styling when done arrives, not only on each state event.
	 */
	if (s->manager) {
		ext_workspace_manager_v1_send_done(s->manager);
	}
}

void ext_workspace_notify(struct comp_server *server)
{
	(void)server;
	struct comp_ext_workspace_session *s;
	wl_list_for_each(s, &sessions, link) {
		session_send_workspace_states(s);
	}
}

static void handle_resource_destroy(struct wl_resource *resource)
{
	struct comp_ext_ws_handle_ud *ud = wl_resource_get_user_data(resource);
	if (!ud) {
		return;
	}
	if (ud->session && ud->index >= 0 && ud->index < COMP_WORKSPACE_COUNT) {
		ud->session->handles[ud->index] = NULL;
	}
	free(ud);
}

static void handle_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static void handle_activate(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	struct comp_ext_ws_handle_ud *ud = wl_resource_get_user_data(resource);
	if (!ud || !ud->session || !ud->session->server) {
		return;
	}
	server_workspace_go(ud->session->server, ud->index);
}

static void handle_deactivate(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void handle_assign(struct wl_client *client, struct wl_resource *resource,
	struct wl_resource *workspace_group)
{
	(void)client;
	(void)resource;
	(void)workspace_group;
}

static void handle_remove(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static const struct ext_workspace_handle_v1_interface handle_interface = {
	.destroy = handle_destroy_request,
	.activate = handle_activate,
	.deactivate = handle_deactivate,
	.assign = handle_assign,
	.remove = handle_remove,
};

static void group_create_workspace(struct wl_client *client, struct wl_resource *resource, const char *workspace)
{
	(void)client;
	(void)resource;
	(void)workspace;
}

static void group_resource_destroy(struct wl_resource *resource)
{
	struct comp_ext_workspace_session *s = wl_resource_get_user_data(resource);
	if (s) {
		s->group = NULL;
	}
}

static void group_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	wl_resource_destroy(resource);
}

static const struct ext_workspace_group_handle_v1_interface group_interface = {
	.create_workspace = group_create_workspace,
	.destroy = group_destroy_request,
};

static void manager_commit(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	(void)resource;
}

static void manager_stop(struct wl_client *client, struct wl_resource *resource)
{
	(void)client;
	ext_workspace_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);
}

static void manager_destroy(struct wl_resource *resource)
{
	struct comp_ext_workspace_session *s = wl_resource_get_user_data(resource);
	if (!s) {
		return;
	}
	if (s->on_sessions_list) {
		wl_list_remove(&s->link);
		s->on_sessions_list = false;
	}
	for (int i = 0; i < COMP_WORKSPACE_COUNT; i++) {
		if (s->handles[i]) {
			wl_resource_destroy(s->handles[i]);
			s->handles[i] = NULL;
		}
	}
	if (s->group) {
		wl_resource_destroy(s->group);
		s->group = NULL;
	}
	free(s);
}

static const struct ext_workspace_manager_v1_interface manager_interface = {
	.commit = manager_commit,
	.stop = manager_stop,
};

static bool send_session_initial(struct comp_ext_workspace_session *sess, struct wl_client *client,
	struct wl_resource *man, int version)
{
	struct wl_resource *grp = wl_resource_create(client, &ext_workspace_group_handle_v1_interface, version, 0);
	if (!grp) {
		wl_client_post_no_memory(client);
		return false;
	}
	wl_resource_set_implementation(grp, &group_interface, sess, group_resource_destroy);
	ext_workspace_manager_v1_send_workspace_group(man, grp);
	ext_workspace_group_handle_v1_send_capabilities(grp, 0);
	sess->group = grp;

	struct comp_output *o;
	wl_list_for_each(o, &sess->server->outputs, link) {
		struct wl_resource *out_res = output_resource_for_client(o->wlr_output, client);
		if (out_res) {
			ext_workspace_group_handle_v1_send_output_enter(grp, out_res);
		}
	}

	for (int i = 0; i < COMP_WORKSPACE_COUNT; i++) {
		struct wl_resource *wsr = wl_resource_create(client, &ext_workspace_handle_v1_interface, version, 0);
		if (!wsr) {
			wl_client_post_no_memory(client);
			return false;
		}
		struct comp_ext_ws_handle_ud *ud = calloc(1, sizeof(*ud));
		if (!ud) {
			wl_resource_destroy(wsr);
			wl_client_post_no_memory(client);
			return false;
		}
		ud->session = sess;
		ud->index = i;
		wl_resource_set_implementation(wsr, &handle_interface, ud, handle_resource_destroy);
		ext_workspace_manager_v1_send_workspace(man, wsr);
		char idbuf[40];
		snprintf(idbuf, sizeof(idbuf), "stackcomp-ws-%d", i);
		ext_workspace_handle_v1_send_id(wsr, idbuf);
		char namebuf[16];
		snprintf(namebuf, sizeof(namebuf), "%d", i + 1);
		ext_workspace_handle_v1_send_name(wsr, namebuf);
		struct wl_array arr;
		wl_array_init(&arr);
		uint32_t *coord = wl_array_add(&arr, sizeof(*coord));
		if (!coord) {
			wl_array_release(&arr);
			wl_client_post_no_memory(client);
			return false;
		}
		*coord = (uint32_t)(i + 1);
		ext_workspace_handle_v1_send_coordinates(wsr, &arr);
		wl_array_release(&arr);
		const uint32_t st = sess->server->current_workspace == i ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0;
		ext_workspace_handle_v1_send_state(wsr, st);
		ext_workspace_handle_v1_send_capabilities(wsr,
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		ext_workspace_group_handle_v1_send_workspace_enter(grp, wsr);
		sess->handles[i] = wsr;
	}
	ext_workspace_manager_v1_send_done(man);
	return true;
}

static void ext_workspace_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct comp_server *server = data;
	int ver = (int)version > 1 ? 1 : (int)version;
	struct comp_ext_workspace_session *sess = calloc(1, sizeof(*sess));
	if (!sess) {
		wl_client_post_no_memory(client);
		return;
	}
	sess->server = server;
	struct wl_resource *man = wl_resource_create(client, &ext_workspace_manager_v1_interface, ver, id);
	if (!man) {
		free(sess);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(man, &manager_interface, sess, manager_destroy);
	sess->manager = man;
	if (!send_session_initial(sess, client, man, ver)) {
		wl_resource_destroy(man);
		return;
	}
	sess->on_sessions_list = true;
	wl_list_insert(&sessions, &sess->link);
}

static void output_bind_notify(struct wl_listener *listener, void *data)
{
	struct ext_out_track *track = wl_container_of(listener, track, bind);
	(void)track;
	struct wlr_output_event_bind *ev = data;
	struct comp_ext_workspace_session *s;
	wl_list_for_each(s, &sessions, link) {
		if (wl_resource_get_client(s->manager) == wl_resource_get_client(ev->resource) && s->group) {
			ext_workspace_group_handle_v1_send_output_enter(s->group, ev->resource);
		}
	}
}

static void output_track_destroy(struct wl_listener *listener, void *data)
{
	(void)data;
	struct ext_out_track *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->bind.link);
	wl_list_remove(&t->destroy.link);
	wl_list_remove(&t->link);
	free(t);
}

void ext_workspace_on_output_new(struct comp_server *server, struct wlr_output *wlr_output)
{
	struct ext_out_track *t = calloc(1, sizeof(*t));
	if (!t) {
		return;
	}
	t->wlr = wlr_output;
	t->server = server;
	t->bind.notify = output_bind_notify;
	t->destroy.notify = output_track_destroy;
	wl_signal_add(&wlr_output->events.bind, &t->bind);
	wl_signal_add(&wlr_output->events.destroy, &t->destroy);
	wl_list_insert(&output_tracks, &t->link);

	struct comp_ext_workspace_session *s;
	wl_list_for_each(s, &sessions, link) {
		struct wl_client *c = wl_resource_get_client(s->manager);
		struct wl_resource *out_res = output_resource_for_client(wlr_output, c);
		if (out_res && s->group) {
			ext_workspace_group_handle_v1_send_output_enter(s->group, out_res);
		}
	}
}

void ext_workspace_on_output_remove(struct comp_server *server, struct wlr_output *wlr_output)
{
	(void)server;
	struct ext_out_track *t, *tmp;
	wl_list_for_each_safe(t, tmp, &output_tracks, link) {
		if (t->wlr != wlr_output) {
			continue;
		}
		struct comp_ext_workspace_session *s;
		wl_list_for_each(s, &sessions, link) {
			struct wl_resource *out_res = output_resource_for_client(wlr_output,
				wl_resource_get_client(s->manager));
			if (out_res && s->group) {
				ext_workspace_group_handle_v1_send_output_leave(s->group, out_res);
			}
		}
		wl_list_remove(&t->bind.link);
		wl_list_remove(&t->destroy.link);
		wl_list_remove(&t->link);
		free(t);
		break;
	}
}

void ext_workspace_init(struct comp_server *server)
{
	wl_list_init(&sessions);
	wl_list_init(&output_tracks);
	workspace_global = wl_global_create(server->wl_display, &ext_workspace_manager_v1_interface, 1, server,
		ext_workspace_bind);
	if (!workspace_global) {
		wlr_log(WLR_ERROR, "ext_workspace: wl_global_create failed");
		return;
	}
	wlr_log(WLR_INFO, "ext_workspace_manager_v1 advertised");
}

void ext_workspace_fini(struct comp_server *server)
{
	(void)server;
	if (workspace_global) {
		wl_global_destroy(workspace_global);
		workspace_global = NULL;
	}
	struct ext_out_track *t, *tt;
	wl_list_for_each_safe(t, tt, &output_tracks, link) {
		wl_list_remove(&t->bind.link);
		wl_list_remove(&t->destroy.link);
		wl_list_remove(&t->link);
		free(t);
	}
	wl_list_init(&output_tracks);
}
