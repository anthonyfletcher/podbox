/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright © 2008 Rafaël Carré
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

#include <stdio.h>
#include <stdbool.h>
#include "system.h"
#include "config.h"
#include "kernel.h"
#include "button.h"
#include "lcd.h"
#include "font.h"
#include "storage.h"
#include "power.h"
#include "usb.h"
#include "adc.h"
#include "pmu-target.h"
#include "pcm-target.h"
#ifdef HAVE_SERIAL
#include "uart-target.h"
#include "uc87xx.h"
#endif
#ifdef HAVE_MIKEY_REMOTE
#include "mikey-target.h"
#endif
#include "clocking-s5l8702.h"

#define DEBUG_CANCEL BUTTON_MENU

/*  Skeleton for adding target specific debug info to the debug menu
 */

#define _DEBUG_PRINTF(a, varargs...) lcd_putsf(0, line++, (a), ##varargs);

extern int lcd_type;
extern int rec_hw_ver;

/* The hardware info screen's lines, handed to addline one at a time; an empty
 * line is a gap. Called again on every refresh, so the values are live. The
 * list holds 48 lines and wraps past that, and this fills 39. */
void dbg_hw_info_lines(void (*addline)(const char *fmt, ...), bool opening)
{
    int i;
    unsigned cpu_hz;

    (void)opening;

    get_system_freqs(&cpu_hz, NULL, NULL);
    addline("CPU:");
    addline("speed: %d MHz", cpu_hz / 1000000);
    addline("current_tick: %d", (unsigned int)current_tick);
    addline("LCD type: %d", lcd_type);
    addline("capture HW type: %d", rec_hw_ver);
#ifdef CLOCKING_DEBUG
    /* show all clocks */
    unsigned f_clk, c_clk, h_clk, p_clk, l_clk, s_clk;

    f_clk = get_system_freqs(&c_clk, &h_clk, &p_clk);
    s_clk = h_clk / soc_get_hsdiv();
    l_clk = h_clk >> ((LCD_CON & 7) + 1); /* div = 2^(val+1) */

    #define MHZ 1000000
    #define TMHZ 100000
    addline("");
    addline("Clocks (MHz):");
    addline("FClk: %d.%d",  f_clk / MHZ, (f_clk % MHZ) / TMHZ);
    addline(" CPU: %d.%d",  c_clk / MHZ, (c_clk % MHZ) / TMHZ);
    addline(" AHB: %d.%d",  h_clk / MHZ, (h_clk % MHZ) / TMHZ);
    addline("  SM1: %d.%d", s_clk / MHZ, (s_clk % MHZ) / TMHZ);
    addline("  LCD: %d.%d", l_clk / MHZ, (l_clk % MHZ) / TMHZ);
    addline(" APB: %d.%d",  p_clk / MHZ, (p_clk % MHZ) / TMHZ);
#endif

    addline("");
    addline("PMU:");
    for(i=0;i<7;i++)
    {
        static const char *const device[] = {"unknown",
                          "unknown",
                          "LCD",
                          "AUDIO",
                          "unknown",
                          "CLICKWHEEL",
                          "ACCESSORY"};
        addline("ldo%d %s: %dmV (%s)",i,
            pmu_read(0x2e + (i << 1))?" on":"off",
            900 + pmu_read(0x2d + (i << 1))*100,
            device[i]);
    }
    addline("cpu voltage: %dmV",625 + pmu_read(0x1e)*25);
    addline("memory voltage: %dmV",625 + pmu_read(0x22)*25);
    /* The LTC4066 charger's pins as driven, not as asked for: HPWR high is
     * 500 mA, SUSP high cuts USB power, C1 high stops battery charging, and
     * CHRG low means it is charging. */
    addline("chg: HPWR %d SUSP %d C1 %d CHRG %d",
            !!(PDAT(11) & 0x40), !!(PDAT(11) & 0x80),
            !!(PDAT(12) & 0x02), !!(PDAT(11) & 0x10));
    addline("backlight: %s", pmu_read(0x29) ? "on" : "off");
    addline("brightness value: %d", pmu_read(0x28));
    addline("USB cable: %s", usb_detect() == USB_INSERTED ? "5V" : "none");
#if CONFIG_CHARGING
    addline("FW present: %s", pmu_firewire_present() ? "true" : "false");
#endif
    addline("holdswitch locked: %s",
            pmu_holdswitch_locked() ? "true" : "false");
#ifdef IPOD_ACCESSORY_PROTOCOL
    addline("accessory present: %s",
            pmu_accessory_present() ? "true" : "false");
#endif
#ifdef HAVE_MIKEY_REMOTE
    /* r4/r5 are live register reads to help characterize the
     * remote-ID behavior across units (it varies; see
     * mikey-6g.c). r4 bit6 = ID bit, r5 = event register. */
    addline("mikey remote ctrl: %s r4=%02x r5=%02x",
            mikey_present() ? "ok" : "--",
            mikey_read(4), mikey_read(5));
    {
        unsigned char r0 = 0;
        int rc = mikey_probe(&r0);
        addline("  jack=%d hw=%d probe rc=%d r0=%02x",
                headphones_inserted() ? 1 : 0, rec_hw_ver, rc, r0);
    }
#endif
    addline("");
    addline("ADC:");
    addline("%s: %d mV", adc_name(ADC_BATTERY), adc_read_battery_voltage());
    addline("%s: %d Ohms", adc_name(ADC_ACCESSORY),
            adc_read_accessory_resistor());
    addline("USB D+: %d mV", adc_read_usbdata_voltage(true));
    addline("USB D-: %d mV", adc_read_usbdata_voltage(false));

#ifdef UC87XX_DEBUG
    extern struct uartc_port ser_port;
    bool opened = !!ser_port.uartc->port_l[ser_port.id];
    addline("");
    addline("UART %d: %s", ser_port.id, opened ? "opened":"closed");
    if (opened)
    {
        int tx_stat, rx_stat, tx_speed, rx_speed;
        char line_cfg[4];
        int abr_stat;
        uint32_t abr_cnt;
        static const char * const abrstatus[] = {"Idle", "Launched", "Counting", "Abnormal"};

        uartc_port_get_line_info(&ser_port,
                    &tx_stat, &rx_stat, &tx_speed, &rx_speed, line_cfg);
        abr_stat = uartc_port_get_abr_info(&ser_port, &abr_cnt);

        addline("line: %s", line_cfg);
        addline("Tx: %s, speed: %d", tx_stat ? "On":"Off", tx_speed);
        addline("Rx: %s, speed: %d", rx_stat ? "On":"Off", rx_speed);
        addline("ABR: %s, cnt: %u", abrstatus[abr_stat], abr_cnt);
    }
    addline("n_tx_bytes: %u", ser_port.n_tx_bytes);
    addline("n_rx_bytes: %u", ser_port.n_rx_bytes);
    addline("n_ovr_err: %u", ser_port.n_ovr_err);
    addline("n_parity_err: %u", ser_port.n_parity_err);
    addline("n_frame_err: %u", ser_port.n_frame_err);
    addline("n_break_detect: %u", ser_port.n_break_detect);
    addline("ABR n_abnormal: %u %u",
            ser_port.n_abnormal0, ser_port.n_abnormal1);
#endif
}

bool dbg_ports(void)
{
    int line;

    lcd_setfont(FONT_SYSFIXED);

    while(1)
    {
        lcd_clear_display();
        line = 0;
        
        _DEBUG_PRINTF("GPIO  0: %08x",(unsigned int)PDAT(0));
        _DEBUG_PRINTF("GPIO  1: %08x",(unsigned int)PDAT(1));
        _DEBUG_PRINTF("GPIO  2: %08x",(unsigned int)PDAT(2));
        _DEBUG_PRINTF("GPIO  3: %08x",(unsigned int)PDAT(3));
        _DEBUG_PRINTF("GPIO  4: %08x",(unsigned int)PDAT(4));
        _DEBUG_PRINTF("GPIO  5: %08x",(unsigned int)PDAT(5));
        _DEBUG_PRINTF("GPIO  6: %08x",(unsigned int)PDAT(6));
        _DEBUG_PRINTF("GPIO  7: %08x",(unsigned int)PDAT(7));
        _DEBUG_PRINTF("GPIO  8: %08x",(unsigned int)PDAT(8));
        _DEBUG_PRINTF("GPIO  9: %08x",(unsigned int)PDAT(9));
        _DEBUG_PRINTF("GPIO 10: %08x",(unsigned int)PDAT(10));
        _DEBUG_PRINTF("GPIO 11: %08x",(unsigned int)PDAT(11));
        _DEBUG_PRINTF("GPIO 12: %08x",(unsigned int)PDAT(12));
        _DEBUG_PRINTF("GPIO 13: %08x",(unsigned int)PDAT(13));
        _DEBUG_PRINTF("GPIO 14: %08x",(unsigned int)PDAT(14));
        _DEBUG_PRINTF("GPIO 15: %08x",(unsigned int)PDAT(15));
        _DEBUG_PRINTF("USEC   : %08x",(unsigned int)USEC_TIMER);

        lcd_update();
        if (button_get_w_tmo(HZ/10) == (DEBUG_CANCEL|BUTTON_REL))
            break;
    }
    lcd_setfont(FONT_UI);
    return false;
}

