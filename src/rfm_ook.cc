#include "rfm_ook.h"

#include <algorithm>
#include <cstdint>

#include "pico/time.h"
#include "rfm_ook_config.h"

namespace jagsawning {

RfmSpiDriver RfmSpiDriver::Create(const RfmSpiDriverConfig& config) {
  spi_inst_t* spi = config.spi;
  for (int i : {config.sck_pin, config.miso_pin, config.mosi_pin}) {
    gpio_init(i);
    gpio_set_function(i, GPIO_FUNC_SPI);
  }
  gpio_init(config.cs_pin);
  gpio_set_dir(config.cs_pin, GPIO_OUT);
  gpio_put(config.cs_pin, true);
  gpio_init(config.reset_pin);
  gpio_set_dir(config.reset_pin, GPIO_OUT);
  gpio_put(config.reset_pin, true);

  // Wait for a few milliseconds in case 3v3 takes longer to come up to full
  // power than it takes for us to reach this point in the code.
  sleep_ms(50);
  gpio_put(config.reset_pin, false);

  // From 7.2.1 in the RFM69HCW datasheet, wait 10ms after reset before module
  // can be used.
  sleep_ms(10);

  // RFM69HCW seems to require about 50-60ns of data hold, so a baud rate of
  // one hz per 100ns should be fine.
  constexpr uint32_t baudrate = 1'000'000'000 / 1000;
  spi_init(spi, baudrate);
  spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  return RfmSpiDriver(spi, config.cs_pin, config.dio_pins);
}

RfmSpiDriver RfmSpiDriver::Create() {
  return Create({
      .spi = RFM_OOK_SPI,
      .miso_pin = RFM_OOK_SPI_MISO,
      .mosi_pin = RFM_OOK_SPI_MOSI,
      .sck_pin = RFM_OOK_SPI_SCK,
      .cs_pin = RFM_OOK_SPI_CS,
      .reset_pin = RFM_OOK_RESET,
  });
}

RfmSpiDriver::RfmSpiDriver(
    spi_inst_t* spi, int cs_pin, std::span<const int, 6> dio_pins)
    : spi_(spi), cs_pin_(cs_pin) {
  std::copy(dio_pins.begin(), dio_pins.end(), dio_pins_.begin());
}

uint8_t RfmSpiDriver::ReadRegister(uint8_t r) {
  assert(!(r & 0b1000'0000));
  uint8_t tx[2] = {r, 0};
  uint8_t rx[2];
  Transceive(tx, rx);
  return rx[1];
}

void RfmSpiDriver::Transmit(std::span<const uint8_t> tx) {
  assert(!tx.empty() > 0);
  // When transmitting, the first bit of the register should be 1 for write
  // access.
  assert(tx[0] & 0b1000'0000);
  gpio_put(cs_pin_, false);
  spi_write_read_blocking(spi_, tx.data(), nullptr, tx.size());
  gpio_put(cs_pin_, true);
}

void RfmSpiDriver::Transceive(
    std::span<const uint8_t> tx, std::span<uint8_t> rx) {
  assert(!tx.empty());
  assert(tx.size() == rx.size());
  gpio_put(cs_pin_, false);
  spi_write_read_blocking(spi_, tx.data(), rx.data(), tx.size());
  gpio_put(cs_pin_, true);
}

void RfmSpiDriver::WriteRegister(uint8_t reg, uint8_t value) {
  assert(!(reg & 0b1000'0000));
  std::array<uint8_t, 2> tx{
      static_cast<uint8_t>(reg | kRegisterWriteFlag), value};
  Transmit(tx);
}

constexpr uint8_t kRegOpMode = 0x01;
void RfmSpiDriver::SetMode(Mode m) {
  constexpr int kModeShift = 2;
  constexpr uint8_t kModeMask = 0b111 << kModeShift;

  if (m != kListen) {
    uint8_t value = ReadRegister(kRegOpMode);
    value = (value & ~kModeMask) | (m << kModeShift);
    WriteRegister(kRegOpMode, value);
  } else {
    // Not implemented yet.
  }
}

constexpr uint8_t kRegDataModul = 0x02;
void RfmSpiDriver::SetModulationType(ModulationType m) {
  constexpr int kModulationTypeShift = 3;
  constexpr uint8_t kModulationTypeMask = 0b11 << kModulationTypeShift;

  uint8_t value = ReadRegister(kRegDataModul);
  value = (value & ~kModulationTypeMask) | (m << kModulationTypeShift);
  WriteRegister(kRegDataModul, value);
}

void RfmSpiDriver::SetDataMode(DataMode m) {
  constexpr int kDataModeShift = 5;
  constexpr uint8_t kDataModeMask = 0b11 << kDataModeShift;

  uint8_t value = ReadRegister(kRegDataModul);
  value = (value & ~kDataModeMask) | (m << kDataModeShift);
  WriteRegister(kRegDataModul, value);
}

void RfmSpiDriver::SetPower(uint8_t power_level) {
  constexpr uint8_t kRegPaLevel = 0x11;
  WriteRegister(kRegPaLevel, power_level);
}

void RfmSpiDriver::SetCarrierFrequency(float freq_hz) {
  constexpr uint8_t kWriteFreqBaseReg = 0x07 | kRegisterWriteFlag;
  constexpr uint32_t kFxosc = 32'000'000.0;
  constexpr uint32_t kFstep = kFxosc / (1 << 19);
  const uint32_t num_steps = freq_hz / kFstep;
  printf("%0d\n", num_steps);

  const std::array<uint8_t, 4> freq_tx = {
      kWriteFreqBaseReg,
      static_cast<uint8_t>(num_steps >> 16),
      static_cast<uint8_t>(num_steps >> 8),
      static_cast<uint8_t>(num_steps),
  };
  Transmit(freq_tx);
}

uint8_t RfmSpiDriver::GetRegIrqFlags() {
  constexpr uint8_t kRegIrqFlags = 0x27;
  return ReadRegister(kRegIrqFlags);
}

}  // namespace jagsawning