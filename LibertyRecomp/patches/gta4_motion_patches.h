#pragma once

// =============================================================================
// GTA IV Motion Control Patches
// =============================================================================
// Implements PS3 Sixaxis motion control script commands for PlayStation controllers
// Provides motion sensing integration for DualShock 3/4 and DualSense controllers

#include <cstdint>

namespace MotionPatches {

// ============================================================================
// Lifecycle
// ============================================================================

// Initialize motion control system (call once at startup)
void Init();

// Update motion state (call each frame in game loop)
void Update();

// ============================================================================
// Script Command Support (for PPC hooks)
// ============================================================================

// GET_MOTION_SENSOR_VALUES - Returns accelerometer values in PS3 SIXAXIS format
// Values are in g-forces (1g = 9.81 m/s²)
void GetMotionSensorValues(float* outX, float* outY, float* outZ);

// GET_PAD_ORIENTATION / GET_PAD_PITCH_ROLL - Returns controller orientation
// Values are in radians
void GetPadOrientation(float* outPitch, float* outRoll);
void GetPadPitchRoll(float* outPitch, float* outRoll);

// ============================================================================
// Motion Control Preferences
// ============================================================================

// SET_ALL_MOTION_CONTROL_PREFERENCES_ON_OFF support
void SetMotionControlPreference(bool enabled);

// GET_MOTION_CONTROL_PREFERENCE support
bool GetMotionControlPreference();

// ============================================================================
// Motion Reload (Shake-to-Reload)
// ============================================================================

// Manually trigger motion reload (for testing)
void TriggerMotionReload();

// HAS_RELOADED_WITH_MOTION_CONTROL support
// Returns true once after shake gesture detected, then resets
bool HasReloadedWithMotionControl();

// ============================================================================
// Vehicle Motion Controls
// ============================================================================

// Get steering input from motion sensor (-1 to +1 range)
// Used for Config::MotionSteering
float GetMotionSteeringInput();

// Get helicopter control input from motion sensor
// Used for Config::MotionHelicopter
void GetMotionHelicopterInput(float* outPitch, float* outRoll);

// ============================================================================
// Gyro Aiming
// ============================================================================

// Get aim offset from gyroscope for fine aiming adjustment
// Used for Config::MotionAiming
void GetMotionAimOffset(float* outX, float* outY);

// ============================================================================
// Phase 2: Motorcycle Wheelie Balance
// ============================================================================

// Get wheelie balance input from motion sensor
// Returns pitch-based balance assist for wheelie control (-1 to +1)
float GetMotionWheelieBalance();

// Called when player starts/stops performing a wheelie
void SetWheelieState(bool performingWheelie);

// Check if currently performing wheelie
bool IsPerformingWheelie();

} // namespace MotionPatches

// ============================================================================
// Vehicle State Tracking (for motion control hooks)
// ============================================================================

namespace VehicleState {
    void SetInVehicle(bool inVehicle);
    void SetInHelicopter(bool inHeli);
    void SetInBoat(bool inBoat);
    void SetAiming(bool aiming);
    void SetOnMotorcycle(bool onBike);  // Phase 2
    
    bool IsInVehicle();
    bool IsInHelicopter();
    bool IsInBoat();
    bool IsAiming();
    bool IsOnMotorcycle();  // Phase 2
}
