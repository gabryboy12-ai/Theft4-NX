#include <catch2/catch_test_macros.hpp>

#include "gta4_vehicle_weapon_policy.h"

namespace gta4::input {

TEST_CASE("Vehicle next weapon retains retail candidate selection",
          "[input][gta4][weapons]") {
  VehicleWeaponCandidatePolicy policy{.manager = 0x1000};
  CHECK(policy.Route(0x1000, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kRetail);
  CHECK(policy.Route(0x1000, 0x823D5998) ==
        VehicleWeaponCandidateRoute::kRetail);
}

TEST_CASE("Vehicle previous weapon reverses only the eligible vehicle search",
          "[input][gta4][weapons]") {
  VehicleWeaponCandidatePolicy policy{
      .manager = 0x1000, .request = KeyboardWeaponRequest::kPrevious};
  CHECK(policy.Route(0x1000, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kAscending);
  CHECK(policy.Route(0x1000, 0x823D5998) ==
        VehicleWeaponCandidateRoute::kAscending);
  CHECK(policy.Route(0x1000, 0x823D59F8) ==
        VehicleWeaponCandidateRoute::kRetail);
  CHECK(policy.Route(0x2000, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kRetail);
}

TEST_CASE("Rejected direct vehicle weapon leaves the original selection",
          "[input][gta4][weapons]") {
  VehicleWeaponCandidatePolicy policy{
      .manager = 0x1000,
      .original_slot = 2,
      .requested_slot = 8,
      .request = KeyboardWeaponRequest::kDirect};
  // An unrelated manager or on-foot search cannot consume the request.
  CHECK(policy.Route(0x2000, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kRetail);
  CHECK(policy.Route(0x1000, 0x823D59F8) ==
        VehicleWeaponCandidateRoute::kRetail);
  CHECK_FALSE(policy.offered_requested_slot);
  CHECK(policy.Route(0x1000, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kRequestedSlot);
  CHECK(policy.Route(0x1000, 0x823D5998) ==
        VehicleWeaponCandidateRoute::kOriginalSlot);
  CHECK(policy.Route(0x1000, 0x823D5998) ==
        VehicleWeaponCandidateRoute::kOriginalSlot);
}

TEST_CASE("Vehicle previous weapon terminates if its original slot disappeared",
          "[input][gta4][weapons]") {
  VehicleWeaponCandidatePolicy policy{
      .manager = 0x1000,
      .original_slot = 4,
      .request = KeyboardWeaponRequest::kPrevious};
  for (uint32_t slot = 0; slot < 11; ++slot) {
    CHECK(policy.Route(0x1000, 0x823D5998) ==
          VehicleWeaponCandidateRoute::kAscending);
  }
  CHECK(policy.Route(0x1000, 0x823D5998) ==
        VehicleWeaponCandidateRoute::kOriginalSlot);
}

TEST_CASE("Unarmed and inactive vehicle policies leave controller input alone",
          "[input][gta4][weapons]") {
  VehicleWeaponCandidatePolicy policy{
      .request = KeyboardWeaponRequest::kDirect};
  CHECK(policy.Route(0, 0x823D58E0) ==
        VehicleWeaponCandidateRoute::kRetail);
  CHECK_FALSE(policy.offered_requested_slot);
}

}  // namespace gta4::input
