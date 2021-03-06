// Copyright (c) 2018-2019 The Unit-e developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef UNIT_E_STAKING_NETWORK_H
#define UNIT_E_STAKING_NETWORK_H

#include <stdint.h>

#include <memory>

namespace staking {

//! \brief An interface to information available from the P2P network.
class Network {

 public:
  //! \brief returns the current network time in seconds.
  //!
  //! The time is determined from peers and the systems clock. The time
  //! is a regular unix timestamp in seconds.
  virtual int64_t GetTime() const = 0;

  //! \brief returns the number of other nodes this node is connected to.
  virtual size_t GetNodeCount() const = 0;

  virtual size_t GetInboundNodeCount() const = 0;

  virtual size_t GetOutboundNodeCount() const = 0;

  virtual ~Network() = default;

  static std::unique_ptr<Network> New();
};

}  // namespace staking

#endif  // UNIT_E_STAKING_NETWORK_H
