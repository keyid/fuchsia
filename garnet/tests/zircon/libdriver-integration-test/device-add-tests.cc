// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>

#include "integration-test.h"

namespace libdriver_integration_test {

class DeviceAddTest : public IntegrationTest {
protected:
    Promise<void> CreateDevice(
            std::initializer_list<zx_device_prop_t> props, zx_status_t expected_status,
            std::unique_ptr<RootMockDevice>* root_device,
            std::unique_ptr<MockDevice>* child_device) {
        return ExpectBind(root_device,
            [=](HookInvocation record, Completer<void> completer) {
                ActionList actions;
                actions.AppendAddMockDevice(loop_.dispatcher(), (*root_device)->path(),
                                            "first_child",
                                            std::vector<zx_device_prop_t>(props),
                                            expected_status, std::move(completer), child_device);
                actions.AppendReturnStatus(expected_status);
                return actions;
            });
    }
};

// This test checks what happens when only one topological properties is
// specified
TEST_F(DeviceAddTest, OneTopologicalProperty) {
    std::unique_ptr<RootMockDevice> root_device;
    std::unique_ptr<MockDevice> child_device;

    auto promise = CreateDevice({
        (zx_device_prop_t){ BIND_TOPO_PCI, 0, BIND_TOPO_PCI_PACK(0, 0, 0) },
        (zx_device_prop_t){ BIND_PCI_VID, 0, 1234 },
    }, ZX_OK, &root_device, &child_device
    ).and_then([&]() -> Promise<void> {
        // Destroy the test device.  This should cause an unbind of the child
        // device.
        root_device.reset();
        return ExpectUnbindThenRelease(child_device);
    });

    RunPromise(std::move(promise));
}

// This test checks what happens when two different topological properties are
// specified
TEST_F(DeviceAddTest, TooManyTopologicalProperties) {
    std::unique_ptr<RootMockDevice> root_device;
    std::unique_ptr<MockDevice> child_device;

    auto promise = CreateDevice({
        (zx_device_prop_t){ BIND_TOPO_PCI, 0, BIND_TOPO_PCI_PACK(0, 0, 0) },
        (zx_device_prop_t){ BIND_TOPO_I2C, 0, BIND_TOPO_I2C_PACK(1) },
        (zx_device_prop_t){ BIND_PCI_VID, 0, 1234 },
    }, ZX_ERR_INVALID_ARGS, &root_device, &child_device);
    RunPromise(std::move(promise));
}

// This test checks what happens when the same topological property is
// specified twice
TEST_F(DeviceAddTest, TooManyTopologicalPropertiesDuplicated) {
    std::unique_ptr<RootMockDevice> root_device;
    std::unique_ptr<MockDevice> child_device;

    auto promise = CreateDevice({
        (zx_device_prop_t){ BIND_TOPO_PCI, 0, BIND_TOPO_PCI_PACK(0, 0, 0) },
        (zx_device_prop_t){ BIND_TOPO_PCI, 0, BIND_TOPO_PCI_PACK(0, 0, 1) },
        (zx_device_prop_t){ BIND_PCI_VID, 0, 1234 },
    }, ZX_ERR_INVALID_ARGS, &root_device, &child_device);
    RunPromise(std::move(promise));
}

} // namespace libdriver_integration_test
