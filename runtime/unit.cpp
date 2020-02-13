//===-- runtime/unit.cpp ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "unit.h"
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

bool ExternalFileUnit::SetPositionInRecord(
    std::int64_t n, IoErrorHandler &handler) {
  n = std::max<std::int64_t>(0, n);
  bool ok{true};
  if (n > static_cast<std::int64_t>(recordLength.value_or(n))) {
    handler.SignalEor();
    n = *recordLength;
    ok = false;
  }
  if (n > furthestPositionInRecord) {
    if (!isReading_ && ok) {
      WriteFrame(offsetInFile, n, handler);
      std::fill_n(Frame() + furthestPositionInRecord,
          n - furthestPositionInRecord, ' ');
    }
    furthestPositionInRecord = n;
  }
  positionInRecord = n;
  return ok;
}

bool ExternalFileUnit::Emit(
    const char *data, std::size_t bytes, IoErrorHandler &handler) {
  auto furthestAfter{std::max(furthestPositionInRecord,
      positionInRecord + static_cast<std::int64_t>(bytes))};
  WriteFrame(offsetInFile, furthestAfter, handler);
  std::memcpy(Frame() + positionInRecord, data, bytes);
  positionInRecord += bytes;
  furthestPositionInRecord = furthestAfter;
  return true;
}

const char *ExternalFileUnit::View(
    std::size_t &bytes, IoErrorHandler &handler) {
  isReading_ = true;  // TODO: manage transitions
  if (access == Access::Stream) {
    auto chunk{std::max<std::size_t>(bytes, 256)};
    bytes = ReadFrame(offsetInFile, chunk, handler);
    return Frame();
  }
  std::size_t wanted{bytes};
  const char *frame{nullptr};
  if (recordLength.has_value()) {
    // Direct, or sequential has determined current record length
    bytes = ReadFrame(offsetInFile + positionInRecord,
        *recordLength - positionInRecord, handler);
    frame = Frame() + positionInRecord;
  } else {
    // Sequential input needs to read next record
    if (endfileRecordNumber.has_value() &&
        currentRecordNumber >= *endfileRecordNumber) {
      handler.SignalEnd();
      bytes = 0;
      return nullptr;
    }
    offsetInFile = nextInputRecordFileOffset;
    if (isUnformatted) {
      frame = NextSequentialUnformattedInputRecord(bytes, handler);
    } else {
      frame = NextSequentialFormattedInputRecord(bytes, handler);
    }
  }
  if (frame && bytes < wanted) {
    handler.SignalEor();
  }
  return frame;
}

void ExternalFileUnit::SetLeftTabLimit() {
  leftTabLimit = furthestPositionInRecord;
  positionInRecord = furthestPositionInRecord;
}

bool ExternalFileUnit::AdvanceRecord(IoErrorHandler &handler) {
  bool ok{true};
  if (isReading_) {
    if (access == Access::Sequential) {
      recordLength.reset();
    }
  } else if (recordLength.has_value()) {  // fill fixed-size record
    ok &= SetPositionInRecord(*recordLength, handler);
  } else if (!isUnformatted) {
    ok &= SetPositionInRecord(furthestPositionInRecord, handler);
    ok &= Emit("\n", 1, handler);  // TODO: Windows CR+LF
    offsetInFile += furthestPositionInRecord;
  }
  ++currentRecordNumber;
  positionInRecord = 0;
  furthestPositionInRecord = 0;
  leftTabLimit.reset();
  return ok;
}

bool ExternalFileUnit::HandleAbsolutePosition(
    std::int64_t n, IoErrorHandler &handler) {
  return SetPositionInRecord(
      std::max(n, std::int64_t{0}) + leftTabLimit.value_or(0), handler);
}

bool ExternalFileUnit::HandleRelativePosition(
    std::int64_t n, IoErrorHandler &handler) {
  return HandleAbsolutePosition(positionInRecord + n, handler);
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

const char *ExternalFileUnit::NextSequentialUnformattedInputRecord(
    std::size_t &bytes, IoErrorHandler &handler) {
  std::uint32_t header;
  // Retain previous footer in frame for more efficient BACKSPACE
  std::size_t retain{std::min<std::size_t>(offsetInFile, sizeof header)};
  std::size_t want{retain + sizeof header};
  std::size_t got{ReadFrame(offsetInFile - retain, want, handler)};
  if (got >= want) {
    std::memcpy(&header, Frame() + retain, sizeof header);
    want = retain + header + 2 * sizeof header;
    got = ReadFrame(offsetInFile - retain, want, handler);
    if (got >= want) {
      const char *start{Frame() + retain + sizeof header};
      if (std::memcmp(start - sizeof header, start + header, sizeof header) ==
          0) {
        recordLength = header;
        nextInputRecordFileOffset = offsetInFile + header + 2 * sizeof header;
        positionInRecord = sizeof header;
        bytes = header;
        return start;
      }
      // TODO: signal corrupt file?
    }
  }
  handler.SignalEnd();
  bytes = 0;
  return nullptr;
}

const char *ExternalFileUnit::NextSequentialFormattedInputRecord(
    std::size_t &bytes, IoErrorHandler &handler) {
  auto chunk{std::max<std::size_t>(bytes, 256)};
  std::size_t length{0};
  while (true) {
    std::size_t got{ReadFrame(offsetInFile, length + chunk, handler)};
    if (got <= length) {
      handler.SignalEnd();
      bytes = 0;
      return nullptr;
    }
    const char *frame{Frame()};
    if (const char *nl{reinterpret_cast<const char *>(
            std::memchr(frame + length, '\n', chunk))}) {
      length += nl - (frame + length);
      nextInputRecordFileOffset = offsetInFile + length + 1;
      if (length > 0 && frame[length - 1] == '\r') {
        --length;
      }
      recordLength = length;
      bytes = length;
      return frame;
    }
  }
}
}
