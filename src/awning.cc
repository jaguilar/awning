#include "SomfyRemote.h"

#include "boards/pico_w.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "portmacro.h"
#include <cstdint>
#include <span>
#include <string_view>

extern uint32_t __flash_binary_end;

namespace jagsawning {

class PicoFlashRCS : public RollingCodeStorage {
public:
  // We'll use up to 10 pages of flash for the rolling code storage, starting
  // from the last page and working our way down. We won't use a page if it
  // includes __flash_binary_end. We don't anticipate this binary growing to
  // take up more than a small fraction of the total flash (it's 720kB as of
  // this writing and the Pico has 2MB of flash).
  PicoFlashRCS() {
    // Let's just assert we have enough space for now.
    assert(PICO_FLASH_SIZE_BYTES - __flash_binary_end > FLASH_PAGE_SIZE * 10);

    constexpr std::string_view kFlashDataMagic = "JAGSAWNING_STORAGE_0000";
    
    // The first flash page will contain a magic string to identify the protocol
    // we're using, then one byte of 1s for each page we've used, up to 10.



  }

  // Reads the flash at memory address. (This is just a memory read, thanks to
  // XIP.)
  static std::span<uint8_t> ReadFlash(std::intptr_t address, std::size_t size) {
    return std::span<uint8_t>(reinterpret_cast<uint8_t *>(address), size);
  }

  static void WriteFlash(std::intptr_t address, std::span<uint8_t> data) {
    static std::intptr_t flash_offset = address - XIP_BASE;
    assert(flash_offset % FLASH_PAGE_SIZE == 0);
    assert(data.size() % FLASH_PAGE_SIZE == 0);
    assert(flash_offset + data.size() < PICO_FLASH_SIZE_BYTES);
#ifndef NDEBUG
    // Check that all bits which are 0 in old are also 0 in data. If not,
    // we're trying to set a bit to 1. That is not possible with flash.
    std::string_view old = ReadFlash(address, data.size());
    for (int i = 0; i < old.size(); ++i) {
      const unsigned char old_zero_bits = ~old[i];
      const unsigned char new_one_bits = data[i];
      if (old_zero_bits & new_one_bits) {
        panic(
            "at memory offset %d, trying to set a bit to 1 which is 0 in flash "
            "(old: %02x, new: %02x)",
            address + i, old[i], data[i]);
      }
    }
#endif
    vTaskEnterCritical();
    multicore_lockout_start_blocking();
    flash_range_program(flash_offset, data.data(), data.size());
    multicore_lockout_end_blocking();
    vTaskExitCritical();
  }

  static void EraseFlash(std::intptr_5 address, )

      /**
       * Get the next rolling code from the store. This should also increase the
       * rolling code and store it persistently.
       *
       * @return next rolling code
       */
      uint16_t nextCode() override {
    return 0;
  }

private:
};

void main_task() { PicoFlashRCS rcs; }

} // namespace jagsawning

extern "C" void main_task(void *) { jagsawning::main_task(); }