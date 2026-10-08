#include <atomic>
#include <doctest.h>
#include <memory>
#include <new>

#include "qt_executor.hpp"

namespace
{
struct QtFixture
{
    QtFixture() { reset(); }
    ~QtFixture() { reset(); }
    void reset() noexcept
    {
        qt_mock::failCreate = false;
        qt_mock::failStart = false;
        qt_mock::callerRuns = false;
    }
};
}

TEST_CASE("Qt submission host protocol: preparation and preaccept failure preserve reuse")
{
    for (const bool preparation : {false, true})
    {
        QtFixture fixture;
        QtExecutor executor{1};
        int calls = 0;
        auto payload = std::make_shared<int>(17);
        const std::weak_ptr<int> retained = payload;
        qt_mock::failCreate = preparation;
        qt_mock::failStart = !preparation;
        CHECK_THROWS_AS(executor.dispatch("rejected", [&, payload] { ++calls; },
                                         [&, payload] { ++calls; }, -1, 5, 0), std::bad_alloc);
        payload.reset();
        executor.waitAll();
        CHECK(retained.expired());
        CHECK(calls == 0);
        fixture.reset();
        executor.dispatch("recover", [&] { ++calls; }, [&] { ++calls; }, -1, 5, 0);
        executor.waitAll();
        CHECK(calls == 2);
    }
}

TEST_CASE("Qt submission host protocol: caller-runs overflow completes without a phantom count")
{
    QtFixture fixture;
    QtExecutor executor{1};
    qt_mock::callerRuns = true;
    int calls = 0;
    executor.dispatch("inline", [&] { ++calls; }, [&] { ++calls; }, -1, 5, 0);
    executor.waitAll();
    CHECK(calls == 2);
}
