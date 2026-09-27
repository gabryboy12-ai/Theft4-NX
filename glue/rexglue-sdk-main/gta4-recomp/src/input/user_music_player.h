#pragma once

#include <filesystem>
#include <memory>

namespace gta4::input {

class UserMusicPlayer {
 public:
  explicit UserMusicPlayer(std::filesystem::path music_root);
  ~UserMusicPlayer();

  UserMusicPlayer(const UserMusicPlayer&) = delete;
  UserMusicPlayer& operator=(const UserMusicPlayer&) = delete;

  // Returns false without changing playback when no supported user track was
  // discovered. The input bridge uses this acknowledgement before muting the
  // guest vehicle radio, so an empty User Music directory can never silence
  // the title's own audio.
  bool Next();
  bool Previous();
  bool Stop();
  bool HasTracks() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The GTA input bridge runs on the guest thread, while AVFoundation is owned
// by the app. Track requests acknowledge the pre-scanned playlist immediately
// and enqueue all playback work on the player's serial queue.
void PublishUserMusicPlayer(UserMusicPlayer* player);
bool IsUserMusicAvailable();
bool RequestUserMusicNext();
bool RequestUserMusicPrevious();
bool RequestUserMusicStop();

}  // namespace gta4::input
