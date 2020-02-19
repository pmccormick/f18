//===-- runtime/unit.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// Fortran external I/O units

#ifndef FORTRAN_RUNTIME_IO_UNIT_H_
#define FORTRAN_RUNTIME_IO_UNIT_H_

#include "buffer.h"
#include "connection.h"
#include "file.h"
#include "format.h"
#include "io-error.h"
#include "io-stmt.h"
#include "lock.h"
#include "memory.h"
#include "terminator.h"
#include <cstdlib>
#include <cstring>
#include <optional>
#include <variant>

namespace Fortran::runtime::io {

class UnitMap;

class ExternalFileUnit : public ConnectionState,
                         public OpenFile,
                         public FileFrame<ExternalFileUnit> {
public:
  explicit ExternalFileUnit(int unitNumber) : unitNumber_{unitNumber} {}
  int unitNumber() const { return unitNumber_; }

  static ExternalFileUnit *LookUp(int unit);
  static ExternalFileUnit &LookUpOrCrash(int unit, const Terminator &);
  static ExternalFileUnit &LookUpOrCreate(
      int unit, const Terminator &, bool *wasExtant = nullptr);
  static ExternalFileUnit *LookUpForClose(int unit);
  static int NewUnit(const Terminator &);
  static void CloseAll(IoErrorHandler &);

  void OpenUnit(OpenStatus, Position, OwningPtr<char> &&path,
      std::size_t pathLength, IoErrorHandler &);
  void CloseUnit(CloseStatus, IoErrorHandler &);
  void DestroyClosed();

  template<typename A, typename... X>
  IoStatementState &BeginIoStatement(X &&... xs) {
    // TODO: Child data transfer statements vs. locking
    lock_.Take();  // dropped in EndIoStatement()
    A &state{u_.emplace<A>(std::forward<X>(xs)...)};
    if constexpr (!std::is_same_v<A, OpenStatementState>) {
      state.mutableModes() = ConnectionState::modes;
    }
    io_.emplace(state);
    return *io_;
  }

  bool Emit(const char *, std::size_t bytes, IoErrorHandler &);
  std::optional<char32_t> NextChar(IoErrorHandler &);
  void SetLeftTabLimit();
  bool AdvanceRecord(IoErrorHandler &);
  bool HandleAbsolutePosition(std::int64_t, IoErrorHandler &);
  bool HandleRelativePosition(std::int64_t, IoErrorHandler &);

  void FlushIfTerminal(IoErrorHandler &);
  void EndIoStatement();

private:
  static UnitMap &GetUnitMap();
  bool SetPositionInRecord(std::int64_t, IoErrorHandler &);
  void NextSequentialUnformattedInputRecord(IoErrorHandler &);
  void NextSequentialFormattedInputRecord(IoErrorHandler &);

  int unitNumber_{-1};
  bool isReading_{false};

  Lock lock_;
  // When an I/O statement is in progress on this unit, holds its state.
  std::variant<std::monostate, OpenStatementState, CloseStatementState,
      ExternalFormattedIoStatementState<false>,
      ExternalFormattedIoStatementState<true>,
      ExternalListIoStatementState<false>, ExternalListIoStatementState<true>,
      UnformattedIoStatementState<false>, UnformattedIoStatementState<true>>
      u_;
  // Points to the active alternative, if any, in u_, for use as a Cookie
  std::optional<IoStatementState> io_;

  static UnitMap *unitMap_;
};

}
#endif  // FORTRAN_RUNTIME_IO_UNIT_H_
