// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_MDNS_SERVICE_MDNS_TRANSCEIVER_H_
#define GARNET_BIN_MDNS_SERVICE_MDNS_TRANSCEIVER_H_

#include <fuchsia/netstack/cpp/fidl.h>
#include <lib/fit/function.h>
#include <netinet/in.h>
#include <memory>
#include <unordered_map>
#include <vector>
#include "garnet/bin/mdns/service/mdns_interface_transceiver.h"
#include "lib/fxl/macros.h"

namespace mdns {

// Sends and receives mDNS messages on any number of interfaces.
class MdnsTransceiver {
 public:
  using InboundMessageCallback =
      fit::function<void(std::unique_ptr<DnsMessage>, const ReplyAddress&)>;

  MdnsTransceiver();

  ~MdnsTransceiver();

  // Starts the transceiver.
  void Start(fuchsia::netstack::NetstackPtr netstack,
             fit::closure link_change_callback,
             InboundMessageCallback inbound_message_callback);

  // Stops the transceiver.
  void Stop();

  // Determines if this transceiver has interfaces.
  bool has_interfaces() { return !interface_transceivers_by_address_.empty(); }

  // Sends a messaage to the specified address. A V6 interface will send to
  // |MdnsAddresses::kV6Multicast| if |reply_address.socket_address()| is
  // |MdnsAddresses::kV4Multicast|.
  void SendMessage(DnsMessage* message, const ReplyAddress& reply_address);

  // Writes log messages describing lifetime traffic.
  void LogTraffic();

 private:
  // Returns the interface transceiver with address |address| if it exists,
  // nullptr if not.
  MdnsInterfaceTransceiver* GetInterfaceTransceiver(
      const inet::IpAddress& address);

  // Handles |OnInterfaceChanged| events from |Netstack|.
  void InterfacesChanged(
      std::vector<fuchsia::netstack::NetInterface> interfaces);

  // Ensures that an interface transciever exists for |address| if |address|
  // is valid. Returns true if a change was made, false otherwise.
  bool EnsureInterfaceTransceiver(
      const inet::IpAddress& address, const inet::IpAddress& alternate_address,
      uint32_t id, const std::string& name,
      std::unordered_map<inet::IpAddress,
                         std::unique_ptr<MdnsInterfaceTransceiver>>* prev);

  fuchsia::netstack::NetstackPtr netstack_;
  fit::closure link_change_callback_;
  InboundMessageCallback inbound_message_callback_;
  std::string host_full_name_;
  std::unordered_map<inet::IpAddress, std::unique_ptr<MdnsInterfaceTransceiver>>
      interface_transceivers_by_address_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MdnsTransceiver);
};

}  // namespace mdns

#endif  // GARNET_BIN_MDNS_SERVICE_MDNS_TRANSCEIVER_H_