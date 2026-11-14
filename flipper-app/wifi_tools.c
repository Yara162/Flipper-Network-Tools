// Wi-Fi Tools: Flipper Zero front end for the Wi-Fi utility firmware on the Wi-Fi Devboard.
// Survey, signal walk test, join-the-network check and service discovery.
//
// The devboard is driven with text lines over the USART (pins 13/14) at 115200 baud; see
// esp32-firmware/src/main.cpp for the protocol.
#include <furi.h>
#include <stdarg.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <input/input.h>
#include <storage/storage.h>
#include <expansion/expansion.h>

#define BAUD 115200
#define TICK_MS 50
#define MAX_APS 32
#define MAX_SERVICES 40
#define LINE_SIZE 200
#define PASSWORD_SIZE 64
#define PASSWORD_FILE STORAGE_APP_DATA_PATH_PREFIX "/passwords.txt"
#define PASSWORD_FILE_MAX 2048
#define LIST_ROWS 5
#define REPORT_SIZE 7168

typedef enum { ViewMenu, ViewScreen, ViewPassword } ViewId;

typedef enum { IntentSurvey, IntentWalk, IntentJoin, IntentDiscovery } Intent;

typedef enum {
    ScreenMessage,
    ScreenScanning,
    ScreenSurvey,
    ScreenChannels,
    ScreenWalk,
    ScreenJoin,
    ScreenDiscovery,
} Screen;

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

typedef enum { JoinConnecting, JoinOk, JoinFailed } JoinState;

typedef struct {
    Screen screen;
    Intent intent;
    const char* message[4];
    bool board_ok;

    AccessPoint aps[MAX_APS];
    size_t ap_count, ap_cursor;
    bool scan_done;

    char target[33]; // network the walk test or join check is about

    // walk test
    bool walk_seen, walk_lost;
    int16_t walk_rssi, walk_min, walk_max;
    int32_t walk_sum;
    uint32_t walk_samples;
    uint8_t walk_channel, walk_ap_count;
    char walk_bssid[18];

    // join check
    JoinState join_state;
    bool join_done, auto_discover;
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
    bool mdns_done;
    uint8_t mdns_step, mdns_steps;
    char mdns_label[14];

    // report file
    bool save_request, saved, save_failed;
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
    bool join_discover;
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

static void send(App* app, const char* text) {
    if(app->serial) furi_hal_serial_tx(app->serial, (const uint8_t*)text, strlen(text));
}

static void send_command(App* app, const char* command, const char* a, const char* b) {
    char buf[LINE_SIZE];
    if(a && b)
        snprintf(buf, sizeof(buf), "%s\t%s\t%s\n", command, a, b);
    else if(a)
        snprintf(buf, sizeof(buf), "%s\t%s\n", command, a);
    else
        snprintf(buf, sizeof(buf), "%s\n", command);
    send(app, buf);
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

static void handle_reply(App* app, Model* m, char* text) {
    char* f[10];
    size_t n = split(text, f, COUNT_OF(f));
    const char* cmd = f[0];

    if(strcmp(cmd, "VER") == 0 || strcmp(cmd, "READY") == 0) {
        m->board_ok = true;
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
        m->scan_done = true;
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
        copy(m->join_fail, sizeof(m->join_fail), f[1]);
    } else if(strcmp(cmd, "PING") == 0 && n >= 3) {
        if(strcmp(f[1], "gateway") == 0) m->ping_gateway = (int16_t)atoi(f[2]);
        if(strcmp(f[1], "internet") == 0) m->ping_internet = (int16_t)atoi(f[2]);
    } else if(strcmp(cmd, "DNS") == 0 && n >= 3) {
        m->dns_ok = strcmp(f[1], "ok") == 0 ? 1 : 0;
        m->dns_ms = (uint32_t)atoi(f[2]);
    } else if(strcmp(cmd, "JOIN_END") == 0) {
        m->join_done = true;
        if(m->join_state == JoinOk && m->auto_discover && m->screen == ScreenJoin) {
            m->auto_discover = false;
            m->service_count = m->service_cursor = 0;
            m->mdns_done = false;
            m->saved = m->save_failed = false;
            m->mdns_step = 0;
            m->screen = ScreenDiscovery;
            send_command(app, "MDNS", NULL, NULL);
        }
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
        m->save_request = m->screen == ScreenDiscovery;
    } else if(strcmp(cmd, "MDNS_FAIL") == 0) {
        m->mdns_done = true;
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

// --- saved passwords: SD Card/apps_data/wifi_tools/passwords.txt ---
// Pairs of lines: a network name, then its password.

static size_t passwords_read(char* buf, size_t size) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(PASSWORD_FILE);
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

static void password_load(const char* ssid, char* out, size_t size) {
    char* buf = malloc(PASSWORD_FILE_MAX);
    out[0] = 0;
    passwords_read(buf, PASSWORD_FILE_MAX);
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
    char* old = malloc(PASSWORD_FILE_MAX);
    char* fresh = malloc(PASSWORD_FILE_MAX + 128);
    size_t n = 0;
    passwords_read(old, PASSWORD_FILE_MAX);
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
        if(!same && n + pair_len < PASSWORD_FILE_MAX) {
            memcpy(fresh + n, p, pair_len);
            n += pair_len;
            if(!pass_end) fresh[n++] = '\n';
        }
        if(!pass_end) break;
        p = pass_end + 1;
    }
    n += (size_t)snprintf(fresh + n, 128, "%s\n%s\n", ssid, password);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_set(PASSWORD_FILE);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS))
        storage_file_write(file, fresh, n);
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    free(old);
    free(fresh);
}

// --- reports: SD Card/apps_data/wifi_tools/report_<network>_<date>_<time>.txt ---

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

static int prefix_length(const char* mask);

// Builds the report text and its file name from the join and discovery results
static size_t report_build(Model* m, char* buf) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    char safe[24];
    size_t k = 0;
    for(const char* c = m->target; *c && k < sizeof(safe) - 1; c++)
        safe[k++] = ((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')) ? *c : '_';
    safe[k] = 0;
    snprintf(
        m->report_name, sizeof(m->report_name), "report_%s_%04u%02u%02u_%02u%02u%02u.txt", safe, now.year, now.month,
        now.day, now.hour, now.minute, now.second);

    size_t len = 0;
    len = report_add(buf, len, "Wi-Fi Tools report\n");
    len = report_add(buf, len, "Network: %s\n", m->target);
    len = report_add(
        buf, len, "Date: %04u-%02u-%02u %02u:%02u:%02u\n\n", now.year, now.month, now.day, now.hour, now.minute,
        now.second);

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
    len = report_add(
        buf, len, "  DNS server: %s (name lookup %s)\n", m->dns, m->dns_ok == 1 ? "worked" : "failed");
    if(m->ping_internet >= 0)
        len = report_add(buf, len, "  Internet: reachable (ping %d ms)\n", m->ping_internet);
    else
        len = report_add(buf, len, "  Internet: not reachable\n");
    len = report_add(buf, len, "  Signal: %d dBm on channel %u\n\n", m->join_rssi, m->join_channel);

    len = report_add(buf, len, "Discovery: %u service%s found\n", (unsigned)m->service_count, m->service_count == 1 ? "" : "s");
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
    return len;
}

static bool report_write(const char* name, const char* buf, size_t len) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_printf("%s/%s", STORAGE_APP_DATA_PATH_PREFIX, name);
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

// --- actions ---

static void show_message(Model* m, const char* a, const char* b, const char* c, const char* d) {
    m->screen = ScreenMessage;
    m->message[0] = a;
    m->message[1] = b;
    m->message[2] = c;
    m->message[3] = d;
}

static void start_scan(App* app, Model* m) {
    if(!m->board_ok) {
        show_message(m, "No reply from the devboard.", "Attach it and flash the", "Wi-Fi Tools firmware", "(see the README).");
        send_command(app, "VER", NULL, NULL);
        return;
    }
    m->ap_count = 0;
    m->scan_done = false;
    m->screen = ScreenScanning;
    send_command(app, "SCAN", NULL, NULL);
}

static void start_walk(App* app, Model* m, const char* ssid) {
    copy(m->target, sizeof(m->target), ssid);
    m->walk_seen = m->walk_lost = false;
    m->walk_sum = 0;
    m->walk_samples = 0;
    m->screen = ScreenWalk;
    send_command(app, "WATCH", ssid, NULL);
}

static void start_join(App* app, Model* m, const char* ssid, const char* password, bool discover) {
    copy(m->target, sizeof(m->target), ssid);
    m->join_state = JoinConnecting;
    m->join_done = false;
    m->auto_discover = discover;
    m->ping_gateway = m->ping_internet = -2;
    m->dns_ok = -1;
    m->screen = ScreenJoin;
    send_command(app, "JOIN", ssid, password);
}

static void password_done(void* context) {
    App* app = context;
    password_save(app->join_ssid, app->password);
    with_view_model(
        app->view, Model * m, { start_join(app, m, app->join_ssid, app->password, app->join_discover); }, true);
    view_dispatcher_switch_to_view(app->dispatcher, ViewScreen);
}

// Returns true if the password keyboard should open
static bool begin_join(App* app, Model* m, const AccessPoint* ap, bool discover) {
    if(strcmp(ap->auth, "Ent") == 0) {
        show_message(m, "This network uses 802.1X", "(enterprise) sign-in, which", "this tool does not support.", "");
        return false;
    }
    if(ap->ssid[0] == 0) {
        show_message(m, "This network hides its", "name, so it cannot be", "joined from the list.", "");
        return false;
    }
    if(strcmp(ap->auth, "Open") == 0) {
        start_join(app, m, ap->ssid, "", discover);
        return false;
    }
    copy(app->join_ssid, sizeof(app->join_ssid), ap->ssid);
    app->join_discover = discover;
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

static void draw_survey(Canvas* canvas, Model* m) {
    char buf[48];
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%u networks (2.4 GHz)", (unsigned)m->ap_count);
    canvas_draw_str(canvas, 1, 8, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);
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
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%.24s", m->target);
    canvas_draw_str(canvas, 1, 8, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);
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

static void draw_join(Canvas* canvas, Model* m) {
    char buf[48];
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "%.24s", m->target);
    canvas_draw_str(canvas, 1, 8, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);
    if(m->join_state == JoinConnecting) {
        canvas_draw_str(canvas, 1, 22, "Connecting...");
        return;
    }
    if(m->join_state == JoinFailed) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 1, 23, "Could not join");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 1, 34, m->join_fail);
        canvas_draw_str(canvas, 1, 63, "Back: networks");
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
    if(m->join_done) canvas_draw_str(canvas, 1, 63, "OK: discovery   Back: leave");
}

static void draw_discovery(Canvas* canvas, Model* m) {
    char buf[72];
    canvas_set_font(canvas, FontSecondary);
    if(m->mdns_done)
        snprintf(
            buf, sizeof(buf), "%u services found%s", (unsigned)m->service_count,
            m->saved ? ", saved" : m->save_failed ? ", NOT saved" : "");
    else
        snprintf(buf, sizeof(buf), "Searching %u/%u: %s", m->mdns_step, m->mdns_steps, m->mdns_label);
    canvas_draw_str(canvas, 1, 8, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);
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

static void draw_cb(Canvas* canvas, void* model) {
    Model* m = model;
    canvas_clear(canvas);
    switch(m->screen) {
    case ScreenMessage:
        canvas_set_font(canvas, FontSecondary);
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
                case ScreenJoin:
                case ScreenDiscovery:
                    send_command(app, "STOP", NULL, NULL);
                    m->screen = m->ap_count ? ScreenSurvey : ScreenMessage;
                    if(!m->ap_count) to_menu = true;
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
                    bool join = m->intent == IntentJoin || m->intent == IntentDiscovery ||
                                (m->intent == IntentSurvey && is_long);
                    if(join) {
                        to_password = begin_join(app, m, ap, m->intent == IntentDiscovery);
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
            } else if(m->screen == ScreenJoin) {
                if(event->key == InputKeyOk && is_short && m->join_done && m->join_state == JoinOk) {
                    m->service_count = m->service_cursor = 0;
                    m->mdns_done = false;
                    m->saved = m->save_failed = false;
                    m->mdns_step = 0;
                    m->screen = ScreenDiscovery;
                    send_command(app, "MDNS", NULL, NULL);
                }
            } else if(m->screen == ScreenDiscovery) {
                if(event->key == InputKeyUp && step && m->service_cursor > 0) m->service_cursor--;
                if(event->key == InputKeyDown && step && m->service_cursor + 1 < m->service_count) m->service_cursor++;
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
    with_view_model(
        app->view,
        Model * m,
        {
            m->intent = (Intent)index;
            start_scan(app, m);
        },
        true);
    view_dispatcher_switch_to_view(app->dispatcher, ViewScreen);
}

static void tick_cb(void* context) {
    App* app = context;
    app->ticks++;
    bool board_ok = false;
    char* report = NULL;
    size_t report_len = 0;
    char report_name[64];
    with_view_model(
        app->view,
        Model * m,
        {
            process_rx(app, m);
            board_ok = m->board_ok;
            if(m->save_request) {
                m->save_request = false;
                report = malloc(REPORT_SIZE);
                report_len = report_build(m, report);
                strlcpy(report_name, m->report_name, sizeof(report_name));
            }
        },
        true);
    if(report) {
        // the SD card write happens outside the lock so the screen keeps drawing
        bool ok = report_write(report_name, report, report_len);
        free(report);
        with_view_model(
            app->view,
            Model * m,
            {
                m->saved = ok;
                m->save_failed = !ok;
            },
            true);
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
    submenu_set_header(app->menu, "Wi-Fi Tools");
    submenu_add_item(app->menu, "Wi-Fi survey", IntentSurvey, menu_cb, app);
    submenu_add_item(app->menu, "Signal walk test", IntentWalk, menu_cb, app);
    submenu_add_item(app->menu, "Join-the-network check", IntentJoin, menu_cb, app);
    submenu_add_item(app->menu, "Discovery check", IntentDiscovery, menu_cb, app);
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
