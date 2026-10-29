// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <iosfwd>
#include <string>

#include <libdeflate/export.hxx>

namespace deflate
{
  // Print a greeting for the specified name into the specified
  // stream. Throw std::invalid_argument if the name is empty.
  //
  LIBDEFLATE_SYMEXPORT void
  say_hello (std::ostream&, const std::string& name);
}
