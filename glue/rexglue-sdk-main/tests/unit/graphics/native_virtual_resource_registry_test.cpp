#include "../../../src/graphics/gta4_native/native_virtual_resource_registry.h"

#include <catch2/catch_test_macros.hpp>

namespace rex::graphics::gta4_native {
namespace {

RegisterVirtualResourceCommand MakeRegistration(uint32_t handle) {
  RegisterVirtualResourceCommand command;
  command.resource = handle;
  command.kind = VirtualResourceKind::kTexture;
  command.wrapper = 0x100;
  command.companion = 0x200;
  command.guest_backing_width = 1;
  command.guest_backing_height = 1;
  command.logical_width = 2560;
  command.logical_height = 2560;
  command.physical_width = 5120;
  command.physical_height = 5120;
  command.scale_domain = VirtualResourceScaleDomain::kPrimaryScene;
  command.constructor_caller = 0x828BEFA8;
  return command;
}

TEST_CASE("virtual resource registrations preserve their explicit scale domain") {
  NativeVirtualResourceRegistry registry;
  RegisterVirtualResourceCommand registration = MakeRegistration(0xD91338A0);
  registration.logical_width = 10;
  registration.logical_height = 10;
  registration.physical_width = 10;
  registration.physical_height = 10;
  registration.scale_domain = VirtualResourceScaleDomain::kLogical;

  REQUIRE(registry.Register(registration) == VirtualResourceRegistrationResult::kCreated);
  const NativeVirtualResourceRecord* resource = registry.Find(registration.resource);
  REQUIRE(resource);
  CHECK(resource->logical_width == 10);
  CHECK(resource->logical_height == 10);
  CHECK(resource->physical_width == 10);
  CHECK(resource->physical_height == 10);
  CHECK(resource->scale_domain == VirtualResourceScaleDomain::kLogical);
}

TEST_CASE("virtual resource registrations have explicit lifetimes") {
  NativeVirtualResourceRegistry registry;
  RegisterVirtualResourceCommand registration = MakeRegistration(0xD91338A0);

  REQUIRE(registry.Register(registration) == VirtualResourceRegistrationResult::kCreated);
  const uint64_t first_lifetime = registry.Find(registration.resource)->lifetime;
  REQUIRE(registry.Register(registration) == VirtualResourceRegistrationResult::kUnchanged);
  REQUIRE(registry.Find(registration.resource)->lifetime == first_lifetime);

  registration.logical_width = 1280;
  REQUIRE(registry.Register(registration) == VirtualResourceRegistrationResult::kReplaced);
  REQUIRE(registry.Find(registration.resource)->lifetime > first_lifetime);
}

TEST_CASE("guest writes conflict until a host write republishes the virtual resource") {
  NativeVirtualResourceRegistry registry;
  const RegisterVirtualResourceCommand registration = MakeRegistration(0xD91338A0);
  registry.Register(registration);

  REQUIRE(registry.MarkGuestWrite(registration.resource));
  REQUIRE(registry.Find(registration.resource)->guest_write_conflict);
  REQUIRE(registry.PublishHostWrite(registration.resource));
  REQUIRE_FALSE(registry.Find(registration.resource)->guest_write_conflict);
}

TEST_CASE("packed depth aliases retain explicit source identity across registration") {
  NativeVirtualResourceRegistry registry;
  auto registration = MakeRegistration(0xD91323A0);
  registration.packed_depth_source = 0xD9132320;
  REQUIRE(registry.Register(registration) == VirtualResourceRegistrationResult::kCreated);
  const auto first = *registry.Find(registration.resource);
  CHECK(first.packed_depth_source == registration.packed_depth_source);
  CHECK(registry.Register(registration) == VirtualResourceRegistrationResult::kUnchanged);
  registration.packed_depth_source = 0xD9132400;
  CHECK(registry.Register(registration) == VirtualResourceRegistrationResult::kReplaced);
  CHECK(registry.Find(registration.resource)->lifetime != first.lifetime);
  CHECK(registry.Find(registration.resource)->packed_depth_source == registration.packed_depth_source);
}

TEST_CASE("unlock policy transfers virtual target ownership only on a guest write") {
  REQUIRE(ClassifyResourceUnlock(true, ResourceUnlockAccess::kNoDirtyUpdate) ==
          ResourceUnlockDecision::kIgnoreNoDirtyUpdate);
  REQUIRE(ClassifyResourceUnlock(true, ResourceUnlockAccess::kGuestWrite) ==
          ResourceUnlockDecision::kInvalidateVirtualHostOwnership);
  REQUIRE(ClassifyResourceUnlock(false, ResourceUnlockAccess::kGuestWrite) ==
          ResourceUnlockDecision::kDirtyGuestResource);
}

TEST_CASE("virtual texture capture preserves host-owned render target generations") {
  REQUIRE(ClassifyVirtualTextureCapture(true, true, false, false) ==
          VirtualTextureCaptureAction::kUseHostGeneration);
  REQUIRE(ClassifyVirtualTextureCapture(true, true, false, true) ==
          VirtualTextureCaptureAction::kUseHostGeneration);
}

TEST_CASE("virtual texture capture creates defined content before the first resolve") {
  REQUIRE(ClassifyVirtualTextureCapture(true, false, false, false) ==
          VirtualTextureCaptureAction::kCreateDeterministicHostGeneration);
  REQUIRE(ClassifyVirtualTextureCapture(true, false, false, true) ==
          VirtualTextureCaptureAction::kCreateDeterministicHostGeneration);
}

TEST_CASE("virtual texture capture admits only complete guest-owned backing") {
  REQUIRE(ClassifyVirtualTextureCapture(true, false, true, true) ==
          VirtualTextureCaptureAction::kCaptureCompleteGuestBacking);
  REQUIRE(ClassifyVirtualTextureCapture(true, false, true, false) ==
          VirtualTextureCaptureAction::kRejectIncompleteGuestBacking);
}

TEST_CASE("virtual texture capture rejects descriptor mismatches before ownership") {
  REQUIRE(ClassifyVirtualTextureCapture(false, true, false, true) ==
          VirtualTextureCaptureAction::kRejectDescriptorMismatch);
  REQUIRE(ClassifyVirtualTextureCapture(false, false, true, true) ==
          VirtualTextureCaptureAction::kRejectDescriptorMismatch);
}

}  // namespace
}  // namespace rex::graphics::gta4_native
