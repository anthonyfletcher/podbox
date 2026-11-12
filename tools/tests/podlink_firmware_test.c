/* Actual serial/session sources with RAM-backed controller and clock. */
#include "config.h"
#include "kernel.h"
#include <string.h>
#include <stdio.h>
#include "../../firmware/usbstack/usb_serial.c"
#include "../../firmware/usbstack/podlink.c"

long current_tick;
bool test_thread_context = true;
static bool lease, plugged, kicked;
static void *recv_ptr;
static int recv_size, recv_have, sent_size;
static int test_high_speed = 1;
int usb_drv_port_speed(void)
{
    return test_high_speed;
}
static unsigned char output[32768];
static int output_size;
bool usb_podlink_open(void)
{
    if (lease || plugged)
        return false;
    lease = true;
    return true;
}
void usb_podlink_close(void)
{
    lease = false;
}
bool usb_podlink_active(void)
{
    return lease;
}
void usb_core_control_response(int response, void *data, int size)
{
    (void)response;
    (void)data;
    (void)size;
}
void usb_signal_class_notify(int driver, uint32_t data)
{
    (void)driver;
    (void)data;
    kicked = true;
}
int usb_drv_recv_nonblocking(int ep, void *data, int size)
{
    (void)ep;
    assert(!recv_ptr);
    recv_ptr = data;
    recv_size = size;
    recv_have = 0;
    return 0;
}
int usb_drv_send_nonblocking(int ep, const void *data, int size)
{
    (void)ep;
    assert(!sent_size);
    assert(output_size + size <= (int)sizeof(output));
    memcpy(output + output_size, data, size);
    output_size += size;
    sent_size = size;
    return 0;
}
static void controller(void)
{
    for (int i = 0; i < 32 && (kicked || sent_size); ++i)
    {
        if (kicked)
        {
            kicked = false;
            serial_notify(0);
        }
        if (sent_size)
        {
            int n = sent_size;
            sent_size = 0;
            usb_serial_transfer_complete(0, USB_DIR_IN, 0, n);
        }
    }
}
static void insert(void)
{
    plugged = true;
    recv_ptr = NULL;
    sent_size = 0;
    assert(usb_serial_init_connection() == 0);
}
static void remove_usb(void)
{
    plugged = false;
    recv_ptr = NULL;
    sent_size = 0;
    usb_serial_disconnect();
}
static void input_bytes(const unsigned char *b, unsigned int n)
{
    while (n)
    {
        controller();
        assert(recv_ptr);
        int packet = test_high_speed ? 512 : 64;
        int chunk = MIN(n, (unsigned)packet);
        chunk = MIN(chunk, recv_size - recv_have);
        memcpy((unsigned char *)recv_ptr + recv_have, b, chunk);
        recv_have += chunk;
        if (recv_have == recv_size || chunk < packet)
        {
            int length = recv_have;
            recv_ptr = NULL;
            usb_serial_transfer_complete(0, USB_DIR_OUT, 0, length);
        }
        b += chunk;
        n -= chunk;
    }
}
static void input_frame(unsigned int type, unsigned int seq, const void *p,
                        unsigned int n)
{
    unsigned char frame[PODLINK_FRAME_MAX];
    unsigned int length = podlink_encode(frame, type, seq, p, n);
    input_bytes(frame, length);
}
static int poll_message(struct podlink_message *m)
{
    controller();
    int n = podlink_poll(m);
    controller();
    return n;
}
static void clear_output(void)
{
    controller();
    output_size = 0;
}

static void test_reconfiguration(void)
{
    usb_serial_init();
    assert(podlink_open());
    struct podlink_message m;
    insert();
    poll_message(&m);
    input_frame(PODLINK_OPEN, 11, hello, sizeof(hello));
    for (int i = 0; i < 20; ++i)
        poll_message(&m);
    assert(podlink_state() == PODLINK_CONNECTED);
    clear_output();
    unsigned char partial[PODLINK_FRAME_MAX];
    unsigned char payload[4096] = {0};
    podlink_encode(partial, PODLINK_BINARY, 12, payload, sizeof(payload));
    input_bytes(partial, 32);
    poll_message(&m);
    assert(parser.used == 32);
    /* Pending application TX and buffered input belong to the old stream. */
    assert(podlink_send(PODLINK_TEXT, "stale", 5) == PODLINK_SENT);
    assert(podlink_send(PODLINK_BINARY, payload, sizeof(payload)) ==
           PODLINK_SENT);
    /* The core disconnects then initializes in one SET_CONFIGURATION call. */
    usb_serial_disconnect();
    recv_ptr = NULL;
    sent_size = 0;
    assert(usb_serial_init_connection() == 0);
    input_frame(PODLINK_OPEN, 100, hello, sizeof(hello));
    assert(poll_message(&m) == 1 && m.type == PODLINK_CLOSE);
    assert(m.length == 2 && m.data[0] == (1006 & 255));
    assert(podlink_state() == PODLINK_WAIT_USB);
    assert(count == 0 && input_pos == 0 && input_length == 0);
    assert(parser.used == 0 && !ping_pending);
    for (int i = 0; i < 20; ++i)
        poll_message(&m);
    printf("After reconfiguration: state=%d parser.used=%u output_bytes=%d\n",
           podlink_state(), parser.used, output_size);
    struct podlink_parser decoded = {0};
    int hellos = 0, opens = 0;
    for (int i = 0; i < output_size; ++i)
    {
        int rc = podlink_decode(&decoded, output[i], &m);
        assert(rc >= 0);
        if (rc == 1)
        {
            if (m.type == PODLINK_HELLO)
                ++hellos;
            else
            {
                assert(m.type == PODLINK_OPEN && m.seq == 100);
                ++opens;
            }
        }
    }
    assert(hellos == 1 && opens == 1);
    assert(podlink_state() == PODLINK_CONNECTED);
    puts("PASS: reconfiguration resets partial RX and pending TX without a UI "
         "poll");
    input_frame(PODLINK_CLOSE, 101, NULL, 0);
    assert(poll_message(&m) == 1 && m.type == PODLINK_CLOSE);
    assert(m.data[0] == (1002 & 255));
    assert(podlink_state() == PODLINK_OPENING && lease);
    for (int i = 0; i < 20; ++i)
        poll_message(&m);
    const unsigned char small_peer[] = {1, 0, 1, 0, 0, 0};
    input_frame(PODLINK_OPEN, 102, small_peer, sizeof(small_peer));
    for (int i = 0; i < 20; ++i)
        poll_message(&m);
    assert(podlink_max_message() == 1);
    assert(podlink_send(PODLINK_TEXT, "xx", 2) == PODLINK_INVALID);
    assert(count == 0);
    assert(podlink_send(PODLINK_TEXT, "x", 1) == PODLINK_SENT);
    for (int i = 0; i < 20; ++i)
        poll_message(&m);
    podlink_close();
    input_frame(PODLINK_CLOSE, 103, NULL, 0);
    assert(poll_message(&m) == 1 && m.type == PODLINK_CLOSE);
    assert(podlink_state() == PODLINK_CLOSING && lease);
    current_tick += HZ + 1;
    poll_message(&m);
    remove_usb();
    clear_output();
}

int main(void)
{
    assert(!usb_cdrv_serial.needs_cpu_boost);
    /* Idle logf can run before init or in an interrupt: no mutex access. */
    serial_mtx.held = 1;
    usb_serial_send((const unsigned char *)"idle", 4);
    serial_mtx.held = 0;
    usb_serial_init();
    insert();
    test_thread_context = false;
    serial_mtx.held = 1;
    usb_serial_send((const unsigned char *)"irq", 3);
    serial_mtx.held = 0;
    test_thread_context = true;
    assert(tx_count == 0);
    remove_usb();
    test_reconfiguration();
    assert(!usb_serial_connected());
    assert(podlink_open());
    assert(!podlink_open());
    struct podlink_message m;
    insert();
    poll_message(&m);
    assert(podlink_state() == PODLINK_OPENING);
    input_frame(PODLINK_OPEN, 11, hello, sizeof(hello));
    for (int i = 0; i < 20 && podlink_state() != PODLINK_CONNECTED; i++)
        poll_message(&m);
    assert(podlink_state() == PODLINK_CONNECTED);
    clear_output();
    unsigned char payload[4096];
    for (int i = 0; i < 4096; i++)
        payload[i] = i % 251;
    /* TX queue is bounded and DMA completes in fragments without losing bytes.
     */
    assert(podlink_send(PODLINK_BINARY, payload, 4096) == 1);
    assert(podlink_send(PODLINK_BINARY, payload, 4096) == 1);
    assert(podlink_send(PODLINK_BINARY, payload, 4096) == 0);
    for (int i = 0; i < 100; i++)
        poll_message(&m);
    struct podlink_parser check = {0};
    int frames = 0;
    for (int i = 0; i < output_size; i++)
        if (podlink_decode(&check, output[i], &m) == 1)
        {
            assert(m.type == PODLINK_BINARY);
            assert(m.length == 4096);
            assert(!memcmp(m.data, payload, 4096));
            ++frames;
        }
    assert(frames == 2);
    /* Fill the bounded RX ring with whole multi-packet DMA requests. */
    clear_output();
    for (int i = 0; i < BUFFER_SIZE / (int)sizeof(payload); i++)
        input_bytes(payload, sizeof(payload));
    assert(!recv_ptr);
    assert(rx_count == BUFFER_SIZE);
    unsigned char readback[BUFFER_SIZE];
    assert(usb_serial_read(readback, BUFFER_SIZE) == BUFFER_SIZE);
    for (int i = 0; i < BUFFER_SIZE / (int)sizeof(payload); i++)
        assert(
            !memcmp(readback + i * sizeof(payload), payload, sizeof(payload)));
    controller();
    assert(recv_ptr);
    /* Corrupt frame followed by a valid frame resynchronizes. */
    unsigned char frame[PODLINK_FRAME_MAX];
    unsigned int n = podlink_encode(frame, PODLINK_TEXT, 20, "broken", 6);
    frame[n - 1] ^= 1;
    input_bytes(frame, n);
    input_frame(PODLINK_TEXT, 21, "hello", 5);
    int got = 0;
    for (int i = 0; i < 100 && !got; i++)
        got = poll_message(&m);
    assert(got && m.type == PODLINK_TEXT && m.length == 5 &&
           !memcmp(m.data, "hello", 5));
    /* A complete frame aligned to one USB packet must not wait for a ZLP. */
    input_frame(PODLINK_BINARY, 122, payload, 492);
    got = 0;
    for (int i = 0; i < 20 && !got; i++)
        got = poll_message(&m);
    assert(got && m.length == 492 && !memcmp(m.data, payload, 492));
    /* Ping replies preserve sequence. */
    clear_output();
    input_frame(PODLINK_PING, 123, "p", 1);
    for (int i = 0; i < 20; i++)
        poll_message(&m);
    memset(&check, 0, sizeof(check));
    got = 0;
    for (int i = 0; i < output_size; i++)
        if (podlink_decode(&check, output[i], &m) == 1)
            if (m.type == PODLINK_PONG && m.seq == 123)
                got = 1;
    assert(got);
    /* Unplug emits one CLOSE; replug handshakes without restarting. */
    remove_usb();
    assert(poll_message(&m) == 1 && m.type == PODLINK_CLOSE &&
           m.data[0] == (1006 & 255));
    assert(!poll_message(&m));
    assert(lease);
    insert();
    poll_message(&m);
    input_frame(PODLINK_OPEN, 22, hello, 6);
    for (int i = 0; i < 20; i++)
        poll_message(&m);
    assert(podlink_state() == PODLINK_CONNECTED);
    /* No keepalive ACK: bounded timeout emits on_close. */
    current_tick += 6 * HZ;
    poll_message(&m);
    current_tick += 16 * HZ;
    assert(poll_message(&m) == 1 && m.type == PODLINK_CLOSE);
    assert(podlink_state() == PODLINK_OPENING);
    input_frame(PODLINK_OPEN, 23, hello, 6);
    for (int i = 0; i < 20; i++)
        poll_message(&m);
    /* Oversized header creates error/CLOSE without allocation. */
    n = podlink_encode(frame, PODLINK_TEXT, 1, NULL, 0);
    frame[12] = 1;
    frame[13] = 16;
    input_bytes(frame, 16);
    got = 0;
    for (int i = 0; i < 20 && !got; i++)
        got = poll_message(&m);
    assert(got && m.type == PODLINK_CLOSE && m.data[0] == (1009 & 255));
    /* User close times out cleanly, releases USB mode, never sleeps here. */
    podlink_close();
    current_tick += 101;
    poll_message(&m);
    assert(podlink_state() == PODLINK_CLOSED && !lease);
    /* Full-speed streams also complete a packet-aligned final write. */
    remove_usb();
    test_high_speed = 0;
    insert();
    assert(recv_size == 64);
    input_bytes(payload, 64);
    unsigned char full_speed[64];
    assert(usb_serial_read(full_speed, 64) == 64);
    assert(!memcmp(full_speed, payload, 64));
    assert(serial_wake.count <= 1);
    remove_usb();
    puts("PASS: real C wire/session/serial: bounded queues, RX backpressure, 4 "
         "KiB integrity, CRC recovery, ping, timeout, close, replug");
    return 0;
}
