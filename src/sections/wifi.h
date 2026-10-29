#ifndef NETWORK_SIDEBAR_SECTIONS_WIFI_H
#define NETWORK_SIDEBAR_SECTIONS_WIFI_H

#include "actions/network_actions.h"
#include "sections/traffic.h"

#include <adwaita.h>

G_BEGIN_DECLS

/* Appends the Wi-Fi status group (connected network + traffic) and returns
 * the available/saved networks group for the caller to append, or NULL. */
GtkWidget *network_sidebar_add_wifi_group(GtkBox *content, NMClient *client,
                                          NetworkSidebarActions *actions,
                                          NetworkSidebarTraffic *traffic);
/* The saved-networks search keeps focus across content rebuilds: ask before
 * clearing the content, restore after rebuilding it. */
gboolean network_sidebar_wifi_search_has_focus(void);
void network_sidebar_wifi_search_restore_focus(gboolean focus);
/* Clears the query; the next show starts unfiltered. */
void network_sidebar_wifi_search_reset(void);

G_END_DECLS

#endif
