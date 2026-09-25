#ifndef NETWORK_SIDEBAR_SECTIONS_CONNECTION_INFO_H
#define NETWORK_SIDEBAR_SECTIONS_CONNECTION_INFO_H

#include <adwaita.h>
#include <NetworkManager.h>

G_BEGIN_DECLS

GtkWidget *network_sidebar_compact_info_row(const char *title, const char *value, GtkWidget **value_label);

void network_sidebar_add_active_connection_info_content(GtkBox *content, NMClient *client, NMActiveConnection *selected);

void network_sidebar_add_connection_info_content(GtkBox *content, NMClient *client);

G_END_DECLS

#endif
