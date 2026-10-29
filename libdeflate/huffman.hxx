// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <span>
#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint16_t, uint64_t

#include <libdeflate/format.hxx>

#include <libdeflate/export.hxx>

namespace deflate
{
  // Longest Huffman code the format allows (3.2.7).
  //
  inline constexpr unsigned huffman_max_length (15);

  // Largest alphabet of a Huffman code, the literal/length alphabet of the
  // fixed code (3.2.6).
  //
  inline constexpr std::size_t huffman_max_symbols (fixed_literal_symbols);

  // How the codes of a set of code lengths fill the code space.
  //
  // A complete code uses every codeword, so any sequence of bits decodes.
  // An incomplete code leaves some unused and an over-subscribed one has
  // more codes than there are codewords (and so isn't a prefix code at
  // all). An empty code has no codes.
  //
  enum class huffman_shape
  {
    complete,
    incomplete,
    over_subscribed,
    empty
  };

  // Return the shape name, as used in diagnostics (for example,
  // "over-subscribed").
  //
  LIBDEFLATE_SYMEXPORT const char*
  to_string (huffman_shape) noexcept;

  // Return the shape of the code that the lengths describe. Each length
  // must not exceed huffman_max_length.
  //
  LIBDEFLATE_SYMEXPORT huffman_shape
  huffman_measure (std::span<const std::uint8_t> lengths) noexcept;

  // Return true if the code that the lengths describe is a single code of
  // length one, which is the one incomplete code the format allows, and
  // only for distances (3.2.7).
  //
  LIBDEFLATE_SYMEXPORT bool
  huffman_single (std::span<const std::uint8_t> lengths) noexcept;

  // Compute the code lengths of an optimal prefix code for the symbol
  // frequencies, with no code longer than limit bits, and write them into
  // lengths, indexed by symbol. The symbols that don't occur get length
  // zero.
  //
  // If two or more symbols occur, then the code is complete. A single
  // symbol gets length one and so a single code of length one (see
  // huffman_single()), which only the distance code may be. So to get a
  // complete code for one symbol, the caller has to make another occur.
  //
  // The frequencies and lengths must be of the same size, which must not
  // exceed huffman_max_symbols, the limit must not exceed
  // huffman_max_length, and the symbols that occur must fit into a code of
  // that limit (up to 2^limit of them).
  //
  LIBDEFLATE_SYMEXPORT void
  huffman_lengths (std::span<const std::uint32_t> frequencies,
                   unsigned limit,
                   std::span<std::uint8_t> lengths);

  // Encoder of a canonical Huffman code (3.2.2).
  //
  class LIBDEFLATE_SYMEXPORT huffman_encoder
  {
  public:
    // Construct an empty encoder.
    //
    huffman_encoder () noexcept;

    // Build the encoder for the code that the lengths describe, indexed by
    // symbol. The code must be complete, empty, or a single code of length
    // one (see huffman_single()).
    //
    void
    build (std::span<const std::uint8_t> lengths) noexcept;

    // Return the symbol's codeword, reversed so that it can be written
    // least significant bit first, and its length (zero if the symbol has
    // no code).
    //
    std::uint16_t
    code (std::size_t s) const noexcept {return code_[s];}

    std::uint8_t
    length (std::size_t s) const noexcept {return length_[s];}

  private:
    std::uint16_t code_[huffman_max_symbols];
    std::uint8_t  length_[huffman_max_symbols];
  };

  // Decoder of a canonical Huffman code (3.2.2).
  //
  // The format packs a codeword starting from its first bit, so the bits
  // that a decoder gets (least significant first) are the codeword
  // reversed. We decode codes of up to table_bits bits with a single lookup
  // in a table indexed by those bits and the longer ones by walking the
  // canonical code a bit at a time, which is how the format describes it.
  //
  // Decoding never needs more bits than the codeword it decodes has. A
  // caller that is short of bits can therefore decode with what it has and
  // get more only when the decoder asks for them, so that it never reads
  // past the data it is given.
  //
  class LIBDEFLATE_SYMEXPORT huffman_decoder
  {
  public:
    // Symbol of an unused codeword.
    //
    static constexpr std::uint16_t unused = 0xffff;

    // Decoded codeword. A length of zero means that the bits don't yet
    // determine it.
    //
    struct result
    {
      std::uint16_t symbol;
      std::uint8_t  length;
    };

    // Construct an empty decoder.
    //
    huffman_decoder () noexcept;

    // Build the decoder for the code that the lengths describe, indexed by
    // symbol. The code must be complete, empty, or a single code of length
    // one (see huffman_single()). An empty code decodes nothing.
    //
    void
    build (std::span<const std::uint8_t> lengths) noexcept;

    // Decode the codeword in the first n bits (least significant first),
    // where the bits above n must be zero. Return the symbol and the
    // codeword length or, if decoding needs more than n bits, zero length.
    // If the bits form an unused codeword, return the unused symbol.
    //
    result
    decode (std::uint64_t bits, unsigned n) const noexcept;

  private:
    static constexpr unsigned table_bits = 9;

    // Table entry: the symbol and the codeword length of the codeword in
    // the index's bits or, if it is longer than table_bits, zero length.
    //
    struct entry
    {
      std::uint16_t symbol;
      std::uint8_t  length;
    };

    entry         table_[1u << table_bits];
    std::uint16_t count_[huffman_max_length + 1];   // Codes of each length.
    std::uint16_t symbol_[huffman_max_symbols];     // By codeword.
    unsigned      max_length_;                      // Zero if empty.
  };
}
