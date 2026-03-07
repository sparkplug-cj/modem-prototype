#include <cstdarg>
#include <cstdio>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fff.h>

extern "C" {
#include "../../../src/modem-shell/modem-shell-core.h"
}

DEFINE_FFF_GLOBALS;

FAKE_VALUE_FUNC0(int, modem_board_power_on_fake);
FAKE_VALUE_FUNC0(int, modem_board_power_off_fake);
FAKE_VALUE_FUNC0(int, modem_board_power_cycle_fake);
FAKE_VALUE_FUNC0(int, modem_board_reset_pulse_fake);
FAKE_VALUE_FUNC(int, modem_board_get_status_fake, struct modem_board_status *);

namespace {

struct ShellCapture {
  std::string lastPrint;
  std::string lastError;
};

void shell_print_capture(void *ctx, const char *fmt, ...)
{
  auto *capture = static_cast<ShellCapture *>(ctx);
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  capture->lastPrint = buffer;
}

void shell_error_capture(void *ctx, const char *fmt, ...)
{
  auto *capture = static_cast<ShellCapture *>(ctx);
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  capture->lastError = buffer;
}

void reset_fakes()
{
  RESET_FAKE(modem_board_power_on_fake);
  RESET_FAKE(modem_board_power_off_fake);
  RESET_FAKE(modem_board_power_cycle_fake);
  RESET_FAKE(modem_board_reset_pulse_fake);
  RESET_FAKE(modem_board_get_status_fake);
  FFF_RESET_HISTORY();
}

int fake_status_success(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on_n = 0;
  out->rst_n = 1;
  out->vgpio_mv = 1800;
  out->modem_state_on = true;
  return 0;
}

int fake_status_off(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on_n = 0;
  out->rst_n = 1;
  out->vgpio_mv = 0;
  out->modem_state_on = false;
  return 0;
}

int fake_status_vgpio_error(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on_n = 0;
  out->rst_n = 1;
  out->vgpio_mv = -EIO;
  out->modem_state_on = false;
  return 0;
}

} // namespace

TEST_CASE("modem status prints the current board state", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_success;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON_N=0, MODEM_RST_N=1, VGPIO_mV=1800, MODEM_STATE=ON");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem status prints OFF when VGPIO is below threshold", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_off;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON_N=0, MODEM_RST_N=1, VGPIO_mV=0, MODEM_STATE=OFF");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem status reports board read errors", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.return_val = -ENODEV;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == -ENODEV);
  REQUIRE(capture.lastPrint.empty());
  REQUIRE(capture.lastError == "status read failed: -19");
}

TEST_CASE("modem status prints partial status when VGPIO read fails", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_vgpio_error;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON_N=0, MODEM_RST_N=1, VGPIO_mV=ERR(-5), MODEM_STATE=OFF");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem reset prints OK on success", "[modem-shell]")
{
  reset_fakes();
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_reset_core(&ops) == 0);
  REQUIRE(modem_board_reset_pulse_fake_fake.call_count == 1);
  REQUIRE(capture.lastPrint == "OK");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem power validates usage and dispatches requested operation", "[modem-shell]")
{
  reset_fakes();
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "power";
  char on[] = "on";
  char *argv[] = {command, on};

  REQUIRE(modem_shell_cmd_power_core(&ops, 2, argv) == 0);
  REQUIRE(modem_board_power_on_fake_fake.call_count == 1);
  REQUIRE(capture.lastPrint == "OK");

  capture = {};
  REQUIRE(modem_shell_cmd_power_core(&ops, 1, argv) == -EINVAL);
  REQUIRE(capture.lastError == "usage: modem power <on|off|cycle>");
}

TEST_CASE("modem power reports unknown operations and downstream failures", "[modem-shell]")
{
  reset_fakes();
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "power";
  char bogus[] = "bogus";
  char *badArgv[] = {command, bogus};

  REQUIRE(modem_shell_cmd_power_core(&ops, 2, badArgv) == -EINVAL);
  REQUIRE(capture.lastError == "unknown power op: bogus");

  capture = {};
  modem_board_power_cycle_fake_fake.return_val = -EIO;
  char cycle[] = "cycle";
  char *cycleArgv[] = {command, cycle};
  REQUIRE(modem_shell_cmd_power_core(&ops, 2, cycleArgv) == -EIO);
  REQUIRE(modem_board_power_cycle_fake_fake.call_count == 1);
  REQUIRE(capture.lastError == "power cycle failed: -5");
}
