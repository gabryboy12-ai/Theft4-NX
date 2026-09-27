#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>

namespace {

using namespace rex;
using namespace rex::system;

static_assert(sizeof(rex::be<uint32_t>) == 4,
              "Xbox FILE_SEGMENT_ELEMENT entries are four-byte big-endian pointers");
static_assert(sizeof(X_IO_STATUS_BLOCK) == 8, "Xbox IO_STATUS_BLOCK is two big-endian words");

TEST_CASE("File scatter policy covers partial pages", "[system][file][scatter]") {
  CHECK(file_io::ScatterSegmentCount(0) == 0);
  CHECK(file_io::ScatterSegmentCount(1) == 1);
  CHECK(file_io::ScatterSegmentCount(4095) == 1);
  CHECK(file_io::ScatterSegmentCount(4096) == 1);
  CHECK(file_io::ScatterSegmentCount(4097) == 2);
  CHECK(file_io::ScatterSegmentCount(8192) == 2);

  CHECK(file_io::ScatterSegmentLength(1) == 1);
  CHECK(file_io::ScatterSegmentLength(4096) == 4096);
  CHECK(file_io::ScatterSegmentLength(4097) == 4096);
}

TEST_CASE("File scatter policy keeps explicit zero stable", "[system][file][scatter]") {
  CHECK_FALSE(file_io::UsesCurrentPosition(0));
  CHECK(file_io::UsesCurrentPosition(file_io::kUseCurrentPosition));

  CHECK(file_io::ScatterExplicitOffset(0, 0) == 0);
  CHECK(file_io::ScatterExplicitOffset(0, 4096) == 0x1000);
  CHECK(file_io::ScatterExplicitOffset(0, 8192) == 0x2000);
  CHECK(file_io::ScatterExplicitOffset(0x12340000, 4096) == 0x12341000);

  CHECK(file_io::AdvancesPosition(true, 0));
  CHECK_FALSE(file_io::AdvancesPosition(false, 0));
  CHECK(file_io::AdvancesPosition(false, file_io::kUseCurrentPosition));
}

TEST_CASE("File scatter policy stops on short reads and errors", "[system][file][scatter]") {
  CHECK(file_io::ContinueScatter(X_STATUS_SUCCESS, 4096, 4096));
  CHECK_FALSE(file_io::ContinueScatter(X_STATUS_SUCCESS, 4096, 2048));
  CHECK_FALSE(file_io::ContinueScatter(X_STATUS_END_OF_FILE, 4096, 0));
  CHECK_FALSE(file_io::ContinueScatter(X_STATUS_ACCESS_DENIED, 4096, 0));

  CHECK_FALSE(file_io::ScatterOffsetOverflows(0, 4096));
  CHECK(file_io::ScatterOffsetOverflows(std::numeric_limits<uint64_t>::max(), 1));
  CHECK_FALSE(file_io::PositionAdvanceOverflows(0, 4096));
  CHECK(file_io::PositionAdvanceOverflows(std::numeric_limits<uint64_t>::max(), 1));
}

TEST_CASE("Host task admission policy rejects teardown work", "[system][file][async]") {
  CHECK(host_task_policy::CanAdmit(true, true, false));
  CHECK_FALSE(host_task_policy::CanAdmit(false, true, false));
  CHECK_FALSE(host_task_policy::CanAdmit(true, false, false));
  CHECK_FALSE(host_task_policy::CanAdmit(true, true, true));
  CHECK(host_task_policy::kAdmissionFailureResult == X_ERROR_CANCELLED);
  CHECK(HostTaskAdmissionResult::kAccepted != HostTaskAdmissionResult::kRejected);
  CHECK(HostTaskAdmissionResult::kNoMemory != HostTaskAdmissionResult::kRejected);
}

TEST_CASE("Async file reads select exactly one wait object", "[system][file][async]") {
  CHECK(file_io::SelectReadWaitObject(false) == file_io::ReadWaitObject::kFile);
  CHECK(file_io::SelectReadWaitObject(true) == file_io::ReadWaitObject::kSuppliedEvent);
}

TEST_CASE("Async file reads retain a real pending path", "[system][file][async]") {
  CHECK(file_io::IsInlineCompletionEligible(true, false, false, false));
  CHECK(file_io::IsInlineCompletionEligible(false, false, false, false));

  CHECK_FALSE(file_io::IsInlineCompletionEligible(false, true, false, false));
  CHECK_FALSE(file_io::IsInlineCompletionEligible(false, false, true, false));
  CHECK_FALSE(file_io::IsInlineCompletionEligible(false, false, false, true));
  CHECK_FALSE(file_io::IsInlineCompletionEligible(false, true, true, true));
}

}  // namespace
