/*
 * Minimal CUDA fatbin parser used by the DLSS-G PTX retargeter.
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ImDreamt
 * Copyright (c) 2026 mavismmg
 * Copyright (c) 2026 nefh
 *
 * Adapted for standalone C++17 use from nefh/MFGAmpereUnlock-RenoDx,
 * commit dd349cdbbae6525188e71fbf2e6d3c648be40db9,
 * src/addons/mfgunlock/fatbin.hpp. See the adjacent LICENSE.
 *
 * NVIDIA does not publish this container format as a stable ABI. Unknown
 * headers, entry layouts, or sizes are rejected rather than guessed. The
 * retargeter separately checks the selected PTX's compression flags.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace trp::ampere::fatbin {

inline constexpr std::uint32_t kMagic = 0xBA55ED50u;
inline constexpr std::size_t kHeaderBytes = 16;
inline constexpr std::size_t kMaxPtxBytes = 32 * 1024 * 1024;
inline constexpr std::size_t kMaxFatbinBytes = 64 * 1024 * 1024;

// The container uses little-endian fields. This retains the upstream parser's
// native-endian reads for the x86/x64 host targeted by this integration.
inline std::uint16_t ReadU16(const std::uint8_t* data) {
  std::uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

inline std::uint32_t ReadU32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

inline std::uint64_t ReadU64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

// Plain LZ4 block format. The optional mapping identifies the source literal
// responsible for one decoded byte. A second full decode is still required to
// prove that editing that literal does not affect any other output position.
// Source and destination must be valid, nonoverlapping buffers of the given sizes.
inline bool Lz4BlockDecompress(const std::uint8_t* src, std::size_t src_bytes,
                               std::uint8_t* dst, std::size_t dst_bytes,
                               std::size_t wanted = (std::numeric_limits<std::size_t>::max)(),
                               std::size_t* literal = nullptr) {
  std::size_t in = 0;
  std::size_t out = 0;

  if (literal != nullptr) *literal = (std::numeric_limits<std::size_t>::max)();
  if ((!src && src_bytes != 0) || (!dst && dst_bytes != 0)) return false;

  auto extend = [&](std::size_t& length) {
    if (length != 15) return true;

    unsigned int extra = 0;
    do {
      if (in == src_bytes) return false;
      extra = src[in++];
      if (length > dst_bytes || extra > dst_bytes - length) return false;
      length += extra;
    } while (extra == 255);

    return true;
  };

  while (in < src_bytes) {
    const unsigned int token = src[in++];
    std::size_t length = token >> 4;
    if (!extend(length) || length > src_bytes - in || length > dst_bytes - out) return false;

    if (literal != nullptr && wanted >= out && wanted - out < length) {
      *literal = in + wanted - out;
    }

    if (length != 0) std::memcpy(dst + out, src + in, length);
    in += length;
    out += length;

    if (in == src_bytes) return out == dst_bytes;
    if (src_bytes - in < 2) return false;

    const std::size_t distance = ReadU16(src + in);
    in += 2;
    if (distance == 0 || distance > out) return false;

    length = token & 15;
    if (!extend(length) || length > (std::numeric_limits<std::size_t>::max)() - 4) return false;
    length += 4;
    if (length > dst_bytes - out) return false;

    // LZ4 match copies may overlap. A byte-at-a-time copy is intentional here;
    // memcpy and memmove do not have the same semantics for this format.
    for (std::size_t i = 0; i < length; ++i) {
      if (i >= dst_bytes - out) return false;
      const std::size_t dst_index = out + i;
      if (distance > dst_index) return false;
      dst[dst_index] = dst[dst_index - distance];
    }
    out += length;
  }

  return out == dst_bytes;
}

struct Entry {
  std::size_t offset;
  std::size_t header_bytes;
  std::size_t payload_bytes;
  std::size_t compressed_bytes;
  std::uint16_t kind;
  std::uint32_t architecture;
  std::uint64_t flags;
  std::size_t unpacked_bytes;

  std::size_t PayloadOffset() const {
    return offset + header_bytes;
  }

  std::size_t End() const {
    return PayloadOffset() + payload_bytes;
  }
};

// The caller supplies readable input of size bytes. Outputs are usable only
// when true is returned. Parsing does not modify the input.
inline bool Parse(const std::uint8_t* bytes, std::size_t size,
                  std::vector<Entry>& entries, std::size_t& end) {
  entries.clear();
  end = 0;

  if (!bytes || size < kHeaderBytes || size > kMaxFatbinBytes ||
      ReadU32(bytes) != kMagic || ReadU16(bytes + 4) != 1 ||
      ReadU16(bytes + 6) != kHeaderBytes) {
    return false;
  }

  const std::uint64_t payload_bytes = ReadU64(bytes + 8);
  if (payload_bytes > size - kHeaderBytes) return false;

  end = kHeaderBytes + static_cast<std::size_t>(payload_bytes);
  std::size_t offset = kHeaderBytes;

  while (offset < end) {
    if (end - offset < 64 || entries.size() == 32) return false;

    const auto* entry = bytes + offset;
    const std::size_t header_bytes = ReadU32(entry + 4);
    const std::uint64_t stored_payload_bytes = ReadU64(entry + 8);
    const std::uint32_t compressed_bytes = ReadU32(entry + 16);
    const std::uint64_t unpacked_bytes = ReadU64(entry + 56);
    const std::uint16_t kind = ReadU16(entry);

    if ((kind != 1 && kind != 2) || ReadU16(entry + 2) != 0x101 ||
        header_bytes < 64 || header_bytes > 4096 || header_bytes % 8 != 0 ||
        stored_payload_bytes % 8 != 0 || header_bytes > end - offset ||
        stored_payload_bytes > end - offset - header_bytes ||
        compressed_bytes > stored_payload_bytes || unpacked_bytes > kMaxPtxBytes) {
      return false;
    }

    entries.push_back({
        offset,
        header_bytes,
        static_cast<std::size_t>(stored_payload_bytes),
        compressed_bytes,
        kind,
        ReadU32(entry + 28),
        ReadU64(entry + 40),
        static_cast<std::size_t>(unpacked_bytes),
    });
    offset = entries.back().End();
  }

  return offset == end && !entries.empty();
}

}  // namespace trp::ampere::fatbin
