#pragma once

#include <cstdint>

/**
 * DualSense Controller Support for LibertyRecomp
 * 
 * Implements adaptive trigger effects and enhanced haptic feedback for the 
 * PlayStation 5 DualSense controller using SDL2's built-in HIDAPI support.
 * 
 * Protocol Reference:
 * - VID: 0x054C, PID: 0x0CE6 (Standard), 0x0DF2 (Edge)
 * - USB Report ID: 0x02 (48 bytes minimum)
 * - Bluetooth Report ID: 0x31 (78 bytes with CRC32)
 * - Trigger Effect: 11 bytes per trigger (mode + 10 parameters)
 * 
 * Effect modes researched from:
 * - pydualsense (github.com/flok/pydualsense)
 * - DualSense-Windows (github.com/Ohjurot/DualSense-Windows)
 * - SDL2 hidapi_ps5.c
 * 
 * Realistic Values Reference:
 * - Position: 0-255 (0=no travel, 255=full travel)
 * - Force: 0-255 (0=no resistance, 255=maximum resistance)
 * - pydualsense uses forces[0-6] array, each 0-255
 * - DualSense-Windows uses 0-8 scale for mode 0x01/0x02
 */

namespace DualSense
{
    // ========================================================================
    // DEVICE IDENTIFIERS
    // ========================================================================
    
    constexpr uint16_t VENDOR_ID_SONY = 0x054C;
    constexpr uint16_t PRODUCT_ID_DUALSENSE = 0x0CE6;
    constexpr uint16_t PRODUCT_ID_DUALSENSE_EDGE = 0x0DF2;
    
    // ========================================================================
    // TRIGGER EFFECT MODES
    // ========================================================================
    // Researched from pydualsense enums.py and DualSense-Windows DS5State.h
    
    enum class TriggerMode : uint8_t
    {
        // Basic modes (from pydualsense and DualSense-Windows research)
        Off = 0x00,                 // No resistance - trigger moves freely
        Continuous = 0x01,          // Continuous resistance from start position (Rigid in pydualsense)
        Section = 0x02,             // Resistance in a defined zone (Pulse in pydualsense)
        
        // Extended modes (from Reddit/community research)
        // These combine zones with vibration
        ContinuousSlope = 0x05,     // Rigid_B in pydualsense - resistance increases gradually
        SectionSlope = 0x06,        // Pulse_B in pydualsense - section with vibration
        
        // Multi-zone modes (from extended research)
        MultiZone = 0x21,           // Rigid_A - multiple resistance zones
        MultiZoneSection = 0x22,    // Pulse_A - pulsing in multiple zones
        MultiZoneSectionSlope = 0x23, // Combined multi-zone with slope
        
        // Combined modes
        MultiZoneContinuous = 0x25, // Rigid_AB - full combined
        MultiZonePulse = 0x26,      // Pulse_AB - full pulsing combined
        
        // Special
        Calibration = 0xFC,         // Trigger calibration mode (use with caution)
        
        // Legacy aliases for compatibility with implementation plan
        Feedback = Continuous,
        Weapon = Section,
        Vibration = SectionSlope,
    };
    
    // ========================================================================
    // TRIGGER EFFECT STRUCTURE
    // ========================================================================
    // 11 bytes per trigger effect
    
    struct TriggerEffect
    {
        TriggerMode mode = TriggerMode::Off;
        
        // Parameters vary by mode. Using union for clarity.
        union
        {
            // Raw bytes for direct manipulation
            uint8_t params[10] = {0};
            
            // Mode 0x01 (Continuous): Resistance from start position to end
            struct
            {
                uint8_t startPosition;  // 0-255: Where resistance begins (0=top, 255=bottom)
                uint8_t strength;       // 0-255: Resistance force (higher = harder to press)
            } continuous;
            
            // Mode 0x02 (Section): Resistance only between start and end
            struct
            {
                uint8_t startPosition;  // 0-255: Where resistance zone begins
                uint8_t endPosition;    // 0-255: Where resistance zone ends
                uint8_t strength;       // 0-255: Resistance force within zone
            } section;
            
            // Mode 0x06 (Vibration/SectionSlope): Vibrating resistance
            struct
            {
                uint8_t startPosition;  // Where vibration begins
                uint8_t strength;       // Vibration amplitude (0-255)
                uint8_t frequency;      // Vibration frequency (0-255, higher = faster)
            } vibration;
            
            // Mode 0x21 (MultiZone): Multiple resistance zones
            // forces[0-6] define resistance at 7 points along trigger travel
            struct
            {
                uint8_t startPosition;  // Where effect begins
                uint8_t keepEffect;     // 0x02 to keep effect when fully pressed
                uint8_t reserved;
                uint8_t beginForce;     // Force at trigger value >= 128
                uint8_t middleForce;    // Force at trigger value <= 128
                uint8_t endForce;       // Force when fully pressed
                uint8_t reserved2[3];
                uint8_t frequency;      // Effect execution frequency
            } extended;
        };
        
        // Convenience constructors
        
        static TriggerEffect MakeOff()
        {
            return TriggerEffect{TriggerMode::Off, {}};
        }
        
        /**
         * Continuous resistance starting at position with given strength
         * @param startPos 0-255, where resistance begins (0=no travel needed)
         * @param strength 0-255, resistance force (0=none, 255=max)
         * 
         * Realistic examples:
         * - Light pistol: startPos=25 (10% travel), strength=60
         * - Heavy revolver: startPos=50 (20% travel), strength=150
         * - Vehicle gas: startPos=0, strength=40-100 based on throttle
         */
        static TriggerEffect MakeContinuous(uint8_t startPos, uint8_t strength)
        {
            TriggerEffect effect;
            effect.mode = TriggerMode::Continuous;
            effect.continuous.startPosition = startPos;
            effect.continuous.strength = strength;
            return effect;
        }
        
        /**
         * Section resistance within a defined zone
         * @param startPos 0-255, where resistance zone begins
         * @param endPos 0-255, where resistance zone ends
         * @param strength 0-255, resistance force within zone
         * 
         * Realistic examples:
         * - Pump shotgun: startPos=25, endPos=180, strength=200 (heavy pump)
         * - Semi-auto: startPos=30, endPos=100, strength=120 (crisp break)
         * - ADS tension: startPos=0, endPos=128, strength=80
         */
        static TriggerEffect MakeSection(uint8_t startPos, uint8_t endPos, uint8_t strength)
        {
            TriggerEffect effect;
            effect.mode = TriggerMode::Section;
            effect.section.startPosition = startPos;
            effect.section.endPosition = endPos;
            effect.section.strength = strength;
            return effect;
        }
        
        /**
         * Vibrating/pulsing resistance effect
         * @param startPos 0-255, where vibration begins
         * @param strength 0-255, vibration amplitude
         * @param freq 0-255, vibration frequency (higher = faster pulse)
         * 
         * Realistic examples:
         * - Full-auto fire: startPos=30, strength=80, freq=60 (rapid pulses)
         * - ABS braking: startPos=0, strength=120, freq=30 (slow pulses)
         * - Engine rumble: startPos=0, strength=40, freq=20
         */
        static TriggerEffect MakeVibration(uint8_t startPos, uint8_t strength, uint8_t freq)
        {
            TriggerEffect effect;
            effect.mode = TriggerMode::Vibration;
            effect.vibration.startPosition = startPos;
            effect.vibration.strength = strength;
            effect.vibration.frequency = freq;
            return effect;
        }
        
        /**
         * Extended effect with multiple force zones
         * Used for complex effects like bow draw or vehicle transmission
         * @param startPos Where effect begins
         * @param beginForce Force in first half of trigger
         * @param middleForce Force at midpoint
         * @param endForce Force when fully pressed
         * @param keepEffect Whether to continue when fully pressed
         * 
         * Realistic examples:
         * - Bow draw: startPos=10, begin=40, middle=120, end=200, keep=true
         * - Diesel engine: startPos=0, begin=60, middle=80, end=40, keep=false
         */
        static TriggerEffect MakeExtended(uint8_t startPos, uint8_t beginForce, 
                                          uint8_t middleForce, uint8_t endForce, 
                                          bool keepEffect = false)
        {
            TriggerEffect effect;
            effect.mode = TriggerMode::MultiZone;
            effect.extended.startPosition = startPos;
            effect.extended.keepEffect = keepEffect ? 0x02 : 0x00;
            effect.extended.beginForce = beginForce;
            effect.extended.middleForce = middleForce;
            effect.extended.endForce = endForce;
            effect.extended.frequency = 1; // Default frequency
            return effect;
        }
    };
    
    // ========================================================================
    // WEAPON TRIGGER PROFILES - REALISTIC VALUES
    // ========================================================================
    // Based on DualSense-Windows examples and real-world weapon feel
    // startPosition: Where resistance begins (0-255, 0=top of travel)
    // strength: Base resistance force (0-255)
    // recoilStrength: Kickback intensity on fire (0-255)
    // recoilFrequency: Vibration speed for auto-fire (0-255)
    
    struct WeaponTriggerProfile
    {
        uint8_t startPosition;      // Trigger travel before resistance (0=instant, 128=halfway)
        uint8_t strength;           // Base resistance (0-255)
        uint8_t recoilStrength;     // Recoil kick (0-255)
        uint8_t recoilFrequency;    // For automatic weapons (0-255)
        bool isAutomatic;           // Continuous fire vibration
        TriggerMode mode;           // Primary effect mode
    };
    
    // GTA IV Weapon Slot Profiles
    // Slot 0: Unarmed, 1: Melee, 2: Pistols, 3: Shotguns, 4: SMGs, 
    // 5: Assault Rifles, 6: Sniper, 7: Heavy, 8: Thrown
    
    constexpr WeaponTriggerProfile WEAPON_PROFILES[] = {
        // Slot 0: Unarmed - No trigger effect
        { 0, 0, 0, 0, false, TriggerMode::Off },
        
        // Slot 1: Melee - No trigger effect (melee uses different button)
        { 0, 0, 0, 0, false, TriggerMode::Off },
        
        // Slot 2: Pistols - Light trigger, crisp break
        // Real pistol has light take-up, then crisp break point
        // Glock: ~2.5kg pull, Combat Pistol similar
        { 25, 70, 45, 0, false, TriggerMode::Section },
        
        // Slot 3: Shotguns - Heavy trigger, pump action feel
        // Pump shotgun has heavy trigger pull + chamber resistance
        // Remington 870: ~3-4kg pull
        { 30, 180, 110, 0, false, TriggerMode::Section },
        
        // Slot 4: SMGs - Medium resistance, rapid fire vibration
        // MP5/Uzi: Moderate trigger pull, ~3kg
        { 15, 80, 50, 70, true, TriggerMode::Vibration },
        
        // Slot 5: Assault Rifles - Moderate resistance, controllable auto
        // M4/AK: ~3.5kg trigger, manageable recoil
        { 20, 100, 70, 55, true, TriggerMode::Vibration },
        
        // Slot 6: Sniper Rifles - Heavy resistance, deliberate shot
        // Bolt action feel, heavy trigger for precision
        { 40, 140, 90, 0, false, TriggerMode::Section },
        
        // Slot 7: Heavy Weapons (Rocket Launcher, Minigun)
        // Very heavy trigger or sustained fire
        { 50, 200, 150, 40, true, TriggerMode::Continuous },
        
        // Slot 8: Thrown - Light pull for grenade release
        // Light resistance, clean release
        { 10, 60, 20, 0, false, TriggerMode::Section },
    };
    
    // ========================================================================
    // VEHICLE TRIGGER PROFILES - REALISTIC VALUES
    // ========================================================================
    // Based on vehicle physics and driving feel
    
    struct VehicleTriggerProfile
    {
        // Throttle (R2) parameters
        uint8_t throttleStart;      // Where throttle resistance begins
        uint8_t throttleBase;       // Base throttle resistance
        uint8_t throttleMax;        // Maximum throttle resistance at redline
        
        // Brake (L2) parameters
        uint8_t brakeStart;         // Where brake resistance begins
        uint8_t brakeBase;          // Base brake resistance
        uint8_t brakeMax;           // Maximum brake resistance at high speed
        
        // ABS vibration (when braking hard at speed)
        uint8_t absFrequency;       // ABS pulsing frequency
        uint8_t absThreshold;       // Speed threshold for ABS (normalized 0-255)
    };
    
    // Vehicle class profiles
    // 0: Compacts, 1: Sedans, 2: SUVs, 3: Sports, 4: Super,
    // 5: Muscle, 6: Trucks, 7: Vans, 8: Motorcycles, 9: Boats, 10: Helicopters
    
    constexpr VehicleTriggerProfile VEHICLE_PROFILES[] = {
        // Compacts - Light, easy to drive
        { 0, 30, 70, 0, 40, 120, 25, 100 },
        
        // Sedans - Moderate resistance
        { 0, 40, 90, 0, 50, 140, 28, 110 },
        
        // SUVs - Heavier resistance, higher brake
        { 0, 50, 100, 0, 60, 160, 22, 90 },
        
        // Sports - Light throttle, strong brakes
        { 0, 35, 80, 0, 70, 200, 35, 130 },
        
        // Super - Very responsive, extreme brakes
        { 0, 30, 75, 0, 80, 220, 40, 150 },
        
        // Muscle - Heavy throttle, moderate brakes (old car feel)
        { 10, 60, 120, 5, 55, 150, 20, 100 },
        
        // Trucks - Very heavy resistance
        { 15, 80, 150, 10, 70, 180, 18, 80 },
        
        // Vans - Heavy, slow response
        { 10, 70, 130, 5, 65, 170, 20, 85 },
        
        // Motorcycles - Light but direct
        { 0, 25, 60, 0, 35, 100, 30, 120 },
        
        // Boats - Variable with water resistance
        { 0, 45, 110, 0, 20, 60, 0, 0 },
        
        // Helicopters - Collective feel
        { 0, 35, 80, 0, 35, 80, 0, 0 },
    };
    
    // ========================================================================
    // PUBLIC API
    // ========================================================================
    
    enum class Trigger
    {
        Left,   // L2 - typically brake/ADS
        Right   // R2 - typically throttle/fire
    };
    
    /**
     * Check if DualSense controller is connected
     */
    bool IsConnected();
    
    /**
     * Check if connected via Bluetooth (affects packet format)
     */
    bool IsBluetoothMode();
    
    /**
     * Set adaptive trigger effect
     * @param trigger Which trigger (Left or Right)
     * @param effect Effect configuration
     */
    void SetTriggerEffect(Trigger trigger, const TriggerEffect& effect);
    
    /**
     * Clear trigger effect (restore normal operation)
     */
    void ClearTriggerEffect(Trigger trigger);
    
    /**
     * Apply weapon trigger profile
     * @param weaponSlot GTA IV weapon slot (0-8)
     */
    void ApplyWeaponProfile(uint8_t weaponSlot);
    
    /**
     * Apply weapon recoil effect (call when firing)
     * @param weaponSlot Weapon slot for recoil intensity
     */
    void ApplyWeaponRecoil(uint8_t weaponSlot);
    
    /**
     * Apply vehicle trigger profiles
     * @param vehicleClass Vehicle class (0-10)
     * @param speed Current speed (0-255 normalized)
     * @param throttle Current throttle input (0-255)
     * @param brake Current brake input (0-255)
     */
    void ApplyVehicleProfile(uint8_t vehicleClass, uint8_t speed, 
                             uint8_t throttle, uint8_t brake);
    
    /**
     * Set HD haptic feedback (enhanced rumble)
     * @param leftMotor Low-frequency motor intensity (0-255)
     * @param rightMotor High-frequency motor intensity (0-255)
     */
    void SetHapticFeedback(uint8_t leftMotor, uint8_t rightMotor);
    
    /**
     * Initialize DualSense subsystem (call once at startup)
     */
    void Initialize();
    
    /**
     * Update DualSense state (call each frame)
     * Sends pending effect updates to controller
     */
    void Update();
    
    /**
     * Shutdown DualSense subsystem (call at exit)
     */
    void Shutdown();
}
