#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ui_tree_preview.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string arg_value(int argc, char** argv, const std::string& name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string(argv[i]) == name) return argv[i + 1];
  }
  return "";
}

int preview_command_count(const nebula::ui::adapter_preview::TreeSummary& summary) {
  int count = summary.has_window ? 1 : 0;
  count += static_cast<int>(summary.text_labels.size());
  count += static_cast<int>(summary.buttons.size());
  count += static_cast<int>(summary.inputs.size());
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  const auto tree_path = arg_value(argc, argv, "--tree-json");
  if (tree_path.empty()) {
    std::cerr << "nebula-ui-metal-submit-error: missing --tree-json\n";
    return 64;
  }
  auto loaded = nebula::ui::adapter_preview::load_tree_json_file(tree_path);
  if (!loaded.ok) {
    std::cerr << "nebula-ui-metal-submit-error: " << loaded.error << "\n";
    return 2;
  }

  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::cout << "nebula-ui-metal-submit-skip-no-device\n";
      return 0;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
      std::cerr << "nebula-ui-metal-submit-error: failed to create command queue\n";
      return 2;
    }
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (command_buffer == nil) {
      std::cerr << "nebula-ui-metal-submit-error: failed to create command buffer\n";
      return 2;
    }
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
  }

  std::cout << "nebula-ui-metal-submit-smoke-ok\n";
  std::cout << "nebula-ui-metal-render-list-schema=nebula-ui.render-list.v1\n";
  std::cout << "nebula-ui-metal-command-count=" << preview_command_count(loaded.summary) << "\n";
  nebula::ui::adapter_preview::append_smoke_summary(std::cout, loaded.summary);
  return 0;
}
