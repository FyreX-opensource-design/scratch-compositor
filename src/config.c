#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "server.h"

static char *xstrdup(const char *s)
{
	if (!s) {
		return NULL;
	}
	return strdup(s);
}

static void trim_inplace(char *s)
{
	char *end;
	while (*s && isspace((unsigned char)*s)) {
		memmove(s, s + 1, strlen(s) + 1);
	}
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1])) {
		*--end = '\0';
	}
}

static bool append_bind(struct comp_config *cfg, struct comp_keybind *b)
{
	struct comp_keybind *n = realloc(cfg->binds, (cfg->n_binds + 1) * sizeof(*n));
	if (!n) {
		return false;
	}
	cfg->binds = n;
	cfg->binds[cfg->n_binds++] = *b;
	memset(b, 0, sizeof(*b));
	return true;
}

static void keybind_clear(struct comp_keybind *b)
{
	free(b->command);
	free(b->when_shell);
	memset(b, 0, sizeof(*b));
}

static void tile_rule_destroy(struct comp_tile_rule *r)
{
	if (r->have_app_id) {
		regfree(&r->app_id_re);
	}
	if (r->have_title) {
		regfree(&r->title_re);
	}
	memset(r, 0, sizeof(*r));
}

void comp_config_free(struct comp_config *cfg)
{
	if (!cfg) {
		return;
	}
	for (size_t i = 0; i < cfg->n_binds; i++) {
		keybind_clear(&cfg->binds[i]);
	}
	free(cfg->binds);
	for (size_t i = 0; i < cfg->n_tile_rules; i++) {
		tile_rule_destroy(&cfg->tile_rules[i]);
	}
	free(cfg->tile_rules);
	free(cfg->hook_startup);
	free(cfg->hook_shutdown);
	free(cfg->hook_reload);
	free(cfg);
}

static uint32_t parse_mod_token(const char *tok)
{
	if (!strcasecmp(tok, "shift")) {
		return WLR_MODIFIER_SHIFT;
	}
	if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) {
		return WLR_MODIFIER_CTRL;
	}
	if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "meta")) {
		return WLR_MODIFIER_ALT;
	}
	if (!strcasecmp(tok, "super") || !strcasecmp(tok, "mod") || !strcasecmp(tok, "logo") ||
	    !strcasecmp(tok, "win") || !strcasecmp(tok, "mod4")) {
		return WLR_MODIFIER_LOGO;
	}
	return 0;
}

static bool parse_mods_string(const char *value, uint32_t *out)
{
	char *buf = strdup(value);
	if (!buf) {
		return false;
	}
	*out = 0;
	for (char *tok = strtok(buf, "+, \t"); tok; tok = strtok(NULL, "+, \t")) {
		uint32_t m = parse_mod_token(tok);
		if (!m) {
			wlr_log(WLR_ERROR, "Unknown modifier token '%s' in mods= line", tok);
			free(buf);
			return false;
		}
		*out |= m;
	}
	free(buf);
	return true;
}

static bool parse_action(const char *v, enum comp_keybind_action *a)
{
	if (!strcasecmp(v, "quit") || !strcasecmp(v, "exit")) {
		*a = COMP_KEYBIND_QUIT;
		return true;
	}
	if (!strcasecmp(v, "close") || !strcasecmp(v, "kill")) {
		*a = COMP_KEYBIND_CLOSE;
		return true;
	}
	if (!strcasecmp(v, "exec") || !strcasecmp(v, "spawn")) {
		*a = COMP_KEYBIND_EXEC;
		return true;
	}
	if (!strcasecmp(v, "layout_toggle") || !strcasecmp(v, "toggle_layout")) {
		*a = COMP_KEYBIND_LAYOUT_TOGGLE;
		return true;
	}
	if (!strcasecmp(v, "layout_tile") || !strcasecmp(v, "tile")) {
		*a = COMP_KEYBIND_LAYOUT_TILE;
		return true;
	}
	if (!strcasecmp(v, "layout_stack") || !strcasecmp(v, "stack")) {
		*a = COMP_KEYBIND_LAYOUT_STACK;
		return true;
	}
	if (!strcasecmp(v, "layout_scroll") || !strcasecmp(v, "scroll")) {
		*a = COMP_KEYBIND_LAYOUT_SCROLL;
		return true;
	}
	if (!strcasecmp(v, "tile_swap_prev") || !strcasecmp(v, "tile_prev") || !strcasecmp(v, "tile_left")) {
		*a = COMP_KEYBIND_TILE_SWAP_PREV;
		return true;
	}
	if (!strcasecmp(v, "tile_swap_next") || !strcasecmp(v, "tile_next") || !strcasecmp(v, "tile_right")) {
		*a = COMP_KEYBIND_TILE_SWAP_NEXT;
		return true;
	}
	if (!strcasecmp(v, "scroll_prev") || !strcasecmp(v, "scroll_left")) {
		*a = COMP_KEYBIND_SCROLL_PREV;
		return true;
	}
	if (!strcasecmp(v, "scroll_next") || !strcasecmp(v, "scroll_right")) {
		*a = COMP_KEYBIND_SCROLL_NEXT;
		return true;
	}
	if (!strcasecmp(v, "tile_to_first") || !strcasecmp(v, "tile_first") || !strcasecmp(v, "tile_begin")) {
		*a = COMP_KEYBIND_TILE_TO_FIRST;
		return true;
	}
	if (!strcasecmp(v, "tile_to_last") || !strcasecmp(v, "tile_last") || !strcasecmp(v, "tile_end")) {
		*a = COMP_KEYBIND_TILE_TO_LAST;
		return true;
	}
	if (!strcasecmp(v, "tile_move") || !strcasecmp(v, "tile_shift")) {
		*a = COMP_KEYBIND_TILE_MOVE;
		return true;
	}
	if (!strcasecmp(v, "tile_grid_up") || !strcasecmp(v, "tile_up")) {
		*a = COMP_KEYBIND_TILE_GRID_UP;
		return true;
	}
	if (!strcasecmp(v, "tile_grid_down") || !strcasecmp(v, "tile_down")) {
		*a = COMP_KEYBIND_TILE_GRID_DOWN;
		return true;
	}
	if (!strcasecmp(v, "tile_column_top") || !strcasecmp(v, "tile_col_top") || !strcasecmp(v, "tile_grid_top")) {
		*a = COMP_KEYBIND_TILE_TO_COLUMN_TOP;
		return true;
	}
	if (!strcasecmp(v, "tile_column_bottom") || !strcasecmp(v, "tile_col_bottom") ||
	    !strcasecmp(v, "tile_grid_bottom")) {
		*a = COMP_KEYBIND_TILE_TO_COLUMN_BOTTOM;
		return true;
	}
	if (!strcasecmp(v, "tile_grid_move") || !strcasecmp(v, "tile_row_move")) {
		*a = COMP_KEYBIND_TILE_GRID_MOVE;
		return true;
	}
	return false;
}

static const char *layout_name(enum comp_layout layout)
{
	switch (layout) {
	case COMP_LAYOUT_TILE:
		return "tile";
	case COMP_LAYOUT_SCROLL:
		return "scroll";
	case COMP_LAYOUT_STACK:
	default:
		return "stack";
	}
}

void comp_config_sync_layout_env(enum comp_layout layout)
{
	setenv("STACKCOMP_LAYOUT", layout_name(layout), 1);
}

static void load_defaults(struct comp_config *cfg)
{
	struct comp_keybind b;

	memset(&b, 0, sizeof(b));
	b.mods = WLR_MODIFIER_LOGO;
	b.keysym = XKB_KEY_Return;
	b.action = COMP_KEYBIND_EXEC;
	b.command = xstrdup("${TERMINAL:-foot}");
	if (!b.command) {
		return;
	}
	append_bind(cfg, &b);

	memset(&b, 0, sizeof(b));
	b.mods = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT;
	b.keysym = XKB_KEY_Q;
	b.action = COMP_KEYBIND_CLOSE;
	append_bind(cfg, &b);

	memset(&b, 0, sizeof(b));
	b.mods = WLR_MODIFIER_LOGO;
	b.keysym = XKB_KEY_Escape;
	b.action = COMP_KEYBIND_QUIT;
	append_bind(cfg, &b);

	memset(&b, 0, sizeof(b));
	b.mods = WLR_MODIFIER_LOGO;
	b.keysym = XKB_KEY_t;
	b.action = COMP_KEYBIND_LAYOUT_TOGGLE;
	append_bind(cfg, &b);
}

bool comp_config_default_path(char *out, size_t out_len)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && xdg[0]) {
		if (snprintf(out, out_len, "%s/stackcomp/config", xdg) < (int)out_len &&
		    access(out, R_OK) == 0) {
			return true;
		}
	}
	const char *home = getenv("HOME");
	if (home && home[0]) {
		if (snprintf(out, out_len, "%s/.config/stackcomp/config", home) < (int)out_len &&
		    access(out, R_OK) == 0) {
			return true;
		}
	}
	return false;
}

bool comp_keybind_when_ok(const struct comp_keybind *bind)
{
	if (!bind->when_shell || !bind->when_shell[0]) {
		return true;
	}
	pid_t pid = 0;
	char *argv[] = {"sh", "-c", (char *)bind->when_shell, NULL};
	int err = posix_spawnp(&pid, "sh", NULL, NULL, argv, environ);
	if (err != 0) {
		errno = err;
		wlr_log_errno(WLR_ERROR, "posix_spawnp when= predicate");
		return false;
	}
	int status = 0;
	for (;;) {
		pid_t w = waitpid(pid, &status, 0);
		if (w == pid) {
			break;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w < 0) {
			wlr_log_errno(WLR_ERROR, "waitpid for when=");
			return false;
		}
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void spawn_sh_c(const char *cmd)
{
	if (!cmd || !cmd[0]) {
		return;
	}
	/* Double-fork so we never leave zombies: parent waits the middle child only. */
	pid_t mid = fork();
	if (mid < 0) {
		wlr_log_errno(WLR_ERROR, "fork (async exec)");
		return;
	}
	if (mid == 0) {
		pid_t leaf = fork();
		if (leaf < 0) {
			_exit(127);
		}
		if (leaf == 0) {
			char *argv[] = {"sh", "-c", (char *)cmd, NULL};
			execvp("sh", argv);
			_exit(127);
		}
		_exit(0);
	}
	for (;;) {
		pid_t w = waitpid(mid, NULL, 0);
		if (w == mid) {
			break;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w < 0) {
			wlr_log_errno(WLR_ERROR, "waitpid (async exec)");
			return;
		}
	}
}

static void spawn_sh_c_wait(const char *cmd)
{
	if (!cmd || !cmd[0]) {
		return;
	}
	pid_t pid = 0;
	char *argv[] = {"sh", "-c", (char *)cmd, NULL};
	int err = posix_spawnp(&pid, "sh", NULL, NULL, argv, environ);
	if (err != 0) {
		errno = err;
		wlr_log_errno(WLR_ERROR, "posix_spawnp (shutdown hook)");
		return;
	}
	int status = 0;
	for (;;) {
		pid_t w = waitpid(pid, &status, 0);
		if (w == pid) {
			break;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w < 0) {
			wlr_log_errno(WLR_ERROR, "waitpid for shutdown hook");
			return;
		}
	}
}

void comp_config_run_startup(const struct comp_config *cfg)
{
	if (!cfg) {
		return;
	}
	spawn_sh_c(cfg->hook_startup);
}

void comp_config_run_reload(const struct comp_config *cfg)
{
	if (!cfg) {
		return;
	}
	spawn_sh_c(cfg->hook_reload);
}

void comp_config_run_shutdown(const struct comp_config *cfg)
{
	if (!cfg) {
		return;
	}
	spawn_sh_c_wait(cfg->hook_shutdown);
}

static bool keysym_matches_bind(xkb_keysym_t want, xkb_keysym_t got)
{
	if (want == got) {
		return true;
	}
	if (want == XKB_KEY_NoSymbol || got == XKB_KEY_NoSymbol) {
		return false;
	}
	/* CapsLock / layout quirks can differ in letter case between config and XKB. */
	return xkb_keysym_to_lower(want) == xkb_keysym_to_lower(got);
}

bool comp_config_try_bindings(struct comp_config *cfg, struct comp_server *server,
	bool key_pressed, uint32_t mods_filtered, xkb_keysym_t sym)
{
	if (!key_pressed || !cfg || !cfg->binds || !sym) {
		return false;
	}

	uint32_t mods = mods_filtered & COMP_BIND_MOD_FILTER;
	comp_config_sync_layout_env(server->layout);

	for (size_t i = 0; i < cfg->n_binds; i++) {
		struct comp_keybind *b = &cfg->binds[i];
		if (!keysym_matches_bind(b->keysym, sym) || b->mods != mods) {
			continue;
		}
		if (!comp_keybind_when_ok(b)) {
			continue;
		}
		switch (b->action) {
		case COMP_KEYBIND_NONE:
			break;
		case COMP_KEYBIND_QUIT:
			wl_display_terminate(server->wl_display);
			return true;
		case COMP_KEYBIND_CLOSE:
			if (server->focused_toplevel) {
				wlr_xdg_toplevel_send_close(server->focused_toplevel->xdg_toplevel);
			}
			return true;
		case COMP_KEYBIND_EXEC:
			wlr_log(WLR_INFO, "keybind exec: %s", b->command);
			spawn_sh_c(b->command);
			return true;
		case COMP_KEYBIND_LAYOUT_TOGGLE:
			server_toggle_layout(server);
			return true;
		case COMP_KEYBIND_LAYOUT_TILE:
			server_set_layout(server, COMP_LAYOUT_TILE);
			return true;
		case COMP_KEYBIND_LAYOUT_STACK:
			server_set_layout(server, COMP_LAYOUT_STACK);
			return true;
		case COMP_KEYBIND_LAYOUT_SCROLL:
			server_set_layout(server, COMP_LAYOUT_SCROLL);
			return true;
		case COMP_KEYBIND_TILE_SWAP_PREV:
			server_tile_move_focused_n(server, -1);
			return true;
		case COMP_KEYBIND_TILE_SWAP_NEXT:
			server_tile_move_focused_n(server, 1);
			return true;
		case COMP_KEYBIND_SCROLL_PREV:
			server_scroll_move(server, -1);
			return true;
		case COMP_KEYBIND_SCROLL_NEXT:
			server_scroll_move(server, 1);
			return true;
		case COMP_KEYBIND_TILE_TO_FIRST:
			server_tile_move_focused_edge(server, true);
			return true;
		case COMP_KEYBIND_TILE_TO_LAST:
			server_tile_move_focused_edge(server, false);
			return true;
		case COMP_KEYBIND_TILE_MOVE: {
			int steps = 1;
			if (b->command && b->command[0]) {
				steps = (int)strtol(b->command, NULL, 10);
			}
			server_tile_move_focused_n(server, steps);
			return true;
		}
		case COMP_KEYBIND_TILE_GRID_UP:
			server_tile_move_focused_grid_vert(server, -1);
			return true;
		case COMP_KEYBIND_TILE_GRID_DOWN:
			server_tile_move_focused_grid_vert(server, 1);
			return true;
		case COMP_KEYBIND_TILE_TO_COLUMN_TOP:
			server_tile_move_focused_grid_vert_edge(server, true);
			return true;
		case COMP_KEYBIND_TILE_TO_COLUMN_BOTTOM:
			server_tile_move_focused_grid_vert_edge(server, false);
			return true;
		case COMP_KEYBIND_TILE_GRID_MOVE:
			server_tile_grid_run_command(server, b->command);
			return true;
		}
	}
	return false;
}

static bool tile_grid_dir_word(const char *w)
{
	return !strcasecmp(w, "left") || !strcasecmp(w, "right") || !strcasecmp(w, "up") ||
	       !strcasecmp(w, "down") || !strcasecmp(w, "top") || !strcasecmp(w, "bottom");
}

/** True if tile_grid_move command= is valid (see server_tile_grid_run_command). */
static bool tile_grid_move_command_ok(const char *cmd, size_t line_no)
{
	if (!cmd || !cmd[0]) {
		wlr_log(WLR_ERROR, "Config line ~%zu: tile_grid_move needs command=", line_no);
		return false;
	}
	char tmp[256];
	strncpy(tmp, cmd, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	char *s = tmp;
	while (*s && isspace((unsigned char)*s)) {
		s++;
	}
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) {
		*--e = '\0';
	}
	if (!s[0]) {
		wlr_log(WLR_ERROR, "Config line ~%zu: tile_grid_move command is empty", line_no);
		return false;
	}
	char *endn = NULL;
	(void)strtol(s, &endn, 10);
	if (endn != s && *endn == '\0') {
		return true; /* legacy: signed integer, vertical steps */
	}
	char dir[32];
	int nch = 0;
	if (sscanf(s, "%31s%n", dir, &nch) != 1) {
		goto bad;
	}
	const char *r = s + nch;
	while (*r && isspace((unsigned char)*r)) {
		r++;
	}
	if (!*r) {
		if (!tile_grid_dir_word(dir)) {
			goto bad;
		}
		return true;
	}
	if (!tile_grid_dir_word(dir)) {
		goto bad;
	}
	char *estep = NULL;
	const long steps = strtol(r, &estep, 10);
	if (r == estep || steps <= 0) {
		goto bad;
	}
	while (*estep && isspace((unsigned char)*estep)) {
		estep++;
	}
	if (*estep != '\0') {
		wlr_log(WLR_ERROR, "Config line ~%zu: tile_grid_move trailing junk after '%s'", line_no, cmd);
		return false;
	}
	return true;
bad:
	wlr_log(WLR_ERROR,
		"Config line ~%zu: tile_grid_move expects left/right/up/down/top/bottom, \"DIR N\" (N≥1), or a signed integer",
		line_no);
	return false;
}

static bool flush_bind(struct comp_config *cfg, struct comp_keybind *cur, size_t line_no)
{
	if (!cur->keysym) {
		if (cur->mods || cur->command || cur->when_shell) {
			wlr_log(WLR_ERROR, "Config line ~%zu: incomplete [bind] (missing key=)", line_no);
			return false;
		}
		return true;
	}
	if (cur->action == COMP_KEYBIND_NONE) {
		wlr_log(WLR_ERROR, "Config line ~%zu: bind needs action=", line_no);
		return false;
	}
	if (cur->action == COMP_KEYBIND_EXEC && (!cur->command || !cur->command[0])) {
		wlr_log(WLR_ERROR, "Config line ~%zu: exec bind needs command=", line_no);
		return false;
	}
	if (cur->action == COMP_KEYBIND_TILE_MOVE && cur->command && cur->command[0]) {
		char *end = NULL;
		(void)strtol(cur->command, &end, 10);
		if (!end || end == cur->command || *end) {
			wlr_log(WLR_ERROR, "Config line ~%zu: tile_move command must be empty or a signed integer",
				line_no);
			return false;
		}
	}
	if (cur->action == COMP_KEYBIND_TILE_GRID_MOVE &&
	    !tile_grid_move_command_ok(cur->command ? cur->command : "", line_no)) {
		return false;
	}
	if (!append_bind(cfg, cur)) {
		return false;
	}
	return true;
}

struct tile_rule_parse {
	char *app_id_pat;
	char *title_pat;
	bool float_in_tile;
	int order;
};

static void tile_rule_parse_reset(struct tile_rule_parse *p)
{
	free(p->app_id_pat);
	free(p->title_pat);
	memset(p, 0, sizeof(*p));
}

static bool flush_tile_rule(struct comp_config *cfg, struct tile_rule_parse *p, size_t line_no)
{
	const bool have_app = p->app_id_pat && p->app_id_pat[0];
	const bool have_tit = p->title_pat && p->title_pat[0];
	if (!have_app && !have_tit) {
		tile_rule_parse_reset(p);
		(void)line_no;
		return true;
	}
	struct comp_tile_rule rule;
	memset(&rule, 0, sizeof(rule));
	rule.float_in_tile = p->float_in_tile;
	rule.order = p->order;
	if (have_app) {
		int err = regcomp(&rule.app_id_re, p->app_id_pat, REG_EXTENDED | REG_NOSUB);
		if (err != 0) {
			char errbuf[128];
			regerror(err, NULL, errbuf, sizeof(errbuf));
			wlr_log(WLR_ERROR, "Config line ~%zu: bad app_id regex: %s", line_no, errbuf);
			tile_rule_parse_reset(p);
			return false;
		}
		rule.have_app_id = true;
	}
	if (have_tit) {
		int err = regcomp(&rule.title_re, p->title_pat, REG_EXTENDED | REG_NOSUB);
		if (err != 0) {
			char errbuf[128];
			regerror(err, NULL, errbuf, sizeof(errbuf));
			wlr_log(WLR_ERROR, "Config line ~%zu: bad title regex: %s", line_no, errbuf);
			if (rule.have_app_id) {
				regfree(&rule.app_id_re);
			}
			tile_rule_parse_reset(p);
			return false;
		}
		rule.have_title = true;
	}
	tile_rule_parse_reset(p);

	struct comp_tile_rule *nr = realloc(cfg->tile_rules, (cfg->n_tile_rules + 1) * sizeof(*nr));
	if (!nr) {
		tile_rule_destroy(&rule);
		return false;
	}
	cfg->tile_rules = nr;
	cfg->tile_rules[cfg->n_tile_rules++] = rule;
	return true;
}

static bool parse_tile_mode(const char *v, bool *float_out)
{
	if (!strcasecmp(v, "float") || !strcasecmp(v, "floating")) {
		*float_out = true;
		return true;
	}
	if (!strcasecmp(v, "tile") || !strcasecmp(v, "tiled")) {
		*float_out = false;
		return true;
	}
	return false;
}

void comp_config_tile_props_for_toplevel(const struct comp_config *cfg, const char *app_id,
	const char *title, bool *out_float_in_tile, int *out_order)
{
	*out_float_in_tile = false;
	*out_order = 0;
	if (!cfg || cfg->n_tile_rules == 0) {
		return;
	}
	const char *a = (app_id && app_id[0]) ? app_id : "";
	const char *t = (title && title[0]) ? title : "";
	for (size_t i = 0; i < cfg->n_tile_rules; i++) {
		const struct comp_tile_rule *r = &cfg->tile_rules[i];
		if (r->have_app_id && regexec(&r->app_id_re, a, 0, NULL, 0) != 0) {
			continue;
		}
		if (r->have_title && regexec(&r->title_re, t, 0, NULL, 0) != 0) {
			continue;
		}
		*out_float_in_tile = r->float_in_tile;
		*out_order = r->order;
		return;
	}
}

bool comp_config_load(const char *path, struct comp_config **cfg_out)
{
	struct comp_config *cfg = calloc(1, sizeof(*cfg));
	if (!cfg) {
		return false;
	}

	FILE *f = NULL;
	if (path) {
		f = fopen(path, "r");
	}
	if (!f) {
		if (path) {
			wlr_log(WLR_INFO, "No config at %s (%s), using built-in defaults", path,
				strerror(errno));
		}
		load_defaults(cfg);
		*cfg_out = cfg;
		return true;
	}

	struct comp_keybind cur = {0};
	struct tile_rule_parse cur_tile = {0};
	bool in_bind = false;
	bool in_tile = false;
	bool in_hooks = false;
	char linebuf[4096];
	size_t line_no = 0;
	bool ok = true;

	while (fgets(linebuf, sizeof(linebuf), f)) {
		line_no++;
		char *line = linebuf;
		trim_inplace(line);
		if (!line[0] || line[0] == '#') {
			continue;
		}
		if (line[0] == '[') {
			if (in_bind && !flush_bind(cfg, &cur, line_no)) {
				ok = false;
				break;
			}
			if (in_tile && !flush_tile_rule(cfg, &cur_tile, line_no)) {
				ok = false;
				break;
			}
			keybind_clear(&cur);
			tile_rule_parse_reset(&cur_tile);
			in_bind = false;
			in_tile = false;
			in_hooks = false;
			if (!strcasecmp(line, "[bind]")) {
				in_bind = true;
			} else if (!strcasecmp(line, "[tile_rule]") || !strcasecmp(line, "[tilerule]")) {
				in_tile = true;
			} else if (!strcasecmp(line, "[hooks]")) {
				in_hooks = true;
			} else {
				wlr_log(WLR_ERROR, "%s:%zu: unknown section %s", path, line_no, line);
				ok = false;
			}
			continue;
		}
		if (!in_bind && !in_tile && !in_hooks) {
			wlr_log(WLR_ERROR, "%s:%zu: key=value outside [bind], [tile_rule], or [hooks]", path, line_no);
			ok = false;
			break;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			wlr_log(WLR_ERROR, "%s:%zu: expected key=value", path, line_no);
			ok = false;
			break;
		}
		*eq++ = '\0';
		trim_inplace(line);
		trim_inplace(eq);
		if (in_bind) {
			if (!strcasecmp(line, "mods")) {
				if (!parse_mods_string(eq, &cur.mods)) {
					ok = false;
				}
			} else if (!strcasecmp(line, "key")) {
				cur.keysym = xkb_keysym_from_name(eq, XKB_KEYSYM_CASE_INSENSITIVE);
				if (!cur.keysym) {
					wlr_log(WLR_ERROR, "%s:%zu: unknown keysym '%s'", path, line_no, eq);
					ok = false;
				}
			} else if (!strcasecmp(line, "action")) {
				if (!parse_action(eq, &cur.action)) {
					wlr_log(WLR_ERROR, "%s:%zu: unknown action '%s'", path, line_no, eq);
					ok = false;
				}
			} else if (!strcasecmp(line, "command")) {
				free(cur.command);
				cur.command = xstrdup(eq);
			} else if (!strcasecmp(line, "when")) {
				free(cur.when_shell);
				cur.when_shell = xstrdup(eq);
			} else {
				wlr_log(WLR_ERROR, "%s:%zu: unknown key '%s'", path, line_no, line);
				ok = false;
			}
		} else if (in_tile) {
			if (!strcasecmp(line, "app_id") || !strcasecmp(line, "app-id")) {
				free(cur_tile.app_id_pat);
				cur_tile.app_id_pat = xstrdup(eq);
			} else if (!strcasecmp(line, "title")) {
				free(cur_tile.title_pat);
				cur_tile.title_pat = xstrdup(eq);
			} else if (!strcasecmp(line, "mode")) {
				if (!parse_tile_mode(eq, &cur_tile.float_in_tile)) {
					wlr_log(WLR_ERROR, "%s:%zu: unknown tile_rule mode '%s' (use tile or float)", path,
						line_no, eq);
					ok = false;
				}
			} else if (!strcasecmp(line, "order")) {
				char *end = NULL;
				long v = strtol(eq, &end, 10);
				if (!end || end == eq || *end) {
					wlr_log(WLR_ERROR, "%s:%zu: order= needs an integer", path, line_no);
					ok = false;
				} else {
					cur_tile.order = (int)v;
				}
			} else {
				wlr_log(WLR_ERROR, "%s:%zu: unknown tile_rule key '%s'", path, line_no, line);
				ok = false;
			}
		} else if (in_hooks) {
			if (!strcasecmp(line, "startup") || !strcasecmp(line, "on_startup")) {
				free(cfg->hook_startup);
				cfg->hook_startup = xstrdup(eq);
			} else if (!strcasecmp(line, "shutdown") || !strcasecmp(line, "on_shutdown")) {
				free(cfg->hook_shutdown);
				cfg->hook_shutdown = xstrdup(eq);
			} else if (!strcasecmp(line, "reload") || !strcasecmp(line, "on_reload")) {
				free(cfg->hook_reload);
				cfg->hook_reload = xstrdup(eq);
			} else {
				wlr_log(WLR_ERROR, "%s:%zu: unknown hooks key '%s'", path, line_no, line);
				ok = false;
			}
		}
		if (!ok) {
			break;
		}
	}
	fclose(f);

	if (ok && in_bind) {
		ok = flush_bind(cfg, &cur, line_no);
	}
	if (ok && in_tile) {
		ok = flush_tile_rule(cfg, &cur_tile, line_no);
	}
	keybind_clear(&cur);
	tile_rule_parse_reset(&cur_tile);

	if (!ok) {
		wlr_log(WLR_ERROR, "Failed to parse config %s", path);
		comp_config_free(cfg);
		return false;
	}

	if (cfg->n_binds == 0) {
		wlr_log(WLR_INFO, "Config %s defined no binds, using defaults", path);
		load_defaults(cfg);
	}

	*cfg_out = cfg;
	return true;
}
