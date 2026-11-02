// The "compiling shaders" popup off Windows: none (it is a GDI window). The precompile still runs behind the ubershader, and its progress goes
// to the log.
#include "compile_screen.h"
#include <cstdio>
#include <string>

namespace uc {
CompileScreen::CompileScreen(std::string_view) {}
CompileScreen::~CompileScreen() = default;
void CompileScreen::show() {}
void CompileScreen::wait_for_peer(std::string_view message) { std::fprintf(stderr, "[network] %.*s\n", int(message.size()), message.data()); }
void CompileScreen::update(size_t done, size_t total) {
    static size_t logged = size_t(-1);   // update() is called while waiting too: one line per 64 finished pipelines
    if (total && done != logged && (done == total || done / 64 != logged / 64 || logged == size_t(-1))) {
        std::fprintf(stderr, "[pipeline] precompiled %zu/%zu\n", done, total);
        logged = done;
    }
}
}  // namespace uc
