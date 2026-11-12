#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include "system.h"
#define HZ 100
#define TIME_AFTER(a, b) ((long)((b) - (a)) < 0)
extern long current_tick;
struct mutex
{
    int held;
};
static inline void mutex_init(struct mutex *m)
{
    m->held = 0;
}
static inline void mutex_lock(struct mutex *m)
{
    assert(!m->held);
    m->held = 1;
}
static inline void mutex_unlock(struct mutex *m)
{
    assert(m->held);
    m->held = 0;
}
struct semaphore
{
    int count, maximum;
};
static inline void semaphore_init(struct semaphore *s, int maximum, int count)
{
    s->maximum = maximum;
    s->count = count;
}
static inline void semaphore_release(struct semaphore *s)
{
    if (s->count < s->maximum)
        ++s->count;
}
static inline int semaphore_wait(struct semaphore *s, int ticks)
{
    (void)ticks;
    if (s->count)
        --s->count;
    return 0;
}
#endif
