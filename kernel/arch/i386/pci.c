#include <kernel/io.h>
#include <kernel/pci.h>

#define PCI_CONFIG_ADDRESS 0x0CF8
#define PCI_CONFIG_DATA 0x0CFC

static pci_device_t devices[PCI_MAX_DEVICES];
static uint32_t device_count;
static int device_list_truncated;

uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function,
                        uint8_t offset, uint16_t value) {
    uint32_t address = 0x80000000U |
        ((uint32_t)bus << 16) | ((uint32_t)device << 11) |
        ((uint32_t)function << 8) | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outw((uint16_t)(PCI_CONFIG_DATA + (offset & 2)), value);
}

void pci_init(void) {
    device_count = 0;
    device_list_truncated = 0;

    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t device = 0; device < 32; device++) {
            uint32_t identity = pci_read_config32(
                (uint8_t)bus, (uint8_t)device, 0, 0);
            if ((identity & 0xFFFF) == 0xFFFF) {
                continue;
            }

            uint32_t header = pci_read_config32(
                (uint8_t)bus, (uint8_t)device, 0, 0x0C);
            uint32_t function_count =
                ((header >> 16) & 0x80) ? 8 : 1;
            for (uint32_t function = 0; function < function_count;
                 function++) {
                identity = pci_read_config32(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0);
                uint16_t vendor_id = (uint16_t)identity;
                if (vendor_id == 0xFFFF) {
                    continue;
                }

                uint32_t class_revision = pci_read_config32(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x08);
                header = pci_read_config32(
                    (uint8_t)bus, (uint8_t)device, (uint8_t)function, 0x0C);
                if (device_count == PCI_MAX_DEVICES) {
                    device_list_truncated = 1;
                    continue;
                }

                pci_device_t* entry = &devices[device_count++];
                entry->bus = (uint8_t)bus;
                entry->device = (uint8_t)device;
                entry->function = (uint8_t)function;
                entry->vendor_id = vendor_id;
                entry->device_id = (uint16_t)(identity >> 16);
                entry->revision = (uint8_t)class_revision;
                entry->prog_if = (uint8_t)(class_revision >> 8);
                entry->subclass = (uint8_t)(class_revision >> 16);
                entry->class_code = (uint8_t)(class_revision >> 24);
                entry->header_type = (uint8_t)(header >> 16);
            }
        }
    }
}

uint32_t pci_device_count(void) {
    return device_count;
}

int pci_device_list_truncated(void) {
    return device_list_truncated;
}

const pci_device_t* pci_device_at(uint32_t index) {
    return index < device_count ? &devices[index] : 0;
}
