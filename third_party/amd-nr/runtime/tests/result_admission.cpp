#include "../core/result_admission.h"
#include <cstdio>
#include <limits>

int main() {
    using dlss5nr::result_arrival_usable;
    // C14 log61: capture 3 arrived at 53 and was previously admitted at fade=1.
    if (result_arrival_usable(53, 3, 8)) return 1;
    if (!result_arrival_usable(100, 97, 8)) return 2;
    if (!result_arrival_usable(100, 92, 8)) return 3;
    if (result_arrival_usable(100, 91, 8)) return 4;
    if (result_arrival_usable(3, 53, 8)) return 5;
    if (!result_arrival_usable(53, 53, 0)) return 6;
    if (result_arrival_usable(std::numeric_limits<std::uint64_t>::max(), 0, 8)) return 7;
    std::puts("stale startup rejected, steady arrivals admitted, bounds checked");
}
