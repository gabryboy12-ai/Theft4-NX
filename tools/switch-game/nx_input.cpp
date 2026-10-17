#include "nx_input.h"

#include <atomic>
#include <cstring>

#include <rex/input/input.h>

namespace theft4::nx_input {
namespace {

using namespace rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

// One published state, packed so the readers load it in one go.
struct Packed {
  uint16_t buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  int16_t lx, ly, rx, ry;
};
static_assert(sizeof(Packed) == 12);

std::atomic<uint64_t> g_lo{0};  // buttons, triggers, lx
std::atomic<uint64_t> g_hi{0};  // ly, rx, ry
std::atomic<uint32_t> g_packet{0};
std::atomic<uint64_t> g_published{0};
std::atomic<uint64_t> g_reads{0};
// Run 26 (C): buttons pressed since the last GetState. The game reads the pad
// once per frame, and a frame took 2-10 s in run 25: a press of 0.1-0.3 s
// fell between two reads and the game never saw it. A press now stays in the
// published state until one read has returned it.
std::atomic<uint16_t> g_latched{0};
// Run 26 (C): every read whose buttons differ from the previous read's, with
// the read's tick, for the heartbeat (a ring of 64; older entries are lost
// when the heartbeat does not drain them in time, and counted).
struct ReadEvent {
  uint64_t tick;
  uint16_t buttons, latched;
  uint32_t packet;
};
constexpr uint32_t kReadRing = 64;
ReadEvent g_read_ring[kReadRing];
std::atomic<uint32_t> g_read_head{0};
uint32_t g_read_tail = 0;  // UI thread only
std::atomic<uint16_t> g_last_read_buttons{0};

Packed Load() {
  const uint64_t lo = g_lo.load(std::memory_order_acquire);
  const uint64_t hi = g_hi.load(std::memory_order_acquire);
  Packed p;
  p.buttons = uint16_t(lo);
  p.left_trigger = uint8_t(lo >> 16);
  p.right_trigger = uint8_t(lo >> 24);
  p.lx = int16_t(lo >> 32);
  p.ly = int16_t(hi);
  p.rx = int16_t(hi >> 16);
  p.ry = int16_t(hi >> 32);
  return p;
}

int16_t Axis(s32 v) {
  return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void Fill(X_INPUT_GAMEPAD* g, const Packed& p) {
  g->buttons = p.buttons;
  g->left_trigger = p.left_trigger;
  g->right_trigger = p.right_trigger;
  g->thumb_lx = p.lx;
  g->thumb_ly = p.ly;
  g->thumb_rx = p.rx;
  g->thumb_ry = p.ry;
}

class PadInputSystem final : public rex::system::IInputSystem {
 public:
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void Shutdown() override {}

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t,
                           X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      Packed all{0xF3FF, 0xFF, 0xFF, -1, -1, -1, -1};
      Fill(&out_caps->gamepad, all);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (user_index != 0) return X_ERROR_DEVICE_NOT_CONNECTED;
    g_reads.fetch_add(1, std::memory_order_relaxed);
    if (out_state) {
      Packed p = Load();
      const uint16_t latched = g_latched.exchange(0, std::memory_order_acq_rel);
      const uint16_t extra = uint16_t(latched & ~p.buttons);
      p.buttons |= latched;
      out_state->packet_number = g_packet.load(std::memory_order_acquire) + (extra ? 1 : 0);
      Fill(&out_state->gamepad, p);
      if (g_last_read_buttons.exchange(p.buttons, std::memory_order_relaxed) != p.buttons) {
        const uint32_t i = g_read_head.fetch_add(1, std::memory_order_acq_rel);
        g_read_ring[i % kReadRing] = {armGetSystemTick(), p.buttons, extra,
                                      out_state->packet_number};
      }
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION*) override {
    return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t, X_INPUT_KEYSTROKE*) override {
    return user_index == 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

}  // namespace

std::unique_ptr<rex::system::IInputSystem> CreatePadInputSystem() {
  return std::make_unique<PadInputSystem>();
}

void Publish(const PadState* pad, bool muted) {
  Packed p{};
  if (!muted) {
    const u64 b = padGetButtons(pad);
    struct Map {
      u64 nx;
      uint16_t x;
    };
    static const Map kMap[] = {
        {HidNpadButton_Up, X_INPUT_GAMEPAD_DPAD_UP},
        {HidNpadButton_Down, X_INPUT_GAMEPAD_DPAD_DOWN},
        {HidNpadButton_Left, X_INPUT_GAMEPAD_DPAD_LEFT},
        {HidNpadButton_Right, X_INPUT_GAMEPAD_DPAD_RIGHT},
        {HidNpadButton_Plus, X_INPUT_GAMEPAD_START},
        {HidNpadButton_Minus, X_INPUT_GAMEPAD_BACK},
        {HidNpadButton_StickL, X_INPUT_GAMEPAD_LEFT_THUMB},
        {HidNpadButton_StickR, X_INPUT_GAMEPAD_RIGHT_THUMB},
        {HidNpadButton_L, X_INPUT_GAMEPAD_LEFT_SHOULDER},
        {HidNpadButton_R, X_INPUT_GAMEPAD_RIGHT_SHOULDER},
        {HidNpadButton_B, X_INPUT_GAMEPAD_A},
        {HidNpadButton_A, X_INPUT_GAMEPAD_B},
        {HidNpadButton_Y, X_INPUT_GAMEPAD_X},
        {HidNpadButton_X, X_INPUT_GAMEPAD_Y},
    };
    for (const Map& m : kMap) {
      if (b & m.nx) p.buttons |= m.x;
    }
    if (p.buttons) {
      g_latched.fetch_or(p.buttons, std::memory_order_acq_rel);
    }
    p.left_trigger = (b & HidNpadButton_ZL) ? 0xFF : 0;
    p.right_trigger = (b & HidNpadButton_ZR) ? 0xFF : 0;
    const HidAnalogStickState l = padGetStickPos(pad, 0);
    const HidAnalogStickState r = padGetStickPos(pad, 1);
    p.lx = Axis(l.x);
    p.ly = Axis(l.y);
    p.rx = Axis(r.x);
    p.ry = Axis(r.y);
  }
  const uint64_t lo = uint64_t(p.buttons) | (uint64_t(p.left_trigger) << 16) |
                      (uint64_t(p.right_trigger) << 24) | (uint64_t(uint16_t(p.lx)) << 32);
  const uint64_t hi = uint64_t(uint16_t(p.ly)) | (uint64_t(uint16_t(p.rx)) << 16) |
                      (uint64_t(uint16_t(p.ry)) << 32);
  if (lo != g_lo.load(std::memory_order_relaxed) || hi != g_hi.load(std::memory_order_relaxed)) {
    g_lo.store(lo, std::memory_order_release);
    g_hi.store(hi, std::memory_order_release);
    g_packet.fetch_add(1, std::memory_order_release);
  }
  g_published.fetch_add(1, std::memory_order_relaxed);
}

void DrainReads(void (*line)(uint64_t tick, uint16_t buttons, uint16_t latched,
                               uint32_t packet),
                void (*lost)(uint32_t count)) {
  const uint32_t head = g_read_head.load(std::memory_order_acquire);
  if (head - g_read_tail > kReadRing) {
    lost(head - g_read_tail - kReadRing);
    g_read_tail = head - kReadRing;
  }
  for (; g_read_tail != head; ++g_read_tail) {
    const ReadEvent& e = g_read_ring[g_read_tail % kReadRing];
    line(e.tick, e.buttons, e.latched, e.packet);
  }
}

void GetCounters(uint64_t* published, uint64_t* reads) {
  *published = g_published.load(std::memory_order_relaxed);
  *reads = g_reads.load(std::memory_order_relaxed);
}

}  // namespace theft4::nx_input
