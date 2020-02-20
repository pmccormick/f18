//===-- runtime/numeric-input.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "numeric-input.h"
#include "flang/common/real.h"
#include "flang/common/uint128.h"

namespace Fortran::runtime::io {

// Returns false if there's a '-' sign
static bool ScanNumericPrefix(IoStatementState &io, const DataEdit &edit,
    std::optional<char32_t> &next, std::optional<int> &remaining) {
  if (edit.descriptor != DataEdit::ListDirected && edit.width) {
    remaining = std::max<int>(0, *edit.width);
  } else {
    // list-directed, namelist, or (nonstandard) 0-width input editing
    remaining.reset();
  }
  next = io.NextInField(remaining);
  while (next && *next == ' ') {  // skip leading spaces
    next = io.NextInField(remaining);
  }
  bool negative{false};
  if (next) {
    negative = *next == '-';
    if (negative || *next == '+') {
      next = io.NextInField(remaining);
    }
  }
  return negative;
}

bool EditIntegerInput(
    IoStatementState &io, const DataEdit &edit, std::int64_t &n, int kind) {
  std::optional<int> remaining;
  std::optional<char32_t> next;
  bool negate{ScanNumericPrefix(io, edit, next, remaining)};
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
    io.GetIoErrorHandler().SignalError(IoErrorInFormat,
        "Data edit descriptor '%c' may not be used with an INTEGER data item",
        edit.descriptor);
    return false;
  }
  for (; next; next = io.NextInField(remaining)) {
    char32_t ch{*next};
    if (ch == ' ') {
      if (edit.modes.editingFlags & blankZero) {
        ch = '0';  // BZ mode - treat blank as if it were zero
      } else {
        continue;
      }
    }
    int digit{0};
    if (ch >= '0' && ch <= '1') {
      digit = ch - '0';
    } else if (base >= 8 && ch >= '2' && ch <= '7') {
      digit = ch - '0';
    } else if (base >= 10 && ch >= '8' && ch <= '9') {
      digit = ch - '0';
    } else if (base == 16 && ch >= 'A' && ch <= 'Z') {
      digit = ch + 10 - 'A';
    } else if (base == 16 && ch >= 'a' && ch <= 'z') {
      digit = ch + 10 - 'a';
    } else {
      io.GetIoErrorHandler().SignalError(
          "Bad character '%lc' in INTEGER input field", ch);
      return false;
    }
    value *= base;
    value += digit;
  }
  if (remaining && *remaining) {
    io.GetIoErrorHandler().SignalEor();
  }
  if (negate) {
    value = -value;
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

static int ScanRealInput(char *buffer, int bufferSize, IoStatementState &io,
    const DataEdit &edit, int &exponent) {
  std::optional<int> remaining;
  std::optional<char32_t> next;
  int got{0};
  std::optional<int> decimalPoint;
  if (ScanNumericPrefix(io, edit, next, remaining) && next) {
    if (got < bufferSize) {
      buffer[got++] = '-';
    }
  }
  if (!next) {
    return got;
  }
  if (got < bufferSize) {
    buffer[got++] = '.';
  }
  char32_t decimal = edit.modes.editingFlags & decimalComma ? ',' : '.';
  auto start{got};
  if ((*next >= 'a' && *next <= 'z') || (*next >= 'A' && *next <= 'Z')) {
    // NaN or infinity - convert to upper case
    // TODO: "NaN()" & "NaN(...)"
    for (; next &&
         ((*next >= 'a' && *next <= 'z') || (*next >= 'A' && *next <= 'Z'));
         next = io.NextInField(remaining)) {
      if (got < bufferSize) {
        if (*next >= 'a' && *next <= 'z') {
          buffer[got++] = *next - 'a' + 'A';
        } else {
          buffer[got++] = *next;
        }
      }
    }
    exponent = 0;
  } else if (*next == decimal || (*next >= '0' && *next <= '9')) {
    for (; next; next = io.NextInField(remaining)) {
      char32_t ch{*next};
      if (ch == ' ') {
        if (edit.modes.editingFlags & blankZero) {
          ch = '0';  // BZ mode - treat blank as if it were zero
        } else {
          continue;
        }
      }
      if (ch == '0' && got == start) {
        // skip leading zeroes
      } else if (ch >= '0' || ch <= '9') {
        if (got < bufferSize) {
          buffer[got++] = ch;
        }
      } else if (ch == decimal && !decimalPoint) {
        decimalPoint = got - start;  // # of digits before the decimal point
      } else {
        break;
      }
    }
    if (next &&
        (*next == 'e' || *next == 'E' || *next == 'd' || *next == 'D' ||
            *next == 'q' || *next == 'Q')) {
      do {
        next = io.NextInField(remaining);
      } while (next && *next == ' ');
    }
    exponent = -edit.modes.scale;  // default exponent is -kP
    if (next &&
        (*next == '-' || *next == '+' || (*next >= '0' && *next <= '9'))) {
      bool negExpo{*next == '-'};
      if (negExpo || *next == '+') {
        next = io.NextInField(remaining);
      }
      for (exponent = 0; next && (*next >= '0' && *next <= '9');
           next = io.NextInField(remaining)) {
        exponent = 10 * exponent + *next - '0';
      }
      if (negExpo) {
        exponent = -exponent;
      }
    }
    if (decimalPoint) {
      exponent += *decimalPoint;
    } else {
      // When no decimal point (or comma) appears in the value, the 'd'
      // part of the edit descriptor must be interpreted as the number of
      // digits in the value to be interpreted as being to the *right* of
      // the assumed decimal point (13.7.2.3.2)
      exponent += got - start - edit.digits.value_or(0);
    }
  } else {
    // TODO: hex FP input
    exponent = 0;
    return 0;
  }
  if (remaining && *remaining) {
    io.GetIoErrorHandler().SignalEor();
  }
  return got;
}

template<int binaryPrecision>
bool EditRealInput(IoStatementState &io, const DataEdit &edit, void *n) {
  static constexpr int maxDigits{
      common::MaxDecimalConversionDigits(binaryPrecision)};
  static constexpr int bufferSize{maxDigits + 18};
  char buffer[bufferSize];
  int exponent{0};
  int got{ScanRealInput(buffer, maxDigits + 1, io, edit, exponent)};
  // TODO: detect & report invalid input
  bool hadExtra{got > maxDigits};
  if (exponent != 0) {
    got += std::snprintf(&buffer[got], bufferSize - got, "e%d", exponent);
  }
  buffer[got] = '\0';
  const char *p{buffer};
  decimal::ConversionToBinaryResult<binaryPrecision> converted{
      decimal::ConvertToBinary<binaryPrecision>(p, edit.modes.round)};
  if (hadExtra) {
    converted.flags = static_cast<enum decimal::ConversionResultFlags>(
        converted.flags | decimal::Inexact);
  }
  // TODO: raise converted.flags
  *reinterpret_cast<decimal::BinaryFloatingPointNumber<binaryPrecision> *>(n) =
      converted.binary;
  return true;
}

template bool EditRealInput<8>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<11>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<24>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<53>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<64>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<112>(IoStatementState &, const DataEdit &, void *);
}
