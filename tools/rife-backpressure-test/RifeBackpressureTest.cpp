#include "../../Source/RifeBackpressure.h"

#include <cstdlib>
#include <iostream>

namespace {

void Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace RifeBackpressure;

    Check(!ShouldCollapse(0), "empty queue stays intact");
    Check(!ShouldCollapse(1), "one queued source frame stays intact");
    Check(ShouldCollapse(2), "two queued source frames trigger latency resync");
    Check(ShouldCollapse(11), "large backlog triggers latency resync");

    std::cout << "RIFE backpressure tests passed\n";
    return 0;
}
