//===-- runtime/numeric-input.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "numeric-input.h"
#include "flang/common/uint128.h"

namespace Fortran::runtime::io {

bool EditIntegerInput(
    IoStatementState &io, const DataEdit &edit, std::int64_t &n, int kind) {
  if (!edit.width.has_value() || !*edit.width) {
    // TODO pmk here and elsewhere: treat as handle-able errors, save IOMSG
    io.GetIoErrorHandler().Crash(
        "INTEGER input data edit descriptor '%c' must have nonzero width",
        edit.descriptor);
    return false;
  }
  std::size_t width{static_cast<std::size_t>(*edit.width)};
  std::size_t remaining{width};
  std::optional<char> next{io.NextInField(remaining)};
  while (next && *next == ' ') {
    next = io.NextInField(remaining);
  }
  common::UnsignedInt128 value;
  int base{10};
  switch (edit.descriptor) {
  case DataEdit::ListDirected:
  case 'G':
  case 'I': break;
  case 'B': base = 2; break;
  case 'O': base = 8; break;
  case 'Z': base = 16; break;
  default:
    io.GetIoErrorHandler().Crash(
        "Data edit descriptor '%c' may not be used with an INTEGER data item",
        edit.descriptor);
    return false;
  }
  while (next) {
    char ch{*next};
    next = io.NextInField(remaining);
    if (ch == ' ') {
      if (!(edit.modes.editingFlags & blankZero)) {
        continue;
      }
      ch = '0';
    }
    if (ch >= '0' && ch <= '9') {
      ch -= '0';
    } else if (ch >= 'A' && ch <= 'Z') {
      ch += 10 - 'A';
    } else if (ch >= 'a' && ch <= 'z') {
      ch += 10 - 'a';
    } else {
      io.GetIoErrorHandler().Crash(
          "Bad character '%c' in INTEGER input field", ch);
    }
    if (ch >= base) {
      io.GetIoErrorHandler().Crash("Invalid digit in INTEGER input field", ch);
    }
    value *= base;
    value += ch;
  }

  switch (kind) {
  case 1: reinterpret_cast<std::int8_t &>(n) = value.low(); break;
  case 2: reinterpret_cast<std::int16_t &>(n) = value.low(); break;
  case 4: reinterpret_cast<std::int32_t &>(n) = value.low(); break;
  case 8: reinterpret_cast<std::int64_t &>(n) = value.low(); break;
  case 16: reinterpret_cast<common::UnsignedInt128 &>(n) = value; break;
  default:
    io.GetIoErrorHandler().Crash("EditIntegerInput: bad INTEGER kind %d", kind);
    return false;
  }
  return true;
}
}
