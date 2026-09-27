/**
 ******************************************************************************
 * @file        community_retry_policy.h
 * @brief       Idempotency-key policy for explicit versus ambiguous retries.
 ******************************************************************************
 */

#pragma once

namespace LibertyRecomp::Network {

// A 412 is an explicit rejection of the submitted revision tuple. Retrying a
// refreshed tuple under the old key is not an idempotent replay. Transport
// failures and server errors remain ambiguous and retain the original key.
constexpr bool RequiresFreshIdempotencyKey(long http_status) {
  return http_status == 412;
}

}  // namespace LibertyRecomp::Network
