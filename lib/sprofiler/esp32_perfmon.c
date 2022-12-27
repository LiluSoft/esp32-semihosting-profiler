#include "esp32_perfmon.h"

#include "perfmon.h"

static uint32_t pm_check_table[] = {
    XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES, // total cycles
    XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL, // total instructions
};

void perfmon_init(){
    ESP_ERROR_CHECK(xtensa_perfmon_init(0, pm_check_table[0*2],pm_check_table[(0*2)+1],0,-1));
    ESP_ERROR_CHECK(xtensa_perfmon_init(1, pm_check_table[1*2],pm_check_table[(1*2)+1],0,-1));

    xtensa_perfmon_start();
    
}
void perfmon_reset(){
    ESP_ERROR_CHECK(xtensa_perfmon_reset(0));
    ESP_ERROR_CHECK(xtensa_perfmon_reset(1));
}

void perfmon_read(struct perfmon_counters_t *counters){
    counters->cycles = xtensa_perfmon_value(0);
    counters->instructions = xtensa_perfmon_value(1);
}