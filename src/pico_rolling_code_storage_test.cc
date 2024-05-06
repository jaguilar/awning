#include "pico_rolling_code_storage.h"

#include <Arduino.h>

#include <cstdio>

#include "pico/flash.h"
#include "pico/stdlib.h"

using jagsawning::PicoFlashRCS;

int main() {
  setup_default_uart();
  flash_safe_execute_core_init();

  uint16_t code = 0;
  {
    PicoFlashRCS rcs;
    printf("Resetting to zero\n");
    rcs.Reset(0);
    printf("Reading next code: %d\n", rcs.nextCode());
    printf("Reading next code: %d\n", rcs.nextCode());
    printf("Reading next code: %d\n", rcs.nextCode());
    printf("Manual reset to 1234\n");
    rcs.Reset(1234);
    printf("Reading next code: %d\n", rcs.nextCode());
    code = rcs.nextCode();
    printf("Reading next code: %d\n", code);
    printf("Destroying RCS\n");
  }

  {
    PicoFlashRCS rcs;
    printf("Recreated RCS, next code should be the code after %d\n", code);
    uint16_t code_after_reset = rcs.nextCode();
    printf("Code? %d\n", code_after_reset);
    if (code_after_reset != code + 1) {
      printf("FAIL\n");
      assert(false);
    }

    rcs.Reset(std::numeric_limits<uint16_t>::max() - 1);

    printf("Testing wrap-around behavior\n");
    uint16_t want = std::numeric_limits<uint16_t>::max() - 1;
    uint16_t got = rcs.nextCode();
    if (want != got) {
      printf("FAIL (want: %d got: %d)\n", want, got);
      assert(false);
    }
    ++want;
    got = rcs.nextCode();
    if (want != got) {
      printf("FAIL (want: %d got: %d)\n", want, got);
      assert(false);
    }
    ++want;
    got = rcs.nextCode();
    if (want != got || got != 0) {
      printf("FAIL (want: %d got: %d)\n", want, got);
      assert(false);
    }

    // Note: the previous value was zero. The flash should have been erased.
    // Recreating the RCS again should give us 1.
  }

  {
    PicoFlashRCS rcs;
    printf("Recreated RCS, next code should be 1\n");
    uint16_t got = rcs.nextCode();
    if (got != 1) {
      printf("FAIL (got: %d)\n", got);
      assert(false);
    }
  }
  printf("PASS\n");
  while (true) {
  }
}