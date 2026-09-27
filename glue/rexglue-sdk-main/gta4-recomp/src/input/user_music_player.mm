#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include "input/user_music_player.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <system_error>
#include <vector>

namespace gta4::input {
namespace {

std::mutex g_user_music_player_mutex;
UserMusicPlayer* g_user_music_player = nullptr;
const void* const kUserMusicQueueKey = &kUserMusicQueueKey;

bool IsSupportedUserMusicFile(const std::filesystem::path& path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
  return extension == ".aac" || extension == ".aif" || extension == ".aiff" ||
         extension == ".caf" || extension == ".flac" || extension == ".m4a" ||
         extension == ".mp3" || extension == ".wav";
}

}  // namespace

struct UserMusicPlayer::Impl {
  struct CompletionLifetime {
    std::atomic<Impl*> owner{nullptr};
  };

  explicit Impl(std::filesystem::path root)
      : music_root(std::move(root)),
        queue(dispatch_queue_create("org.libertyrecomp.user-music",
                                    DISPATCH_QUEUE_SERIAL)),
        engine([[AVAudioEngine alloc] init]),
        player([[AVAudioPlayerNode alloc] init]),
        completion_lifetime(std::make_shared<CompletionLifetime>()) {
    dispatch_queue_set_specific(queue, kUserMusicQueueKey, this, nullptr);
    completion_lifetime->owner.store(this, std::memory_order_release);
    [engine attachNode:player];
    [engine connect:player to:engine.mainMixerNode format:nil];
    [engine prepare];
    // Discover the host-side playlist before the player is published to the
    // guest input thread. Key handling can then acknowledge a nonempty
    // playlist without doing filesystem or AVFoundation work on that thread.
    Scan();
  }

  ~Impl() {
    completion_lifetime->owner.store(nullptr, std::memory_order_release);
    auto teardown = ^{
      stopping = true;
      ++generation;
      [player stop];
      [engine stop];
      current_file = nil;
      files.clear();
      tracks_available.store(false, std::memory_order_release);
      active.store(false, std::memory_order_release);
    };
    // The app currently owns and destroys the player off-queue, but preserve
    // that invariant defensively so future queue-owned teardown cannot
    // deadlock by synchronously dispatching onto itself.
    if (dispatch_get_specific(kUserMusicQueueKey) ==
        static_cast<void*>(this)) {
      teardown();
    } else {
      dispatch_sync(queue, teardown);
    }
  }

  bool RequestRelative(int direction) {
    if (!tracks_available.load(std::memory_order_acquire)) {
      REXLOG_WARN("gta4-user-music: action={} accepted=false reason=no-tracks",
                  direction < 0 ? "previous" : "next");
      return false;
    }

    active.store(true, std::memory_order_release);
    dispatch_async(queue, ^{
      if (stopping) {
        active.store(false, std::memory_order_release);
        return;
      }
      if (files.empty()) {
        active.store(false, std::memory_order_release);
        return;
      }
      if (!has_current) {
        current_index = direction < 0 ? files.size() - 1 : 0;
        has_current = true;
      } else if (direction < 0) {
        current_index = current_index == 0 ? files.size() - 1 : current_index - 1;
      } else {
        current_index = (current_index + 1) % files.size();
      }
      const bool scheduled = ScheduleCurrent();
      if (!scheduled) {
        has_current = false;
      }
      REXLOG_INFO("gta4-user-music: action={} scheduled={} track_index={}",
                  direction < 0 ? "previous" : "next", scheduled,
                  current_index);
    });
    return true;
  }

  bool EnqueueStop() {
    const bool was_active = active.exchange(false, std::memory_order_acq_rel);
    dispatch_async(queue, ^{
      if (stopping) {
        return;
      }
      ++generation;
      [player stop];
      current_file = nil;
      has_current = false;
      active.store(false, std::memory_order_release);
    });
    return was_active;
  }

  void Scan() {
    std::error_code error;
    std::filesystem::create_directories(music_root, error);
    error.clear();
    std::vector<std::filesystem::path> discovered;
    for (std::filesystem::directory_iterator iterator(music_root, error), end;
         !error && iterator != end; iterator.increment(error)) {
      const auto status = iterator->status(error);
      if (error) {
        break;
      }
      if (!std::filesystem::is_regular_file(status) ||
          !IsSupportedUserMusicFile(iterator->path())) {
        continue;
      }
      auto canonical = std::filesystem::weakly_canonical(iterator->path(), error);
      if (error) {
        error.clear();
        continue;
      }
      discovered.push_back(std::move(canonical));
    }
    std::ranges::sort(discovered);
    discovered.erase(std::unique(discovered.begin(), discovered.end()),
                     discovered.end());
    files = std::move(discovered);
    tracks_available.store(!files.empty(), std::memory_order_release);
    REXLOG_INFO("gta4-user-music: scan supported_files={}", files.size());
  }

  bool ScheduleCurrent() {
    ++generation;
    const uint64_t scheduled_generation = generation;
    [player stop];
    current_file = nil;

    NSError* error = nil;
    NSURL* url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:files[current_index].string().c_str()]];
    AVAudioFile* file = [[AVAudioFile alloc] initForReading:url error:&error];
    if (file == nil) {
      const char* description =
          error && error.localizedDescription.UTF8String
              ? error.localizedDescription.UTF8String
              : "unknown error";
      REXLOG_WARN("gta4-user-music: open failed: {}", description);
      active.store(false, std::memory_order_release);
      return false;
    }
    current_file = file;
    if (!engine.isRunning && ![engine startAndReturnError:&error]) {
      const char* description =
          error && error.localizedDescription.UTF8String
              ? error.localizedDescription.UTF8String
              : "unknown error";
      REXLOG_ERROR("gta4-user-music: audio engine start failed: {}", description);
      current_file = nil;
      active.store(false, std::memory_order_release);
      return false;
    }

    const auto lifetime = completion_lifetime;
    dispatch_queue_t callback_queue = queue;
    [player scheduleFile:file
                  atTime:nil
       completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
       completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) {
         dispatch_async(callback_queue, ^{
           if (Impl* owner =
                   lifetime->owner.load(std::memory_order_acquire)) {
             owner->AdvanceAfterCompletion(scheduled_generation);
           }
         });
       }];
    [player play];
    active.store(true, std::memory_order_release);
    REXLOG_INFO("gta4-user-music: playback started track_index={}", current_index);
    return true;
  }

  void AdvanceAfterCompletion(uint64_t completed_generation) {
    if (stopping || completed_generation != generation || files.empty()) {
      return;
    }
    current_index = (current_index + 1) % files.size();
    has_current = true;
    if (!ScheduleCurrent()) {
      has_current = false;
    }
  }

  std::filesystem::path music_root;
  dispatch_queue_t queue;
  AVAudioEngine* engine;
  AVAudioPlayerNode* player;
  AVAudioFile* current_file = nil;
  std::shared_ptr<CompletionLifetime> completion_lifetime;
  std::vector<std::filesystem::path> files;
  size_t current_index = 0;
  uint64_t generation = 0;
  std::atomic<bool> tracks_available{false};
  std::atomic<bool> active{false};
  bool has_current = false;
  bool stopping = false;
};

UserMusicPlayer::UserMusicPlayer(std::filesystem::path music_root)
    : impl_(std::make_unique<Impl>(std::move(music_root))) {}

UserMusicPlayer::~UserMusicPlayer() = default;

bool UserMusicPlayer::Next() { return impl_->RequestRelative(1); }

bool UserMusicPlayer::Previous() { return impl_->RequestRelative(-1); }

bool UserMusicPlayer::Stop() { return impl_->EnqueueStop(); }

bool UserMusicPlayer::HasTracks() const {
  return impl_->tracks_available.load(std::memory_order_acquire);
}

void PublishUserMusicPlayer(UserMusicPlayer* player) {
  std::lock_guard lock(g_user_music_player_mutex);
  g_user_music_player = player;
}

bool IsUserMusicAvailable() {
  std::lock_guard lock(g_user_music_player_mutex);
  return g_user_music_player && g_user_music_player->HasTracks();
}

bool RequestUserMusicNext() {
  std::lock_guard lock(g_user_music_player_mutex);
  return g_user_music_player && g_user_music_player->Next();
}

bool RequestUserMusicPrevious() {
  std::lock_guard lock(g_user_music_player_mutex);
  return g_user_music_player && g_user_music_player->Previous();
}

bool RequestUserMusicStop() {
  std::lock_guard lock(g_user_music_player_mutex);
  return g_user_music_player && g_user_music_player->Stop();
}

}  // namespace gta4::input
