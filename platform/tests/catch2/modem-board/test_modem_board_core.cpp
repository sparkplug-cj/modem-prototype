#include <catch2/catch_test_macros.hpp>
#include <fff.h>

extern "C" {
#include <modem-board.h>
#include "../../../src/modem-board/modem-board-core.h"
}

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC(int, ensure_ready_fake, void *);
FAKE_VALUE_FUNC(int, set_rail_en_fake, void *, int);
FAKE_VALUE_FUNC(int, set_pwr_on_asserted_fake, void *, bool);
FAKE_VALUE_FUNC(int, set_rst_asserted_fake, void *, bool);
FAKE_VALUE_FUNC(int, get_rail_en_fake, void *);
FAKE_VALUE_FUNC(int, get_pwr_on_fake, void *);
FAKE_VALUE_FUNC(int, get_rst_fake, void *);
FAKE_VALUE_FUNC(int, get_vgpio_mv_fake, void *);
FAKE_VOID_FUNC(sleep_ms_fake, void *, int);

namespace {

const modem_board_ops TEST_OPS = {
  ensure_ready_fake,
  set_rail_en_fake,
  set_pwr_on_asserted_fake,
  set_rst_asserted_fake,
  get_rail_en_fake,
  get_pwr_on_fake,
  get_rst_fake,
  get_vgpio_mv_fake,
  sleep_ms_fake,
  nullptr,
};

void reset_fakes()
{
  RESET_FAKE(ensure_ready_fake);
  RESET_FAKE(set_rail_en_fake);
  RESET_FAKE(set_pwr_on_asserted_fake);
  RESET_FAKE(set_rst_asserted_fake);
  RESET_FAKE(get_rail_en_fake);
  RESET_FAKE(get_pwr_on_fake);
  RESET_FAKE(get_rst_fake);
  RESET_FAKE(get_vgpio_mv_fake);
  RESET_FAKE(sleep_ms_fake);
  FFF_RESET_HISTORY();
}

} // namespace

TEST_CASE("power_on drives rail enable and power pulse sequence", "[modem-board]")
{
  reset_fakes();

  REQUIRE(modem_board_power_on_core(&TEST_OPS) == 0);

  REQUIRE(ensure_ready_fake_fake.call_count == 1);
  REQUIRE(set_rail_en_fake_fake.call_count == 1);
  REQUIRE(set_rail_en_fake_fake.arg1_val == 1);
  REQUIRE(set_pwr_on_asserted_fake_fake.call_count == 2);
  REQUIRE(set_pwr_on_asserted_fake_fake.arg1_history[0] == true);
  REQUIRE(set_pwr_on_asserted_fake_fake.arg1_history[1] == false);
  REQUIRE(sleep_ms_fake_fake.call_count == 3);
  REQUIRE(sleep_ms_fake_fake.arg1_history[0] == 10);
  REQUIRE(sleep_ms_fake_fake.arg1_history[1] == 250);
  REQUIRE(sleep_ms_fake_fake.arg1_history[2] == 100);

  REQUIRE(fff.call_history[0] == (void (*)())ensure_ready_fake);
  REQUIRE(fff.call_history[1] == (void (*)())set_rail_en_fake);
  REQUIRE(fff.call_history[2] == (void (*)())sleep_ms_fake);
  REQUIRE(fff.call_history[3] == (void (*)())set_pwr_on_asserted_fake);
  REQUIRE(fff.call_history[4] == (void (*)())sleep_ms_fake);
  REQUIRE(fff.call_history[5] == (void (*)())set_pwr_on_asserted_fake);
}

TEST_CASE("power_on returns early if hardware is not ready", "[modem-board]")
{
  reset_fakes();
  ensure_ready_fake_fake.return_val = -ENODEV;

  REQUIRE(modem_board_power_on_core(&TEST_OPS) == -ENODEV);
  REQUIRE(set_rail_en_fake_fake.call_count == 0);
  REQUIRE(set_pwr_on_asserted_fake_fake.call_count == 0);
  REQUIRE(sleep_ms_fake_fake.call_count == 0);
}

TEST_CASE("power_off pulses power pin and disables rail", "[modem-board]")
{
  reset_fakes();

  REQUIRE(modem_board_power_off_core(&TEST_OPS) == 0);

  REQUIRE(ensure_ready_fake_fake.call_count == 1);
  REQUIRE(set_pwr_on_asserted_fake_fake.call_count == 2);
  REQUIRE(set_pwr_on_asserted_fake_fake.arg1_history[0] == true);
  REQUIRE(set_pwr_on_asserted_fake_fake.arg1_history[1] == false);
  REQUIRE(sleep_ms_fake_fake.call_count == 1);
  REQUIRE(sleep_ms_fake_fake.arg1_val == 1500);
  REQUIRE(set_rail_en_fake_fake.call_count == 1);
  REQUIRE(set_rail_en_fake_fake.arg1_val == 0);
}

TEST_CASE("power_cycle performs power_off delay power_on", "[modem-board]")
{
  reset_fakes();

  REQUIRE(modem_board_power_cycle_core(&TEST_OPS) == 0);

  REQUIRE(ensure_ready_fake_fake.call_count == 2);
  REQUIRE(set_pwr_on_asserted_fake_fake.call_count == 4);
  REQUIRE(set_rail_en_fake_fake.call_count == 2);
  REQUIRE(set_rail_en_fake_fake.arg1_history[0] == 0);
  REQUIRE(set_rail_en_fake_fake.arg1_history[1] == 1);
  REQUIRE(sleep_ms_fake_fake.call_count == 5);
  REQUIRE(sleep_ms_fake_fake.arg1_history[0] == 1500);
  REQUIRE(sleep_ms_fake_fake.arg1_history[1] == 500);
  REQUIRE(sleep_ms_fake_fake.arg1_history[2] == 10);
  REQUIRE(sleep_ms_fake_fake.arg1_history[3] == 250);
  REQUIRE(sleep_ms_fake_fake.arg1_history[4] == 100);
}

TEST_CASE("reset_pulse asserts and then deasserts reset", "[modem-board]")
{
  reset_fakes();

  REQUIRE(modem_board_reset_pulse_core(&TEST_OPS) == 0);

  REQUIRE(ensure_ready_fake_fake.call_count == 1);
  REQUIRE(set_rst_asserted_fake_fake.call_count == 2);
  REQUIRE(set_rst_asserted_fake_fake.arg1_history[0] == true);
  REQUIRE(set_rst_asserted_fake_fake.arg1_history[1] == false);
  REQUIRE(sleep_ms_fake_fake.call_count == 1);
  REQUIRE(sleep_ms_fake_fake.arg1_val == 200);
}

TEST_CASE("get_status reports current line values", "[modem-board]")
{
  reset_fakes();
  get_rail_en_fake_fake.return_val = 1;
  get_pwr_on_fake_fake.return_val = 0;
  get_rst_fake_fake.return_val = 1;
  get_vgpio_mv_fake_fake.return_val = 1800;

  modem_board_status status{};
  REQUIRE(modem_board_get_status_core(&TEST_OPS, &status) == 0);

  REQUIRE(ensure_ready_fake_fake.call_count == 1);
  REQUIRE(status.rail_en == 1);
  REQUIRE(status.pwr_on == 0);
  REQUIRE(status.rst == 1);
  REQUIRE(status.vgpio_mv == 1800);
  REQUIRE(status.modem_state_on == true);
}

TEST_CASE("get_status derives OFF below threshold and ON at threshold", "[modem-board]")
{
  reset_fakes();
  get_vgpio_mv_fake_fake.return_val = 899;

  modem_board_status status{};
  REQUIRE(modem_board_get_status_core(&TEST_OPS, &status) == 0);
  REQUIRE(status.vgpio_mv == 899);
  REQUIRE(status.modem_state_on == false);

  reset_fakes();
  get_vgpio_mv_fake_fake.return_val = 900;

  REQUIRE(modem_board_get_status_core(&TEST_OPS, &status) == 0);
  REQUIRE(status.vgpio_mv == 900);
  REQUIRE(status.modem_state_on == true);
}

TEST_CASE("get_status preserves ADC errors as partial status", "[modem-board]")
{
  reset_fakes();
  get_rail_en_fake_fake.return_val = 1;
  get_pwr_on_fake_fake.return_val = 0;
  get_rst_fake_fake.return_val = 1;
  get_vgpio_mv_fake_fake.return_val = -EIO;

  modem_board_status status{};
  REQUIRE(modem_board_get_status_core(&TEST_OPS, &status) == 0);
  REQUIRE(status.rail_en == 1);
  REQUIRE(status.pwr_on == 0);
  REQUIRE(status.rst == 1);
  REQUIRE(status.vgpio_mv == -EIO);
  REQUIRE(status.modem_state_on == false);
}

TEST_CASE("get_status rejects null output", "[modem-board]")
{
  reset_fakes();

  REQUIRE(modem_board_get_status_core(&TEST_OPS, nullptr) == -EINVAL);
  REQUIRE(ensure_ready_fake_fake.call_count == 0);
}
