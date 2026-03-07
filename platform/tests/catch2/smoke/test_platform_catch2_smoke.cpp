#include <catch2/catch_test_macros.hpp>

TEST_CASE("platform Catch2 scaffolding builds and runs on the host", "[smoke]")
{
  REQUIRE(2 + 2 == 4);
}
