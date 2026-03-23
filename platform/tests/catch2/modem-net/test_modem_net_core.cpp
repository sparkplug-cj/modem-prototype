#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "modem-net-core.h"
}

namespace {

struct Capture {
  std::string lastPrint;
  std::string lastError;
  std::vector<std::string> prints;
  std::vector<std::string> errors;
};

struct State {
  int owner = 0;
  int ensurePoweredRet = 0;
  int configureRet = 0;
  int openRet = 0;
  int dialRet = 0;
  int attachRet = 0;
  int waitRet = 0;
  bool sessionOpen = false;
  bool connected = false;
  bool dnsReady = false;
  bool modemPowered = true;
  int lastError = 0;
  std::string lastErrorText;
  std::string apn;
  std::string ipv4;
  int closeCalls = 0;
  int hangupCalls = 0;
  Capture capture;
};

State g_state;

void reset_state()
{
  g_state = State{};
}

int fake_owner_get()
{
  return g_state.owner;
}

int fake_ensure_powered(void *)
{
  return g_state.ensurePoweredRet;
}

int fake_configure_context(void *, const char *apn)
{
  if (g_state.configureRet == 0) {
    g_state.apn = apn != nullptr ? apn : "";
  }
  return g_state.configureRet;
}

int fake_open_uart_session()
{
  if (g_state.openRet == 0) {
    g_state.sessionOpen = true;
  }
  return g_state.openRet;
}

int fake_dial_ppp(void *)
{
  return g_state.dialRet;
}

int fake_attach_ppp()
{
  if (g_state.attachRet == 0) {
    g_state.connected = true;
  }
  return g_state.attachRet;
}

int fake_wait_for_network(void *)
{
  if (g_state.waitRet == 0) {
    g_state.connected = true;
    g_state.dnsReady = true;
    g_state.ipv4 = "10.0.0.2";
  }
  return g_state.waitRet;
}

void fake_close_uart_session()
{
  g_state.closeCalls++;
  g_state.sessionOpen = false;
  g_state.connected = false;
  g_state.dnsReady = false;
  g_state.ipv4.clear();
}

int fake_escape_and_hangup()
{
  g_state.hangupCalls++;
  return 0;
}

int fake_get_status(struct modem_net_status *out)
{
  if (out == nullptr) {
    return -22;
  }

  out->modemPowered = g_state.modemPowered;
  out->sessionOpen = g_state.sessionOpen;
  out->connected = g_state.connected;
  out->dnsReady = g_state.dnsReady;
  out->uartOwner = g_state.owner;
  out->lastError = g_state.lastError;
  out->lastErrorText = g_state.lastErrorText.empty() ? nullptr : g_state.lastErrorText.c_str();
  out->apn = g_state.apn.empty() ? nullptr : g_state.apn.c_str();
  out->ipv4 = g_state.ipv4.empty() ? nullptr : g_state.ipv4.c_str();
  return 0;
}

void fake_set_apn(const char *apn)
{
  g_state.apn = apn != nullptr ? apn : "";
}

void fake_set_error(int error, const char *message)
{
  g_state.lastError = error;
  g_state.lastErrorText = message != nullptr ? message : "";
}

void fake_clear_error()
{
  g_state.lastError = 0;
  g_state.lastErrorText.clear();
}

void fake_print(void *ctx, const char *fmt, ...)
{
  auto *capture = static_cast<Capture *>(ctx);
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  capture->lastPrint = buffer;
  capture->prints.emplace_back(buffer);
}

void fake_error(void *ctx, const char *fmt, ...)
{
  auto *capture = static_cast<Capture *>(ctx);
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  capture->lastError = buffer;
  capture->errors.emplace_back(buffer);
}

modem_net_ops make_ops()
{
  return modem_net_ops{
      fake_owner_get,
      fake_ensure_powered,
      fake_configure_context,
      fake_open_uart_session,
      fake_dial_ppp,
      fake_attach_ppp,
      fake_wait_for_network,
      fake_close_uart_session,
      fake_escape_and_hangup,
      fake_get_status,
      fake_set_apn,
      fake_set_error,
      fake_clear_error,
      fake_print,
      fake_error,
      &g_state.capture,
  };
}

} // namespace

TEST_CASE("modem ppp connect requires an APN", "[modem-net]")
{
  reset_state();
  auto ops = make_ops();
  char *argv[] = {const_cast<char *>("connect")};

  REQUIRE(modem_net_cmd_connect_core(&ops, 1, argv) == -22);
  REQUIRE(g_state.capture.lastError == "usage: modem ppp connect <apn>");
  REQUIRE(g_state.lastError == -22);
  REQUIRE(g_state.lastErrorText == "APN required");
}

TEST_CASE("modem ppp connect rejects a busy modem UART", "[modem-net]")
{
  reset_state();
  g_state.owner = 3;
  auto ops = make_ops();
  char *argv[] = {const_cast<char *>("connect"), const_cast<char *>("internet")};

  REQUIRE(modem_net_cmd_connect_core(&ops, 2, argv) == -16);
  REQUIRE(g_state.capture.lastError == "modem UART is busy");
  REQUIRE(g_state.lastError == -16);
}

TEST_CASE("modem ppp connect reports PPP connected after full success path", "[modem-net]")
{
  reset_state();
  auto ops = make_ops();
  char *argv[] = {const_cast<char *>("connect"), const_cast<char *>("internet")};

  REQUIRE(modem_net_cmd_connect_core(&ops, 2, argv) == 0);
  REQUIRE(g_state.apn == "internet");
  REQUIRE(g_state.capture.lastPrint == "PPP connected");
  REQUIRE(g_state.capture.prints == std::vector<std::string>{
      "PPP connect: ensure modem powered",
      "PPP connect: configure PDP/APN context",
      "PPP connect: open UART session",
      "PPP connect: dial PPP",
      "PPP connect: attach PPP",
      "PPP connect: wait for network",
      "PPP connected",
  });
  REQUIRE(g_state.closeCalls == 0);
  REQUIRE(g_state.hangupCalls == 0);
}

TEST_CASE("modem ppp connect tears down on post-open failure", "[modem-net]")
{
  reset_state();
  g_state.waitRet = -110;
  auto ops = make_ops();
  char *argv[] = {const_cast<char *>("connect"), const_cast<char *>("internet")};

  REQUIRE(modem_net_cmd_connect_core(&ops, 2, argv) == -110);
  REQUIRE(g_state.hangupCalls == 1);
  REQUIRE(g_state.closeCalls == 1);
  REQUIRE(g_state.capture.prints == std::vector<std::string>{
      "PPP connect: ensure modem powered",
      "PPP connect: configure PDP/APN context",
      "PPP connect: open UART session",
      "PPP connect: dial PPP",
      "PPP connect: attach PPP",
      "PPP connect: wait for network",
      "PPP network wait timed out, tearing down session...",
  });
  REQUIRE(g_state.capture.lastError == "connect failed at wait_for_network: -110");
  REQUIRE(g_state.lastError == -110);
  REQUIRE(g_state.lastErrorText == "connect failed");
}

TEST_CASE("modem ppp disconnect is idempotent when already down", "[modem-net]")
{
  reset_state();
  auto ops = make_ops();

  REQUIRE(modem_net_cmd_disconnect_core(&ops, 1, nullptr) == 0);
  REQUIRE(g_state.capture.lastPrint == "PPP already disconnected");
  REQUIRE(g_state.closeCalls == 0);
}

TEST_CASE("modem ppp disconnect closes an active PPP session", "[modem-net]")
{
  reset_state();
  g_state.sessionOpen = true;
  g_state.connected = true;
  auto ops = make_ops();

  REQUIRE(modem_net_cmd_disconnect_core(&ops, 1, nullptr) == 0);
  REQUIRE(g_state.hangupCalls == 1);
  REQUIRE(g_state.closeCalls == 1);
  REQUIRE(g_state.capture.lastPrint == "PPP disconnected");
}

TEST_CASE("modem ppp status prints current PPP details", "[modem-net]")
{
  reset_state();
  g_state.modemPowered = true;
  g_state.sessionOpen = true;
  g_state.connected = true;
  g_state.dnsReady = true;
  g_state.owner = 3;
  g_state.apn = "internet";
  g_state.ipv4 = "10.0.0.2";
  g_state.lastError = -5;
  g_state.lastErrorText = "connect failed";
  auto ops = make_ops();

  REQUIRE(modem_net_cmd_status_core(&ops, 1, nullptr) == 0);
  REQUIRE(g_state.capture.lastPrint == "modem=on owner=ppp ppp=connected ip=10.0.0.2 dns=ready apn=internet last_error=-5 connect failed");
}
