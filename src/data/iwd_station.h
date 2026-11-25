#ifndef NETWORK_SIDEBAR_DATA_IWD_STATION_H
#define NETWORK_SIDEBAR_DATA_IWD_STATION_H

#include <glib.h>

G_BEGIN_DECLS

/* A normal iwd disconnect takes well under a second. One that lasts this long
 * has lost its netdev callback: iwd refuses scans and connections until the
 * device is powered off and on. */
#define NETWORK_SIDEBAR_IWD_STUCK_SECONDS 5

typedef struct _NetworkSidebarIwdStation NetworkSidebarIwdStation;

typedef void (*NetworkSidebarIwdStationChanged)(gpointer user_data);

/* Watches iwd's Station objects on the system bus, read-only. NetworkManager's
 * iwd backend does not expose the station state, and refuses Wi-Fi scans
 * whenever it is neither "connected" nor "disconnected". */
NetworkSidebarIwdStation *
network_sidebar_iwd_station_new(NetworkSidebarIwdStationChanged changed,
                                gpointer user_data);
void network_sidebar_iwd_station_free(NetworkSidebarIwdStation *self);

/* The state of a station that cannot scan now ("connecting",
 * "disconnecting", "roaming"), preferring "disconnecting"; NULL when every
 * station can scan or iwd is not running. */
const char *
network_sidebar_iwd_station_get_busy_state(NetworkSidebarIwdStation *self);

/* TRUE when a station has been "disconnecting" for at least
 * NETWORK_SIDEBAR_IWD_STUCK_SECONDS; out_seconds gets how long. */
gboolean network_sidebar_iwd_station_is_stuck(NetworkSidebarIwdStation *self,
                                              gint64 *out_seconds);

G_END_DECLS

#endif
