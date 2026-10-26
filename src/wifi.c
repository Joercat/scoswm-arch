#define _POSIX_C_SOURCE 200809L
#include "wifi.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <linux/wireless.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

static int adapter_index(struct wifi_adapter *out, int count, const char *name)
{
    for (int i = 0; i < count; i++) if (!strcmp(out[i].name, name)) return i;
    return -1;
}

static int adapter_add(struct wifi_adapter *out, int count, int maximum, const char *name)
{
    int existing = adapter_index(out, count, name);
    if (existing >= 0) return existing;
    if (count >= maximum || !name || !*name) return -1;
    memset(&out[count], 0, sizeof(out[count]));
    snprintf(out[count].name, sizeof(out[count].name), "%s", name);
    out[count].quality = -1; out[count].signal_dbm = -1000;
    return count;
}

int wifi_parse_proc_wireless(const char *text, struct wifi_adapter *out, int maximum)
{
    if (!text || !out || maximum <= 0) return 0;
    char *copy = strdup(text);
    if (!copy) return 0;
    int count = 0;
    char *line_save = NULL;
    for (char *line = strtok_r(copy, "\n", &line_save); line;
         line = strtok_r(NULL, "\n", &line_save)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *name = line;
        while (*name == ' ' || *name == '\t') name++;
        char *end = name + strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*name || strlen(name) >= WIFI_IFNAME_MAX) continue;
        int index = adapter_add(out, count, maximum, name);
        if (index < 0) continue;
        if (index == count) count++;
        char *save = NULL;
        strtok_r(colon + 1, " \t", &save); /* status */
        char *quality = strtok_r(NULL, " \t", &save);
        char *level = strtok_r(NULL, " \t", &save);
        if (quality) out[index].quality = (int)strtod(quality, NULL);
        if (level) out[index].signal_dbm = (int)strtod(level, NULL);
    }
    free(copy);
    return count;
}

static void fill_addresses(struct wifi_adapter *adapters, int count)
{
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0) return;
    /* Prefer IPv4 for the compact panel, but retain an IPv6-only card. */
    for (int pass = 0; pass < 2; pass++) {
        int family = pass == 0 ? AF_INET : AF_INET6;
        for (struct ifaddrs *it = list; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != family) continue;
            int index = adapter_index(adapters, count, it->ifa_name);
            if (index < 0 || adapters[index].address[0]) continue;
            if (getnameinfo(it->ifa_addr, family == AF_INET ? sizeof(struct sockaddr_in) :
                            sizeof(struct sockaddr_in6), adapters[index].address,
                            sizeof(adapters[index].address), NULL, 0, NI_NUMERICHOST) != 0)
                adapters[index].address[0] = 0;
        }
    }
    freeifaddrs(list);
}

static int adapter_compare(const void *left, const void *right)
{
    return strcmp(((const struct wifi_adapter *)left)->name,
                  ((const struct wifi_adapter *)right)->name);
}

int wifi_scan_adapters(struct wifi_adapter *out, int maximum)
{
    if (!out || maximum <= 0) return 0;
    int count = 0;
    FILE *proc = fopen("/proc/net/wireless", "r");
    if (proc) {
        char buffer[16384];
        size_t n = fread(buffer, 1, sizeof(buffer) - 1, proc);
        fclose(proc); buffer[n] = 0;
        count = wifi_parse_proc_wireless(buffer, out, maximum);
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { fill_addresses(out, count); return count; }
    DIR *sys = opendir("/sys/class/net");
    if (sys) {
        struct dirent *entry;
        while ((entry = readdir(sys)) != NULL) {
            if (entry->d_name[0] == '.' || strlen(entry->d_name) >= WIFI_IFNAME_MAX) continue;
            struct iwreq req; memset(&req, 0, sizeof(req));
            snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", entry->d_name);
            if (ioctl(fd, SIOCGIWNAME, &req) != 0) continue;
            int index = adapter_add(out, count, maximum, entry->d_name);
            if (index >= 0 && index == count) count++;
        }
        closedir(sys);
    }

    for (int i = 0; i < count; i++) {
        struct iwreq req; memset(&req, 0, sizeof(req));
        struct ifreq flags; memset(&flags, 0, sizeof(flags));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", out[i].name);
        snprintf(flags.ifr_name, sizeof(flags.ifr_name), "%s", out[i].name);
        if (ioctl(fd, SIOCGIFFLAGS, &flags) == 0) out[i].enabled = (flags.ifr_flags & IFF_UP) != 0;
        char ssid[WIFI_SSID_MAX]; memset(ssid, 0, sizeof(ssid));
        memset(&req, 0, sizeof(req));
        snprintf(req.ifr_name, sizeof(req.ifr_name), "%s", out[i].name);
        req.u.essid.pointer = ssid; req.u.essid.length = sizeof(ssid) - 1;
        if (ioctl(fd, SIOCGIWESSID, &req) == 0) {
            size_t n = req.u.essid.length;
            if (n > sizeof(ssid) - 1) n = sizeof(ssid) - 1;
            while (n && ssid[n - 1] == 0) n--;
            ssid[n] = 0;
            if (n && strcmp(ssid, "off/any")) {
                memcpy(out[i].ssid, ssid, n + 1);
                out[i].connected = 1;
            }
        }
    }
    close(fd);
    fill_addresses(out, count);
    qsort(out, (size_t)count, sizeof(*out), adapter_compare);
    return count;
}

static int nmcli_field(char **cursor, char *out, size_t cap, int *last)
{
    if (!cursor || !*cursor || !out || !cap) return 0;
    char *p = *cursor;
    size_t used = 0;
    *last = 0;
    while (*p && *p != ':' && *p != '\n' && *p != '\r') {
        if (*p == '\\' && p[1] && p[1] != '\n' && p[1] != '\r') p++;
        if (used + 1 < cap) out[used++] = *p;
        p++;
    }
    out[used] = 0;
    if (*p == ':') p++;
    else *last = 1;
    *cursor = p;
    return 1;
}

int wifi_parse_nmcli_networks(const char *text, struct wifi_network *out, int maximum)
{
    if (!text || !out || maximum <= 0) return 0;
    char *copy = strdup(text);
    if (!copy) return 0;
    int count = 0;
    char *cursor = copy;
    while (*cursor && count < maximum) {
        char active[16], ssid[WIFI_SSID_MAX], signal[24];
        char security[WIFI_SECURITY_MAX], device[WIFI_IFNAME_MAX];
        int last = 0;
        nmcli_field(&cursor, active, sizeof(active), &last);
        nmcli_field(&cursor, ssid, sizeof(ssid), &last);
        nmcli_field(&cursor, signal, sizeof(signal), &last);
        nmcli_field(&cursor, security, sizeof(security), &last);
        nmcli_field(&cursor, device, sizeof(device), &last);
        while (*cursor == '\r' || *cursor == '\n') cursor++;
        if (!device[0] || !signal[0]) continue;
        struct wifi_network *network = &out[count++];
        memset(network, 0, sizeof(*network));
        network->in_use = !strcmp(active, "*");
        network->signal = atoi(signal);
        snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
        snprintf(network->security, sizeof(network->security), "%s", security);
        snprintf(network->device, sizeof(network->device), "%s", device);
    }
    free(copy);
    return count;
}
