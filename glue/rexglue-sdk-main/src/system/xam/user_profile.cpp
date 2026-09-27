/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <sstream>
#include <unordered_set>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace rex {
namespace system {
namespace xam {

UserProfile::UserProfile() {
  // 58410A1F checks the user XUID against a mask of 0x00C0000000000000 (3<<54),
  // if non-zero, it prevents the user from playing the game.
  // "You do not have permissions to perform this operation."
  xuid_ = 0xB13EBABEBABEBABE;
  name_ = "User";

  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=gfwl+live&start=195
  // https://github.com/arkem/py360/blob/master/py360/constants.py
  // XPROFILE_GAMER_YAXIS_INVERSION
  AddSetting(std::make_unique<Int32Setting>(0x10040002, 0));
  // XPROFILE_OPTION_CONTROLLER_VIBRATION
  AddSetting(std::make_unique<Int32Setting>(0x10040003, 3));
  // XPROFILE_GAMERCARD_ZONE
  AddSetting(std::make_unique<Int32Setting>(0x10040004, 0));
  // XPROFILE_GAMERCARD_REGION
  AddSetting(std::make_unique<Int32Setting>(0x10040005, 0));
  // XPROFILE_GAMERCARD_CRED
  AddSetting(std::make_unique<Int32Setting>(0x10040006, 0xFA));
  // XPROFILE_GAMERCARD_REP
  AddSetting(std::make_unique<FloatSetting>(0x5004000B, 0.0f));
  // XPROFILE_OPTION_VOICE_MUTED
  AddSetting(std::make_unique<Int32Setting>(0x1004000C, 0));
  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
  AddSetting(std::make_unique<Int32Setting>(0x1004000D, 0));
  // XPROFILE_OPTION_VOICE_VOLUME
  AddSetting(std::make_unique<Int32Setting>(0x1004000E, 0x64));
  // XPROFILE_GAMERCARD_MOTTO
  AddSetting(std::make_unique<UnicodeSetting>(0x402C0011, u""));
  // XPROFILE_GAMERCARD_TITLES_PLAYED
  AddSetting(std::make_unique<Int32Setting>(0x10040012, 1));
  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040013, 0));
  // XPROFILE_GAMER_DIFFICULTY
  AddSetting(std::make_unique<Int32Setting>(0x10040015, 0));
  // XPROFILE_GAMER_CONTROL_SENSITIVITY
  AddSetting(std::make_unique<Int32Setting>(0x10040018, 0));
  // Preferred color 1
  AddSetting(std::make_unique<Int32Setting>(0x1004001D, 0xFFFF0000u));
  // Preferred color 2
  AddSetting(std::make_unique<Int32Setting>(0x1004001E, 0xFF00FF00u));
  // XPROFILE_GAMER_ACTION_AUTO_AIM
  AddSetting(std::make_unique<Int32Setting>(0x10040022, 1));
  // XPROFILE_GAMER_ACTION_AUTO_CENTER
  AddSetting(std::make_unique<Int32Setting>(0x10040023, 0));
  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040024, 0));
  // XPROFILE_GAMER_RACE_TRANSMISSION
  AddSetting(std::make_unique<Int32Setting>(0x10040026, 0));
  // XPROFILE_GAMER_RACE_CAMERA_LOCATION
  AddSetting(std::make_unique<Int32Setting>(0x10040027, 0));
  // XPROFILE_GAMER_RACE_BRAKE_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040028, 0));
  // XPROFILE_GAMER_RACE_ACCELERATOR_CONTROL
  AddSetting(std::make_unique<Int32Setting>(0x10040029, 0));
  // XPROFILE_GAMERCARD_TITLE_CRED_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040038, 0));
  // XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED
  AddSetting(std::make_unique<Int32Setting>(0x10040039, 0));

  // If we set this, games will try to get it.
  // XPROFILE_GAMERCARD_PICTURE_KEY
  AddSetting(std::make_unique<UnicodeSetting>(0x4064000F, u"gamercard_picture_key"));

  // XPROFILE_TITLE_SPECIFIC1
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFF));
  // XPROFILE_TITLE_SPECIFIC2
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFE));
  // XPROFILE_TITLE_SPECIFIC3
  AddSetting(std::make_unique<BinarySetting>(0x63E83FFD));
}

uint32_t UserProfile::signin_state() const {
  return kernel_state_ && kernel_state_->live_compatibility() &&
                 kernel_state_->live_compatibility()->signed_in()
             ? 2
             : 1;
}

uint64_t UserProfile::xuid() const {
  std::lock_guard lock(mutex_);
  return xuid_;
}

std::string UserProfile::name() const {
  std::lock_guard lock(mutex_);
  return name_;
}

void UserProfile::SetIdentity(uint64_t xuid, std::string name) {
  std::lock_guard lock(mutex_);
  if (xuid) {
    xuid_ = xuid;
  }
  if (!name.empty()) {
    name_ = std::move(name);
  }
}

std::string UserProfile::storage_id() const {
  std::lock_guard lock(mutex_);
  return StorageIdLocked();
}

std::string UserProfile::StorageIdLocked() const {
  // Multiplayer display names are user-editable and must not select the
  // title-specific profile directory. XUID is the stable local identity that
  // already owns this XAM profile.
  return fmt::format("{:016X}", xuid_);
}

std::optional<std::filesystem::path> UserProfile::ResolveTitleSettingContentPath(
    uint32_t setting_id) const {
  if (!kernel_state_ || (setting_id & 0x3F00) != 0x3F00) {
    return std::nullopt;
  }
  // Resolve before acquiring the profile mutex. ContentManager consults the
  // stable profile identity while migrating legacy directories.
  return kernel_state_->content_manager()->ResolveGameUserContentPath();
}

void UserProfile::AddSetting(std::unique_ptr<Setting> setting) {
  const auto content_dir = ResolveTitleSettingContentPath(setting->setting_id);
  std::lock_guard lock(mutex_);
  AddSettingLocked(std::move(setting), content_dir ? &*content_dir : nullptr);
}

void UserProfile::AddSettingLocked(std::unique_ptr<Setting> setting,
                                   const std::filesystem::path* content_dir) {
  Setting* previous_setting = setting.get();
  std::swap(settings_[setting->setting_id], previous_setting);

  if (kernel_state_ && setting->is_set && setting->is_title_specific()) {
    assert_not_null(content_dir);
    SaveSettingLocked(setting.get(), *content_dir);
  }

  if (previous_setting) {
    // replace: swap out the old setting from the owning list
    for (auto vec_it = setting_list_.begin(); vec_it != setting_list_.end(); ++vec_it) {
      if (vec_it->get() == previous_setting) {
        vec_it->swap(setting);
        break;
      }
    }
  } else {
    // new setting: add to the owning list
    setting_list_.push_back(std::move(setting));
  }
}

bool UserProfile::HasSetting(uint32_t setting_id) const {
  std::lock_guard lock(mutex_);
  return settings_.contains(setting_id);
}

UserProfile::Setting* UserProfile::GetSettingLocked(
    uint32_t setting_id, const std::filesystem::path* content_dir) {
  const auto& it = settings_.find(setting_id);
  if (it == settings_.end()) {
    return nullptr;
  }
  UserProfile::Setting* setting = it->second;
  if (kernel_state_ && setting->is_title_specific()) {
    // If what we have loaded in memory isn't for the title that is running
    // right now, then load it from disk.
    if (kernel_state_->title_id() != setting->loaded_title_id) {
      assert_not_null(content_dir);
      LoadSettingLocked(setting, *content_dir);
    }
  }
  return setting;
}

std::optional<UserProfile::SettingReadResult> UserProfile::AppendSetting(
    uint32_t setting_id, X_USER_PROFILE_SETTING_DATA* data, SettingByteStream* stream) {
  const auto content_dir = ResolveTitleSettingContentPath(setting_id);
  std::lock_guard lock(mutex_);
  auto* setting = GetSettingLocked(setting_id, content_dir ? &*content_dir : nullptr);
  if (!setting) {
    return std::nullopt;
  }
  if (setting->is_set) {
    setting->Append(data, stream);
  }
  return SettingReadResult{.is_set = setting->is_set,
                           .is_title_specific = setting->is_title_specific()};
}

bool UserProfile::ValidateGta4TitleProfileBlob(std::span<const uint8_t> blob) {
  if (blob.size() > kGta4TitleProfileMaximumSize ||
      blob.size() % kGta4TitleProfileEntrySize != 0) {
    return false;
  }

  std::unordered_set<uint32_t> keys;
  keys.reserve(blob.size() / kGta4TitleProfileEntrySize);
  for (size_t offset = 0; offset < blob.size(); offset += kGta4TitleProfileEntrySize) {
    const uint32_t key = memory::load_and_swap<uint32_t>(blob.data() + offset);
    if (!keys.insert(key).second) {
      return false;
    }
  }
  return true;
}

bool UserProfile::WriteGuestBinarySetting(uint32_t title_id, uint32_t setting_id,
                                          std::vector<uint8_t> value) {
  const bool is_gta4_profile =
      title_id == kGta4TitleId && setting_id == kGta4TitleProfileSettingId;
  if (is_gta4_profile && !ValidateGta4TitleProfileBlob(value)) {
    return false;
  }

  TitleProfileWriteCallback callback;
  uint64_t generation = 0;
  std::vector<uint8_t> callback_blob;
  const auto content_dir = ResolveTitleSettingContentPath(setting_id);
  {
    std::lock_guard lock(mutex_);
    AddSettingLocked(std::make_unique<BinarySetting>(setting_id, value),
                     content_dir ? &*content_dir : nullptr);
    if (is_gta4_profile) {
      generation = ++gta4_title_profile_generation_;
      callback = gta4_title_profile_write_callback_;
      if (callback) {
        callback_blob = std::move(value);
      }
    }
  }
  if (callback) {
    callback(std::move(callback_blob), generation);
  }
  return true;
}

std::optional<std::vector<uint8_t>> UserProfile::SnapshotGta4TitleProfileBlob() {
  const auto content_dir = ResolveTitleSettingContentPath(kGta4TitleProfileSettingId);
  std::lock_guard lock(mutex_);
  auto* setting = GetSettingLocked(kGta4TitleProfileSettingId,
                                   content_dir ? &*content_dir : nullptr);
  auto* binary = setting ? dynamic_cast<BinarySetting*>(setting) : nullptr;
  if (!binary || !binary->is_set || !ValidateGta4TitleProfileBlob(binary->value)) {
    return std::nullopt;
  }
  return binary->value;
}

bool UserProfile::ImportGta4TitleProfileBlobIfGeneration(
    std::span<const uint8_t> blob, uint64_t expected_generation) {
  if (!ValidateGta4TitleProfileBlob(blob)) {
    return false;
  }
  const auto content_dir = ResolveTitleSettingContentPath(kGta4TitleProfileSettingId);
  std::lock_guard lock(mutex_);
  if (gta4_title_profile_generation_ != expected_generation) {
    return false;
  }
  AddSettingLocked(
      std::make_unique<BinarySetting>(kGta4TitleProfileSettingId,
                                      std::vector<uint8_t>(blob.begin(), blob.end())),
      content_dir ? &*content_dir : nullptr);
  return true;
}

uint64_t UserProfile::gta4_title_profile_generation() const {
  std::lock_guard lock(mutex_);
  return gta4_title_profile_generation_;
}

void UserProfile::SetGta4TitleProfileWriteCallback(TitleProfileWriteCallback callback) {
  std::lock_guard lock(mutex_);
  gta4_title_profile_write_callback_ = std::move(callback);
}

void UserProfile::LoadSettingLocked(UserProfile::Setting* setting,
                                    const std::filesystem::path& content_dir) {
  if (setting->is_title_specific()) {
    BinarySetting* gta4_profile = nullptr;
    if (kernel_state_->title_id() == kGta4TitleId &&
        setting->setting_id == kGta4TitleProfileSettingId) {
      gta4_profile = dynamic_cast<BinarySetting*>(setting);
    }
    if (gta4_profile) {
      gta4_profile->value.clear();
      gta4_profile->is_set = false;
    }
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "rb");
    if (file) {
      fseek(file, 0, SEEK_END);
      const long input_file_size = ftell(file);
      fseek(file, 0, SEEK_SET);
      if (input_file_size < 0 ||
          (gta4_profile && static_cast<unsigned long>(input_file_size) >
                               kGta4TitleProfileMaximumSize)) {
        fclose(file);
        REXSYS_WARN("Ignoring oversized or unreadable GTA IV title-profile blob from {}",
                    file_path.string());
        setting->loaded_title_id = kernel_state_->title_id();
        return;
      }

      std::vector<uint8_t> serialized_data(static_cast<size_t>(input_file_size));
      const size_t bytes_read = fread(serialized_data.data(), 1, serialized_data.size(), file);
      fclose(file);
      if (bytes_read != serialized_data.size()) {
        REXSYS_WARN("Ignoring incompletely read profile setting from {}", file_path.string());
      } else if (!gta4_profile || ValidateGta4TitleProfileBlob(serialized_data)) {
        setting->Deserialize(std::move(serialized_data));
      } else {
        REXSYS_WARN("Ignoring malformed GTA IV title-profile blob from {}", file_path.string());
      }
    }
    setting->loaded_title_id = kernel_state_->title_id();
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to load unsupported profile setting from disk");
  }
}

void UserProfile::SaveSettingLocked(UserProfile::Setting* setting,
                                    const std::filesystem::path& content_dir) {
  if (setting->is_title_specific()) {
    auto serialized_setting = setting->Serialize();
    std::filesystem::create_directories(content_dir);
    auto setting_id = fmt::format("{:08X}", setting->setting_id);
    auto file_path = content_dir / setting_id;
    auto file = rex::filesystem::OpenFile(file_path, "wb");
    if (!file) {
      REXSYS_WARN("Unable to save profile setting {:08X} to {}", setting->setting_id,
                  file_path.string());
      return;
    }
    fwrite(serialized_setting.data(), 1, serialized_setting.size(), file);
    fclose(file);
  } else {
    // Unsupported for now.  Other settings aren't per-game and need to be
    // stored some other way.
    REXSYS_WARN("Attempting to save unsupported profile setting to disk");
  }
}

}  // namespace xam
}  // namespace system
}  // namespace rex
