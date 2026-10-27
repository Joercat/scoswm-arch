#include "wifi.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; \
} } while (0)

int main(void)
{
    struct wifi_adapter adapters[8];
    const char *proc =
        "Inter-| sta-| Quality | Discarded packets | Missed | WE\n"
        " face | tus | link level noise | nwid crypt frag retry misc | beacon | 22\n"
        "wlp1s0: 0000   61.  -45.  -256.      0    0    0    0    0        0\n"
        "wlp2s0: 0000   42.  -67.  -256.      0    0    0    0    0        0\n";
    int count = wifi_parse_proc_wireless(proc, adapters, 8);
    CHECK(count == 2);
    CHECK(!strcmp(adapters[0].name, "wlp1s0") && adapters[0].quality == 61);
    CHECK(adapters[0].signal_dbm == -45);
    CHECK(!strcmp(adapters[1].name, "wlp2s0") && adapters[1].quality == 42);
    CHECK(adapters[1].signal_dbm == -67);

    struct wifi_network networks[8];
    const char *nm =
        "*:Home\\:Net:82:WPA2:wlp1s0\n"
        ":Home\\:Net:65:WPA2:wlp1s0\n" /* duplicate BSSID on the same card */
        "*:Home\\:Net:71:WPA2:wlp2s0\n"
        ":Coffee \\\\ Guest:55:--:wlp1s0\n";
    count = wifi_parse_nmcli_networks(nm, networks, 8);
    CHECK(count == 3);
    CHECK(networks[0].in_use && networks[1].in_use && !networks[2].in_use);
    CHECK(!strcmp(networks[0].ssid, "Home:Net") && networks[0].signal == 82);
    CHECK(!strcmp(networks[1].ssid, "Home:Net"));
    CHECK(strcmp(networks[0].device, networks[1].device) != 0);
    CHECK(!strcmp(networks[0].device, "wlp1s0"));
    CHECK(!strcmp(networks[1].device, "wlp2s0"));
    CHECK(!strcmp(networks[2].ssid, "Coffee \\ Guest"));
    CHECK(!strcmp(networks[2].security, "--") && networks[2].signal == 55);

    memset(adapters, 0, sizeof(adapters));
    snprintf(adapters[0].name, sizeof(adapters[0].name), "wlp2s0");
    snprintf(adapters[0].ssid, sizeof(adapters[0].ssid), "Cafe Hotspot");
    adapters[0].hosted = 1;
    snprintf(adapters[1].name, sizeof(adapters[1].name), "wlp1s0");
    snprintf(adapters[1].ssid, sizeof(adapters[1].ssid), "Home:Net");
    adapters[1].connected = 1;
    count = wifi_merge_adapter_activity(networks, count, 8, adapters, 2);
    CHECK(count == 4);
    CHECK(networks[0].in_use && networks[1].in_use && networks[2].in_use);
    CHECK(!networks[3].in_use && !strcmp(networks[3].ssid, "Coffee \\ Guest"));
    CHECK(networks[2].hosted && !strcmp(networks[2].ssid, "Cafe Hotspot"));
    CHECK(!strcmp(networks[2].device, "wlp2s0") && networks[2].signal == -1);
    CHECK(!networks[0].hosted && !strcmp(networks[0].device, "wlp1s0"));

    const char *dedup =
        ":Net:40:--:wlp1s0\n"
        "*:Net:55:WPA2:wlp1s0\n" /* active state and security arrive later */
        ":Net:60:WPA2:wlp1s0\n"  /* strongest signal wins */
        ":Net:70:WPA2:wlp2s0\n"
        "::50:--:wlp1s0\n"
        "::60:--:wlp1s0\n";      /* hidden rows are never collapsed */
    count = wifi_parse_nmcli_networks(dedup, networks, 8);
    CHECK(count == 4);
    CHECK(networks[0].in_use && networks[0].signal == 60);
    CHECK(!strcmp(networks[0].security, "WPA2"));
    CHECK(!strcmp(networks[1].device, "wlp2s0") && networks[1].signal == 70);
    CHECK(!networks[2].ssid[0] && !networks[3].ssid[0]);

    const char *inactive_first =
        ":Coffee:55:--:wlp1s0\n"
        "*:Home:82:WPA2:wlp1s0\n"
        ":Lab:60:WPA2:wlp2s0\n";
    count = wifi_parse_nmcli_networks(inactive_first, networks, 8);
    count = wifi_merge_adapter_activity(networks, count, 8, NULL, 0);
    CHECK(count == 3);
    CHECK(networks[0].in_use && !strcmp(networks[0].ssid, "Home"));
    CHECK(!networks[1].in_use && !strcmp(networks[1].ssid, "Coffee"));
    CHECK(!networks[2].in_use && !strcmp(networks[2].ssid, "Lab"));

    if (failures) printf("test_wifi: %d failures\n", failures);
    else printf("test_wifi: all checks passed\n");
    return failures ? 1 : 0;
}
