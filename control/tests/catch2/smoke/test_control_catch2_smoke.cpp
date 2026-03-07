#include <catch2/catch_test_macros.hpp>

TEST_CASE("control Catch2 scaffolding builds and runs on the host", "[smoke]")
{
  REQUIRE(3 * 3 == 9);
}
