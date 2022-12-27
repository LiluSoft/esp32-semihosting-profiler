#pragma once

#include <stdint.h>

#define PROFILING_ITEMS_PER_BANK 100

#ifdef __cplusplus
extern "C"
{
#endif
    void sprofiler_initialize(uint32_t samples_per_second);
#ifdef __cplusplus
}
#endif