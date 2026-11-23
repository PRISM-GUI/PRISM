// Nothing in PRISM allocates before main. A board's heap can live in RAM that main brings up
// (the STM32F746's SDRAM): a global initializer that allocates hard-faults there. operator new is
// replaced here to count, and main reads the count before anything else runs. A plain program,
// not doctest: doctest registers its test cases from global initializers, which allocate.
#include <prism/ui/layout.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
std::atomic<int> allocations{0};
}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    const int before_main = allocations.load();
    if (before_main != 0) {
        std::printf("FAIL: %d allocations before main\n", before_main);
        return 1;
    }
    const prism::ui::LayoutNode node;   // the default theme, made on first use
    if (node.theme == nullptr || node.theme->node_families.empty()) {
        std::printf("FAIL: no default theme\n");
        return 1;
    }
    std::printf("ok: nothing allocated before main\n");
    return 0;
}
