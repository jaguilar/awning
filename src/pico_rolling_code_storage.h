#ifndef AWNING_PICO_ROLLING_CODE_STORAGE_H
#define AWNING_PICO_ROLLING_CODE_STORAGE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "RollingCodeStorage.h"
#include "boards/pico_w.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"

namespace jagsawning {

// The rolling code storage for the Pico uses two sectors of flash at the top of
// the address range. The number of zero bits in the flash is the next rolling
// code to return. This value is read once on startup and updated after each
// use.
class PicoFlashRCS : public RollingCodeStorage {
 public:
  using RollingCodeType = uint16_t;
  static constexpr int kCodeBitmapSectors =
      (static_cast<uint32_t>(std::numeric_limits<RollingCodeType>::max()) + 1) /
      (static_cast<uint32_t>(FLASH_SECTOR_SIZE) * 8);
  static constexpr std::uintptr_t kStorageBase =
      XIP_BASE + PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE * kCodeBitmapSectors;
  const int kBitsPerPage = 8 * FLASH_PAGE_SIZE;

  PicoFlashRCS();

  void Reset(uint16_t initial_code = 0);

  static void SetLeadingNZeroBits(std::span<uint8_t> s, int n);

  static uint16_t ReadCode();

  static void WriteCode(uint16_t code);

  // Reads the flash at memory address. (This looks just like a memory read,
  // thanks to XIP.)
  static std::span<const uint8_t> ReadFlash(
      std::uintptr_t address, std::size_t size);

  static void WriteFlash(
      std::uintptr_t memory_address, std::span<const uint8_t> page_data);

  // Erases flash sectors. The address must be aligned to a sector boundary and
  // the number of bytes must be a multiple of the sector size.
  static void EraseFlash(
      std::uintptr_t memory_address, std::ptrdiff_t num_bytes);

  /**
   * Get the next rolling code from the store. This should also increase the
   * rolling code and store it persistently.
   *
   * @return next rolling code
   */
  RollingCodeType nextCode() override;

 private:
  RollingCodeType code_ = 0;
};

}  // namespace jagsawning

#endif  // AWNING_PICO_ROLLING_CODE_STORAGE_H
