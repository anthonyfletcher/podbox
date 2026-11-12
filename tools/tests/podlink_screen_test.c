#include "config.h"
#include "kernel.h"
#include <stdio.h>
void yield(void);
#include "../../apps-ipod/screens/system/podlink_screen.c"
long current_tick;
bool test_thread_context = true;
unsigned long core_available(void)
{
    return 0;
}
int battery_level(void)
{
    return 50;
}
const char *rbversion = "host-test";
static void command(const char *s)
{
    struct podlink_message m = {.type = PODLINK_TEXT,
                                .data = (const unsigned char *)s,
                                .length = strlen(s)};
    message(&m);
}

static unsigned int limit, attempts, binary_bytes, invalid_sends;
static unsigned int turns, stage;
static bool closed;
bool podlink_open(void)
{
    return true;
}
unsigned int podlink_max_message(void)
{
    return limit;
}
int podlink_send(unsigned int type, const void *data, unsigned int length)
{
    ++attempts;
    if (length > limit)
    {
        ++invalid_sends;
        return PODLINK_INVALID;
    }
    if (type == PODLINK_BINARY)
    {
        const unsigned char *bytes = data;
        for (unsigned int i = 0; i < length; ++i)
            assert(bytes[i] == (binary_bytes + i) % 251);
        binary_bytes += length;
    }
    return PODLINK_SENT;
}
int podlink_poll(struct podlink_message *m)
{
    static unsigned char oversized[PODLINK_MAX_MESSAGE];
    static const unsigned char bench[] = "BENCH RX 8192";
    if (stage == 0)
    {
        *m = (struct podlink_message){.type = PODLINK_TEXT,
             .data = oversized, .length = sizeof(oversized)};
        ++stage;
        return 1;
    }
    if (stage == 1 && !reply_type)
    {
        *m = (struct podlink_message){.type = PODLINK_TEXT,
             .data = bench, .length = sizeof(bench) - 1};
        ++stage;
        return 1;
    }
    return 0;
}
enum podlink_connection podlink_state(void)
{
    return closed ? PODLINK_CLOSED : PODLINK_CONNECTED;
}
void podlink_close(void)
{
    closed = true;
}
void podlink_wait(int ticks)
{
    (void)ticks;
}
void yield(void)
{
}
void splash(int ticks, const char *s)
{
    (void)ticks;
    (void)s;
}
void lcd_clear_display(void)
{
}
void lcd_update(void)
{
}
void lcd_puts(int x, int y, const char *s)
{
    (void)x;
    (void)y;
    (void)s;
}
void lcd_putsf(int x, int y, const char *fmt, ...)
{
    (void)x;
    (void)y;
    (void)fmt;
}
int default_event_handler(int event)
{
    (void)event;
    return 0;
}
int button_get(bool block)
{
    (void)block;
    /* Bound the real screen loop even when an invalid reply never clears. */
    return ++turns >= 2000 || (binary_bytes == 8192 && !reply_type)
           ? BUTTON_MENU : 0;
}

int main(void)
{
    const char *invalid[] = {
        "BENCH RX 0",        "BENCH RX x",
        "BENCH RX 16777217", "BENCH RX 999999999999999999999",
        "BENCH TX 12x",      "BENCH TX 0"};
    const char *start[] = {"BENCH RX 8192", "BENCH TX 8192"};
    for (unsigned int d = 0; d < 2; ++d)
    {
        command(start[d]);
        assert(goal == 8192);
        int active = operation;
        offset = 4096;
        errors = 3;
        for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        {
            command(invalid[i]);
            assert(reply_type == PODLINK_TEXT);
            assert(!memcmp(reply, "ERR benchmark size", reply_len));
            assert((int)operation == active && goal == 8192);
            assert(offset == 4096 && errors == 3 && goal - offset == 4096);
        }
        /* A valid command explicitly replaces either active transfer. */
        command("BENCH TX 4096");
        assert(operation == UPLOAD && goal == 4096 && offset == 0 &&
               errors == 0);
    }
    const unsigned int limits[] = {1, 8, 32, 1024};
    for (unsigned int i = 0; i < ARRAYLEN(limits); ++i)
    {
        limit = limits[i];
        attempts = binary_bytes = invalid_sends = turns = stage = 0;
        closed = false;
        podlink_screen();
        assert(binary_bytes == 8192);
        assert(stage == 2 && invalid_sends <= 3);
        assert(attempts < 8192 / limit + 10);
    }
    puts("PASS: small peer limits recover from INVALID and finish DOWNLOAD");
    puts("PASS: rejected BENCH commands preserve upload/download state");
    return 0;
}
