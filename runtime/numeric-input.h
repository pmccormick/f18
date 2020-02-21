//===-- runtime/numeric-input.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef FORTRAN_RUNTIME_NUMERIC_INPUT_H_
#define FORTRAN_RUNTIME_NUMERIC_INPUT_H_

#include "format.h"
#include "io-stmt.h"
#include "flang/decimal/decimal.h"

namespace Fortran::runtime::io {

bool EditIntegerInput(IoStatementState &, const DataEdit &, void *, int kind);

template<int binaryPrecision>
bool EditRealInput(IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<8>(
    IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<11>(
    IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<24>(
    IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<53>(
    IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<64>(
    IoStatementState &, const DataEdit &, void *);
extern template bool EditRealInput<112>(
    IoStatementState &, const DataEdit &, void *);
}
#endif  // FORTRAN_RUNTIME_NUMERIC_INPUT_H_
