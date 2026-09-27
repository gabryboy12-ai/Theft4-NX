#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/xgi_stats_abi.h>

namespace rex::kernel::xam::detail {
namespace {

TEST_CASE("GTA IV stats service status distinguishes sign-in from backend readiness",
          "[xam][stats][status]") {
  CHECK(GtaStatsServiceResult(false, false, false) == X_ERROR_NOT_LOGGED_ON);
  CHECK(GtaStatsServiceResult(true, false, false) == X_ERROR_FUNCTION_FAILED);
  CHECK(GtaStatsServiceResult(true, true, false) == X_ERROR_FUNCTION_FAILED);
  CHECK(GtaStatsServiceResult(true, true, true) == X_ERROR_SUCCESS);
}

TEST_CASE("GTA IV stats results use the generated PPC row and column ABI", "[xam][stats][abi]") {
  using rex::system::xam::StatColumn;
  using rex::system::xam::StatRow;
  using rex::system::xam::StatValueType;
  using rex::system::xam::StatView;
  using rex::system::xam::XUserDataType;

  constexpr uint32_t guest_address = 0x1000;
  const std::vector<StatView> views = {{
      .id = 3,
      .rows = {{.xuid = 0x1122334455667788,
                .rank = 7,
                .rating = -9,
                .player_name = "PPCPlayerNameLong",
                .columns =
                    {{.id = 11, .type = StatValueType::kInt32, .value = int32_t{42}},
                     {.id = 12, .type = StatValueType::kUnicode, .value = std::u16string(u"IV")}}}},
  }};

  const auto required_size = GtaStatsResultSize(views);
  REQUIRE(required_size);
  REQUIRE(*required_size == 126);
  // The generated caller allocates 132 bytes for this shape using its
  // conservative 52-byte row and 28-byte column accounting.
  std::vector<uint8_t> output(132, 0xA5);
  REQUIRE(WriteGtaStatsResults(output, guest_address, views));

  const auto* results = reinterpret_cast<const GtaStatsResults*>(output.data());
  CHECK(static_cast<uint32_t>(results->view_count) == 1);
  CHECK(static_cast<uint32_t>(results->views_ptr) == 0x1008);

  const auto* view = reinterpret_cast<const GtaStatsViewResult*>(output.data() + 8);
  CHECK(static_cast<uint32_t>(view->view_id) == 3);
  CHECK(static_cast<uint32_t>(view->row_count) == 1);
  CHECK(static_cast<uint32_t>(view->rows_ptr) == 0x1018);

  const auto* row = reinterpret_cast<const GtaStatsRow*>(output.data() + 24);
  CHECK(static_cast<uint64_t>(row->xuid) == 0x1122334455667788);
  CHECK(static_cast<uint32_t>(row->rank) == 7);
  CHECK(row->reserved == 0);
  CHECK(static_cast<int64_t>(row->rating) == -9);
  CHECK(memory::load_and_swap<int64_t>(reinterpret_cast<const uint8_t*>(row) +
                                       offsetof(GtaStatsRow, rating)) == -9);
  CHECK(std::string_view(row->gamertag.data()) == "PPCPlayerNameLo");
  CHECK(static_cast<uint32_t>(row->column_count) == 2);
  CHECK(static_cast<uint32_t>(row->columns_ptr) == 0x1048);

  const auto* columns = reinterpret_cast<const GtaStatsColumn*>(output.data() + 72);
  CHECK(static_cast<uint16_t>(columns[0].column_id) == 11);
  CHECK(columns[0].value.type == XUserDataType::kInt32);
  CHECK(static_cast<int32_t>(columns[0].value.value.s32) == 42);
  CHECK(static_cast<uint16_t>(columns[1].column_id) == 12);
  CHECK(columns[1].value.type == XUserDataType::kWString);
  CHECK(static_cast<uint32_t>(columns[1].value.value.unicode.size) == 6);
  CHECK(static_cast<uint32_t>(columns[1].value.value.unicode.pointer) == 0x1078);
  CHECK(memory::load_and_swap<uint16_t>(output.data() + 120) == u'I');
  CHECK(memory::load_and_swap<uint16_t>(output.data() + 122) == u'V');
  CHECK(memory::load_and_swap<uint16_t>(output.data() + 124) == 0);
  CHECK(std::ranges::all_of(output.begin() + 126, output.end(),
                            [](uint8_t byte) { return byte == 0; }));
}

TEST_CASE("GTA IV stats results preserve unset column slots", "[xam][stats][abi]") {
  using rex::system::xam::StatColumn;
  using rex::system::xam::StatRow;
  using rex::system::xam::StatValueType;
  using rex::system::xam::StatView;
  using rex::system::xam::XUserDataType;

  const std::vector<StatView> views = {{
      .id = 1,
      .rows = {{.xuid = 1,
                .rating = 50,
                .player_name = "Player",
                .columns =
                    {StatColumn{.id = 1,
                                .type = StatValueType::kUnset,
                                .value = int32_t{}},
                     StatColumn{.id = 5,
                                .type = StatValueType::kInt32,
                                .value = int32_t{7}}}}},
  }};
  const auto required_size = GtaStatsResultSize(views);
  REQUIRE(required_size);
  std::vector<uint8_t> output(*required_size);
  REQUIRE(WriteGtaStatsResults(output, 0x1000, views));

  const size_t columns_offset = sizeof(GtaStatsResults) + sizeof(GtaStatsViewResult) +
                                sizeof(GtaStatsRow);
  const auto* columns =
      reinterpret_cast<const GtaStatsColumn*>(output.data() + columns_offset);
  CHECK(static_cast<uint16_t>(columns[0].column_id) == 1);
  CHECK(columns[0].value.type == XUserDataType::kUnset);
  CHECK(static_cast<uint16_t>(columns[1].column_id) == 5);
  CHECK(columns[1].value.type == XUserDataType::kInt32);
  CHECK(static_cast<int32_t>(columns[1].value.value.s32) == 7);
}

TEST_CASE("GTA IV flush stats ignores every opaque retail request-tail byte",
          "[xam][stats][abi]") {
  constexpr uint32_t session_object = 0x12345678;
  std::array<uint8_t, sizeof(GtaFlushStatsRequest)> request{};
  memory::store_and_swap<uint32_t>(request.data(), session_object);

  const auto read_session_object = [&] {
    const auto parsed = GtaFlushStatsSessionObject(request);
    REQUIRE(parsed);
    CHECK(*parsed == session_object);
  };

  SECTION("zero tail") { read_session_object(); }

  SECTION("fixed pattern tail") {
    std::fill(request.begin() + offsetof(GtaFlushStatsRequest, opaque_tail), request.end(),
              uint8_t{0xA5});
    read_session_object();
  }

  SECTION("deterministic random tail") {
    std::mt19937 generator(0x545407F2);
    std::uniform_int_distribution<unsigned int> bytes(0, 0xFF);
    std::generate(request.begin() + offsetof(GtaFlushStatsRequest, opaque_tail), request.end(),
                  [&] { return static_cast<uint8_t>(bytes(generator)); });
    read_session_object();
  }
}

TEST_CASE("GTA IV stats writer rejects a short destination without modifying it",
          "[xam][stats][abi]") {
  using rex::system::xam::StatRow;
  using rex::system::xam::StatView;

  const std::vector<StatView> views = {
      StatView{.id = 1, .rows = {StatRow{.xuid = 1, .player_name = "Player", .columns = {}}}},
  };
  const auto required_size = GtaStatsResultSize(views);
  REQUIRE(required_size);
  std::vector<uint8_t> output(*required_size - 1, 0xA5);
  CHECK_FALSE(WriteGtaStatsResults(output, 0x1000, views));
  CHECK(std::ranges::all_of(output, [](uint8_t byte) { return byte == 0xA5; }));
}

}  // namespace
}  // namespace rex::kernel::xam::detail
