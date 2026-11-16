#include "net.h"
#include <furi.h>
#include <furi_hal.h>
#include <string.h>
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"
#include "dns.h"

#define SPI (&furi_hal_spi_bus_handle_external)
#define SPI_TIMEOUT 100

// Socket use on the chip
#define SOCK_RAW 0 // raw Ethernet frames: only socket 0 can do this
#define SOCK_DHCP 1
#define SOCK_DNS 2

#define ETH_TYPE_IP 0x0800
#define ETH_TYPE_ARP 0x0806
#define FRAME_MAX 1514

static FuriTimer* second_timer;
static uint8_t* work_buf; // DHCP and DNS message buffer
static uint8_t* frame_buf; // raw frame buffer
static bool raw_open;

// --- SPI glue for the driver library ---

static void cs_select(void) {
    furi_hal_spi_acquire(SPI);
}

static void cs_deselect(void) {
    furi_hal_spi_release(SPI);
}

static uint8_t spi_read_byte(void) {
    uint8_t byte = 0;
    furi_hal_spi_bus_rx(SPI, &byte, 1, SPI_TIMEOUT);
    return byte;
}

static void spi_write_byte(uint8_t byte) {
    furi_hal_spi_bus_tx(SPI, &byte, 1, SPI_TIMEOUT);
}

static void spi_read_burst(uint8_t* buf, uint16_t len) {
    furi_hal_spi_bus_rx(SPI, buf, len, SPI_TIMEOUT);
}

static void spi_write_burst(uint8_t* buf, uint16_t len) {
    furi_hal_spi_bus_tx(SPI, buf, len, SPI_TIMEOUT);
}

// The library's DHCP and DNS code count seconds through these
static void second_tick(void* context) {
    UNUSED(context);
    DHCP_time_handler();
    DNS_time_handler();
}

static uint32_t now_ms(void) {
    return furi_get_tick() * 1000 / furi_kernel_get_tick_frequency();
}

// --- chip ---

bool net_start(NetConfig* config) {
    memset(config, 0, sizeof(*config));
    furi_hal_spi_bus_handle_init(SPI);
    reg_wizchip_cs_cbfunc(cs_select, cs_deselect);
    reg_wizchip_spi_cbfunc(spi_read_byte, spi_write_byte);
    reg_wizchip_spiburst_cbfunc(spi_read_burst, spi_write_burst);

    // software reset, then check that it really is a W5500 (version register reads 4)
    setMR(MR_RST);
    furi_delay_ms(50);
    if(getVERSIONR() != 0x04) return false;

    uint8_t sizes[8] = {4, 2, 2, 2, 2, 2, 1, 1}; // kB per socket, 16 in total
    if(wizchip_init(sizes, sizes) != 0) return false;
    // Give up on an unanswered address lookup after about half a second instead of two.
    // This also shortens the duplicate-address check the DHCP client makes.
    setRTR(1000); // 100 ms
    setRCR(3);

    // The chip has no address of its own: derive a private one from this Flipper's ID
    const uint8_t* uid = furi_hal_version_uid();
    size_t uid_size = furi_hal_version_uid_size();
    config->mac[0] = 0x02; // locally administered
    for(size_t i = 0; i < 5; i++) config->mac[1 + i] = uid[uid_size - 5 + i];
    setSHAR(config->mac);

    work_buf = malloc(1024);
    frame_buf = malloc(FRAME_MAX);
    second_timer = furi_timer_alloc(second_tick, FuriTimerTypePeriodic, NULL);
    furi_timer_start(second_timer, furi_kernel_get_tick_frequency());
    raw_open = false;
    return true;
}

void net_stop(void) {
    if(second_timer) {
        furi_timer_stop(second_timer);
        furi_timer_free(second_timer);
        second_timer = NULL;
    }
    free(work_buf);
    free(frame_buf);
    work_buf = frame_buf = NULL;
    furi_hal_spi_bus_handle_deinit(SPI);
}

void net_link(NetLink* link) {
    uint8_t phy = getPHYCFGR();
    link->up = phy & PHYCFGR_LNK_ON;
    link->speed_100 = phy & PHYCFGR_SPD_100;
    link->full_duplex = phy & PHYCFGR_DPX_FULL;
}

// --- DHCP and DNS (ioLibrary) ---

bool net_dhcp(NetConfig* config, uint32_t timeout_ms, uint32_t* took_ms) {
    uint8_t zero[4] = {0, 0, 0, 0};
    setSIPR(zero);
    setGAR(zero);
    setSUBR(zero);
    raw_open = false; // DHCP_init resets the chip's sockets it needs; reopen raw afterwards
    setSn_CR(SOCK_RAW, Sn_CR_CLOSE);

    uint32_t start = now_ms();
    DHCP_init(SOCK_DHCP, work_buf);
    bool leased = false;
    while(now_ms() - start < timeout_ms) {
        uint8_t state = DHCP_run();
        if(state == DHCP_IP_LEASED || state == DHCP_IP_ASSIGN || state == DHCP_IP_CHANGED) {
            leased = true;
            break;
        }
        if(state == DHCP_FAILED) break;
        furi_delay_ms(10);
    }
    *took_ms = now_ms() - start;
    if(!leased) {
        DHCP_stop();
        return false;
    }
    getIPfromDHCP(config->ip);
    getGWfromDHCP(config->gateway);
    getSNfromDHCP(config->mask);
    getDNSfromDHCP(config->dns);
    // make sure the chip itself uses what was leased
    setSIPR(config->ip);
    setGAR(config->gateway);
    setSUBR(config->mask);
    return true;
}

bool net_dns(const NetConfig* config, const char* name, uint8_t ip[4], uint32_t* took_ms) {
    uint8_t dns[4];
    memcpy(dns, config->dns, 4);
    uint32_t start = now_ms();
    DNS_init(SOCK_DNS, work_buf);
    int8_t result = DNS_run(dns, (uint8_t*)name, ip);
    *took_ms = now_ms() - start;
    return result == 1;
}

// --- raw Ethernet frames on socket 0 ---

static bool wait_command(uint8_t sn) {
    for(int i = 0; i < 1000 && getSn_CR(sn); i++) furi_delay_us(100);
    return getSn_CR(sn) == 0;
}

static bool raw_start(void) {
    if(raw_open) return true;
    setSn_CR(SOCK_RAW, Sn_CR_CLOSE);
    wait_command(SOCK_RAW);
    setSn_IR(SOCK_RAW, 0xFF);
    // MAC filter on: only frames for us, broadcast and multicast
    setSn_MR(SOCK_RAW, Sn_MR_MACRAW | Sn_MR_MFEN);
    setSn_CR(SOCK_RAW, Sn_CR_OPEN);
    wait_command(SOCK_RAW);
    raw_open = getSn_SR(SOCK_RAW) == SOCK_MACRAW;
    return raw_open;
}

static bool raw_send(const uint8_t* frame, uint16_t len) {
    if(getSn_TX_FSR(SOCK_RAW) < len) return false;
    wiz_send_data(SOCK_RAW, (uint8_t*)frame, len);
    setSn_CR(SOCK_RAW, Sn_CR_SEND);
    if(!wait_command(SOCK_RAW)) return false;
    for(int i = 0; i < 500; i++) {
        uint8_t ir = getSn_IR(SOCK_RAW);
        if(ir & Sn_IR_SENDOK) {
            setSn_IR(SOCK_RAW, Sn_IR_SENDOK);
            return true;
        }
        if(ir & Sn_IR_TIMEOUT) {
            setSn_IR(SOCK_RAW, Sn_IR_TIMEOUT);
            return false;
        }
        furi_delay_us(200);
    }
    return false;
}

// Reads one waiting frame into frame_buf. Returns its length, or 0 if nothing is waiting.
static uint16_t raw_receive(void) {
    if(getSn_RX_RSR(SOCK_RAW) < 2) return 0;
    uint8_t head[2];
    wiz_recv_data(SOCK_RAW, head, 2);
    setSn_CR(SOCK_RAW, Sn_CR_RECV);
    wait_command(SOCK_RAW);
    uint16_t len = (uint16_t)((head[0] << 8) | head[1]);
    if(len < 2) return 0;
    len -= 2; // the length counts its own two bytes
    if(len > FRAME_MAX) {
        // should not happen; drop it and resynchronise by reopening
        raw_open = false;
        raw_start();
        return 0;
    }
    wiz_recv_data(SOCK_RAW, frame_buf, len);
    setSn_CR(SOCK_RAW, Sn_CR_RECV);
    wait_command(SOCK_RAW);
    return len;
}

static void raw_flush(void) {
    for(int i = 0; i < 64 && raw_receive(); i++) {
    }
}

static uint16_t checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    for(size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)((data[i] << 8) | data[i + 1]);
    if(len & 1) sum += (uint32_t)(data[len - 1] << 8);
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// Find the hardware address that owns an IP address on this network
static bool arp_resolve(const NetConfig* config, const uint8_t ip[4], uint8_t mac[6]) {
    uint8_t request[42];
    memset(request, 0xFF, 6); // to everyone
    memcpy(request + 6, config->mac, 6);
    request[12] = 0x08;
    request[13] = 0x06;
    static const uint8_t arp_head[8] = {0x00, 0x01, 0x08, 0x00, 6, 4, 0x00, 0x01}; // Ethernet, IPv4, request
    memcpy(request + 14, arp_head, 8);
    memcpy(request + 22, config->mac, 6);
    memcpy(request + 28, config->ip, 4);
    memset(request + 32, 0, 6);
    memcpy(request + 38, ip, 4);

    for(int attempt = 0; attempt < 3; attempt++) {
        raw_flush();
        if(!raw_send(request, sizeof(request))) continue;
        uint32_t start = now_ms();
        while(now_ms() - start < 400) {
            uint16_t len = raw_receive();
            if(len == 0) {
                furi_delay_ms(1);
                continue;
            }
            const uint8_t* f = frame_buf;
            // an ARP reply whose sender address is the one we asked about
            if(len >= 42 && f[12] == 0x08 && f[13] == 0x06 && f[20] == 0x00 && f[21] == 0x02 &&
               memcmp(f + 28, ip, 4) == 0) {
                memcpy(mac, f + 22, 6);
                return true;
            }
        }
    }
    return false;
}

static bool same_subnet(const NetConfig* config, const uint8_t ip[4]) {
    for(int i = 0; i < 4; i++)
        if((ip[i] & config->mask[i]) != (config->ip[i] & config->mask[i])) return false;
    return true;
}

int net_ping(const NetConfig* config, const uint8_t ip[4], uint8_t count, uint32_t timeout_ms) {
    if(!raw_start()) return -1;
    // off-subnet targets go through the gateway
    uint8_t next_mac[6];
    if(!arp_resolve(config, same_subnet(config, ip) ? ip : config->gateway, next_mac)) return -2;

    static uint16_t sequence;
    const uint16_t id = 0x464C; // "FL"
    uint32_t total_ms = 0, replies = 0;
    for(uint8_t n = 0; n < count; n++) {
        uint8_t frame[74];
        uint16_t seq = ++sequence;
        memcpy(frame, next_mac, 6);
        memcpy(frame + 6, config->mac, 6);
        frame[12] = 0x08;
        frame[13] = 0x00;
        uint8_t* iph = frame + 14;
        memset(iph, 0, 20);
        iph[0] = 0x45;
        iph[2] = 0;
        iph[3] = 60; // total length: 20 + 8 + 32
        iph[4] = (uint8_t)(seq >> 8);
        iph[5] = (uint8_t)seq;
        iph[8] = 64; // time to live
        iph[9] = 1; // ICMP
        memcpy(iph + 12, config->ip, 4);
        memcpy(iph + 16, ip, 4);
        uint16_t sum = checksum(iph, 20);
        iph[10] = (uint8_t)(sum >> 8);
        iph[11] = (uint8_t)sum;
        uint8_t* icmp = frame + 34;
        icmp[0] = 8; // echo request
        icmp[1] = 0;
        icmp[2] = icmp[3] = 0;
        icmp[4] = (uint8_t)(id >> 8);
        icmp[5] = (uint8_t)id;
        icmp[6] = (uint8_t)(seq >> 8);
        icmp[7] = (uint8_t)seq;
        for(int i = 0; i < 32; i++) icmp[8 + i] = (uint8_t)('a' + i % 23);
        sum = checksum(icmp, 40);
        icmp[2] = (uint8_t)(sum >> 8);
        icmp[3] = (uint8_t)sum;

        raw_flush();
        uint32_t start = now_ms();
        if(!raw_send(frame, sizeof(frame))) continue;
        bool got = false;
        while(!got && now_ms() - start < timeout_ms) {
            uint16_t len = raw_receive();
            if(len == 0) {
                furi_delay_ms(1);
                continue;
            }
            const uint8_t* f = frame_buf;
            if(len < 42 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 1) continue;
            size_t ihl = (size_t)(f[14] & 0x0F) * 4;
            const uint8_t* r = f + 14 + ihl;
            if(14 + ihl + 8 > len) continue;
            // echo reply from the target carrying our id and sequence number
            if(r[0] == 0 && memcmp(f + 26, ip, 4) == 0 && r[4] == (uint8_t)(id >> 8) && r[5] == (uint8_t)id &&
               r[6] == (uint8_t)(seq >> 8) && r[7] == (uint8_t)seq)
                got = true;
        }
        if(got) {
            total_ms += now_ms() - start;
            replies++;
        }
        if(n + 1 < count) furi_delay_ms(100);
    }
    return replies ? (int)(total_ms / replies) : -1;
}
