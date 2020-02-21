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

static bool EditBOZInput(IoStatementState &io, const DataEdit &edit, void *n,
    int base, int totalBitSize) {
  std::optional<int> remaining;
  if (edit.width) {
    remaining = std::max(0, *edit.width);
  }
  std::optional<char32_t> next{io.NextInField(remaining)};
  while (next && *next == ' ') {  // skip leading spaces
    next = io.NextInField(remaining);
  }
  common::UnsignedInt128 value{0};
  for (; next; next = io.NextInField(remaining)) {
    char32_t ch{*next};
    if (ch == ' ') {
      continue;
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
          "Bad character '%lc' in B/O/Z input field", ch);
      return false;
    }
    value *= base;
    value += digit;
  }
  // TODO: check for overflow
  std::memcpy(n, &value, totalBitSize >> 3);
  return true;
}

// Returns false if there's a '-' sign
static bool ScanNumericPrefix(IoStatementState &io, const DataEdit &edit,
    std::optional<char32_t> &next, std::optional<int> &remaining) {
  if (edit.descriptor != DataEdit::ListDirected && edit.width) {
    remaining = std::max(0, *edit.width);
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
    IoStatementState &io, const DataEdit &edit, void *n, int kind) {
  RUNTIME_CHECK(io.GetIoErrorHandler(), kind >= 1 && !(kind & (kind - 1)));
  switch (edit.descriptor) {
  case DataEdit::ListDirected:
  case 'G':
  case 'I': break;
  case 'B': return EditBOZInput(io, edit, n, 2, kind << 3);
  case 'O': return EditBOZInput(io, edit, n, 8, kind << 3);
  case 'Z': return EditBOZInput(io, edit, n, 16, kind << 3);
  default:
    io.GetIoErrorHandler().SignalError(IostatErrorInFormat,
        "Data edit descriptor '%c' may not be used with an INTEGER data item",
        edit.descriptor);
    return false;
  }
  std::optional<int> remaining;
  std::optional<char32_t> next;
  bool negate{ScanNumericPrefix(io, edit, next, remaining)};
  common::UnsignedInt128 value;
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
    if (ch >= '0' && ch <= '9') {
      digit = ch - '0';
    } else {
      io.GetIoErrorHandler().SignalError(
          "Bad character '%lc' in INTEGER input field", ch);
      return false;
    }
    value *= 10;
    value += digit;
  }
  if (negate) {
    value = -value;
  }
  std::memcpy(n, &value, kind);
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
  if (!next) {  // empty field means zero
    if (got < bufferSize) {
      buffer[got++] = '0';
    }
    return got;
  }
  if (got < bufferSize) {
    buffer[got++] = '.';  // input field is normalized to a fraction
  }
  char32_t decimal = edit.modes.editingFlags & decimalComma ? ',' : '.';
  auto start{got};
  if ((*next >= 'a' && *next <= 'z') || (*next >= 'A' && *next <= 'Z')) {
    // NaN or infinity - convert to upper case
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
    if (next && *next == '(') {  // NaN(...)
      while (next && *next != ')') {
        next = io.NextInField(remaining);
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
        // omit leading zeroes
      } else if (ch >= '0' && ch <= '9') {
        if (got < bufferSize) {
          buffer[got++] = ch;
        }
      } else if (ch == decimal && !decimalPoint) {
        // the decimal point is *not* copied to the buffer
        decimalPoint = got - start;  // # of digits before the decimal point
      } else {
        break;
      }
    }
    if (got == start && got < bufferSize) {
      buffer[got++] = '0';  // all digits were zeroes
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
  if (remaining) {
    while (next && *next == ' ') {
      next = io.NextInField(remaining);
    }
    if (next) {
      return 0;  // error: unused nonblank character in fixed-width field
    }
  }
  return got;
}

template<int binaryPrecision>
bool EditCommonRealInput(IoStatementState &io, const DataEdit &edit, void *n) {
  static constexpr int maxDigits{
      common::MaxDecimalConversionDigits(binaryPrecision)};
  static constexpr int bufferSize{maxDigits + 18};
  char buffer[bufferSize];
  int exponent{0};
  int got{ScanRealInput(buffer, maxDigits + 2, io, edit, exponent)};
  if (got >= maxDigits + 2) {
    io.GetIoErrorHandler().Crash("EditRealInput: buffer was too small");
    return false;
  }
  if (got == 0) {
    io.GetIoErrorHandler().SignalError("Bad REAL input value");
    return false;
  }
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
  // TODO: raise converted.flags as exceptions?
  *reinterpret_cast<decimal::BinaryFloatingPointNumber<binaryPrecision> *>(n) =
      converted.binary;
  return true;
}

template<int binaryPrecision>
bool EditRealInput(IoStatementState &io, const DataEdit &edit, void *n) {
  switch (edit.descriptor) {
  case DataEdit::ListDirected:
  case 'F':
  case 'E':  // incl. EN, ES, & EX
  case 'D':
  case 'G': return EditCommonRealInput<binaryPrecision>(io, edit, n);
  case 'B':
    return EditBOZInput(
        io, edit, n, 2, common::BitsForBinaryPrecision(binaryPrecision));
  case 'O':
    return EditBOZInput(
        io, edit, n, 8, common::BitsForBinaryPrecision(binaryPrecision));
  case 'Z':
    return EditBOZInput(
        io, edit, n, 16, common::BitsForBinaryPrecision(binaryPrecision));
  default:
    io.GetIoErrorHandler().SignalError(IostatErrorInFormat,
        "Data edit descriptor '%c' may not be used for REAL input",
        edit.descriptor);
    return false;
  }
}

template bool EditRealInput<8>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<11>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<24>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<53>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<64>(IoStatementState &, const DataEdit &, void *);
template bool EditRealInput<112>(IoStatementState &, const DataEdit &, void *);
}
