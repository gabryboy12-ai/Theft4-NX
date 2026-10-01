/**
 * @file        system/null_graphics_system.cpp
 * @brief       A guest GPU that keeps the game in step without drawing
 *
 * The packet handling follows rex::graphics::CommandProcessor (Xenia), cut
 * down to the effects the CPU side can observe. It depends only on the
 * header-only parts of rex/graphics (register indices, PM4 opcodes, GpuSwap),
 * so a runtime without rexgraphics (the headless Switch build) can use it.
 */

#include <rex/system/null_graphics_system.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

#include <rex/chrono/clock.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

namespace rex::system {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::memory::RingBuffer;

// RegisterFile::kRegisterCount: the register indices the packets can name.
constexpr uint32_t kRegisterCount = 0x5003;
constexpr uint32_t kRegCpRbRptr = 0x01C4;
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr int kMaxIndirectDepth = 8;
// query_occlusion_fake_sample_count's default in command_processor.cpp.
constexpr uint32_t kFakeOcclusionSamples = 1000;

struct Counters {
  std::atomic<uint64_t> wptr_writes{0};
  std::atomic<uint64_t> packets{0};
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> swaps{0};
  std::atomic<uint64_t> vblanks{0};
  std::atomic<uint64_t> interrupts{0};
  std::atomic<uint64_t> unknown_opcodes{0};
  std::atomic<uint32_t> read_index{0};
  std::atomic<uint32_t> write_index{0};
  std::atomic<uint32_t> waiting_opcode{0};
};
Counters g_counters;
class NullGraphicsSystem;
std::atomic<NullGraphicsSystem*> g_last_null_gpu{nullptr};

class NullGraphicsSystem final : public IGraphicsSystem {
 public:
  NullGraphicsSystem() : wptr_event_(rex::thread::Event::CreateAutoResetEvent(false)) {
    std::memset(regs_, 0, sizeof(regs_));
    g_last_null_gpu.store(this);
  }
  ~NullGraphicsSystem() override {
    NullGraphicsSystem* self = this;
    g_last_null_gpu.compare_exchange_strong(self, nullptr);
    Shutdown();
  }

  void RequestStop() {
    running_.store(false);
    wptr_event_->Set();
  }

  X_STATUS SetupPresentation(ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }
  bool has_presentation() const override { return false; }

  X_STATUS SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                         KernelState* kernel_state) override {
    function_dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    memory_ = function_dispatcher->memory();
    // The GPU registers, as GraphicsSystem::SetupGuestGpu maps them.
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, ReadRegisterThunk,
                                   WriteRegisterThunk);
    running_ = true;
    cp_thread_ = object_ref<XHostThread>(new XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
      CommandThreadMain();
      return 0;
    }));
    cp_thread_->set_name("GPU Commands (null)");
    cp_thread_->Create();
    vsync_thread_ =
        object_ref<XHostThread>(new XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          VsyncThreadMain();
          return 0;
        }));
    vsync_thread_->set_name("GPU VSync (null)");
    vsync_thread_->Create();
    REXGPU_INFO("null GPU: no rendering; ring consumed, fences and interrupts emulated");
    return X_STATUS_SUCCESS;
  }

  void Shutdown() override {
    running_.store(false);
    if (shut_down_.exchange(true)) {
      return;
    }
    wptr_event_->Set();
    if (cp_thread_) {
      cp_thread_->Wait(0, 0, 0, nullptr);
      cp_thread_.reset();
    }
    if (vsync_thread_) {
      vsync_thread_->Wait(0, 0, 0, nullptr);
      vsync_thread_.reset();
    }
  }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_data_.store(user_data);
    interrupt_callback_.store(callback);
    REXGPU_INFO("null GPU: SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_size_.store(uint32_t(1) << (size_log2 + 3));
    ring_ptr_.store(ptr);
    REXGPU_INFO("null GPU: ring buffer {:08X}, {} bytes", ptr, uint32_t(1) << (size_log2 + 3));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    writeback_ptr_.store(ptr);
    REXGPU_INFO("null GPU: read pointer writeback at {:08X} (block log2 {})", ptr,
                block_size_log2);
  }

 private:
  static uint32_t ReadRegisterThunk(void*, void* context, uint32_t addr) {
    return static_cast<NullGraphicsSystem*>(context)->ReadRegister(addr);
  }
  static void WriteRegisterThunk(void*, void* context, uint32_t addr, uint32_t value) {
    static_cast<NullGraphicsSystem*>(context)->WriteMmio(addr, value);
  }

  // GraphicsSystem::ReadRegister's fixed answers, plus the ring read pointer.
  uint32_t ReadRegister(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00:  // RB_EDRAM_TIMING
        return 0x08100748;
      case 0x0F01:  // RB_BC_CONTROL
        return 0x0000200E;
      case 0x194C: {  // D1MODE_V_COUNTER
        X_VIDEO_MODE mode;
        kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case 0x1951:  // interrupt status: vblank
        return 1;
      case 0x1961: {  // D1MODE_VIEWPORT_SIZE
        X_VIDEO_MODE mode;
        kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegCpRbRptr:
        return g_counters.read_index.load();
      default:
        return r < kRegisterCount ? Reg(r) : 0;
    }
  }

  void WriteMmio(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      g_counters.wptr_writes.fetch_add(1, std::memory_order_relaxed);
      write_index_.store(value);
      g_counters.write_index.store(value, std::memory_order_relaxed);
      wptr_event_->Set();
    }
    if (r < kRegisterCount) {
      SetReg(r, value);
    }
  }

  uint32_t Reg(uint32_t index) const {
    return const_cast<const volatile uint32_t&>(regs_[index]);
  }
  void SetReg(uint32_t index, uint32_t value) {
    const_cast<volatile uint32_t&>(regs_[index]) = value;
  }

  // CommandProcessor::WriteRegister: the effects the CPU can see.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    SetReg(index, value);
    if (index >= graphics::XE_GPU_REG_SCRATCH_REG0 && index <= graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t scratch = index - graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((1u << scratch) & Reg(graphics::XE_GPU_REG_SCRATCH_UMSK)) {
        const uint32_t mem_addr = Reg(graphics::XE_GPU_REG_SCRATCH_ADDR) + scratch * 4;
        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      }
    } else if (index == graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      SetReg(index, Reg(index) | 0x80000000u);
    }
  }

  void WriteRegisterRange(RingBuffer* reader, uint32_t base, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      WriteRegister(base + i, reader->ReadAndSwap<uint32_t>());
    }
  }

  void WriteRegisterRangeFromMem(uint32_t base, const uint32_t* source, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      WriteRegister(base + i, memory::load_and_swap<uint32_t>(source + i));
    }
  }

  static uint32_t ConstantBase(uint32_t type) {
    switch (type) {
      case 0:  // ALU
        return 0x4000;
      case 1:  // FETCH
        return 0x4800;
      case 2:  // BOOL
        return 0x4900;
      case 3:  // LOOP
        return 0x4908;
      case 4:  // REGISTERS
        return 0x2000;
      default:
        return UINT32_MAX;
    }
  }

  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = interrupt_callback_.load();
    if (!callback) {
      return;
    }
    auto* thread = XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu == 0xFFFFFFFF ? 2 : cpu));
    uint64_t args[] = {source, interrupt_data_.load()};
    function_dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
  }

  void VsyncThreadMain() {
    X_VIDEO_MODE mode;
    kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    // The guest tick frequency must be known, or the interval would be one
    // tick and every pass would raise thousands of interrupts.
    while (running_ && rex::chrono::Clock::guest_tick_frequency() == 0) {
      rex::thread::Sleep(std::chrono::milliseconds(10));
    }
    const uint64_t interval = std::max<uint64_t>(
        1, uint64_t(double(rex::chrono::Clock::guest_tick_frequency()) / hz));
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    while (running_) {
      const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      if (now - last > 4 * interval) {
        last = now - interval;  // fell behind (thread not scheduled): no burst
      }
      while (now - last >= interval) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        g_counters.vblanks.fetch_add(1, std::memory_order_relaxed);
        DispatchInterrupt(0, 2);
        last += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
  }

  void CommandThreadMain() {
    uint32_t read_index = 0;
    while (running_) {
      const uint32_t ring_ptr = ring_ptr_.load();
      const uint32_t write_index = write_index_.load();
      if (!ring_ptr || write_index == read_index) {
        rex::thread::Wait(wptr_event_.get(), false, std::chrono::milliseconds(5));
        continue;
      }
      const uint32_t ring_size = ring_size_.load();
      RingBuffer reader(memory_->TranslatePhysical(ring_ptr), ring_size);
      reader.set_read_offset(read_index * sizeof(uint32_t));
      reader.set_write_offset(write_index * sizeof(uint32_t));
      while (running_ && reader.read_count()) {
        if (!ExecutePacket(&reader, 0)) {
          REXGPU_ERROR("null GPU: bad packet in the ring at index {}, skipping to {}",
                       reader.read_offset() / 4, write_index);
          break;
        }
      }
      read_index = write_index;
      g_counters.read_index.store(read_index, std::memory_order_relaxed);
      if (const uint32_t writeback = writeback_ptr_.load()) {
        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read_index);
      }
    }
  }

  void ExecuteIndirect(uint32_t ptr, uint32_t count, int depth) {
    if (!count) {
      return;
    }
    if (depth >= kMaxIndirectDepth) {
      REXGPU_ERROR("null GPU: indirect buffers nested deeper than {}", kMaxIndirectDepth);
      return;
    }
    RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
    reader.set_write_offset(count * sizeof(uint32_t));
    do {
      if (!ExecutePacket(&reader, depth)) {
        REXGPU_ERROR("null GPU: bad packet in the indirect buffer at {:08X}", ptr);
        break;
      }
    } while (reader.read_count());
  }

  bool ExecutePacket(RingBuffer* reader, int depth) {
    const uint32_t packet = reader->ReadAndSwap<uint32_t>();
    g_counters.packets.fetch_add(1, std::memory_order_relaxed);
    if (packet == 0) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
        if (reader->read_count() < count * 4) {
          return false;
        }
        const uint32_t base = packet & 0x7FFF;
        const bool one_reg = (packet >> 15) & 1;
        for (uint32_t m = 0; m < count; ++m) {
          WriteRegister(one_reg ? base : base + m, reader->ReadAndSwap<uint32_t>());
        }
        return true;
      }
      case 1: {
        const uint32_t r1 = packet & 0x7FF;
        const uint32_t r2 = (packet >> 11) & 0x7FF;
        const uint32_t v1 = reader->ReadAndSwap<uint32_t>();
        const uint32_t v2 = reader->ReadAndSwap<uint32_t>();
        WriteRegister(r1, v1);
        WriteRegister(r2, v2);
        return true;
      }
      case 2:
        return true;
      default:
        return ExecuteType3(reader, packet, depth);
    }
  }

  static bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
    switch (function & 0x7) {
      case 0x1:
        return value < ref;
      case 0x2:
        return value <= ref;
      case 0x3:
        return value == ref;
      case 0x4:
        return value != ref;
      case 0x5:
        return value >= ref;
      case 0x6:
        return value > ref;
      case 0x7:
        return true;
      default:
        return false;
    }
  }

  uint32_t LoadPhysical(uint32_t address_and_endian) {
    const uint32_t value = memory::load<uint32_t>(
        memory_->TranslatePhysical(address_and_endian & ~uint32_t(0x3)));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address_and_endian & 0x3));
  }

  void StorePhysical(uint32_t address_and_endian, uint32_t value) {
    memory::store(memory_->TranslatePhysical(address_and_endian & ~uint32_t(0x3)),
                  xenos::GpuSwap(value, static_cast<xenos::Endian>(address_and_endian & 0x3)));
  }

  // Skips what is left of a packet after its first `used` words.
  static void Skip(RingBuffer* reader, uint32_t count, uint32_t used) {
    if (count > used) {
      reader->AdvanceRead((count - used) * 4);
    }
  }

  bool ExecuteType3(RingBuffer* reader, uint32_t packet, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (reader->read_count() < count * 4) {
      return false;
    }
    // Predicated packets run only if a selected bin is in the mask; a
    // predicated swap never runs (CommandProcessor::ExecutePacketType3).
    if ((packet & 1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      reader->AdvanceRead(count * 4);
      return true;
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        const uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 1);
        for (uint32_t n = 0; n < 6; ++n) {
          if (cpu_mask & (1u << n)) {
            g_counters.interrupts.fetch_add(1, std::memory_order_relaxed);
            DispatchInterrupt(1, n);
          }
        }
        return true;
      }
      case xenos::PM4_XE_SWAP:
        counter_.fetch_add(1, std::memory_order_relaxed);
        g_counters.swaps.fetch_add(1, std::memory_order_relaxed);
        reader->AdvanceRead(count * 4);
        return true;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        const uint32_t list_ptr = xenos::CpuToGpu(reader->ReadAndSwap<uint32_t>());
        const uint32_t list_length = reader->ReadAndSwap<uint32_t>() & 0xFFFFF;
        Skip(reader, count, 2);
        ExecuteIndirect(xenos::GpuToCpu(list_ptr), list_length, depth + 1);
        return true;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        const uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
        const uint32_t poll = reader->ReadAndSwap<uint32_t>();
        const uint32_t ref = reader->ReadAndSwap<uint32_t>();
        const uint32_t mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t wait = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 5);
        const bool is_memory = (wait_info & 0x10) != 0;
        const auto start = std::chrono::steady_clock::now();
        bool reported = false;
        while (running_) {
          uint32_t value;
          if (is_memory) {
            value = LoadPhysical(poll);
          } else {
            if (poll == graphics::XE_GPU_REG_COHER_STATUS_HOST) {
              SetReg(poll, 0);  // CommandProcessor::MakeCoherent
            }
            value = poll < kRegisterCount ? Reg(poll) : 0;
          }
          if (Compare(wait_info, value & mask, ref)) {
            break;
          }
          if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
            reported = true;
            g_counters.waiting_opcode.store(xenos::PM4_WAIT_REG_MEM);
            REXGPU_WARN("null GPU: WAIT_REG_MEM on {} {:08X} (function {}, ref {:08X}, mask "
                        "{:08X}) for 1 s, value {:08X}",
                        is_memory ? "memory" : "register", poll, wait_info & 7, ref, mask, value);
          }
          if (wait >= 0x100) {
            rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
          } else {
            rex::thread::MaybeYield();
          }
        }
        if (reported) {
          g_counters.waiting_opcode.store(0);
          REXGPU_WARN("null GPU: WAIT_REG_MEM on {:08X} done", poll);
        }
        return true;
      }
      case xenos::PM4_REG_RMW: {
        const uint32_t info = reader->ReadAndSwap<uint32_t>();
        const uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 3);
        uint32_t value = Reg(info & 0x1FFF);
        value &= (info >> 31) & 1 ? Reg(and_mask & 0x1FFF) : and_mask;
        value |= (info >> 30) & 1 ? Reg(or_mask & 0x1FFF) : or_mask;
        WriteRegister(info & 0x1FFF, value);
        return true;
      }
      case xenos::PM4_REG_TO_MEM: {
        const uint32_t reg = reader->ReadAndSwap<uint32_t>();
        const uint32_t mem = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 2);
        StorePhysical(mem, reg < kRegisterCount ? Reg(reg) : 0);
        return true;
      }
      case xenos::PM4_MEM_WRITE: {
        uint32_t address = reader->ReadAndSwap<uint32_t>();
        for (uint32_t i = 0; i < count - 1; ++i, address += 4) {
          StorePhysical(address, reader->ReadAndSwap<uint32_t>());
        }
        return true;
      }
      case xenos::PM4_COND_WRITE: {
        const uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
        const uint32_t poll = reader->ReadAndSwap<uint32_t>();
        const uint32_t ref = reader->ReadAndSwap<uint32_t>();
        const uint32_t mask = reader->ReadAndSwap<uint32_t>();
        const uint32_t target = reader->ReadAndSwap<uint32_t>();
        const uint32_t data = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 6);
        const uint32_t value =
            (wait_info & 0x10) ? LoadPhysical(poll) : (poll < kRegisterCount ? Reg(poll) : 0);
        if (Compare(wait_info, value & mask, ref)) {
          if (wait_info & 0x100) {
            StorePhysical(target, data);
          } else {
            WriteRegister(target, data);
          }
        }
        return true;
      }
      case xenos::PM4_EVENT_WRITE: {
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 1);
        WriteRegister(graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        return true;
      }
      case xenos::PM4_EVENT_WRITE_SHD: {
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        const uint32_t value = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 3);
        WriteRegister(graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        StorePhysical(address, (initiator >> 31) & 1 ? counter_.load() : value);
        return true;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        const uint32_t address = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 2);
        WriteRegister(graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extents of the last draw: the whole screen.
        const uint16_t extents[] = {0, xenos::kTexture2DCubeMaxWidthHeight >> 3, 0,
                                    xenos::kTexture2DCubeMaxWidthHeight >> 3, 0, 1};
        memory::copy_and_swap_16_unaligned(
            memory_->TranslatePhysical(address & ~uint32_t(0x3)), extents, 6);
        return true;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        const uint32_t initiator = reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 1);
        WriteRegister(graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Occlusion queries: a fixed number of samples passed, as the
        // command processor does by default.
        const uint32_t counts_addr = Reg(graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (counts_addr) {
          auto* counts =
              memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(counts_addr);
          const uint32_t finished = rex::byte_swap(0xFFFFFEEDu);
          const bool end = (counts->ZPass_A == finished && counts->ZPass_B == finished) ||
                           (counts->ZFail_A == finished && counts->ZFail_B == finished);
          std::memset(counts, 0, sizeof(*counts));
          if (end) {
            counts->ZPass_A = kFakeOcclusionSamples;
            counts->Total_A = kFakeOcclusionSamples;
          }
        }
        return true;
      }
      case xenos::PM4_SET_CONSTANT: {
        const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
        const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
        if (base == UINT32_MAX) {
          Skip(reader, count, 1);
        } else {
          WriteRegisterRange(reader, base + (offset_type & 0x7FF), count - 1);
        }
        return true;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
        WriteRegisterRange(reader, offset_type & 0xFFFF, count - 1);
        return true;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        const uint32_t address = reader->ReadAndSwap<uint32_t>() & 0x3FFFFFFF;
        const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
        const uint32_t size_dwords = reader->ReadAndSwap<uint32_t>() & 0xFFF;
        Skip(reader, count, 3);
        const uint32_t base = ConstantBase((offset_type >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          WriteRegisterRangeFromMem(base + (offset_type & 0x7FF),
                                    memory_->TranslatePhysical<const uint32_t*>(address),
                                    size_dwords);
        }
        return true;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 1);
        return true;
      case xenos::PM4_SET_BIN_MASK_HI:
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) |
                    (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
        Skip(reader, count, 1);
        return true;
      case xenos::PM4_SET_BIN_SELECT_LO:
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | reader->ReadAndSwap<uint32_t>();
        Skip(reader, count, 1);
        return true;
      case xenos::PM4_SET_BIN_SELECT_HI:
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) |
                      (uint64_t(reader->ReadAndSwap<uint32_t>()) << 32);
        Skip(reader, count, 1);
        return true;
      case xenos::PM4_SET_BIN_MASK:
      case xenos::PM4_SET_BIN_SELECT: {
        const uint64_t hi = count >= 1 ? reader->ReadAndSwap<uint32_t>() : 0;
        const uint64_t lo = count >= 2 ? reader->ReadAndSwap<uint32_t>() : 0;
        Skip(reader, count, 2);
        (opcode == xenos::PM4_SET_BIN_MASK ? bin_mask_ : bin_select_) = (hi << 32) | lo;
        return true;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2:
        g_counters.draws.fetch_add(1, std::memory_order_relaxed);
        reader->AdvanceRead(count * 4);
        return true;
      case xenos::PM4_ME_INIT:
      case xenos::PM4_NOP:
      case xenos::PM4_IM_LOAD:
      case xenos::PM4_IM_LOAD_IMMEDIATE:
      case xenos::PM4_INVALIDATE_STATE:
      case xenos::PM4_VIZ_QUERY:
      case xenos::PM4_CONTEXT_UPDATE:
      case xenos::PM4_WAIT_FOR_IDLE:
        reader->AdvanceRead(count * 4);
        return true;
      default: {
        if (g_counters.unknown_opcodes.fetch_add(1, std::memory_order_relaxed) < 16) {
          REXGPU_WARN("null GPU: type-3 opcode {:02X} ({} words) skipped", opcode, count);
        }
        reader->AdvanceRead(count * 4);
        return true;
      }
    }
  }

  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  KernelState* kernel_state_ = nullptr;
  memory::Memory* memory_ = nullptr;

  std::atomic<bool> running_{false};
  std::atomic<bool> shut_down_{false};
  std::unique_ptr<rex::thread::Event> wptr_event_;
  object_ref<XHostThread> cp_thread_;
  object_ref<XHostThread> vsync_thread_;

  std::atomic<uint32_t> interrupt_callback_{0};
  std::atomic<uint32_t> interrupt_data_{0};
  std::atomic<uint32_t> ring_ptr_{0};
  std::atomic<uint32_t> ring_size_{0};
  std::atomic<uint32_t> writeback_ptr_{0};
  std::atomic<uint32_t> write_index_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, for EVENT_WRITE_SHD

  // Command-thread only.
  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  uint32_t regs_[kRegisterCount];
};

}  // namespace

std::unique_ptr<IGraphicsSystem> CreateNullGraphicsSystem() {
  return std::make_unique<NullGraphicsSystem>();
}

void RequestNullGpuStop() {
  if (NullGraphicsSystem* gpu = g_last_null_gpu.load()) {
    gpu->RequestStop();
  }
}

NullGpuStats GetNullGpuStats() {
  NullGpuStats s;
  s.wptr_writes = g_counters.wptr_writes.load(std::memory_order_relaxed);
  s.packets = g_counters.packets.load(std::memory_order_relaxed);
  s.draws = g_counters.draws.load(std::memory_order_relaxed);
  s.swaps = g_counters.swaps.load(std::memory_order_relaxed);
  s.vblanks = g_counters.vblanks.load(std::memory_order_relaxed);
  s.interrupts = g_counters.interrupts.load(std::memory_order_relaxed);
  s.unknown_opcodes = g_counters.unknown_opcodes.load(std::memory_order_relaxed);
  s.read_index = g_counters.read_index.load(std::memory_order_relaxed);
  s.write_index = g_counters.write_index.load(std::memory_order_relaxed);
  s.waiting_opcode = g_counters.waiting_opcode.load(std::memory_order_relaxed);
  return s;
}

}  // namespace rex::system
