#include "sections/wifi.h"

#include "data/connection_state.h"
#include "data/labels.h"
#include "sections/helpers.h"
#include "sections/traffic.h"

#include <string.h>

typedef struct {
  NetworkSidebarActions *actions;
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMActiveConnection *active;
  NMRemoteConnection *saved;
} WifiRowAction;

typedef struct {
  NMDeviceWifi *device;
  NMAccessPoint *ap;
  NMActiveConnection *active;
  NetworkSidebarRowState row_state;
} WifiMember;

typedef struct {
  GPtrArray *members;
  GPtrArray *saved_connections;
  gboolean show_device_iface;
} WifiEntry;

static void
wifi_row_action_free(WifiRowAction *data)
{
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->device);
  g_clear_object(&data->ap);
  g_clear_object(&data->active);
  g_clear_object(&data->saved);
  g_free(data);
}

static void
wifi_row_action_closure_notify(gpointer data, GClosure *closure)
{
  (void) closure;
  wifi_row_action_free(data);
}

static void
wifi_member_free(WifiMember *member)
{
  if (member == NULL)
    return;
  g_clear_object(&member->device);
  g_clear_object(&member->ap);
  g_clear_object(&member->active);
  g_free(member);
}

static void
wifi_entry_free(WifiEntry *entry)
{
  if (entry == NULL)
    return;
  g_clear_pointer(&entry->members, g_ptr_array_unref);
  g_clear_pointer(&entry->saved_connections, g_ptr_array_unref);
  g_free(entry);
}

static gboolean
ssid_bytes_equal(GBytes *left, GBytes *right)
{
  const guint8 *left_bytes;
  const guint8 *right_bytes;
  gsize left_len = 0;
  gsize right_len = 0;

  if (left != NULL)
    left_bytes = g_bytes_get_data(left, &left_len);
  else
    left_bytes = NULL;
  if (right != NULL)
    right_bytes = g_bytes_get_data(right, &right_len);
  else
    right_bytes = NULL;
  if (left_len != right_len)
    return FALSE;
  if (left_len == 0)
    return TRUE;
  return memcmp(left_bytes, right_bytes, left_len) == 0;
}

static gboolean
ap_has_ssid(NMAccessPoint *ap)
{
  GBytes *ssid = nm_access_point_get_ssid(ap);
  gsize length = 0;

  if (ssid == NULL)
    return FALSE;
  g_bytes_get_data(ssid, &length);
  return length > 0;
}

static NM80211ApFlags
ap_security_flags(NMAccessPoint *ap)
{
  return nm_access_point_get_flags(ap) & NM_802_11_AP_FLAGS_PRIVACY;
}

static char *
ap_bssid_or_path(NMAccessPoint *ap)
{
  const char *bssid = nm_access_point_get_bssid(ap);
  const char *path;

  if (bssid != NULL && *bssid != '\0')
    return g_ascii_strdown(bssid, -1);
  path = nm_object_get_path(NM_OBJECT(ap));
  return g_strdup(path != NULL ? path : "");
}

static char *
device_identity(NMDeviceWifi *device)
{
  const char *path = nm_object_get_path(NM_OBJECT(device));
  const char *iface;

  if (path != NULL && *path != '\0')
    return g_strdup(path);
  iface = nm_device_get_iface(NM_DEVICE(device));
  return g_strdup(iface != NULL ? iface : "unknown");
}

static gboolean
same_hidden_ap_identity(WifiMember *member, NMDeviceWifi *device, NMAccessPoint *ap)
{
  g_autofree char *member_device = device_identity(member->device);
  g_autofree char *candidate_device = device_identity(device);
  g_autofree char *entry_bssid = ap_bssid_or_path(member->ap);
  g_autofree char *candidate_bssid = ap_bssid_or_path(ap);

  return g_strcmp0(member_device, candidate_device) == 0 &&
         g_strcmp0(entry_bssid, candidate_bssid) == 0 &&
         nm_access_point_get_frequency(member->ap) == nm_access_point_get_frequency(ap) &&
         nm_access_point_get_mode(member->ap) == nm_access_point_get_mode(ap) &&
         nm_access_point_get_flags(member->ap) == nm_access_point_get_flags(ap) &&
         nm_access_point_get_wpa_flags(member->ap) == nm_access_point_get_wpa_flags(ap) &&
         nm_access_point_get_rsn_flags(member->ap) == nm_access_point_get_rsn_flags(ap);
}

static gboolean
same_network_identity(WifiEntry *entry, NMDeviceWifi *device, NMAccessPoint *ap)
{
  WifiMember *first = g_ptr_array_index(entry->members, 0);
  gboolean first_has_ssid = ap_has_ssid(first->ap);
  gboolean candidate_has_ssid = ap_has_ssid(ap);

  if (!first_has_ssid || !candidate_has_ssid)
    return !first_has_ssid && !candidate_has_ssid && same_hidden_ap_identity(first, device, ap);

  return ssid_bytes_equal(nm_access_point_get_ssid(first->ap), nm_access_point_get_ssid(ap)) &&
         nm_access_point_get_mode(first->ap) == nm_access_point_get_mode(ap) &&
         ap_security_flags(first->ap) == ap_security_flags(ap) &&
         nm_access_point_get_wpa_flags(first->ap) == nm_access_point_get_wpa_flags(ap) &&
         nm_access_point_get_rsn_flags(first->ap) == nm_access_point_get_rsn_flags(ap);
}

static gboolean
row_state_preferred(NetworkSidebarRowState state)
{
  return state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE || state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
}

static int
row_state_rank(NetworkSidebarRowState state)
{
  if (state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
    return 0;
  if (state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING)
    return 1;
  return 2;
}

static gboolean
ap_matches_active(NMDeviceWifi *device, NMAccessPoint *ap, NMActiveConnection *active)
{
  NMAccessPoint *active_ap = nm_device_wifi_get_active_access_point(device);
  const char *ap_path = nm_object_get_path(NM_OBJECT(ap));
  const char *specific_path = active != NULL ? nm_active_connection_get_specific_object_path(active) : NULL;

  if (active_ap != NULL && g_strcmp0(nm_object_get_path(NM_OBJECT(active_ap)), ap_path) == 0)
    return TRUE;
  return specific_path != NULL && g_strcmp0(specific_path, "/") != 0 && g_strcmp0(specific_path, ap_path) == 0;
}

static WifiMember *
wifi_member_new(NMDeviceWifi *device,
                NMAccessPoint *ap,
                NMActiveConnection *active,
                NetworkSidebarRowState row_state)
{
  WifiMember *member = g_new0(WifiMember, 1);

  member->device = g_object_ref(device);
  member->ap = g_object_ref(ap);
  member->active = active != NULL ? g_object_ref(active) : NULL;
  member->row_state = row_state;
  return member;
}

static gboolean
wifi_member_matches_profile(WifiMember *member, NMRemoteConnection *profile)
{
  NMConnection *connection = NM_CONNECTION(profile);

  return nm_device_connection_valid(NM_DEVICE(member->device), connection) &&
         nm_access_point_connection_valid(member->ap, connection);
}

static gboolean
wifi_member_active_for_profile(WifiMember *member, NMRemoteConnection *profile)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
  const char *active_uuid = member->active != NULL ? nm_active_connection_get_uuid(member->active) : NULL;

  return uuid != NULL && active_uuid != NULL && g_strcmp0(uuid, active_uuid) == 0;
}

static gboolean
wifi_member_preferred(WifiMember *candidate, WifiMember *current, NMRemoteConnection *profile)
{
  int candidate_rank;
  int current_rank;

  if (profile != NULL) {
    candidate_rank = wifi_member_active_for_profile(candidate, profile) ? row_state_rank(candidate->row_state) : 2;
    current_rank = wifi_member_active_for_profile(current, profile) ? row_state_rank(current->row_state) : 2;
  } else {
    candidate_rank = row_state_rank(candidate->row_state);
    current_rank = row_state_rank(current->row_state);
  }

  if (candidate_rank != current_rank)
    return candidate_rank < current_rank;
  return nm_access_point_get_strength(candidate->ap) > nm_access_point_get_strength(current->ap);
}

static WifiMember *
wifi_entry_best_member_for_profile(WifiEntry *entry, NMRemoteConnection *profile)
{
  WifiMember *best = NULL;

  for (guint i = 0; i < entry->members->len; i++) {
    WifiMember *member = g_ptr_array_index(entry->members, i);

    if (profile != NULL &&
        !wifi_member_active_for_profile(member, profile) &&
        !wifi_member_matches_profile(member, profile))
      continue;
    if (best == NULL || wifi_member_preferred(member, best, profile))
      best = member;
  }
  return best;
}

static WifiMember *
wifi_entry_best_member(WifiEntry *entry)
{
  return wifi_entry_best_member_for_profile(entry, NULL);
}

static void
assign_saved_wifi_connections(GPtrArray *entries, GPtrArray *profiles)
{
  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);

    entry->saved_connections = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint j = 0; profiles != NULL && j < profiles->len; j++) {
      NMRemoteConnection *profile = g_ptr_array_index(profiles, j);

      if (wifi_entry_best_member_for_profile(entry, profile) != NULL)
        g_ptr_array_add(entry->saved_connections, g_object_ref(profile));
    }
  }
}

static gint
saved_connection_compare(gconstpointer left, gconstpointer right)
{
  NMRemoteConnection *left_connection = *(NMRemoteConnection * const *) left;
  NMRemoteConnection *right_connection = *(NMRemoteConnection * const *) right;
  const char *left_id = nm_connection_get_id(NM_CONNECTION(left_connection));
  const char *right_id = nm_connection_get_id(NM_CONNECTION(right_connection));
  g_autofree char *left_folded = g_utf8_casefold(left_id != NULL ? left_id : "", -1);
  g_autofree char *right_folded = g_utf8_casefold(right_id != NULL ? right_id : "", -1);
  int id_compare = g_strcmp0(left_folded, right_folded);

  if (id_compare != 0)
    return id_compare;
  return g_strcmp0(nm_connection_get_uuid(NM_CONNECTION(left_connection)), nm_connection_get_uuid(NM_CONNECTION(right_connection)));
}

static GPtrArray *
saved_wifi_profiles(NMClient *client)
{
  const GPtrArray *connections = nm_client_get_connections(client);
  GPtrArray *profiles = g_ptr_array_new_with_free_func(g_object_unref);

  for (guint i = 0; connections != NULL && i < connections->len; i++) {
    NMRemoteConnection *connection = g_ptr_array_index((GPtrArray *) connections, i);

    if (g_strcmp0(nm_connection_get_connection_type(NM_CONNECTION(connection)), NM_SETTING_WIRELESS_SETTING_NAME) == 0)
      g_ptr_array_add(profiles, g_object_ref(connection));
  }
  g_ptr_array_sort(profiles, saved_connection_compare);
  return profiles;
}

static char *
saved_profile_key(NMRemoteConnection *connection)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(connection));
  const char *path;

  if (uuid != NULL && *uuid != '\0')
    return g_strdup_printf("uuid:%s", uuid);
  path = nm_object_get_path(NM_OBJECT(connection));
  if (path != NULL && *path != '\0')
    return g_strdup_printf("path:%s", path);
  return g_strdup_printf("object:%p", connection);
}

static NMActiveConnection *
active_connection_by_uuid(NMClient *client, const char *uuid)
{
  const GPtrArray *active_connections = nm_client_get_active_connections(client);

  if (uuid == NULL)
    return NULL;
  for (guint i = 0; active_connections != NULL && i < active_connections->len; i++) {
    NMActiveConnection *active = g_ptr_array_index((GPtrArray *) active_connections, i);
    if (g_strcmp0(uuid, nm_active_connection_get_uuid(active)) == 0)
      return active;
  }
  return NULL;
}

static NMDeviceWifi *
compatible_wifi_device_for_profile(NMClient *client, NMRemoteConnection *connection)
{
  const GPtrArray *devices = nm_client_get_devices(client);
  NMConnection *nm_connection = NM_CONNECTION(connection);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    if (nm_device_connection_valid(device, nm_connection))
      return NM_DEVICE_WIFI(device);
  }
  return NULL;
}

static gboolean
saved_profile_is_connected(NMClient *client, NMRemoteConnection *profile)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
  NMActiveConnection *active = active_connection_by_uuid(client, uuid);

  return row_state_preferred(network_sidebar_connection_row_state(active));
}

static GPtrArray *
build_wifi_entries(NMClient *client)
{
  const GPtrArray *devices = nm_client_get_devices(client);
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify) wifi_entry_free);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    NMDeviceWifi *wifi;
    NMActiveConnection *active_connection;
    const GPtrArray *access_points;

    if (nm_device_get_device_type(device) != NM_DEVICE_TYPE_WIFI)
      continue;
    wifi = NM_DEVICE_WIFI(device);
    active_connection = nm_device_get_active_connection(device);
    access_points = nm_device_wifi_get_access_points(wifi);

    for (guint j = 0; access_points != NULL && j < access_points->len; j++) {
      NMAccessPoint *ap = g_ptr_array_index((GPtrArray *) access_points, j);
      NMActiveConnection *current_active = ap_matches_active(wifi, ap, active_connection) ? active_connection : NULL;
      NetworkSidebarRowState row_state = network_sidebar_connection_row_state(current_active);
      WifiEntry *current = NULL;

      for (guint k = 0; k < entries->len; k++) {
        WifiEntry *entry = g_ptr_array_index(entries, k);
        if (same_network_identity(entry, wifi, ap)) {
          current = entry;
          break;
        }
      }

      if (current == NULL) {
        current = g_new0(WifiEntry, 1);
        current->members = g_ptr_array_new_with_free_func((GDestroyNotify) wifi_member_free);
        g_ptr_array_add(entries, current);
      }
      g_ptr_array_add(current->members, wifi_member_new(wifi, ap, current_active, row_state));
    }
  }
  return entries;
}

static gint
wifi_entry_compare(gconstpointer left, gconstpointer right)
{
  WifiEntry *left_entry = *(WifiEntry * const *) left;
  WifiEntry *right_entry = *(WifiEntry * const *) right;
  WifiMember *left_member = wifi_entry_best_member(left_entry);
  WifiMember *right_member = wifi_entry_best_member(right_entry);
  int left_rank = row_state_rank(left_member->row_state);
  int right_rank = row_state_rank(right_member->row_state);

  if (left_rank != right_rank)
    return left_rank - right_rank;
  return (int) nm_access_point_get_strength(right_member->ap) - (int) nm_access_point_get_strength(left_member->ap);
}

static gboolean
wifi_entry_has_multiple_devices(WifiEntry *entry)
{
  WifiMember *first = g_ptr_array_index(entry->members, 0);

  for (guint i = 1; i < entry->members->len; i++) {
    WifiMember *member = g_ptr_array_index(entry->members, i);

    if (member->device != first->device)
      return TRUE;
  }
  return FALSE;
}

static gboolean
same_ap_display_identity(NMAccessPoint *left, NMAccessPoint *right)
{
  const char *left_bssid = nm_access_point_get_bssid(left);
  const char *right_bssid = nm_access_point_get_bssid(right);

  return ssid_bytes_equal(nm_access_point_get_ssid(left), nm_access_point_get_ssid(right)) &&
         g_ascii_strcasecmp(left_bssid != NULL ? left_bssid : "", right_bssid != NULL ? right_bssid : "") == 0 &&
         nm_access_point_get_frequency(left) == nm_access_point_get_frequency(right) &&
         nm_access_point_get_mode(left) == nm_access_point_get_mode(right) &&
         ap_security_flags(left) == ap_security_flags(right) &&
         nm_access_point_get_wpa_flags(left) == nm_access_point_get_wpa_flags(right) &&
         nm_access_point_get_rsn_flags(left) == nm_access_point_get_rsn_flags(right);
}

static void
mark_device_interfaces(GPtrArray *entries)
{
  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    WifiMember *member = g_ptr_array_index(entry->members, 0);

    entry->show_device_iface = wifi_entry_has_multiple_devices(entry);
    if (entry->show_device_iface || ap_has_ssid(member->ap))
      continue;

    for (guint j = 0; j < entries->len; j++) {
      WifiEntry *other = g_ptr_array_index(entries, j);
      WifiMember *other_member = g_ptr_array_index(other->members, 0);

      if (entry == other || member->device == other_member->device)
        continue;
      if (same_ap_display_identity(member->ap, other_member->ap)) {
        entry->show_device_iface = TRUE;
        break;
      }
    }
  }
}

static char *
wifi_entry_frequency_label(WifiEntry *entry, NMRemoteConnection *profile)
{
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GString) labels = g_string_new(NULL);

  for (guint i = 0; i < entry->members->len; i++) {
    WifiMember *member = g_ptr_array_index(entry->members, i);
    g_autofree char *label = network_sidebar_frequency_label(nm_access_point_get_frequency(member->ap));

    if (profile != NULL &&
        !wifi_member_active_for_profile(member, profile) &&
        !wifi_member_matches_profile(member, profile))
      continue;
    if (g_hash_table_contains(seen, label))
      continue;
    g_hash_table_add(seen, g_strdup(label));
    if (labels->len > 0)
      g_string_append(labels, ", ");
    g_string_append(labels, label);
  }
  return g_string_free(g_steal_pointer(&labels), FALSE);
}

static char *
channel_label(guint32 mhz)
{
  if (mhz == 2484)
    return g_strdup("Channel 14");
  if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 2407) / 5);
  if (mhz >= 5000 && mhz <= 5900 && (mhz - 5000) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 5000) / 5);
  if (mhz >= 5955 && mhz <= 7115 && (mhz - 5950) % 5 == 0)
    return g_strdup_printf("Channel %u", (mhz - 5950) / 5);
  return NULL;
}

static void
append_subtitle_part(GString *subtitle, const char *part)
{
  if (part == NULL || *part == '\0')
    return;
  if (subtitle->len > 0)
    g_string_append(subtitle, " - ");
  g_string_append(subtitle, part);
}

static char *
wifi_entry_subtitle(WifiEntry *entry, WifiMember *member, NMRemoteConnection *saved)
{
  g_autoptr(GString) subtitle = g_string_new(NULL);
  g_autofree char *signal = g_strdup_printf("Signal %u%%", nm_access_point_get_strength(member->ap));
  g_autofree char *security = network_sidebar_ap_security_label(member->ap);
  g_autofree char *frequency = wifi_entry_frequency_label(entry, saved);

  append_subtitle_part(subtitle, signal);
  append_subtitle_part(subtitle, security);
  append_subtitle_part(subtitle, frequency);
  if (!ap_has_ssid(member->ap)) {
    const char *bssid = nm_access_point_get_bssid(member->ap);
    g_autofree char *bssid_part = bssid != NULL && *bssid != '\0' ? g_strdup_printf("BSSID %s", bssid) : NULL;
    g_autofree char *channel = channel_label(nm_access_point_get_frequency(member->ap));
    append_subtitle_part(subtitle, bssid_part);
    append_subtitle_part(subtitle, channel);
  }
  if (entry->show_device_iface) {
    const char *iface = nm_device_get_iface(NM_DEVICE(member->device));
    g_autofree char *iface_part = g_strdup_printf("Interface %s", iface != NULL ? iface : "unknown");
    append_subtitle_part(subtitle, iface_part);
  }
  /* Connected and connecting rows are saved by definition. */
  if (saved != NULL && !row_state_preferred(member->row_state))
    append_subtitle_part(subtitle, "Saved");
  return g_string_free(g_steal_pointer(&subtitle), FALSE);
}

static char *
wifi_entry_title(WifiMember *member, NMRemoteConnection *saved, NMActiveConnection *active)
{
  g_autofree char *ssid = network_sidebar_ap_ssid_text(member->ap);

  if (saved != NULL)
    return network_sidebar_connection_name(NM_CONNECTION(saved), ssid);
  if (active != NULL)
    return network_sidebar_active_connection_name(active, ssid);
  return g_steal_pointer(&ssid);
}

static GtkWidget *
subgroup_list_with_suffix(const char *title, GtkWidget *suffix, GtkWidget **out_list)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *list = gtk_list_box_new();

  gtk_widget_add_css_class(box, "network-subgroup");
  if (title != NULL || suffix != NULL) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    if (title != NULL) {
      GtkWidget *label = gtk_label_new(title);

      gtk_widget_add_css_class(label, "network-subsection-label");
      gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
      gtk_widget_set_hexpand(label, TRUE);
      gtk_box_append(GTK_BOX(header), label);
    }
    if (suffix != NULL)
      gtk_box_append(GTK_BOX(header), suffix);
    gtk_box_append(GTK_BOX(box), header);
  }
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_box_append(GTK_BOX(box), list);
  *out_list = list;
  return box;
}

static GtkWidget *
subgroup_list(const char *title, GtkWidget **out_list)
{
  return subgroup_list_with_suffix(title, NULL, out_list);
}

static void
on_wifi_row_activated(GtkListBoxRow *row, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) row;

  if (data->active != NULL)
    network_sidebar_actions_show_connection(data->actions, data->saved, data->active);
  else if (data->saved != NULL && data->device != NULL && data->ap != NULL)
    network_sidebar_actions_activate_saved_wifi_profile_on_device(data->actions, data->saved, data->device, data->ap);
  else if (data->device != NULL && data->ap != NULL)
    network_sidebar_actions_connect_wifi(data->actions, data->device, data->ap);
  else if (data->saved != NULL)
    network_sidebar_actions_activate_saved_wifi_profile(data->actions, data->saved);
}

static void
on_wifi_details_clicked(GtkButton *button, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) button;
  network_sidebar_actions_show_connection(data->actions, data->saved, data->active);
}

static void
on_wifi_disconnect_clicked(GtkButton *button, gpointer user_data)
{
  WifiRowAction *data = user_data;
  (void) button;
  if (data->active) network_sidebar_actions_deactivate(data->actions, data->active);
}

static void
add_wifi_row_controls(GtkWidget *row, WifiRowAction *data)
{
  GtkWidget *button;
  if (data->active) {
    button = network_sidebar_flat_button("network-disconnect-symbolic", "Disconnect");
    g_signal_connect(button, "clicked", G_CALLBACK(on_wifi_disconnect_clicked), data);
    gtk_widget_set_tooltip_text(row, "Open network details and settings");
  } else {
    button = network_sidebar_flat_button("emblem-system-symbolic", "Network details and settings");
    g_signal_connect(button, "clicked", G_CALLBACK(on_wifi_details_clicked), data);
  }
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
}

static void
on_wifi_switch(GtkSwitch *switch_widget, GParamSpec *pspec, gpointer user_data)
{
  NetworkSidebarActions *actions = user_data;
  (void) pspec;
  network_sidebar_actions_set_wireless_enabled(actions, gtk_switch_get_active(switch_widget));
}

static void
on_add_wifi_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_actions_open_editor(user_data, (const char *const[]) { "--create", "--type=802-11-wireless", NULL });
}

/* What a connecting device is waiting on, in words. */
static const char *
connecting_phase(NMDevice *device)
{
  switch (device != NULL ? nm_device_get_state(device) : NM_DEVICE_STATE_UNKNOWN) {
  case NM_DEVICE_STATE_PREPARE: return "Preparing";
  case NM_DEVICE_STATE_CONFIG: return "Associating";
  case NM_DEVICE_STATE_NEED_AUTH: return "Waiting for password";
  case NM_DEVICE_STATE_IP_CONFIG: return "Getting IP address";
  case NM_DEVICE_STATE_IP_CHECK: return "Checking connectivity";
  case NM_DEVICE_STATE_SECONDARIES: return "Starting secondary connections";
  default: return "Connecting";
  }
}

static char *
status_subtitle(NetworkSidebarRowState state, NMDevice *device, const char *details)
{
  const char *status = NULL;

  if (state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
    status = "Connected";
  else if (state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING)
    status = connecting_phase(device);
  if (status == NULL)
    return g_strdup(details);
  if (details == NULL || *details == '\0')
    return g_strdup(status);
  return g_strdup_printf("%s - %s", status, details);
}

static void
add_connecting_spinner(GtkWidget *row, NetworkSidebarRowState state)
{
  if (state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING) {
    GtkWidget *spinner = adw_spinner_new();
    gtk_widget_set_size_request(spinner, 16, 16);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spinner);
  }
}

static char *
saved_profile_row_subtitle(NMRemoteConnection *profile, NMActiveConnection *active, NetworkSidebarRowState row_state)
{
  g_autoptr(GString) subtitle = g_string_new(NULL);
  g_autofree char *details = network_sidebar_wifi_profile_subtitle(profile);

  if (row_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)
    append_subtitle_part(subtitle, "Connected");
  else if (row_state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING)
    append_subtitle_part(subtitle, "Connecting");
  else if (active != NULL)
    append_subtitle_part(subtitle, "Active connection");

  append_subtitle_part(subtitle, details);
  return g_string_free(g_steal_pointer(&subtitle), FALSE);
}

static char *
saved_profile_activation_note(NMClient *client, NMRemoteConnection *profile)
{
  if (!nm_client_networking_get_enabled(client))
    return g_strdup("Enable networking to connect");
  if (!nm_client_wireless_hardware_get_enabled(client))
    return g_strdup("Wi-Fi hardware is disabled");
  if (!nm_client_wireless_get_enabled(client))
    return g_strdup("Enable Wi-Fi to connect");
  if (compatible_wifi_device_for_profile(client, profile) == NULL)
    return g_strdup("No compatible Wi-Fi device");
  return NULL;
}

static void
add_saved_profile_row(GtkListBox *list, NMClient *client, NetworkSidebarActions *actions, NMRemoteConnection *profile)
{
  const char *uuid = nm_connection_get_uuid(NM_CONNECTION(profile));
  NMActiveConnection *active = active_connection_by_uuid(client, uuid);
  NetworkSidebarRowState row_state = network_sidebar_connection_row_state(active);
  g_autofree char *title = network_sidebar_connection_name(NM_CONNECTION(profile), "Wi-Fi");
  g_autofree char *subtitle = saved_profile_row_subtitle(profile, active, row_state);
  g_autofree char *activation_note = active == NULL ? saved_profile_activation_note(client, profile) : NULL;
  GtkWidget *row = network_sidebar_action_row(title, subtitle, "network-wireless-symbolic");
  WifiRowAction *row_action = g_new0(WifiRowAction, 1);
  gboolean can_activate = active != NULL || activation_note == NULL;

  row_action->actions = network_sidebar_actions_ref(actions);
  row_action->active = active != NULL ? g_object_ref(active) : NULL;
  row_action->saved = g_object_ref(profile);
  network_sidebar_apply_row_state(row, row_state);
  add_connecting_spinner(row, row_state);

  add_wifi_row_controls(row, row_action);

  if (activation_note != NULL)
    gtk_widget_set_tooltip_text(row, activation_note);
  gtk_widget_set_focusable(row, TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), can_activate);
  g_signal_connect_data(row, "activated", G_CALLBACK(on_wifi_row_activated), row_action, wifi_row_action_closure_notify, 0);
  gtk_list_box_append(list, row);
}

static guint
unmatched_saved_profile_count(NMClient *client, GPtrArray *profiles, GHashTable *visible_saved_keys)
{
  guint count = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (saved_profile_is_connected(client, profile))
      continue;
    count++;
  }
  return count;
}

static guint
add_unmatched_connected_saved_profile_rows(GtkListBox *list,
                                          NMClient *client,
                                          NetworkSidebarActions *actions,
                                          GPtrArray *profiles,
                                          GHashTable *visible_saved_keys)
{
  guint added = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (!saved_profile_is_connected(client, profile))
      continue;
    add_saved_profile_row(list, client, actions, profile);
    added++;
  }
  return added;
}

static guint
add_unmatched_saved_profile_rows(GtkListBox *list,
                                 NMClient *client,
                                 NetworkSidebarActions *actions,
                                 GPtrArray *profiles,
                                 GHashTable *visible_saved_keys)
{
  guint added = 0;

  for (guint i = 0; profiles != NULL && i < profiles->len; i++) {
    NMRemoteConnection *profile = g_ptr_array_index(profiles, i);
    g_autofree char *key = saved_profile_key(profile);

    if (visible_saved_keys != NULL && g_hash_table_contains(visible_saved_keys, key))
      continue;
    if (saved_profile_is_connected(client, profile))
      continue;
    add_saved_profile_row(list, client, actions, profile);
    added++;
  }
  return added;
}

static void
add_empty_saved_row(GtkListBox *list, const char *title)
{
  gtk_list_box_append(list, network_sidebar_action_row(title, NULL, NULL));
}

static gboolean
add_saved_profiles_only_group(AdwPreferencesGroup *group,
                              NMClient *client,
                              NetworkSidebarActions *actions,
                              GPtrArray *profiles)
{
  guint out_of_range_count;
  GtkWidget *saved_list = NULL;
  GtkWidget *saved_box;
  guint added;

  out_of_range_count = unmatched_saved_profile_count(client, profiles, NULL);
  if (out_of_range_count == 0)
    return FALSE;

  saved_box = subgroup_list("Saved networks", &saved_list);
  adw_preferences_group_add(group, saved_box);
  added = add_unmatched_saved_profile_rows(GTK_LIST_BOX(saved_list), client, actions, profiles, NULL);
  if (added == 0)
    add_empty_saved_row(GTK_LIST_BOX(saved_list), "No saved Wi-Fi profiles");
  return TRUE;
}

static void
add_network_row(GtkListBox *list,
                NMClient *client,
                NetworkSidebarActions *actions,
                WifiEntry *entry,
                WifiMember *member,
                NMRemoteConnection *saved,
                NMActiveConnection *active,
                NetworkSidebarRowState row_state)
{
  g_autofree char *title = wifi_entry_title(member, saved, active);
  g_autofree char *details = wifi_entry_subtitle(entry, member, saved);
  g_autofree char *subtitle = status_subtitle(row_state, NM_DEVICE(member->device), details);
  GtkWidget *row = network_sidebar_action_row(title, subtitle, network_sidebar_signal_icon(nm_access_point_get_strength(member->ap)));
  WifiRowAction *row_action = g_new0(WifiRowAction, 1);
  gboolean can_activate = active != NULL || (nm_client_networking_get_enabled(client) && nm_client_wireless_get_enabled(client));

  row_action->actions = network_sidebar_actions_ref(actions);
  row_action->device = g_object_ref(member->device);
  row_action->ap = g_object_ref(member->ap);
  row_action->active = active != NULL ? g_object_ref(active) : NULL;
  row_action->saved = saved != NULL ? g_object_ref(saved) : NULL;
  network_sidebar_apply_row_state(row, row_state);
  add_connecting_spinner(row, row_state);

  if (row_action->saved || row_action->active) add_wifi_row_controls(row, row_action);

  gtk_widget_set_focusable(row, TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), can_activate);
  g_signal_connect_data(row, "activated", G_CALLBACK(on_wifi_row_activated), row_action, wifi_row_action_closure_notify, 0);
  gtk_list_box_append(list, row);
}

static guint
add_active_network_rows(GtkListBox *list,
                        NMClient *client,
                        NetworkSidebarActions *actions,
                        WifiEntry *entry)
{
  g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  guint added = 0;

  for (guint i = 0; i < entry->members->len; i++) {
    WifiMember *member = g_ptr_array_index(entry->members, i);
    NMRemoteConnection *saved = NULL;

    if (!row_state_preferred(member->row_state) || member->active == NULL)
      continue;
    if (g_hash_table_contains(seen, member->active))
      continue;

    for (guint j = 0; entry->saved_connections != NULL && j < entry->saved_connections->len; j++) {
      NMRemoteConnection *candidate = g_ptr_array_index(entry->saved_connections, j);

      if (wifi_member_active_for_profile(member, candidate)) {
        saved = candidate;
        break;
      }
    }
    g_hash_table_add(seen, member->active);
    add_network_row(list, client, actions, entry, member, saved, member->active, member->row_state);
    added++;
  }
  return added;
}

static void
add_network_rows(GtkListBox *connected_list,
                 GtkListBox *available_list,
                 NMClient *client,
                 NetworkSidebarActions *actions,
                 WifiEntry *entry,
                 guint *connected_count,
                 guint *available_count)
{
  guint active_count = add_active_network_rows(connected_list, client, actions, entry);

  *connected_count += active_count;
  if (entry->saved_connections != NULL && entry->saved_connections->len > 0) {
    for (guint i = 0; i < entry->saved_connections->len; i++) {
      NMRemoteConnection *saved = g_ptr_array_index(entry->saved_connections, i);
      WifiMember *member = wifi_entry_best_member_for_profile(entry, saved);

      if (wifi_member_active_for_profile(member, saved) && row_state_preferred(member->row_state))
        continue;
      add_network_row(available_list,
                      client,
                      actions,
                      entry,
                      member,
                      saved,
                      NULL,
                      NETWORK_SIDEBAR_ROW_STATE_NONE);
      (*available_count)++;
    }
    return;
  }

  if (active_count > 0)
    return;

  {
    WifiMember *member = wifi_entry_best_member(entry);

    add_network_row(available_list,
                    client,
                    actions,
                    entry,
                    member,
                    NULL,
                    NULL,
                    NETWORK_SIDEBAR_ROW_STATE_NONE);
    (*available_count)++;
  }
}

/* The saved-networks search entry outlives content rebuilds so its text,
 * cursor and focus survive the frequent NetworkManager-driven refreshes. */
static GtkWidget *saved_search;
static GtkWidget *saved_search_list;

static gboolean
text_matches(const char *text, const char *query)
{
  g_autofree char *folded = NULL;

  if (text == NULL)
    return FALSE;
  folded = g_utf8_casefold(text, -1);
  return strstr(folded, query) != NULL;
}

static gboolean
saved_row_visible(GtkListBoxRow *row, gpointer user_data)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(saved_search));
  g_autofree char *query = NULL;
  (void) user_data;

  if (text == NULL || *text == '\0' || !ADW_IS_ACTION_ROW(row))
    return TRUE;
  query = g_utf8_casefold(text, -1);
  return text_matches(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row)), query) ||
         text_matches(adw_action_row_get_subtitle(ADW_ACTION_ROW(row)), query);
}

/* Typing from anywhere starts a search far down the panel: bring the entry
 * to the top so the matches are visible. */
static void
scroll_search_into_view(void)
{
  GtkWidget *scrolled = gtk_widget_get_ancestor(saved_search, GTK_TYPE_SCROLLED_WINDOW);
  GtkWidget *child = scrolled != NULL ? gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled)) : NULL;
  GtkAdjustment *adjustment;
  graphene_point_t point;

  if (child == NULL || !gtk_widget_compute_point(saved_search, child, &GRAPHENE_POINT_INIT(0, 0), &point))
    return;
  adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled));
  gtk_adjustment_set_value(adjustment, point.y - 36);
}

static void
on_saved_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  (void) user_data;
  if (saved_search_list != NULL)
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(saved_search_list));
  if (text != NULL && *text != '\0')
    scroll_search_into_view();
}

static GtkWidget *
saved_search_entry(void)
{
  if (saved_search == NULL) {
    saved_search = g_object_ref_sink(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(saved_search), "Search saved networks");
    g_signal_connect(saved_search, "search-changed", G_CALLBACK(on_saved_search_changed), NULL);
  }
  return saved_search;
}

static void
make_saved_list_searchable(GtkWidget *saved_box, GtkWidget *saved_list)
{
  GtkWidget *entry = saved_search_entry();
  GtkWidget *parent = gtk_widget_get_parent(entry);
  GtkWidget *placeholder = gtk_label_new("No matching saved networks");

  if (GTK_IS_BOX(parent))
    gtk_box_remove(GTK_BOX(parent), entry);
  gtk_box_insert_child_after(GTK_BOX(saved_box), entry, gtk_widget_get_first_child(saved_box));
  if (saved_search_list != NULL)
    g_object_remove_weak_pointer(G_OBJECT(saved_search_list), (gpointer *) &saved_search_list);
  saved_search_list = saved_list;
  g_object_add_weak_pointer(G_OBJECT(saved_search_list), (gpointer *) &saved_search_list);
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_widget_add_css_class(placeholder, "network-search-empty");
  gtk_list_box_set_placeholder(GTK_LIST_BOX(saved_list), placeholder);
  gtk_list_box_set_filter_func(GTK_LIST_BOX(saved_list), saved_row_visible, NULL, NULL);
}

void
network_sidebar_wifi_search_reset(void)
{
  if (saved_search != NULL)
    gtk_editable_set_text(GTK_EDITABLE(saved_search), "");
}

gboolean
network_sidebar_wifi_search_has_focus(void)
{
  GtkRoot *root = saved_search != NULL ? gtk_widget_get_root(saved_search) : NULL;
  GtkWidget *focus = root != NULL ? gtk_root_get_focus(root) : NULL;

  return focus != NULL && (focus == saved_search || gtk_widget_is_ancestor(focus, saved_search));
}

void
network_sidebar_wifi_search_restore_focus(gboolean focus)
{
  GtkWidget *text;

  if (saved_search == NULL)
    return;
  /* Typing anywhere in the panel searches saved networks. */
  gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(saved_search),
                                          gtk_widget_get_root(saved_search) != NULL ? GTK_WIDGET(gtk_widget_get_root(saved_search)) : NULL);
  if (!focus || gtk_widget_get_root(saved_search) == NULL)
    return;
  text = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(saved_search)));
  if (GTK_IS_TEXT(text))
    gtk_text_grab_focus_without_selecting(GTK_TEXT(text));
  else
    gtk_widget_grab_focus(saved_search);
}

static void
on_rescan_clicked(GtkButton *button, gpointer user_data)
{
  (void) button;
  network_sidebar_actions_rescan(user_data);
}

static gboolean
wifi_device_activated(NMClient *client)
{
  const GPtrArray *devices = nm_client_get_devices(client);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI && nm_device_get_state(device) == NM_DEVICE_STATE_ACTIVATED)
      return TRUE;
  }
  return FALSE;
}

static GtkWidget *
discard_group(GtkWidget *group)
{
  g_object_unref(g_object_ref_sink(group));
  return NULL;
}

/* Returns the group of other networks (available, then saved) for the caller
 * to place below the VPN section, or NULL when there is none. */
GtkWidget *
network_sidebar_add_wifi_group(GtkBox *content, NMClient *client, NetworkSidebarActions *actions, NetworkSidebarTraffic *traffic)
{
  GtkWidget *group = network_sidebar_section_group("Wi-Fi");
  GtkWidget *networks = adw_preferences_group_new();
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *add_button = network_sidebar_flat_button("list-add-symbolic", "Add Wi-Fi network");
  const GPtrArray *devices = nm_client_get_devices(client);
  gboolean has_wifi_device = FALSE;
  g_autoptr(GPtrArray) saved_profiles = NULL;
  g_autoptr(GPtrArray) entries = NULL;
  g_autoptr(GHashTable) visible_saved_keys = NULL;
  GtkWidget *connected_list = NULL;
  GtkWidget *available_list = NULL;
  GtkWidget *saved_list = NULL;
  GtkWidget *connected_box;
  GtkWidget *available_box;
  GtkWidget *saved_box;
  guint connected_count = 0;
  guint available_count = 0;
  guint saved_count = 0;
  gboolean wifi_usable = nm_client_networking_get_enabled(client) &&
                         nm_client_wireless_hardware_get_enabled(client) &&
                         nm_client_wireless_get_enabled(client);

  /* Scanning shows as a spinner where the rescan button sits. */
  if (wifi_usable && network_sidebar_actions_get_wifi_scan_in_progress(actions)) {
    GtkWidget *spinner = adw_spinner_new();
    gtk_widget_set_tooltip_text(spinner, "Scanning for networks");
    gtk_widget_set_size_request(spinner, 16, 16);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(spinner, 9);
    gtk_widget_set_margin_end(spinner, 9);
    gtk_box_append(GTK_BOX(header), spinner);
  } else if (wifi_usable) {
    GtkWidget *rescan = network_sidebar_flat_button("view-refresh-symbolic", "Scan for networks");
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan_clicked), actions);
    gtk_box_append(GTK_BOX(header), rescan);
  }
  g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_wifi_clicked), actions);
  gtk_box_append(GTK_BOX(header), add_button);

  if (nm_client_wireless_hardware_get_enabled(client)) {
    GtkWidget *wifi_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(wifi_switch), nm_client_wireless_get_enabled(client));
    gtk_widget_set_valign(wifi_switch, GTK_ALIGN_CENTER);
    g_signal_connect(wifi_switch, "notify::active", G_CALLBACK(on_wifi_switch), actions);
    gtk_box_append(GTK_BOX(header), wifi_switch);
  }
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), header);
  gtk_box_append(content, group);

  for (guint i = 0; devices != NULL && i < devices->len; i++) {
    NMDevice *device = g_ptr_array_index((GPtrArray *) devices, i);
    if (nm_device_get_device_type(device) == NM_DEVICE_TYPE_WIFI) {
      has_wifi_device = TRUE;
      break;
    }
  }
  saved_profiles = saved_wifi_profiles(client);

  if (!has_wifi_device) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No Wi-Fi devices", "No wireless adapters are managed", "network-wireless-symbolic");
    return add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(networks), client, actions, saved_profiles) ? networks : discard_group(networks);
  }
  if (!nm_client_wireless_hardware_get_enabled(client)) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi hardware is disabled", "Check airplane mode or the wireless hardware switch", "network-wireless-disabled-symbolic");
    return add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(networks), client, actions, saved_profiles) ? networks : discard_group(networks);
  }
  if (!nm_client_wireless_get_enabled(client)) {
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi is off", NULL, "network-wireless-disabled-symbolic");
    return add_saved_profiles_only_group(ADW_PREFERENCES_GROUP(networks), client, actions, saved_profiles) ? networks : discard_group(networks);
  }

  entries = build_wifi_entries(client);
  if (entries->len == 0 && saved_profiles->len == 0) {
    const char *scan_error = network_sidebar_actions_get_wifi_scan_error(actions);

    if (network_sidebar_actions_get_wifi_scan_in_progress(actions))
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Scanning for networks...", "Updating nearby Wi-Fi access points", "view-refresh-symbolic");
    else if (scan_error == NULL)
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "No networks found", "No access points are currently visible", "network-wireless-signal-none-symbolic");
    if (scan_error != NULL)
      network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi scan failed", scan_error, "dialog-warning-symbolic");
    return discard_group(networks);
  }

  if (network_sidebar_actions_get_wifi_scan_error(actions) != NULL)
    network_sidebar_add_notice(ADW_PREFERENCES_GROUP(group), "Wi-Fi scan failed", network_sidebar_actions_get_wifi_scan_error(actions), "dialog-warning-symbolic");

  visible_saved_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  mark_device_interfaces(entries);
  assign_saved_wifi_connections(entries, saved_profiles);
  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    g_ptr_array_sort(entry->saved_connections, saved_connection_compare);
    for (guint j = 0; entry->saved_connections != NULL && j < entry->saved_connections->len; j++) {
      NMRemoteConnection *saved = g_ptr_array_index(entry->saved_connections, j);
      g_hash_table_add(visible_saved_keys, saved_profile_key(saved));
    }
  }
  g_ptr_array_sort(entries, wifi_entry_compare);

  /* The connected network, with its traffic, is the panel's status card. */
  connected_box = subgroup_list(NULL, &connected_list);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), connected_box);

  available_box = subgroup_list("Available networks", &available_list);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(networks), available_box);

  saved_box = subgroup_list("Saved networks", &saved_list);
  gtk_widget_add_css_class(saved_box, "network-subgroup-separated");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(networks), saved_box);

  for (guint i = 0; i < entries->len; i++) {
    WifiEntry *entry = g_ptr_array_index(entries, i);
    add_network_rows(GTK_LIST_BOX(connected_list),
                     GTK_LIST_BOX(available_list),
                     client,
                     actions,
                     entry,
                     &connected_count,
                     &available_count);
  }
  connected_count += add_unmatched_connected_saved_profile_rows(GTK_LIST_BOX(connected_list),
                                                               client,
                                                               actions,
                                                               saved_profiles,
                                                               visible_saved_keys);
  saved_count += add_unmatched_saved_profile_rows(GTK_LIST_BOX(saved_list), client, actions, saved_profiles, visible_saved_keys);

  if (connected_count == 0)
    gtk_list_box_append(GTK_LIST_BOX(connected_list),
                        network_sidebar_action_row("Not connected", "Choose a network below", "network-wireless-disconnected-symbolic"));
  else if (wifi_device_activated(client))
    network_sidebar_traffic_add_rows(traffic, GTK_LIST_BOX(connected_list));
  if (available_count == 0)
    gtk_list_box_append(GTK_LIST_BOX(available_list), network_sidebar_action_row("No other networks in range", NULL, NULL));
  if (saved_count == 0)
    add_empty_saved_row(GTK_LIST_BOX(saved_list), "No saved networks out of range");
  else
    make_saved_list_searchable(saved_box, saved_list);

  return networks;
}
