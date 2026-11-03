#include <kernel/e1000.h>
#include <kernel/net.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <string.h>

#define NET_IP_ADDRESS 0x0A00020FU
#define NET_SUBNET_MASK 0xFFFFFF00U
#define NET_GATEWAY 0x0A000202U
#define NET_ARP_CACHE_SIZE 8
#define NET_UDP_QUEUE_SIZE 4
#define NET_FRAME_MAX 1536
#define NET_ARP_WAIT_TICKS 200
#define NET_UDP_SOURCE_PORT 49152

typedef struct {
    uint32_t ip;
    uint8_t mac[6];
    int valid;
} arp_entry_t;

static arp_entry_t arp_cache[NET_ARP_CACHE_SIZE];
static uint32_t arp_next_entry;
static net_udp_packet_t udp_queue[NET_UDP_QUEUE_SIZE];
static uint32_t udp_head;
static uint32_t udp_count;
static uint16_t next_ip_id;
static uint32_t received_frames;
static uint32_t dropped_udp_packets;
static int initialized;

static uint16_t read_be16(const uint8_t* data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) | data[3];
}

static void write_be16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static uint32_t checksum_add(uint32_t sum, const uint8_t* data,
                             uint32_t length) {
    while (length >= 2) {
        sum += read_be16(data);
        data += 2;
        length -= 2;
    }
    if (length) {
        sum += (uint16_t)data[0] << 8;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t ipv4_checksum(const uint8_t* header, uint32_t length) {
    return checksum_finish(checksum_add(0, header, length));
}

static uint32_t udp_checksum_sum(uint32_t source_ip, uint32_t destination_ip,
                                 const uint8_t* udp, uint16_t length) {
    uint8_t pseudo_header[12];
    write_be32(pseudo_header, source_ip);
    write_be32(pseudo_header + 4, destination_ip);
    pseudo_header[8] = 0;
    pseudo_header[9] = 17;
    write_be16(pseudo_header + 10, length);
    return checksum_add(checksum_add(0, pseudo_header, sizeof(pseudo_header)),
                        udp, length);
}

int net_parse_ipv4(const char* text, uint32_t* address) {
    if (!text || !address) {
        return 0;
    }

    uint32_t result = 0;
    for (uint32_t part = 0; part < 4; part++) {
        if (*text < '0' || *text > '9') {
            return 0;
        }
        uint32_t value = 0;
        uint32_t digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (uint32_t)(*text - '0');
            if (value > 255 || ++digits > 3) {
                return 0;
            }
            text++;
        }
        result = (result << 8) | value;
        if (part != 3) {
            if (*text++ != '.') {
                return 0;
            }
        } else if (*text) {
            return 0;
        }
    }
    *address = result;
    return 1;
}

void net_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(udp_queue, 0, sizeof(udp_queue));
    arp_next_entry = 0;
    udp_head = 0;
    udp_count = 0;
    next_ip_id = 1;
    received_frames = 0;
    dropped_udp_packets = 0;
    initialized = e1000_init();
}

int net_is_available(void) {
    return initialized && e1000_is_available();
}

const char* net_driver_name(void) {
    return net_is_available() ? "Intel e1000" : "not detected";
}

const uint8_t* net_mac_address(void) {
    return net_is_available() ? e1000_mac_address() : 0;
}

uint32_t net_ipv4_address(void) {
    return NET_IP_ADDRESS;
}

uint32_t net_received_frame_count(void) {
    return received_frames;
}

uint32_t net_arp_cache_count(void) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        count += arp_cache[i].valid != 0;
    }
    return count;
}

uint32_t net_udp_dropped_count(void) {
    return dropped_udp_packets;
}

static void arp_cache_insert(uint32_t ip, const uint8_t* mac) {
    if (!ip || !mac || (mac[0] & 1)) {
        return;
    }
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, sizeof(arp_cache[i].mac));
            return;
        }
    }
    arp_entry_t* entry = &arp_cache[arp_next_entry];
    entry->ip = ip;
    memcpy(entry->mac, mac, sizeof(entry->mac));
    entry->valid = 1;
    arp_next_entry = (arp_next_entry + 1) % NET_ARP_CACHE_SIZE;
}

static const uint8_t* arp_cache_find(uint32_t ip) {
    for (uint32_t i = 0; i < NET_ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return arp_cache[i].mac;
        }
    }
    return 0;
}

static int send_arp(uint16_t operation, const uint8_t* destination_mac,
                    uint32_t destination_ip) {
    const uint8_t* own_mac = e1000_mac_address();
    if (!own_mac) {
        return 0;
    }
    uint8_t frame[42] = {0};
    memcpy(frame, destination_mac, 6);
    memcpy(frame + 6, own_mac, 6);
    write_be16(frame + 12, 0x0806);
    write_be16(frame + 14, 1);
    write_be16(frame + 16, 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    write_be16(frame + 20, operation);
    memcpy(frame + 22, own_mac, 6);
    write_be32(frame + 28, NET_IP_ADDRESS);
    if (operation == 2 && destination_mac) {
        memcpy(frame + 32, destination_mac, 6);
    }
    write_be32(frame + 38, destination_ip);
    return e1000_send(frame, sizeof(frame));
}

static void process_arp(const uint8_t* frame, uint16_t length) {
    if (length < 42 || read_be16(frame + 14) != 1 ||
        read_be16(frame + 16) != 0x0800 ||
        frame[18] != 6 || frame[19] != 4) {
        return;
    }
    uint16_t operation = read_be16(frame + 20);
    if (operation != 1 && operation != 2) {
        return;
    }
    const uint8_t* sender_mac = frame + 22;
    uint32_t sender_ip = read_be32(frame + 28);
    uint32_t target_ip = read_be32(frame + 38);
    arp_cache_insert(sender_ip, sender_mac);
    if (operation == 1 && target_ip == NET_IP_ADDRESS) {
        (void)send_arp(2, sender_mac, sender_ip);
    }
}

static int ipv4_header_valid(const uint8_t* header, uint16_t available_length,
                             uint16_t* header_length, uint16_t* total_length) {
    if (available_length < 20 || (header[0] >> 4) != 4) {
        return 0;
    }
    uint16_t size = (uint16_t)(header[0] & 0x0F) * 4;
    uint16_t total = read_be16(header + 2);
    if (size < 20 || size > available_length || total < size ||
        total > available_length || ipv4_checksum(header, size) != 0) {
        return 0;
    }
    if (read_be16(header + 6) & 0x3FFF) {
        return 0;
    }
    *header_length = size;
    *total_length = total;
    return 1;
}

static void queue_udp(uint32_t source_ip, uint16_t source_port,
                      uint16_t destination_port, const uint8_t* payload,
                      uint16_t length) {
    if (length > NET_UDP_MAX_PAYLOAD ||
        udp_count == NET_UDP_QUEUE_SIZE) {
        dropped_udp_packets++;
        return;
    }
    uint32_t tail = (udp_head + udp_count) % NET_UDP_QUEUE_SIZE;
    net_udp_packet_t* packet = &udp_queue[tail];
    packet->source_ip = source_ip;
    packet->source_port = source_port;
    packet->destination_port = destination_port;
    packet->length = length;
    if (length) {
        memcpy(packet->payload, payload, length);
    }
    udp_count++;
}

static void process_ipv4(const uint8_t* frame, uint16_t length) {
    if (length < 34 || frame[23] != 17) {
        return;
    }
    uint16_t ip_header_length;
    uint16_t ip_total_length;
    if (!ipv4_header_valid(frame + 14, length - 14,
                           &ip_header_length, &ip_total_length)) {
        return;
    }

    const uint8_t* ip = frame + 14;
    uint32_t source_ip = read_be32(ip + 12);
    uint32_t destination_ip = read_be32(ip + 16);
    if (destination_ip != NET_IP_ADDRESS &&
        destination_ip != 0xFFFFFFFFU) {
        return;
    }
    const uint8_t* udp = ip + ip_header_length;
    uint16_t udp_length = read_be16(udp + 4);
    if (udp_length < 8 || udp_length > ip_total_length - ip_header_length) {
        return;
    }
    uint16_t udp_checksum = read_be16(udp + 6);
    if (udp_checksum && checksum_finish(udp_checksum_sum(
            source_ip, destination_ip, udp, udp_length)) != 0) {
        return;
    }
    arp_cache_insert(source_ip, frame + 6);
    queue_udp(source_ip, read_be16(udp), read_be16(udp + 2),
              udp + 8, (uint16_t)(udp_length - 8));
}

void net_poll(void) {
    if (!net_is_available()) {
        return;
    }
    uint8_t frame[NET_FRAME_MAX];
    for (uint32_t count = 0; count < 16; count++) {
        int length = e1000_receive(frame, sizeof(frame));
        if (length == 0) {
            break;
        }
        if (length < 0 || length < 14) {
            continue;
        }
        received_frames++;
        uint16_t type = read_be16(frame + 12);
        if (type == 0x0806) {
            process_arp(frame, (uint16_t)length);
        } else if (type == 0x0800) {
            process_ipv4(frame, (uint16_t)length);
        }
    }
}

static int arp_resolve(uint32_t ip, uint8_t* mac) {
    const uint8_t* cached = arp_cache_find(ip);
    if (cached) {
        memcpy(mac, cached, 6);
        return 1;
    }

    static const uint8_t broadcast[6] =
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (!send_arp(1, broadcast, ip)) {
        return 0;
    }

    uint32_t start = timer_get_ticks();
    while ((uint32_t)(timer_get_ticks() - start) < NET_ARP_WAIT_TICKS) {
        net_poll();
        cached = arp_cache_find(ip);
        if (cached) {
            memcpy(mac, cached, 6);
            return 1;
        }
        task_yield();
    }
    return 0;
}

int net_udp_send(uint32_t destination_ip, uint16_t destination_port,
                 const void* payload, uint16_t length) {
    if (!net_is_available() || !destination_ip || !destination_port ||
        (length && !payload) || length > NET_UDP_MAX_PAYLOAD) {
        return 0;
    }

    uint32_t next_hop = (destination_ip & NET_SUBNET_MASK) ==
        (NET_IP_ADDRESS & NET_SUBNET_MASK) ? destination_ip : NET_GATEWAY;
    uint8_t destination_mac[6];
    if (!arp_resolve(next_hop, destination_mac)) {
        return 0;
    }

    uint8_t frame[14 + 20 + 8 + NET_UDP_MAX_PAYLOAD] = {0};
    const uint8_t* own_mac = e1000_mac_address();
    memcpy(frame, destination_mac, 6);
    memcpy(frame + 6, own_mac, 6);
    write_be16(frame + 12, 0x0800);

    uint8_t* ip = frame + 14;
    ip[0] = 0x45;
    write_be16(ip + 2, (uint16_t)(20 + 8 + length));
    write_be16(ip + 4, next_ip_id++);
    write_be16(ip + 6, 0x4000);
    ip[8] = 64;
    ip[9] = 17;
    write_be32(ip + 12, NET_IP_ADDRESS);
    write_be32(ip + 16, destination_ip);
    write_be16(ip + 10, ipv4_checksum(ip, 20));

    uint8_t* udp = ip + 20;
    write_be16(udp, NET_UDP_SOURCE_PORT);
    write_be16(udp + 2, destination_port);
    uint16_t udp_length = (uint16_t)(8 + length);
    write_be16(udp + 4, udp_length);
    if (length) {
        memcpy(udp + 8, payload, length);
    }
    uint16_t udp_checksum = checksum_finish(udp_checksum_sum(
        NET_IP_ADDRESS, destination_ip, udp, udp_length));
    write_be16(udp + 6, udp_checksum ? udp_checksum : 0xFFFF);
    return e1000_send(frame, (uint16_t)(14 + 20 + udp_length));
}

int net_udp_receive(net_udp_packet_t* packet) {
    if (!packet || !udp_count) {
        return 0;
    }
    *packet = udp_queue[udp_head];
    udp_head = (udp_head + 1) % NET_UDP_QUEUE_SIZE;
    udp_count--;
    return 1;
}
