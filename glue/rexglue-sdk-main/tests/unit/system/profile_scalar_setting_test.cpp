#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory.h>
#include <rex/system/xam/user_profile.h>

namespace rex::system::xam {

TEST_CASE("Profile scalar setting classes emit their XAM data types and values", "[xam][profile]") {
  std::array<uint8_t, 64> payload{};
  UserProfile::SettingByteStream stream(0x2000, payload.data(), payload.size());
  X_USER_PROFILE_SETTING_DATA data{};

  UserProfile::Int32Setting int32_setting(1, -17);
  int32_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::INT32));
  CHECK(static_cast<int32_t>(data.s32) == -17);

  UserProfile::Int64Setting int64_setting(2, -123456789);
  int64_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::INT64));
  CHECK(static_cast<int64_t>(data.s64) == -123456789);

  UserProfile::DoubleSetting double_setting(3, 42.5);
  double_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::DOUBLE));
  CHECK(static_cast<double>(data.f64) == 42.5);

  UserProfile::FloatSetting float_setting(4, -3.25f);
  float_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::FLOAT));
  CHECK(static_cast<float>(data.f32) == -3.25f);

  UserProfile::DateTimeSetting datetime_setting(5, 987654321);
  datetime_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::DATETIME));
  CHECK(static_cast<uint64_t>(data.filetime) == 987654321);

  UserProfile::UnicodeSetting unicode_setting(6, u"Liberty");
  unicode_setting.Append(&data, &stream);
  CHECK(data.type == static_cast<uint8_t>(UserProfile::Setting::Type::WSTRING));
  CHECK(static_cast<uint32_t>(data.unicode.size) == 16);
  CHECK(static_cast<uint32_t>(data.unicode.ptr) == 0x2000);
  CHECK(memory::load_and_swap<uint16_t>(payload.data()) == u'L');
  CHECK(memory::load_and_swap<uint16_t>(payload.data() + 14) == 0);
}

}  // namespace rex::system::xam
