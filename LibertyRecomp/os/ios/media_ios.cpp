#include <os/media.h>
#include <AVFoundation/AVAudioSession.h>

bool os::media::IsExternalMediaPlaying()
{
    // Return true if another app is currently playing audio (e.g., Spotify)
    return [[AVAudioSession sharedInstance] isOtherAudioPlaying];
}
