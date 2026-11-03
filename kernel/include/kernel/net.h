#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

#include <stdint.h>

#define NET_UDP_MAX_PAYLOAD 256

typedef struct {
    uint32_t source_ip;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
    uint8_t payload[NET_UDP_MAX_PAYLOAD];
} net_udp_packet_t;

void net_init(void);
void net_poll(void);
int net_is_available(void);
const char* net_driver_name(void);
const uint8_t* net_mac_address(void);
uint32_t net_ipv4_address(void);
uint32_t net_received_frame_count(void);
uint32_t net_arp_cache_count(void);
uint32_t net_udp_dropped_count(void);
int net_parse_ipv4(const char* text, uint32_t* address);
int net_udp_send(uint32_t destination_ip, uint16_t destination_port,
                 const void* payload, uint16_t length);
int net_udp_receive(net_udp_packet_t* packet);

#endif
