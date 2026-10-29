#include "sections/vpn.h"

#include "data/vpn_profiles.h"
#include "sections/helpers.h"

typedef struct {
  NetworkSidebarActions *actions;
  NMRemoteConnection *connection;
  NMActiveConnection *active;
  NetworkSidebarVpnProfileKind kind;
  gboolean can_activate;
} VpnRowAction;

typedef struct {
  NetworkSidebarActions *actions;
  GSimpleActionGroup *create_actions;
  NetworkSidebarProfileEditorPresenter *editor_presenter;
  /* The expander owns the rows. */
  AdwExpanderRow *expander;
  GPtrArray *rows;
  /* Collapsed unless a VPN is up or connecting; a manual toggle sticks until
   * that changes. */
  int user_expanded;
  gboolean was_active;
  gulong expanded_handler;
} VpnSection;

static void vpn_section_free(gpointer user_data) {
  VpnSection *data = user_data;

  g_ptr_array_unref(data->rows);
  g_object_unref(data->create_actions);
  network_sidebar_actions_unref(data->actions);
  network_sidebar_profile_editor_presenter_unref(data->editor_presenter);
  g_free(data);
}

static void vpn_section_add_row(VpnSection *data, GtkWidget *row) {
  adw_expander_row_add_row(data->expander, row);
  g_ptr_array_add(data->rows, row);
}

static void vpn_section_add_notice(VpnSection *data, const char *title,
                                   const char *subtitle) {
  vpn_section_add_row(data, network_sidebar_action_row(title, subtitle, NULL));
}

static void vpn_row_action_free(VpnRowAction *data) {
  network_sidebar_actions_unref(data->actions);
  g_clear_object(&data->connection);
  g_clear_object(&data->active);
  g_free(data);
}

static void vpn_row_action_closure_notify(gpointer data, GClosure *closure) {
  (void)closure;
  vpn_row_action_free(data);
}

static const char *profile_kind_label(NetworkSidebarVpnProfileKind kind) {
  return kind == NETWORK_SIDEBAR_VPN_PROFILE_WIREGUARD ? "WireGuard" : "VPN";
}

static const char *
activation_reason_label(NetworkSidebarVpnActivationReason reason) {
  switch (reason) {
  case NETWORK_SIDEBAR_VPN_ACTIVATION_NETWORKING_DISABLED:
    return "Enable networking to connect";
  case NETWORK_SIDEBAR_VPN_ACTIVATION_NO_BASE_CONNECTION:
    return "VPN needs an active network connection";
  case NETWORK_SIDEBAR_VPN_ACTIVATION_PLUGIN_MISSING:
    return "VPN plugin is not installed for this profile";
  case NETWORK_SIDEBAR_VPN_ACTIVATION_ALLOWED:
    return NULL;
  }
  return NULL;
}

static void on_vpn_row_activated(GtkListBoxRow *row, gpointer user_data) {
  VpnRowAction *data = user_data;
  (void)row;

  if (data->active != NULL)
    network_sidebar_actions_deactivate(data->actions, data->active);
  else if (!data->can_activate)
    return;
  else if (data->kind == NETWORK_SIDEBAR_VPN_PROFILE_WIREGUARD)
    network_sidebar_actions_activate_wireguard_connection(data->actions,
                                                          data->connection);
  else
    network_sidebar_actions_activate_vpn_connection(data->actions,
                                                    data->connection);
}

static void on_vpn_edit_clicked(GtkButton *button, gpointer user_data) {
  VpnRowAction *data = user_data;
  (void)button;
  network_sidebar_actions_edit_connection(data->actions, data->connection);
}

static void on_vpn_remove_clicked(GtkButton *button, gpointer user_data) {
  VpnRowAction *data = user_data;
  (void)button;
  network_sidebar_actions_confirm_delete_connection(
      data->actions, data->connection, profile_kind_label(data->kind));
}

static void on_import_profile_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  network_sidebar_profile_editor_presenter_import(user_data);
}

static void on_create_profile_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data) {
  NetworkSidebarProfileKind kind =
      g_strcmp0(g_action_get_name(G_ACTION(action)), "wireguard") == 0
          ? NETWORK_SIDEBAR_PROFILE_WIREGUARD
          : NETWORK_SIDEBAR_PROFILE_VPN;
  (void)parameter;

  network_sidebar_actions_create_profile(user_data, kind);
}

static GtkWidget *create_profile_menu(VpnSection *data) {
  static const GActionEntry entries[] = {
      {.name = "vpn", .activate = on_create_profile_activated},
      {.name = "wireguard", .activate = on_create_profile_activated},
  };
  GtkWidget *button = gtk_menu_button_new();
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GSimpleActionGroup) action_group = g_simple_action_group_new();

  g_action_map_add_action_entries(G_ACTION_MAP(action_group), entries,
                                  G_N_ELEMENTS(entries), data->actions);
  g_object_set_data_full(G_OBJECT(action_group), "nm-sidebar-actions",
                         network_sidebar_actions_ref(data->actions),
                         (GDestroyNotify)network_sidebar_actions_unref);
  data->create_actions = g_object_ref(action_group);
  g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                  G_ACTION_MAP(action_group), "vpn")),
                              FALSE);
  g_menu_append(menu, "VPN", "create.vpn");
  g_menu_append(menu, "WireGuard", "create.wireguard");
  gtk_widget_insert_action_group(button, "create",
                                 G_ACTION_GROUP(action_group));
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "list-add-symbolic");
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(button, "Create VPN or WireGuard");
  return button;
}

static void on_vpn_expanded(GObject *row, GParamSpec *pspec,
                            gpointer user_data) {
  VpnSection *data = user_data;
  (void)pspec;
  data->user_expanded = adw_expander_row_get_expanded(ADW_EXPANDER_ROW(row));
}

GtkWidget *network_sidebar_vpn_section_new(
    NetworkSidebarActions *actions,
    NetworkSidebarProfileEditorPresenter *editor_presenter) {
  GtkWidget *group = adw_preferences_group_new();
  GtkWidget *expander = adw_expander_row_new();
  GtkWidget *header_suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *import_button = network_sidebar_flat_button(
      "document-open-symbolic", "Import VPN configuration");
  VpnSection *data = g_new0(VpnSection, 1);

  data->actions = network_sidebar_actions_ref(actions);
  data->editor_presenter =
      network_sidebar_profile_editor_presenter_ref(editor_presenter);
  data->expander = ADW_EXPANDER_ROW(expander);
  data->rows = g_ptr_array_new();
  data->user_expanded = -1;
  g_signal_connect(import_button, "clicked",
                   G_CALLBACK(on_import_profile_clicked),
                   data->editor_presenter);
  gtk_box_append(GTK_BOX(header_suffix), import_button);
  gtk_box_append(GTK_BOX(header_suffix), create_profile_menu(data));
  gtk_widget_set_valign(header_suffix, GTK_ALIGN_CENTER);
  adw_expander_row_add_suffix(data->expander, header_suffix);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), "VPN");
  adw_expander_row_add_prefix(
      data->expander, gtk_image_new_from_icon_name("network-vpn-symbolic"));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), expander);
  data->expanded_handler = g_signal_connect(expander, "notify::expanded",
                                            G_CALLBACK(on_vpn_expanded), data);
  g_object_set_data_full(G_OBJECT(group), "vpn-section", data,
                         vpn_section_free);
  return group;
}

static char *join_subtitle_parts(const char *first, const char *second) {
  if (first != NULL && *first != '\0' && second != NULL && *second != '\0')
    return g_strdup_printf("%s - %s", first, second);
  if (first != NULL && *first != '\0')
    return g_strdup(first);
  if (second != NULL && *second != '\0')
    return g_strdup(second);
  return NULL;
}

static void add_connecting_spinner(GtkWidget *row,
                                   NetworkSidebarRowState state) {
  if (state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING) {
    GtkWidget *spinner = adw_spinner_new();
    gtk_widget_set_size_request(spinner, 16, 16);
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spinner);
  }
}

/* The collapsed header summarizes the section: the shown connection, or how
 * many profiles there are. */
static void
vpn_section_update_summary(VpnSection *data,
                           const NetworkSidebarVpnProfiles *snapshot) {
  const NetworkSidebarVpnProfile *shown = NULL;
  NetworkSidebarRowState shown_state = NETWORK_SIDEBAR_ROW_STATE_NONE;
  g_autofree char *summary = NULL;
  gboolean any_active;
  guint count = snapshot->profiles->len;

  for (guint i = 0; i < count; i++) {
    const NetworkSidebarVpnProfile *profile =
        g_ptr_array_index(snapshot->profiles, i);

    if (profile->active != NULL &&
        (shown == NULL ||
         profile->row_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE)) {
      shown = profile;
      shown_state = profile->row_state;
    }
  }

  any_active = shown_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE ||
               shown_state == NETWORK_SIDEBAR_ROW_STATE_CONNECTING;
  if (shown != NULL)
    summary = g_strdup_printf("%s: %s",
                              shown_state == NETWORK_SIDEBAR_ROW_STATE_ACTIVE
                                  ? "Connected"
                                  : "Connecting",
                              shown->name);
  else if (count == 0 &&
           snapshot->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_MISSING)
    summary = g_strdup("No VPN plugins installed");
  else
    summary = g_strdup_printf("%u profile%s", count, count == 1 ? "" : "s");
  adw_expander_row_set_subtitle(data->expander, summary);
  network_sidebar_apply_row_state(GTK_WIDGET(data->expander), shown_state);

  if (any_active != data->was_active)
    data->user_expanded = -1;
  data->was_active = any_active;
  g_signal_handler_block(data->expander, data->expanded_handler);
  adw_expander_row_set_expanded(data->expander, data->user_expanded >= 0
                                                    ? data->user_expanded
                                                    : any_active);
  g_signal_handler_unblock(data->expander, data->expanded_handler);
}

void network_sidebar_vpn_section_update(GtkWidget *section, NMClient *client) {
  VpnSection *data = g_object_get_data(G_OBJECT(section), "vpn-section");
  g_autoptr(NetworkSidebarVpnProfiles) snapshot =
      network_sidebar_vpn_profiles_load(client);

  /* Preserve the header, action group, and mapped popover across refreshes. */
  g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(
                                  G_ACTION_MAP(data->create_actions), "vpn")),
                              snapshot->can_create_vpn);
  for (guint i = 0; i < data->rows->len; i++)
    adw_expander_row_remove(data->expander, g_ptr_array_index(data->rows, i));
  g_ptr_array_set_size(data->rows, 0);

  if (snapshot->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_UNKNOWN)
    vpn_section_add_notice(data, "VPN plugin status unavailable",
                           "Could not check installed VPN plugins; "
                           "VPN actions remain available");
  else if (snapshot->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_MISSING)
    vpn_section_add_notice(
        data, "No VPN plugins installed",
        "Install a NetworkManager VPN plugin; WireGuard remains available");

  for (guint i = 0; i < snapshot->profiles->len; i++) {
    const NetworkSidebarVpnProfile *profile =
        g_ptr_array_index(snapshot->profiles, i);
    g_autofree char *subtitle = NULL;
    GtkWidget *row;
    GtkWidget *edit_button;
    GtkWidget *remove_button;
    VpnRowAction *row_action;
    const char *activation_note =
        activation_reason_label(profile->activation_reason);
    g_autofree char *duplicate_note = NULL;

    if (profile->duplicate_name && profile->uuid != NULL)
      duplicate_note = g_strdup_printf("UUID %s", profile->uuid);
    subtitle = join_subtitle_parts(profile->status, duplicate_note);
    row = network_sidebar_action_row(profile->name, subtitle,
                                     "network-vpn-symbolic");
    network_sidebar_apply_row_state(row, profile->row_state);
    if (activation_note != NULL)
      gtk_widget_set_tooltip_text(row, activation_note);

    row_action = g_new0(VpnRowAction, 1);
    row_action->actions = network_sidebar_actions_ref(data->actions);
    row_action->connection = g_object_ref(profile->connection);
    row_action->active =
        profile->active != NULL ? g_object_ref(profile->active) : NULL;
    row_action->kind = profile->kind;
    row_action->can_activate = profile->can_activate;

    edit_button = network_sidebar_flat_button("document-edit-symbolic", "Edit");
    if (profile->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_MISSING)
      gtk_widget_set_tooltip_text(
          edit_button,
          "Install the NetworkManager VPN plugin for this profile");
    else if (profile->plugin_status == NETWORK_SIDEBAR_VPN_PLUGIN_UNKNOWN)
      gtk_widget_set_tooltip_text(edit_button,
                                  "VPN plugin support could not be verified");
    gtk_widget_set_sensitive(edit_button, profile->can_edit);
    g_signal_connect(edit_button, "clicked", G_CALLBACK(on_vpn_edit_clicked),
                     row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), edit_button);

    remove_button =
        network_sidebar_flat_button("user-trash-symbolic", "Remove");
    g_signal_connect(remove_button, "clicked",
                     G_CALLBACK(on_vpn_remove_clicked), row_action);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row),
                                     profile->can_activate);
    g_signal_connect_data(row, "activated", G_CALLBACK(on_vpn_row_activated),
                          row_action, vpn_row_action_closure_notify, 0);
    add_connecting_spinner(row, profile->row_state);
    vpn_section_add_row(data, row);
  }

  if (snapshot->profiles->len == 0)
    vpn_section_add_notice(data, "No VPN profiles",
                           "Use Import or + to add one");

  vpn_section_update_summary(data, snapshot);
}
