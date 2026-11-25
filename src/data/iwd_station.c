#include "data/iwd_station.h"

#include <gio/gio.h>

#define IWD_SERVICE "net.connman.iwd"
#define IWD_STATION_INTERFACE "net.connman.iwd.Station"

typedef struct {
  char *state;
  gint64 since_us;
} StationState;

struct _NetworkSidebarIwdStation {
  GDBusObjectManager *manager;
  GCancellable *cancellable;
  GHashTable *stations; /* object path -> StationState */
  guint stuck_source;
  NetworkSidebarIwdStationChanged changed;
  gpointer user_data;
};

static void station_state_free(StationState *station) {
  g_free(station->state);
  g_free(station);
}

static gboolean can_scan(const char *state) {
  return g_strcmp0(state, "connected") == 0 ||
         g_strcmp0(state, "disconnected") == 0;
}

static void schedule_stuck_check(NetworkSidebarIwdStation *self);

static gboolean stuck_check_cb(gpointer user_data) {
  NetworkSidebarIwdStation *self = user_data;

  self->stuck_source = 0;
  if (self->changed != NULL)
    self->changed(self->user_data);
  schedule_stuck_check(self);
  return G_SOURCE_REMOVE;
}

/* Wake up when the oldest pending disconnect turns stuck, so the panel can
 * offer the fix without waiting for another iwd signal. */
static void schedule_stuck_check(NetworkSidebarIwdStation *self) {
  GHashTableIter iter;
  gpointer value;
  gint64 now = g_get_monotonic_time();
  gint64 next_us = G_MAXINT64;

  g_clear_handle_id(&self->stuck_source, g_source_remove);
  g_hash_table_iter_init(&iter, self->stations);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StationState *station = value;
    gint64 stuck_at =
        station->since_us + NETWORK_SIDEBAR_IWD_STUCK_SECONDS * G_USEC_PER_SEC;

    if (g_strcmp0(station->state, "disconnecting") == 0 && stuck_at > now)
      next_us = MIN(next_us, stuck_at);
  }
  if (next_us != G_MAXINT64)
    self->stuck_source = g_timeout_add(
        (guint)((next_us - now) / 1000) + 50, stuck_check_cb, self);
}

static void sync_stations(NetworkSidebarIwdStation *self) {
  g_autoptr(GHashTable) seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GList *objects = NULL;
  GHashTableIter iter;
  gpointer key;
  gboolean changed = FALSE;
  gint64 now = g_get_monotonic_time();

  if (self->manager != NULL)
    objects = g_dbus_object_manager_get_objects(self->manager);
  for (GList *l = objects; l != NULL; l = l->next) {
    const char *path = g_dbus_object_get_object_path(l->data);
    g_autoptr(GDBusInterface) iface =
        g_dbus_object_get_interface(l->data, IWD_STATION_INTERFACE);
    g_autoptr(GVariant) value = NULL;
    const char *state;
    StationState *station;

    if (iface == NULL)
      continue;
    value = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(iface), "State");
    state = value != NULL &&
                    g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)
                ? g_variant_get_string(value, NULL)
                : "unknown";
    g_hash_table_add(seen, g_strdup(path));
    station = g_hash_table_lookup(self->stations, path);
    if (station == NULL) {
      station = g_new0(StationState, 1);
      g_hash_table_insert(self->stations, g_strdup(path), station);
    } else if (g_strcmp0(station->state, state) == 0) {
      continue;
    }
    g_free(station->state);
    station->state = g_strdup(state);
    station->since_us = now;
    changed = TRUE;
  }
  g_list_free_full(objects, g_object_unref);

  g_hash_table_iter_init(&iter, self->stations);
  while (g_hash_table_iter_next(&iter, &key, NULL)) {
    if (!g_hash_table_contains(seen, key)) {
      g_hash_table_iter_remove(&iter);
      changed = TRUE;
    }
  }

  schedule_stuck_check(self);
  if (changed && self->changed != NULL)
    self->changed(self->user_data);
}

static void on_manager_changed(NetworkSidebarIwdStation *self) {
  sync_stations(self);
}

static void manager_ready_cb(GObject *source, GAsyncResult *result,
                             gpointer user_data) {
  NetworkSidebarIwdStation *self = user_data;
  g_autoptr(GError) error = NULL;
  GDBusObjectManager *manager =
      g_dbus_object_manager_client_new_for_bus_finish(result, &error);

  (void)source;
  if (manager == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_debug("iwd station watch unavailable: %s", error->message);
    return;
  }
  self->manager = manager;
  g_signal_connect_swapped(manager, "object-added",
                           G_CALLBACK(on_manager_changed), self);
  g_signal_connect_swapped(manager, "object-removed",
                           G_CALLBACK(on_manager_changed), self);
  g_signal_connect_swapped(manager, "interface-added",
                           G_CALLBACK(on_manager_changed), self);
  g_signal_connect_swapped(manager, "interface-removed",
                           G_CALLBACK(on_manager_changed), self);
  g_signal_connect_swapped(manager, "interface-proxy-properties-changed",
                           G_CALLBACK(on_manager_changed), self);
  g_signal_connect_swapped(manager, "notify::name-owner",
                           G_CALLBACK(on_manager_changed), self);
  sync_stations(self);
}

NetworkSidebarIwdStation *
network_sidebar_iwd_station_new(NetworkSidebarIwdStationChanged changed,
                                gpointer user_data) {
  NetworkSidebarIwdStation *self = g_new0(NetworkSidebarIwdStation, 1);

  self->changed = changed;
  self->user_data = user_data;
  self->cancellable = g_cancellable_new();
  self->stations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)station_state_free);
  g_dbus_object_manager_client_new_for_bus(
      G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
      IWD_SERVICE, "/", NULL, NULL, NULL, self->cancellable, manager_ready_cb,
      self);
  return self;
}

void network_sidebar_iwd_station_free(NetworkSidebarIwdStation *self) {
  if (self == NULL)
    return;
  g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  g_clear_handle_id(&self->stuck_source, g_source_remove);
  if (self->manager != NULL)
    g_signal_handlers_disconnect_by_data(self->manager, self);
  g_clear_object(&self->manager);
  g_clear_pointer(&self->stations, g_hash_table_unref);
  g_free(self);
}

const char *
network_sidebar_iwd_station_get_busy_state(NetworkSidebarIwdStation *self) {
  GHashTableIter iter;
  gpointer value;
  const char *busy = NULL;

  if (self == NULL)
    return NULL;
  g_hash_table_iter_init(&iter, self->stations);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StationState *station = value;

    if (can_scan(station->state) ||
        g_strcmp0(station->state, "unknown") == 0)
      continue;
    if (busy == NULL || g_strcmp0(station->state, "disconnecting") == 0)
      busy = station->state;
  }
  return busy;
}

gboolean network_sidebar_iwd_station_is_stuck(NetworkSidebarIwdStation *self,
                                              gint64 *out_seconds) {
  GHashTableIter iter;
  gpointer value;
  gint64 now = g_get_monotonic_time();
  gint64 longest_us = -1;

  if (self == NULL)
    return FALSE;
  g_hash_table_iter_init(&iter, self->stations);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    StationState *station = value;

    if (g_strcmp0(station->state, "disconnecting") == 0)
      longest_us = MAX(longest_us, now - station->since_us);
  }
  if (longest_us < NETWORK_SIDEBAR_IWD_STUCK_SECONDS * G_USEC_PER_SEC)
    return FALSE;
  if (out_seconds != NULL)
    *out_seconds = longest_us / G_USEC_PER_SEC;
  return TRUE;
}
