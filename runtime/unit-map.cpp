//===-- runtime/unit-map.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "unit-map.h"

namespace Fortran::runtime::io {

ExternalFileUnit *UnitMap::LookUpForClose(int n) {
  CriticalSection critical{lock_};
  Chain *previous{nullptr};
  int hash{Hash(n)};
  for (Chain *p{bucket_[hash]}; p; previous = p, p = p->next) {
    if (p->unit.unitNumber() == n) {
      if (previous) {
        previous->next = p->next;
      } else {
        bucket_[hash] = p->next;
      }
      p->next = closing_;
      closing_ = p;
      return &p->unit;
    }
  }
  return nullptr;
}

void UnitMap::DestroyClosed(ExternalFileUnit &unit) {
  Chain *p{nullptr};
  {
    CriticalSection critical{lock_};
    Chain *previous{nullptr};
    for (p = closing_; p; previous = p, p = p->next) {
      if (&p->unit == &unit) {
        if (previous) {
          previous->next = p->next;
        } else {
          closing_ = p->next;
        }
        break;
      }
    }
  }
  if (p) {
    p->unit.~ExternalFileUnit();
    FreeMemory(p);
  }
}

void UnitMap::CloseAll(IoErrorHandler &handler) {
  CriticalSection critical{lock_};
  for (int j{0}; j < buckets_; ++j) {
    while (Chain * p{bucket_[j]}) {
      bucket_[j] = p->next;
      p->unit.CloseUnit(CloseStatus::Keep, handler);
      p->unit.~ExternalFileUnit();
      FreeMemory(p);
    }
  }
}

ExternalFileUnit &UnitMap::Create(int n, const Terminator &terminator) {
  Chain &chain{New<Chain>{}(terminator, n)};
  Chain *&head{bucket_[Hash(n)]};
  chain.next = head;
  head = &chain;
  return chain.unit;
}
}
