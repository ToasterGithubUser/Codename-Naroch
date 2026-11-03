#ifndef _KERNEL_PCI_H
#define _KERNEL_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 256

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;
    uint16_t vendor_id;
    uint16_t device_id;
} pci_device_t;

void pci_init(void);
uint32_t pci_device_count(void);
int pci_device_list_truncated(void);
const pci_device_t* pci_device_at(uint32_t index);
uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);
void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value);

#endif
