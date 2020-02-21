//===-- runtime/unit.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "unit.h"
#include "io-error.h"
#include "unit-map.h"

namespace Fortran::runtime::io {

UnitMap *ExternalFileUnit::unitMap_{nullptr};
static ExternalFileUnit *defaultOutput{nullptr};

void FlushOutputOnCrash(const Terminator &terminator) {
  if (defaultOutput) {
    IoErrorHandler handler{terminator};
    handler.HasIoStat();  // prevent nested crash if flush has error
    defaultOutput->Flush(handler);
  }
}

ExternalFileUnit *ExternalFileUnit::LookUp(int unit) {
  return GetUnitMap().LookUp(unit);
}

ExternalFileUnit &ExternalFileUnit::LookUpOrCrash(
    int unit, const Terminator &terminator) {
  ExternalFileUnit *file{LookUp(unit)};
  if (!file) {
    terminator.Crash("Not an open I/O unit number: %d", unit);
  }
  return *file;
}

ExternalFileUnit &ExternalFileUnit::LookUpOrCreate(
    int unit, const Terminator &terminator, bool *wasExtant) {
  return GetUnitMap().LookUpOrCreate(unit, terminator, wasExtant);
}

ExternalFileUnit *ExternalFileUnit::LookUpForClose(int unit) {
  return GetUnitMap().LookUpForClose(unit);
}

int ExternalFileUnit::NewUnit(const Terminator &terminator) {
  return GetUnitMap().NewUnit(terminator).unitNumber();
}

void ExternalFileUnit::OpenUnit(OpenStatus status, Position position,
    OwningPtr<char> &&newPath, std::size_t newPathLength,
    IoErrorHandler &handler) {
  if (IsOpen()) {
    if (status == OpenStatus::Old &&
        (!newPath.get() ||
            (path() && pathLength() == newPathLength &&
                std::memcmp(path(), newPath.get(), newPathLength) == 0))) {
      // OPEN of existing unit, STATUS='OLD', not new FILE=
      newPath.reset();
      return;
    }
    // Otherwise, OPEN on open unit with new FILE= implies CLOSE
    Flush(handler);
    Close(CloseStatus::Keep, handler);
  }
  set_path(std::move(newPath), newPathLength);
  Open(status, position, handler);
}

void ExternalFileUnit::CloseUnit(CloseStatus status, IoErrorHandler &handler) {
  Flush(handler);
  Close(status, handler);
}

void ExternalFileUnit::DestroyClosed() {
  GetUnitMap().DestroyClosed(*this);  // destroys *this
}

UnitMap &ExternalFileUnit::GetUnitMap() {
  if (unitMap_) {
    return *unitMap_;
  }
  Terminator terminator{__FILE__, __LINE__};
  unitMap_ = &New<UnitMap>{}(terminator);
  ExternalFileUnit &out{ExternalFileUnit::LookUpOrCreate(6, terminator)};
  out.Predefine(1);
  out.set_mayRead(false);
  out.set_mayWrite(true);
  out.set_mayPosition(false);
  defaultOutput = &out;
  ExternalFileUnit &in{ExternalFileUnit::LookUpOrCreate(5, terminator)};
  in.Predefine(0);
  in.set_mayRead(true);
  in.set_mayWrite(false);
  in.set_mayPosition(false);
  // TODO: Set UTF-8 mode from the environment
  return *unitMap_;
}

void ExternalFileUnit::CloseAll(IoErrorHandler &handler) {
  if (unitMap_) {
    defaultOutput = nullptr;
    unitMap_->CloseAll(handler);
    FreeMemoryAndNullify(unitMap_);
  }
}

bool ExternalFileUnit::Emit(
    const char *data, std::size_t bytes, IoErrorHandler &handler) {
  auto furthestAfter{std::max(furthestPositionInRecord,
      positionInRecord + static_cast<std::int64_t>(bytes))};
  if (furthestAfter > recordLength.value_or(furthestAfter)) {
    handler.SignalError(IostatRecordWriteOverrun);
    return false;
  }
  WriteFrame(offsetInFile, furthestAfter, handler);
  std::memcpy(Frame() + positionInRecord, data, bytes);
  positionInRecord += bytes;
  furthestPositionInRecord = furthestAfter;
  return true;
}

std::optional<char32_t> ExternalFileUnit::NextChar(IoErrorHandler &handler) {
  isReading_ = true;  // TODO: manage transitions
  if (isUnformatted) {
    handler.Crash("NextChar() called for unformatted input");
    return std::nullopt;
  }
  std::size_t chunk{256};  // for stream input
  if (recordLength.has_value()) {
    if (positionInRecord >= *recordLength) {
      if (nonAdvancing) {
        handler.SignalEor();
      } else {
        handler.SignalError(IostatRecordReadOverrun);
      }
      return std::nullopt;
    }
    chunk = *recordLength - positionInRecord;
  }
  auto got{ReadFrame(offsetInFile + positionInRecord, chunk, handler)};
  if (got <= 0) {
    return std::nullopt;
  }
  const char *frame{Frame()};
  if (isUTF8) {
    // TODO: UTF-8 decoding
  }
  return *frame;
}

void ExternalFileUnit::SetLeftTabLimit() {
  leftTabLimit = furthestPositionInRecord;
  positionInRecord = furthestPositionInRecord;
}

bool ExternalFileUnit::AdvanceRecord(IoErrorHandler &handler) {
  bool ok{true};
  if (isReading_) {
    if (access == Access::Sequential) {
      if (isUnformatted) {
        NextSequentialUnformattedInputRecord(handler);
      } else {
        NextSequentialFormattedInputRecord(handler);
      }
    }
  } else if (!isUnformatted) {
    if (recordLength.has_value()) {
      // fill fixed-size record
      if (furthestPositionInRecord < *recordLength) {
        WriteFrame(offsetInFile, *recordLength, handler);
        std::memset(Frame() + furthestPositionInRecord, ' ',
            *recordLength - furthestPositionInRecord);
      }
    } else {
      positionInRecord = furthestPositionInRecord + 1;
      ok &= Emit("\n", 1, handler);  // TODO: Windows CR+LF
      offsetInFile += furthestPositionInRecord;
    }
  }
  ++currentRecordNumber;
  positionInRecord = 0;
  furthestPositionInRecord = 0;
  leftTabLimit.reset();
  return ok;
}

void ExternalFileUnit::FlushIfTerminal(IoErrorHandler &handler) {
  if (isTerminal()) {
    Flush(handler);
  }
}

void ExternalFileUnit::EndIoStatement() {
  io_.reset();
  u_.emplace<std::monostate>();
  lock_.Drop();
}

void ExternalFileUnit::NextSequentialUnformattedInputRecord(
    IoErrorHandler &handler) {
  std::int32_t header{0}, footer{0};
  // Retain previous footer (if any) in frame for more efficient BACKSPACE
  std::size_t retain{sizeof header};
  if (recordLength.has_value()) {
    // not first record - advance to next
    ++currentRecordNumber;
    if (endfileRecordNumber && currentRecordNumber >= *endfileRecordNumber) {
      handler.SignalEnd();
      return;
    }
    offsetInFile += *recordLength + 2 * sizeof header;
  } else {
    retain = 0;
  }
  std::size_t need{retain + sizeof header};
  std::size_t got{ReadFrame(offsetInFile - retain, need, handler)};
  const char *error{nullptr};
  if (got < need) {
    if (got == retain) {
      handler.SignalEnd();
    } else {
      error = "Unformatted sequential file input failed at record #%jd (file "
              "offset %jd): truncated record header";
    }
  } else {
    std::memcpy(&header, Frame() + retain, sizeof header);
    need = retain + header + 2 * sizeof header;
    got = ReadFrame(
        offsetInFile - retain, need + sizeof header /* next one */, handler);
    if (got < need) {
      error = "Unformatted sequential file input failed at record #%jd (file "
              "offset %jd): hit EOF reading record with length %jd bytes";
    } else {
      const char *start{Frame() + retain + sizeof header};
      std::memcpy(&footer, start + header, sizeof footer);
      if (footer != header) {
        error = "Unformatted sequential file input failed at record #%jd (file "
                "offset %jd): record header has length %jd that does not match "
                "record footer (%jd)";
      } else {
        recordLength = header;
      }
    }
  }
  if (error) {
    handler.SignalError(error, static_cast<std::intmax_t>(currentRecordNumber),
        static_cast<std::intmax_t>(offsetInFile),
        static_cast<std::intmax_t>(header), static_cast<std::intmax_t>(footer));
  }
  positionInRecord = sizeof header;
}

void ExternalFileUnit::NextSequentialFormattedInputRecord(
    IoErrorHandler &handler) {
  static constexpr std::size_t chunk{256};
  std::size_t length{0};
  if (recordLength.has_value()) {
    // not first record - advance to next
    ++currentRecordNumber;
    if (endfileRecordNumber && currentRecordNumber >= *endfileRecordNumber) {
      handler.SignalEnd();
      return;
    }
    if (Frame()[*recordLength] == '\r') {
      ++*recordLength;
    }
    offsetInFile += *recordLength + 1;
  }
  while (true) {
    std::size_t got{ReadFrame(offsetInFile, length + chunk, handler)};
    if (got <= length) {
      handler.SignalEnd();
      break;
    }
    const char *frame{Frame()};
    if (const char *nl{reinterpret_cast<const char *>(
            std::memchr(frame + length, '\n', chunk))}) {
      recordLength = nl - (frame + length) + 1;
      if (*recordLength > 0 && frame[*recordLength - 1] == '\r') {
        --*recordLength;
      }
      return;
    }
    length += got;
  }
}
}
