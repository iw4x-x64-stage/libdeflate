// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <span>
#include <memory>  // unique_ptr
#include <string>
#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint16_t, int32_t, uint64_t
#include <string_view>

#include <libdeflate/types.hxx>
#include <libdeflate/format.hxx>

#include <libdeflate/export.hxx>

namespace deflate
{
  // Compression level, from 0 to 9.
  //
  // Level 0 stores the data as it is, in stored blocks. The levels from 1
  // up spend more and more time looking for repeated strings, and so make
  // the data smaller and smaller. Any value in the range is a level, the
  // enumerators name the usual ones.
  //
  enum class compression_level: std::uint8_t
  {
    store    = 0,
    fastest  = 1,
    balanced = 6,
    smallest = 9
  };

  // Incremental compressor into deflate data (RFC 1951).
  //
  // The compressor takes the data in parts of any size and writes the
  // compressed data into output buffers of any size. The compressed data
  // depends only on the data and the level, so however the data is split
  // into parts, the result is the same. Each block of it is written in the
  // form that makes it the smallest: stored, with the fixed codes, or with
  // codes of its own.
  //
  // For example:
  //
  // compressor c (compression_level::smallest);
  // char out[4096];
  //
  // while (!c.finished ())
  // {
  //   string_view in (...); // Next part of the data.
  //   bool end (...);       // Whether it is the last part.
  //
  //   for (progress p {0, sizeof (out)};
  //        !c.finished () && (!in.empty () || p.produced == sizeof (out)); )
  //   {
  //     p = c.next (in, out, end);
  //     in.remove_prefix (p.consumed);
  //
  //     ... // Write the first p.produced bytes of out.
  //   }
  // }
  //
  class LIBDEFLATE_SYMEXPORT compressor
  {
  public:
    // The level must be in the range from 0 to 9.
    //
    explicit
    compressor (compression_level = compression_level::balanced);

    // Compress the next part of the data from the input into the output.
    // Stop when the output fills up, or when the input runs out and all that
    // can be compressed of it is written, and return how much of each was
    // used.
    //
    // If end is true, then the input is the last part of the data, and the
    // compressor finishes the compressed data once it has taken all of it.
    // Once end is true, it must stay so in the calls that follow.
    //
    // Note that the compressor holds on to the data that it may still find
    // a repeated string in. So until end is true, it may write nothing for a
    // while.
    //
    progress
    next (std::string_view in, std::span<char> out, bool end);

    // Return true if all of the compressed data has been written.
    //
    bool
    finished () const noexcept {return final_written_ && pending_.empty ();}

    // Reset to the state after construction, to compress other data at the
    // same level.
    //
    void
    reset () noexcept;

  private:
    bool
    invariant () const noexcept;

    // Find the repeated strings in the window from pos_ on, recording
    // literals and matches and writing blocks as they fill up. Stop when
    // too little of the window is left to find the longest match, unless
    // flush is true, in which case go on to the end of the window.
    //
    void
    search (bool flush);

    // Return the length of the longest match for the string at pos_ among
    // the strings in the hash chain that starts at head, if it is longer
    // than shortest, and set start to where it starts. Otherwise, return
    // shortest.
    //
    std::size_t
    longest_match (std::int32_t head,
                   std::size_t shortest,
                   std::size_t& start) const noexcept;

    // Insert the string at the position into its hash chain and return the
    // previous head of the chain (or -1 if it was empty).
    //
    std::int32_t
    insert (std::size_t) noexcept;

    // Move the second half of the window to the first, to make room for more
    // data.
    //
    void
    slide () noexcept;

    // Record a literal or a match, writing the block if it is full.
    //
    void
    record_literal (std::size_t position);

    void
    record_match (std::size_t position,
                  std::size_t length,
                  std::size_t distance);

    // Write the block of the recorded literals and matches, which is the
    // final one if final is true.
    //
    void
    write_block (bool final);

    // Write the first n bits of the value, least significant first.
    //
    void
    write_bits (std::uint64_t value, unsigned n);

    // Write zero bits up to the next byte boundary.
    //
    void
    align ();

  private:
    // Size of the window buffer, which holds two windows: the data that the
    // matches may refer to and the data being compressed.
    //
    static constexpr std::size_t buffer_size = 2 * window_size;

    // Literals and matches that a block holds at most.
    //
    static constexpr std::size_t block_tokens = 16384;

    // Literal (distance 0) or match.
    //
    struct token
    {
      std::uint16_t value;    // Literal byte or match length.
      std::uint16_t distance;
    };

    compression_level level_;
    bool              ending_;        // End of the data was given.
    bool              final_written_; // Final block is in pending_.

    // The window buffer with the data from 0 to end_. The data up to pos_
    // has been searched and the literals and matches up to covered_
    // recorded, starting from block_start_ (unless the start of the block
    // was slid out, which block_lost_ indicates).
    //
    std::unique_ptr<char[]> window_;
    std::size_t             end_;
    std::size_t             pos_;
    std::size_t             covered_;
    std::size_t             block_start_;
    bool                    block_lost_;

    // Match at the previous position, which with lazy matching we only
    // record if the match at the current position isn't longer. If there
    // is no match there, then its length is less than min_match and the
    // literal is pending if pending_literal_ is true.
    //
    std::size_t previous_length_;
    std::size_t previous_start_;
    bool        pending_literal_;

    // Hash chains: the head position of each hash and the previous position
    // of the same hash for each window position, -1 if none.
    //
    std::unique_ptr<std::int32_t[]> head_;
    std::unique_ptr<std::int32_t[]> chain_;

    // Recorded literals and matches of the current block.
    //
    std::unique_ptr<token[]> tokens_;
    std::size_t              token_count_;

    // Bits not yet written as a byte, and the compressed data not yet
    // written to the output.
    //
    std::uint64_t bits_;
    unsigned      bit_count_;
    std::string   pending_;
    std::size_t   pending_pos_;
  };

  // Compress the whole data at the level and return the result. The level
  // must be in the range from 0 to 9.
  //
  LIBDEFLATE_SYMEXPORT std::string
  compress (std::string_view,
            compression_level = compression_level::balanced);
}
