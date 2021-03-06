// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_DRIVERS_BLUETOOTH_LIB_GAP_IDENTITY_RESOLVING_LIST_H_
#define GARNET_DRIVERS_BLUETOOTH_LIB_GAP_IDENTITY_RESOLVING_LIST_H_

#include <optional>
#include <unordered_map>

#include <fbl/macros.h>

#include "garnet/drivers/bluetooth/lib/common/device_address.h"
#include "garnet/drivers/bluetooth/lib/common/uint128.h"

namespace btlib {
namespace gap {

// This class provides functions to obtain an identity address by resolving a
// given RPA. Resolution is performed using identity information stored in the
// registry.
//
// TODO(NET-1165): Manage the controller-based list here.
class IdentityResolvingList final {
 public:
  IdentityResolvingList() = default;
  ~IdentityResolvingList() = default;

  // Associate the given |irk| with |identity|. If |identity| is already in the
  // list, the existing entry is updated with the new IRK.
  void Add(const common::DeviceAddress& identity, const common::UInt128& irk);

  // Tries to resolve the given RPA against the identities in the registry.
  // Returns std::nullopt if the address is not a RPA or cannot be resolved.
  // Otherwise, returns a value containing the identity address.
  std::optional<common::DeviceAddress> Resolve(
      const common::DeviceAddress& rpa) const;

 private:
  // Maps identity addresses to IRKs.
  std::unordered_map<common::DeviceAddress, common::UInt128> registry_;

  DISALLOW_COPY_ASSIGN_AND_MOVE(IdentityResolvingList);
};

}  // namespace gap
}  // namespace btlib

#endif  // GARNET_DRIVERS_BLUETOOTH_LIB_GAP_IDENTITY_RESOLVING_LIST_H_
