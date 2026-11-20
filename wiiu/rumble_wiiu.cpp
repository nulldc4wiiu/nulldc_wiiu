// rumble=1: the Puru Puru Pack (dc/maple/maple_devs.cpp maple_sega_purupuru) on the player's motors, as the pcsx
// player does it. The GamePad (player 1) plays a 120 Hz bit pattern of up to 1 s: the share of set bits is the pack's
// power, renewed every half second while it runs. A Pro Controller's motor is on or off. Classic Controllers and
// Wii Remotes have no motor in the player's hands.
// The pack runs on the emulator thread; the motors are driven from the main loop (wiiu_rumble_tick), and stopped
// while the HOME menu is open and at exit (wiiu_rumble_stop).
#include <stdint.h>
#include <stdio.h>
#include <coreinit/time.h>
#include <vpad/input.h>
#include "kpad_wiiu.h"

extern "C" { int wiiu_rumble; }
#ifdef WIIU_INSTRUMENT
int wiiu_rumble_test;   // rumble_test=s: player 1's pack at full power for 1 s at s seconds (test runs)
#endif

namespace
{
struct Player
{
    volatile float power;      // what the game asked for
    volatile OSTime until;     // and until when
    // the main loop's side
    float playing;
    OSTime renew;
};
Player players[4];
uint32_t changes;

void motors(int p, float power)
{
    if (p == 0)
    {
        if (power > 0.0f)
        {
            // power as a share of the 120 bits: 1/7 one bit in 7, 7/7 every bit
            uint8_t pattern[15] = {};
            const int every = power >= 0.99f ? 1 : (int)(1.0f / power + 0.5f);
            for (int b = 0; b < 120; b += every)
                pattern[b >> 3] |= 0x80 >> (b & 7);
            VPADControlMotor(VPAD_CHAN_0, pattern, 120);
        }
        else
            VPADStopMotor(VPAD_CHAN_0);
    }
    wiiu_kpad_motor(p, power > 0.0f);
}
}

// the pack (emulator thread): power 0..1 for ms milliseconds
extern "C" void wiiu_rumble_set(int bus, float power, uint32_t ms)
{
    if (bus < 0 || bus > 3)
        return;
    Player &pl = players[bus];
    pl.power = power;
    pl.until = OSGetSystemTime() + OSMillisecondsToTicks(ms);
    if (++changes <= 20)
        printf("[rumble] player %d: power %.2f for %u ms\n", bus + 1, power, ms);
}

// the main loop, once a frame
void wiiu_rumble_tick(uint32_t frame)
{
    if (!wiiu_rumble)
        return;
#ifdef WIIU_INSTRUMENT
    if (wiiu_rumble_test && frame == (uint32_t)wiiu_rumble_test * 60)
        wiiu_rumble_set(0, 1.0f, 1000);
#endif
    const OSTime now = OSGetSystemTime();
    for (int p = 0; p < 4; p++)
    {
        Player &pl = players[p];
        const float want = (int64_t)(pl.until - now) > 0 ? pl.power : 0.0f;
        if (want != pl.playing || (want > 0.0f && (int64_t)(now - pl.renew) > 0))
        {
            if (want != pl.playing && p == 0 && changes <= 40)
                printf("[rumble] player 1 motor %s\n", want > 0.0f ? "on" : "off");
            motors(p, want);
            pl.playing = want;
            pl.renew = now + OSMillisecondsToTicks(500);
        }
    }
}

// the HOME menu opens, the app goes to the background, or the game stops: every motor off now; a pack still running
// stays off until the game sets it again
void wiiu_rumble_stop()
{
    if (!wiiu_rumble)
        return;
    for (int p = 0; p < 4; p++)
    {
        players[p].until = 0;
        players[p].playing = 0.0f;
        motors(p, 0.0f);
    }
}
