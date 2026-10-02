/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* iAP2's control session, with the player as the phone. A message is
 * 40 40, its length and its ID, then parameters, each its length, its ID and
 * its data; a group parameter's data is more parameters. On the USB thread.
 *
 * Parts, in file order:
 * - names and logging: every message each way, and the car's parameters
 *   raw, so that one that is not handled can still be read;
 * - building and reading messages;
 * - the conversation, in the order a car takes it: once the link is up the
 *   player asks for the car's certificate, sends a challenge and accepts
 *   whatever answers it, asks the car to identify itself and accepts that
 *   too, then sends what an iPhone sends next. The car then starts what it
 *   wants: USB audio, now playing, its buttons;
 * - USB audio: the iAP sink takes playback and the player announces its
 *   rate, again whenever the sink changes rate;
 * - power: the answer to the car's StartPowerUpdates;
 * - media library: every track in the database, sent as the link has room,
 *   the tracks the car picks played, and the playing track's cover sent as
 *   a file transfer;
 * - now playing: the track's attributes the car asked for, whenever the
 *   track changes, and its playback attributes whenever the state changes
 *   or the position jumps;
 * - the car's buttons: its HID report descriptor is read for the consumer
 *   controls, and a control's press is posted as the multimedia button the
 *   player's own remotes use.
 *
 * Message IDs and identification parameters from lvalen91/carplayd (public
 * domain), rust/carplayd/crates/iap2-core/src/spec.rs. Now playing and HID
 * parameter layouts read from usenocturne/nocturne, crates/iap2/src/csm/,
 * for facts only. Message order and the parameters an iPhone fills, from an
 * iPhone in this car (its iAP debug log) and answering the car's replayed
 * messages. carplay-provenance.md gives the source of every fact here. */

#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "audio.h"
#include "button.h"
#include "metadata.h"
#include "pcm_mixer.h"
#include "pcm_sink.h"
#include "playlist.h"
#include "powermgmt.h"
#include "settings.h"
#include "usb_log.h"

#include "iap/audio.h"
#include "iap_library.h"
#include "usb_iap2.h"

#define REQUEST_CERTIFICATE        0xAA00
#define CERTIFICATE                0xAA01
#define REQUEST_CHALLENGE_RESPONSE 0xAA02
#define CHALLENGE_RESPONSE         0xAA03
#define AUTHENTICATION_SUCCEEDED   0xAA05
#define START_IDENTIFICATION       0x1D00
#define IDENTIFICATION_INFORMATION 0x1D01
#define IDENTIFICATION_ACCEPTED    0x1D02
#define START_BLUETOOTH_PAIRING    0x0B00
#define STOP_BLUETOOTH_PAIRING     0x0B03
#define START_BT_CONNECTION        0x4E03
#define BT_CONNECTION_UPDATE       0x4E04
#define START_LIBRARY_INFORMATION  0x4C00
#define LIBRARY_INFORMATION        0x4C01
#define START_LIBRARY_UPDATES      0x4C03
#define LIBRARY_UPDATE             0x4C04
#define STOP_LIBRARY_UPDATES       0x4C05
#define PLAY_LIBRARY_ITEMS         0x4C07
#define DEVICE_INFORMATION_UPDATE  0x4E09
#define DEVICE_LANGUAGE_UPDATE     0x4E0A
#define DEVICE_UUID_UPDATE         0x4E0C
#define CARPLAY_AVAILABILITY       0x4300
#define POWER_SOURCE_UPDATE        0xAE03
#define START_POWER_UPDATES        0xAE00
#define POWER_UPDATE               0xAE01
#define START_NOW_PLAYING          0x5000
#define NOW_PLAYING_UPDATE         0x5001
#define STOP_NOW_PLAYING           0x5002
#define SET_NOW_PLAYING            0x5003
#define START_HID                  0x6800
#define ACCESSORY_HID_REPORT       0x6802
#define STOP_HID                   0x6803
#define START_USB_AUDIO            0xDA00
#define USB_AUDIO_INFORMATION      0xDA01
#define STOP_USB_AUDIO             0xDA02

#define DEVICE_UUID "50F0D0B0-6E2B-4F8A-9C1D-0000000006C0"
/* In an iPhone's form: a UUID, then -MPN- for the device's own library, then
 * a version. An ID ending -PODBOX was the last thing this car took before
 * going silent; unconfirmed as the cause. */
#define LIBRARY_ID  "50F0D0B0-6E2B-4F8A-9C1D-0000000006C1-MPN-26.6.2"

/* The player's name, read as the car identifies. */
static char device_name[32];

/* ---- names and logging -------------------------------------------------- */

static const struct
{
    uint16_t id;
    const char *name;
} names[] = {
    {0x0B00, "StartBluetoothPairing"},
    {0x0B01, "BluetoothPairingAccessoryInformation"},
    {0x0B02, "BluetoothPairingStatus"},
    {0x0B03, "StopBluetoothPairing"},
    {0x1D00, "StartIdentification"},
    {0x1D01, "IdentificationInformation"},
    {0x1D02, "IdentificationAccepted"},
    {0x1D03, "IdentificationRejected"},
    {0x1D05, "CancelIdentification"},
    {0x1D06, "IdentificationInformationUpdate"},
    {0x4154, "StartCallStateUpdates"},
    {0x4156, "StopCallStateUpdates"},
    {0x4157, "StartCommunicationsUpdates"},
    {0x4159, "StopCommunicationsUpdates"},
    {0x4300, "CarPlayAvailability"},
    {0x4301, "CarPlayStartSession"},
    {0x4C00, "StartMediaLibraryInformation"},
    {0x4C01, "MediaLibraryInformation"},
    {0x4C02, "StopMediaLibraryInformation"},
    {0x4C03, "StartMediaLibraryUpdates"},
    {0x4C04, "MediaLibraryUpdate"},
    {0x4C05, "StopMediaLibraryUpdates"},
    {0x4C06, "PlayMediaLibraryCurrentSelection"},
    {0x4C07, "PlayMediaLibraryItems"},
    {0x4C08, "PlayMediaLibraryCollection"},
    {0x4C09, "PlayMediaLibrarySpecial"},
    {0x4E01, "BluetoothComponentInformation"},
    {0x4E03, "StartBluetoothConnectionUpdates"},
    {0x4E05, "StopBluetoothConnectionUpdates"},
    {0x4E09, "DeviceInformationUpdate"},
    {0x4E0A, "DeviceLanguageUpdate"},
    {0x4E0B, "DeviceTimeUpdate"},
    {0x4E0C, "DeviceUUIDUpdate"},
    {0x4E0E, "DeviceTransportIdentifierNotification"},
    {0x5000, "StartNowPlayingUpdates"},
    {0x5001, "NowPlayingUpdate"},
    {0x5002, "StopNowPlayingUpdates"},
    {0x5003, "SetNowPlayingInformation"},
    {0x5200, "StartRouteGuidanceUpdates"},
    {0x5203, "StopRouteGuidanceUpdates"},
    {0x6800, "StartHID"},
    {0x6801, "DeviceHIDReport"},
    {0x6802, "AccessoryHIDReport"},
    {0x6803, "StopHID"},
    {0x6806, "StartNativeHID"},
    {0x6807, "HIDComponentUpdate"},
    {0xAA00, "RequestAuthenticationCertificate"},
    {0xAA01, "AuthenticationCertificate"},
    {0xAA02, "RequestAuthenticationChallengeResponse"},
    {0xAA03, "AuthenticationResponse"},
    {0xAA04, "AuthenticationFailed"},
    {0xAA05, "AuthenticationSucceeded"},
    {0xAA06, "AccessoryAuthenticationSerialNumber"},
    {0xAE00, "StartPowerUpdates"},
    {0xAE01, "PowerUpdate"},
    {0xAE02, "StopPowerUpdates"},
    {0xAE03, "PowerSourceUpdate"},
    {0xAE05, "AccessoryPowerUpdate"},
    {0xDA00, "StartUSBDeviceModeAudio"},
    {0xDA01, "USBDeviceModeAudioInformation"},
    {0xDA02, "StopUSBDeviceModeAudio"},
    {0xEA00, "StartExternalAccessoryProtocolSession"},
    {0xEA01, "StopExternalAccessoryProtocolSession"},
    {0xEA02, "RequestAppLaunch"},
    {0xFFFA, "StartLocationInformation"},
    {0xFFFB, "LocationInformation"},
    {0xFFFC, "StopLocationInformation"},
};

const char *usb_log_iap2_message(int id)
{
    for (size_t i = 0; i < ARRAYLEN(names); i++)
        if (names[i].id == id)
            return names[i].name;
    return "?";
}

/* A message's parameters, eight bytes an entry, up to max. */
static void log_params(const uint8_t *msg, size_t len, size_t max)
{
    for (size_t off = 6; off < MIN(len, 6 + max); off += 8)
    {
        uint32_t c = 0, d = 0;
        for (size_t i = 0; i < 4; i++)
        {
            c = c << 8 | (off + i < len ? msg[off + i] : 0);
            d = d << 8 | (off + 4 + i < len ? msg[off + 4 + i] : 0);
        }
        usb_log(USB_LOG_IAP2_PARAMS, MIN(len - off, 8), off - 6, c, d);
    }
}

static void event(int what, uint32_t c, uint32_t d)
{
    usb_log(USB_LOG_IAP2_EVENT, what, 0, c, d);
}

/* Logged as "iap2 event 12": the whole library has gone, c its slots. */
#define EVENT_LIBRARY_SENT 12

/* ---- building and reading messages -------------------------------------- */

static struct
{
    uint8_t *b;
    size_t len, room, group;
    uint16_t id;
    bool ok;
} m;

static bool msg_start(uint16_t id)
{
    m.b = usb_iap2_message_start(&m.room);
    if (!m.b)
    {
        event(USB_LOG_IAP2_DROPPED, id, 0);
        return false;
    }
    m.b[0] = 0x40;
    m.b[1] = 0x40;
    m.b[4] = id >> 8;
    m.b[5] = id;
    m.len = 6;
    m.id = id;
    m.ok = true;
    return true;
}

static void param(uint16_t id, const void *data, size_t n)
{
    if (m.len + 4 + n > m.room)
    {
        m.ok = false;
        return;
    }
    m.b[m.len] = (4 + n) >> 8;
    m.b[m.len + 1] = 4 + n;
    m.b[m.len + 2] = id >> 8;
    m.b[m.len + 3] = id;
    if (n)
        memcpy(m.b + m.len + 4, data, n);
    m.len += 4 + n;
}

static void param_u8(uint16_t id, uint8_t v)
{
    param(id, &v, 1);
}

static void param_u16(uint16_t id, uint16_t v)
{
    uint8_t b[2] = {v >> 8, v};
    param(id, b, 2);
}

static void param_u32(uint16_t id, uint32_t v)
{
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    param(id, b, 4);
}

static void param_u64(uint16_t id, uint32_t hi, uint32_t lo)
{
    uint8_t b[8] = {hi >> 24, hi >> 16, hi >> 8, hi, lo >> 24, lo >> 16,
                    lo >> 8, lo};
    param(id, b, 8);
}

/* NUL-terminated, cut at 200 bytes on a character boundary. */
static void param_str(uint16_t id, const char *s)
{
    size_t n = strlen(s);
    if (n > 200)
    {
        n = 200;
        while (n && ((uint8_t)s[n] & 0xC0) == 0x80)
            n--;
    }
    if (m.len + 4 + n + 1 > m.room)
    {
        m.ok = false;
        return;
    }
    param(id, s, n);
    m.b[m.len++] = 0;
    m.b[m.len - n - 5] = (4 + n + 1) >> 8;
    m.b[m.len - n - 4] = 4 + n + 1;
}

static void group_start(uint16_t id)
{
    m.group = m.len;
    param(id, NULL, 0);
}

static void group_end(void)
{
    const size_t n = m.len - m.group;
    m.b[m.group] = n >> 8;
    m.b[m.group + 1] = n;
}

static void msg_send(void)
{
    if (!m.ok)
    {
        event(USB_LOG_IAP2_DROPPED, m.id, m.len);
        return;
    }
    m.b[2] = m.len >> 8;
    m.b[3] = m.len;
    usb_log(USB_LOG_IAP2_MSG, USB_LOG_IAP_FROM_PLAYER, m.len, m.id, 0);
    usb_iap2_message_send(m.len);
}

static void send_empty(uint16_t id)
{
    if (msg_start(id))
        msg_send();
}

/* The first parameter id in data[0..len), or NULL. */
static const uint8_t *find(const uint8_t *data, size_t len, uint16_t id,
                           size_t *n)
{
    size_t off = 0;
    while (off + 4 <= len)
    {
        const size_t plen = data[off] << 8 | data[off + 1];
        if (plen < 4 || off + plen > len)
            break;
        if ((data[off + 2] << 8 | data[off + 3]) == id)
        {
            *n = plen - 4;
            return data + off + 4;
        }
        off += plen;
    }
    return NULL;
}

static uint32_t get_be(const uint8_t *p, size_t n)
{
    uint32_t v = 0;
    while (n--)
        v = v << 8 | *p++;
    return v;
}

/* ---- state -------------------------------------------------------------- */

static uint16_t car_receives[64]; /* what identification says it takes */
static size_t car_receives_n;
static bool identified;
static long availability_due;     /* when to send CarPlayAvailability, or 0 */
static uint8_t next_transfer = 0x80; /* file transfer IDs run 80 to FF */
static uint8_t artwork;           /* the artwork transfer just named, or 0 */
/* The artwork transfer under way: see the now playing part */
enum { ART_IDLE, ART_FINDING, ART_ANNOUNCED, ART_SENDING };
static struct
{
    int phase;
    uint8_t id;
    uint32_t size, sent;
    long started;
} aw;
/* The library on its way to the car: see the media library part */
static struct
{
    bool active;
    bool reset;             /* the next update opens with a reset */
    bool waiting;           /* for the database, to send it unasked */
    int next, slots;        /* the path index slot to send next, of */
    uint32_t mask;          /* the item properties the car asked for */
    char rev[12];           /* the revision once all is sent */
} lib;

/* The revision on the way, which no whole library has */
#define PARTIAL_REVISION "0"

static bool audio_on;
static volatile unsigned long rate_wanted;
static unsigned long rate_sent;

static bool now_playing;
static uint32_t media_mask, playback_mask;
static uint32_t np_key;           /* the track last described */
static int np_state;
static unsigned long np_elapsed;
static long np_tick;
static bool np_full;

#define MAX_CONTROLS 24
struct control
{
    uint8_t report;     /* report ID, or 0 */
    uint16_t bit;       /* offset from the report's data */
    uint8_t size;
    uint8_t count;      /* > 1: an array of usages */
    uint16_t usage;     /* a bit's usage, or an array's first */
};
static struct control controls[MAX_CONTROLS];
static size_t n_controls;
static bool report_ids;
static uint16_t hid_component;
static uint8_t last_report[4][16];
static uint8_t last_report_id[4];

static bool car_takes(uint16_t id)
{
    for (size_t i = 0; i < car_receives_n; i++)
        if (car_receives[i] == id)
            return true;
    return false;
}

void usb_iap2_control_reset(void)
{
    identified = false;
    availability_due = 0;
    artwork = 0;
    if (aw.phase != ART_IDLE)
        iap_library_artwork_stop();
    aw.phase = ART_IDLE;
    lib.active = false;
    lib.waiting = false;
    car_receives_n = 0;
    now_playing = false;
    n_controls = 0;
    memset(last_report_id, 0, sizeof(last_report_id));
    if (audio_on)
    {
        audio_on = false;
        mixer_switch_sink(PCM_SINK_BUILTIN);
    }
}

/* ---- USB audio ---------------------------------------------------------- */

/* The rate as an index into this table, in a uint8; an iPhone at 44100
 * sends 7. It follows that with two uint32s, both 0 from the iPhone, whose
 * meaning is unknown. */
static const unsigned long rates[] = {
    8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000,
};

static void send_rate(unsigned long rate)
{
    size_t i = 0;
    while (i < ARRAYLEN(rates) - 1 && rates[i] != rate)
        i++;
    if (!msg_start(USB_AUDIO_INFORMATION))
        return;
    param_u8(0, i);
    param_u32(1, 0);
    param_u32(2, 0);
    msg_send();
    rate_sent = rate;
    event(USB_LOG_IAP2_RATE, rate, i);
}

void usb_iap2_control_rate(unsigned long rate)
{
    rate_wanted = rate;
}

bool usb_iap2_control_audio(void)
{
    return audio_on;
}

static void start_audio(void)
{
    audio_on = true;
    mixer_switch_sink(PCM_SINK_IAP);
    if (!rate_wanted)
        rate_wanted = iap_audio_sampr();
    send_rate(rate_wanted);
}

static void stop_audio(void)
{
    if (!audio_on)
        return;
    audio_on = false;
    audio_pause();
    mixer_switch_sink(PCM_SINK_BUILTIN);
}

/* ---- power -------------------------------------------------------------- */

/* StartPowerUpdates lists the attributes the car wants, as a group of empty
 * parameters like StartNowPlayingUpdates'. An iPhone in this car answers
 * each one asked for in a message of its own, 4, 5, 0, 1 and then 6:
 * 4 `01`; 5 `02` at full charge, read as the charging state (0 none,
 * 1 charging, 2 charged); 0 `uint16` 1500; 1 `01`; 6 `uint16` 100 at full
 * charge, read as the battery percentage. */
static void send_power_update(uint32_t wanted_mask)
{
    static const uint8_t order[] = {4, 5, 0, 1, 6};
    bool sent = false;
    for (size_t i = 0; i < ARRAYLEN(order); i++)
    {
        const unsigned a = order[i];
        if (!(wanted_mask & 1u << a) || !msg_start(POWER_UPDATE))
            continue;
        if (a == 5) /* on the car's power, not charging means charged */
            param_u8(a, charge_state == DISCHARGING ? 2 : 1);
        else if (a == 0)
            param_u16(a, 1500);
        else if (a == 6)
            param_u16(a, battery_level());
        else
            param_u8(a, 1);
        msg_send();
        sent = true;
    }
    event(USB_LOG_IAP2_POWER, sent, wanted_mask);
}

/* ---- media library ------------------------------------------------------ */

/* The library: 4C01 is a group per library (0 name, 1 ID, 2 type, 0 the
 * device's own). To 4C03, which names the revision the car holds and the
 * item properties it wants (parameter 2), the 4C04s an iPhone sends: the
 * first with 8 `00`, the second 9 `01`; then, unless the car holds this
 * revision already, the items (parameter 2, a group each) in updates of up
 * to a packet, the first opening with 6, a reset; the last carries the
 * revision and 7, the progress, 100. An update on the way carries the
 * revision before this one and progress 0, so a car cut off mid-way asks
 * again from the start. Every track goes, by its key, from the database in
 * RAM, as fast as packets leave and a few slots short of full; without the
 * database the library is the playing track alone. An iPhone's own library
 * always holds tracks, and this car reads nothing more after an empty one.
 * Item layout from an iPhone in this car; carplay.md has the properties. */

static void send_library_information(void)
{
    if (!msg_start(LIBRARY_INFORMATION))
        return;
    group_start(0);
    param_str(0, device_name);
    param_str(1, LIBRARY_ID);
    param_u8(2, 0);
    group_end();
    msg_send();
}

static uint32_t hash(const char *s);
static const char *track_title(const struct mp3entry *id3);

/* A name and its ID, where the track has the name. */
static void named(uint32_t mask, unsigned id_param, uint64_t id,
                  const char *name)
{
    if (!name || !*name)
        return;
    if ((mask & 1u << id_param) && id)
        param_u64(id_param, id >> 32, id);
    if (mask & 1u << (id_param + 1))
        param_str(id_param + 1, name);
}

static void put_track(const struct iap_library_track *t, uint32_t mask)
{
    group_start(2);
    if (mask & 1u << 0)
        param_u64(0, t->key >> 32, t->key); /* now playing's ID for it too */
    if ((mask & 1u << 1) && t->title)
        param_str(1, t->title);
    if (mask & 1u << 2)
        param_u8(2, 0);
    if (mask & 1u << 3)
        param_u8(3, 0);
    if ((mask & 1u << 4) && t->length)
        param_u32(4, t->length);
    named(mask, 5, t->album_id, t->album);
    if ((mask & 1u << 7) && t->tracknum > 0)
        param_u16(7, t->tracknum);
    if ((mask & 1u << 9) && t->discnum > 0)
        param_u16(9, t->discnum);
    named(mask, 11, t->artist_id, t->artist);
    named(mask, 13, t->albumartist_id, t->albumartist);
    named(mask, 15, t->genre_id, t->genre);
    named(mask, 17, t->composer_id, t->composer);
    if (mask & 1u << 19)
        param_u8(19, 0);
    if (mask & 1u << 25)
        param_u8(25, 1);
    if (mask & 1u << 27)
        param_u16(27, 0);
    group_end();
}

/* The playing track, for a library without the database. */
static void track_from_id3(const struct mp3entry *id3,
                           struct iap_library_track *t)
{
    memset(t, 0, sizeof(*t));
    t->key = iap_library_key(id3->path);
    t->title = track_title(id3);
    t->album = id3->album;
    t->artist = id3->artist;
    t->albumartist = id3->albumartist;
    t->genre = id3->genre_string;
    t->composer = id3->composer;
    t->album_id = t->album ? hash(t->album) : 0;
    t->artist_id = t->artist ? hash(t->artist) : 0;
    t->albumartist_id = t->albumartist ? hash(t->albumartist) : 0;
    t->genre_id = t->genre ? hash(t->genre) : 0;
    t->composer_id = t->composer ? hash(t->composer) : 0;
    t->length = id3->length;
    t->tracknum = id3->tracknum;
    t->discnum = id3->discnum;
}

static void library_flag(unsigned id, uint8_t value)
{
    if (msg_start(LIBRARY_UPDATE))
    {
        param_str(0, LIBRARY_ID);
        param_u8(id, value);
        msg_send();
    }
}

/* Updates while there is room, each as many tracks as a packet holds. */
static void library_pump(void)
{
    while (lib.active && usb_iap2_room() > 2 && msg_start(LIBRARY_UPDATE))
    {
        int items = 0;
        param_str(0, LIBRARY_ID);
        if (lib.reset)
        {
            param(6, NULL, 0);
            lib.reset = false;
        }
        while (lib.next < lib.slots)
        {
            struct iap_library_track t;
            if (!iap_library_track(lib.next, &t))
            {
                /* The database has left RAM: the tick sends it again, from
                 * the start, once it is back */
                lib.active = false;
                lib.waiting = true;
                break;
            }
            if (!t.key)
            {
                lib.next++;
                continue;
            }
            const size_t mark = m.len;
            put_track(&t, lib.mask);
            /* Room left for the revision and the progress after it */
            if (!m.ok || m.len + 4 + sizeof(lib.rev) + 5 > m.room)
            {
                m.len = mark;
                m.ok = true;
                if (!items)
                    lib.next++; /* a track no packet holds */
                break;
            }
            lib.next++;
            items++;
        }
        const bool done = lib.active && lib.next >= lib.slots;
        param_str(1, done ? lib.rev : PARTIAL_REVISION);
        param_u8(7, done ? 100 : 0);
        msg_send();
        if (done)
        {
            lib.active = false;
            event(EVENT_LIBRARY_SENT, lib.slots, 0);
        }
    }
}

static void library_start(uint32_t revision, uint32_t mask,
                          const char *car_revision);

static void send_library_updates(uint32_t mask, const char *car_revision)
{
    const uint32_t revision = iap_library_revision();
    struct mp3entry *id3;

    library_flag(8, 0);
    library_flag(9, 1);
    if (!revision)
    {
        if (msg_start(LIBRARY_UPDATE))
        {
            param_str(0, LIBRARY_ID);
            if ((id3 = audio_current_track()))
            {
                struct iap_library_track t;
                track_from_id3(id3, &t);
                put_track(&t, mask);
            }
            /* Always revision 1: the track goes again at every connection */
            param_str(1, "1");
            param_u8(7, 100);
            msg_send();
        }
        /* The database loads into RAM a while after a start; the car's
         * request is a subscription, so the whole library follows unasked
         * when it can, as an iPhone sends a change. */
        lib.waiting = true;
        lib.mask = mask;
        return;
    }
    library_start(revision, mask, car_revision);
}

/* The whole library, unless the car holds this revision already. */
static void library_start(uint32_t revision, uint32_t mask,
                          const char *car_revision)
{
    lib.waiting = false;
    snprintf(lib.rev, sizeof(lib.rev), "%lu", (unsigned long)revision);
    if (car_revision && !strcmp(car_revision, lib.rev))
    {
        if (msg_start(LIBRARY_UPDATE))
        {
            param_str(0, LIBRARY_ID);
            param_str(1, lib.rev);
            param_u8(7, 100);
            msg_send();
        }
        return;
    }
    lib.active = true;
    lib.reset = true;
    lib.next = 0;
    lib.slots = iap_library_track_slots();
    lib.mask = mask;
    library_pump();
}

static void artwork_pump(void);

void usb_iap2_control_room(void)
{
    artwork_pump();
    library_pump();
}

/* ---- now playing -------------------------------------------------------- */

/* MediaItemAttributes and PlaybackAttributes sub-parameters. */
#define MI_PERSISTENT_ID 0x00
#define MI_TITLE         0x01
#define MI_DURATION      0x04
#define MI_ALBUM         0x06
#define MI_TRACK_NUMBER  0x07
#define MI_ARTIST        0x0C
#define MI_ALBUM_ARTIST  0x0E
#define MI_LIKE_SUPPORTED 0x15 /* 0x15 to 0x18: like and ban, bools */
#define MI_BANNED        0x18
#define MI_CHAPTER_COUNT 0x1B
#define MI_ARTWORK       0x1A /* a file transfer's ID */
#define PB_STATE         0x00
#define PB_POSITION      0x01
#define PB_QUEUE_INDEX   0x02
#define PB_QUEUE_COUNT   0x03
#define PB_CHAPTER_INDEX 0x04
#define PB_SHUFFLE       0x05
#define PB_REPEAT        0x06
#define PB_APP_NAME      0x07
#define PB_LIBRARY       0x08 /* the library the track is in */
#define PB_SPEED         0x0C /* percent */
#define PB_SEEKABLE      0x0D
#define PB_APP_BUNDLE    0x10

#define APP_BUNDLE "org.podbox.player"

#define DEFAULT_MEDIA \
    (1u << MI_PERSISTENT_ID | 1u << MI_TITLE | 1u << MI_DURATION | \
     1u << MI_ALBUM | 1u << MI_ARTIST)
#define DEFAULT_PLAYBACK \
    (1u << PB_STATE | 1u << PB_POSITION | 1u << PB_QUEUE_INDEX | \
     1u << PB_QUEUE_COUNT)

/* The attribute IDs a StartNowPlayingUpdates group lists, as a mask. */
static uint32_t wanted(const uint8_t *g, size_t len, uint32_t fallback)
{
    if (!g)
        return fallback;
    uint32_t mask = 0;
    for (size_t off = 0; off + 4 <= len;)
    {
        const size_t plen = g[off] << 8 | g[off + 1];
        const unsigned id = g[off + 2] << 8 | g[off + 3];
        if (plen < 4)
            break;
        if (id < 32)
            mask |= 1u << id;
        off += plen;
    }
    return mask;
}

static uint32_t hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* The tag's title, else the file name. */
static const char *track_title(const struct mp3entry *id3)
{
    const char *title = id3->title;
    if (!title)
    {
        title = strrchr(id3->path, '/');
        title = title ? title + 1 : id3->path;
    }
    return title;
}

static int play_state(void)
{
    const int status = audio_status();
    return status & AUDIO_STATUS_PAUSE ? 2 : status & AUDIO_STATUS_PLAY ? 1 : 0;
}

/* Artwork goes on the file transfer session, a transfer named by its ID in
 * the track's attribute 26. An iPhone opens one with every track, even with
 * nothing to send: [ID, 04 setup, 8-byte size, 00 02 for artwork]; the car
 * answers [ID, 01] to go; the data follows in pieces of [ID, op, bytes], op
 * C0 for the only piece, else 80 the first, 00 the middle ones and 40 the
 * last; the car answers [ID, 05], and [ID, 02] cancels either way. Without
 * it this car waits half a second before its media library. The JPEG is
 * read on the library's worker; the setup goes when it has been found, or
 * empty after 300 ms, inside the half second. Op values for pieces after
 * the first from Nocturne (session/file_transfer.rs), facts only. */

static void file_packet(uint8_t id, uint8_t op, const uint8_t *data,
                        size_t n)
{
    size_t room;
    uint8_t *b = usb_iap2_message_start(&room);
    if (!b || n + 2 > room)
        return;
    b[0] = id;
    b[1] = op;
    if (n)
        memcpy(b + 2, data, n);
    usb_iap2_file_send(n + 2);
}

static void announce_artwork(uint32_t size)
{
    const uint8_t setup[10] = {0, 0, 0, 0, size >> 24, size >> 16,
                               size >> 8, size, 0, 2};
    file_packet(aw.id, 0x04, setup, sizeof(setup));
    aw.size = size;
    aw.phase = ART_ANNOUNCED;
}

/* A new track's transfer, the last one cancelled if it is still going. */
static void start_artwork(uint8_t id, const struct mp3entry *id3)
{
    if (aw.phase != ART_IDLE)
    {
        iap_library_artwork_stop();
        file_packet(aw.id, 0x02, NULL, 0);
    }
    aw.id = id;
    aw.sent = 0;
    if (iap_library_artwork_find(id3))
    {
        aw.phase = ART_FINDING;
        aw.started = current_tick;
    }
    else
        announce_artwork(0);
}

static void artwork_pump(void)
{
    uint32_t size;
    if (aw.phase == ART_FINDING)
    {
        const int state = iap_library_artwork_state(&size);
        if (state == IAP_ART_FOUND)
            announce_artwork(size);
        else if (state != IAP_ART_FINDING ||
                 TIME_AFTER(current_tick, aw.started + HZ * 3 / 10))
        {
            iap_library_artwork_stop();
            announce_artwork(0);
        }
    }
    while (aw.phase == ART_SENDING && usb_iap2_room() > 1)
    {
        const uint8_t *data;
        const size_t n = iap_library_artwork_chunk(&data);
        if (!n)
        {
            if (iap_library_artwork_state(&size) == IAP_ART_FAILED)
            {
                file_packet(aw.id, 0x02, NULL, 0);
                aw.phase = ART_IDLE;
            }
            break;
        }
        const bool first = !aw.sent, last = aw.sent + n >= aw.size;
        file_packet(aw.id, first ? (last ? 0xC0 : 0x80) : (last ? 0x40 : 0x00),
                    data, n);
        iap_library_artwork_next();
        aw.sent += n;
        if (last)
            aw.phase = ART_IDLE;    /* the car's 05 needs no answer */
    }
}

void usb_iap2_control_file(const uint8_t *data, size_t len)
{
    if (len < 2 || data[0] != aw.id)
        return;
    if (data[1] == 0x01 && aw.phase == ART_ANNOUNCED)
    {
        if (aw.size)
        {
            aw.phase = ART_SENDING;
            artwork_pump();
        }
        else
        {
            file_packet(aw.id, 0xC0, NULL, 0);
            aw.phase = ART_IDLE;
        }
    }
    else if (data[1] == 0x02 && aw.phase != ART_IDLE)
    {
        iap_library_artwork_stop();
        aw.phase = ART_IDLE;
    }
}

static void send_now_playing(bool media)
{
    struct mp3entry *id3 = audio_current_track();
    if (!msg_start(NOW_PLAYING_UPDATE))
        return;
    if (media && id3)
    {
        group_start(0);
        if (media_mask & 1u << MI_PERSISTENT_ID)
        {
            const uint64_t key = iap_library_key(id3->path);
            param_u64(MI_PERSISTENT_ID, key >> 32, key);
        }
        if (media_mask & 1u << MI_TITLE)
            param_str(MI_TITLE, track_title(id3));
        if (media_mask & 1u << MI_DURATION)
            param_u32(MI_DURATION, id3->length);
        if ((media_mask & 1u << MI_ALBUM) && id3->album)
            param_str(MI_ALBUM, id3->album);
        if ((media_mask & 1u << MI_TRACK_NUMBER) && id3->tracknum > 0)
            param_u16(MI_TRACK_NUMBER, id3->tracknum);
        if ((media_mask & 1u << MI_ARTIST) && id3->artist)
            param_str(MI_ARTIST, id3->artist);
        if ((media_mask & 1u << MI_ALBUM_ARTIST) && id3->albumartist)
            param_str(MI_ALBUM_ARTIST, id3->albumartist);
        for (unsigned a = MI_LIKE_SUPPORTED; a <= MI_BANNED; a++)
            if (media_mask & 1u << a)
                param_u8(a, 0);
        if (media_mask & 1u << MI_CHAPTER_COUNT)
            param_u16(MI_CHAPTER_COUNT, 0);
        if (media_mask & 1u << MI_ARTWORK)
        {
            artwork = next_transfer;
            next_transfer = next_transfer == 0xFF ? 0x80 : next_transfer + 1;
            param_u8(MI_ARTWORK, artwork);
        }
        group_end();
    }
    group_start(1);
    if (playback_mask & 1u << PB_STATE)
        param_u8(PB_STATE, np_state);
    if (playback_mask & 1u << PB_POSITION)
        param_u32(PB_POSITION, np_elapsed);
    if (playback_mask & 1u << PB_QUEUE_INDEX)
        param_u32(PB_QUEUE_INDEX, MAX(0, playlist_get_display_index() - 1));
    if (playback_mask & 1u << PB_QUEUE_COUNT)
        param_u32(PB_QUEUE_COUNT, playlist_amount());
    if (playback_mask & 1u << PB_CHAPTER_INDEX)
        param_u32(PB_CHAPTER_INDEX, 0);
    if (playback_mask & 1u << PB_SHUFFLE)
        param_u8(PB_SHUFFLE, global_settings.playlist_shuffle ? 1 : 0);
    if (playback_mask & 1u << PB_REPEAT)
        param_u8(PB_REPEAT, global_settings.repeat_mode == REPEAT_ONE ? 1 :
                            global_settings.repeat_mode == REPEAT_ALL ? 2 : 0);
    if (playback_mask & 1u << PB_APP_NAME)
        param_str(PB_APP_NAME, IAP_PLAYER_NAME_DEFAULT);
    if (playback_mask & 1u << PB_LIBRARY)
        param_str(PB_LIBRARY, LIBRARY_ID);
    if (playback_mask & 1u << PB_SPEED)
        param_u16(PB_SPEED, 100);
    if (playback_mask & 1u << PB_SEEKABLE)
        param_u8(PB_SEEKABLE, 1);
    if (playback_mask & 1u << PB_APP_BUNDLE)
        param_str(PB_APP_BUNDLE, APP_BUNDLE);
    group_end();
    msg_send();
    event(USB_LOG_IAP2_NOW_PLAYING, np_key, np_state << 24 | np_elapsed / 1000);
    if (artwork && id3)
        start_artwork(artwork, id3);
    artwork = 0;
}

/* The position alone, as an iPhone sends it about twice a second while
 * playing: this car does not run the position on itself, and its progress
 * bar stays at 0:00 without. */
static void send_position(unsigned long elapsed)
{
    if (!(playback_mask & 1u << PB_POSITION) || !msg_start(NOW_PLAYING_UPDATE))
        return;
    group_start(1);
    param_u32(PB_POSITION, elapsed);
    group_end();
    msg_send();
}

/* What changed since the last update: the track, the state, or a position
 * more than three seconds from where the last one runs on to; and while
 * playing, the position every half second. */
static void poll_now_playing(void)
{
    struct mp3entry *id3 = audio_current_track();
    const uint32_t key = id3 ? hash(id3->path) : 0;
    const int state = play_state();
    const unsigned long elapsed = id3 ? id3->elapsed : 0;
    unsigned long expected = np_elapsed;
    if (np_state == 1)
        expected += (current_tick - np_tick) * 1000 / HZ;
    const long drift = (long)elapsed - (long)expected;
    const bool track = key != np_key || np_full;
    if (!track && state == np_state && drift < 3000 && drift > -3000)
    {
        if (state == 1 && TIME_AFTER(current_tick, np_tick + HZ / 2))
        {
            np_elapsed = elapsed;
            np_tick = current_tick;
            send_position(elapsed);
        }
        return;
    }
    np_key = key;
    np_state = state;
    np_elapsed = elapsed;
    np_tick = current_tick;
    np_full = false;
    send_now_playing(track);
}

/* ---- the car's buttons -------------------------------------------------- */

/* The input controls of a HID report descriptor on the consumer page: each
 * one-bit variable as itself, each array as its run of usages. */
static void parse_hid(const uint8_t *d, size_t len)
{
    uint32_t page = 0, size = 0, count = 0, id = 0, logical_min = 0;
    uint32_t usages[16], usage_min = 0;
    size_t n_usages = 0;
    uint32_t bits[8] = {0}; /* per report ID, mod 8 */

    n_controls = 0;
    report_ids = false;
    for (size_t i = 0; i < len;)
    {
        const uint8_t prefix = d[i];
        if (prefix == 0xFE) /* a long item */
        {
            i += 3 + (i + 1 < len ? d[i + 1] : 0);
            continue;
        }
        const size_t n = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (i + 1 + n > len)
            break;
        const uint32_t v = n == 4 ? d[i + 1] | d[i + 2] << 8 |
                                    (uint32_t)d[i + 3] << 16 |
                                    (uint32_t)d[i + 4] << 24
                         : n == 2 ? (uint32_t)(d[i + 1] | d[i + 2] << 8)
                         : n == 1 ? d[i + 1] : 0u;
        i += 1 + n;
        switch (prefix & 0xFC)
        {
        case 0x04: page = v; break;          /* Usage Page */
        case 0x14: logical_min = v; break;   /* Logical Minimum */
        case 0x74: size = v; break;          /* Report Size */
        case 0x84: id = v; report_ids = true; break;
        case 0x94: count = v; break;         /* Report Count */
        case 0x08:                           /* Usage */
            if (n_usages < ARRAYLEN(usages))
                usages[n_usages++] = n == 4 ? v : page << 16 | v;
            break;
        case 0x18: usage_min = v; break;     /* Usage Minimum */
        case 0x80:                           /* Input */
        {
            uint32_t *at = &bits[id & 7];
            if (!(v & 1) && page == 0x0C && n_controls < MAX_CONTROLS)
            {
                if (v & 2)
                {
                    for (uint32_t k = 0; k < count && k < n_usages; k++)
                        if (size == 1 && usages[k] >> 16 == 0x0C &&
                            n_controls < MAX_CONTROLS)
                            controls[n_controls++] = (struct control){
                                id, *at + k, 1, 1, usages[k] & 0xFFFF};
                }
                /* field() reads at most 32 bits, and each report is
                 * compared count squared times */
                else if (size && size <= 32 && count <= 16)
                    controls[n_controls++] = (struct control){
                        id, *at, size, count, usage_min - logical_min};
            }
            *at += size * count;
        }
            /* fall through - a main item clears the local items */
        case 0x90: case 0xB0: case 0xA0: case 0xC0:
            n_usages = 0;
            usage_min = 0;
            break;
        }
    }
}

static uint32_t field(const uint8_t *r, size_t len, unsigned bit,
                      unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++, bit++)
        if (bit / 8 < len && (r[bit / 8] >> (bit % 8) & 1))
            v |= 1u << i;
    return v;
}

static void press(uint16_t usage)
{
    long button = BUTTON_NONE;
    const int state = play_state();
    switch (usage)
    {
    case 0xCD: button = BUTTON_MULTIMEDIA_PLAYPAUSE; break;
    case 0xB0: if (state != 1) button = BUTTON_MULTIMEDIA_PLAYPAUSE; break;
    case 0xB1: if (state == 1) button = BUTTON_MULTIMEDIA_PLAYPAUSE; break;
    case 0xB5: button = BUTTON_MULTIMEDIA_NEXT; break;
    case 0xB6: button = BUTTON_MULTIMEDIA_PREV; break;
    case 0xB7: button = BUTTON_MULTIMEDIA_STOP; break;
    case 0xB3: button = BUTTON_MULTIMEDIA_FFWD; break;
    case 0xB4: button = BUTTON_MULTIMEDIA_REW; break;
    case 0xE9: button = BUTTON_MULTIMEDIA_VOLUME_UP; break;
    case 0xEA: button = BUTTON_MULTIMEDIA_VOLUME_DOWN; break;
    }
    if (button != BUTTON_NONE)
        button_queue_try_post(button, 0);
    event(USB_LOG_IAP2_BUTTON, usage, button != BUTTON_NONE);
}

/* Each control that is down now and was not in this report's last copy. */
static void take_hid_report(const uint8_t *r, size_t len)
{
    uint8_t id = 0;
    if (report_ids && len)
    {
        id = *r++;
        len--;
    }
    size_t slot = 0;
    while (slot < 4 && last_report_id[slot] != id + 1)
        slot++;
    if (slot == 4)
    {
        slot = 0;
        while (slot < 3 && last_report_id[slot])
            slot++;
        last_report_id[slot] = id + 1;
        memset(last_report[slot], 0, sizeof(last_report[slot]));
    }
    uint8_t *last = last_report[slot];
    len = MIN(len, sizeof(last_report[slot]));

    for (size_t i = 0; i < n_controls; i++)
    {
        const struct control *c = &controls[i];
        if (c->report != id)
            continue;
        if (c->count == 1 && c->size == 1)
        {
            if (field(r, len, c->bit, 1) && !field(last, len, c->bit, 1))
                press(c->usage);
            continue;
        }
        for (unsigned k = 0; k < c->count; k++)
        {
            const uint32_t v = field(r, len, c->bit + k * c->size, c->size);
            bool was = false;
            for (unsigned j = 0; j < c->count && v; j++)
                was |= field(last, len, c->bit + j * c->size, c->size) == v;
            if (v && !was)
                press(c->usage + v);
        }
    }
    memcpy(last, r, len);
}

/* ---- the conversation --------------------------------------------------- */

static uint32_t rnd;

static void send_challenge(size_t cert_len)
{
    /* MFi 2.0 certificates run about 900 bytes and sign a 20-byte
     * challenge; 3.0 ones about 600, and 32 bytes. */
    const size_t n = cert_len > 800 ? 20 : 32;
    uint8_t challenge[32];
    if (!rnd)
        rnd = USEC_TIMER | 1;
    for (size_t i = 0; i < n; i++)
    {
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        challenge[i] = rnd;
    }
    event(USB_LOG_IAP2_CERTIFICATE, cert_len, n);
    if (msg_start(REQUEST_CHALLENGE_RESPONSE))
    {
        param(0, challenge, n);
        msg_send();
    }
}

/* CarPlay is never available: an iPhone with it switched off, for a car to
 * fall back to plain USB audio. Groups 0 (wired) and 1 (wireless) each hold
 * Available, a bool in sub-parameter 0; the transport identifiers that may
 * follow it are left out. */
static void send_carplay_availability(void)
{
    availability_due = 0;
    if (!msg_start(CARPLAY_AVAILABILITY))
        return;
    group_start(0);
    param_u8(0, 0);
    group_end();
    group_start(1);
    param_u8(0, 0);
    group_end();
    msg_send();
}


static void take_identification(const uint8_t *p, size_t len)
{
    static long last_splash;
    size_t n, sent_n = 0;
    const uint8_t *list = find(p, len, 7, &n);
    car_receives_n = 0;
    for (size_t i = 0; list && i + 1 < n &&
                       car_receives_n < ARRAYLEN(car_receives); i += 2)
        car_receives[car_receives_n++] = list[i] << 8 | list[i + 1];
    if (find(p, len, 6, &sent_n))
        sent_n /= 2;

    iap_player_name(device_name, sizeof(device_name));
    send_empty(IDENTIFICATION_ACCEPTED);
    identified = true;
    event(USB_LOG_IAP2_IDENTIFIED, sent_n, car_receives_n);
    /* Once a connection: a car that resets and identifies again would
     * otherwise hold the screen with one splash after another. */
    if (!last_splash || TIME_AFTER(current_tick, last_splash + 30 * HZ))
    {
        last_splash = current_tick;
        queue_broadcast(SYS_ACCESSORY_CONNECTED, 0);
    }

    /* What an iPhone sends this car's identification, in its order, a
     * packet each (the car reads only the first message of a packet); the
     * car resets without the UUID. */
    if (car_takes(DEVICE_UUID_UPDATE) && msg_start(DEVICE_UUID_UPDATE))
    {
        param_str(0, DEVICE_UUID);
        msg_send();
    }
    if (car_takes(DEVICE_LANGUAGE_UPDATE) && msg_start(DEVICE_LANGUAGE_UPDATE))
    {
        param_str(0, "en");
        msg_send();
    }
    if (car_takes(DEVICE_INFORMATION_UPDATE) &&
        msg_start(DEVICE_INFORMATION_UPDATE))
    {
        param_str(0, device_name);
        msg_send();
    }

    /* The iPhone then starts pairing with the car's Bluetooth component and
     * stops it at once, the stop carrying an empty parameter 2. */
    const uint8_t *bt = find(p, len, 17, &n);
    if (bt && (bt = find(bt, n, 0, &n)) && n == 2 &&
        car_takes(START_BLUETOOTH_PAIRING) && car_takes(STOP_BLUETOOTH_PAIRING))
    {
        const uint16_t component = get_be(bt, 2);
        if (msg_start(START_BLUETOOTH_PAIRING))
        {
            param_u16(0, component);
            msg_send();
        }
        if (msg_start(STOP_BLUETOOTH_PAIRING))
        {
            param_u16(0, component);
            param(2, NULL, 0);
            msg_send();
        }
    }
}

void usb_iap2_control_link_up(void)
{
    send_empty(REQUEST_CERTIFICATE);
}

void usb_iap2_control_receive(const uint8_t *msg, size_t len)
{
    const uint16_t id = msg[4] << 8 | msg[5];
    const uint8_t *p = msg + 6;
    const size_t plen = len - 6;
    const uint8_t *v;
    size_t n;

    usb_log(USB_LOG_IAP2_MSG, 0, len, id, 0);
    log_params(msg, len, id == IDENTIFICATION_INFORMATION ? 512 :
                         id == CERTIFICATE || id == CHALLENGE_RESPONSE ? 16 :
                         64);

    switch (id)
    {
    case CERTIFICATE:
        send_challenge(find(p, plen, 0, &n) ? n : 0);
        break;
    case CHALLENGE_RESPONSE:
        send_empty(AUTHENTICATION_SUCCEEDED);
        send_empty(START_IDENTIFICATION);
        break;
    case IDENTIFICATION_INFORMATION:
        take_identification(p, plen);
        break;
    case START_POWER_UPDATES:
        if (car_takes(POWER_UPDATE))
            send_power_update(wanted(p, plen, 0));
        break;
    case POWER_SOURCE_UPDATE:
        /* CarPlayAvailability before this, or within a millisecond of it,
         * and the car goes quiet after StartHID; 10 ms after, behind
         * StartHID, and it goes on to StartPowerUpdates. An iPhone sends it
         * 8 to 115 ms after accepting identification. Sent from the tick,
         * 50 to 150 ms on, or at StartHID if that comes first. */
        if (identified && car_takes(CARPLAY_AVAILABILITY))
            availability_due = current_tick + HZ / 20;
        break;
    case START_BT_CONNECTION:
        /* An iPhone answers with the component and an empty parameter 1,
         * no Bluetooth profile connected. */
        if (car_takes(BT_CONNECTION_UPDATE) &&
            (v = find(p, plen, 0, &n)) && n == 2 &&
            msg_start(BT_CONNECTION_UPDATE))
        {
            param_u16(0, get_be(v, 2));
            param(1, NULL, 0);
            msg_send();
        }
        break;
    case START_LIBRARY_INFORMATION:
        if (car_takes(LIBRARY_INFORMATION))
            send_library_information();
        break;
    case START_LIBRARY_UPDATES:
        if (car_takes(LIBRARY_UPDATE))
        {
            const uint8_t *rev = find(p, plen, 1, &n);
            const bool has_rev = rev && n && !rev[n - 1];
            v = find(p, plen, 2, &n);
            send_library_updates(wanted(v, n, 0x0A0FFFFF),
                                 has_rev ? (const char *)rev : NULL);
        }
        break;
    case STOP_LIBRARY_UPDATES:
        lib.active = false;
        lib.waiting = false;
        break;
    case PLAY_LIBRARY_ITEMS:
        /* 0 the tracks' keys back to back, 1 the index to start at */
        if ((v = find(p, plen, 0, &n)) && n >= 8)
        {
            const uint8_t *s;
            size_t sn;
            const uint32_t start = (s = find(p, plen, 1, &sn)) && sn == 4
                                   ? get_be(s, 4) : 0;
            iap_library_play_keys(v, n / 8, start);
        }
        break;
    case START_USB_AUDIO:
        start_audio();
        break;
    case STOP_USB_AUDIO:
        stop_audio();
        break;
    case START_NOW_PLAYING:
        v = find(p, plen, 0, &n);
        media_mask = wanted(v, n, DEFAULT_MEDIA);
        v = find(p, plen, 1, &n);
        playback_mask = wanted(v, n, DEFAULT_PLAYBACK);
        now_playing = true;
        np_full = true;
        poll_now_playing();
        break;
    case STOP_NOW_PLAYING:
        now_playing = false;
        break;
    case SET_NOW_PLAYING:
        if ((v = find(p, plen, 0, &n)) && n == 4)
        {
            audio_ff_rewind(get_be(v, 4));
            event(USB_LOG_IAP2_SEEK, get_be(v, 4), 0);
        }
        break;
    case START_HID:
        hid_component = (v = find(p, plen, 0, &n)) && n == 2 ? get_be(v, 2)
                                                             : 0;
        if ((v = find(p, plen, 4, &n)))
            parse_hid(v, n);
        event(USB_LOG_IAP2_HID, n_controls, v ? n : 0);
        if (availability_due)
            send_carplay_availability();
        break;
    case ACCESSORY_HID_REPORT:
        if ((v = find(p, plen, 1, &n)))
            take_hid_report(v, n);
        break;
    case STOP_HID:
        n_controls = 0;
        break;
    }
}

void usb_iap2_control_tick(void)
{
    if (availability_due && TIME_AFTER(current_tick, availability_due))
        send_carplay_availability();
    if (audio_on && rate_wanted && rate_wanted != rate_sent)
        send_rate(rate_wanted);
    if (identified && now_playing)
        poll_now_playing();
    artwork_pump();
    if (lib.waiting && !lib.active)
    {
        const uint32_t revision = iap_library_revision();
        if (revision)
            library_start(revision, lib.mask, NULL);
    }
    library_pump();
}
