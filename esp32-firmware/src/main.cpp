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
//   STOP                     -> stops WATCH or MDNS, leaves the network: $STOPPED
#include <Arduino.h>
#include <WiFi.h>
#include <mdns.h>
#include <stdarg.h>
#include "ping/ping_sock.h"

#define FW_VERSION 1
#define LED_RED 6
#define LED_GREEN 5
#define LED_BLUE 4
#define CMD_LINE_MAX 200
#define SCAN_MS_PER_CHANNEL 120
#define JOIN_TIMEOUT_MS 15000
#define DHCP_TIMEOUT_MS 12000

enum Mode { ModeIdle, ModeWatch };

static Mode mode = ModeIdle;
static char watch_ssid[33];
static uint8_t watch_channel;
static uint32_t watch_round;

static volatile bool sta_associated, sta_got_ip;
static volatile uint8_t last_disconnect_reason;
static bool mdns_started;

static char line[CMD_LINE_MAX];
static size_t line_len;

// --- output ---

static void out(const char* fmt, ...) {
    char buf[256];
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

// True if a STOP arrived; any other command received meanwhile is dropped
static bool stop_requested(void) {
    while(read_line())
        if(strcmp(line, "STOP") == 0) return true;
    return false;
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
        if(stop_requested()) {
            out("MDNS_END\t%d", total);
            leave_network();
            out("STOPPED");
            led_idle();
            return;
        }
    }
    out("MDNS_END\t%d", total);
    led_idle();
}

// --- main ---

static void handle_command(void) {
    char* args = strchr(line, '\t');
    if(args) *args++ = 0;

    if(strcmp(line, "VER") == 0) {
        out("VER\t%d", FW_VERSION);
    } else if(strcmp(line, "STOP") == 0) {
        mode = ModeIdle;
        leave_network();
        out("STOPPED");
        led_idle();
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

    Serial.setRxBufferSize(512);
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
    if(mode == ModeWatch)
        watch_once();
    else
        delay(5);
}
