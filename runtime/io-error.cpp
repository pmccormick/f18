//===-- runtime/io-error.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "io-error.h"
#include "magic-numbers.h"
#include "tools.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace Fortran::runtime::io {

void IoErrorHandler::Begin(const char *sourceFileName, int sourceLine) {
  flags_ = 0;
  ioStat_ = 0;
  ioMsg_.reset();
  SetLocation(sourceFileName, sourceLine);
}

static const char *FortranErrorString(int iostat) {
  switch (iostat) {
  case IoErrorEnd: return "End of file during input";
  case IoErrorEor: return "End of record during non-advancing input";
  case IoErrorUnflushable: return "FLUSH not possible";
  case IoErrorInquireInternal: return "INQUIRE on internal unit";
  case IoErrorRecordWriteOverrun:
    return "Excessive output to fixed-size record";
  case IoErrorRecordReadOverrun:
    return "Excessive input from fixed-size record";
  case IoErrorInternalWriteOverrun:
    return "Internal write overran available records";
  case IoErrorInKeyword: return "Bad keyword argument value";
  case IoErrorGeneric:
    return "I/O error";  // dummy value, there's always a message
  default: return nullptr;
  }
}

void IoErrorHandler::SignalError(int iostatOrErrno, const char *msg, ...) {
  if (iostatOrErrno == IoErrorEnd) {
    SignalEnd();
  } else if (iostatOrErrno == IoErrorEor) {
    SignalEor();
  } else if (iostatOrErrno != 0) {
    if (flags_ & (hasIoStat | hasErr)) {
      if (ioStat_ <= 0) {
        ioStat_ = iostatOrErrno;  // priority over END=/EOR=
        if (msg && (flags_ & hasIoMsg)) {
          char buffer[256];
          va_list ap;
          va_start(ap, msg);
          std::vsnprintf(buffer, sizeof buffer, msg, ap);
          ioMsg_ = SaveDefaultCharacter(buffer, std::strlen(buffer) + 1, *this);
        }
      }
    } else if (const char *errstr{FortranErrorString(iostatOrErrno)}) {
      Crash(errstr);
    } else {
      Crash("I/O error (errno=%d): %s", iostatOrErrno,
          std::strerror(iostatOrErrno));
    }
  }
}

void IoErrorHandler::SignalError(int iostatOrErrno) {
  SignalError(iostatOrErrno, nullptr);
}

void IoErrorHandler::SignalErrno() { SignalError(errno); }

void IoErrorHandler::SignalEnd() {
  if (flags_ & hasEnd) {
    if (!ioStat_ || ioStat_ < IoErrorEnd) {
      ioStat_ = IoErrorEnd;
    }
  } else {
    SignalError(IoErrorEnd);
  }
}

void IoErrorHandler::SignalEor() {
  if (flags_ & hasEor) {
    if (!ioStat_ || ioStat_ < IoErrorEor) {
      ioStat_ = IoErrorEor;  // least priority
    }
  } else {
    SignalError(IoErrorEor);
  }
}

bool IoErrorHandler::GetIoMsg(char *buffer, std::size_t bufferLength) {
  const char *msg{ioMsg_.get()};
  if (!msg) {
    msg = FortranErrorString(ioStat_);
  }
  if (msg) {
    std::size_t len{std::strlen(msg)};
    std::memcpy(buffer, msg, std::max(bufferLength, len));
    if (bufferLength > len) {
      std::memset(buffer + len, ' ', bufferLength - len);
    }
    return true;
  }
  return ::strerror_r(ioStat_, buffer, bufferLength) == 0;
}
}
