// The test harness transport off Windows (test_pipe.cpp uses named pipes): not available yet, so --test-server is refused with a clear error.
#include "test_pipe.h"
#include <stdexcept>

namespace uc {
TestPipes::TestPipes(const std::string&) { throw std::runtime_error("the test server is not available on this platform yet"); }
TestPipes::~TestPipes() = default;
std::vector<std::string> TestPipes::take_lines() { return {}; }
void TestPipes::send(std::string, bool) {}
bool TestPipes::connected() const { return false; }
bool TestPipes::disconnected() const { return true; }
uint64_t TestPipes::dropped() const { return 0; }
void TestPipes::read_loop() {}
void TestPipes::write_loop() {}
}  // namespace uc
