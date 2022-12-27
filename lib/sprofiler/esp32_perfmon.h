#pragma once

#include <stdint.h>


struct perfmon_counters_t{
    uint32_t cycles;
    uint32_t instructions;
};

void perfmon_init();
void perfmon_reset();
void perfmon_read(struct perfmon_counters_t *counters);