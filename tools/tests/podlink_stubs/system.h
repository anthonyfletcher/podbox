#ifndef TEST_SYSTEM_H
#define TEST_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))

extern bool test_thread_context;
#define is_thread_context() test_thread_context
extern long current_tick;
#define USEC_TIMER ((uint32_t)(current_tick * 10000))
static inline int disable_irq_save(void)
{
    return 0;
}
static inline void restore_irq(int level)
{
    (void)level;
}

#endif
