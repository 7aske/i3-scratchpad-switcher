#define _GNU_SOURCE
#include <ctype.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
  long long con_id;
  unsigned long window_id;
  char *title;
  char *wm_class;
  char *instance;
  GIcon *icon;
} ScratchpadEntry;

typedef struct {
  GPtrArray *entries;
  GPtrArray *buttons;
  GPtrArray *tile_children;
  GArray *visible_indices;
  GtkWidget *search_entry;
  GtkWidget *flow;
  GtkWidget *window;
  int selected_idx;
  int cols;
} AppState;

static int g_lock_fd = -1;
static char *g_xresources_text = NULL;
static gboolean g_xresources_loaded = FALSE;

enum {
  I3_IPC_MESSAGE_COMMAND = 0,
  I3_IPC_MESSAGE_GET_TREE = 4,
};

#define I3_IPC_MAGIC "i3-ipc"
#define I3_IPC_HEADER_SIZE 14

static gboolean write_full(int fd, const void *buf, size_t len) {
  const char *cursor = (const char *)buf;
  while (len > 0) {
    ssize_t wrote = write(fd, cursor, len);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return FALSE;
    }
    cursor += (size_t)wrote;
    len -= (size_t)wrote;
  }
  return TRUE;
}

static gboolean read_full(int fd, void *buf, size_t len) {
  char *cursor = (char *)buf;
  while (len > 0) {
    ssize_t got = read(fd, cursor, len);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return FALSE;
    }
    if (got == 0) {
      return FALSE;
    }
    cursor += (size_t)got;
    len -= (size_t)got;
  }
  return TRUE;
}

static gboolean acquire_single_instance_lock(void) {
  const char *lock_path = "/tmp/i3-scratchpad-switcher.lock";
  int fd = open(lock_path, O_CREAT | O_RDWR, 0666);
  if (fd < 0) {
    return FALSE;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    return FALSE;
  }
  g_lock_fd = fd;
  return TRUE;
}

static void release_single_instance_lock(void) {
  if (g_lock_fd >= 0) {
    flock(g_lock_fd, LOCK_UN);
    close(g_lock_fd);
    g_lock_fd = -1;
  }
}

static void entry_free(gpointer data) {
  ScratchpadEntry *entry = (ScratchpadEntry *)data;
  if (!entry) {
    return;
  }
  g_clear_pointer(&entry->title, g_free);
  g_clear_pointer(&entry->wm_class, g_free);
  g_clear_pointer(&entry->instance, g_free);
  g_clear_object(&entry->icon);
  g_free(entry);
}

static gboolean str_case_equal(const char *a, const char *b) {
  if (!a || !b) {
    return FALSE;
  }
  return g_ascii_strcasecmp(a, b) == 0;
}

static gboolean app_id_contains(const char *app_id, const char *needle) {
  if (!app_id || !needle || !needle[0]) {
    return FALSE;
  }
  char *a = g_ascii_strdown(app_id, -1);
  char *n = g_ascii_strdown(needle, -1);
  gboolean ok = strstr(a, n) != NULL;
  g_free(a);
  g_free(n);
  return ok;
}

static GIcon *find_icon_for_app(const char *wm_class, const char *instance) {
  GIcon *result = NULL;
  GList *apps = g_app_info_get_all();

  for (GList *it = apps; it != NULL; it = it->next) {
    if (!G_IS_DESKTOP_APP_INFO(it->data)) {
      continue;
    }
    GDesktopAppInfo *app = G_DESKTOP_APP_INFO(it->data);
    const char *startup = g_desktop_app_info_get_startup_wm_class(app);
    if (str_case_equal(startup, wm_class) || str_case_equal(startup, instance)) {
      GIcon *icon = g_app_info_get_icon(G_APP_INFO(app));
      if (icon) {
        result = g_object_ref(icon);
        break;
      }
    }
  }

  if (!result) {
    for (GList *it = apps; it != NULL; it = it->next) {
      if (!G_IS_DESKTOP_APP_INFO(it->data)) {
        continue;
      }
      GDesktopAppInfo *app = G_DESKTOP_APP_INFO(it->data);
      const char *app_id = g_app_info_get_id(G_APP_INFO(app));
      if (app_id_contains(app_id, wm_class) || app_id_contains(app_id, instance)) {
        GIcon *icon = g_app_info_get_icon(G_APP_INFO(app));
        if (icon) {
          result = g_object_ref(icon);
          break;
        }
      }
    }
  }

  g_list_free_full(apps, g_object_unref);
  return result;
}

static char *run_command_first_line(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp) {
    return NULL;
  }

  char *line = NULL;
  size_t cap = 0;
  if (getline(&line, &cap, fp) == -1) {
    free(line);
    pclose(fp);
    return NULL;
  }

  g_strchomp(line);
  char *out = g_strdup(line);
  free(line);
  pclose(fp);
  return out;
}

static void ensure_xresources_loaded(void) {
  if (g_xresources_loaded) {
    return;
  }
  g_xresources_loaded = TRUE;
  XrmInitialize();

  Display *display = XOpenDisplay(NULL);
  if (!display) {
    return;
  }

  const char *resource_text = XResourceManagerString(display);
  if (!resource_text) {
    resource_text = XScreenResourceString(DefaultScreenOfDisplay(display));
  }
  if (resource_text && resource_text[0] != '\0') {
    g_xresources_text = g_strdup(resource_text);
  }

  XCloseDisplay(display);
}

static char *xresources_lookup_raw_value(const char *token) {
  if (!token || token[0] == '\0') {
    return NULL;
  }
  ensure_xresources_loaded();
  if (!g_xresources_text || g_xresources_text[0] == '\0') {
    return NULL;
  }

  gchar *token_lower = g_ascii_strdown(token, -1);
  gchar *star_token = g_strdup_printf("*.%s", token_lower);
  gchar *dot_token = g_strdup_printf(".%s", token_lower);
  gchar **lines = g_strsplit(g_xresources_text, "\n", -1);
  char *matched_value = NULL;

  for (guint i = 0; lines[i] != NULL; i++) {
    char *line = g_strstrip(lines[i]);
    if (line[0] == '\0' || line[0] == '!' || line[0] == '#') {
      continue;
    }

    char *sep = strchr(line, ':');
    if (!sep) {
      continue;
    }
    *sep = '\0';
    char *key = g_strstrip(line);
    char *value = g_strstrip(sep + 1);
    if (value[0] == '\0') {
      continue;
    }

    gchar *key_lower = g_ascii_strdown(key, -1);
    gboolean is_match = FALSE;
    if (strcmp(key_lower, star_token) == 0 || strcmp(key_lower, token_lower) == 0 ||
        g_str_has_suffix(key_lower, dot_token)) {
      is_match = TRUE;
    }

    if (is_match) {
      matched_value = g_strdup(value);
      g_free(key_lower);
      break;
    }
    g_free(key_lower);
  }

  g_strfreev(lines);
  g_free(star_token);
  g_free(dot_token);
  g_free(token_lower);
  return matched_value;
}

static char *get_i3_socket_path(void) {
  const char *env_socket = g_getenv("I3SOCK");
  if (env_socket && env_socket[0] != '\0') {
    return g_strdup(env_socket);
  }
  return run_command_first_line("i3 --get-socketpath 2>/dev/null");
}

static char *i3_ipc_request(guint32 msg_type, const char *payload, GError **error) {
  char *socket_path = get_i3_socket_path();
  if (!socket_path || socket_path[0] == '\0') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Could not resolve i3 socket path.");
    g_free(socket_path);
    return NULL;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to create i3 IPC socket: %s", g_strerror(errno));
    g_free(socket_path);
    return NULL;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  g_strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));
  g_free(socket_path);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to connect to i3 IPC socket: %s", g_strerror(errno));
    close(fd);
    return NULL;
  }

  if (!payload) {
    payload = "";
  }
  guint32 payload_len = (guint32)strlen(payload);
  char header[I3_IPC_HEADER_SIZE] = {0};
  memcpy(header, I3_IPC_MAGIC, 6);
  guint32 payload_len_le = GUINT32_TO_LE(payload_len);
  guint32 msg_type_le = GUINT32_TO_LE(msg_type);
  memcpy(header + 6, &payload_len_le, sizeof(payload_len_le));
  memcpy(header + 10, &msg_type_le, sizeof(msg_type_le));

  if (!write_full(fd, header, sizeof(header)) || !write_full(fd, payload, payload_len)) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to send i3 IPC request: %s", g_strerror(errno));
    close(fd);
    return NULL;
  }

  char resp_header[I3_IPC_HEADER_SIZE] = {0};
  if (!read_full(fd, resp_header, sizeof(resp_header))) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to read i3 IPC response header.");
    close(fd);
    return NULL;
  }
  if (memcmp(resp_header, I3_IPC_MAGIC, 6) != 0) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid i3 IPC response magic.");
    close(fd);
    return NULL;
  }

  guint32 resp_payload_len_le = 0;
  memcpy(&resp_payload_len_le, resp_header + 6, sizeof(resp_payload_len_le));
  guint32 resp_payload_len = GUINT32_FROM_LE(resp_payload_len_le);

  char *resp_payload = g_malloc((size_t)resp_payload_len + 1);
  if (!read_full(fd, resp_payload, resp_payload_len)) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Failed to read i3 IPC response payload.");
    g_free(resp_payload);
    close(fd);
    return NULL;
  }
  resp_payload[resp_payload_len] = '\0';
  close(fd);
  return resp_payload;
}

static gboolean i3_ipc_run_command(const char *command, GError **error) {
  char *payload = i3_ipc_request(I3_IPC_MESSAGE_COMMAND, command, error);
  if (!payload) {
    return FALSE;
  }

  JsonParser *parser = json_parser_new();
  gboolean success = FALSE;
  GError *parse_error = NULL;
  if (json_parser_load_from_data(parser, payload, -1, &parse_error)) {
    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
      JsonArray *result = json_node_get_array(root);
      if (result && json_array_get_length(result) > 0) {
        JsonObject *first = json_array_get_object_element(result, 0);
        if (first && json_object_has_member(first, "success")) {
          success = json_object_get_boolean_member(first, "success");
        }
      }
    }
  } else {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Failed to parse i3 command response: %s", parse_error->message);
    g_clear_error(&parse_error);
  }

  g_object_unref(parser);
  g_free(payload);
  if (!success && error && *error == NULL) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "i3 rejected command: %s", command);
  }
  return success;
}

static char *xresources_color(const char *token, const char *fallback) {
  char *raw = xresources_lookup_raw_value(token);
  if (!raw || raw[0] == '\0') {
    g_free(raw);
    return g_strdup(fallback);
  }

  GdkRGBA rgba;
  if (!gdk_rgba_parse(&rgba, raw)) {
    g_free(raw);
    return g_strdup(fallback);
  }

  return raw;
}

static char *xresources_value(const char *token, const char *fallback) {
  char *raw = xresources_lookup_raw_value(token);
  if (!raw || raw[0] == '\0') {
    g_free(raw);
    return g_strdup(fallback);
  }

  return raw;
}

typedef struct {
  char *family;
  int size_pt;
} UiFont;

static int boosted_font_size_pt(int base_size) {
  int boosted = base_size + 2;
  if (boosted < 10) {
    boosted = 10;
  }
  return boosted;
}

static UiFont ui_font_from_xresources(void) {
  UiFont out = {.family = g_strdup("Sans"), .size_pt = 10};
  char *raw = xresources_value("font", "Sans:size=10");
  if (!raw || raw[0] == '\0') {
    g_free(raw);
    return out;
  }

  g_strstrip(raw);
  while (*raw && (raw[strlen(raw) - 1] == ';' || g_ascii_isspace(raw[strlen(raw) - 1]))) {
    raw[strlen(raw) - 1] = '\0';
  }

  char *work = g_strdup(raw);
  char *first_colon = strchr(work, ':');
  if (first_colon) {
    *first_colon = '\0';
  }
  g_strstrip(work);
  if (work[0] != '\0') {
    g_free(out.family);
    out.family = g_strdup(work);
  }

  char *size_pos = g_strstr_len(raw, -1, "size=");
  if (size_pos) {
    int size = atoi(size_pos + 5);
    if (size > 0) {
      out.size_pt = size;
    }
  }

  g_free(work);
  g_free(raw);
  return out;
}

static double alpha_from_xresources(void) {
  char *raw = xresources_value("alpha", "0.95");
  if (!raw) {
    return 0.95;
  }

  // Some configs use percentages; others use 0..1.
  g_strstrip(raw);
  while (*raw && raw[strlen(raw) - 1] == ';') {
    raw[strlen(raw) - 1] = '\0';
  }

  char *end = NULL;
  double value = g_ascii_strtod(raw, &end);
  g_free(raw);
  if (end == raw) {
    return 0.95;
  }

  if (value > 1.0) {
    value = value / 100.0;
  }
  if (value < 0.10) {
    value = 0.10;
  }
  if (value > 1.0) {
    value = 1.0;
  }
  return value;
}

static JsonObject *find_scratch_container(JsonObject *node) {
  if (!node) {
    return NULL;
  }

  const char *name = NULL;
  if (json_object_has_member(node, "name")) {
    JsonNode *name_node = json_object_get_member(node, "name");
    if (name_node && JSON_NODE_HOLDS_VALUE(name_node)) {
      name = json_node_get_string(name_node);
    }
  }
  if (name && strcmp(name, "__i3_scratch") == 0) {
    return node;
  }

  const char *children_keys[] = {"nodes", "floating_nodes"};
  for (guint k = 0; k < G_N_ELEMENTS(children_keys); k++) {
    const char *key = children_keys[k];
    if (!json_object_has_member(node, key)) {
      continue;
    }
    JsonNode *children_node = json_object_get_member(node, key);
    if (!children_node || !JSON_NODE_HOLDS_ARRAY(children_node)) {
      continue;
    }
    JsonArray *children = json_node_get_array(children_node);
    guint children_count = json_array_get_length(children);
    for (guint i = 0; i < children_count; i++) {
      JsonObject *child = json_array_get_object_element(children, i);
      JsonObject *scratch = find_scratch_container(child);
      if (scratch) {
        return scratch;
      }
    }
  }

  return NULL;
}

static void collect_scratch_windows(JsonObject *node, GPtrArray *entries) {
  if (!node || !entries) {
    return;
  }

  JsonNode *window_node = json_object_get_member(node, "window");
  if (window_node && !JSON_NODE_HOLDS_NULL(window_node) && JSON_NODE_HOLDS_VALUE(window_node)) {
    JsonNode *id_node = json_object_get_member(node, "id");
    long long con_id = id_node && JSON_NODE_HOLDS_VALUE(id_node) ? (long long)json_node_get_int(id_node) : -1;
    unsigned long window_id = (unsigned long)json_node_get_int(window_node);
    if (con_id != -1 && window_id != 0) {
      const char *title = "";
      JsonNode *title_node = json_object_get_member(node, "name");
      if (title_node && JSON_NODE_HOLDS_VALUE(title_node)) {
        const char *maybe_title = json_node_get_string(title_node);
        title = maybe_title ? maybe_title : "";
      }

      const char *wm_class = "";
      const char *instance = "";
      JsonNode *props_node = json_object_get_member(node, "window_properties");
      if (props_node && JSON_NODE_HOLDS_OBJECT(props_node)) {
        JsonObject *props = json_node_get_object(props_node);
        JsonNode *class_node = json_object_get_member(props, "class");
        if (class_node && JSON_NODE_HOLDS_VALUE(class_node)) {
          const char *raw = json_node_get_string(class_node);
          wm_class = raw ? raw : "";
        }
        JsonNode *instance_node = json_object_get_member(props, "instance");
        if (instance_node && JSON_NODE_HOLDS_VALUE(instance_node)) {
          const char *raw = json_node_get_string(instance_node);
          instance = raw ? raw : "";
        }
      }

      ScratchpadEntry *entry = g_new0(ScratchpadEntry, 1);
      entry->con_id = con_id;
      entry->window_id = window_id;
      entry->title = g_strdup(title);
      entry->wm_class = g_strdup(wm_class);
      entry->instance = g_strdup(instance);
      entry->icon = find_icon_for_app(entry->wm_class, entry->instance);
      g_ptr_array_add(entries, entry);
    }
  }

  const char *children_keys[] = {"nodes", "floating_nodes"};
  for (guint k = 0; k < G_N_ELEMENTS(children_keys); k++) {
    const char *key = children_keys[k];
    JsonNode *children_node = json_object_get_member(node, key);
    if (!children_node || !JSON_NODE_HOLDS_ARRAY(children_node)) {
      continue;
    }
    JsonArray *children = json_node_get_array(children_node);
    guint children_count = json_array_get_length(children);
    for (guint i = 0; i < children_count; i++) {
      JsonObject *child = json_array_get_object_element(children, i);
      collect_scratch_windows(child, entries);
    }
  }
}

static GPtrArray *load_scratchpad_entries(void) {
  GError *error = NULL;
  char *tree_payload = i3_ipc_request(I3_IPC_MESSAGE_GET_TREE, "", &error);
  if (!tree_payload) {
    g_printerr("Failed to request i3 tree: %s\n", error ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, tree_payload, -1, &error)) {
    g_printerr("Failed to parse i3 tree JSON: %s\n", error ? error->message : "unknown parse error");
    g_clear_error(&error);
    g_object_unref(parser);
    g_free(tree_payload);
    return NULL;
  }
  g_free(tree_payload);

  GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
  JsonNode *root = json_parser_get_root(parser);
  if (root && JSON_NODE_HOLDS_OBJECT(root)) {
    JsonObject *root_obj = json_node_get_object(root);
    JsonObject *scratch = find_scratch_container(root_obj);
    if (scratch) {
      collect_scratch_windows(scratch, entries);
    }
  }

  g_object_unref(parser);
  return entries;
}

static int compute_max_cols_for_screen(int tile_px, int gap_px, int chrome_px) {
  int screen_width = 1920;
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) {
      monitor = gdk_display_get_monitor(display, 0);
    }
    if (monitor) {
      GdkRectangle geo = {0};
      gdk_monitor_get_geometry(monitor, &geo);
      if (geo.width > 0) {
        screen_width = geo.width;
      }
    }
  }
  int max_window_width = screen_width - 100;
  if (max_window_width < 500) {
    max_window_width = 500;
  }
  int cols_fit = (max_window_width - chrome_px + gap_px) / (tile_px + gap_px);
  if (cols_fit < 1) {
    cols_fit = 1;
  }
  return cols_fit;
}

static void apply_window_width_for_count(AppState *state, int count) {
  if (!state || !state->window) {
    return;
  }
  int tile_px = 160;
  int gap_px = 10;
  int chrome_px = 64;
  int cols_requested = count > 0 ? count : 1;
  int cols_fit = compute_max_cols_for_screen(tile_px, gap_px, chrome_px);
  int cols = cols_requested < cols_fit ? cols_requested : cols_fit;
  int screen_width = 1920;
  GdkDisplay *display = gdk_display_get_default();
  if (display) {
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) {
      monitor = gdk_display_get_monitor(display, 0);
    }
    if (monitor) {
      GdkRectangle geo = {0};
      gdk_monitor_get_geometry(monitor, &geo);
      if (geo.width > 0) {
        screen_width = geo.width;
      }
    }
  }
  int max_window_width = screen_width - 100;
  if (max_window_width < 500) {
    max_window_width = 500;
  }
  int window_width = cols * tile_px + (cols - 1) * gap_px + chrome_px;
  if (window_width < 760) {
    window_width = 760;
  }
  if (window_width > max_window_width) {
    window_width = max_window_width;
  }

  int rows = (count + cols - 1) / cols;
  if (rows < 1) {
    rows = 1;
  }
  if (rows > 3) {
    rows = 3;
  }
  int vertical_chrome_px = 180; // title + search + hint + margins
  int window_height = vertical_chrome_px + rows * tile_px + (rows - 1) * gap_px;
  if (window_height < 240) {
    window_height = 240;
  }
  if (window_height > 620) {
    window_height = 620;
  }

  state->cols = cols;
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(state->flow), cols);
  gtk_window_resize(GTK_WINDOW(state->window), window_width, window_height);
}

static void focus_entry(const ScratchpadEntry *entry) {
  if (!entry) {
    return;
  }

  gchar *cmd = g_strdup_printf("[con_id=%lld] scratchpad show, focus", entry->con_id);
  GError *error = NULL;
  if (!i3_ipc_run_command(cmd, &error)) {
    g_printerr("Failed to focus entry: %s\n", error->message);
    g_clear_error(&error);
  }
  g_free(cmd);
}

static gboolean index_is_visible(const AppState *state, int idx) {
  if (!state || !state->visible_indices) {
    return FALSE;
  }
  for (guint i = 0; i < state->visible_indices->len; i++) {
    int v = g_array_index(state->visible_indices, int, i);
    if (v == idx) {
      return TRUE;
    }
  }
  return FALSE;
}

static int visible_position_for_index(const AppState *state, int idx) {
  if (!state || !state->visible_indices) {
    return -1;
  }
  for (guint i = 0; i < state->visible_indices->len; i++) {
    int v = g_array_index(state->visible_indices, int, i);
    if (v == idx) {
      return (int)i;
    }
  }
  return -1;
}

static void update_selected_tile(AppState *state, int new_idx) {
  if (!state || !state->buttons || state->buttons->len == 0 || !state->visible_indices ||
      state->visible_indices->len == 0) {
    return;
  }

  int visible_count = (int)state->visible_indices->len;
  int normalized_idx = new_idx;
  if (!index_is_visible(state, normalized_idx)) {
    int pos = new_idx;
    while (pos < 0) {
      pos += visible_count;
    }
    pos %= visible_count;
    normalized_idx = g_array_index(state->visible_indices, int, (guint)pos);
  }

  int count = (int)state->buttons->len;
  if (state->selected_idx >= 0 && state->selected_idx < count) {
    GtkWidget *old_btn = g_ptr_array_index(state->buttons, (guint)state->selected_idx);
    GtkStyleContext *old_ctx = gtk_widget_get_style_context(old_btn);
    gtk_style_context_remove_class(old_ctx, "entry-tile-selected");
  }

  GtkWidget *new_btn = g_ptr_array_index(state->buttons, (guint)normalized_idx);
  GtkStyleContext *new_ctx = gtk_widget_get_style_context(new_btn);
  gtk_style_context_add_class(new_ctx, "entry-tile-selected");
  gtk_widget_grab_focus(new_btn);
  state->selected_idx = normalized_idx;
}

static gboolean entry_matches_filter(const ScratchpadEntry *entry, const char *query) {
  if (!query || query[0] == '\0') {
    return TRUE;
  }

  const char *title = (entry && entry->title) ? entry->title : "";
  const char *klass = (entry && entry->wm_class) ? entry->wm_class : "";
  const char *instance = (entry && entry->instance) ? entry->instance : "";

  gchar *needle = g_utf8_casefold(query, -1);
  gchar *title_fold = g_utf8_casefold(title, -1);
  gchar *class_fold = g_utf8_casefold(klass, -1);
  gchar *inst_fold = g_utf8_casefold(instance, -1);
  gboolean match = (strstr(title_fold, needle) != NULL) || (strstr(class_fold, needle) != NULL) ||
                   (strstr(inst_fold, needle) != NULL);
  g_free(needle);
  g_free(title_fold);
  g_free(class_fold);
  g_free(inst_fold);
  return match;
}

static void apply_filter(AppState *state) {
  if (!state || !state->entries || !state->tile_children || !state->visible_indices || !state->search_entry) {
    return;
  }

  g_array_set_size(state->visible_indices, 0);
  const char *query = gtk_entry_get_text(GTK_ENTRY(state->search_entry));

  for (guint i = 0; i < state->entries->len; i++) {
    ScratchpadEntry *entry = g_ptr_array_index(state->entries, i);
    gboolean visible = entry_matches_filter(entry, query);
    GtkWidget *tile_child = g_ptr_array_index(state->tile_children, i);
    gtk_widget_set_visible(tile_child, visible);
    if (visible) {
      int idx = (int)i;
      g_array_append_val(state->visible_indices, idx);
    }
  }

  if (state->visible_indices->len == 0) {
    if (state->selected_idx >= 0 && state->selected_idx < (int)state->buttons->len) {
      GtkWidget *old_btn = g_ptr_array_index(state->buttons, (guint)state->selected_idx);
      gtk_style_context_remove_class(gtk_widget_get_style_context(old_btn), "entry-tile-selected");
    }
    state->selected_idx = -1;
    return;
  }

  if (!index_is_visible(state, state->selected_idx)) {
    int first = g_array_index(state->visible_indices, int, 0);
    update_selected_tile(state, first);
  } else {
    update_selected_tile(state, state->selected_idx);
  }
}

static void on_search_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  AppState *state = (AppState *)user_data;
  apply_filter(state);
}

static gboolean append_typed_char_to_search(AppState *state, guint keyval) {
  if (!state || !state->search_entry) {
    return FALSE;
  }
  gunichar uc = gdk_keyval_to_unicode(keyval);
  if (!g_unichar_isprint(uc) || uc == '\r' || uc == '\n' || uc == '\t') {
    return FALSE;
  }

  gchar buf[8] = {0};
  gint len = g_unichar_to_utf8(uc, buf);
  buf[len] = '\0';
  const char *current = gtk_entry_get_text(GTK_ENTRY(state->search_entry));
  gchar *next = g_strconcat(current, buf, NULL);
  gtk_entry_set_text(GTK_ENTRY(state->search_entry), next);
  gtk_editable_set_position(GTK_EDITABLE(state->search_entry), -1);
  g_free(next);
  return TRUE;
}

static gboolean backspace_search(AppState *state) {
  if (!state || !state->search_entry) {
    return FALSE;
  }
  const char *current = gtk_entry_get_text(GTK_ENTRY(state->search_entry));
  if (!current || current[0] == '\0') {
    return FALSE;
  }

  gchar *copy = g_strdup(current);
  gchar *end = g_utf8_find_prev_char(copy, copy + strlen(copy));
  if (end) {
    *end = '\0';
  } else {
    copy[0] = '\0';
  }
  gtk_entry_set_text(GTK_ENTRY(state->search_entry), copy);
  gtk_editable_set_position(GTK_EDITABLE(state->search_entry), -1);
  g_free(copy);
  return TRUE;
}

static gboolean delete_prev_word_search(AppState *state) {
  if (!state || !state->search_entry) {
    return FALSE;
  }
  const char *current = gtk_entry_get_text(GTK_ENTRY(state->search_entry));
  if (!current || current[0] == '\0') {
    return FALSE;
  }

  gchar *copy = g_strdup(current);
  gchar *start = copy;
  gchar *cur = copy + strlen(copy);
  gchar *prev = g_utf8_find_prev_char(start, cur);
  if (!prev) {
    copy[0] = '\0';
    gtk_entry_set_text(GTK_ENTRY(state->search_entry), copy);
    gtk_editable_set_position(GTK_EDITABLE(state->search_entry), -1);
    g_free(copy);
    return TRUE;
  }

  // Trim trailing spaces first.
  while (prev) {
    gunichar uc = g_utf8_get_char(prev);
    if (!g_unichar_isspace(uc)) {
      break;
    }
    cur = prev;
    prev = g_utf8_find_prev_char(start, cur);
  }

  // Remove previous word characters.
  while (prev) {
    gunichar uc = g_utf8_get_char(prev);
    if (g_unichar_isspace(uc)) {
      break;
    }
    cur = prev;
    prev = g_utf8_find_prev_char(start, cur);
  }

  *cur = '\0';
  gtk_entry_set_text(GTK_ENTRY(state->search_entry), copy);
  gtk_editable_set_position(GTK_EDITABLE(state->search_entry), -1);
  g_free(copy);
  return TRUE;
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
  (void)widget;
  AppState *state = (AppState *)user_data;
  if (event->keyval == GDK_KEY_Escape) {
    gtk_main_quit();
    return TRUE;
  }

  if (!state || !state->entries || state->entries->len == 0) {
    return FALSE;
  }

  int visible_count = state->visible_indices ? (int)state->visible_indices->len : 0;
  gboolean ctrl_pressed = (event->state & GDK_CONTROL_MASK) != 0;
  if (ctrl_pressed && (event->keyval == GDK_KEY_w || event->keyval == GDK_KEY_W)) {
    return delete_prev_word_search(state);
  }

  if (visible_count == 0) {
    if (event->keyval == GDK_KEY_BackSpace) {
      return backspace_search(state);
    }
    if (!(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK))) {
      return append_typed_char_to_search(state, event->keyval);
    }
    return FALSE;
  }

  int current_pos = visible_position_for_index(state, state->selected_idx);
  if (current_pos < 0) {
    current_pos = 0;
  }
  int next_pos = current_pos;

  if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
    if (state->selected_idx >= 0 && state->selected_idx < (int)state->entries->len) {
      ScratchpadEntry *entry = g_ptr_array_index(state->entries, (guint)state->selected_idx);
      focus_entry(entry);
      gtk_main_quit();
      return TRUE;
    }
    return FALSE;
  }

  switch (event->keyval) {
    case GDK_KEY_Tab:
      if ((event->state & GDK_SHIFT_MASK) != 0) {
        next_pos = current_pos - 1;
      } else {
        next_pos = current_pos + 1;
      }
      break;
    case GDK_KEY_Right:
    case GDK_KEY_l:
      next_pos = current_pos + 1;
      break;
    case GDK_KEY_Left:
    case GDK_KEY_h:
      next_pos = current_pos - 1;
      break;
    case GDK_KEY_Down:
    case GDK_KEY_j:
      next_pos = current_pos + (state->cols > 0 ? state->cols : 1);
      break;
    case GDK_KEY_Up:
    case GDK_KEY_k:
      next_pos = current_pos - (state->cols > 0 ? state->cols : 1);
      break;
    case GDK_KEY_BackSpace:
      return backspace_search(state);
    default:
      if (!(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_META_MASK))) {
        return append_typed_char_to_search(state, event->keyval);
      }
      return FALSE;
  }

  if (next_pos < 0) {
    while (next_pos < 0) {
      next_pos += visible_count;
    }
  }
  if (next_pos >= visible_count) {
    next_pos %= visible_count;
  }
  int next_idx = g_array_index(state->visible_indices, int, (guint)next_pos);
  update_selected_tile(state, next_idx);
  return TRUE;
}

static gboolean on_window_focus_out(GtkWidget *widget, GdkEventFocus *event, gpointer user_data) {
  (void)widget;
  (void)event;
  (void)user_data;
  gtk_main_quit();
  return FALSE;
}

static void on_entry_clicked(GtkWidget *button, gpointer user_data) {
  AppState *state = (AppState *)user_data;
  gpointer idx_ptr = g_object_get_data(G_OBJECT(button), "entry-index");
  int idx = (int)(intptr_t)idx_ptr;
  if (state && idx >= 0 && state->entries && idx < (int)state->entries->len) {
    update_selected_tile(state, idx);
    ScratchpadEntry *entry = g_ptr_array_index(state->entries, (guint)idx);
    focus_entry(entry);
  }
  gtk_main_quit();
}

static gboolean on_entry_button_focus(GtkWidget *button, GdkEventFocus *event, gpointer user_data) {
  (void)event;
  AppState *state = (AppState *)user_data;
  gpointer idx_ptr = g_object_get_data(G_OBJECT(button), "entry-index");
  int idx = (int)(intptr_t)idx_ptr;
  if (state && idx >= 0) {
    update_selected_tile(state, idx);
  }
  return FALSE;
}

static GtkWidget *build_entry_tile(AppState *state, ScratchpadEntry *entry, int index) {
  GtkWidget *btn = gtk_button_new();
  gtk_widget_set_size_request(btn, 160, 160);
  gtk_widget_set_hexpand(btn, FALSE);
  gtk_widget_set_vexpand(btn, FALSE);
  gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(btn), "entry-tile");

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 10);
  gtk_widget_set_margin_bottom(box, 10);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_container_add(GTK_CONTAINER(btn), box);

  GtkWidget *icon = NULL;
  if (entry->icon) {
    icon = gtk_image_new_from_gicon(entry->icon, GTK_ICON_SIZE_DIALOG);
  } else {
    icon = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
  }
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 56);

  GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(icon_box, 72, 72);
  gtk_widget_set_halign(icon_box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(icon_box, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(icon_box), "icon-square");
  gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(icon_box), icon, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), icon_box, FALSE, FALSE, 0);

  const char *label_text = entry->title && entry->title[0] ? entry->title : entry->wm_class;
  if (!label_text || !label_text[0]) {
    label_text = "Unknown";
  }
  GtkWidget *label = gtk_label_new(label_text);
  gtk_style_context_add_class(gtk_widget_get_style_context(label), "entry-label");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 18);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
  gtk_label_set_single_line_mode(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  g_object_set_data(G_OBJECT(btn), "entry-index", (gpointer)(intptr_t)index);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_entry_clicked), state);
  g_signal_connect(btn, "focus-in-event", G_CALLBACK(on_entry_button_focus), state);
  return btn;
}

static void clear_tiles(AppState *state) {
  if (!state || !state->tile_children || !state->buttons) {
    return;
  }
  for (guint i = 0; i < state->tile_children->len; i++) {
    GtkWidget *tile_child = g_ptr_array_index(state->tile_children, i);
    if (tile_child) {
      gtk_widget_destroy(tile_child);
    }
  }
  g_ptr_array_set_size(state->tile_children, 0);
  g_ptr_array_set_size(state->buttons, 0);
}

static void refresh_entries(AppState *state) {
  if (!state || !state->flow) {
    return;
  }

  long long selected_con_id = -1;
  if (state->entries && state->selected_idx >= 0 && state->selected_idx < (int)state->entries->len) {
    ScratchpadEntry *selected = g_ptr_array_index(state->entries, (guint)state->selected_idx);
    selected_con_id = selected ? selected->con_id : -1;
  }

  clear_tiles(state);
  if (state->entries) {
    g_ptr_array_free(state->entries, TRUE);
    state->entries = NULL;
  }

  state->entries = load_scratchpad_entries();
  if (!state->entries) {
    state->entries = g_ptr_array_new_with_free_func(entry_free);
  }

  apply_window_width_for_count(state, (int)state->entries->len);

  for (guint i = 0; i < state->entries->len; i++) {
    ScratchpadEntry *entry = g_ptr_array_index(state->entries, i);
    GtkWidget *tile = build_entry_tile(state, entry, (int)i);
    g_ptr_array_add(state->buttons, tile);
    gtk_flow_box_insert(GTK_FLOW_BOX(state->flow), tile, -1);
    GtkWidget *tile_child = gtk_widget_get_parent(tile);
    g_ptr_array_add(state->tile_children, tile_child);
  }

  gtk_widget_show_all(state->flow);
  apply_filter(state);
  if (state->visible_indices->len == 0) {
    state->selected_idx = -1;
    return;
  }

  int target_idx = -1;
  if (selected_con_id != -1) {
    for (guint i = 0; i < state->entries->len; i++) {
      ScratchpadEntry *entry = g_ptr_array_index(state->entries, i);
      if (entry && entry->con_id == selected_con_id && index_is_visible(state, (int)i)) {
        target_idx = (int)i;
        break;
      }
    }
  }
  if (target_idx == -1) {
    target_idx = g_array_index(state->visible_indices, int, 0);
  }
  update_selected_tile(state, target_idx);
}

static void apply_css(void) {
  char *bg = xresources_color("background", "#121218");
  char *fg = xresources_color("foreground", "#e6e6e6");
  char *accent = xresources_color("color4", "#5e81ac");
  char *tile = xresources_color("color8", "#4c566a");
  UiFont ui_font = ui_font_from_xresources();
  int font_size_pt = ui_font.size_pt;
  double alpha = alpha_from_xresources();

  GdkRGBA bg_rgba = {0};
  if (!gdk_rgba_parse(&bg_rgba, bg)) {
    gdk_rgba_parse(&bg_rgba, "#121218");
  }
  bg_rgba.alpha = alpha;
  char *bg_with_alpha = gdk_rgba_to_string(&bg_rgba);

  gchar *css = g_strdup_printf(
      "window { background: %s; color: %s; }"
      "* { font-family: \"%s\"; font-size: %dpt; }"
      "label { color: %s; }"
      ".entry-tile {"
      "  min-width: 160px;"
      "  min-height: 160px;"
      "  max-width: 160px;"
      "  max-height: 160px;"
      "  border-radius: 12px;"
      "  border: 2px solid %s;"
      "  background: shade(%s, 0.80);"
      "  color: %s;"
      "}"
      ".entry-tile:hover {"
      "  background: shade(%s, 1.05);"
      "  border-color: %s;"
      "  color: %s;"
      "}"
      ".entry-tile-selected {"
      "  border-color: %s;"
      "  background: shade(%s, 1.12);"
      "}"
      ".entry-tile-selected label {"
      "  color: %s;"
      "}"
      ".entry-tile label, .entry-tile:hover label {"
      "  color: %s;"
      "}"
      ".hint-label {"
      "  color: %s;"
      "  opacity: 0.85;"
      "}"
      "entry {"
      "  color: %s;"
      "}"
      ".icon-square {"
      "  border-radius: 10px;"
      "  border: 1px solid %s;"
      "  background: shade(%s, 0.65);"
      "}",
      bg_with_alpha, fg, ui_font.family, font_size_pt, fg, accent, tile, fg, tile, accent, fg, accent, tile, fg, fg, tile, fg, accent, bg);

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  g_free(css);
  g_free(bg);
  g_free(fg);
  g_free(accent);
  g_free(tile);
  g_free(ui_font.family);
  g_free(bg_with_alpha);
}

int main(int argc, char **argv) {
  (void)argv;
  if (!acquire_single_instance_lock()) {
    g_printerr("i3-scratchpad-switcher is already running.\n");
    return 1;
  }

  gtk_init(&argc, &argv);
  UiFont ui_font = ui_font_from_xresources();
  int font_size_pt = boosted_font_size_pt(ui_font.size_pt);
  gchar *gtk_font_name = g_strdup_printf("%s %d", ui_font.family, font_size_pt);
  GtkSettings *settings = gtk_settings_get_default();
  if (settings) {
    g_object_set(settings, "gtk-font-name", gtk_font_name, NULL);
  }
  g_free(gtk_font_name);
  g_free(ui_font.family);
  apply_css();

  GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(win), "i3 Scratchpad Switcher");
  gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER_ALWAYS);
  gtk_window_set_default_size(GTK_WINDOW(win), 760, 240);
  gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
  gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
  gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
  gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
  gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(outer, 16);
  gtk_widget_set_margin_bottom(outer, 16);
  gtk_widget_set_margin_start(outer, 16);
  gtk_widget_set_margin_end(outer, 16);
  gtk_container_add(GTK_CONTAINER(win), outer);

  GtkWidget *title = gtk_label_new("Scratchpad");
  gtk_box_pack_start(GTK_BOX(outer), title, FALSE, FALSE, 0);

  GtkWidget *search = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Type to filter windows...");
  gtk_box_pack_start(GTK_BOX(outer), search, FALSE, FALSE, 0);

  GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
  const int tile_height_px = 160;
  const int tile_row_gap_px = 10;
  const int rows_before_scroll = 3;
  const int max_scroller_height =
      rows_before_scroll * tile_height_px + (rows_before_scroll - 1) * tile_row_gap_px;
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 120);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), max_scroller_height);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), FALSE);
  gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);

  GtkWidget *hint = gtk_label_new("Type to filter  •  Tab/Shift+Tab or Arrow keys (h/j/k/l) to navigate  •  Enter to open  •  Esc to close");
  gtk_style_context_add_class(gtk_widget_get_style_context(hint), "hint-label");
  gtk_label_set_xalign(GTK_LABEL(hint), 0.5f);
  gtk_box_pack_end(GTK_BOX(outer), hint, FALSE, FALSE, 0);

  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 1);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 10);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 10);
  gtk_container_add(GTK_CONTAINER(scroller), flow);

  AppState state = {
      .entries = NULL,
      .buttons = g_ptr_array_new(),
      .tile_children = g_ptr_array_new(),
      .visible_indices = g_array_new(FALSE, FALSE, sizeof(int)),
      .search_entry = search,
      .flow = flow,
      .window = win,
      .selected_idx = -1,
      .cols = 1,
  };
  g_signal_connect(win, "key-press-event", G_CALLBACK(on_key_press), &state);
  g_signal_connect(win, "focus-out-event", G_CALLBACK(on_window_focus_out), NULL);
  g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_signal_connect(search, "changed", G_CALLBACK(on_search_changed), &state);

  refresh_entries(&state);
  gtk_widget_show_all(win);
  gtk_main();

  if (state.entries) {
    g_ptr_array_free(state.entries, TRUE);
  }
  g_ptr_array_free(state.buttons, TRUE);
  g_ptr_array_free(state.tile_children, TRUE);
  g_array_free(state.visible_indices, TRUE);
  g_clear_pointer(&g_xresources_text, g_free);
  release_single_instance_lock();
  return 0;
}
