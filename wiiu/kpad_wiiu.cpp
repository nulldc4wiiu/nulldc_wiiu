#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>
#include "kpad_wiiu.h"
#include <stdio.h>

void wiiu_kpad_init()
{
    static bool done;
    if (done)
        return;
    done = true;
    KPADInit();
    WPADEnableURCC(TRUE);   // Pro Controllers
}

// wut's Wii Remote bits to libogc's
static uint32_t core_buttons(uint32_t h)
{
    static const struct { uint32_t wut, ogc; } map[] = {
        { WPAD_BUTTON_LEFT, 0x0100 }, { WPAD_BUTTON_RIGHT, 0x0200 }, { WPAD_BUTTON_DOWN, 0x0400 },
        { WPAD_BUTTON_UP, 0x0800 },   { WPAD_BUTTON_PLUS, 0x1000 },  { WPAD_BUTTON_2, 0x0001 },
        { WPAD_BUTTON_1, 0x0002 },    { WPAD_BUTTON_B, 0x0004 },     { WPAD_BUTTON_A, 0x0008 },
        { WPAD_BUTTON_MINUS, 0x0010 }, { WPAD_BUTTON_HOME, 0x0080 },
    };
    uint32_t b = 0;
    for (const auto &m : map)
        if (h & m.wut)
            b |= m.ogc;
    return b;
}

static bool want_dpd[4];
static uint32_t dpd_reads[4], dpd_valid[4];
static bool is_pro[4];   // the channel's controller is a Pro Controller (it has a motor the player holds)
void wiiu_kpad_read(int chan, WiiuPad *pad)
{
    KPADStatus st;
    KPADError err;
    if (want_dpd[chan & 3] && ++dpd_reads[chan & 3] % 300 == 0)
    {
        // the light gun's pointer: enabling it before the remote is connected doesn't take, so keep asking while
        // no position comes, and say how it goes
        printf("[kpad] remote %d pointer: %u of the last 300 reads had a position\n", chan, dpd_valid[chan & 3]);
        if (!dpd_valid[chan & 3])
        {
            KPADEnableDPD((KPADChan)chan);
            VPADSetSensorBar(VPAD_CHAN_0, TRUE);
        }
        dpd_valid[chan & 3] = 0;
    }
    if (KPADReadEx((KPADChan)chan, &st, 1, &err) < 1)
    {
        if (err != KPAD_ERROR_NO_SAMPLES)
        {
            *pad = {};   // gone
            is_pro[chan & 3] = false;
        }
        return;
    }
    *pad = {};
    is_pro[chan & 3] = false;
    pad->connected = true;
    if (st.posValid > 0)
    {
        dpd_valid[chan & 3]++;
        pad->pointing = true;
        pad->px = st.pos.x;
        pad->py = st.pos.y;
    }
    switch (st.extensionType)
    {
    case WPAD_EXT_NUNCHUK:
    case WPAD_EXT_MPLUS_NUNCHUK:
        pad->ext = 1;
        pad->core = core_buttons(st.hold);
        pad->exp = (st.hold & WPAD_BUTTON_Z ? 1 : 0) | (st.hold & WPAD_BUTTON_C ? 2 : 0);
        pad->lx = st.nunchuk.stick.x;
        pad->ly = st.nunchuk.stick.y;
        break;
    case WPAD_EXT_CLASSIC:
    case WPAD_EXT_MPLUS_CLASSIC:
        // wut's classic bits are libogc's shifted down by 16
        pad->ext = 2;
        pad->core = core_buttons(st.hold);
        pad->exp = st.classic.hold & 0xFFFF;
        pad->lx = st.classic.leftStick.x;
        pad->ly = st.classic.leftStick.y;
        break;
    case WPAD_EXT_PRO_CONTROLLER:
        // reported as a Classic Controller, whose buttons it shares bit for bit
        is_pro[chan & 3] = true;
        pad->ext = 2;
        pad->exp = st.pro.hold & 0xFFFF;
        pad->lx = st.pro.leftStick.x;
        pad->ly = st.pro.leftStick.y;
        break;
    default:
        pad->core = core_buttons(st.hold);
        break;
    }
}

void wiiu_kpad_pointer(int chan, bool on)
{
    want_dpd[chan & 3] = on;
    // the sensor bar is powered from the console's side and stays off unless an app asks (the Wii U Menu does when
    // it needs the pointer): without it the remote's camera sees nothing
    if (on)
    {
        VPADSetSensorBar(VPAD_CHAN_0, TRUE);
        KPADEnableDPD((KPADChan)chan);
    }
    else
        KPADDisableDPD((KPADChan)chan);
}

void wiiu_kpad_motor(int chan, bool on)
{
    static bool running[4];
    if (chan < 0 || chan > 3 || (on && !is_pro[chan]) || running[chan] == on)
        return;
    WPADControlMotor((WPADChan)chan, on);
    running[chan] = on;
}
