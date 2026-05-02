#include "ui/TerminalUI.h"

#include <cstring>

int main(int argc, char** argv) {
  bool debug_mode = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--debug") == 0) {
      debug_mode = true;
    }
  }

  TerminalUI ui(debug_mode);
  if (!ui.init()) {
    return 1;
  }
  ui.run();
  return 0;
}
