// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint16_t

// The constants and tables of the deflate format, as RFC 1951 defines them.
// Section numbers below refer to that document.
//
namespace deflate
{
  // Largest distance of a match and so the size of the window that the
  // matches refer to (3.2.5).
  //
  inline constexpr std::size_t window_size (32768);

  // Shortest and longest match (3.2.5).
  //
  inline constexpr std::size_t min_match (3);
  inline constexpr std::size_t max_match (258);

  // Largest length of a stored block (3.2.4).
  //
  inline constexpr std::size_t max_stored (65535);

  // Literal/length alphabet: the literals (0-255), the end of the block
  // (256), and the lengths (257-285). Symbols 286 and 287 only take part in
  // the construction of the fixed code (3.2.5, 3.2.6).
  //
  inline constexpr std::uint16_t end_of_block (256);
  inline constexpr std::uint16_t first_length_symbol (257);
  inline constexpr std::size_t   literal_symbols (286);
  inline constexpr std::size_t   fixed_literal_symbols (288);

  inline constexpr std::size_t length_symbols (literal_symbols -
                                               first_length_symbol);

  // Distance alphabet. Symbols 30 and 31 only take part in the construction
  // of the codes (3.2.5, 3.2.6, 3.2.7).
  //
  inline constexpr std::size_t distance_symbols (30);
  inline constexpr std::size_t max_distance_codes (32);

  // Code length alphabet and the order in which a dynamic block lists the
  // lengths of its code (3.2.7).
  //
  inline constexpr std::size_t code_length_symbols (19);

  inline constexpr std::uint8_t code_length_order[code_length_symbols]
  {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };

  // Shortest length and the number of extra bits of each length symbol,
  // starting from 257 (3.2.5).
  //
  // Note that symbol 284 with all its extra bits set would make 258, which
  // lies outside its range (227-257) and is the length of symbol 285.
  //
  inline constexpr std::uint16_t length_base[length_symbols]
  {
    3,   4,   5,   6,   7,   8,   9,   10,  11,  13,
    15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
    67,  83,  99,  115, 131, 163, 195, 227, 258
  };

  inline constexpr std::uint8_t length_extra[length_symbols]
  {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
    1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
  };

  // Shortest distance and the number of extra bits of each distance symbol
  // (3.2.5).
  //
  inline constexpr std::uint16_t distance_base[distance_symbols]
  {
    1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
    33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577
  };

  inline constexpr std::uint8_t distance_extra[distance_symbols]
  {
    0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
    4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
  };
}
