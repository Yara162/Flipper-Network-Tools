// Wi-Fi utility firmware for the official Flipper Zero Wi-Fi Devboard (ESP32-S2).
//
// The Flipper sends one command per line on UART0 (GPIO43 TX / GPIO44 RX, wired to the
// Flipper's pins 14 / 13) at 115200 baud. Fields are separated by tabs. Every reply line
// starts with '$' so the Flipper can ignore the chip's boot messages.
//
//   VER                      -> $VER <version>
//   SCAN                     -> $AP <ssid> <bssid> <channel> <rssi> <auth> ... $SCAN_END <count>
//   WATCH <ssid>             -> $RSSI <rssi> <channel> <bssid> <ap count>  or  $RSSI_NONE   (repeats)
//   JOIN <ssid> <password>   -> $JOIN_OK ... / $JOIN_FAIL <reason>, then $PING / $DNS lines, $JOIN_END
//   MDNS                     -> $SVC <label> <instance> <host> <ip> <port> ... $MDNS_END <count>
//   FIND                     -> $HOST <ip> <mac> <maker> ... $FIND_END <count>   (ARP sweep of the subnet)
//   PORTS <ip>               -> $PORT <number> <name> ... $PORTS_END <count>      (common control ports)
//   STAB                     -> $STAB <seq> <ms | -1 lost | -2 link down> <rssi> <bssid> <channel>  (repeats)
//   CTL <type> <rest>        -> $CTL ok|fail <reply text>     type: tcp, udp, http, wol, pjlink
//   TIME                     -> $TIME <unix time, UTC>  or  $TIME_FAIL
//   HALT                     -> stops the running activity, stays on the network: $HALTED
//   STOP                     -> stops the running activity and leaves the network: $STOPPED
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mdns.h>
#include <stdarg.h>
#include <time.h>
#include "ping/ping_sock.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "esp_netif_net_stack.h"
#include "mbedtls/md5.h"
#include "oui_table.h"

#define FW_VERSION 2
#define LED_RED 6
#define LED_GREEN 5
#define LED_BLUE 4
#define CMD_LINE_MAX 400
#define REPLY_MAX 150
#define SCAN_MS_PER_CHANNEL 120
#define JOIN_TIMEOUT_MS 15000
#define DHCP_TIMEOUT_MS 12000

enum Mode { ModeIdle, ModeWatch };

static Mode mode = ModeIdle;
static char watch_ssid[33];
static uint8_t watch_channel;
static uint32_t watch_round;

static volatile bool sta_associated, sta_got_ip;
static volatile bool link_lost; // the network dropped us while joined
static volatile uint8_t last_disconnect_reason;
static bool mdns_started;

static char line[CMD_LINE_MAX];
static size_t line_len;

// --- output ---

static void out(const char* fmt, ...) {
    char buf[320];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print('$');
    Serial.print(buf);
    Serial.print('\n');
}

// Network names can hold any bytes: keep them printable and free of our separator
static void clean(const char* src, char* dst, size_t size) {
    size_t n = 0;
    for(; src && *src && n + 1 < size; src++) dst[n++] = (*src >= 32 && *src < 127) ? *src : '?';
    dst[n] = 0;
}

static void led(bool r, bool g, bool b) {
    // the devboard's LEDs light when the pin is low
    digitalWrite(LED_RED, r ? LOW : HIGH);
    digitalWrite(LED_GREEN, g ? LOW : HIGH);
    digitalWrite(LED_BLUE, b ? LOW : HIGH);
}

static void led_idle(void) {
    if(WiFi.status() == WL_CONNECTED)
        led(false, true, false);
    else
        led(false, false, true);
}

// --- input ---

// Reads pending bytes; returns true when `line` holds a complete command
static bool read_line(void) {
    while(Serial.available()) {
        char c = (char)Serial.read();
        if(c == '\n' || c == '\r') {
            if(line_len == 0) continue;
            line[line_len] = 0;
            line_len = 0;
            return true;
        }
        if(line_len < CMD_LINE_MAX - 1)
            line[line_len++] = c;
        else
            line_len = 0;
    }
    return false;
}

enum Interrupt { InterruptNone, InterruptHalt, InterruptStop };

// Checks for HALT or STOP while an activity runs; any other command meanwhile is dropped
static Interrupt interrupt_requested(void) {
    while(read_line()) {
        if(strcmp(line, "STOP") == 0) return InterruptStop;
        if(strcmp(line, "HALT") == 0) return InterruptHalt;
    }
    return InterruptNone;
}

// --- survey ---

static const char* auth_name(wifi_auth_mode_t auth) {
    switch(auth) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "Ent";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default: return "?";
    }
}

static void leave_network(void) {
    if(mdns_started) {
        mdns_free();
        mdns_started = false;
    }
    WiFi.disconnect(false, true);
    sta_associated = sta_got_ip = false;
    link_lost = false;
}

static void led_idle(void);

// Acknowledge a HALT or STOP that ended an activity
static void finish_interrupt(Interrupt why) {
    if(why == InterruptStop) {
        leave_network();
        out("STOPPED");
    } else if(why == InterruptHalt) {
        out("HALTED");
    }
    led_idle();
}

static void cmd_scan(void) {
    led(true, false, true);
    int n = WiFi.scanNetworks(false, true, false, SCAN_MS_PER_CHANNEL);
    for(int i = 0; i < n; i++) {
        char ssid[33];
        clean(WiFi.SSID(i).c_str(), ssid, sizeof(ssid));
        out("AP\t%s\t%s\t%d\t%d\t%s", ssid, WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i),
            auth_name(WiFi.encryptionType(i)));
    }
    out("SCAN_END\t%d", n < 0 ? 0 : n);
    WiFi.scanDelete();
    led_idle();
}

// --- walk test ---

static void watch_once(void) {
    // Stay on the access point's channel for speed; rescan all channels every 25th round
    // (or when it disappears) so a stronger access point elsewhere is picked up.
    bool full = watch_channel == 0 || (watch_round % 25) == 24;
    int n = WiFi.scanNetworks(false, true, false, SCAN_MS_PER_CHANNEL, full ? 0 : watch_channel, watch_ssid);
    int best = -1;
    for(int i = 0; i < n; i++)
        if(best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
    if(best >= 0) {
        watch_channel = (uint8_t)WiFi.channel(best);
        out("RSSI\t%d\t%d\t%s\t%d", (int)WiFi.RSSI(best), (int)watch_channel, WiFi.BSSIDstr(best).c_str(), n);
    } else {
        watch_channel = 0;
        out("RSSI_NONE");
    }
    WiFi.scanDelete();
    watch_round++;
}

// --- join check ---

static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
    if(event == ARDUINO_EVENT_WIFI_STA_CONNECTED) sta_associated = true;
    if(event == ARDUINO_EVENT_WIFI_STA_GOT_IP) sta_got_ip = true;
    if(event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        if(sta_got_ip) link_lost = true;
        sta_associated = sta_got_ip = false;
        last_disconnect_reason = info.wifi_sta_disconnected.reason;
    }
}

static const char* disconnect_text(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE: return "wrong password";
    case WIFI_REASON_AUTH_EXPIRE: return "authentication timed out";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY: return "access point refused";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
    case 0: return "timed out";
    default: return "could not connect";
    }
}

static volatile bool ping_finished;
static volatile uint32_t ping_replies, ping_total_ms;

static void ping_on_success(esp_ping_handle_t handle, void*) {
    uint32_t elapsed = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ping_replies++;
    ping_total_ms += elapsed;
}

static void ping_on_end(esp_ping_handle_t, void*) {
    ping_finished = true;
}

// Three pings; returns the average round trip in ms, or -1 if nothing came back
static int ping_host(IPAddress ip) {
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    IP_ADDR4(&config.target_addr, ip[0], ip[1], ip[2], ip[3]);
    config.count = 3;
    config.interval_ms = 200;
    config.timeout_ms = 1000;
    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = ping_on_success;
    callbacks.on_ping_end = ping_on_end;
    esp_ping_handle_t handle;
    ping_finished = false;
    ping_replies = ping_total_ms = 0;
    if(esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) return -1;
    esp_ping_start(handle);
    for(uint32_t waited = 0; !ping_finished && waited < 6000; waited += 20) delay(20);
    esp_ping_stop(handle);
    esp_ping_delete_session(handle);
    return ping_replies ? (int)(ping_total_ms / ping_replies) : -1;
}

static void cmd_join(char* args) {
    char* ssid = args;
    char* pass = strchr(args, '\t');
    if(pass) *pass++ = 0;
    if(!ssid[0]) {
        out("JOIN_FAIL\tno network name");
        out("JOIN_END");
        return;
    }

    led(true, true, false);
    leave_network();
    delay(100);
    link_lost = false;
    last_disconnect_reason = 0;
    uint32_t t0 = millis();
    WiFi.begin(ssid, (pass && pass[0]) ? pass : NULL);

    while(!sta_associated && millis() - t0 < JOIN_TIMEOUT_MS) {
        delay(50);
        // a definite refusal ends the wait early
        uint8_t r = last_disconnect_reason;
        if(r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT)
            break;
    }
    if(!sta_associated) {
        out("JOIN_FAIL\t%s", disconnect_text(last_disconnect_reason));
        out("JOIN_END");
        leave_network();
        led_idle();
        return;
    }
    uint32_t assoc_ms = millis() - t0;

    uint32_t t1 = millis();
    while(!sta_got_ip && sta_associated && millis() - t1 < DHCP_TIMEOUT_MS) delay(50);
    if(!sta_got_ip) {
        out("JOIN_FAIL\t%s", sta_associated ? "joined, but no DHCP answer" : disconnect_text(last_disconnect_reason));
        out("JOIN_END");
        leave_network();
        led_idle();
        return;
    }
    uint32_t dhcp_ms = millis() - t1;

    out("JOIN_OK\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%u\t%u", WiFi.localIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
        (int)WiFi.RSSI(), (int)WiFi.channel(), WiFi.BSSIDstr().c_str(), (unsigned)assoc_ms, (unsigned)dhcp_ms);

    out("PING\tgateway\t%d", ping_host(WiFi.gatewayIP()));

    IPAddress resolved;
    uint32_t t2 = millis();
    bool dns_ok = WiFi.hostByName("example.com", resolved) == 1;
    out("DNS\t%s\t%u", dns_ok ? "ok" : "fail", (unsigned)(millis() - t2));

    out("PING\tinternet\t%d", ping_host(IPAddress(1, 1, 1, 1)));
    out("JOIN_END");
    led_idle();
}

// --- discovery ---

struct Service {
    const char* label;
    const char* type;
    const char* proto;
};

static const Service services[] = {
    {"AirPlay", "_airplay", "_tcp"},
    {"AirPlay audio", "_raop", "_tcp"},
    {"Chromecast", "_googlecast", "_tcp"},
    {"Dante", "_netaudio-arc", "_udp"},
    {"NDI", "_ndi", "_tcp"},
    {"RTSP/AES67", "_rtsp", "_tcp"},
    {"Spotify", "_spotify-connect", "_tcp"},
    {"Sonos", "_sonos", "_tcp"},
    {"Apple remote", "_companion-link", "_tcp"},
    {"Android TV", "_androidtvremote2", "_tcp"},
    {"HomeKit", "_hap", "_tcp"},
    {"Web page", "_http", "_tcp"},
    {"Printer", "_ipp", "_tcp"},
    {"SSH", "_ssh", "_tcp"},
};

static void cmd_mdns(void) {
    if(WiFi.status() != WL_CONNECTED) {
        out("MDNS_FAIL\tnot connected");
        return;
    }
    if(!mdns_started) {
        if(mdns_init() != ESP_OK) {
            out("MDNS_FAIL\tcould not start");
            return;
        }
        mdns_started = true;
    }
    led(false, true, true);
    int total = 0;
    const int count = sizeof(services) / sizeof(services[0]);
    for(int i = 0; i < count; i++) {
        out("MDNS_STEP\t%d\t%d\t%s", i + 1, count, services[i].label);
        mdns_result_t* results = NULL;
        if(mdns_query_ptr(services[i].type, services[i].proto, 1500, 12, &results) == ESP_OK) {
            for(mdns_result_t* r = results; r; r = r->next) {
                char name[48], host[48], ip[16] = "-";
                clean(r->instance_name ? r->instance_name : "", name, sizeof(name));
                clean(r->hostname ? r->hostname : "", host, sizeof(host));
                for(mdns_ip_addr_t* a = r->addr; a; a = a->next) {
                    if(a->addr.type == ESP_IPADDR_TYPE_V4) {
                        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                        break;
                    }
                }
                out("SVC\t%s\t%s\t%s\t%s\t%u", services[i].label, name, host, ip, (unsigned)r->port);
                total++;
            }
            mdns_query_results_free(results);
        }
        Interrupt why = interrupt_requested();
        if(why != InterruptNone) {
            out("MDNS_END\t%d", total);
            finish_interrupt(why);
            return;
        }
    }
    out("MDNS_END\t%d", total);
    led_idle();
}


// --- device finder ---

// Company name for a hardware address, from the table in flash
static void maker_name(const uint8_t* mac, char* dst, size_t size) {
    if(mac[0] & 0x02) { // locally administered: phones and laptops randomise these
        strlcpy(dst, "Private address", size);
        return;
    }
    uint32_t key = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    int lo = 0, hi = OUI_COUNT - 1;
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t k = ((uint32_t)pgm_read_byte(oui_prefix + mid * 3) << 16) |
                     ((uint32_t)pgm_read_byte(oui_prefix + mid * 3 + 1) << 8) | pgm_read_byte(oui_prefix + mid * 3 + 2);
        if(k == key) {
            strlcpy(dst, oui_names + pgm_read_dword(oui_name_offset + mid), size);
            return;
        }
        if(k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    strlcpy(dst, "Unknown", size);
}

#define ARP_BATCH 8

struct ArpJob {
    struct netif* nif;
    ip4_addr_t ip[ARP_BATCH];
    int count;
    bool lookup; // false: send requests, true: read the answers
    bool found[ARP_BATCH];
    uint8_t mac[ARP_BATCH][6];
    SemaphoreHandle_t done;
};

// Runs inside the network stack's own task, where its tables may be touched
static void arp_job_run(void* arg) {
    ArpJob* job = (ArpJob*)arg;
    for(int i = 0; i < job->count; i++) {
        if(!job->lookup) {
            etharp_request(job->nif, &job->ip[i]);
        } else {
            struct eth_addr* eth = NULL;
            const ip4_addr_t* ip = NULL;
            job->found[i] = etharp_find_addr(job->nif, &job->ip[i], &eth, &ip) >= 0 && eth;
            if(job->found[i]) memcpy(job->mac[i], eth->addr, 6);
        }
    }
    xSemaphoreGive(job->done);
}

static bool arp_job(ArpJob* job, bool lookup) {
    job->lookup = lookup;
    if(tcpip_callback(arp_job_run, job) != ERR_OK) return false;
    return xSemaphoreTake(job->done, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void cmd_find(void) {
    if(WiFi.status() != WL_CONNECTED) {
        out("FIND_FAIL\tnot connected");
        return;
    }
    esp_netif_t* esp_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    struct netif* nif = esp_netif ? (struct netif*)esp_netif_get_netif_impl(esp_netif) : NULL;
    if(!nif) {
        out("FIND_FAIL\tno network interface");
        return;
    }
    uint32_t self = ntohl((uint32_t)WiFi.localIP());
    uint32_t mask = ntohl((uint32_t)WiFi.subnetMask());
    if((~mask) > 255) { // larger than 254 hosts: stay within our own /24
        mask = 0xFFFFFF00u;
        IPAddress base(htonl(self & mask));
        out("FIND_NOTE\tLarge network: only %s/24 swept", base.toString().c_str());
    }
    uint32_t first = (self & mask) + 1, last = (self | ~mask) - 1;
    int total = (int)(last - first + 1), done = 0, found = 0;

    static ArpJob job;
    job.nif = nif;
    if(!job.done) job.done = xSemaphoreCreateBinary();
    led(true, false, true);

    for(uint32_t addr = first; addr <= last;) {
        job.count = 0;
        while(job.count < ARP_BATCH && addr <= last) {
            if(addr != self) job.ip[job.count++].addr = htonl(addr);
            addr++;
            done++;
        }
        // ask twice: a device dozing in power save can miss the first request
        arp_job(&job, false);
        delay(150);
        arp_job(&job, false);
        delay(250);
        if(arp_job(&job, true)) {
            for(int i = 0; i < job.count; i++) {
                if(!job.found[i]) continue;
                char maker[24];
                const uint8_t* m = job.mac[i];
                maker_name(m, maker, sizeof(maker));
                out("HOST\t%s\t%02X:%02X:%02X:%02X:%02X:%02X\t%s", IPAddress(job.ip[i].addr).toString().c_str(), m[0], m[1],
                    m[2], m[3], m[4], m[5], maker);
                found++;
            }
        }
        out("FIND_STEP\t%d\t%d", done, total);
        Interrupt why = interrupt_requested();
        if(why != InterruptNone) {
            out("FIND_END\t%d", found);
            finish_interrupt(why);
            return;
        }
    }
    out("FIND_END\t%d", found);
    led_idle();
}

struct PortName {
    uint16_t port;
    const char* name;
};

static const PortName control_ports[] = {
    {22, "SSH"},           {23, "Telnet"},        {80, "Web page"},       {443, "Secure web"},
    {554, "RTSP video"},   {1319, "AMX"},         {1515, "Samsung MDC"},  {1710, "Q-SYS control"},
    {4352, "PJLink"},      {4998, "Global Cache"}, {5000, "AirPlay"},     {7000, "AirPlay"},
    {7142, "NEC display"}, {8008, "Chromecast"},  {8009, "Chromecast"},   {8080, "Web (alt)"},
    {9100, "Printer"},     {9761, "LG display"},  {10002, "Sharp display"}, {20060, "Sony display"},
    {41794, "Crestron CIP"}, {41795, "Crestron console"},
};

static void cmd_ports(const char* host) {
    IPAddress ip;
    if(WiFi.status() != WL_CONNECTED || !ip.fromString(host)) {
        out("PORTS_END\t0");
        return;
    }
    led(true, false, true);
    int open = 0;
    const int count = sizeof(control_ports) / sizeof(control_ports[0]);
    for(int i = 0; i < count; i++) {
        WiFiClient client;
        if(client.connect(ip, control_ports[i].port, 350)) {
            out("PORT\t%u\t%s", control_ports[i].port, control_ports[i].name);
            open++;
        }
        client.stop();
        Interrupt why = interrupt_requested();
        if(why != InterruptNone) {
            out("PORTS_END\t%d", open);
            finish_interrupt(why);
            return;
        }
    }
    out("PORTS_END\t%d", open);
    led_idle();
}

// --- stability logger ---

static volatile bool stab_event;
static volatile int stab_ms;
static volatile uint32_t stab_seq;

static void stab_on_success(esp_ping_handle_t handle, void*) {
    uint32_t elapsed = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    stab_ms = (int)elapsed;
    stab_seq++;
    stab_event = true;
}

static void stab_on_timeout(esp_ping_handle_t, void*) {
    stab_ms = -1;
    stab_seq++;
    stab_event = true;
}

static esp_ping_handle_t stab_start(void) {
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    IPAddress gw = WiFi.gatewayIP();
    IP_ADDR4(&config.target_addr, gw[0], gw[1], gw[2], gw[3]);
    config.count = ESP_PING_COUNT_INFINITE;
    config.interval_ms = 1000;
    config.timeout_ms = 1000;
    esp_ping_callbacks_t callbacks = {};
    callbacks.on_ping_success = stab_on_success;
    callbacks.on_ping_timeout = stab_on_timeout;
    esp_ping_handle_t handle = NULL;
    if(esp_ping_new_session(&config, &callbacks, &handle) != ESP_OK) return NULL;
    esp_ping_start(handle);
    return handle;
}

static void stab_end(esp_ping_handle_t handle) {
    if(!handle) return;
    esp_ping_stop(handle);
    esp_ping_delete_session(handle);
}

// Pings the gateway once a second until told to stop; rejoins by itself if the link drops
static void cmd_stab(void) {
    if(WiFi.status() != WL_CONNECTED) {
        out("STAB_FAIL\tnot connected");
        return;
    }
    stab_seq = 0;
    stab_event = false;
    esp_ping_handle_t handle = stab_start();
    uint32_t last_down_report = 0, last_reconnect = 0;
    bool down = false;
    led(false, true, true);

    while(true) {
        Interrupt why = interrupt_requested();
        if(why != InterruptNone) {
            stab_end(handle);
            link_lost = false;
            finish_interrupt(why);
            return;
        }
        bool connected = WiFi.status() == WL_CONNECTED;
        if(!connected) {
            if(!down) {
                down = true;
                stab_end(handle);
                handle = NULL;
            }
            uint32_t now = millis();
            if(now - last_down_report >= 1000) {
                last_down_report = now;
                out("STAB\t%u\t-2\t0\t-\t0", (unsigned)++stab_seq);
            }
            if(now - last_reconnect >= 3000) {
                last_reconnect = now;
                WiFi.reconnect();
            }
        } else {
            if(down) {
                down = false;
                link_lost = false;
                stab_event = false;
                handle = stab_start();
            }
            if(stab_event) {
                stab_event = false;
                out("STAB\t%u\t%d\t%d\t%s\t%d", (unsigned)stab_seq, stab_ms, (int)WiFi.RSSI(), WiFi.BSSIDstr().c_str(),
                    (int)WiFi.channel());
            }
        }
        delay(20);
    }
}

// --- control client ---

// Turn \r \n \t \\ and \xNN in a command file's text into the bytes they stand for
static size_t unescape(const char* src, uint8_t* dst, size_t size) {
    size_t n = 0;
    while(*src && n < size) {
        if(src[0] == '\\' && src[1]) {
            char c = src[1];
            if(c == 'x' && isxdigit((unsigned char)src[2]) && isxdigit((unsigned char)src[3])) {
                char hex[3] = {src[2], src[3], 0};
                dst[n++] = (uint8_t)strtol(hex, NULL, 16);
                src += 4;
                continue;
            }
            dst[n++] = c == 'r' ? '\r' : c == 'n' ? '\n' : c == 't' ? '\t' : (uint8_t)c;
            src += 2;
        } else {
            dst[n++] = (uint8_t)*src++;
        }
    }
    return n;
}

// Make a device's reply safe to show on one line
static void printable(const uint8_t* src, size_t len, char* dst, size_t size) {
    size_t n = 0;
    for(size_t i = 0; i < len && n + 5 < size; i++) {
        uint8_t c = src[i];
        if(c == '\r')
            n += snprintf(dst + n, size - n, "\\r");
        else if(c == '\n')
            n += snprintf(dst + n, size - n, "\\n");
        else if(c >= 32 && c < 127)
            dst[n++] = (char)c;
        else
            n += snprintf(dst + n, size - n, "\\x%02X", c);
    }
    dst[n] = 0;
}

// Split "host:port rest" in place
static bool split_endpoint(char* text, char** host, uint16_t* port, char** rest) {
    *host = text;
    char* space = strchr(text, ' ');
    *rest = (char*)"";
    if(space) {
        *space = 0;
        *rest = space + 1;
    }
    char* colon = strrchr(text, ':');
    if(!colon) return false;
    *colon = 0;
    int p = atoi(colon + 1);
    if(p < 1 || p > 65535) return false;
    *port = (uint16_t)p;
    return true;
}

// Read whatever the device sends back: up to `quiet_ms` of silence or `total_ms` overall
static size_t read_reply(WiFiClient& client, uint8_t* buf, size_t size, uint32_t quiet_ms, uint32_t total_ms) {
    size_t n = 0;
    uint32_t start = millis(), last = millis();
    while(n < size && millis() - start < total_ms && millis() - last < quiet_ms) {
        while(client.available() && n < size) {
            buf[n++] = (uint8_t)client.read();
            last = millis();
        }
        if(!client.connected() && !client.available()) break;
        delay(5);
    }
    return n;
}

static void ctl_tcp(char* rest) {
    char *host, *payload;
    uint16_t port;
    if(!split_endpoint(rest, &host, &port, &payload)) {
        out("CTL\tfail\tneeds host:port");
        return;
    }
    WiFiClient client;
    if(!client.connect(host, port, 2000)) {
        out("CTL\tfail\tcould not connect");
        return;
    }
    uint8_t data[256], reply[REPLY_MAX];
    size_t len = unescape(payload, data, sizeof(data));
    if(len) client.write(data, len);
    size_t got = read_reply(client, reply, sizeof(reply), 700, 2500);
    client.stop();
    char text[REPLY_MAX * 2];
    printable(reply, got, text, sizeof(text));
    out("CTL\tok\t%s", got ? text : "sent (no reply)");
}

static void ctl_udp(char* rest) {
    char *host, *payload;
    uint16_t port;
    IPAddress ip;
    if(!split_endpoint(rest, &host, &port, &payload) || !WiFi.hostByName(host, ip)) {
        out("CTL\tfail\tneeds host:port");
        return;
    }
    uint8_t data[256], reply[REPLY_MAX];
    size_t len = unescape(payload, data, sizeof(data));
    WiFiUDP udp;
    udp.begin(0);
    udp.beginPacket(ip, port);
    udp.write(data, len);
    bool sent = udp.endPacket() == 1;
    size_t got = 0;
    for(uint32_t t0 = millis(); sent && millis() - t0 < 700 && !got;) {
        if(udp.parsePacket() > 0) got = (size_t)udp.read(reply, sizeof(reply));
        delay(10);
    }
    udp.stop();
    char text[REPLY_MAX * 2];
    printable(reply, got, text, sizeof(text));
    if(!sent)
        out("CTL\tfail\tcould not send");
    else
        out("CTL\tok\t%s", got ? text : "sent (no reply)");
}

static void ctl_wol(char* rest) {
    unsigned m[6];
    if(sscanf(rest, "%x%*[:-]%x%*[:-]%x%*[:-]%x%*[:-]%x%*[:-]%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        out("CTL\tfail\tneeds a MAC address");
        return;
    }
    uint8_t packet[102];
    memset(packet, 0xFF, 6);
    for(int i = 0; i < 16; i++)
        for(int k = 0; k < 6; k++) packet[6 + i * 6 + k] = (uint8_t)m[k];
    IPAddress local = WiFi.localIP(), mask = WiFi.subnetMask(), subnet_broadcast;
    for(int i = 0; i < 4; i++) subnet_broadcast[i] = local[i] | (uint8_t)~mask[i];
    WiFiUDP udp;
    udp.begin(0);
    bool sent = false;
    for(int i = 0; i < 3; i++) {
        udp.beginPacket(IPAddress(255, 255, 255, 255), 9);
        udp.write(packet, sizeof(packet));
        sent = (udp.endPacket() == 1) || sent;
        udp.beginPacket(subnet_broadcast, 9);
        udp.write(packet, sizeof(packet));
        sent = (udp.endPacket() == 1) || sent;
        delay(50);
    }
    udp.stop();
    out(sent ? "CTL\tok\twake packet sent" : "CTL\tfail\tcould not send");
}

static void ctl_http(char* rest) {
    char* url = strchr(rest, ' ');
    if(!url) {
        out("CTL\tfail\tneeds GET or POST and a URL");
        return;
    }
    *url++ = 0;
    char* body = strchr(url, ' ');
    if(body) *body++ = 0;
    bool post = strcasecmp(rest, "POST") == 0;
    bool secure = strncasecmp(url, "https://", 8) == 0;

    WiFiClient plain;
    WiFiClientSecure tls;
    tls.setInsecure(); // AV devices almost always present self-signed certificates
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(4000);
    bool begun = secure ? http.begin(tls, url) : http.begin(plain, url);
    if(!begun) {
        out("CTL\tfail\tbad URL");
        return;
    }
    int code;
    if(post) {
        uint8_t data[256];
        size_t len = unescape(body ? body : "", data, sizeof(data));
        http.addHeader("Content-Type", (len && data[0] == '{') ? "application/json" : "text/plain");
        code = http.POST(data, len);
    } else {
        code = http.GET();
    }
    if(code <= 0) {
        out("CTL\tfail\t%s", http.errorToString(code).c_str());
    } else {
        String text = http.getString();
        char shown[REPLY_MAX * 2];
        printable((const uint8_t*)text.c_str(), text.length() > 100 ? 100 : text.length(), shown, sizeof(shown));
        out("CTL\t%s\tHTTP %d %s", code < 400 ? "ok" : "fail", code, shown);
    }
    http.end();
}

// PJLink class 1: "host [password=secret] COMMAND [value]", for example "192.168.1.50 POWR 1"
static void ctl_pjlink(char* rest) {
    char* host = rest;
    char* command = strchr(rest, ' ');
    if(!command) {
        out("CTL\tfail\tneeds host and command");
        return;
    }
    *command++ = 0;
    const char* password = "";
    if(strncmp(command, "password=", 9) == 0) {
        password = command + 9;
        command = strchr(command, ' ');
        if(!command) {
            out("CTL\tfail\tneeds a command");
            return;
        }
        *command++ = 0;
    }
    WiFiClient client;
    if(!client.connect(host, 4352, 2000)) {
        out("CTL\tfail\tcould not connect");
        return;
    }
    uint8_t greeting[64];
    size_t got = read_reply(client, greeting, sizeof(greeting) - 1, 400, 2000);
    greeting[got] = 0;
    char prefix[40] = "";
    if(strncmp((char*)greeting, "PJLINK 1 ", 9) == 0) {
        // authentication: MD5 of the projector's random number followed by the password
        char seed[80];
        char* random = (char*)greeting + 9;
        random[strcspn(random, "\r\n")] = 0;
        snprintf(seed, sizeof(seed), "%s%s", random, password);
        uint8_t digest[16];
        mbedtls_md5_ret((const uint8_t*)seed, strlen(seed), digest);
        for(int i = 0; i < 16; i++) snprintf(prefix + i * 2, 3, "%02x", digest[i]);
    } else if(strncmp((char*)greeting, "PJLINK 0", 8) != 0) {
        client.stop();
        out("CTL\tfail\tnot a PJLink device");
        return;
    }
    client.printf("%s%%1%s\r", prefix, command);
    uint8_t reply[REPLY_MAX];
    got = read_reply(client, reply, sizeof(reply), 700, 3000);
    client.stop();
    char text[REPLY_MAX * 2];
    printable(reply, got, text, sizeof(text));
    if(!got)
        out("CTL\tfail\tno reply");
    else if(strstr(text, "ERRA"))
        out("CTL\tfail\twrong PJLink password");
    else
        out("CTL\t%s\t%s", strstr(text, "=ERR") ? "fail" : "ok", text);
}

static void cmd_ctl(char* args) {
    char* rest = strchr(args, '\t');
    if(WiFi.status() != WL_CONNECTED) {
        out("CTL\tfail\tnot connected");
        return;
    }
    if(!rest) {
        out("CTL\tfail\tincomplete command");
        return;
    }
    *rest++ = 0;
    led(true, true, false);
    if(strcmp(args, "tcp") == 0)
        ctl_tcp(rest);
    else if(strcmp(args, "udp") == 0)
        ctl_udp(rest);
    else if(strcmp(args, "wol") == 0)
        ctl_wol(rest);
    else if(strcmp(args, "http") == 0)
        ctl_http(rest);
    else if(strcmp(args, "pjlink") == 0)
        ctl_pjlink(rest);
    else
        out("CTL\tfail\tunknown type");
    led_idle();
}

// --- clock ---

static void cmd_time(void) {
    if(WiFi.status() != WL_CONNECTED) {
        out("TIME_FAIL\tnot connected");
        return;
    }
    static char gateway[16];
    strlcpy(gateway, WiFi.gatewayIP().toString().c_str(), sizeof(gateway));
    struct timeval zero = {0, 0};
    settimeofday(&zero, NULL); // forget any earlier answer
    // two public time servers, then the gateway in case the network has no internet
    configTime(0, 0, "pool.ntp.org", "time.google.com", gateway);
    for(uint32_t t0 = millis(); millis() - t0 < 10000;) {
        time_t now = time(NULL);
        if(now > 1700000000) {
            out("TIME\t%lu", (unsigned long)now);
            return;
        }
        delay(50);
    }
    out("TIME_FAIL\tno time server answered");
}

// --- main ---

static void handle_command(void) {
    char* args = strchr(line, '\t');
    if(args) *args++ = 0;

    if(strcmp(line, "VER") == 0) {
        out("VER\t%d", FW_VERSION);
    } else if(strcmp(line, "STOP") == 0) {
        mode = ModeIdle;
        finish_interrupt(InterruptStop);
    } else if(strcmp(line, "HALT") == 0) {
        mode = ModeIdle;
        finish_interrupt(InterruptHalt);
    } else if(strcmp(line, "FIND") == 0) {
        mode = ModeIdle;
        cmd_find();
    } else if(strcmp(line, "PORTS") == 0 && args) {
        mode = ModeIdle;
        cmd_ports(args);
    } else if(strcmp(line, "STAB") == 0) {
        mode = ModeIdle;
        cmd_stab();
    } else if(strcmp(line, "CTL") == 0 && args) {
        mode = ModeIdle;
        cmd_ctl(args);
    } else if(strcmp(line, "TIME") == 0) {
        mode = ModeIdle;
        cmd_time();
    } else if(strcmp(line, "SCAN") == 0) {
        mode = ModeIdle;
        cmd_scan();
    } else if(strcmp(line, "WATCH") == 0 && args) {
        leave_network();
        strncpy(watch_ssid, args, sizeof(watch_ssid) - 1);
        watch_ssid[sizeof(watch_ssid) - 1] = 0;
        watch_channel = 0;
        watch_round = 0;
        mode = ModeWatch;
        led(true, false, true);
    } else if(strcmp(line, "JOIN") == 0 && args) {
        mode = ModeIdle;
        cmd_join(args);
    } else if(strcmp(line, "MDNS") == 0) {
        mode = ModeIdle;
        cmd_mdns();
    } else {
        out("ERR\tunknown command");
    }
}

void setup() {
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    led(true, false, false);

    Serial.setRxBufferSize(1024);
    Serial.begin(115200);
    WiFi.persistent(false); // never store network passwords in the chip's flash
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(on_wifi_event);
    WiFi.disconnect(false, true);

    led_idle();
    Serial.print('\n'); // end whatever the boot loader left on the line
    out("READY\t%d", FW_VERSION);
}

void loop() {
    if(read_line()) handle_command();
    if(link_lost) {
        link_lost = false;
        out("LINK\tdown");
        led_idle();
    }
    if(mode == ModeWatch)
        watch_once();
    else
        delay(5);
}
