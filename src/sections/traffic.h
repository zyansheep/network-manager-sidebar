#ifndef NETWORK_SIDEBAR_SECTIONS_TRAFFIC_H
#define NETWORK_SIDEBAR_SECTIONS_TRAFFIC_H

#include <adwaita.h>
#include <NetworkManager.h>

G_BEGIN_DECLS

typedef struct _NetworkSidebarTraffic NetworkSidebarTraffic;

/* Samples the Wi-Fi interface's byte counters in the background (every 2 s,
 * every 1 s while active) so the live graph is already filled when shown. */
NetworkSidebarTraffic *network_sidebar_traffic_new(NMClient *client);
void network_sidebar_traffic_free(NetworkSidebarTraffic *traffic);
/* Appends the live graph and the usage rows to the connected-network card.
 * The rows survive content rebuilds; call again after each clear. */
void network_sidebar_traffic_add_rows(NetworkSidebarTraffic *traffic, GtkListBox *list);
void network_sidebar_traffic_set_active(NetworkSidebarTraffic *traffic, gboolean active);

G_END_DECLS

#endif
