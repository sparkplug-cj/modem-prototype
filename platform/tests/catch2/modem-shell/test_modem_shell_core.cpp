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
FAKE_VALUE_FUNC3(int, modem_at_send_fake, const char *, char *, size_t);
FAKE_VOID_FUNC1(modem_sleep_ms_fake, int32_t);

namespace {

struct ShellCapture {
  std::string lastPrint;
  std::string lastError;
};

modem_at_diagnostics g_lastDiagnostics = {};

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
  RESET_FAKE(modem_at_send_fake);
  RESET_FAKE(modem_sleep_ms_fake);
  FFF_RESET_HISTORY();
  g_lastDiagnostics = {};
}

int fake_status_success(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on = 0;
  out->rst = 1;
  out->vgpio_mv = 1800;
  out->modem_state_on = true;
  return 0;
}

int fake_status_off(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on = 0;
  out->rst = 1;
  out->vgpio_mv = 0;
  out->modem_state_on = false;
  return 0;
}

int fake_status_vgpio_error(struct modem_board_status *out)
{
  out->rail_en = 1;
  out->pwr_on = 0;
  out->rst = 1;
  out->vgpio_mv = -EIO;
  out->modem_state_on = false;
  return 0;
}

int fake_status_unpowered(struct modem_board_status *out)
{
  out->rail_en = 0;
  out->pwr_on = 1;
  out->rst = 0;
  return 0;
}

int fake_at_send_success(const char *command, char *response, size_t responseSize)
{
  (void)command;
  snprintf(response, responseSize, "Quectel RC7620-1");
  g_lastDiagnostics.bytesReceived = strlen(response);
  g_lastDiagnostics.sawAnyByte = true;
  g_lastDiagnostics.exitReason = MODEM_AT_EXIT_INTER_CHAR_TIMEOUT;
  return 0;
}

int fake_at_send_empty(const char *command, char *response, size_t responseSize)
{
  (void)command;
  if (responseSize > 0) {
    response[0] = '\0';
  }
  g_lastDiagnostics.bytesReceived = 0;
  g_lastDiagnostics.sawAnyByte = true;
  g_lastDiagnostics.exitReason = MODEM_AT_EXIT_MATCH_OK;
  return 0;
}

int fake_at_send_echo(const char *command, char *response, size_t responseSize)
{
  snprintf(response, responseSize, "%s", command);
  g_lastDiagnostics.bytesReceived = strlen(response);
  g_lastDiagnostics.sawAnyByte = true;
  g_lastDiagnostics.exitReason = MODEM_AT_EXIT_INTER_CHAR_TIMEOUT;
  return 0;
}

int fake_at_send_power_on_success(const char *command, char *response, size_t responseSize)
{
  if (strcmp(command, "AT") == 0 || strcmp(command, "AT+KSLEEP=2") == 0) {
    snprintf(response, responseSize, "OK");
    return 0;
  }
  return -EINVAL;
}

int fake_at_send_power_on_ksleep_fail(const char *command, char *response, size_t responseSize)
{
  if (strcmp(command, "AT") == 0) {
    snprintf(response, responseSize, "OK");
    return 0;
  }
  if (strcmp(command, "AT+KSLEEP=2") == 0) {
    if (responseSize > 0) {
      response[0] = '\0';
    }
    return -ETIMEDOUT;
  }
  return -EINVAL;
}


} // namespace

extern "C" void modem_at_get_last_diagnostics(struct modem_at_diagnostics *diagnostics)
{
  if (diagnostics != nullptr) {
    *diagnostics = g_lastDiagnostics;
  }
}

extern "C" const char *modem_at_exit_reason_str(enum modem_at_exit_reason reason)
{
  switch (reason) {
  case MODEM_AT_EXIT_NONE:
    return "none";
  case MODEM_AT_EXIT_MATCH_OK:
    return "matched-ok";
  case MODEM_AT_EXIT_MATCH_ERROR:
    return "matched-error";
  case MODEM_AT_EXIT_INTER_CHAR_TIMEOUT:
    return "inter-char-timeout";
  case MODEM_AT_EXIT_OVERALL_TIMEOUT:
    return "overall-timeout";
  case MODEM_AT_EXIT_BUFFER_FULL:
    return "buffer-full";
  case MODEM_AT_EXIT_UART_ERROR:
    return "uart-error";
  default:
    return "unknown";
  }
}

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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON=0, MODEM_RST=1, VGPIO_mV=1800, MODEM_STATE=ON");
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON=0, MODEM_RST=1, VGPIO_mV=0, MODEM_STATE=OFF");
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  REQUIRE(modem_shell_cmd_status_core(&ops) == 0);
  REQUIRE(capture.lastPrint == "MODEM_3V8_EN=1, MODEM_PWR_ON=0, MODEM_RST=1, VGPIO_mV=ERR(-5), MODEM_STATE=OFF");
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  modem_at_send_fake_fake.custom_fake = fake_at_send_power_on_success;

  char command[] = "power";
  char on[] = "on";
  char *argv[] = {command, on};

  REQUIRE(modem_shell_cmd_power_core(&ops, 2, argv) == 0);
  REQUIRE(modem_board_power_on_fake_fake.call_count == 1);
  REQUIRE(modem_sleep_ms_fake_fake.call_count == 1);
  REQUIRE(modem_sleep_ms_fake_fake.arg0_val == 5000);
  REQUIRE(modem_at_send_fake_fake.call_count == 2);
  REQUIRE(std::string(modem_at_send_fake_fake.arg0_history[0]) == "AT");
  REQUIRE(std::string(modem_at_send_fake_fake.arg0_history[1]) == "AT+KSLEEP=2");
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
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
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

TEST_CASE("modem power reports sleep-disable failures after successful power on", "[modem-shell]")
{
  reset_fakes();
  modem_at_send_fake_fake.custom_fake = fake_at_send_power_on_ksleep_fail;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "power";
  char on[] = "on";
  char *argv[] = {command, on};

  REQUIRE(modem_shell_cmd_power_core(&ops, 2, argv) == -ETIMEDOUT);
  REQUIRE(modem_board_power_on_fake_fake.call_count == 1);
  REQUIRE(modem_sleep_ms_fake_fake.call_count == 1);
  REQUIRE(modem_at_send_fake_fake.call_count == 2);
  REQUIRE(capture.lastError == "failed to disable modem sleep: -110");
}

TEST_CASE("modem at validates usage", "[modem-shell]")
{
  reset_fakes();
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char *argv[] = {command};

  REQUIRE(modem_shell_cmd_at_core(&ops, 1, argv) == -EINVAL);
  REQUIRE(capture.lastError == "usage: at <command>");
}

TEST_CASE("modem at refuses requests while modem rail is off", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_unpowered;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char ati[] = "ATI";
  char *argv[] = {command, ati};

  REQUIRE(modem_shell_cmd_at_core(&ops, 2, argv) == -EHOSTDOWN);
  REQUIRE(modem_at_send_fake_fake.call_count == 0);
  REQUIRE(capture.lastError == "modem is not powered");
}

TEST_CASE("modem at prints transport response on success", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_success;
  modem_at_send_fake_fake.custom_fake = fake_at_send_success;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char ati[] = "ATI";
  char *argv[] = {command, ati};

  REQUIRE(modem_shell_cmd_at_core(&ops, 2, argv) == 0);
  REQUIRE(modem_at_send_fake_fake.call_count == 1);
  REQUIRE(std::string(modem_at_send_fake_fake.arg0_val) == "ATI");
  REQUIRE(capture.lastPrint == "[raw modem response]\nQuectel RC7620-1\n[modem-at] exit=inter-char-timeout bytes=16");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem at reports empty modem response explicitly", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_success;
  modem_at_send_fake_fake.custom_fake = fake_at_send_empty;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char ati[] = "ATI";
  char *argv[] = {command, ati};

  REQUIRE(modem_shell_cmd_at_core(&ops, 2, argv) == 0);
  REQUIRE(capture.lastPrint == "[empty modem response]\n[modem-at] exit=matched-ok bytes=0");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem at reports echo-only modem response explicitly", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_success;
  modem_at_send_fake_fake.custom_fake = fake_at_send_echo;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char ati[] = "ATI";
  char *argv[] = {command, ati};

  REQUIRE(modem_shell_cmd_at_core(&ops, 2, argv) == 0);
  REQUIRE(capture.lastPrint == "[echo only]\nATI\n[modem-at] exit=inter-char-timeout bytes=3");
  REQUIRE(capture.lastError.empty());
}

TEST_CASE("modem at reports transport errors cleanly", "[modem-shell]")
{
  reset_fakes();
  modem_board_get_status_fake_fake.custom_fake = fake_status_success;
  modem_at_send_fake_fake.return_val = -ETIMEDOUT;
  g_lastDiagnostics.bytesReceived = 0;
  g_lastDiagnostics.sawAnyByte = false;
  g_lastDiagnostics.exitReason = MODEM_AT_EXIT_OVERALL_TIMEOUT;
  ShellCapture capture;

  modem_shell_ops ops = {
    modem_board_power_on_fake,
    modem_board_power_off_fake,
    modem_board_power_cycle_fake,
    modem_board_reset_pulse_fake,
    modem_board_get_status_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_at_send_fake,
    modem_sleep_ms_fake,
    shell_print_capture,
    shell_error_capture,
    &capture,
  };

  char command[] = "at";
  char csq[] = "AT+CSQ";
  char *argv[] = {command, csq};

  REQUIRE(modem_shell_cmd_at_core(&ops, 2, argv) == -ETIMEDOUT);
  REQUIRE(capture.lastError == "AT command timed out waiting for modem response (exit=overall-timeout, bytes=0)");
}
