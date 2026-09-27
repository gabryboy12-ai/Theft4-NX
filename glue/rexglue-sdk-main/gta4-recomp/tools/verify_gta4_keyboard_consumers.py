#!/usr/bin/env python3
"""Check PC keyboard hooks against the C++ actually compiled for GTA IV."""

from pathlib import Path
import re


TITLE = Path(__file__).resolve().parents[1]
GENERATED = TITLE / "generated"
ACTION_BASE = 2328
ACTION_STRIDE = 12


def body(name: str) -> str:
    pattern = re.compile(r"DEFINE_REX_FUNC\(" + re.escape(name) + r"\) \{")
    for path in GENERATED.glob("gta4_recomp.*.cpp"):
        source = path.read_text()
        match = pattern.search(source)
        if match:
            end = source.find("\nDEFINE_REX_FUNC(", match.end())
            return source[match.start():end if end != -1 else len(source)]
    raise AssertionError(f"Missing compiled function: {name}")


def action_offset(action: int) -> int:
    return ACTION_BASE + action * ACTION_STRIDE


def predicate_site(source: str, action: int, callee: str, caller: int) -> None:
    expected = (
        rf"ctx\.r3\.s64 = ctx\.r28\.s64 \+ {action_offset(action)};"
        rf"\s*// bl 0x[0-9a-f]+\s*ctx\.lr = 0x{caller:08X};"
        rf"\s*{callee}\(ctx, base\);"
    )
    assert re.search(expected, source), (action, callee, hex(caller))


def native_registration(function: str) -> dict[int, int]:
    """Evaluate only constant addresses passed to the retail native registrar."""
    registers: dict[str, int] = {}
    result: dict[int, int] = {}
    for line in body(function).splitlines():
        literal = re.fullmatch(r"\s*ctx\.(r\d+)\.s64 = (-?\d+);", line)
        addition = re.fullmatch(
            r"\s*ctx\.(r\d+)\.s64 = ctx\.(r\d+)\.s64 \+ (-?\d+);", line
        )
        if literal:
            registers[literal[1]] = int(literal[2])
        elif addition and addition[2] in registers:
            registers[addition[1]] = registers[addition[2]] + int(addition[3])
        elif line.strip() == "sub_82845600(ctx, base);":
            result[registers["r3"] & 0xFFFFFFFF] = registers["r4"] & 0xFFFFFFFF
            registers.clear()
    return result


def verify_phone_ownership() -> None:
    # This source is address/string metadata, never the excavation pseudocode.
    strings = (TITLE.parents[2] / "gta_iv/xex_excavation_retail/all_strings_with_addrs.txt").read_text()
    registrations = native_registration("sub_8217CCB0")
    for address, name, native in (
        (0x820BD6D0, "CREATE_MOBILE_PHONE", 0x8217D140),
        (0x820BD6E4, "DESTROY_MOBILE_PHONE", 0x8217D150),
        (0x820BD814, "SCRIPT_IS_MOVING_MOBILE_PHONE_OFFSCREEN", 0x8217D3C0),
        (0x820BD83C, "CAN_PHONE_BE_SEEN_ON_SCREEN", 0x8217D3E0),
    ):
        assert re.search(rf"^\s*0x{address:08X}\s+{name}\s*$", strings, re.M)
        assert registrations[address] == native
    assert "sub_8217C958(ctx, base);" in body("sub_8217D140")
    assert "sub_82146BE8(ctx, base);" in body("sub_8217D150")
    created = (-2095251456 + 19924) & 0xFFFFFFFF
    moving_offscreen = (-2095251456 + 21324) & 0xFFFFFFFF
    hooks = (TITLE / "src/gta4_input_hooks.cpp").read_text()
    assert f"kPhoneCreatedAddress = 0x{created:08X};" in hooks
    assert f"kPhoneMovingOffscreenAddress = 0x{moving_offscreen:08X};" in hooks
    for function in ("sub_8217C958", "sub_82146BE8"):
        source = body(function)
        assert "ctx.r10.s64 = -2095251456;" in source
        assert "REX_STORE_U8(ctx.r10.u32 + 19924, ctx.r11.u8);" in source
    moving = body("sub_8217D3C0")
    assert "ctx.r10.s64 = -2095251456;" in moving
    assert "REX_STORE_U8(ctx.r10.u32 + 21324, ctx.r11.u8);" in moving
    for function in ("sub_823CA860", "sub_823CE3A8"):
        source = body(function)
        for offset in (19924, 21324):
            assert re.search(
                rf"ctx\.r11\.s64 = -2095251456;\s*// lbz[^\n]+\s*"
                rf"ctx\.r11\.u64 = REX_LOAD_U8\(ctx.r11.u32 \+ {offset}\);", source
            )
    visible = body("sub_8217D3E0")
    assert "sub_821C2FA8(ctx, base);" in visible
    assert "REX_LOAD_U8(ctx.r3.u32 + 17);" in body("sub_821C2FA8")


def verify_pause_ownership() -> None:
    strings = (TITLE.parents[2] / "gta_iv/xex_excavation_retail/all_strings_with_addrs.txt").read_text()
    assert re.search(r"^\s*0x820394E8\s+IS_PAUSE_MENU_ACTIVE\s*$", strings, re.M)
    assert native_registration("sub_825DD3E8")[0x820394E8] == 0x825DAA40
    native = body("sub_825DAA40")
    assert "ctx.r11.s64 = -2101346304;" in native
    assert "REX_LOAD_U32(ctx.r11.u32 + -24260);" in native
    assert "REX_LOAD_U8(ctx.r11.u32 + -24252);" in native
    for exit_state in (2, 6):
        assert f"ctx.cr6.compare<int32_t>(ctx.r11.s32, {exit_state}, ctx.xer);" in native
    hooks = (TITLE / "src/gta4_input_hooks.cpp").read_text()
    transition = (-2101346304 - 24260) & 0xFFFFFFFF
    visible = (-2101346304 - 24252) & 0xFFFFFFFF
    assert f"kPauseMenuTransitionAddress = 0x{transition:08X};" in hooks
    assert f"kPauseMenuVisibleAddress = 0x{visible:08X};" in hooks


def main() -> None:
    bindings = (TITLE / "src/gta4_keyboard_controller.h").read_text()
    for direction, key in (("up", "Up"), ("down", "Down"),
                           ("left", "Left"), ("right", "Right")):
        assert f".dpad_{direction} = VirtualKey::k{key}" in bindings
    for button, key in (("a", "Return"), ("b", "Delete"), ("b_alias", "Back"),
                         ("y", "F")):
        assert f".{button} = VirtualKey::k{key}" in bindings
    for button, key in (("a_alias", "LButton"), ("x", "Shift"),
                         ("left_trigger", "S"), ("right_trigger", "W"),
                         ("left_shoulder", "Numpad4"), ("right_shoulder", "Numpad6"),
                         ("left_stick_up", "Numpad8"), ("left_stick_down", "Numpad2"),
                         ("left_stick_left", "A"), ("left_stick_right", "D")):
        assert f"bindings.{button} = VirtualKey::k{key};" in bindings
    app = (TITLE / "src/gta4_app.cpp").read_text()
    assert re.search(
        r"SetNativeControllerCompatibilityBindings\(\s*"
        r"gta4::input::KeyboardControllerBindings\(\)\);", app
    )
    hooks = (TITLE / "src/gta4_input_hooks.cpp").read_text()
    binding_start = hooks.index("constexpr ButtonBinding kButtonBindings[]")
    binding_end = hooks.index("};", binding_start)
    assert not re.search(r"VirtualKey::k(?:Up|Down|Left|Right|Return|Back|Delete|F)\b",
                         hooks[binding_start:binding_end])
    poll = hooks[hooks.index('extern "C" void sub_828CCD60('):]
    assert poll.index("ConfigureKeyboardControllerForPoll(ctx, base)") < poll.index(
        "__imp__sub_828CCD60(ctx, base)")
    assert "CaptureEpoch(ctx, base, caller, helicopter_controls)" in poll
    heli_native = body("sub_825C4400")
    assert "REX_LOAD_U32(ctx.r11.u32 + 572)" in heli_native
    assert "& 0x20000000" in heli_native
    assert "REX_LOAD_U32(ctx.r11.u32 + 2688)" in heli_native
    assert "REX_LOAD_U32(ctx.r11.u32 + 4836)" in heli_native
    assert "ctx.cr6.compare<int32_t>(ctx.r11.s32, 4, ctx.xer)" in heli_native
    assert "REX_LOAD_U32(ctx.r31.u32 + 3904)" in body("sub_8259D440")
    verify_phone_ownership()
    verify_pause_ownership()
    frontend = body("sub_8224FFC8")
    for action in (64, 65, 66, 67, 76, 77, 78):
        for field in (0, 2, 3):
            offset = action_offset(action) + field
            assert f"REX_LOAD_U8(ctx.r3.u32 + {offset});" in frontend
    replay = body("sub_828D3058")
    assert "REX_STORE_U8(ctx.r11.u32 + 2, ctx.r28.u8);" in replay
    assert "REX_STORE_U8(ctx.r11.u32 + 3, ctx.r10.u8);" in replay
    pause_entry = body("sub_8214B168")
    assert "ctx.r3.s64 = 12;" in pause_entry
    assert "sub_8224FFC8(ctx, base);" in pause_entry

    selection = body("sub_823CF9C0")
    predicate_site(selection, 8, "sub_82163CE0", 0x823CFD54)
    predicate_site(selection, 42, "sub_82163CE0", 0x823CFB28)
    predicate_site(selection, 42, "sub_822D3230", 0x823CFAF0)
    assert "ctx.lr = 0x823CFD94;\n\tsub_823D5800(ctx, base);" in selection

    candidates = body("sub_823D5800")
    for caller in (0x823D58E0, 0x823D5998):
        assert f"ctx.lr = 0x{caller:08X};\n\tsub_823D5358(ctx, base);" in candidates
    assert "ctx.r30.s64 = ctx.r4.s64 + 1;" in body("sub_823D52D0")
    assert "ctx.r31.s64 = ctx.r31.s64 + -1;" in body("sub_823D5358")
    assert "ctx.cr6.compare<int32_t>(ctx.r30.s32, 11, ctx.xer);" in body("sub_823D52D0")

    primary = body("sub_822A6E90")
    secondary = body("sub_822A6F78")
    for source, action in ((primary, 39), (secondary, 45)):
        assert f"REX_LOAD_U8(ctx.r3.u32 + {action_offset(action)});" in source
        assert f"REX_LOAD_U8(ctx.r3.u32 + {action_offset(action) + 2});" in source

    helicopter = body("sub_822ABEE0")
    for action in (55, 56, 57, 58):
        assert re.search(rf"\+ {action_offset(action)}[;)]", helicopter)
    assert "sub_822B8178(ctx, base);" in helicopter
    pitch = body("sub_822B8178")
    for action in (32, 33):
        assert f"ctx.r31.s64 + {action_offset(action)};" in pitch

    sniper = body("sub_825ED3C8")
    for action, caller in ((24, 0x825ED65C), (26, 0x825ED638)):
        assert f"ctx.r11.s64 = ctx.r28.s64 + {action_offset(action)};" in sniper
        assert f"ctx.lr = 0x{caller:08X};\n\tsub_822B7958(ctx, base);" in sniper
    assert "ctx.f25.f64 = double(float(ctx.f13.f64 * ctx.f25.f64));" in sniper
    assert "ctx.f25.f64 = double(float(ctx.f25.f64 / ctx.f13.f64));" in sniper

    print("GTA IV compiled keyboard consumers verified: phone ownership/native registration, menu release edges, action history, weapon predicates, candidate order, helicopter fire/pitch/yaw, signed sniper zoom")


if __name__ == "__main__":
    main()
