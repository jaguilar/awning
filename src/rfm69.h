#ifndef AWNING_RFM_OOK_H
#define AWNING_RFM_OOK_H

#include <expected>
#include <span>

#include "hardware/spi.h"

namespace jagsawning {

class RfmSpiDriver {
 public:
  struct RfmSpiDriverConfig {
    spi_inst_t* spi;
    int miso_pin;
    int mosi_pin;
    int sck_pin;
    int cs_pin;
    int reset_pin;
    std::array<int, 6> dio_pins = {-1, -1, -1, -1, -1, -1};
  };

  // Creates the driver with the settings provided in rfm_ook_config.h.
  static RfmSpiDriver Create();
  static RfmSpiDriver Create(const RfmSpiDriverConfig& config);

  enum Mode {
    kSleep = 0b000,
    kStandby = 0b001,
    kFrequencySynthesis = 0b010,
    kTransmit = 0b011,
    kReceive = 0b111,
    kListen,  // This value is not written directly to register.
  };
  void SetMode(Mode m);

  enum DataMode {
    kPacket = 0b00,
    kContinuousWithSynchronizer = 0b10,
    kContinuousWithoutSynchronizer = 0b11,
  };
  void SetDataMode(DataMode m);

  enum ModulationType {
    kFsk = 0b00,
    kOok = 0b01,
  };
  void SetModulationType(ModulationType m);

  enum PaLevel {
    kPa0On = 0b1000'0000,
    kPa1On = 0b0100'0000,
    kPa2On = 0b0010'0000,
  };
  void SetPower(uint8_t power_level);

  // Sets the carrier frequency in hz.
  void SetCarrierFrequency(float freq_hz);

  // Get status register. There are a few other flags but they don't seem useful
  // right now.
  enum RegIrqFlagValues {
    kModeReady = 0xb1000'0000,
    kRxReady = 0b0100'0000,
    kTxReady = 0b0010'0000,
    kPllLock = 0b0001'0000,
  };
  uint8_t GetRegIrqFlags();

 private:
  friend class RfmOokDriverTestFriend;

  RfmSpiDriver(spi_inst_t* spi, int cs_pin, std::span<const int, 6> data_pin);

  // Reads the value of a register.
  uint8_t ReadRegister(uint8_t reg);

  // Sets a register to a value.
  void WriteRegister(uint8_t reg, uint8_t value);

  // Sends tx.
  void Transmit(std::span<const uint8_t> tx);

  // Sends tx and receives into rx. rx must be the same size as tx.
  void Transceive(std::span<const uint8_t> tx, std::span<uint8_t> rx);

  static constexpr uint8_t kRegisterWriteFlag = 0b1000'0000;

  spi_inst_t* spi_;
  int cs_pin_;
  std::array<int, 6> dio_pins_;
};

}  // namespace jagsawning

#endif