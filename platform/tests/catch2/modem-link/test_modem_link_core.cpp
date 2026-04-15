#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "modem-link-internal.h"
}

namespace {

struct FakeContext {
  int resumeRet = 0;
  int getIfaceRet = 0;
  int ifaceUpRet = 0;
  int l4WaitRet = 0;
  int dnsWaitRet = 0;
  uint64_t l4Event = 0x1111;
  uint64_t dnsEvent = 0x2222;
  void *iface = reinterpret_cast<void *>(0x1234);
  int resumeCalls = 0;
  int getIfaceCalls = 0;
  int ifaceUpCalls = 0;
  int waitCalls = 0;
};

int fake_resume_modem(void *ctx)
{
  auto *fake = static_cast<FakeContext *>(ctx);
  fake->resumeCalls++;
  return fake->resumeRet;
}

int fake_get_iface(void *ctx, void **ifaceOut)
{
  auto *fake = static_cast<FakeContext *>(ctx);
  fake->getIfaceCalls++;

  if (fake->getIfaceRet != 0) {
    return fake->getIfaceRet;
  }

  *ifaceOut = fake->iface;
  return 0;
}

int fake_iface_up(void *ctx, void *iface)
{
  auto *fake = static_cast<FakeContext *>(ctx);
  fake->ifaceUpCalls++;
  REQUIRE(iface == fake->iface);
  return fake->ifaceUpRet;
}

int fake_wait_event(void *ctx, void *iface, enum modem_link_core_event event,
                    int32_t timeoutMs, uint64_t *raisedEvent)
{
  auto *fake = static_cast<FakeContext *>(ctx);
  fake->waitCalls++;
  REQUIRE(iface == fake->iface);

  if (event == MODEM_LINK_CORE_EVENT_L4_CONNECTED) {
    REQUIRE(timeoutMs == MODEM_LINK_DEFAULT_L4_TIMEOUT_MS);
    if (raisedEvent != nullptr) {
      *raisedEvent = fake->l4Event;
    }
    return fake->l4WaitRet;
  }

  REQUIRE(event == MODEM_LINK_CORE_EVENT_DNS_SERVER_ADD);
  REQUIRE(timeoutMs == MODEM_LINK_DEFAULT_DNS_TIMEOUT_MS);
  if (raisedEvent != nullptr) {
    *raisedEvent = fake->dnsEvent;
  }
  return fake->dnsWaitRet;
}

const modem_link_core_ops fakeOps = {
  fake_resume_modem,
  fake_get_iface,
  fake_iface_up,
  fake_wait_event,
};

} // namespace

TEST_CASE("modem link core reaches DNS ready on success", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == 0);
  REQUIRE(fake.resumeCalls == 1);
  REQUIRE(fake.getIfaceCalls == 1);
  REQUIRE(fake.ifaceUpCalls == 1);
  REQUIRE(fake.waitCalls == 2);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_DNS_READY);
  REQUIRE(diagnostics.lastError == 0);
  REQUIRE(diagnostics.lastEvent == fake.dnsEvent);
  REQUIRE(diagnostics.modemResumed);
  REQUIRE(diagnostics.pppInterfaceFound);
  REQUIRE(diagnostics.pppInterfaceUp);
  REQUIRE(diagnostics.l4Connected);
  REQUIRE(diagnostics.dnsServerAdded);
}

TEST_CASE("modem link core can skip DNS wait", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;
  modem_link_options options = modem_link_default_options();

  options.waitForDns = false;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, &options, &diagnostics) == 0);
  REQUIRE(fake.waitCalls == 1);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_L4_CONNECTED);
  REQUIRE(diagnostics.lastEvent == fake.l4Event);
  REQUIRE(diagnostics.l4Connected);
  REQUIRE_FALSE(diagnostics.dnsServerAdded);
}

TEST_CASE("modem link core reports missing PPP iface", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  fake.getIfaceRet = -ENODEV;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == -ENODEV);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_MODEM_RESUMED);
  REQUIRE(diagnostics.lastError == -ENODEV);
  REQUIRE(diagnostics.modemResumed);
  REQUIRE_FALSE(diagnostics.pppInterfaceFound);
}

TEST_CASE("modem link core treats already-resumed modem as ready", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  fake.resumeRet = -EALREADY;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == 0);
  REQUIRE(fake.resumeCalls == 1);
  REQUIRE(fake.getIfaceCalls == 1);
  REQUIRE(fake.ifaceUpCalls == 1);
  REQUIRE(fake.waitCalls == 2);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_DNS_READY);
  REQUIRE(diagnostics.modemResumed);
  REQUIRE(diagnostics.pppInterfaceFound);
  REQUIRE(diagnostics.pppInterfaceUp);
}

TEST_CASE("modem link core treats already-up PPP iface as ready", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  fake.ifaceUpRet = -EALREADY;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == 0);
  REQUIRE(fake.resumeCalls == 1);
  REQUIRE(fake.getIfaceCalls == 1);
  REQUIRE(fake.ifaceUpCalls == 1);
  REQUIRE(fake.waitCalls == 2);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_DNS_READY);
  REQUIRE(diagnostics.modemResumed);
  REQUIRE(diagnostics.pppInterfaceFound);
  REQUIRE(diagnostics.pppInterfaceUp);
}

TEST_CASE("modem link core reports L4 wait failure", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  fake.l4WaitRet = -ETIMEDOUT;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == -ETIMEDOUT);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_PPP_IFACE_UP);
  REQUIRE(diagnostics.lastError == -ETIMEDOUT);
  REQUIRE(diagnostics.pppInterfaceUp);
  REQUIRE_FALSE(diagnostics.l4Connected);
}

TEST_CASE("modem link core reports DNS wait failure after L4", "[modem-link]")
{
  FakeContext fake;
  modem_link_diagnostics diagnostics;

  fake.dnsWaitRet = -ETIMEDOUT;

  REQUIRE(modem_link_ensure_ready_core(&fakeOps, &fake, nullptr, &diagnostics) == -ETIMEDOUT);
  REQUIRE(diagnostics.stage == MODEM_LINK_STAGE_L4_CONNECTED);
  REQUIRE(diagnostics.lastError == -ETIMEDOUT);
  REQUIRE(diagnostics.l4Connected);
  REQUIRE_FALSE(diagnostics.dnsServerAdded);
}
