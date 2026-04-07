#include <cstdarg>
#include <deque>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <zephyr/device.h>
#include "modem-at.h"
}

namespace {

struct FakeTransport {
  int openRet = 0;
  int openCalls = 0;
  int closeCalls = 0;
  std::deque<std::string> chunks;
};

struct FakeDebug {
  std::vector<std::string> lines;
};

struct FakeUart {
  bool ready = true;
  int64_t nowMs = 0;
  std::deque<int> pollIn;
  std::string writes;
  std::string pendingResponse;
};

FakeUart g_uart;

int transport_open(void *ctx, char *response, size_t responseSize)
{
  auto *transport = static_cast<FakeTransport *>(ctx);
  transport->openCalls++;
  if ((response != nullptr) && (responseSize > 0U)) {
    response[0] = '\0';
  }
  return transport->openRet;
}

void transport_close(void *ctx)
{
  auto *transport = static_cast<FakeTransport *>(ctx);
  transport->closeCalls++;
}

uint32_t transport_read(void *ctx, uint8_t *buffer, size_t bufferSize)
{
  auto *transport = static_cast<FakeTransport *>(ctx);
  if (transport->chunks.empty()) {
    return 0;
  }

  std::string chunk = transport->chunks.front();
  transport->chunks.pop_front();
  const size_t copySize = std::min(bufferSize, chunk.size());
  for (size_t i = 0; i < copySize; ++i) {
    buffer[i] = static_cast<uint8_t>(chunk[i]);
  }
  return static_cast<uint32_t>(copySize);
}

void debug_log(void *ctx, const char *fmt, ...)
{
  auto *debug = static_cast<FakeDebug *>(ctx);
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  debug->lines.emplace_back(buffer);
}

void reset_uart()
{
  g_uart = {};
}

} // namespace

extern "C" {

const struct device g_modem_at_test_device = {};

bool device_is_ready(const struct device *dev)
{
  return (dev == &g_modem_at_test_device) && g_uart.ready;
}

int uart_poll_in(const struct device *dev, unsigned char *ch)
{
  if (dev != &g_modem_at_test_device) {
    return -1;
  }
  if (g_uart.pollIn.empty()) {
    return -1;
  }

  const int value = g_uart.pollIn.front();
  g_uart.pollIn.pop_front();
  if (value < 0) {
    return value;
  }

  *ch = static_cast<unsigned char>(value);
  return 0;
}

void uart_poll_out(const struct device *dev, unsigned char ch)
{
  if (dev == &g_modem_at_test_device) {
    g_uart.writes.push_back(static_cast<char>(ch));
    if ((ch == '\r') && !g_uart.pendingResponse.empty()) {
      for (char pending : g_uart.pendingResponse) {
        g_uart.pollIn.push_back(static_cast<unsigned char>(pending));
      }
      g_uart.pendingResponse.clear();
    }
  }
}

int64_t k_uptime_get(void)
{
  return g_uart.nowMs;
}

void k_msleep(int32_t ms)
{
  g_uart.nowMs += ms;
}

void uart_irq_update(const struct device *dev)
{
  (void)dev;
  // Noop for test
}

int uart_irq_rx_ready(const struct device *dev)
{
  (void)dev;
  return g_uart.pollIn.empty() ? 0 : 1;
}

int uart_fifo_read(const struct device *dev, uint8_t *buf, const int size)
{
  (void)dev;
  if (g_uart.pollIn.empty()) {
    return 0;
  }

  int read = 0;
  for (int i = 0; i < size && !g_uart.pollIn.empty(); i++) {
    const int byte = g_uart.pollIn.front();
    g_uart.pollIn.pop_front();
    buf[i] = static_cast<uint8_t>(byte);
    read++;
  }
  return read;
}

void uart_irq_callback_user_data_set(const struct device *dev,
                                     uart_irq_callback_user_data_t callback,
                                     void *user_data)
{
  (void)dev;
  (void)callback;
  (void)user_data;
  // Noop for test
}

void uart_irq_rx_enable(const struct device *dev)
{
  (void)dev;
  // Noop for test
}

void uart_irq_rx_disable(const struct device *dev)
{
  (void)dev;
  // Noop for test
}

} // extern "C"

TEST_CASE("modem_at_send_irq rejects invalid arguments", "[modem-at]")
{
  reset_uart();
  char response[32] = {};
  FakeTransport transport;

  const modem_at_irq_transport transportOps = {
    &transport,
    transport_open,
    transport_close,
    transport_read,
  };

  REQUIRE(modem_at_send_irq(nullptr, response, sizeof(response), &transportOps, nullptr) == -EINVAL);
  REQUIRE(modem_at_send_irq("AT", nullptr, sizeof(response), &transportOps, nullptr) == -EINVAL);
  REQUIRE(modem_at_send_irq("AT", response, 0, &transportOps, nullptr) == -EINVAL);
  REQUIRE(modem_at_send_irq("AT", response, sizeof(response), nullptr, nullptr) == -EINVAL);
  REQUIRE(transport.openCalls == 0);
}

TEST_CASE("modem_at_send_irq writes command and returns response on OK", "[modem-at]")
{
  reset_uart();
  FakeTransport transport;
  transport.chunks.push_back("\r\nOK\r\n");
  FakeDebug debug;
  char response[32] = {};

  const modem_at_irq_transport transportOps = {
    &transport,
    transport_open,
    transport_close,
    transport_read,
  };
  const modem_at_irq_debug debugOps = {
    &debug,
    debug_log,
  };

  REQUIRE(modem_at_send_irq("AT", response, sizeof(response), &transportOps, &debugOps) == 0);
  REQUIRE(transport.openCalls == 1);
  REQUIRE(transport.closeCalls == 1);
  REQUIRE(g_uart.writes == std::string("AT\r"));
  REQUIRE(std::string(response) == "\r\nOK\r\n");
  REQUIRE_FALSE(debug.lines.empty());
}

TEST_CASE("modem_at_send_irq returns EIO on modem error text", "[modem-at]")
{
  reset_uart();
  FakeTransport transport;
  transport.chunks.push_back("\r\nERROR\r\n");
  char response[32] = {};

  const modem_at_irq_transport transportOps = {
    &transport,
    transport_open,
    transport_close,
    transport_read,
  };

  REQUIRE(modem_at_send_irq("AT", response, sizeof(response), &transportOps, nullptr) == -EIO);
  REQUIRE(transport.openCalls == 1);
  REQUIRE(transport.closeCalls == 1);
  REQUIRE(std::string(response) == "\r\nERROR\r\n");
}

TEST_CASE("modem_at_send_irq times out and still closes transport", "[modem-at]")
{
  reset_uart();
  FakeTransport transport;
  char response[32] = {};

  const modem_at_irq_transport transportOps = {
    &transport,
    transport_open,
    transport_close,
    transport_read,
  };

  REQUIRE(modem_at_send_irq("AT", response, sizeof(response), &transportOps, nullptr) == -ETIMEDOUT);
  REQUIRE(transport.openCalls == 1);
  REQUIRE(transport.closeCalls == 1);
  REQUIRE(g_uart.nowMs >= 5000);
}

TEST_CASE("modem_at_send uses polling UART path and trims response", "[modem-at]")
{
  reset_uart();
  g_uart.pendingResponse = "\r\nSierra Wireless RC7620-1\r\nOK\r\n";

  char response[64] = {};
  REQUIRE(modem_at_send("ATI", response, sizeof(response)) == 0);
  REQUIRE(g_uart.writes == std::string("ATI\r"));
  REQUIRE(std::string(response) == "Sierra Wireless RC7620-1\nOK");

  modem_at_diagnostics diagnostics = {};
  modem_at_get_last_diagnostics(&diagnostics);
  REQUIRE(diagnostics.sawAnyByte);
  REQUIRE(diagnostics.exitReason == MODEM_AT_EXIT_MATCH_OK);
}

TEST_CASE("modem_at_send reports overall timeout when no bytes arrive", "[modem-at]")
{
  reset_uart();
  char response[16] = {};

  REQUIRE(modem_at_send("AT", response, sizeof(response)) == -ETIMEDOUT);

  modem_at_diagnostics diagnostics = {};
  modem_at_get_last_diagnostics(&diagnostics);
  REQUIRE_FALSE(diagnostics.sawAnyByte);
  REQUIRE(diagnostics.exitReason == MODEM_AT_EXIT_OVERALL_TIMEOUT);
}
