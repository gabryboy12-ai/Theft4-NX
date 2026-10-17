// The Switch controller as the game's Xbox 360 pad (user 0).
//
// Run 14 diverged from the PC at XamInputGetState: without an input system
// it returned ERROR_DEVICE_NOT_CONNECTED (0x48F), so the game never called
// XamInputGetCapabilities and saw no START. The UI thread reads the pad
// (padUpdate) and publishes it here; the game's threads read the published
// state. Buttons by position, as on the 360 pad: Switch B = A, A = B, Y = X,
// X = Y; + = START, - = BACK; ZL/ZR = triggers.

#pragma once

#include <memory>

#include <switch.h>

#include <rex/system/interfaces/input.h>

namespace theft4::nx_input {

std::unique_ptr<rex::system::IInputSystem> CreatePadInputSystem();

// From the UI thread after padUpdate. muted: the game sees a connected pad
// with nothing pressed (the exit menu is open).
void Publish(const PadState* pad, bool muted);

// Run 26 (C): the reads whose buttons changed since the previous read, oldest
// first (X360 buttons; latched = pressed and released between two reads,
// delivered by the latch), and how many the ring lost. UI thread only.
void DrainReads(void (*line)(uint64_t tick, uint16_t buttons, uint16_t latched, uint32_t packet),
                void (*lost)(uint32_t count));

// Packets published and GetState calls, for the heartbeat.
void GetCounters(uint64_t* published, uint64_t* reads);

}  // namespace theft4::nx_input
