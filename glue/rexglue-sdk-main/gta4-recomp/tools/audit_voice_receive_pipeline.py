#!/usr/bin/env python3
"""Verify the generated and host boundaries used by incoming GTA IV voice."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATED_57 = ROOT / "generated" / "gta4_recomp.57.cpp"
GENERATED_77 = ROOT / "generated" / "gta4_recomp.77.cpp"
GENERATED_79 = ROOT / "generated" / "gta4_recomp.79.cpp"
GENERATED_80 = ROOT / "generated" / "gta4_recomp.80.cpp"
ONLINE_HOOKS = ROOT / "src" / "gta4_online_hooks.cpp"
COMMUNITY = ROOT / "src" / "network" / "community_multiplayer.cpp"


def between(source: str, begin: str, end: str) -> str:
    start = source.index(begin)
    finish = source.index(end, start)
    return source[start:finish]


def require_in_order(source: str, *needles: str) -> None:
    cursor = 0
    for needle in needles:
        cursor = source.index(needle, cursor) + len(needle)


def main() -> None:
    generated_57 = GENERATED_57.read_text()
    generated_77 = GENERATED_77.read_text()
    generated_79 = GENERATED_79.read_text()
    generated_80 = GENERATED_80.read_text()
    online_hooks = ONLINE_HOOKS.read_text()
    community = COMMUNITY.read_text()

    title_receive = between(
        generated_57,
        "DEFINE_REX_FUNC(sub_827D7E08)",
        "DEFINE_REX_FUNC(sub_827D7F00)",
    )
    require_in_order(
        title_receive,
        "sub_829DBAB0(ctx, base);",
        "sub_829E4F88(ctx, base);",
        "// lwz r11,84(r28)",
        "REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);",
    )

    identity_accessor = between(
        generated_77,
        "DEFINE_REX_FUNC(sub_829E4F88)",
        "DEFINE_REX_FUNC(sub_829E4F90)",
    )
    assert "REX_LOAD_U64(ctx.r3.u32 + 8)" in identity_accessor
    friendship_call = between(
        generated_57,
        "DEFINE_REX_FUNC(sub_827D6F20)",
        "DEFINE_REX_FUNC(sub_827D7060)",
    )
    require_in_order(
        friendship_call,
        "sub_829E4F88(ctx, base);",
        "REX_STORE_U64(ctx.r1.u32 + 88, ctx.r3.u64);",
        "sub_82A12190(ctx, base);",
    )
    friendship_import = between(
        generated_79,
        "DEFINE_REX_FUNC(sub_82A12190)",
        "DEFINE_REX_FUNC(sub_82A12198)",
    )
    assert "__imp__XamUserAreUsersFriends(ctx, base);" in friendship_import

    function_table = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A26F18)",
        "DEFINE_REX_FUNC(sub_82A27038)",
    )
    assert "REX_STORE_U32(ctx.r3.u32 + 84, ctx.r11.u32);" in function_table
    title_queue = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A26DF8)",
        "DEFINE_REX_FUNC(sub_82A26E68)",
    )
    require_in_order(
        title_queue,
        "__imp__RtlEnterCriticalSection(ctx, base);",
        "sub_82A26130(ctx, base);",
        "sub_82A2AEF0(ctx, base);",
        "__imp__RtlLeaveCriticalSection(ctx, base);",
    )
    compressed_copy = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A2AEF0)",
        "DEFINE_REX_FUNC(sub_82A2AF68)",
    )
    require_in_order(
        compressed_copy,
        "REX_LOAD_U16(ctx.r28.u32 + 0)",
        "sub_82A25448(ctx, base);",
    )

    capture_encoder = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A297F8)",
        "DEFINE_REX_FUNC(sub_82A29B08)",
    )
    require_in_order(
        capture_encoder,
        "ctx.r3.s64 = 0;",
        "sub_82A2BBD8(ctx, base);",
        "ctx.r4.s64 = 0;",
        "__imp__XamVoiceSubmitPacket(ctx, base);",
    )
    playback_decoder = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A2D320)",
        "DEFINE_REX_FUNC(sub_82A2D858)",
    )
    require_in_order(
        playback_decoder,
        "ctx.r3.s64 = 1;",
        "sub_82A2BBD8(ctx, base);",
    )
    pcm_submit = between(
        generated_80,
        "DEFINE_REX_FUNC(sub_82A2D110)",
        "DEFINE_REX_FUNC(sub_82A2D1E0)",
    )
    require_in_order(
        pcm_submit,
        "ctx.r4.s64 = 1;",
        "__imp__XamVoiceSubmitPacket(ctx, base);",
    )

    hook = between(
        online_hooks,
        'extern "C" void sub_827D7FA8',
        'extern "C" void sub_82A355E0',
    )
    require_in_order(hook, "__imp__sub_827D7FA8(ctx, base);", "PumpReceivedVoice")
    pump = between(online_hooks, "void PumpReceivedVoice", "}  // namespace")
    assert "sub_82A26DF8(voice_ctx, base);" in pump
    assert "packet->session_id != active_session_id" in pump
    assert "packet->source_xuid == live->identity().xuid" in pump

    receive = between(
        community,
        "std::optional<VoicePacket> Receive",
        "void Close() override",
    )
    assert "state_->Request" not in receive
    assert "std::lock_guard lock(voice_mutex_)" in receive
    worker = between(
        community,
        "void VoiceReceiveWorkerMain()",
        "std::shared_ptr<CommunityState> state_",
    )
    assert 'state_->Request(' in worker
    assert "*session != session_id" in worker
    assert "received_voice_packets_.size() >= kMaximumPendingVoicePackets" in worker
    assert "WaitForVoicePollDelay(token);" in worker
    assert "packets.size() > kMaximumVoiceBatchPackets" in worker

    print("voice_receive_pipeline_audit=ok")


if __name__ == "__main__":
    main()
