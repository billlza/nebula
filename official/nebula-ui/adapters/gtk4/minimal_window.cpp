#include <gtk/gtk.h>

#include "ui_tree_preview.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

std::string arg_value(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return "";
}

bool has_arg(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return true;
  }
  return false;
}

nebula::ui::adapter_preview::TreeSummary summary_from_args(int argc, char** argv) {
  auto tree_path = arg_value(argc, argv, "--tree-json");
  if (!tree_path.empty()) {
    auto loaded = nebula::ui::adapter_preview::load_tree_json_file(tree_path);
    if (!loaded.ok) {
      std::cerr << "nebula-ui-gtk4-adapter-error: " << loaded.error << "\n";
      std::exit(2);
    }
    return loaded.summary;
  }

  nebula::ui::adapter_preview::TreeSummary summary;
  auto title = arg_value(argc, argv, "--title");
  if (!title.empty()) summary.title = title;
  return summary;
}

void activate(GtkApplication* app, gpointer user_data) {
  const auto* summary = static_cast<const nebula::ui::adapter_preview::TreeSummary*>(user_data);
  GtkWidget* window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), summary->title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window), 480, 240);

  GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  for (const auto& label : summary->text_labels) {
    gtk_box_append(GTK_BOX(box), gtk_label_new(label.c_str()));
  }
  for (const auto& button : summary->buttons) {
    const std::string text = button.text.empty() ? button.action : button.text;
    GtkWidget* native_button = gtk_button_new_with_label(text.c_str());
    gtk_widget_set_sensitive(native_button, false);
    gtk_box_append(GTK_BOX(box), native_button);
  }
  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
}

}  // namespace

int main(int argc, char** argv) {
  auto summary = summary_from_args(argc, argv);
  auto dispatch_id = arg_value(argc, argv, "--smoke-dispatch-action");
  auto dispatch_result = nebula::ui::adapter_preview::ActionDispatchResult{};
  if (!dispatch_id.empty()) {
    dispatch_result = nebula::ui::adapter_preview::dispatch_action(summary, dispatch_id);
    if (!dispatch_result.ok) {
      std::cerr << "nebula-ui-gtk4-adapter-error: " << dispatch_result.message << "\n";
      return 2;
    }
  }
  if (has_arg(argc, argv, "--smoke")) {
    std::cout << "nebula-ui-gtk4-adapter-ok\n";
    nebula::ui::adapter_preview::append_smoke_summary(std::cout, summary);
    if (!dispatch_id.empty()) {
      nebula::ui::adapter_preview::append_dispatch_result(std::cout, dispatch_result);
    }
    return 0;
  }

  GtkApplication* app = gtk_application_new("dev.nebula.ui.preview", G_APPLICATION_DEFAULT_FLAGS);
  if (!dispatch_id.empty()) {
    nebula::ui::adapter_preview::append_dispatch_result(std::cout, dispatch_result);
  }
  g_signal_connect(app, "activate", G_CALLBACK(activate), &summary);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
