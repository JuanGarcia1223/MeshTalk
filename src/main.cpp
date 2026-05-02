#include <notcurses/notcurses.h>

#include <chrono>
#include <thread>

int main() {
  notcurses_options opts{};
  opts.flags = NCOPTION_NO_WINCH_SIGHANDLER;

  notcurses* nc = notcurses_init(&opts, nullptr);
  if (!nc) {
    return 1;
  }

  ncplane* stdp = notcurses_stdplane(nc);
  if (!stdp) {
    notcurses_stop(nc);
    return 1;
  }

  const char* title = "meshtalk notcurses demo";
  const char* subtitle = "UI bootstrap is working";
  const char* footer = "Exiting in 2 seconds...";

  ncplane_set_fg_rgb8(stdp, 0x10, 0xb9, 0x81);
  ncplane_set_bg_rgb8(stdp, 0x0b, 0x11, 0x14);

  ncplane_putchar_yx(stdp, 0, 0, ' ');
  ncplane_putstr_yx(stdp, 1, 2, title);
  ncplane_set_fg_rgb8(stdp, 0x93, 0xc5, 0xfd);
  ncplane_putstr_yx(stdp, 3, 2, subtitle);
  ncplane_set_fg_rgb8(stdp, 0xf5, 0xd0, 0xfe);
  ncplane_putstr_yx(stdp, 5, 2, footer);

  if (notcurses_render(nc) != 0) {
    notcurses_stop(nc);
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  notcurses_stop(nc);
  return 0;
}
