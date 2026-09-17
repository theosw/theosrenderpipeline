/*
 * Pure SM89-to-SM86 PTX planning for known DLSS-G fatbin layouts.
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 ImDreamt
 * Copyright (c) 2026 mavismmg
 * Copyright (c) 2026 nefh
 *
 * Adapted for standalone C++17 use from nefh/MFGAmpereUnlock-RenoDx,
 * commit dd349cdbbae6525188e71fbf2e6d3c648be40db9,
 * src/addons/mfgunlock/ptx_retarget.hpp. See the adjacent LICENSE.
 *
 * Only a verified `.target sm_89` directive is rewritten to SM86, and only
 * when the changed LZ4 byte is an independent literal. A second decode verifies
 * that no other PTX byte changed. This does not translate unsupported GPU
 * instructions, compile PTX, load a DLL, or publish memory to a provider.
 */
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "fatbin.hpp"

namespace trp::ampere {

enum class Status {
  Unchanged,
  Retargeted,
  Rejected,
};

struct Plan {
  std::vector<std::uint8_t> replacement;
  // Offset of the selected compressed PTX payload and its decompressed size.
  std::size_t ptx_offset = 0;
  std::size_t ptx_bytes = 0;
  std::size_t hidden_cubins = 0;
};

namespace internal {

inline std::string_view Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r");
  if (first == value.npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

// Preserve byte positions while blanking comments and strings. This prevents a
// filename, quoted string, or comment containing "sm_89" from being retargeted.
inline bool FindTargetDigits(const std::vector<std::uint8_t>& raw, std::size_t& digit) {
  const auto nul = std::find(raw.begin(), raw.end(), 0);
  if (nul == raw.end()) return false;

  std::string text(raw.begin(), nul);
  bool block_comment = false;
  bool quoted = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    if (block_comment) {
      if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
        text[i] = ' ';
        text[i + 1] = ' ';
        ++i;
        block_comment = false;
      } else if (text[i] != '\n') {
        text[i] = ' ';
      }
      continue;
    }

    if (quoted) {
      const char c = text[i];
      text[i] = ' ';
      if (c == '\\' && i + 1 < text.size()) {
        text[++i] = ' ';
      } else if (c == '"') {
        quoted = false;
      } else if (c == '\n') {
        return false;
      }
      continue;
    }

    if (text[i] == '"') {
      text[i] = ' ';
      quoted = true;
    } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      text[i] = ' ';
      text[i + 1] = ' ';
      ++i;
      block_comment = true;
    } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      for (; i < text.size() && text[i] != '\n'; ++i) text[i] = ' ';
    }
  }

  if (block_comment || quoted) return false;

  std::size_t targets = 0;
  std::size_t versions = 0;
  std::size_t addresses = 0;

  for (std::size_t offset = 0; offset < text.size();) {
    const std::size_t newline = text.find('\n', offset);
    const std::size_t end = newline == text.npos ? text.size() : newline;
    const auto line = Trim(std::string_view(text).substr(offset, end - offset));

    const auto is_directive = [&](std::string_view name) {
      return line.size() > name.size() && line.compare(0, name.size(), name) == 0 &&
             (line[name.size()] == ' ' || line[name.size()] == '\t');
    };

    if (is_directive(".target")) {
      const auto target = Trim(line.substr(7));
      if (target != "sm_89" || ++targets != 1) return false;
      digit = static_cast<std::size_t>(target.data() - text.data()) + 3;
    } else if (is_directive(".version")) {
      const auto version = Trim(line.substr(8));
      if (version.size() != 3 || version[0] != '8' || version[1] != '.' ||
          version[2] < '0' || version[2] > '7' || ++versions != 1) {
        return false;
      }
    } else if (is_directive(".address_size")) {
      if (Trim(line.substr(13)) != "64" || ++addresses != 1) return false;
    }

    offset = end + 1;
  }

  // Keep the audited upstream SM86 rejection list. This structural check is
  // not a complete PTX semantic validator or evidence of GPU compatibility.
  constexpr std::string_view kUnsupported[] = {
      "wgmma.",
      "tcgen05.",
      "tensormap.",
      "cp.async.bulk",
      ".e4m3",
      ".e5m2",
      ".e2m1",
  };
  for (const auto token : kUnsupported) {
    if (text.find(token) != text.npos) return false;
  }

  return targets == 1 && versions == 1 && addresses == 1 &&
         text.find(".entry") != text.npos;
}

}  // namespace internal

// Input must remain readable for the supplied size throughout the call.
// Retargeted returns a private replacement through the original fatbin's end;
// hidden cubin bytes remain allocated but lie beyond its new visible length.
// Trailing bytes outside the original outer payload are not copied.
// Unchanged/Rejected clear plan; reason is populated only for Rejected.
// Allocation exceptions propagate; callers must use plan only after Retargeted.
inline Status Retarget(const std::uint8_t* bytes, std::size_t size,
                       Plan& plan, std::string& reason) {
  reason.clear();

  std::vector<fatbin::Entry> entries;
  std::size_t end = 0;
  const auto reject = [&](const char* message) {
    plan = {};
    reason = message;
    return Status::Rejected;
  };

  if (!fatbin::Parse(bytes, size, entries, end)) return reject("invalid fatbin bounds/layout");

  const auto sm89 = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
    return entry.architecture == 89;
  });
  if (sm89 == entries.end()) {
    plan = {};
    return Status::Unchanged;
  }

  // Only these two entry orders were reviewed upstream. A new provider layout
  // must be reviewed as a complete selectable image set.
  const bool single = entries.size() == 1 && entries[0].kind == 1 &&
                      entries[0].architecture == 89;
  const bool mixed = entries.size() == 3 && entries[0].kind == 1 &&
                     entries[0].architecture == 120 && entries[1].kind == 1 &&
                     entries[1].architecture == 89 && entries[2].kind == 2 &&
                     entries[2].architecture == 89;
  if (!single && !mixed) return reject("unreviewed fatbin image ordering");

  const auto& entry = entries[mixed ? 1 : 0];
  if (entry.flags != 0x2041 || entry.compressed_bytes == 0 || entry.unpacked_bytes == 0) {
    return reject("unreviewed PTX compression flags");
  }

  std::vector<std::uint8_t> raw(entry.unpacked_bytes);
  std::vector<std::uint8_t> check(entry.unpacked_bytes);
  const auto* compressed = bytes + entry.PayloadOffset();
  if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, raw.data(), raw.size())) {
    return reject("invalid LZ4 block");
  }

  std::size_t target_digit = 0;
  if (!internal::FindTargetDigits(raw, target_digit))
    return reject("unreviewed PTX header/instructions");

  std::vector<std::uint8_t> replacement(bytes, bytes + end);
  constexpr std::uint32_t target_sm = 86;
  constexpr std::uint8_t digits[] = {'8', '6'};
  for (std::size_t i = 0; i < 2; ++i) {
    if (raw[target_digit + i] == digits[i]) continue;
    std::size_t literal = 0;
    if (!fatbin::Lz4BlockDecompress(compressed, entry.compressed_bytes, check.data(), check.size(),
                                    target_digit + i, &literal) ||
        literal >= entry.compressed_bytes || compressed[literal] != raw[target_digit + i]) {
      return reject("target is not an independent LZ4 literal");
    }
    replacement[entry.PayloadOffset() + literal] = digits[i];
  }
  if (!fatbin::Lz4BlockDecompress(replacement.data() + entry.PayloadOffset(),
                                  entry.compressed_bytes, check.data(), check.size())) {
    return reject("edited LZ4 block invalid");
  }

  raw[target_digit] = digits[0];
  raw[target_digit + 1] = digits[1];
  if (raw != check) return reject("target literal is shared with another output byte");

  const std::uint64_t visible_bytes = entry.End() - fatbin::kHeaderBytes;
  std::memcpy(replacement.data() + entry.offset + 28, &target_sm, sizeof(target_sm));
  std::memcpy(replacement.data() + 8, &visible_bytes, sizeof(visible_bytes));

  plan.replacement = std::move(replacement);
  plan.ptx_offset = entry.PayloadOffset();
  plan.ptx_bytes = entry.unpacked_bytes;
  plan.hidden_cubins = mixed ? 1 : 0;
  return Status::Retargeted;
}

}  // namespace trp::ampere
