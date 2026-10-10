#include "../../Source/RifePresentationPreparation.h"
#include <cassert>
#include <iostream>
#include <limits>

int main()
{
    // Do not wake early without timed, eligible enhancement.
    assert(RifePresentationPreparationLead(100000, 8.0, false) == 0);
    assert(RifePresentationPreparationLead(0, 8.0, true) == 0);
    assert(RifePresentationPreparationLead(-1, 8.0, true) == 0);
    assert(RifePresentationPreparationLead(100000, -1.0, true) == 0);
    assert(RifePresentationPreparationLead(100000, std::numeric_limits<double>::quiet_NaN(), true) == 0);
    assert(RifePresentationPreparationLead(100000, std::numeric_limits<double>::infinity(), true) == 0);

    // At 60 Hz, a 6 ms pass gets a 7 ms lead; even a slow pass never gets
    // more than 16 ms. At 240 Hz, the same pass gets only one output interval.
    assert(RifePresentationPreparationLead(166833, 6.0, true) == 70000);
    assert(RifePresentationPreparationLead(166833, 40.0, true) == 160000);
    assert(RifePresentationPreparationLead(41666, 6.0, true) == 41666);
    assert(RifePresentationPreparationLead(1000000, std::numeric_limits<double>::max(), true) == 160000);

    // Varying presentation intervals and measured cost still keep the prepared
    // frame inside its own interval, including fractional source timestamps.
    for (const int64_t interval : {1LL, 33333LL, 41666LL, 83375LL, 111222LL, 333667LL}) {
        for (double ms : {0.0, 0.125, 3.5, 8.5, 20.0, 500.0}) {
            const auto lead = RifePresentationPreparationLead(interval, ms, true);
            assert(lead > 0 && lead <= interval && lead <= 160000);
        }
    }

    // A reused pool slot, seek generation, or new timestamp cannot consume the
    // old prepared image. Clearing is effective even with identical timestamps.
    RifePreparedPresentationKey image;
    assert(!image.Matches(0, 0, UINT32_MAX));
    image = {17, 1001000, 2};
    assert(image.Matches(17, 1001000, 2));
    assert(!image.Matches(18, 1001000, 2));
    assert(!image.Matches(17, 1001001, 2));
    assert(!image.Matches(17, 1001000, 3));
    image.Clear();
    assert(!image.Matches(17, 1001000, 2));
    image = {18, 1001000, 2};
    assert(image.Matches(18, 1001000, 2));
    std::cout << "RIFE preparation policy: eligibility, bounds, VFR intervals and invalidation passed\n";
}
