#pragma once

#include <notcurses/notcurses.h>

#include <string>
#include <vector>

class TerminalUI {
public:
  explicit TerminalUI(bool debug_mode);
  ~TerminalUI();

  TerminalUI(const TerminalUI&) = delete;
  TerminalUI& operator=(const TerminalUI&) = delete;

  bool init();
  void run();
  void stop();

private:
  void render();
  bool ensure_layout();
  void rebuild_layout(unsigned rows, unsigned cols);
  void destroy_layout();
  ncplane* make_plane(int y, int x, int h, int w, const char* name);
  void draw_panel(ncplane* plane, const std::string& title, uint64_t border_channels,
                  uint64_t text_channels, uint32_t bg_ul, uint32_t bg_ur,
                  uint32_t bg_ll, uint32_t bg_lr);
  void draw_contacts();
  void draw_chat();
  void draw_debug();
  void add_debug(const std::string& line);

  bool debug_mode_{false};
  bool running_{false};
  notcurses* nc_{nullptr};
  ncplane* people_plane_{nullptr};
  ncplane* chat_plane_{nullptr};
  ncplane* debug_plane_{nullptr};
  unsigned last_rows_{0};
  unsigned last_cols_{0};
  std::vector<std::string> debug_lines_;
};
