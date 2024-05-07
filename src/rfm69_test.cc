#include "rfm69.h"

#include <cstdio>

#include "RollingCodeStorage.h"
#include "SomfyRemote.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pico/time.h"

namespace jagsawning {
class RfmOokDriverTestFriend {
 public:
  static uint8_t ReadRegister(RfmSpiDriver& driver, uint8_t r) {
    return driver.ReadRegister(r);
  }
};
}  // namespace jagsawning

class FixedRollingCodeStorage : public RollingCodeStorage {
 public:
  FixedRollingCodeStorage(uint16_t code) : code_(code) {}
  uint16_t nextCode() override { return code_; }

 private:
  uint16_t code_;
};

using jagsawning::RfmOokDriverTestFriend;
using jagsawning::RfmSpiDriver;

int main() {
  setup_default_uart();
  printf("RfmSpiDriverTest\n");

  RfmSpiDriver driver = RfmSpiDriver::Create();
  auto ReadRegister = [&](uint8_t r) {
    return RfmOokDriverTestFriend::ReadRegister(driver, r);
  };

  uint8_t got = ReadRegister(0x1);
  if (got != 0x04) {
    printf("FAIL -- reg[0x1] 0x4 != %d\n", got);
    exit(1);
  }

  driver.SetMode(RfmSpiDriver::kSleep);
  driver.SetCarrierFrequency(433'420'000);
  driver.SetModulationType(RfmSpiDriver::kOok);
  driver.SetDataMode(RfmSpiDriver::kContinuousWithoutSynchronizer);
  driver.SetPower(0b1111'1111);
  driver.SetMode(RfmSpiDriver::kTransmit);

  while (!(driver.GetRegIrqFlags() & RfmSpiDriver::kTxReady)) {
    printf("tx not ready\n");
    sleep_ms(1000);
  }
  printf("tx ready\n");

  FixedRollingCodeStorage storage(0x1234);
  gpio_init(15);
  gpio_set_dir(15, true);
  SomfyRemote remote(static_cast<unsigned char>(15), 0x123456, &storage);
  remote.setup();
  while (true) {
    remote.sendCommand(Command::My, 7);
    sleep_ms(10000);
  }

  printf("PASS\n");

  while (true) {
    sleep_ms(10000);
  }
  return 0;
}