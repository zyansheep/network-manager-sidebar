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
/* The group survives content rebuilds; append it again after each clear. */
void network_sidebar_traffic_attach(NetworkSidebarTraffic *traffic, GtkBox *content);
void network_sidebar_traffic_set_active(NetworkSidebarTraffic *traffic, gboolean active);

G_END_DECLS

#endif
