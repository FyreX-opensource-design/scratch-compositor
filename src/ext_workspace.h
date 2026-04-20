#pragma once

struct comp_server;
struct wlr_output;

void ext_workspace_init(struct comp_server *server);
void ext_workspace_fini(struct comp_server *server);

/** Broadcast active workspace state to ext_workspace clients (after current_workspace changes). */
void ext_workspace_notify(struct comp_server *server);

void ext_workspace_on_output_new(struct comp_server *server, struct wlr_output *wlr_output);
void ext_workspace_on_output_remove(struct comp_server *server, struct wlr_output *wlr_output);
