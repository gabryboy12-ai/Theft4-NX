// =============================================================================
// GTA IV DualShock 4 / DualSense Patches
// =============================================================================
// Implements DualShock 4 and DualSense specific features:
// - Light bar (wanted level, health indicators)
// - Touchpad gestures (radio switching, weapon selection)
// - Enhanced haptics (DualSense)

#pragma once

#include <cstdint>

namespace DS4Patches {

// ============================================================================
// Lifecycle
// ============================================================================

// Initialize DualShock 4 features (call once at startup)
void Init();

// Update DualShock 4 features (call each frame in game loop)
void Update();

// ============================================================================
// Light Bar Control
// ============================================================================

// Set light bar based on game state (called automatically in Update)
void UpdateLightBar();

// Force a specific light bar color (overrides automatic control)
void SetLightBarOverride(uint8_t r, uint8_t g, uint8_t b);
void ClearLightBarOverride();

// Get current light bar state
void GetLightBarColor(uint8_t* r, uint8_t* g, uint8_t* b);

// ============================================================================
// Game State Hooks (called from PPC patches)
// ============================================================================

// Called when wanted level changes
void OnWantedLevelChanged(int newLevel);

// Called when player health changes
void OnHealthChanged(int currentHealth, int maxHealth);

// Called when player dies
void OnPlayerDeath();

// Called when player respawns
void OnPlayerRespawn();

// ============================================================================
// Touchpad Gestures
// ============================================================================

enum class TouchpadGesture {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Tap,
    DoubleTap
};

// Get last detected gesture (resets after reading)
TouchpadGesture GetLastGesture();

// Check for specific gesture
bool WasGestureDetected(TouchpadGesture gesture);

// ============================================================================
// Touchpad Actions
// ============================================================================

// Called when radio station should change
// direction: -1 = previous, +1 = next
void ChangeRadioStation(int direction);

// ============================================================================
// Haptic Feedback (Phase 1-3 Features)
// ============================================================================

// Explosion haptics - triggered when explosion occurs nearby
// intensity: 0.0 to 1.0 based on distance
void TriggerExplosionHaptic(float intensity);

// Collision/impact haptics - triggered when vehicle takes damage
// intensity: 0.0 to 1.0 based on damage amount
void TriggerImpactHaptic(float intensity);

// Nearby gunfire haptics - triggered when NPCs shoot nearby
// intensity: 0.0 to 1.0 based on distance
void TriggerGunfireHaptic(float intensity);

// Weather haptics - continuous subtle feedback during rain
// enabled: true when raining, false otherwise
void SetWeatherHaptic(bool enabled, float intensity = 0.15f);

// Gear change haptics - brief bump when vehicle changes gear
void TriggerGearChangeHaptic();

// ============================================================================
// Game Event Hooks (called from PPC patches)
// ============================================================================

// Called when an explosion occurs in the game world
// distance: distance from player to explosion
// radius: explosion radius
void OnExplosion(float x, float y, float z, float radius, int type);

// Called when vehicle health changes
void OnVehicleHealthChanged(int currentHealth, int previousHealth);

// Called when NPC fires a weapon nearby
void OnNearbyGunfire(float distance);

// Called when weather changes
void OnWeatherChanged(int weatherType);

// Called to update speed-based systems (gear detection)
void OnVehicleSpeedUpdate(float speed);

// ============================================================================
// Camera Control (Phase 3)
// ============================================================================

// Toggle cinematic camera (called on touchpad tap)
void ToggleCinematicCamera();

// Check if camera toggle is pending (for PPC hook)
bool IsCameraTogglePending();
void ClearCameraTogglePending();

// Called when weapon should cycle
// direction: -1 = previous, +1 = next
void CycleWeapon(int direction);

} // namespace DS4Patches
