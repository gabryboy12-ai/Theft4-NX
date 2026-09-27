#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string_view>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

#include "gta4_init.h"

namespace {

// All offsets and limits below come directly from generated sub_82A21BF0,
// sub_82A21680, sub_82A21F70, and the block parser in sub_82A21700.
constexpr uint32_t kEmbeddedDecoderOffset = 0x14;
constexpr uint32_t kInputBeginOffset = 0x2B04;
constexpr uint32_t kInputEndOffset = 0x2B08;
constexpr uint32_t kOutputOffset = 0x2B0C;
constexpr uint32_t kTotalOutputOffset = 0x2B10;
constexpr uint32_t kDecodeCountOffset = 0x2EC4;
constexpr uint32_t kOutputProtectionOffset = 0x2FE4;
constexpr uint32_t kWindowSizeOffset = 0x4;
constexpr uint32_t kRetailWindowSize = 0x20000;
constexpr uint32_t kMaximumFrameSize = 0x8000;
constexpr uint32_t kMaximumCompressedFrameSize = 0xFFFF;
constexpr uint32_t kRetailReadPadding = 0x4;
constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;

enum class LzxMode : uint8_t {
  kGuest,
  kVerify,
  kHost,
};

struct NativeDecoderEntry {
  explicit NativeDecoderEntry(uint32_t window_size)
      : decoder(rex::lzx::PersistentDecoder::Create(window_size)),
        input_snapshot(new (std::nothrow)
                           uint8_t[kMaximumCompressedFrameSize + kRetailReadPadding]) {}

  std::mutex mutex;
  std::unique_ptr<rex::lzx::PersistentDecoder> decoder;
  std::unique_ptr<uint8_t[]> input_snapshot;
  bool host_failed = false;
};

std::mutex g_registry_mutex;
std::unordered_map<uint32_t, std::shared_ptr<NativeDecoderEntry>> g_registry;
std::atomic<bool> g_native_verification_enabled{true};
std::atomic<uint64_t> g_verified_frames{0};

LzxMode GetMode() {
  static const LzxMode mode = [] {
    const char* value = std::getenv("REX_GTA4_LZX_MODE");
    if (!value) {
      return LzxMode::kGuest;
    }
    const std::string_view requested(value);
    if (requested == "verify") {
      return LzxMode::kVerify;
    }
    if (requested == "host") {
      return LzxMode::kHost;
    }
    return LzxMode::kGuest;
  }();
  return mode;
}

bool IsGuestRange(uint32_t address, uint64_t size) {
  if (size == 0) {
    return true;
  }
  if (address == 0 || size > kGuestAddressSpaceSize) {
    return false;
  }
  return static_cast<uint64_t>(address) + size <= kGuestAddressSpaceSize;
}

std::shared_ptr<NativeDecoderEntry> FindEntry(uint32_t decoder_address) {
  std::lock_guard lock(g_registry_mutex);
  const auto found = g_registry.find(decoder_address);
  return found == g_registry.end() ? nullptr : found->second;
}

std::shared_ptr<NativeDecoderEntry> GetOrCreateEntry(uint32_t decoder_address,
                                                     uint32_t window_size) {
  std::lock_guard lock(g_registry_mutex);
  const auto found = g_registry.find(decoder_address);
  if (found != g_registry.end()) {
    return found->second;
  }

  try {
    auto entry = std::shared_ptr<NativeDecoderEntry>(
        new (std::nothrow) NativeDecoderEntry(window_size));
    if (!entry || !entry->decoder || !entry->input_snapshot) {
      return nullptr;
    }
    g_registry.emplace(decoder_address, entry);
    return entry;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void RemoveEntry(uint32_t decoder_address) {
  std::lock_guard lock(g_registry_mutex);
  g_registry.erase(decoder_address);
}

void ResetEntry(uint32_t decoder_address) {
  const auto entry = FindEntry(decoder_address);
  if (!entry) {
    return;
  }
  std::lock_guard lock(entry->mutex);
  entry->host_failed = !entry->decoder->Reset();
}

void SetRetailDecodeBookkeeping(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                                uint32_t input_address, uint32_t compressed_size,
                                uint32_t output_address) {
  REX_STORE_U32(decoder_address + kInputBeginOffset, input_address);
  REX_STORE_U32(decoder_address + kInputEndOffset,
                input_address + compressed_size + kRetailReadPadding);
  REX_STORE_U32(decoder_address + kOutputOffset, output_address);

  rex::CallFrame protection_query(ctx);
  protection_query.ctx.r3.u64 = output_address;
  __imp__MmQueryAddressProtect(protection_query.ctx, base);
  REX_STORE_U8(decoder_address + kOutputProtectionOffset,
               (protection_query.ctx.r3.u32 & 0x600) != 0 ? 1 : 0);

  const uint32_t count = REX_LOAD_U32(decoder_address + kDecodeCountOffset);
  REX_STORE_U32(decoder_address + kDecodeCountOffset, count + 1);
}

void CompleteHostDecode(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                        uint32_t bytes_written_address, bool success,
                        uint32_t bytes_written) {
  REX_STORE_U32(bytes_written_address, success ? bytes_written : 0);
  if (success) {
    const uint32_t total = REX_LOAD_U32(decoder_address + kTotalOutputOffset);
    REX_STORE_U32(decoder_address + kTotalOutputOffset, total + bytes_written);
    ctx.r3.u64 = 0;
  } else {
    ctx.r3.u64 = 1;
  }
}

struct DecodeArguments {
  uint32_t decoder = 0;
  uint32_t expected_output = 0;
  uint32_t input = 0;
  uint32_t compressed_size = 0;
  uint32_t output = 0;
  uint32_t bytes_written = 0;
};

DecodeArguments ReadArguments(const PPCContext& ctx) {
  // r8 redundantly carries the output size at the generated call site, but
  // retail sub_82A21BF0 never reads it. Deliberately ignoring r8 is required
  // for ABI fidelity; r4 is the sole decode-size argument.
  return {
      ctx.r3.u32,
      ctx.r4.u32,
      ctx.r5.u32,
      ctx.r6.u32,
      ctx.r7.u32,
      ctx.r9.u32,
  };
}

bool ValidateArguments(const DecodeArguments& args) {
  const uint64_t decoder_span =
      static_cast<uint64_t>(kOutputProtectionOffset) + sizeof(uint8_t);
  const uint64_t padded_input_size =
      static_cast<uint64_t>(args.compressed_size) + kRetailReadPadding;
  const uint64_t input_end = static_cast<uint64_t>(args.input) + padded_input_size;
  return args.expected_output <= kMaximumFrameSize &&
         args.compressed_size <= kMaximumCompressedFrameSize &&
         IsGuestRange(args.decoder, decoder_span) &&
         input_end < kGuestAddressSpaceSize &&
         IsGuestRange(args.input, padded_input_size) &&
         IsGuestRange(args.output, args.expected_output) &&
         IsGuestRange(args.bytes_written, sizeof(uint32_t));
}

void DisableVerification(uint32_t decoder, const char* reason, uint32_t guest_status,
                         uint32_t native_status, uint32_t guest_size,
                         uint32_t native_size) {
  bool expected = true;
  if (g_native_verification_enabled.compare_exchange_strong(expected, false)) {
    REXLOG_ERROR(
        "gta4-lzx: native verification disabled context={:08X} reason={} "
        "guest_status={} native_status={} guest_size={} native_size={}",
        decoder, reason, guest_status, native_status, guest_size, native_size);
  }
}

void RunVerify(PPCContext& ctx, uint8_t* base, const DecodeArguments& args) {
  if (!g_native_verification_enabled.load(std::memory_order_acquire) ||
      !ValidateArguments(args) || REX_LOAD_U32(args.decoder + kWindowSizeOffset) !=
                                      kRetailWindowSize) {
    __imp__sub_82A21BF0(ctx, base);
    return;
  }

  const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize);
  std::unique_ptr<uint8_t[]> scratch(
      args.expected_output ? new (std::nothrow) uint8_t[args.expected_output] : nullptr);
  if (!entry || (args.expected_output != 0 && !scratch)) {
    __imp__sub_82A21BF0(ctx, base);
    DisableVerification(args.decoder, "allocation", 0, 1, 0, 0);
    return;
  }

  rex::lzx::DecodeFrameResult native_result;
  {
    std::lock_guard lock(entry->mutex);
    native_result = entry->decoder->DecodeFrame(
        std::span<const uint8_t>(base + args.input,
                                 static_cast<size_t>(args.compressed_size) +
                                     kRetailReadPadding),
        std::span<uint8_t>(scratch.get(), args.expected_output));
  }

  __imp__sub_82A21BF0(ctx, base);
  const uint32_t guest_status = ctx.r3.u32;
  const uint32_t guest_size = REX_LOAD_U32(args.bytes_written);
  const uint32_t native_status = native_result ? 0 : 1;
  const uint32_t native_size =
      native_result ? static_cast<uint32_t>(native_result.bytes_written) : 0;

  const bool status_matches = guest_status == native_status;
  const bool size_matches = guest_size == native_size;
  const bool bytes_match = guest_status != 0 || !status_matches || !size_matches ||
                           guest_size == 0 ||
                           std::memcmp(base + args.output, scratch.get(), guest_size) == 0;
  if (!status_matches || !size_matches || !bytes_match || !native_result) {
    DisableVerification(args.decoder,
                        !native_result       ? "native-decode"
                        : !status_matches    ? "status"
                        : !size_matches      ? "size"
                                             : "payload",
                        guest_status, native_status, guest_size, native_size);
    return;
  }

  const uint64_t verified = g_verified_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (verified == 1) {
    REXLOG_INFO("gta4-lzx: native verifier active (retail 128 KiB window)");
  }
}

void RunHost(PPCContext& ctx, uint8_t* base, const DecodeArguments& args) {
  if (!ValidateArguments(args)) {
    const uint64_t decoder_span =
        static_cast<uint64_t>(kOutputProtectionOffset) + sizeof(uint8_t);
    if (IsGuestRange(args.decoder, decoder_span) &&
        REX_LOAD_U32(args.decoder + kWindowSizeOffset) == kRetailWindowSize) {
      if (const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize)) {
        std::lock_guard lock(entry->mutex);
        entry->host_failed = true;
      }
    }
    if (IsGuestRange(args.bytes_written, sizeof(uint32_t))) {
      REX_STORE_U32(args.bytes_written, 0);
    }
    ctx.r3.u64 = 1;
    return;
  }

  SetRetailDecodeBookkeeping(ctx, base, args.decoder, args.input, args.compressed_size,
                             args.output);
  if (REX_LOAD_U32(args.decoder + kWindowSizeOffset) != kRetailWindowSize) {
    if (const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize)) {
      std::lock_guard lock(entry->mutex);
      entry->host_failed = true;
    }
    CompleteHostDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    return;
  }

  const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize);
  if (!entry) {
    CompleteHostDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    return;
  }

  rex::lzx::DecodeFrameResult result;
  {
    std::lock_guard lock(entry->mutex);
    if (entry->host_failed) {
      CompleteHostDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
      return;
    }
    const size_t readable_input_size =
        static_cast<size_t>(args.compressed_size) + kRetailReadPadding;
    std::memcpy(entry->input_snapshot.get(), base + args.input, readable_input_size);
    result = entry->decoder->DecodeFrame(
        std::span<const uint8_t>(entry->input_snapshot.get(), readable_input_size),
        std::span<uint8_t>(base + args.output, args.expected_output));
    if (!result) {
      entry->host_failed = true;
    }
  }

  CompleteHostDecode(ctx, base, args.decoder, args.bytes_written, static_cast<bool>(result),
                     static_cast<uint32_t>(result.bytes_written));
}

}  // namespace

REX_HOOK_RAW(sub_82A21BF0) {
  const DecodeArguments args = ReadArguments(ctx);
  switch (GetMode()) {
    case LzxMode::kVerify:
      RunVerify(ctx, base, args);
      return;
    case LzxMode::kHost:
      RunHost(ctx, base, args);
      return;
    case LzxMode::kGuest:
    default:
      __imp__sub_82A21BF0(ctx, base);
      return;
  }
}

REX_HOOK_RAW(sub_82A21680) {
  const uint32_t outer_object = ctx.r3.u32;
  __imp__sub_82A21680(ctx, base);
  if (IsGuestRange(outer_object, kEmbeddedDecoderOffset + sizeof(uint32_t))) {
    ResetEntry(outer_object + kEmbeddedDecoderOffset);
  }
}

REX_HOOK_RAW(sub_82A15060) {
  const uint32_t outer_object = ctx.r3.u32;
  if (IsGuestRange(outer_object, kEmbeddedDecoderOffset + sizeof(uint32_t))) {
    RemoveEntry(outer_object + kEmbeddedDecoderOffset);
  }
  __imp__sub_82A15060(ctx, base);
}
