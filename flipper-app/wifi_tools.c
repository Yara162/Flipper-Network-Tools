// Wi-Fi Tools: Flipper Zero front end for the Wi-Fi utility firmware on the Wi-Fi Devboard.
// Survey, signal walk test, join check, discovery, device finder, stability logger,
// control client and clock sync.
//
// The devboard is driven with text lines over the USART (pins 13/14) at 115200 baud; see
// esp32-firmware/src/main.cpp for the protocol.
#include <furi.h>
#include <furi_hal.h>
#include <stdarg.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <input/input.h>
#include <storage/storage.h>
#include <expansion/expansion.h>
#include <datetime/datetime.h>

#define BAUD 115200
#define TICK_MS 50
#define MAX_APS 32
#define MAX_SERVICES 40
#define MAX_HOSTS 96
#define MAX_PORTS 24
#define MAX_COMMANDS 24
#define MAX_EVENTS 40
#define HISTORY 120
#define LINE_SIZE 400
#define PASSWORD_SIZE 64
#define PASSWORD_FILE STORAGE_APP_DATA_PATH_PREFIX "/passwords.txt"
#define COMMANDS_FILE STORAGE_APP_DATA_PATH_PREFIX "/commands.txt"
#define TEXT_FILE_MAX 4096
#define LIST_ROWS 5
#define REPORT_SIZE 12288

typedef enum { ViewMenu, ViewScreen, ViewPassword } ViewId;

typedef enum {
    MenuSurvey,
    MenuWalk,
    MenuJoin,
    MenuDiscovery,
    MenuFinder,
    MenuStability,
    MenuControl,
    MenuClock,
    MenuLeave,
} MenuItem;

// What the network list is being shown for
typedef enum { IntentSurvey, IntentWalk, IntentJoin } Intent;

// What to run once a network has been joined
typedef enum { ToolNone, ToolDiscovery, ToolFinder, ToolStability, ToolControl, ToolClock } Tool;

typedef enum {
    ScreenMessage,
    ScreenScanning,
    ScreenSurvey,
    ScreenChannels,
    ScreenWalk,
    ScreenJoin,
    ScreenDiscovery,
    ScreenFinder,
    ScreenPorts,
    ScreenStability,
    ScreenControl,
    ScreenControlResult,
    ScreenClock,
} Screen;

typedef enum { SaveNone, SaveDiscovery, SaveDevices, SaveStability } SaveKind;

typedef struct {
    char ssid[33];
    char bssid[18];
    char auth[8];
    uint8_t channel;
    int8_t rssi;
} AccessPoint;

typedef struct {
    char label[14];
    char name[48];
    char host[40];
    char ip[16];
    uint16_t port;
} ServiceEntry;

typedef struct {
    char ip[16];
    char mac[18];
    char maker[22];
    char ports[48]; // open control ports, once checked
    bool ports_checked;
} Host;

typedef struct {
    uint16_t number;
    char name[18];
} OpenPort;

typedef struct {
    char name[26];
    char type[8];
    char rest[160];
} Command;

typedef struct {
    uint32_t at_s;
    char text[40];
} StabEvent;

typedef enum { JoinConnecting, JoinOk, JoinFailed } JoinState;
typedef enum { ClockAsking, ClockReady, ClockFailed, ClockSet } ClockState;
typedef enum { ResultPending, ResultOk, ResultFailed } ResultState;

typedef struct {
    Screen screen;
    Intent intent;
    Tool next_tool;
    const char* message[4];
    char message_buf[2][40];
    bool board_ok;
    bool connected;
    char connected_ssid[33];

    AccessPoint aps[MAX_APS];
    size_t ap_count, ap_cursor;

    char target[33]; // network the current activity is about

    // walk test
    bool walk_seen, walk_lost;
    int16_t walk_rssi, walk_min, walk_max;
    int32_t walk_sum;
    uint32_t walk_samples;
    uint8_t walk_channel, walk_ap_count;
    char walk_bssid[18];

    // join check
    JoinState join_state;
    bool join_done;
    char join_fail[40];
    char ip[16], mask[16], gateway[16], dns[16];
    int16_t join_rssi;
    uint8_t join_channel;
    uint32_t assoc_ms, dhcp_ms, dns_ms;
    int16_t ping_gateway, ping_internet; // ms, -1 no reply, -2 not tested yet
    int8_t dns_ok; // 1 ok, 0 failed, -1 not tested yet

    // discovery
    ServiceEntry services[MAX_SERVICES];
    size_t service_count, service_cursor;
    bool mdns_done, mdns_saved, mdns_save_failed;
    uint8_t mdns_step, mdns_steps;
    char mdns_label[14];

    // device finder
    Host hosts[MAX_HOSTS];
    size_t host_count, host_cursor;
    bool find_done, find_saved;
    uint16_t find_step, find_total;
    char find_note[40];
    char devices_name[64];
    OpenPort ports[MAX_PORTS];
    size_t port_count;
    bool ports_done;

    // stability logger
    int16_t history[HISTORY]; // ms, -1 lost, -2 link down
    size_t history_count, history_head;
    uint32_t stab_start_tick;
    DateTime stab_started;
    uint32_t stab_sent, stab_lost, stab_down_s, stab_replies;
    int16_t stab_last, stab_min, stab_max, stab_rssi, stab_rssi_min;
    int32_t stab_sum, stab_jitter_sum;
    uint32_t stab_roams, stab_drops, stab_burst, stab_down_run;
    char stab_bssid[18];
    uint8_t stab_channel;
    StabEvent events[MAX_EVENTS];
    size_t event_count;

    // control client
    Command commands[MAX_COMMANDS];
    size_t command_count, command_cursor;
    ResultState result_state;
    char result_text[200];

    // clock sync
    ClockState clock_state;
    uint32_t utc, utc_tick;
    int16_t zone_minutes;
    bool zone_guessed;
    char clock_fail[40];

    // report files
    SaveKind save_request;
    char report_name[64];
} Model;

typedef struct {
    Gui* gui;
    ViewDispatcher* dispatcher;
    Submenu* menu;
    TextInput* text_input;
    View* view;
    Expansion* expansion;
    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx;
    char line[LINE_SIZE];
    size_t line_len;
    char password[PASSWORD_SIZE];
    char join_ssid[33];
    char header[48];
    uint32_t ticks;
} App;

// --- devboard link ---

static void rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* context) {
    App* app = context;
    if(event & FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx, &byte, 1, 0);
    }
}

static void send_command(App* app, const char* command, const char* a, const char* b) {
    char buf[LINE_SIZE];
    if(a && b)
        snprintf(buf, sizeof(buf), "%s\t%s\t%s\n", command, a, b);
    else if(a)
        snprintf(buf, sizeof(buf), "%s\t%s\n", command, a);
    else
        snprintf(buf, sizeof(buf), "%s\n", command);
    if(app->serial) furi_hal_serial_tx(app->serial, (const uint8_t*)buf, strlen(buf));
}

// Split a reply at its tabs; returns the number of fields
static size_t split(char* text, char** fields, size_t max) {
    size_t n = 0;
    while(n < max) {
        fields[n++] = text;
        char* tab = strchr(text, '\t');
        if(!tab) break;
        *tab = 0;
        text = tab + 1;
    }
    return n;
}

static void copy(char* dst, size_t size, const char* src) {
    strlcpy(dst, src, size);
}

static void show_message(Model* m, const char* a, const char* b, const char* c, const char* d) {
    m->screen = ScreenMessage;
    m->message[0] = a;
    m->message[1] = b;
    m->message[2] = c;
    m->message[3] = d;
}

static void sort_access_points(Model* m) {
    for(size_t i = 1; i < m->ap_count; i++) {
        AccessPoint ap = m->aps[i];
        size_t j = i;
        while(j > 0 && m->aps[j - 1].rssi < ap.rssi) {
            m->aps[j] = m->aps[j - 1];
            j--;
        }
        m->aps[j] = ap;
    }
}

// --- stability bookkeeping ---

static void stab_event(Model* m, const char* fmt, ...) {
    if(m->event_count >= MAX_EVENTS) return;
    StabEvent* e = &m->events[m->event_count++];
    e->at_s = (furi_get_tick() - m->stab_start_tick) / furi_kernel_get_tick_frequency();
    va_list args;
    va_start(args, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, args);
    va_end(args);
}

// Close any run of lost pings or link-down seconds into one log entry
static void stab_close_runs(Model* m) {
    if(m->stab_burst) stab_event(m, "%lu ping%s lost", (unsigned long)m->stab_burst, m->stab_burst == 1 ? "" : "s");
    if(m->stab_down_run) stab_event(m, "link down for %lu s", (unsigned long)m->stab_down_run);
    m->stab_burst = m->stab_down_run = 0;
}

static void stab_sample(Model* m, int ms, int rssi, const char* bssid, int channel) {
    m->history[(m->history_head + m->history_count) % HISTORY] = (int16_t)ms;
    if(m->history_count < HISTORY)
        m->history_count++;
    else
        m->history_head = (m->history_head + 1) % HISTORY;

    if(ms == -2) {
        if(m->stab_down_run == 0) {
            if(m->stab_burst) stab_close_runs(m);
            m->stab_drops++;
        }
        m->stab_down_run++;
        m->stab_down_s++;
        return;
    }
    if(m->stab_down_run) stab_close_runs(m);
    m->stab_sent++;
    m->stab_rssi = (int16_t)rssi;
    if(rssi < m->stab_rssi_min || m->stab_rssi_min == 0) m->stab_rssi_min = (int16_t)rssi;
    m->stab_channel = (uint8_t)channel;
    if(m->stab_bssid[0] && strcmp(m->stab_bssid, bssid) != 0) {
        m->stab_roams++;
        stab_event(m, "roam to %s %ddBm", strlen(bssid) >= 17 ? bssid + 9 : bssid, rssi);
    }
    copy(m->stab_bssid, sizeof(m->stab_bssid), bssid);

    if(ms < 0) {
        m->stab_lost++;
        m->stab_burst++;
        return;
    }
    if(m->stab_burst) stab_close_runs(m);
    if(m->stab_replies) {
        int diff = ms - m->stab_last;
        m->stab_jitter_sum += diff < 0 ? -diff : diff;
    }
    if(!m->stab_replies || ms < m->stab_min) m->stab_min = (int16_t)ms;
    if(!m->stab_replies || ms > m->stab_max) m->stab_max = (int16_t)ms;
    m->stab_sum += ms;
    m->stab_last = (int16_t)ms;
    m->stab_replies++;
}

// --- clock ---

// Guess the time zone from how far the Flipper's clock is from network time
static void clock_received(Model* m, uint32_t utc) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    int32_t diff = (int32_t)datetime_datetime_to_timestamp(&now) - (int32_t)utc;
    m->utc = utc;
    m->utc_tick = furi_get_tick();
    m->zone_guessed = diff > -13 * 3600 && diff < 15 * 3600;
    if(m->zone_guessed) {
        int32_t quarters = (diff + (diff >= 0 ? 450 : -450)) / 900;
        m->zone_minutes = (int16_t)(quarters * 15);
    } else {
        m->zone_minutes = 0;
    }
    m->clock_state = ClockReady;
}

static uint32_t clock_utc_now(const Model* m) {
    return m->utc + (furi_get_tick() - m->utc_tick) / furi_kernel_get_tick_frequency();
}

// --- replies from the devboard ---

static void start_tool(App* app, Model* m, Tool tool);

static void handle_reply(App* app, Model* m, char* text) {
    char* f[10];
    size_t n = split(text, f, COUNT_OF(f));
    const char* cmd = f[0];

    if(strcmp(cmd, "VER") == 0 || strcmp(cmd, "READY") == 0) {
        m->board_ok = true;
        if(strcmp(cmd, "READY") == 0) m->connected = false; // the devboard restarted
    } else if(strcmp(cmd, "AP") == 0 && n >= 6) {
        if(m->ap_count < MAX_APS) {
            AccessPoint* ap = &m->aps[m->ap_count++];
            copy(ap->ssid, sizeof(ap->ssid), f[1]);
            copy(ap->bssid, sizeof(ap->bssid), f[2]);
            ap->channel = (uint8_t)atoi(f[3]);
            ap->rssi = (int8_t)atoi(f[4]);
            copy(ap->auth, sizeof(ap->auth), f[5]);
        }
    } else if(strcmp(cmd, "SCAN_END") == 0) {
        sort_access_points(m);
        m->ap_cursor = 0;
        if(m->screen == ScreenScanning) m->screen = ScreenSurvey;
    } else if(strcmp(cmd, "RSSI") == 0 && n >= 5) {
        int16_t rssi = (int16_t)atoi(f[1]);
        m->walk_rssi = rssi;
        m->walk_channel = (uint8_t)atoi(f[2]);
        copy(m->walk_bssid, sizeof(m->walk_bssid), f[3]);
        m->walk_ap_count = (uint8_t)atoi(f[4]);
        if(!m->walk_seen || rssi < m->walk_min) m->walk_min = rssi;
        if(!m->walk_seen || rssi > m->walk_max) m->walk_max = rssi;
        m->walk_sum += rssi;
        m->walk_samples++;
        m->walk_seen = true;
        m->walk_lost = false;
    } else if(strcmp(cmd, "RSSI_NONE") == 0) {
        m->walk_lost = true;
    } else if(strcmp(cmd, "JOIN_OK") == 0 && n >= 10) {
        m->join_state = JoinOk;
        m->connected = true;
        copy(m->connected_ssid, sizeof(m->connected_ssid), m->target);
        copy(m->ip, sizeof(m->ip), f[1]);
        copy(m->mask, sizeof(m->mask), f[2]);
        copy(m->gateway, sizeof(m->gateway), f[3]);
        copy(m->dns, sizeof(m->dns), f[4]);
        m->join_rssi = (int16_t)atoi(f[5]);
        m->join_channel = (uint8_t)atoi(f[6]);
        m->assoc_ms = (uint32_t)atoi(f[8]);
        m->dhcp_ms = (uint32_t)atoi(f[9]);
    } else if(strcmp(cmd, "JOIN_FAIL") == 0 && n >= 2) {
        m->join_state = JoinFailed;
        m->connected = false;
        copy(m->join_fail, sizeof(m->join_fail), f[1]);
    } else if(strcmp(cmd, "PING") == 0 && n >= 3) {
        if(strcmp(f[1], "gateway") == 0) m->ping_gateway = (int16_t)atoi(f[2]);
        if(strcmp(f[1], "internet") == 0) m->ping_internet = (int16_t)atoi(f[2]);
    } else if(strcmp(cmd, "DNS") == 0 && n >= 3) {
        m->dns_ok = strcmp(f[1], "ok") == 0 ? 1 : 0;
        m->dns_ms = (uint32_t)atoi(f[2]);
    } else if(strcmp(cmd, "JOIN_END") == 0) {
        m->join_done = true;
        if(m->join_state == JoinOk && m->next_tool != ToolNone && m->screen == ScreenJoin) {
            Tool tool = m->next_tool;
            m->next_tool = ToolNone;
            start_tool(app, m, tool);
        }
    } else if(strcmp(cmd, "LINK") == 0) {
        m->connected = false;
        if(m->screen == ScreenDiscovery || m->screen == ScreenFinder || m->screen == ScreenPorts ||
           m->screen == ScreenControl || m->screen == ScreenControlResult || m->screen == ScreenClock ||
           m->screen == ScreenJoin)
            show_message(m, "The network connection", "dropped.", "", "Join it again from the menu.");
    } else if(strcmp(cmd, "MDNS_STEP") == 0 && n >= 4) {
        m->mdns_step = (uint8_t)atoi(f[1]);
        m->mdns_steps = (uint8_t)atoi(f[2]);
        copy(m->mdns_label, sizeof(m->mdns_label), f[3]);
    } else if(strcmp(cmd, "SVC") == 0 && n >= 6) {
        if(m->service_count < MAX_SERVICES) {
            ServiceEntry* s = &m->services[m->service_count++];
            copy(s->label, sizeof(s->label), f[1]);
            copy(s->name, sizeof(s->name), f[2]);
            copy(s->host, sizeof(s->host), f[3]);
            copy(s->ip, sizeof(s->ip), f[4]);
            s->port = (uint16_t)atoi(f[5]);
        }
    } else if(strcmp(cmd, "MDNS_END") == 0) {
        m->mdns_done = true;
        // a search cut short by Back is not worth keeping
        if(m->screen == ScreenDiscovery) m->save_request = SaveDiscovery;
    } else if(strcmp(cmd, "MDNS_FAIL") == 0) {
        m->mdns_done = true;
    } else if(strcmp(cmd, "HOST") == 0 && n >= 4) {
        if(m->host_count < MAX_HOSTS) {
            Host* h = &m->hosts[m->host_count++];
            memset(h, 0, sizeof(*h));
            copy(h->ip, sizeof(h->ip), f[1]);
            copy(h->mac, sizeof(h->mac), f[2]);
            copy(h->maker, sizeof(h->maker), f[3]);
        }
    } else if(strcmp(cmd, "FIND_STEP") == 0 && n >= 3) {
        m->find_step = (uint16_t)atoi(f[1]);
        m->find_total = (uint16_t)atoi(f[2]);
    } else if(strcmp(cmd, "FIND_NOTE") == 0 && n >= 2) {
        copy(m->find_note, sizeof(m->find_note), f[1]);
    } else if(strcmp(cmd, "FIND_END") == 0 || strcmp(cmd, "FIND_FAIL") == 0) {
        m->find_done = true;
        if(m->screen == ScreenFinder && m->host_count) m->save_request = SaveDevices;
    } else if(strcmp(cmd, "PORT") == 0 && n >= 3) {
        if(m->port_count < MAX_PORTS) {
            OpenPort* p = &m->ports[m->port_count++];
            p->number = (uint16_t)atoi(f[1]);
            copy(p->name, sizeof(p->name), f[2]);
        }
        if(m->host_cursor < m->host_count) {
            Host* h = &m->hosts[m->host_cursor];
            size_t len = strlen(h->ports);
            snprintf(h->ports + len, sizeof(h->ports) - len, "%s%s", len ? " " : "", f[1]);
        }
    } else if(strcmp(cmd, "PORTS_END") == 0) {
        m->ports_done = true;
        if(m->screen == ScreenPorts && m->host_cursor < m->host_count) {
            m->hosts[m->host_cursor].ports_checked = true;
            m->save_request = SaveDevices;
        }
    } else if(strcmp(cmd, "STAB") == 0 && n >= 6) {
        if(m->screen == ScreenStability) stab_sample(m, atoi(f[2]), atoi(f[3]), f[4], atoi(f[5]));
    } else if(strcmp(cmd, "CTL") == 0 && n >= 3) {
        m->result_state = strcmp(f[1], "ok") == 0 ? ResultOk : ResultFailed;
        copy(m->result_text, sizeof(m->result_text), f[2]);
    } else if(strcmp(cmd, "TIME") == 0 && n >= 2) {
        clock_received(m, (uint32_t)strtoul(f[1], NULL, 10));
    } else if(strcmp(cmd, "TIME_FAIL") == 0) {
        m->clock_state = ClockFailed;
        copy(m->clock_fail, sizeof(m->clock_fail), n >= 2 ? f[1] : "no answer");
    }
}

// Collect received bytes into lines; only lines starting with '$' are replies
static void process_rx(App* app, Model* m) {
    uint8_t chunk[64];
    size_t got;
    while((got = furi_stream_buffer_receive(app->rx, chunk, sizeof(chunk), 0)) > 0) {
        for(size_t i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if(c == '\n' || c == '\r') {
                app->line[app->line_len] = 0;
                if(app->line_len > 1 && app->line[0] == '$') handle_reply(app, m, app->line + 1);
                app->line_len = 0;
            } else if(app->line_len < LINE_SIZE - 1) {
                app->line[app->line_len++] = c;
            } else {
                app->line_len = 0;
            }
        }
    }
}

// --- files on the SD card, all in apps_data/wifi_tools/ ---

static size_t file_read(const char* app_path, char* buf, size_t size) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(app_path);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    size_t len = 0;
    if(storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING))
        len = storage_file_read(file, buf, size - 1);
    buf[len] = 0;
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return len;
}

static bool file_write(const char* app_path, const char* buf, size_t len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(app_path);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
              storage_file_write(file, buf, len) == len;
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// passwords.txt holds pairs of lines: a network name, then its password
static void password_load(const char* ssid, char* out, size_t size) {
    char* buf = malloc(TEXT_FILE_MAX);
    out[0] = 0;
    file_read(PASSWORD_FILE, buf, TEXT_FILE_MAX);
    char* p = buf;
    while(*p) {
        char* name = p;
        char* end = strchr(p, '\n');
        if(!end) break;
        *end = 0;
        char* pass = end + 1;
        char* pass_end = strchr(pass, '\n');
        if(pass_end) *pass_end = 0;
        size_t nl = strlen(name);
        if(nl && name[nl - 1] == '\r') name[nl - 1] = 0;
        size_t pl = strlen(pass);
        if(pl && pass[pl - 1] == '\r') pass[pl - 1] = 0;
        if(strcmp(name, ssid) == 0) {
            strlcpy(out, pass, size);
            break;
        }
        if(!pass_end) break;
        p = pass_end + 1;
    }
    free(buf);
}

static void password_save(const char* ssid, const char* password) {
    char* old = malloc(TEXT_FILE_MAX);
    char* fresh = malloc(TEXT_FILE_MAX + 128);
    size_t n = 0;
    file_read(PASSWORD_FILE, old, TEXT_FILE_MAX);
    // copy every other network's pair, then append this one
    char* p = old;
    while(*p) {
        char* end = strchr(p, '\n');
        if(!end) break;
        char* pass_end = strchr(end + 1, '\n');
        size_t pair_len = pass_end ? (size_t)(pass_end + 1 - p) : strlen(p);
        size_t name_len = (size_t)(end - p);
        if(name_len && p[name_len - 1] == '\r') name_len--;
        bool same = strlen(ssid) == name_len && strncmp(p, ssid, name_len) == 0;
        if(!same && n + pair_len < TEXT_FILE_MAX) {
            memcpy(fresh + n, p, pair_len);
            n += pair_len;
            if(!pass_end) fresh[n++] = '\n';
        }
        if(!pass_end) break;
        p = pass_end + 1;
    }
    n += (size_t)snprintf(fresh + n, 128, "%s\n%s\n", ssid, password);
    file_write(PASSWORD_FILE, fresh, n);
    free(old);
    free(fresh);
}

static const char commands_sample[] =
    "# Wi-Fi Tools commands, one per line:   Name = type details\n"
    "#\n"
    "#   pjlink <address> [password=<secret>] <command>    for example POWR 1, POWR 0, INPT 31\n"
    "#   tcp <address>:<port> <text>       \\r \\n and \\xNN stand for special bytes\n"
    "#   udp <address>:<port> <text>\n"
    "#   http GET <url>        or        http POST <url> <body>\n"
    "#   wol <MAC address>\n"
    "#\n"
    "# Lines starting with # are ignored. Remove the # from an example and edit it.\n"
    "#Projector on = pjlink 192.168.1.50 POWR 1\n"
    "#Projector off = pjlink 192.168.1.50 POWR 0\n"
    "#Wake rack PC = wol AA:BB:CC:DD:EE:FF\n"
    "#DSP mute = tcp 192.168.1.70:23 DEVICE set mute 1\\r\\n\n"
    "#Recall preset = http GET http://192.168.1.80/api/preset/1\n";

static char* trim(char* s) {
    while(*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

// commands.txt: "Name = type details". Creates a commented sample if the file is missing.
static void commands_load(Model* m) {
    char* buf = malloc(TEXT_FILE_MAX);
    m->command_count = m->command_cursor = 0;
    if(file_read(COMMANDS_FILE, buf, TEXT_FILE_MAX) == 0) {
        file_write(COMMANDS_FILE, commands_sample, strlen(commands_sample));
        buf[0] = 0;
    }
    char* p = buf;
    while(*p && m->command_count < MAX_COMMANDS) {
        char* end = strchr(p, '\n');
        if(end) *end = 0;
        char* line = trim(p);
        char* eq = strchr(line, '=');
        if(line[0] != '#' && eq) {
            *eq = 0;
            char* name = trim(line);
            char* type = trim(eq + 1);
            char* rest = strchr(type, ' ');
            if(rest && name[0]) {
                *rest = 0;
                rest = trim(rest + 1);
                Command* c = &m->commands[m->command_count++];
                copy(c->name, sizeof(c->name), name);
                copy(c->type, sizeof(c->type), type);
                copy(c->rest, sizeof(c->rest), rest);
            }
        }
        if(!end) break;
        p = end + 1;
    }
    free(buf);
}

// --- reports ---

static size_t report_add(char* buf, size_t len, const char* fmt, ...) {
    if(len >= REPORT_SIZE - 1) return len;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + len, REPORT_SIZE - len, fmt, args);
    va_end(args);
    if(n < 0) return len;
    len += (size_t)n;
    return len >= REPORT_SIZE ? REPORT_SIZE - 1 : len;
}

static int prefix_length(const char* mask) {
    int bits = 0;
    unsigned a = 0, b = 0, c = 0, d = 0;
    if(sscanf(mask, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    uint32_t v = (a << 24) | (b << 16) | (c << 8) | d;
    while(v & 0x80000000u) {
        bits++;
        v <<= 1;
    }
    return bits;
}

// "<kind>_<network>_<date>_<time>.txt"
static void report_filename(char* out, size_t size, const char* kind, const char* ssid, const DateTime* t) {
    char safe[24];
    size_t k = 0;
    for(const char* c = ssid; *c && k < sizeof(safe) - 1; c++)
        safe[k++] = ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) ? *c : '_';
    safe[k] = 0;
    snprintf(
        out, size, "%s_%s_%04u%02u%02u_%02u%02u%02u.txt", kind, safe, t->year, t->month, t->day, t->hour, t->minute,
        t->second);
}

static size_t report_header(char* buf, const char* title, const char* ssid, const DateTime* t) {
    size_t len = report_add(buf, 0, "Wi-Fi Tools: %s\n", title);
    len = report_add(buf, len, "Network: %s\n", ssid);
    return report_add(
        buf, len, "Date: %04u-%02u-%02u %02u:%02u:%02u\n\n", t->year, t->month, t->day, t->hour, t->minute, t->second);
}

static size_t report_join(Model* m, char* buf, size_t len) {
    len = report_add(buf, len, "Join check\n");
    len = report_add(
        buf, len, "  Joined in %lu.%lu s, DHCP answered in %lu.%lu s\n", (unsigned long)(m->assoc_ms / 1000),
        (unsigned long)(m->assoc_ms % 1000 / 100), (unsigned long)(m->dhcp_ms / 1000),
        (unsigned long)(m->dhcp_ms % 1000 / 100));
    len = report_add(buf, len, "  IP address: %s/%d\n", m->ip, prefix_length(m->mask));
    if(m->ping_gateway >= 0)
        len = report_add(buf, len, "  Gateway: %s (ping %d ms)\n", m->gateway, m->ping_gateway);
    else
        len = report_add(buf, len, "  Gateway: %s (no ping reply)\n", m->gateway);
    len = report_add(buf, len, "  DNS server: %s (name lookup %s)\n", m->dns, m->dns_ok == 1 ? "worked" : "failed");
    if(m->ping_internet >= 0)
        len = report_add(buf, len, "  Internet: reachable (ping %d ms)\n", m->ping_internet);
    else
        len = report_add(buf, len, "  Internet: not reachable\n");
    return report_add(buf, len, "  Signal: %d dBm on channel %u\n\n", m->join_rssi, m->join_channel);
}

// Builds the text and file name for the requested report
static size_t report_build(Model* m, SaveKind kind, char* buf) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    size_t len = 0;
    if(kind == SaveDiscovery) {
        report_filename(m->report_name, sizeof(m->report_name), "report", m->connected_ssid, &now);
        len = report_header(buf, "discovery report", m->connected_ssid, &now);
        len = report_join(m, buf, len);
        len = report_add(
            buf, len, "Discovery: %u service%s found\n", (unsigned)m->service_count, m->service_count == 1 ? "" : "s");
        if(m->service_count == MAX_SERVICES) len = report_add(buf, len, "  (list is full; there may be more)\n");
        const char* group = "";
        for(size_t i = 0; i < m->service_count; i++) {
            const ServiceEntry* s = &m->services[i];
            if(strcmp(group, s->label) != 0) {
                group = s->label;
                len = report_add(buf, len, "\n  %s\n", group);
            }
            len = report_add(buf, len, "    %s\n      %s:%u  host %s\n", s->name, s->ip, s->port, s->host);
        }
    } else if(kind == SaveDevices) {
        // one file per sweep: port checks made later are added to the same file
        if(!m->devices_name[0])
            report_filename(m->devices_name, sizeof(m->devices_name), "devices", m->connected_ssid, &now);
        copy(m->report_name, sizeof(m->report_name), m->devices_name);
        len = report_header(buf, "device list", m->connected_ssid, &now);
        len = report_add(buf, len, "This device: %s/%d, gateway %s\n", m->ip, prefix_length(m->mask), m->gateway);
        if(m->find_note[0]) len = report_add(buf, len, "%s\n", m->find_note);
        len = report_add(buf, len, "\n%u device%s found\n\n", (unsigned)m->host_count, m->host_count == 1 ? "" : "s");
        if(m->host_count == MAX_HOSTS) len = report_add(buf, len, "(list is full; there may be more)\n\n");
        for(size_t i = 0; i < m->host_count; i++) {
            const Host* h = &m->hosts[i];
            len = report_add(buf, len, "%-15s  %s  %s\n", h->ip, h->mac, h->maker);
            if(h->ports_checked)
                len = report_add(
                    buf, len, "                 open ports: %s\n", h->ports[0] ? h->ports : "none of those checked");
        }
    } else if(kind == SaveStability) {
        report_filename(m->report_name, sizeof(m->report_name), "stability", m->connected_ssid, &m->stab_started);
        len = report_header(buf, "stability log", m->connected_ssid, &m->stab_started);
        uint32_t secs = (furi_get_tick() - m->stab_start_tick) / furi_kernel_get_tick_frequency();
        len = report_add(
            buf, len, "Ran for %lu min %lu s, pinging the gateway (%s) once a second\n\n", (unsigned long)(secs / 60),
            (unsigned long)(secs % 60), m->gateway);
        len = report_add(buf, len, "Pings sent: %lu\n", (unsigned long)m->stab_sent);
        len = report_add(
            buf, len, "Pings lost: %lu (%lu.%lu%%)\n", (unsigned long)m->stab_lost,
            (unsigned long)(m->stab_sent ? m->stab_lost * 100 / m->stab_sent : 0),
            (unsigned long)(m->stab_sent ? m->stab_lost * 1000 / m->stab_sent % 10 : 0));
        if(m->stab_replies) {
            len = report_add(
                buf, len, "Ping time: min %d ms, average %ld ms, max %d ms\n", m->stab_min,
                (long)(m->stab_sum / (int32_t)m->stab_replies), m->stab_max);
            len = report_add(
                buf, len, "Jitter (average change between pings): %ld ms\n",
                (long)(m->stab_replies > 1 ? m->stab_jitter_sum / (int32_t)(m->stab_replies - 1) : 0));
        }
        len = report_add(buf, len, "Roams between access points: %lu\n", (unsigned long)m->stab_roams);
        len = report_add(
            buf, len, "Link drops: %lu (%lu s offline in total)\n", (unsigned long)m->stab_drops,
            (unsigned long)m->stab_down_s);
        len = report_add(buf, len, "Weakest signal: %d dBm\n\n", m->stab_rssi_min);
        len = report_add(buf, len, "Events (time since start)\n");
        if(m->event_count == 0) len = report_add(buf, len, "  none\n");
        for(size_t i = 0; i < m->event_count; i++)
            len = report_add(
                buf, len, "  %02lu:%02lu  %s\n", (unsigned long)(m->events[i].at_s / 60),
                (unsigned long)(m->events[i].at_s % 60), m->events[i].text);
        if(m->event_count == MAX_EVENTS) len = report_add(buf, len, "  (event list is full; later events not shown)\n");
    }
    return len;
}

// --- actions ---

static void start_scan(App* app, Model* m) {
    if(!m->board_ok) {
        show_message(m, "No reply from the devboard.", "Attach it and flash the", "Wi-Fi Tools firmware", "(see the README).");
        send_command(app, "VER", NULL, NULL);
        return;
    }
    m->ap_count = 0;
    m->screen = ScreenScanning;
    send_command(app, "SCAN", NULL, NULL);
}

static void start_walk(App* app, Model* m, const char* ssid) {
    copy(m->target, sizeof(m->target), ssid);
    m->walk_seen = m->walk_lost = false;
    m->walk_sum = 0;
    m->walk_samples = 0;
    m->connected = false; // the walk test leaves any joined network
    m->screen = ScreenWalk;
    send_command(app, "WATCH", ssid, NULL);
}

static void start_join(App* app, Model* m, const char* ssid, const char* password) {
    copy(m->target, sizeof(m->target), ssid);
    m->join_state = JoinConnecting;
    m->join_done = false;
    m->connected = false;
    m->ping_gateway = m->ping_internet = -2;
    m->dns_ok = -1;
    m->screen = ScreenJoin;
    send_command(app, "JOIN", ssid, password);
}

static void start_tool(App* app, Model* m, Tool tool) {
    copy(m->target, sizeof(m->target), m->connected_ssid);
    switch(tool) {
    case ToolDiscovery:
        m->service_count = m->service_cursor = 0;
        m->mdns_done = m->mdns_saved = m->mdns_save_failed = false;
        m->mdns_step = 0;
        m->screen = ScreenDiscovery;
        send_command(app, "MDNS", NULL, NULL);
        break;
    case ToolFinder:
        m->host_count = m->host_cursor = 0;
        m->find_done = m->find_saved = false;
        m->find_step = m->find_total = 0;
        m->find_note[0] = 0;
        m->devices_name[0] = 0;
        m->screen = ScreenFinder;
        send_command(app, "FIND", NULL, NULL);
        break;
    case ToolStability:
        m->history_count = m->history_head = 0;
        m->stab_start_tick = furi_get_tick();
        furi_hal_rtc_get_datetime(&m->stab_started);
        m->stab_sent = m->stab_lost = m->stab_down_s = m->stab_replies = 0;
        m->stab_sum = m->stab_jitter_sum = 0;
        m->stab_roams = m->stab_drops = m->stab_burst = m->stab_down_run = 0;
        m->stab_rssi = m->stab_rssi_min = 0;
        m->stab_bssid[0] = 0;
        m->event_count = 0;
        m->screen = ScreenStability;
        send_command(app, "STAB", NULL, NULL);
        break;
    case ToolControl:
        commands_load(m);
        if(m->command_count == 0)
            show_message(
                m, "No commands yet. Edit", "apps_data/wifi_tools/", "commands.txt on the SD card;", "it holds examples.");
        else
            m->screen = ScreenControl;
        break;
    case ToolClock:
        m->clock_state = ClockAsking;
        m->screen = ScreenClock;
        send_command(app, "TIME", NULL, NULL);
        break;
    default:
        break;
    }
}

static void password_done(void* context) {
    App* app = context;
    password_save(app->join_ssid, app->password);
    with_view_model(app->view, Model * m, { start_join(app, m, app->join_ssid, app->password); }, true);
    view_dispatcher_switch_to_view(app->dispatcher, ViewScreen);
}

// Returns true if the password keyboard should open
static bool begin_join(App* app, Model* m, const AccessPoint* ap) {
    if(strcmp(ap->auth, "Ent") == 0) {
        show_message(m, "This network uses 802.1X", "(enterprise) sign-in, which", "this tool does not support.", "");
        return false;
    }
    if(ap->ssid[0] == 0) {
        show_message(m, "This network hides its", "name, so it cannot be", "joined from the list.", "");
        return false;
    }
    if(strcmp(ap->auth, "Open") == 0) {
        start_join(app, m, ap->ssid, "");
        return false;
    }
    copy(app->join_ssid, sizeof(app->join_ssid), ap->ssid);
    return true;
}

// --- drawing ---

static const char* quality(int rssi) {
    if(rssi >= -55) return "Excellent";
    if(rssi >= -67) return "Good";
    if(rssi >= -75) return "Fair";
    if(rssi >= -82) return "Weak";
    return "Poor";
}

static void draw_row(Canvas* canvas, int32_t y, const char* text, bool selected) {
    if(selected) {
        canvas_draw_box(canvas, 0, y - 8, 128, 9);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str(canvas, 1, y, text);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_title(Canvas* canvas, const char* text) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 1, 8, text);
    canvas_draw_line(canvas, 0, 10, 127, 10);
}

static void draw_survey(Canvas* canvas, Model* m) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u networks (2.4 GHz)", (unsigned)m->ap_count);
    draw_title(canvas, buf);
    if(m->ap_count == 0) canvas_draw_str(canvas, 1, 22, "None found. Left: scan again");
    size_t first = m->ap_cursor >= LIST_ROWS ? m->ap_cursor - LIST_ROWS + 1 : 0;
    for(size_t i = 0; i < LIST_ROWS && first + i < m->ap_count; i++) {
        const AccessPoint* ap = &m->aps[first + i];
        snprintf(
            buf, sizeof(buf), "%2u %4d %c%.17s", ap->channel, ap->rssi, strcmp(ap->auth, "Open") == 0 ? ' ' : '*',
            ap->ssid[0] ? ap->ssid : "(hidden)");
        draw_row(canvas, (int32_t)(19 + 9 * i), buf, first + i == m->ap_cursor);
    }
    const char* hint = m->intent == IntentSurvey ? "OK walk  hold OK join  > ch" : "OK: choose this network";
    canvas_draw_str(canvas, 1, 63, hint);
}

static void draw_channels(Canvas* canvas, Model* m) {
    uint8_t count[15] = {0};
    uint8_t most = 1;
    for(size_t i = 0; i < m->ap_count; i++) {
        uint8_t ch = m->aps[i].channel;
        if(ch >= 1 && ch <= 14) {
            count[ch]++;
            if(count[ch] > most) most = count[ch];
        }
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 1, 8, "Networks per channel");
    char buf[8];
    for(uint8_t ch = 1; ch <= 13; ch++) {
        int32_t x = 3 + (ch - 1) * 9;
        int32_t h = count[ch] * 34 / most;
        if(count[ch] && h < 2) h = 2;
        canvas_draw_box(canvas, x, 52 - h, 7, (size_t)h);
        if(count[ch]) {
            snprintf(buf, sizeof(buf), "%u", count[ch]);
            canvas_draw_str_aligned(canvas, x + 3, 50 - h, AlignCenter, AlignBottom, buf);
        }
        if(ch == 1 || ch == 6 || ch == 11) {
            snprintf(buf, sizeof(buf), "%u", ch);
            canvas_draw_str_aligned(canvas, x + 3, 63, AlignCenter, AlignBottom, buf);
        }
    }
    canvas_draw_line(canvas, 0, 53, 127, 53);
}

static void draw_walk(Canvas* canvas, Model* m) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.24s", m->target);
    draw_title(canvas, buf);
    if(!m->walk_seen) {
        canvas_draw_str(canvas, 1, 28, m->walk_lost ? "Not in range" : "Looking for the network...");
        return;
    }
    int rssi = m->walk_rssi;
    canvas_draw_box(canvas, 2, 21, 7, 2); // minus sign
    canvas_set_font(canvas, FontBigNumbers);
    snprintf(buf, sizeof(buf), "%d", -rssi);
    canvas_draw_str(canvas, 12, 29, buf);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 50, 20, "dBm");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 50, 30, m->walk_lost ? "LOST" : quality(rssi));
    canvas_set_font(canvas, FontSecondary);

    // bar: -90 dBm empty, -30 dBm full
    int fill = (rssi + 90) * 124 / 60;
    if(fill < 0) fill = 0;
    if(fill > 124) fill = 124;
    canvas_draw_frame(canvas, 1, 33, 126, 6);
    if(!m->walk_lost) canvas_draw_box(canvas, 2, 34, (size_t)fill, 4);

    snprintf(
        buf, sizeof(buf), "min %d  max %d  avg %ld", m->walk_min, m->walk_max,
        (long)(m->walk_sum / (int32_t)m->walk_samples));
    canvas_draw_str(canvas, 1, 49, buf);
    snprintf(
        buf, sizeof(buf), "ch %u  AP %s  x%u", m->walk_channel, strlen(m->walk_bssid) >= 17 ? m->walk_bssid + 9 : "?",
        m->walk_ap_count);
    canvas_draw_str(canvas, 1, 58, buf);
}

static void draw_join(Canvas* canvas, Model* m) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%.24s", m->target);
    draw_title(canvas, buf);
    if(m->join_state == JoinConnecting) {
        canvas_draw_str(canvas, 1, 22, "Connecting...");
        return;
    }
    if(m->join_state == JoinFailed) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 1, 23, "Could not join");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 1, 34, m->join_fail);
        canvas_draw_str(canvas, 1, 63, "Back: menu");
        return;
    }
    snprintf(
        buf, sizeof(buf), "Joined %lu.%lus  DHCP %lu.%lus", (unsigned long)(m->assoc_ms / 1000),
        (unsigned long)(m->assoc_ms % 1000 / 100), (unsigned long)(m->dhcp_ms / 1000),
        (unsigned long)(m->dhcp_ms % 1000 / 100));
    canvas_draw_str(canvas, 1, 18, buf);
    snprintf(buf, sizeof(buf), "IP %s/%d", m->ip, prefix_length(m->mask));
    canvas_draw_str(canvas, 1, 27, buf);
    if(m->ping_gateway == -2)
        snprintf(buf, sizeof(buf), "GW %s ...", m->gateway);
    else if(m->ping_gateway < 0)
        snprintf(buf, sizeof(buf), "GW %s no reply", m->gateway);
    else
        snprintf(buf, sizeof(buf), "GW %s %d ms", m->gateway, m->ping_gateway);
    canvas_draw_str(canvas, 1, 36, buf);
    if(m->dns_ok < 0)
        snprintf(buf, sizeof(buf), "DNS %s ...", m->dns);
    else
        snprintf(buf, sizeof(buf), "DNS %s %s", m->dns, m->dns_ok ? "ok" : "FAIL");
    canvas_draw_str(canvas, 1, 45, buf);
    if(m->ping_internet == -2)
        snprintf(buf, sizeof(buf), "Internet ...   %d dBm ch %u", m->join_rssi, m->join_channel);
    else if(m->ping_internet < 0)
        snprintf(buf, sizeof(buf), "Internet none  %d dBm ch %u", m->join_rssi, m->join_channel);
    else
        snprintf(buf, sizeof(buf), "Internet %d ms  %d dBm ch %u", m->ping_internet, m->join_rssi, m->join_channel);
    canvas_draw_str(canvas, 1, 54, buf);
    if(m->join_done) canvas_draw_str(canvas, 1, 63, "Back: menu (stays joined)");
}

static void draw_discovery(Canvas* canvas, Model* m) {
    char buf[72];
    if(m->mdns_done)
        snprintf(
            buf, sizeof(buf), "%u services found%s", (unsigned)m->service_count,
            m->mdns_saved ? ", saved" : m->mdns_save_failed ? ", NOT saved" : "");
    else
        snprintf(buf, sizeof(buf), "Searching %u/%u: %s", m->mdns_step, m->mdns_steps, m->mdns_label);
    draw_title(canvas, buf);
    if(m->service_count == 0 && m->mdns_done) {
        canvas_draw_str(canvas, 1, 22, "Nothing is announcing itself");
        canvas_draw_str(canvas, 1, 31, "on this network. Discovery");
        canvas_draw_str(canvas, 1, 40, "may be blocked between VLANs.");
        return;
    }
    size_t rows = 4;
    size_t first = m->service_cursor >= rows ? m->service_cursor - rows + 1 : 0;
    for(size_t i = 0; i < rows && first + i < m->service_count; i++) {
        const ServiceEntry* s = &m->services[first + i];
        snprintf(buf, sizeof(buf), "%s: %.40s", s->label, s->name);
        buf[27] = 0;
        draw_row(canvas, (int32_t)(19 + 9 * i), buf, first + i == m->service_cursor);
    }
    if(m->service_count) {
        const ServiceEntry* s = &m->services[m->service_cursor];
        canvas_draw_line(canvas, 0, 47, 127, 47);
        snprintf(buf, sizeof(buf), "%s:%u", s->ip, s->port);
        canvas_draw_str(canvas, 1, 55, buf);
        snprintf(buf, sizeof(buf), "%.27s", s->host);
        canvas_draw_str(canvas, 1, 63, buf);
    }
}

static void draw_finder(Canvas* canvas, Model* m) {
    char buf[64];
    if(m->find_done)
        snprintf(buf, sizeof(buf), "%u devices found%s", (unsigned)m->host_count, m->find_saved ? ", saved" : "");
    else
        snprintf(buf, sizeof(buf), "Sweeping %u/%u, found %u", m->find_step, m->find_total, (unsigned)m->host_count);
    draw_title(canvas, buf);
    if(m->host_count == 0) {
        if(m->find_done) canvas_draw_str(canvas, 1, 22, "No other devices answered.");
        return;
    }
    size_t rows = 3;
    size_t first = m->host_cursor >= rows ? m->host_cursor - rows + 1 : 0;
    for(size_t i = 0; i < rows && first + i < m->host_count; i++) {
        const Host* h = &m->hosts[first + i];
        const char* last = strrchr(h->ip, '.');
        snprintf(buf, sizeof(buf), "%-4s %s", last ? last : h->ip, h->maker);
        draw_row(canvas, (int32_t)(19 + 9 * i), buf, first + i == m->host_cursor);
    }
    const Host* h = &m->hosts[m->host_cursor];
    canvas_draw_line(canvas, 0, 39, 127, 39);
    canvas_draw_str(canvas, 1, 47, h->ip);
    canvas_draw_str(canvas, 1, 55, h->mac);
    if(h->ports_checked)
        snprintf(buf, sizeof(buf), "Ports: %.20s", h->ports[0] ? h->ports : "none open");
    else
        snprintf(buf, sizeof(buf), "OK: check control ports");
    canvas_draw_str(canvas, 1, 63, buf);
}

static void draw_ports(Canvas* canvas, Model* m) {
    char buf[64];
    const Host* h = &m->hosts[m->host_cursor];
    snprintf(buf, sizeof(buf), "%s %.10s", h->ip, h->maker);
    draw_title(canvas, buf);
    for(size_t i = 0; i < 5 && i < m->port_count; i++) {
        // show the last five while the list grows
        size_t idx = m->port_count > 5 ? m->port_count - 5 + i : i;
        snprintf(buf, sizeof(buf), "%u  %s", m->ports[idx].number, m->ports[idx].name);
        canvas_draw_str(canvas, 1, (int32_t)(19 + 9 * i), buf);
    }
    if(!m->ports_done)
        canvas_draw_str(canvas, 1, 63, "Checking...");
    else if(m->port_count == 0)
        canvas_draw_str(canvas, 1, 22, "No control ports open.");
    else if(m->port_count <= 4) {
        snprintf(buf, sizeof(buf), "%u open. Back: device list", (unsigned)m->port_count);
        canvas_draw_str(canvas, 1, 63, buf);
    }
}

static void draw_stability(Canvas* canvas, Model* m) {
    char buf[64];
    uint32_t secs = (furi_get_tick() - m->stab_start_tick) / furi_kernel_get_tick_frequency();
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buf, sizeof(buf), "%02lu:%02lu  %d dBm  ch %u", (unsigned long)(secs / 60), (unsigned long)(secs % 60),
        m->stab_rssi, m->stab_channel);
    canvas_draw_str(canvas, 1, 8, buf);

    // one column per ping, newest at the right; 60 ms or more fills the height
    const int32_t top = 11, height = 20;
    canvas_draw_frame(canvas, 0, top - 1, 128, (size_t)(height + 2));
    for(size_t i = 0; i < m->history_count; i++) {
        int16_t ms = m->history[(m->history_head + i) % HISTORY];
        int32_t x = 4 + (int32_t)(HISTORY - m->history_count + i);
        if(ms < 0) {
            // lost: a dotted full-height column; offline: a solid one
            for(int32_t y = top; y < top + height; y += (ms == -2 ? 1 : 2)) canvas_draw_dot(canvas, x, y);
        } else {
            int32_t h = 1 + ms * (height - 1) / 60;
            if(h > height) h = height;
            canvas_draw_line(canvas, x, top + height - h, x, top + height - 1);
        }
    }

    if(m->stab_replies)
        snprintf(
            buf, sizeof(buf), "now %d  avg %ld  max %d ms", m->stab_last, (long)(m->stab_sum / (int32_t)m->stab_replies),
            m->stab_max);
    else
        snprintf(buf, sizeof(buf), "Waiting for the first reply...");
    canvas_draw_str(canvas, 1, 41, buf);
    snprintf(
        buf, sizeof(buf), "lost %lu of %lu  jitter %ld ms", (unsigned long)m->stab_lost, (unsigned long)m->stab_sent,
        (long)(m->stab_replies > 1 ? m->stab_jitter_sum / (int32_t)(m->stab_replies - 1) : 0));
    canvas_draw_str(canvas, 1, 50, buf);
    snprintf(buf, sizeof(buf), "roams %lu  drops %lu", (unsigned long)m->stab_roams, (unsigned long)m->stab_drops);
    canvas_draw_str(canvas, 1, 59, buf);
    canvas_draw_str_aligned(canvas, 127, 59, AlignRight, AlignBottom, "Back: save");
}

static void draw_control(Canvas* canvas, Model* m) {
    char buf[48];
    snprintf(buf, sizeof(buf), "Commands (%u)", (unsigned)m->command_count);
    draw_title(canvas, buf);
    size_t first = m->command_cursor >= LIST_ROWS ? m->command_cursor - LIST_ROWS + 1 : 0;
    for(size_t i = 0; i < LIST_ROWS && first + i < m->command_count; i++)
        draw_row(canvas, (int32_t)(19 + 9 * i), m->commands[first + i].name, first + i == m->command_cursor);
    canvas_draw_str(canvas, 1, 63, "OK: send");
}

static void draw_control_result(Canvas* canvas, Model* m) {
    const Command* c = &m->commands[m->command_cursor];
    draw_title(canvas, c->name);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(
        canvas, 1, 22, m->result_state == ResultPending ? "Sending..." : m->result_state == ResultOk ? "OK" : "FAILED");
    canvas_set_font(canvas, FontSecondary);
    if(m->result_state == ResultPending) return;
    // wrap the reply over up to four lines
    char line[27];
    size_t len = strlen(m->result_text);
    for(size_t row = 0; row < 4 && row * 26 < len; row++) {
        strlcpy(line, m->result_text + row * 26, sizeof(line));
        canvas_draw_str(canvas, 1, (int32_t)(32 + 9 * row), line);
    }
}

static void format_zone(int minutes, char* out, size_t size) {
    int a = minutes < 0 ? -minutes : minutes;
    snprintf(out, size, "UTC%c%02d:%02d", minutes < 0 ? '-' : '+', a / 60, a % 60);
}

static void draw_clock(Canvas* canvas, Model* m) {
    char buf[64], zone[16];
    draw_title(canvas, "Clock sync");
    if(m->clock_state == ClockAsking) {
        canvas_draw_str(canvas, 1, 24, "Asking a time server...");
        return;
    }
    if(m->clock_state == ClockFailed) {
        canvas_draw_str(canvas, 1, 24, "Could not get the time:");
        canvas_draw_str(canvas, 1, 34, m->clock_fail);
        return;
    }
    DateTime net, mine;
    uint32_t local = clock_utc_now(m) + (uint32_t)((int32_t)m->zone_minutes * 60);
    datetime_timestamp_to_datetime(local, &net);
    furi_hal_rtc_get_datetime(&mine);
    int32_t off = (int32_t)datetime_datetime_to_timestamp(&mine) - (int32_t)local;
    format_zone(m->zone_minutes, zone, sizeof(zone));

    snprintf(
        buf, sizeof(buf), "Network %04u-%02u-%02u %02u:%02u:%02u", net.year, net.month, net.day, net.hour, net.minute,
        net.second);
    canvas_draw_str(canvas, 1, 20, buf);
    snprintf(
        buf, sizeof(buf), "Flipper  %04u-%02u-%02u %02u:%02u:%02u", mine.year, mine.month, mine.day, mine.hour,
        mine.minute, mine.second);
    canvas_draw_str(canvas, 1, 29, buf);
    if(m->clock_state == ClockSet)
        snprintf(buf, sizeof(buf), "Flipper clock set.");
    else if(off > -3600 && off < 3600)
        snprintf(buf, sizeof(buf), "Flipper is %ld s %s", (long)(off < 0 ? -off : off), off < 0 ? "behind" : "ahead");
    else
        snprintf(buf, sizeof(buf), "Flipper is off by over an hour");
    canvas_draw_str(canvas, 1, 40, buf);
    snprintf(buf, sizeof(buf), "Zone %s%s  (L/R)", zone, m->zone_guessed ? "" : " ?");
    canvas_draw_str(canvas, 1, 51, buf);
    canvas_draw_str(canvas, 1, 63, m->clock_state == ClockSet ? "Back: menu" : "OK: set the Flipper clock");
}

static void draw_cb(Canvas* canvas, void* model) {
    Model* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    switch(m->screen) {
    case ScreenMessage:
        for(int i = 0; i < 4; i++)
            if(m->message[i]) canvas_draw_str(canvas, 1, 14 + 10 * i, m->message[i]);
        canvas_draw_str(canvas, 1, 63, "Back: menu");
        break;
    case ScreenScanning:
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, "Scanning...");
        break;
    case ScreenSurvey:
        draw_survey(canvas, m);
        break;
    case ScreenChannels:
        draw_channels(canvas, m);
        break;
    case ScreenWalk:
        draw_walk(canvas, m);
        break;
    case ScreenJoin:
        draw_join(canvas, m);
        break;
    case ScreenDiscovery:
        draw_discovery(canvas, m);
        break;
    case ScreenFinder:
        draw_finder(canvas, m);
        break;
    case ScreenPorts:
        draw_ports(canvas, m);
        break;
    case ScreenStability:
        draw_stability(canvas, m);
        break;
    case ScreenControl:
        draw_control(canvas, m);
        break;
    case ScreenControlResult:
        draw_control_result(canvas, m);
        break;
    case ScreenClock:
        draw_clock(canvas, m);
        break;
    }
}

// --- input ---

static bool input_cb(InputEvent* event, void* context) {
    App* app = context;
    bool step = event->type == InputTypeShort || event->type == InputTypeRepeat;
    bool is_short = event->type == InputTypeShort, is_long = event->type == InputTypeLong;
    bool to_menu = false, to_password = false;
    if(!step && !is_long) return false;

    with_view_model(
        app->view,
        Model * m,
        {
            const AccessPoint* ap = m->ap_count ? &m->aps[m->ap_cursor] : NULL;
            if(event->key == InputKeyBack && is_short) {
                switch(m->screen) {
                case ScreenChannels:
                    m->screen = ScreenSurvey;
                    break;
                case ScreenWalk:
                    send_command(app, "STOP", NULL, NULL);
                    m->screen = ScreenSurvey;
                    break;
                case ScreenPorts:
                    if(!m->ports_done) send_command(app, "HALT", NULL, NULL);
                    m->screen = ScreenFinder;
                    break;
                case ScreenControlResult:
                    m->screen = ScreenControl;
                    break;
                case ScreenStability:
                    send_command(app, "HALT", NULL, NULL);
                    stab_close_runs(m);
                    if(m->stab_sent || m->stab_down_s) {
                        m->save_request = SaveStability;
                        show_message(m, "Saving the stability log...", "", "", "");
                    } else {
                        to_menu = true;
                    }
                    break;
                case ScreenDiscovery:
                    if(!m->mdns_done) send_command(app, "HALT", NULL, NULL);
                    to_menu = true;
                    break;
                case ScreenFinder:
                    if(!m->find_done) send_command(app, "HALT", NULL, NULL);
                    to_menu = true;
                    break;
                default:
                    to_menu = true;
                    break;
                }
            } else if(m->screen == ScreenSurvey) {
                if(event->key == InputKeyUp && step && m->ap_cursor > 0) m->ap_cursor--;
                if(event->key == InputKeyDown && step && m->ap_cursor + 1 < m->ap_count) m->ap_cursor++;
                if(event->key == InputKeyLeft && is_short) start_scan(app, m);
                if(event->key == InputKeyRight && is_short) m->screen = ScreenChannels;
                if(event->key == InputKeyOk && ap && (is_short || is_long)) {
                    bool join = m->intent == IntentJoin || (m->intent == IntentSurvey && is_long);
                    if(join) {
                        if(m->intent == IntentSurvey) m->next_tool = ToolNone;
                        to_password = begin_join(app, m, ap);
                    } else if(is_short) {
                        if(ap->ssid[0])
                            start_walk(app, m, ap->ssid);
                        else
                            show_message(m, "This network hides its", "name, so it cannot be", "tracked from the list.", "");
                    }
                }
            } else if(m->screen == ScreenChannels) {
                if((event->key == InputKeyLeft || event->key == InputKeyRight) && is_short) m->screen = ScreenSurvey;
            } else if(m->screen == ScreenWalk) {
                if(event->key == InputKeyOk && is_short) {
                    m->walk_seen = false;
                    m->walk_sum = 0;
                    m->walk_samples = 0;
                }
            } else if(m->screen == ScreenDiscovery) {
                if(event->key == InputKeyUp && step && m->service_cursor > 0) m->service_cursor--;
                if(event->key == InputKeyDown && step && m->service_cursor + 1 < m->service_count) m->service_cursor++;
            } else if(m->screen == ScreenFinder) {
                if(event->key == InputKeyUp && step && m->host_cursor > 0) m->host_cursor--;
                if(event->key == InputKeyDown && step && m->host_cursor + 1 < m->host_count) m->host_cursor++;
                if(event->key == InputKeyOk && is_short && m->find_done && m->host_count) {
                    Host* h = &m->hosts[m->host_cursor];
                    h->ports[0] = 0;
                    h->ports_checked = false;
                    m->port_count = 0;
                    m->ports_done = false;
                    m->screen = ScreenPorts;
                    send_command(app, "PORTS", h->ip, NULL);
                }
            } else if(m->screen == ScreenControl) {
                if(event->key == InputKeyUp && step && m->command_cursor > 0) m->command_cursor--;
                if(event->key == InputKeyDown && step && m->command_cursor + 1 < m->command_count) m->command_cursor++;
                if(event->key == InputKeyOk && is_short && m->command_count) {
                    const Command* c = &m->commands[m->command_cursor];
                    m->result_state = ResultPending;
                    m->result_text[0] = 0;
                    m->screen = ScreenControlResult;
                    send_command(app, "CTL", c->type, c->rest);
                }
            } else if(m->screen == ScreenControlResult) {
                if(event->key == InputKeyOk && is_short && m->result_state != ResultPending) {
                    const Command* c = &m->commands[m->command_cursor];
                    m->result_state = ResultPending;
                    send_command(app, "CTL", c->type, c->rest);
                }
            } else if(m->screen == ScreenClock && m->clock_state != ClockAsking && m->clock_state != ClockFailed) {
                if(event->key == InputKeyRight && step && m->zone_minutes < 14 * 60) {
                    m->zone_minutes += 15;
                    m->clock_state = ClockReady;
                }
                if(event->key == InputKeyLeft && step && m->zone_minutes > -12 * 60) {
                    m->zone_minutes -= 15;
                    m->clock_state = ClockReady;
                }
                if(event->key == InputKeyOk && is_short) {
                    DateTime set;
                    datetime_timestamp_to_datetime(clock_utc_now(m) + (uint32_t)((int32_t)m->zone_minutes * 60), &set);
                    furi_hal_rtc_set_datetime(&set);
                    m->zone_guessed = true;
                    m->clock_state = ClockSet;
                }
            }
        },
        true);

    if(to_password) {
        password_load(app->join_ssid, app->password, sizeof(app->password));
        text_input_set_header_text(app->text_input, "Wi-Fi password");
        text_input_set_result_callback(
            app->text_input, password_done, app, app->password, sizeof(app->password), false);
        view_dispatcher_switch_to_view(app->dispatcher, ViewPassword);
    } else if(to_menu) {
        view_dispatcher_switch_to_view(app->dispatcher, ViewMenu);
    }
    return true;
}

static void menu_cb(void* context, uint32_t index) {
    App* app = context;
    bool stay_in_menu = false;
    with_view_model(
        app->view,
        Model * m,
        {
            Tool tool = index == MenuDiscovery ? ToolDiscovery :
                        index == MenuFinder    ? ToolFinder :
                        index == MenuStability ? ToolStability :
                        index == MenuControl   ? ToolControl :
                        index == MenuClock     ? ToolClock :
                                                 ToolNone;
            if(index == MenuLeave) {
                send_command(app, "STOP", NULL, NULL);
                m->connected = false;
                stay_in_menu = true;
            } else if(tool != ToolNone && m->connected) {
                start_tool(app, m, tool); // already on a network: use it
            } else {
                m->next_tool = tool;
                m->intent = index == MenuSurvey ? IntentSurvey : index == MenuWalk ? IntentWalk : IntentJoin;
                start_scan(app, m);
            }
        },
        true);
    if(!stay_in_menu) view_dispatcher_switch_to_view(app->dispatcher, ViewScreen);
}

static void tick_cb(void* context) {
    App* app = context;
    app->ticks++;
    bool board_ok = false, connected = false;
    SaveKind kind = SaveNone;
    char* report = NULL;
    size_t report_len = 0;
    char report_name[80];
    char ssid[33];
    with_view_model(
        app->view,
        Model * m,
        {
            process_rx(app, m);
            board_ok = m->board_ok;
            connected = m->connected;
            strlcpy(ssid, m->connected_ssid, sizeof(ssid));
            if(m->save_request != SaveNone) {
                kind = m->save_request;
                m->save_request = SaveNone;
                report = malloc(REPORT_SIZE);
                report_len = report_build(m, kind, report);
                snprintf(report_name, sizeof(report_name), "%s/%s", STORAGE_APP_DATA_PATH_PREFIX, m->report_name);
            }
        },
        true);
    if(report) {
        // the SD card write happens outside the lock so the screen keeps drawing
        bool ok = file_write(report_name, report, report_len);
        free(report);
        with_view_model(
            app->view,
            Model * m,
            {
                if(kind == SaveDiscovery) {
                    m->mdns_saved = ok;
                    m->mdns_save_failed = !ok;
                } else if(kind == SaveDevices) {
                    m->find_saved = ok;
                } else if(kind == SaveStability) {
                    copy(
                        m->message_buf[0], sizeof(m->message_buf[0]),
                        ok ? "Stability log saved as" : "Could not save the log");
                    snprintf(m->message_buf[1], sizeof(m->message_buf[1]), "%.38s", ok ? m->report_name : "");
                    show_message(m, m->message_buf[0], m->message_buf[1], "in apps_data/wifi_tools", "");
                }
            },
            true);
    }
    // show the joined network in the menu title
    char header[48];
    if(connected)
        snprintf(header, sizeof(header), "On: %.24s", ssid);
    else
        snprintf(header, sizeof(header), "Wi-Fi Tools");
    if(strcmp(header, app->header) != 0) {
        strlcpy(app->header, header, sizeof(app->header));
        submenu_set_header(app->menu, app->header);
    }
    // keep asking until the devboard answers (it may still be booting)
    if(!board_ok && app->ticks % 20 == 1) send_command(app, "VER", NULL, NULL);
}

static uint32_t menu_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t back_to_screen(void* context) {
    UNUSED(context);
    return ViewScreen;
}

int32_t wifi_tools_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->rx = furi_stream_buffer_alloc(4096, 1);

    // Take the UART from the expansion-module service while the app runs
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        furi_hal_serial_init(app->serial, BAUD);
        furi_hal_serial_async_rx_start(app->serial, rx_cb, app, false);
    }

    app->gui = furi_record_open(RECORD_GUI);
    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_tick_event_callback(app->dispatcher, tick_cb, furi_ms_to_ticks(TICK_MS));

    app->menu = submenu_alloc();
    submenu_add_item(app->menu, "Wi-Fi survey", MenuSurvey, menu_cb, app);
    submenu_add_item(app->menu, "Signal walk test", MenuWalk, menu_cb, app);
    submenu_add_item(app->menu, "Join-the-network check", MenuJoin, menu_cb, app);
    submenu_add_item(app->menu, "Discovery check", MenuDiscovery, menu_cb, app);
    submenu_add_item(app->menu, "Device finder", MenuFinder, menu_cb, app);
    submenu_add_item(app->menu, "Stability logger", MenuStability, menu_cb, app);
    submenu_add_item(app->menu, "Control client", MenuControl, menu_cb, app);
    submenu_add_item(app->menu, "Clock sync", MenuClock, menu_cb, app);
    submenu_add_item(app->menu, "Leave network", MenuLeave, menu_cb, app);
    view_set_previous_callback(submenu_get_view(app->menu), menu_exit);
    view_dispatcher_add_view(app->dispatcher, ViewMenu, submenu_get_view(app->menu));

    app->view = view_alloc();
    view_set_context(app->view, app);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(Model));
    view_set_draw_callback(app->view, draw_cb);
    view_set_input_callback(app->view, input_cb);
    view_dispatcher_add_view(app->dispatcher, ViewScreen, app->view);

    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), back_to_screen);
    view_dispatcher_add_view(app->dispatcher, ViewPassword, text_input_get_view(app->text_input));

    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_switch_to_view(app->dispatcher, ViewMenu);
    send_command(app, "STOP", NULL, NULL);
    send_command(app, "VER", NULL, NULL);
    view_dispatcher_run(app->dispatcher);

    send_command(app, "STOP", NULL, NULL);
    view_dispatcher_remove_view(app->dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->dispatcher, ViewScreen);
    view_dispatcher_remove_view(app->dispatcher, ViewPassword);
    submenu_free(app->menu);
    view_free(app->view);
    text_input_free(app->text_input);
    view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_GUI);
    if(app->serial) {
        furi_hal_serial_tx_wait_complete(app->serial);
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }
    expansion_enable(app->expansion);
    furi_record_close(RECORD_EXPANSION);
    furi_stream_buffer_free(app->rx);
    free(app);
    return 0;
}
