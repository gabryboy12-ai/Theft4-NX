/**
 ******************************************************************************
 * @file        community_multiplayer.h
 * @brief       Community-operated multiplayer directory and relay backend.
 *
 * This compatibility backend never contacts Microsoft or Rockstar services.
 ******************************************************************************
 */

#pragma once

#include <rex/system/xam/live_compatibility.h>

namespace LibertyRecomp::Network {

rex::system::xam::LiveBackendServices CreateCommunityMultiplayerBackend(
    const rex::system::xam::LiveConfig& config,
    const rex::system::xam::LiveIdentity& identity);

}  // namespace LibertyRecomp::Network
