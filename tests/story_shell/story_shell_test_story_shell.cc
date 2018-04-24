// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation of the StoryShell service that just lays out the
// views of all modules side by side.

#include <memory>

#include <fuchsia/cpp/modular.h>
#include <fuchsia/cpp/views_v1_token.h>
#include "lib/app/cpp/application_context.h"
#include "lib/app_driver/cpp/app_driver.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/macros.h"
#include "peridot/lib/testing/component_base.h"
#include "peridot/lib/testing/reporting.h"
#include "peridot/lib/testing/testing.h"

namespace {

void Put(const fidl::StringPtr& message) {
  // We use the message as both the key and the value. The value is used by the
  // receiver to display what key it was waiting on. That way the same receiver
  // function can be used to wait for multiple keys.
  modular::testing::GetStore()->Put(message, message, []{});
}

class TestApp : public modular::testing::ComponentBase<modular::StoryShell> {
 public:
  TestApp(component::ApplicationContext* const application_context)
      : ComponentBase(application_context) {
    TestInit(__FILE__);
  }

  ~TestApp() override = default;

 private:
  using TestPoint = modular::testing::TestPoint;

  // |StoryShell|
  void Initialize(
      fidl::InterfaceHandle<modular::StoryContext> story_context) override {
    story_context_.Bind(std::move(story_context));
  }

  // Keep state to check ordering. Cf. below.
  bool seen_root_one_{};

  // |StoryShell|
  void ConnectView(fidl::InterfaceHandle<views_v1_token::ViewOwner> view_owner,
                   fidl::StringPtr view_id,
                   fidl::StringPtr anchor_id,
                   modular::SurfaceRelationPtr /*surface_relation*/,
                   modular::ModuleManifestPtr module_manifest) override {
    FXL_LOG(INFO) << "ConnectView " << view_id << " " << anchor_id
                  << " "
                  << (module_manifest ? module_manifest->composition_pattern : " NO MANIFEST");

    if (view_id == "root:one" && anchor_id == "root") {
      Put("root:one");

      if (module_manifest && module_manifest->composition_pattern == "ticker" &&
          module_manifest->action == "com.google.fuchsia.common.null") {
        Put("root:one manifest");
      }

      seen_root_one_ = true;
    }

    if (view_id == "root:one:two" && anchor_id == "root:one") {
      Put("root:one:two");

      if (module_manifest && module_manifest->composition_pattern == "ticker" &&
          module_manifest->action == "com.google.fuchsia.common.null") {
        Put("root:one:two manifest");
      }

      if (seen_root_one_) {
        Put("root:one:two ordering");
      }
    }
  }

  // |StoryShell|
  void FocusView(fidl::StringPtr /*view_id*/,
                 fidl::StringPtr /*relative_view_id*/) override {}

  // |StoryShell|
  void DefocusView(fidl::StringPtr /*view_id*/,
                   DefocusViewCallback callback) override {
    callback();
  }

  // |StoryShell|
  void AddContainer(
      fidl::StringPtr /*container_name*/,
      fidl::StringPtr /*parent_id*/,
      modular::SurfaceRelation /*relation*/,
      fidl::VectorPtr<modular::ContainerLayout> /*layout*/,
      fidl::VectorPtr<modular::ContainerRelationEntry> /* relationships */,
      fidl::VectorPtr<modular::ContainerView> /* views */) override {}

  modular::StoryContextPtr story_context_;
  FXL_DISALLOW_COPY_AND_ASSIGN(TestApp);
};

}  // namespace

int main(int /* argc */, const char** /* argv */) {
  FXL_LOG(INFO) << "Story Shell main";
  modular::testing::ComponentMain<TestApp>();
  return 0;
}
