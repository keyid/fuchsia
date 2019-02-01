// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/component/cpp/termination_reason.h>
#include <lib/fxl/command_line.h>
#include <lib/fxl/log_settings_command_line.h>
#include <lib/fxl/logging.h>
#include <iostream>
#include "sandbox.h"

using namespace netemul;

void PrintUsage() {
  fprintf(stderr, R"(
Usage: netemul_sandbox <package_url> [arguments...]

       *package_url* takes the form of component manifest URL which uniquely
       identifies a component. Example:
          fuchsia-pkg://fuchsia.com/component_hello_world#meta/hello.cmx
)");
}

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  async::Loop loop(&kAsyncLoopConfigNoAttachToThread);
  async_set_default_dispatcher(loop.dispatcher());

  SandboxArgs args;

  auto posargs = command_line.positional_args().begin();
  if (posargs == command_line.positional_args().end()) {
    PrintUsage();
    return 1;
  }
  args.package = *posargs++;
  args.args.insert(args.args.end(), posargs,
                   command_line.positional_args().end());

  FXL_LOG(INFO) << "Starting netemul sandbox for " << args.package;

  Sandbox sandbox(std::move(args));
  sandbox.SetTerminationCallback([](int64_t exit_code,
                                    Sandbox::TerminationReason reason) {
    FXL_LOG(INFO) << "Sandbox terminated with (" << exit_code << ") reason: "
                  << component::HumanReadableTerminationReason(reason);
    if (reason != Sandbox::TerminationReason::EXITED) {
      exit_code = 1;
    }
    zx_process_exit(exit_code);
  });

  sandbox.Start(loop.dispatcher());
  loop.Run();

  return 0;
}