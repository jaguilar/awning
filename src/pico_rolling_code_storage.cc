#include "pico_rolling_code_storage.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include "boards/pico_w.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/platform.h"

extern char __flash_binary_end;

namespace jagsawning {

std::uintptr_t FlashBinaryEndPtr() {
  return reinterpret_cast<std::uintptr_t>(&__flash_binary_end);
}

PicoFlashRCS::PicoFlashRCS() {
  // Let's just assert we have enough space for now.
  assert(FlashBinaryEndPtr() < kStorageBase);
  code_ = ReadCode();
}

void PicoFlashRCS::Reset(uint16_t initial_code) {
  if (initial_code < code_) {
    // Only need to erase if we're going down.
    EraseFlash(kStorageBase, kCodeBitmapSectors * FLASH_SECTOR_SIZE);
  }
  if (initial_code != 0) {
    std::uintptr_t page_offset = kStorageBase;
    std::array<uint8_t, FLASH_PAGE_SIZE> data;
    data.fill(0);
    uint16_t remaining_code_points = initial_code;
    for (; remaining_code_points > kBitsPerPage;
         remaining_code_points -= kBitsPerPage,
         page_offset += FLASH_PAGE_SIZE) {
      // Write a page full of zeroes to the leading pages.
      WriteFlash(page_offset, data);
    }
    data.fill(0xff);
    SetLeadingNZeroBits(data, remaining_code_points);
    WriteFlash(page_offset, data);
  }
  code_ = initial_code;
}

void PicoFlashRCS::SetLeadingNZeroBits(std::span<uint8_t> s, int n) {
  while (n >= 8) {
    s[0] = 0;
    s = s.subspan(1);
    n -= 8;
  }
  s[0] = 0xffu >> n;
}

uint16_t PicoFlashRCS::ReadCode() {
  uint16_t count = 0;
  for (uint32_t byte :
       ReadFlash(kStorageBase, kCodeBitmapSectors * FLASH_SECTOR_SIZE)) {
    constexpr int kMaxBits = sizeof(byte) * 8;
    if (byte == std::numeric_limits<decltype(byte)>::max()) {
      // No more bits will be zero, since we zero the bits in sequence.
      return count;
    }
    count += kMaxBits - std::popcount(byte);
  }
  return count;
}

void PicoFlashRCS::WriteCode(uint16_t code) {
  if (code == 0) {
    // We write zero by erasing the flash.
    EraseFlash(kStorageBase, kCodeBitmapSectors * FLASH_SECTOR_SIZE);
    return;
  }
  const int kBitsPerPage = 8 * FLASH_PAGE_SIZE;
  std::uintptr_t page_offset = kStorageBase;
  // Skip any sectors that would have been written on previous code
  // increments. This way we only write one sector per update.
  for (; code > kBitsPerPage;
       page_offset += FLASH_PAGE_SIZE, code -= kBitsPerPage) {
  }

  // Construct the page contents. There should be as zero bits as there are
  // code increments remaining.
  std::array<uint8_t, FLASH_PAGE_SIZE> data;
  data.fill(0xff);
  SetLeadingNZeroBits(data, code);
  WriteFlash(page_offset, data);
}

std::span<const uint8_t> PicoFlashRCS::ReadFlash(
    std::uintptr_t address, std::size_t size) {
  uint8_t *const ptr = reinterpret_cast<uint8_t *>(address);
  return std::span<const uint8_t>(ptr, ptr + size);
}

template <typename F>
static void WithSingleCoreAndInterruptsSuppressed(F &&f) {
  void *fptr = &f;
  flash_safe_execute(
      +[](void *fptr) {
        auto f = reinterpret_cast<F *>(fptr);
        (*f)();
      },
      fptr,
      std::numeric_limits<uint32_t>::max());
}

void PicoFlashRCS::WriteFlash(
    std::uintptr_t memory_address, std::span<const uint8_t> page_data) {
  const std::uintptr_t flash_offset = memory_address - XIP_BASE;
  assert(flash_offset % FLASH_PAGE_SIZE == 0);
  assert(page_data.size() % FLASH_PAGE_SIZE == 0);
  assert(flash_offset + page_data.size() <= PICO_FLASH_SIZE_BYTES);
#ifndef NDEBUG
  // Check that all bits which are 0 in old are also 0 in data. If not,
  // we're trying to set a bit to 1. That is not possible with flash.
  std::span<const uint8_t> old = ReadFlash(memory_address, page_data.size());
  for (int i = 0; i < old.size(); ++i) {
    const unsigned char old_zero_bits = ~old[i];
    const unsigned char new_one_bits = page_data[i];
    if (old_zero_bits & new_one_bits) {
      panic(
          "at memory offset %d, trying to set a bit to 1 which is 0 in flash "
          "(old: %02x, new: %02x)",
          memory_address + i,
          old[i],
          page_data[i]);
    }
  }
#endif
  printf(
      "preparing to write flash: %08x[%d]\n", flash_offset, page_data.size());
  WithSingleCoreAndInterruptsSuppressed([&] {
    flash_range_program(flash_offset, page_data.data(), page_data.size());
  });
}

void PicoFlashRCS::EraseFlash(
    std::uintptr_t memory_address, std::ptrdiff_t num_bytes) {
  const std::uintptr_t flash_offset = memory_address - XIP_BASE;
  assert(flash_offset % FLASH_SECTOR_SIZE == 0);
  assert(num_bytes % FLASH_SECTOR_SIZE == 0);
  printf("preparing to erase flash: %p[%d]\n", memory_address, num_bytes);
  WithSingleCoreAndInterruptsSuppressed(
      [&] { flash_range_erase(flash_offset, num_bytes); });
}

PicoFlashRCS::RollingCodeType PicoFlashRCS::nextCode() {
  RollingCodeType code = code_;
  ++code_;
  WriteCode(code_);
  return code;
}

}  // namespace jagsawning