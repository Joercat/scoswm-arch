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
        "*:Home\\:Net:71:WPA2:wlp2s0\n"
        ":Coffee \\\\ Guest:55:--:wlp1s0\n";
    count = wifi_parse_nmcli_networks(nm, networks, 8);
    CHECK(count == 3);
    CHECK(networks[0].in_use && networks[1].in_use && !networks[2].in_use);
    CHECK(!strcmp(networks[0].ssid, "Home:Net"));
    CHECK(!strcmp(networks[1].ssid, "Home:Net"));
    CHECK(strcmp(networks[0].device, networks[1].device) != 0);
    CHECK(!strcmp(networks[0].device, "wlp1s0"));
    CHECK(!strcmp(networks[1].device, "wlp2s0"));
    CHECK(!strcmp(networks[2].ssid, "Coffee \\ Guest"));
    CHECK(!strcmp(networks[2].security, "--") && networks[2].signal == 55);

    if (failures) printf("test_wifi: %d failures\n", failures);
    else printf("test_wifi: all checks passed\n");
    return failures ? 1 : 0;
}
