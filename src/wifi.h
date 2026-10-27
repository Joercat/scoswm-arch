/* Lightweight Linux Wi-Fi status and NetworkManager output parsing. */
#ifndef SCOS_WIFI_H
#define SCOS_WIFI_H

#define WIFI_SSID_MAX 129
#define WIFI_IFNAME_MAX 16
#define WIFI_ADDRESS_MAX 64
#define WIFI_SECURITY_MAX 64

struct wifi_adapter {
    char name[WIFI_IFNAME_MAX];
    char ssid[WIFI_SSID_MAX];
    char address[WIFI_ADDRESS_MAX];
    int quality;
    int signal_dbm;
    int enabled;
    int connected;                 /* joined a network as a client */
    int hosted;                    /* running an access-point hotspot */
};

struct wifi_network {
    char ssid[WIFI_SSID_MAX];
    char security[WIFI_SECURITY_MAX];
    char device[WIFI_IFNAME_MAX];
    int signal;                    /* -1 when only local active state is known */
    int in_use;
    int hosted;                    /* active row is our hotspot, not a joined network */
};

int wifi_parse_proc_wireless(const char *text, struct wifi_adapter *out, int maximum);
int wifi_scan_adapters(struct wifi_adapter *out, int maximum);
int wifi_parse_nmcli_networks(const char *text, struct wifi_network *out, int maximum);
int wifi_merge_adapter_activity(struct wifi_network *networks, int count, int maximum,
                                const struct wifi_adapter *adapters, int adapter_count);

#endif
