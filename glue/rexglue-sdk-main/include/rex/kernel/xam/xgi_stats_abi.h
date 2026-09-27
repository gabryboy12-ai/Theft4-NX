/**
 ******************************************************************************
 * @file        xgi_stats_abi.h
 * @brief       GTA IV title-facing XGI statistics result ABI.
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <rex/memory.h>
#include <rex/system/xam/live_compatibility.h>
#include <rex/system/xam/xsession.h>
#include <rex/types.h>

namespace rex::kernel::xam::detail {

inline X_RESULT GtaStatsServiceResult(bool signed_in, bool service_present,
                                      bool service_ready) noexcept {
  if (!signed_in) return X_ERROR_NOT_LOGGED_ON;
  if (!service_present || !service_ready) return X_ERROR_FUNCTION_FAILED;
  return X_ERROR_SUCCESS;
}

struct GtaStatsResults {
  rex::be<uint32_t> view_count;
  rex::be<uint32_t> views_ptr;
};
static_assert_size(GtaStatsResults, 8);

struct GtaStatsViewResult {
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> total_rows;
  rex::be<uint32_t> row_count;
  rex::be<uint32_t> rows_ptr;
};
static_assert_size(GtaStatsViewResult, 16);

// The generated retail PPC consumes rows with a 48-byte stride. It reads rank
// at +8, rating at +16, the byte gamertag at +24, the column count at +40, and
// the columns pointer at +44.
#pragma pack(push, 4)
struct GtaStatsRow {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> rank;
  uint32_t reserved = 0;
  rex::be<int64_t> rating;
  std::array<char, 16> gamertag{};
  rex::be<uint32_t> column_count;
  rex::be<uint32_t> columns_ptr;
};
#pragma pack(pop)
static_assert_size(GtaStatsRow, 48);
static_assert(offsetof(GtaStatsRow, xuid) == 0);
static_assert(offsetof(GtaStatsRow, rank) == 8);
static_assert(offsetof(GtaStatsRow, reserved) == 12);
static_assert(offsetof(GtaStatsRow, rating) == 16);
static_assert(offsetof(GtaStatsRow, gamertag) == 24);
static_assert(offsetof(GtaStatsRow, column_count) == 40);
static_assert(offsetof(GtaStatsRow, columns_ptr) == 44);

// GTA IV's retail XSessionFlushStats wrapper initializes only the native
// session object in this 24-byte XGI request. The remaining bytes are opaque
// caller stack contents and must never be interpreted as XUID/view fields.
struct GtaFlushStatsRequest {
  rex::be<uint32_t> session_object;
  std::array<uint8_t, 20> opaque_tail{};
};
static_assert_size(GtaFlushStatsRequest, 24);
static_assert(offsetof(GtaFlushStatsRequest, session_object) == 0);
static_assert(offsetof(GtaFlushStatsRequest, opaque_tail) == 4);

inline std::optional<uint32_t> GtaFlushStatsSessionObject(
    std::span<const uint8_t> request) {
  if (request.size() < offsetof(GtaFlushStatsRequest, opaque_tail)) return std::nullopt;
  return rex::memory::load_and_swap<uint32_t>(request.data());
}

struct alignas(8) GtaStatsColumn {
  rex::be<uint16_t> column_id;
  std::array<uint8_t, 6> reserved{};
  rex::system::xam::XUSER_DATA value;
};
static_assert_size(GtaStatsColumn, 24);
static_assert(offsetof(GtaStatsColumn, column_id) == 0);
static_assert(offsetof(GtaStatsColumn, value) == 8);

constexpr size_t kMaximumGtaStatsResultBytes = 2097152;

inline bool CheckedAddStatsSize(size_t& value, size_t increment) {
  if (increment > kMaximumGtaStatsResultBytes || value > kMaximumGtaStatsResultBytes - increment) {
    return false;
  }
  value += increment;
  return true;
}

inline std::optional<size_t> CheckedStatsProduct(size_t count, size_t stride) {
  if (count && stride > kMaximumGtaStatsResultBytes / count) {
    return std::nullopt;
  }
  return count * stride;
}

inline std::optional<size_t> GtaStatsResultSize(std::span<const rex::system::xam::StatView> views) {
  using rex::system::xam::StatColumnIdKind;
  using rex::system::xam::StatValueType;

  size_t size = sizeof(GtaStatsResults);
  const auto view_bytes = CheckedStatsProduct(views.size(), sizeof(GtaStatsViewResult));
  if (!view_bytes || !CheckedAddStatsSize(size, *view_bytes)) {
    return std::nullopt;
  }
  for (const auto& view : views) {
    if ((view.total_rows && *view.total_rows < view.rows.size()) ||
        view.rows.size() > std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;
    }
    const auto row_bytes = CheckedStatsProduct(view.rows.size(), sizeof(GtaStatsRow));
    if (!row_bytes || !CheckedAddStatsSize(size, *row_bytes)) {
      return std::nullopt;
    }
    for (const auto& row : view.rows) {
      const auto column_bytes = CheckedStatsProduct(row.columns.size(), sizeof(GtaStatsColumn));
      if (!column_bytes || !CheckedAddStatsSize(size, *column_bytes)) {
        return std::nullopt;
      }
      for (const auto& column : row.columns) {
        if (column.id_kind != StatColumnIdKind::kAttribute || !column.id ||
            column.id > std::numeric_limits<uint16_t>::max()) {
          return std::nullopt;
        }
        switch (column.type) {
          case StatValueType::kUnset:
            if (!std::holds_alternative<int32_t>(column.value) ||
                std::get<int32_t>(column.value) != 0) {
              return std::nullopt;
            }
            break;
          case StatValueType::kInt32:
            if (!std::holds_alternative<int32_t>(column.value))
              return std::nullopt;
            break;
          case StatValueType::kInt64:
            if (!std::holds_alternative<int64_t>(column.value))
              return std::nullopt;
            break;
          case StatValueType::kDouble:
            if (!std::holds_alternative<double>(column.value) ||
                !std::isfinite(std::get<double>(column.value))) {
              return std::nullopt;
            }
            break;
          case StatValueType::kUnicode: {
            if (!std::holds_alternative<std::u16string>(column.value))
              return std::nullopt;
            const auto& text = std::get<std::u16string>(column.value);
            const auto units = CheckedStatsProduct(text.size() + 1, sizeof(char16_t));
            if (!units || !CheckedAddStatsSize(size, *units))
              return std::nullopt;
            break;
          }
          case StatValueType::kBinary: {
            if (!std::holds_alternative<std::vector<uint8_t>>(column.value) ||
                !CheckedAddStatsSize(size, std::get<std::vector<uint8_t>>(column.value).size())) {
              return std::nullopt;
            }
            break;
          }
        }
      }
    }
  }
  return size;
}

inline bool WriteGtaStatsResults(std::span<uint8_t> output, uint32_t guest_address,
                                 std::span<const rex::system::xam::StatView> views) {
  using rex::system::xam::StatValueType;
  using rex::system::xam::XUserDataType;

  const auto required_size = GtaStatsResultSize(views);
  if (!required_size || !guest_address || output.size() < *required_size ||
      *required_size > std::numeric_limits<uint32_t>::max() ||
      guest_address >
          std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(*required_size - 1)) {
    return false;
  }

  std::memset(output.data(), 0, output.size());
  auto* header = reinterpret_cast<GtaStatsResults*>(output.data());
  auto* guest_views = reinterpret_cast<GtaStatsViewResult*>(output.data() + sizeof(*header));
  size_t row_offset = sizeof(*header) + views.size() * sizeof(*guest_views);
  size_t column_offset = row_offset;
  for (const auto& view : views) {
    column_offset += view.rows.size() * sizeof(GtaStatsRow);
  }
  size_t payload_offset = column_offset;
  for (const auto& view : views) {
    for (const auto& row : view.rows) {
      payload_offset += row.columns.size() * sizeof(GtaStatsColumn);
    }
  }

  header->view_count = static_cast<uint32_t>(views.size());
  header->views_ptr = guest_address + sizeof(*header);
  for (size_t view_index = 0; view_index < views.size(); ++view_index) {
    const auto& source_view = views[view_index];
    auto& guest_view = guest_views[view_index];
    guest_view.view_id = source_view.id;
    guest_view.total_rows =
        source_view.total_rows.value_or(static_cast<uint32_t>(source_view.rows.size()));
    guest_view.row_count = static_cast<uint32_t>(source_view.rows.size());
    guest_view.rows_ptr =
        source_view.rows.empty() ? 0 : guest_address + static_cast<uint32_t>(row_offset);
    for (const auto& source_row : source_view.rows) {
      auto* guest_row = reinterpret_cast<GtaStatsRow*>(output.data() + row_offset);
      guest_row->xuid = source_row.xuid;
      guest_row->rank = source_row.rank;
      guest_row->rating = source_row.rating;
      const std::string name = source_row.player_name.substr(0, guest_row->gamertag.size() - 1);
      std::memcpy(guest_row->gamertag.data(), name.data(), name.size());
      guest_row->column_count = static_cast<uint32_t>(source_row.columns.size());
      guest_row->columns_ptr =
          source_row.columns.empty() ? 0 : guest_address + static_cast<uint32_t>(column_offset);
      for (const auto& source_column : source_row.columns) {
        auto* guest_column = reinterpret_cast<GtaStatsColumn*>(output.data() + column_offset);
        // GtaStatsResultSize has already proven this is a retail 16-bit
        // attribute ID. Never coerce a 32-bit XUSER_PROPERTY ID into this ABI.
        guest_column->column_id = static_cast<uint16_t>(source_column.id);
        switch (source_column.type) {
          case StatValueType::kUnset:
            guest_column->value.type = XUserDataType::kUnset;
            break;
          case StatValueType::kInt32:
            guest_column->value.type = XUserDataType::kInt32;
            guest_column->value.value.s32 = std::get<int32_t>(source_column.value);
            break;
          case StatValueType::kInt64:
            guest_column->value.type = XUserDataType::kInt64;
            guest_column->value.value.s64 = std::get<int64_t>(source_column.value);
            break;
          case StatValueType::kDouble:
            guest_column->value.type = XUserDataType::kDouble;
            guest_column->value.value.f64 = std::get<double>(source_column.value);
            break;
          case StatValueType::kUnicode: {
            const auto& text = std::get<std::u16string>(source_column.value);
            const size_t byte_count = (text.size() + 1) * sizeof(char16_t);
            guest_column->value.type = XUserDataType::kWString;
            guest_column->value.value.unicode.size = static_cast<uint32_t>(byte_count);
            guest_column->value.value.unicode.pointer =
                guest_address + static_cast<uint32_t>(payload_offset);
            for (size_t index = 0; index < text.size(); ++index) {
              rex::memory::store_and_swap<uint16_t>(
                  output.data() + payload_offset + index * sizeof(char16_t), text[index]);
            }
            payload_offset += byte_count;
            break;
          }
          case StatValueType::kBinary: {
            const auto& value = std::get<std::vector<uint8_t>>(source_column.value);
            guest_column->value.type = XUserDataType::kBinary;
            guest_column->value.value.binary.size = static_cast<uint32_t>(value.size());
            guest_column->value.value.binary.pointer =
                value.empty() ? 0 : guest_address + static_cast<uint32_t>(payload_offset);
            if (!value.empty()) {
              std::memcpy(output.data() + payload_offset, value.data(), value.size());
              payload_offset += value.size();
            }
            break;
          }
        }
        column_offset += sizeof(*guest_column);
      }
      row_offset += sizeof(*guest_row);
    }
  }
  return payload_offset == *required_size;
}

}  // namespace rex::kernel::xam::detail
