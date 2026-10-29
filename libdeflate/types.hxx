// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <format>
#include <cstddef>   // size_t
#include <utility>   // forward()
#include <stdexcept> // invalid_argument

namespace deflate
{
  // Compressed data that doesn't conform to the deflate format, with the
  // description of the fault as the message. For example:
  //
  // throw invalid_data ("invalid block type {}", t);
  //
  // Note that the class has only inline members and so isn't exported (see
  // export.hxx for the reason).
  //
  class invalid_data: public std::invalid_argument
  {
  public:
    template <std::formattable<char>... A>
    explicit
    invalid_data (std::format_string<A...> f, A&&... a)
      : std::invalid_argument (std::format (f, std::forward<A> (a)...)) {}
  };

  // Number of bytes that a step of the compressor or decompressor took from
  // its input and wrote to its output.
  //
  struct progress
  {
    std::size_t consumed;
    std::size_t produced;
  };
}
