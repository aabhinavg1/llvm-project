//===-- Implementation of setlocale ---------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/locale/setlocale.h"
#include "hdr/locale_macros.h"
#include "src/__support/CPP/string_view.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

LLVM_LIBC_FUNCTION(char *, setlocale, (int category, const char *locale_name)) {
  cpp::string_view name(locale_name);
  if (category > LC_ALL || (!name.empty() && name != "C"))
    return nullptr;

  static char locale_str[] = "C";
  return locale_str;
}

} // namespace LIBC_NAMESPACE_DECL
