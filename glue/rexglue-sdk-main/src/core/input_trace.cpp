#include <rex/input/input_trace.h>

#include <atomic>

#include <rex/cvar.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_BOOL(input_trace, false, "Input/Diagnostics",
                    "Trace changed keyboard and controller state end to end");

namespace rex::input {

namespace {

std::atomic<uint64_t> g_input_trace_sequence{0};
thread_local uint64_t g_input_trace_causal_sequence = 0;

}  // namespace

bool IsInputTraceEnabled() {
  return REXCVAR_GET(input_trace);
}

uint64_t NextInputTraceSequence() {
  return g_input_trace_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

void BeginInputTracePoll() {
  g_input_trace_causal_sequence = 0;
}

void LinkInputTraceSequence(uint64_t sequence) {
  if (sequence != 0) {
    g_input_trace_causal_sequence = sequence;
  }
}

uint64_t InputTraceCausalSequence() {
  return g_input_trace_causal_sequence;
}

const char* InputTraceVirtualKeyName(uint32_t virtual_key) {
  using rex::ui::VirtualKey;
  switch (static_cast<VirtualKey>(virtual_key)) {
    case VirtualKey::kNone:
      return "none";
    case VirtualKey::kLButton:
      return "mouse-left";
    case VirtualKey::kRButton:
      return "mouse-right";
    case VirtualKey::kMButton:
      return "mouse-middle";
    case VirtualKey::kBack:
      return "backspace";
    case VirtualKey::kTab:
      return "tab";
    case VirtualKey::kReturn:
      return "return";
    case VirtualKey::kShift:
      return "shift";
    case VirtualKey::kControl:
      return "control";
    case VirtualKey::kMenu:
      return "alt";
    case VirtualKey::kPause:
      return "pause";
    case VirtualKey::kCapital:
      return "caps-lock";
    case VirtualKey::kEscape:
      return "escape";
    case VirtualKey::kSpace:
      return "space";
    case VirtualKey::kPrior:
      return "page-up";
    case VirtualKey::kNext:
      return "page-down";
    case VirtualKey::kEnd:
      return "end";
    case VirtualKey::kHome:
      return "home";
    case VirtualKey::kUp:
      return "up";
    case VirtualKey::kDown:
      return "down";
    case VirtualKey::kLeft:
      return "left";
    case VirtualKey::kRight:
      return "right";
    case VirtualKey::kInsert:
      return "insert";
    case VirtualKey::kDelete:
      return "delete";
    case VirtualKey::k0:
      return "0";
    case VirtualKey::k1:
      return "1";
    case VirtualKey::k2:
      return "2";
    case VirtualKey::k3:
      return "3";
    case VirtualKey::k4:
      return "4";
    case VirtualKey::k5:
      return "5";
    case VirtualKey::k6:
      return "6";
    case VirtualKey::k7:
      return "7";
    case VirtualKey::k8:
      return "8";
    case VirtualKey::k9:
      return "9";
    case VirtualKey::kA:
      return "a";
    case VirtualKey::kB:
      return "b";
    case VirtualKey::kC:
      return "c";
    case VirtualKey::kD:
      return "d";
    case VirtualKey::kE:
      return "e";
    case VirtualKey::kF:
      return "f";
    case VirtualKey::kG:
      return "g";
    case VirtualKey::kH:
      return "h";
    case VirtualKey::kI:
      return "i";
    case VirtualKey::kJ:
      return "j";
    case VirtualKey::kK:
      return "k";
    case VirtualKey::kL:
      return "l";
    case VirtualKey::kM:
      return "m";
    case VirtualKey::kN:
      return "n";
    case VirtualKey::kO:
      return "o";
    case VirtualKey::kP:
      return "p";
    case VirtualKey::kQ:
      return "q";
    case VirtualKey::kR:
      return "r";
    case VirtualKey::kS:
      return "s";
    case VirtualKey::kT:
      return "t";
    case VirtualKey::kU:
      return "u";
    case VirtualKey::kV:
      return "v";
    case VirtualKey::kW:
      return "w";
    case VirtualKey::kX:
      return "x";
    case VirtualKey::kY:
      return "y";
    case VirtualKey::kZ:
      return "z";
    case VirtualKey::kNumpad0:
      return "numpad-0";
    case VirtualKey::kNumpad1:
      return "numpad-1";
    case VirtualKey::kNumpad2:
      return "numpad-2";
    case VirtualKey::kNumpad3:
      return "numpad-3";
    case VirtualKey::kNumpad4:
      return "numpad-4";
    case VirtualKey::kNumpad5:
      return "numpad-5";
    case VirtualKey::kNumpad6:
      return "numpad-6";
    case VirtualKey::kNumpad7:
      return "numpad-7";
    case VirtualKey::kNumpad8:
      return "numpad-8";
    case VirtualKey::kNumpad9:
      return "numpad-9";
    case VirtualKey::kMultiply:
      return "numpad-multiply";
    case VirtualKey::kAdd:
      return "numpad-add";
    case VirtualKey::kSubtract:
      return "numpad-subtract";
    case VirtualKey::kDecimal:
      return "numpad-decimal";
    case VirtualKey::kDivide:
      return "numpad-divide";
    case VirtualKey::kF1:
      return "f1";
    case VirtualKey::kF2:
      return "f2";
    case VirtualKey::kF3:
      return "f3";
    case VirtualKey::kF4:
      return "f4";
    case VirtualKey::kF5:
      return "f5";
    case VirtualKey::kF6:
      return "f6";
    case VirtualKey::kF7:
      return "f7";
    case VirtualKey::kF8:
      return "f8";
    case VirtualKey::kF9:
      return "f9";
    case VirtualKey::kF10:
      return "f10";
    case VirtualKey::kF11:
      return "f11";
    case VirtualKey::kF12:
      return "f12";
    case VirtualKey::kF13:
      return "f13";
    case VirtualKey::kF14:
      return "f14";
    case VirtualKey::kF15:
      return "f15";
    case VirtualKey::kF16:
      return "f16";
    case VirtualKey::kF17:
      return "f17";
    case VirtualKey::kF18:
      return "f18";
    case VirtualKey::kF19:
      return "f19";
    case VirtualKey::kF20:
      return "f20";
    case VirtualKey::kF21:
      return "f21";
    case VirtualKey::kF22:
      return "f22";
    case VirtualKey::kF23:
      return "f23";
    case VirtualKey::kF24:
      return "f24";
    case VirtualKey::kOem1:
      return "semicolon";
    case VirtualKey::kOemPlus:
      return "equals";
    case VirtualKey::kOemComma:
      return "comma";
    case VirtualKey::kOemMinus:
      return "minus";
    case VirtualKey::kOemPeriod:
      return "period";
    case VirtualKey::kOem2:
      return "slash";
    case VirtualKey::kOem3:
      return "grave";
    case VirtualKey::kOem4:
      return "left-bracket";
    case VirtualKey::kOem5:
      return "backslash";
    case VirtualKey::kOem6:
      return "right-bracket";
    case VirtualKey::kOem7:
      return "quote";
    default:
      return "other";
  }
}

}  // namespace rex::input
