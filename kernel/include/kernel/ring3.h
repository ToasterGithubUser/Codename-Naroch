#ifndef _KERNEL_RING3_H
#define _KERNEL_RING3_H

#include <stdint.h>

enum {
    RING3_TEST_IDLE,
    RING3_TEST_RUNNING,
    RING3_TEST_BLOCKED,
    RING3_TEST_BYPASSED
};

typedef struct {
    volatile uint32_t result;
    volatile uint32_t pid;
} ring3_test_state_t;

extern volatile ring3_test_state_t* ring3_test_state;

#endif
