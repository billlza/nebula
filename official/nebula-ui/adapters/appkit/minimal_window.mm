#import <Cocoa/Cocoa.h>

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

bool has_arg(int argc, char** argv, const std::string& name) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == name) return true;
  }
  return false;
}

nebula::ui::adapter_preview::TreeSummary summary_from_args(int argc, char** argv) {
  auto tree_path = arg_value(argc, argv, "--tree-json");
  if (!tree_path.empty()) {
    auto loaded = nebula::ui::adapter_preview::load_tree_json_file(tree_path);
    if (!loaded.ok) {
      std::cerr << "nebula-ui-appkit-adapter-error: " << loaded.error << "\n";
      std::exit(2);
    }
    return loaded.summary;
  }

  nebula::ui::adapter_preview::TreeSummary summary;
  auto title = arg_value(argc, argv, "--title");
  if (!title.empty()) summary.title = title;
  return summary;
}

void attach_preview_content(NSWindow* window, const nebula::ui::adapter_preview::TreeSummary& summary) {
  NSStackView* stack = [NSStackView stackViewWithViews:@[]];
  [stack setOrientation:NSUserInterfaceLayoutOrientationVertical];
  [stack setSpacing:12.0];
  [stack setTranslatesAutoresizingMaskIntoConstraints:NO];

  for (const auto& label : summary.text_labels) {
    NSTextField* text = [NSTextField labelWithString:[NSString stringWithUTF8String:label.c_str()]];
    [stack addArrangedSubview:text];
  }
  for (const auto& button : summary.buttons) {
    std::string label = button.text.empty() ? button.action : button.text;
    NSButton* native_button =
        [NSButton buttonWithTitle:[NSString stringWithUTF8String:label.c_str()]
                           target:nil
                           action:nil];
    [native_button setEnabled:NO];
    [stack addArrangedSubview:native_button];
  }

  NSView* content = [window contentView];
  [content addSubview:stack];
  [NSLayoutConstraint activateConstraints:@[
    [[stack leadingAnchor] constraintEqualToAnchor:[content leadingAnchor] constant:24.0],
    [[stack trailingAnchor] constraintLessThanOrEqualToAnchor:[content trailingAnchor] constant:-24.0],
    [[stack topAnchor] constraintEqualToAnchor:[content topAnchor] constant:24.0]
  ]];
}

}  // namespace

int main(int argc, char** argv) {
  auto summary = summary_from_args(argc, argv);
  auto dispatch_id = arg_value(argc, argv, "--smoke-dispatch-action");
  auto dispatch_result = nebula::ui::adapter_preview::ActionDispatchResult{};
  if (!dispatch_id.empty()) {
    dispatch_result = nebula::ui::adapter_preview::dispatch_action(summary, dispatch_id);
    if (!dispatch_result.ok) {
      std::cerr << "nebula-ui-appkit-adapter-error: " << dispatch_result.message << "\n";
      return 2;
    }
  }
  if (has_arg(argc, argv, "--smoke")) {
    std::cout << "nebula-ui-appkit-adapter-ok\n";
    nebula::ui::adapter_preview::append_smoke_summary(std::cout, summary);
    if (!dispatch_id.empty()) {
      nebula::ui::adapter_preview::append_dispatch_result(std::cout, dispatch_result);
    }
    return 0;
  }

  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect frame = NSMakeRect(0, 0, 480, 240);
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];

    NSString* title = [NSString stringWithUTF8String:summary.title.c_str()];
    [window setTitle:title];
    attach_preview_content(window, summary);
    if (!dispatch_id.empty()) {
      nebula::ui::adapter_preview::append_dispatch_result(std::cout, dispatch_result);
    }
    [window center];
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
  }

  return 0;
}
