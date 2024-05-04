#include "SomfyRemote.h"

#include "boards/pico_w.h"
#include "hardware/flash.h"
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
    // Let's just assume we have enough space for now.
    assert(PICO_FLASH_SIZE_BYTES - __flash_binary_end > FLASH_PAGE_SIZE * 10);

    constexpr std::string_view kFlashDataMagic = "JAGSAWNING_STORAGE_0000";
    
    // The first flash page will contain a magic string to identify the protocol
    // we're using, then one byte of 1s for each page we've used, up to 10.



  }

  /**
   * Get the next rolling code from the store. This should also increase the
   * rolling code and store it persistently.
   *
   * @return next rolling code
   */
  uint16_t nextCode() override { return 0; }

private:
};

void main_task() { PicoFlashRCS rcs; }

} // namespace jagsawning

extern "C" void main_task(void *) { jagsawning::main_task(); }