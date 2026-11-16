// Wired network layer for the W5500 Ethernet chip: chip setup, link state, DHCP, DNS,
// and ping. DHCP and DNS use WIZnet's ioLibrary; ping and address lookup are done with
// raw Ethernet frames, because the W5500's built-in networking cannot send pings.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t mac[6];
    uint8_t ip[4], mask[4], gateway[4], dns[4];
} NetConfig;

typedef struct {
    bool up;
    bool speed_100; // otherwise 10 Mbit/s
    bool full_duplex;
} NetLink;

// Powers up the SPI bus and resets the chip. False if no W5500 answers.
bool net_start(NetConfig* config);
void net_stop(void);

void net_link(NetLink* link);

// Asks a DHCP server for an address and applies it. Fills `config` and the time taken.
bool net_dhcp(NetConfig* config, uint32_t timeout_ms, uint32_t* took_ms);

// Looks a name up at the given DNS server.
bool net_dns(const NetConfig* config, const char* name, uint8_t ip[4], uint32_t* took_ms);

// Sends `count` pings. Returns the average round trip in ms, -1 if none came back,
// or -2 if the next hop (the target or the gateway) did not answer an address lookup.
int net_ping(const NetConfig* config, const uint8_t ip[4], uint8_t count, uint32_t timeout_ms);
