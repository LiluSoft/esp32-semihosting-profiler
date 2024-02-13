#include "sprofiler.h"
#include <stdbool.h>

#include <driver/timer.h>
#include <esp_vfs_semihost.h>
#include <esp_err.h>
#include <esp_log.h>
#include <malloc.h>
#include <esp_debug_helpers.h>
#include <errno.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#include <esp_cpu_utils.h>
#endif

#include "esp32_perfmon.h"

static const char *TAG = "sprofiler";
static const char *profiling_filename = "/host/sprof.out";

#define MIN(X, Y) (((X) < (Y)) ? (X) : (Y))

#define TIMER_DIVIDER 8000

struct sprofiler_header_t
{
    char header[4];
    uint32_t pointer_size;
    uint32_t samples_per_bank;
    uint32_t samples_per_second;
    uint32_t cores;
};

struct sprofiler_header_t profiler_header;

struct profiling_item
{
    intptr_t callee;
    intptr_t caller;
    uint_fast32_t calls;

    uint_fast32_t cycles;
    uint_fast32_t instructions;
};

struct profiling_bank_t
{
    struct profiling_item items[PROFILING_ITEMS_PER_BANK];
    uint_fast16_t last_index;
    uint_fast16_t check_number;
};

struct profiling_buffer
{
    struct profiling_bank_t bank[2];
    uint_fast8_t current_bank;
};

volatile struct profiling_buffer profiling_data[2];

void switch_banks(struct profiling_buffer *buffer)
{
    buffer->current_bank = (buffer->current_bank == 0) ? 1 : 0;
}

void IRAM_ATTR profile_function(uint_fast8_t core, intptr_t caller, intptr_t callee,struct perfmon_counters_t *counters)
{
    struct profiling_buffer *buffer = &profiling_data[core];
    uint_fast8_t current_bank_id = buffer->current_bank;
    struct profiling_bank_t *bank = &buffer->bank[current_bank_id];

    // look for existing item
    bool item_found = false;
    for (uint_fast16_t i = 0; i < MIN(PROFILING_ITEMS_PER_BANK, bank->last_index); i++)
    {
        if ((bank->items[i].caller == caller) &&
            (bank->items[i].callee == callee))
        {
            struct profiling_item * item = &bank->items[i];
            item->calls++;

            item->cycles += counters->cycles;
            item->instructions += counters->instructions;

            item_found = true;
            break;
        }
    }

    // insert new item
    if (!item_found)
    {
        struct profiling_item *item = &bank->items[bank->last_index++];
        item->callee = callee;
        item->caller = caller;
        item->calls = 1;

        item->cycles = counters->cycles;
        item->instructions = counters->instructions;
    }

    // switch banks
    if (bank->last_index >= PROFILING_ITEMS_PER_BANK)
    {
        // switch_banks(buffer);
        bank->last_index = 0; // override bank
    }
}

static void __attribute__((optimize("O0"))) IRAM_ATTR drill_stack(esp_backtrace_frame_t *frame, uint8_t max_depth, uint8_t skip)
{
    BaseType_t current_core_id = xPortGetCoreID();

    struct perfmon_counters_t counters = {0};
    perfmon_read(&counters);


    // Initialize stk_frame with first frame of stack
    esp_backtrace_frame_t stk_frame = {0};
    memcpy(&stk_frame, frame, sizeof(esp_backtrace_frame_t));

    // Check if first frame is valid
    bool corrupted = !(esp_stack_ptr_is_sane(stk_frame.sp) &&
                       (esp_ptr_executable((void *)esp_cpu_process_stack_pc(stk_frame.pc)) ||
                        /* Ignore the first corrupted PC in case of InstrFetchProhibited */
                        (stk_frame.exc_frame && ((XtExcFrame *)stk_frame.exc_frame)->exccause == EXCCAUSE_INSTR_PROHIBITED)));

    uint_fast16_t depth = 0;
    uint32_t i = (max_depth <= 0) ? INT32_MAX : max_depth;
    while (i-- > 0 && stk_frame.next_pc != 0 && !corrupted)
    {
        intptr_t previous_pc = esp_cpu_process_stack_pc(stk_frame.pc);
        if (!esp_backtrace_get_next_frame(&stk_frame))
        { // Get previous stack frame
            corrupted = true;
        }
        depth++;

        if (!corrupted && depth > skip)
        {
            profile_function(current_core_id, esp_cpu_process_stack_pc(stk_frame.pc), previous_pc, &counters);
        }
    }
}

static bool __attribute__((optimize("O0"))) IRAM_ATTR timer_group_isr_callback(void *args)
{
    esp_backtrace_frame_t start = {0};
    esp_backtrace_get_start(&(start.pc), &(start.sp), &(start.next_pc));
    drill_stack(&start, 20,2);
    perfmon_reset();
    return false;
}

void initializeProfilerTimer(void *parameter)
{
    BaseType_t current_core_id = xPortGetCoreID();

    timer_config_t config = {};
    config.divider = TIMER_DIVIDER;
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en = TIMER_PAUSE;
    config.alarm_en = TIMER_ALARM_EN;
    config.auto_reload = TIMER_AUTORELOAD_EN;
    // }; // default clock source is APB
    timer_init(TIMER_GROUP_0, current_core_id, &config);

    /* Timer's counter will initially start from value below.
       Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(TIMER_GROUP_0, current_core_id, 0);

    /* Configure the alarm value and the interrupt on alarm. */
    uint64_t alarm = 80000000 / TIMER_DIVIDER / profiler_header.samples_per_second;
    timer_set_alarm_value(TIMER_GROUP_0, current_core_id, alarm); // 80,000,000 APB Clock / 8,000 TIMER_DIVIDER / 10 times per second
    timer_enable_intr(TIMER_GROUP_0, current_core_id);

    timer_isr_callback_add(TIMER_GROUP_0, current_core_id, timer_group_isr_callback, NULL, 0);

    timer_start(TIMER_GROUP_0, current_core_id);

    vTaskDelete(NULL);
}

static void write_header(FILE *fout)
{
    write(fileno(fout), &profiler_header, sizeof(struct sprofiler_header_t));
    // fwrite(&profiler_header, sizeof(struct sprofiler_header_t),1,  fout);
    // fflush(fout);
}

void write_counters(FILE *fout)
{
    const int high_watermark = PROFILING_ITEMS_PER_BANK * 0.75; // flush 3/4 full counters

    for (uint_fast8_t core_id = 0; core_id < 2; core_id++)
    {
        struct profiling_buffer *profiling_data_for_core = &profiling_data[core_id];
        uint_fast8_t current_bank = profiling_data_for_core->current_bank;
        profiling_data_for_core->bank[current_bank].check_number++;

        if ((profiling_data_for_core->bank[current_bank].last_index > high_watermark) ||
            (profiling_data_for_core->bank[current_bank].check_number > profiler_header.samples_per_second))
        {
            // ESP_LOGI(TAG, "writing %d:%d", core_id, current_bank);
            // switch banks,
            switch_banks(profiling_data_for_core);
            // flush previous bank
            write(fileno(fout),&profiling_data_for_core->bank[current_bank], sizeof(struct profiling_bank_t) );
            // fwrite(&profiling_data_for_core->bank[current_bank], sizeof(struct profiling_bank_t), 1, fout);
            // fflush(fout);
            // zero index
            profiling_data_for_core->bank[current_bank].last_index = 0;
            profiling_data_for_core->bank[current_bank].check_number =0;
        }
    }
}

void flush_counters(void *params)
{
    FILE *fout = fopen(profiling_filename, "wb");
    if (fout == NULL)
    {
        ESP_LOGE(TAG, "Failed to reopen stdout (%d)!", errno);
        return;
    }

    setvbuf(fout, NULL, _IOFBF, 4096);

    write_header(fout);

    while (true)
    {
        write_counters(fout);
        // should flush every ...?
        // samples per second + buffer size + average stack length
        // profiler_header.samples_per_second;
        // PROFILING_ITEMS_PER_BANK;
        const int average_stack_length = 10; // 10 levels deep
        const int flushes_per_interval = 2;
        const int ms = 1000;
        const int sleep_between_flushes = (PROFILING_ITEMS_PER_BANK / (float)(profiler_header.samples_per_second * average_stack_length) / (float)flushes_per_interval) * ms;
        // (PROFILING_ITEMS_PER_BANK / average_stack_length)
        // or something..
        vTaskDelay(pdMS_TO_TICKS(sleep_between_flushes));
    }
}

void sprofiler_initialize(uint32_t samples_per_second)
{
    // open sprofiler.out file
    // write header
    // initialize task to start timer on each core
    // initialize task to flush profiling data out and flush on each write
    ESP_LOGI(TAG, "Initializing SProfiler...");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_err_t ret = esp_vfs_semihost_register("/host");
#else
    esp_err_t ret = esp_vfs_semihost_register("/host", NULL);
#endif
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register semihost driver (%s)!", esp_err_to_name(ret));
        return;
    }

    

    memcpy(profiler_header.header, "sprf", 4);
#ifdef CONFIG_FREERTOS_UNICORE
    profiler_header.cores = 1;
#else
    profiler_header.cores = 2;
#endif
    profiler_header.pointer_size = sizeof(intptr_t);
    profiler_header.samples_per_bank = PROFILING_ITEMS_PER_BANK;
    profiler_header.samples_per_second = samples_per_second;

    perfmon_init();
    perfmon_reset();

    xTaskCreatePinnedToCore(initializeProfilerTimer, "prof0", 1024, 0, configMAX_PRIORITIES - 1, NULL, 0);

#ifndef CONFIG_FREERTOS_UNICORE
    xTaskCreatePinnedToCore(initializeProfilerTimer, "prof1", 1024, 1, configMAX_PRIORITIES - 1, NULL, 1);
#endif

    // create flush task
    xTaskCreate(flush_counters, "profflush", 4096, 0, configMAX_PRIORITIES - 1, NULL);

    // wait for tasks to start, since each one is the highest priority, it should be done very quickly
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Select and initialize basic parameters of the timer */
    ESP_LOGI(TAG, "Done Initializing SProfiler...");
}
