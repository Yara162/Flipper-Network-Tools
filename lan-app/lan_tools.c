// LAN Tools: wired network checks for the Flipper Zero with a W5500 Ethernet module.
// This first version shows the link state and runs a join-the-network check: DHCP,
// gateway ping, DNS lookup and internet ping, with a saved report.
//
// Wiring (W5500 module -> Flipper GPIO header):
//   3.3V -> pin 9  (3V3)        MOSI -> pin 2 (A7)
//   GND  -> pin 8  (GND)        MISO -> pin 3 (A6)
//   SCLK -> pin 5  (B3)         SCS  -> pin 4 (A4)
//   RST and INT: leave unconnected
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <datetime/datetime.h>
#include "net.h"

#define DHCP_TIMEOUT_MS 15000

typedef enum { JobCheck, JobExit } Job;

typedef enum { CheckIdle, CheckDhcp, CheckGateway, CheckDns, CheckInternet, CheckDone, CheckFailed } CheckState;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input_queue;
    FuriMessageQueue* job_queue;
    ViewPort* view_port;
    // everything below is shared between the worker and the screen
    bool chip_ok;
    NetLink link;
    NetConfig config;
    CheckState state;
    const char* failure;
    uint32_t dhcp_ms, dns_ms;
    int ping_gateway, ping_internet;
    bool dns_ok, saved;
    char file_name[40];
} App;

static int prefix_length(const uint8_t mask[4]) {
    int bits = 0;
    for(int i = 0; i < 4; i++)
        for(int b = 7; b >= 0 && (mask[i] >> b) & 1; b--) bits++;
    return bits;
}

static void set_state(App* app, CheckState state) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->state = state;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static void save_report(App* app) {
    DateTime t;
    furi_hal_rtc_get_datetime(&t);
    const NetConfig* c = &app->config;
    char* text = malloc(1024);
    int n = snprintf(
        text, 1024,
        "LAN Tools: join check\nDate: %04u-%02u-%02u %02u:%02u:%02u\n\n"
        "Link: %s Mbit/s, %s duplex\nThis device: %02X:%02X:%02X:%02X:%02X:%02X\n\n"
        "DHCP answered in %lu.%lu s\n  IP address: %u.%u.%u.%u/%d\n  Gateway: %u.%u.%u.%u",
        t.year, t.month, t.day, t.hour, t.minute, t.second, app->link.speed_100 ? "100" : "10",
        app->link.full_duplex ? "full" : "half", c->mac[0], c->mac[1], c->mac[2], c->mac[3], c->mac[4], c->mac[5],
        (unsigned long)(app->dhcp_ms / 1000), (unsigned long)(app->dhcp_ms % 1000 / 100), c->ip[0], c->ip[1], c->ip[2],
        c->ip[3], prefix_length(c->mask), c->gateway[0], c->gateway[1], c->gateway[2], c->gateway[3]);
    if(app->ping_gateway >= 0)
        n += snprintf(text + n, 1024 - (size_t)n, " (ping %d ms)\n", app->ping_gateway);
    else
        n += snprintf(text + n, 1024 - (size_t)n, " (no ping reply)\n");
    n += snprintf(
        text + n, 1024 - (size_t)n, "  DNS server: %u.%u.%u.%u (name lookup %s)\n", c->dns[0], c->dns[1], c->dns[2],
        c->dns[3], app->dns_ok ? "worked" : "failed");
    if(app->ping_internet >= 0)
        n += snprintf(text + n, 1024 - (size_t)n, "  Internet: reachable (ping %d ms)\n", app->ping_internet);
    else
        n += snprintf(text + n, 1024 - (size_t)n, "  Internet: not reachable\n");

    snprintf(
        app->file_name, sizeof(app->file_name), "lan_%04u%02u%02u_%02u%02u%02u.txt", t.year, t.month, t.day, t.hour,
        t.minute, t.second);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc_printf("%s/%s", STORAGE_APP_DATA_PATH_PREFIX, app->file_name);
    storage_common_resolve_path_and_ensure_app_directory(storage, path);
    File* file = storage_file_alloc(storage);
    app->saved = storage_file_open(file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                 storage_file_write(file, text, (size_t)n) == (size_t)n;
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    free(text);
}

static void run_check(App* app) {
    NetConfig config = app->config;
    NetLink link;
    net_link(&link);
    if(!link.up) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->failure = "No link: plug in a cable";
        app->state = CheckFailed;
        furi_mutex_release(app->mutex);
        return;
    }
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->ping_gateway = app->ping_internet = -3;
    app->dns_ok = false;
    app->saved = false;
    furi_mutex_release(app->mutex);

    set_state(app, CheckDhcp);
    uint32_t took = 0;
    bool leased = net_dhcp(&config, DHCP_TIMEOUT_MS, &took);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->config = config;
    app->dhcp_ms = took;
    if(!leased) {
        app->failure = "No DHCP answer";
        app->state = CheckFailed;
    }
    furi_mutex_release(app->mutex);
    if(!leased) return;

    set_state(app, CheckGateway);
    int ms = net_ping(&config, config.gateway, 3, 1000);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->ping_gateway = ms;
    furi_mutex_release(app->mutex);

    set_state(app, CheckDns);
    uint8_t resolved[4];
    bool dns_ok = net_dns(&config, "example.com", resolved, &took);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->dns_ok = dns_ok;
    app->dns_ms = took;
    furi_mutex_release(app->mutex);

    set_state(app, CheckInternet);
    static const uint8_t public_ip[4] = {1, 1, 1, 1};
    ms = net_ping(&config, public_ip, 3, 1500);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->ping_internet = ms;
    furi_mutex_release(app->mutex);

    save_report(app);
    set_state(app, CheckDone);
}

// All access to the chip happens on this thread
static int32_t worker(void* context) {
    App* app = context;
    bool chip_ok = false;
    while(true) {
        Job job;
        FuriStatus status = furi_message_queue_get(app->job_queue, &job, furi_ms_to_ticks(500));
        if(status == FuriStatusOk && job == JobExit) break;

        if(!chip_ok) {
            NetConfig config;
            chip_ok = net_start(&config);
            if(!chip_ok) net_stop();
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->chip_ok = chip_ok;
            if(chip_ok) app->config = config;
            furi_mutex_release(app->mutex);
        }
        if(chip_ok) {
            NetLink link;
            net_link(&link);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->link = link;
            furi_mutex_release(app->mutex);
            if(status == FuriStatusOk && job == JobCheck) run_check(app);
        }
        view_port_update(app->view_port);
    }
    if(chip_ok) net_stop();
    return 0;
}

static void draw_cb(Canvas* canvas, void* context) {
    App* app = context;
    char buf[48];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    const NetConfig* c = &app->config;

    if(!app->chip_ok) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "LAN Tools");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 126, 10, AlignRight, AlignBottom, "W5500 > Flipper");
        canvas_draw_line(canvas, 0, 12, 127, 12);
        canvas_draw_str(canvas, 2, 22, "3.3V > pin 9    GND > pin 8");
        canvas_draw_str(canvas, 2, 31, "MOSI > pin 2   MISO > pin 3");
        canvas_draw_str(canvas, 2, 40, "SCLK > pin 5   SCS > pin 4");
        canvas_draw_str(canvas, 2, 49, "RST, INT: not connected");
        canvas_draw_line(canvas, 0, 52, 127, 52);
        canvas_draw_str(canvas, 2, 62, "Looking for the module...");
        furi_mutex_release(app->mutex);
        return;
    }

    if(app->link.up)
        snprintf(
            buf, sizeof(buf), "Link %s Mbit/s %s", app->link.speed_100 ? "100" : "10",
            app->link.full_duplex ? "full" : "half");
    else
        snprintf(buf, sizeof(buf), "No link (no cable?)");
    canvas_draw_str(canvas, 1, 8, buf);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    if(app->state == CheckIdle) {
        canvas_draw_str(canvas, 1, 24, "Join-the-network check:");
        canvas_draw_str(canvas, 1, 33, "DHCP, gateway, DNS, internet");
        canvas_draw_str(canvas, 1, 63, "OK: run the check");
    } else if(app->state == CheckFailed) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 1, 24, "Check failed");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 1, 35, app->failure ? app->failure : "");
        canvas_draw_str(canvas, 1, 63, "OK: try again");
    } else if(app->state == CheckDhcp) {
        canvas_draw_str(canvas, 1, 24, "Asking for an address (DHCP)...");
    } else {
        snprintf(
            buf, sizeof(buf), "DHCP %lu.%lus", (unsigned long)(app->dhcp_ms / 1000),
            (unsigned long)(app->dhcp_ms % 1000 / 100));
        canvas_draw_str_aligned(canvas, 127, 8, AlignRight, AlignBottom, buf);
        snprintf(buf, sizeof(buf), "IP %u.%u.%u.%u/%d", c->ip[0], c->ip[1], c->ip[2], c->ip[3], prefix_length(c->mask));
        canvas_draw_str(canvas, 1, 19, buf);
        int n = snprintf(buf, sizeof(buf), "GW %u.%u.%u.%u ", c->gateway[0], c->gateway[1], c->gateway[2], c->gateway[3]);
        if(app->ping_gateway == -3)
            snprintf(buf + n, sizeof(buf) - (size_t)n, "...");
        else if(app->ping_gateway < 0)
            snprintf(buf + n, sizeof(buf) - (size_t)n, "no reply");
        else
            snprintf(buf + n, sizeof(buf) - (size_t)n, "%d ms", app->ping_gateway);
        canvas_draw_str(canvas, 1, 28, buf);
        n = snprintf(buf, sizeof(buf), "DNS %u.%u.%u.%u ", c->dns[0], c->dns[1], c->dns[2], c->dns[3]);
        if(app->state <= CheckDns)
            snprintf(buf + n, sizeof(buf) - (size_t)n, "...");
        else
            snprintf(buf + n, sizeof(buf) - (size_t)n, "%s", app->dns_ok ? "ok" : "FAIL");
        canvas_draw_str(canvas, 1, 37, buf);
        if(app->ping_internet == -3)
            snprintf(buf, sizeof(buf), "Internet ...");
        else if(app->ping_internet < 0)
            snprintf(buf, sizeof(buf), "Internet none");
        else
            snprintf(buf, sizeof(buf), "Internet %d ms", app->ping_internet);
        canvas_draw_str(canvas, 1, 46, buf);
        if(app->state == CheckDone) {
            canvas_draw_str(canvas, 1, 55, app->saved ? app->file_name : "Could not save the report");
            canvas_draw_str(canvas, 1, 63, "OK: run again");
        }
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent* event, void* context) {
    App* app = context;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

int32_t lan_tools_app(void* p) {
    UNUSED(p);
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->job_queue = furi_message_queue_alloc(4, sizeof(Job));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, draw_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    FuriThread* thread = furi_thread_alloc_ex("LanToolsWorker", 4096, worker, app);
    furi_thread_start(thread);

    bool running = true;
    while(running) {
        InputEvent event;
        if(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) != FuriStatusOk) continue;
        if(event.type != InputTypeShort) continue;
        if(event.key == InputKeyBack) {
            running = false;
        } else if(event.key == InputKeyOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            bool busy = app->state == CheckDhcp || app->state == CheckGateway || app->state == CheckDns ||
                        app->state == CheckInternet;
            bool ready = app->chip_ok && !busy;
            furi_mutex_release(app->mutex);
            if(ready) {
                Job job = JobCheck;
                furi_message_queue_put(app->job_queue, &job, 0);
            }
        }
    }

    Job job = JobExit;
    furi_message_queue_put(app->job_queue, &job, FuriWaitForever);
    furi_thread_join(thread);
    furi_thread_free(thread);
    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->job_queue);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
