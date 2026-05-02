#include "ui/TerminalUI.h"

#include <algorithm>
#include <sstream>

namespace {
uint64_t make_channels(unsigned fg_r, unsigned fg_g, unsigned fg_b, unsigned bg_r,
                       unsigned bg_g, unsigned bg_b) {
  uint64_t ch = 0;
  ncchannels_set_fg_rgb8(&ch, fg_r, fg_g, fg_b);
  ncchannels_set_bg_rgb8(&ch, bg_r, bg_g, bg_b);
  return ch;
}
}  // namespace

TerminalUI::TerminalUI(bool debug_mode) : debug_mode_(debug_mode) {}

TerminalUI::~TerminalUI() { stop(); }

bool TerminalUI::init() {
  notcurses_options opts{};
  opts.flags = NCOPTION_NO_WINCH_SIGHANDLER;
  nc_ = notcurses_init(&opts, nullptr);
  if (!nc_) {
    return false;
  }

  if (debug_mode_) {
    add_debug("debug mode enabled");
  }
  return true;
}

void TerminalUI::run() {
  if (!nc_ || running_) {
    return;
  }
  running_ = true;
  add_debug("press q to quit");
  render();

  ncinput in{};
  while (running_) {
    const uint32_t input = notcurses_get_blocking(nc_, &in);
    if (input == static_cast<uint32_t>('q') || input == static_cast<uint32_t>('Q')) {
      running_ = false;
      break;
    }

    if (debug_mode_) {
      if (input == NCKEY_RESIZE) {
        add_debug("resize event");
      } else {
        std::ostringstream oss;
        oss << "key input: " << static_cast<unsigned int>(input);
        add_debug(oss.str());
      }
    }
    render();
  }
}

void TerminalUI::stop() {
  destroy_layout();
  if (nc_) {
    notcurses_stop(nc_);
    nc_ = nullptr;
  }
  running_ = false;
}

void TerminalUI::render() {
  ncplane* stdp = notcurses_stdplane(nc_);
  if (!stdp) {
    return;
  }

  if (!ensure_layout()) {
    ncplane_erase(stdp);
    const uint64_t ch = make_channels(0xff, 0xff, 0xff, 0x14, 0x14, 0x14);
    ncplane_set_base(stdp, " ", 0, ch);
    ncplane_putstr_yx(stdp, 0, 0, "terminal too small");
    notcurses_render(nc_);
    return;
  }

  draw_contacts();
  draw_chat();
  if (debug_mode_ && debug_plane_) {
    draw_debug();
  }

  notcurses_render(nc_);
}

bool TerminalUI::ensure_layout() {
  ncplane* stdp = notcurses_stdplane(nc_);
  if (!stdp) {
    return false;
  }

  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(stdp, &rows, &cols);
  if (rows < 8 || cols < 40) {
    destroy_layout();
    return false;
  }

  if (rows != last_rows_ || cols != last_cols_ || !people_plane_ || !chat_plane_ ||
      (debug_mode_ && !debug_plane_)) {
    rebuild_layout(rows, cols);
  }
  return people_plane_ && chat_plane_ && (!debug_mode_ || debug_plane_);
}

void TerminalUI::rebuild_layout(unsigned rows, unsigned cols) {
  destroy_layout();

  last_rows_ = rows;
  last_cols_ = cols;

  int app_x = 0;
  int app_w = static_cast<int>(cols);
  if (debug_mode_) {
    app_w = static_cast<int>(cols / 2);
    const int debug_x = app_w;
    const int debug_w = static_cast<int>(cols) - app_w;
    if (debug_w >= 20) {
      debug_plane_ = make_plane(0, debug_x, static_cast<int>(rows), debug_w, "debug");
    }
  }

  if (app_w < 24) {
    return;
  }

  int people_w = std::max(12, (app_w * 20) / 100);
  int chat_w = app_w - people_w;
  if (chat_w < 12) {
    chat_w = 12;
    people_w = app_w - chat_w;
  }

  people_plane_ = make_plane(0, app_x, static_cast<int>(rows), people_w, "people");
  chat_plane_ = make_plane(0, app_x + people_w, static_cast<int>(rows), chat_w, "chat");
}

void TerminalUI::destroy_layout() {
  if (people_plane_) {
    ncplane_destroy(people_plane_);
    people_plane_ = nullptr;
  }
  if (chat_plane_) {
    ncplane_destroy(chat_plane_);
    chat_plane_ = nullptr;
  }
  if (debug_plane_) {
    ncplane_destroy(debug_plane_);
    debug_plane_ = nullptr;
  }
  last_rows_ = 0;
  last_cols_ = 0;
}

ncplane* TerminalUI::make_plane(int y, int x, int h, int w, const char* name) {
  if (h < 2 || w < 2) {
    return nullptr;
  }
  ncplane_options opts{};
  opts.y = y;
  opts.x = x;
  opts.rows = h;
  opts.cols = w;
  opts.name = name;
  return ncplane_create(notcurses_stdplane(nc_), &opts);
}

void TerminalUI::draw_panel(ncplane* plane, const std::string& title, uint64_t border_channels,
                            uint64_t text_channels, uint32_t bg_ul, uint32_t bg_ur,
                            uint32_t bg_ll, uint32_t bg_lr) {
  if (!plane) {
    return;
  }

  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(plane, &rows, &cols);
  if (rows < 2 || cols < 2) {
    return;
  }

  ncplane_erase(plane);
  uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
  ncchannels_set_bg_rgb(&ul, bg_ul);
  ncchannels_set_bg_rgb(&ur, bg_ur);
  ncchannels_set_bg_rgb(&ll, bg_ll);
  ncchannels_set_bg_rgb(&lr, bg_lr);
  ncchannels_set_fg_rgb(&ul, bg_ul);
  ncchannels_set_fg_rgb(&ur, bg_ur);
  ncchannels_set_fg_rgb(&ll, bg_ll);
  ncchannels_set_fg_rgb(&lr, bg_lr);
  ncplane_gradient(plane, 0, 0, rows, cols, " ", 0, ul, ur, ll, lr);

  ncplane_rounded_box_sized(plane, 0, border_channels, static_cast<int>(rows),
                            static_cast<int>(cols), 0);

  if (!title.empty() && cols > 4) {
    ncplane_set_channels(plane, text_channels);
    ncplane_on_styles(plane, NCSTYLE_BOLD);
    const std::string capped = title.substr(0, static_cast<size_t>(cols - 4));
    ncplane_putstr_yx(plane, 0, 2, capped.c_str());
    ncplane_off_styles(plane, NCSTYLE_BOLD);
  }
}

void TerminalUI::draw_contacts() {
  const uint64_t border_ch = make_channels(0x86, 0xef, 0xac, 0x0b, 0x1c, 0x16);
  const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0b, 0x1c, 0x16);
  draw_panel(people_plane_, " People ", border_ch, text_ch, 0x0b1c16, 0x0b1c16, 0x10271f,
             0x10271f);

  if (!people_plane_) {
    return;
  }
  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(people_plane_, &rows, &cols);
  if (rows < 4 || cols < 4) {
    return;
  }

  const std::vector<std::string> peers = {"alice", "bob", "charlie", "dana"};
  ncplane_set_channels(people_plane_, text_ch);
  for (unsigned i = 0; i < peers.size(); ++i) {
    const int y = 2 + static_cast<int>(i);
    if (y >= static_cast<int>(rows) - 1) {
      break;
    }
    std::string line = "• " + peers[i];
    line = line.substr(0, cols - 2);
    ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
  }
}

void TerminalUI::draw_chat() {
  const uint64_t border_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
  const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
  draw_panel(chat_plane_, " Chat ", border_ch, text_ch, 0x0f172a, 0x111c33, 0x0f172a, 0x111c33);

  if (!chat_plane_) {
    return;
  }
  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(chat_plane_, &rows, &cols);
  if (rows < 5 || cols < 4) {
    return;
  }

  const std::vector<std::string> lines = {
      "[10:41] alice: Hello", "[10:42] you: UI split is ready",
      "[10:43] alice: next step: message input"};
  ncplane_set_channels(chat_plane_, text_ch);

  int y = 2;
  for (const auto& line : lines) {
    if (y >= static_cast<int>(rows) - 3) {
      break;
    }
    std::string clipped = line.substr(0, cols - 2);
    ncplane_putstr_yx(chat_plane_, y, 1, clipped.c_str());
    ++y;
  }

  ncplane_set_channels(chat_plane_, make_channels(0xc4, 0xb5, 0xfd, 0x0f, 0x17, 0x2a));
  std::string prompt = "> ";
  prompt.resize(std::max(2u, cols - 2), ' ');
  ncplane_putstr_yx(chat_plane_, static_cast<int>(rows) - 2, 1, prompt.c_str());
}

void TerminalUI::draw_debug() {
  const uint64_t border_ch = make_channels(0xf5, 0xd0, 0xfe, 0x22, 0x12, 0x29);
  const uint64_t text_ch = make_channels(0xf8, 0xfa, 0xfc, 0x22, 0x12, 0x29);
  draw_panel(debug_plane_, " Debug ", border_ch, text_ch, 0x221229, 0x2a1633, 0x150b1f,
             0x1a1026);

  if (!debug_plane_) {
    return;
  }

  unsigned rows = 0;
  unsigned cols = 0;
  ncplane_dim_yx(debug_plane_, &rows, &cols);
  if (rows < 4 || cols < 4) {
    return;
  }

  const int visible = static_cast<int>(rows) - 2;
  const int total = static_cast<int>(debug_lines_.size());
  const int start = std::max(0, total - visible);

  ncplane_set_channels(debug_plane_, text_ch);
  int row = 1;
  for (int i = start; i < total && row < static_cast<int>(rows) - 1; ++i, ++row) {
    std::string line = debug_lines_[i];
    line = line.substr(0, cols - 2);
    ncplane_putstr_yx(debug_plane_, row, 1, line.c_str());
  }
}

void TerminalUI::add_debug(const std::string& line) {
  if (!debug_mode_) {
    return;
  }
  debug_lines_.push_back(line);
  if (debug_lines_.size() > 1000) {
    debug_lines_.erase(debug_lines_.begin());
  }
}
