#pragma once
#include "dlss5nr.h"

// Private, size-checked extension. The original C API remains the first member;
// older callers keep their original structure size and ordinal-0 behavior.
typedef struct trp_dlss5nr_create_info {
    dlss5nr_create_info base;
    const char* adapter_pci_bus_id;
    uint64_t adapter_luid;
} trp_dlss5nr_create_info;

#ifdef __cplusplus
extern "C" {
#endif
DLSS5NR_API uint32_t dlss5nr_trp_adapter_selection_version(void);
#ifdef __cplusplus
}
#endif
