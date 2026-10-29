// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <span>
#include <memory>  // unique_ptr
#include <string>
#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint64_t
#include <string_view>

#include <libdeflate/types.hxx>
#include <libdeflate/format.hxx>
#include <libdeflate/huffman.hxx>

#include <libdeflate/export.hxx>

namespace deflate
{
  // Incremental decompressor of deflate data (RFC 1951).
  //
  // The decompressor takes the data in parts of any size, down to a byte at
  // a time, and writes the result into output buffers of any size, so the
  // caller decides how much memory either side takes. It accepts every data
  // set that the format defines and rejects everything else, throwing
  // invalid_data on the first fault it finds. In particular, it rejects
  // what the format only defines by omission: a literal/length or code
  // length code that is incomplete, a distance code that is incomplete
  // other than by being a single code of length one, length 258 encoded
  // with symbol 284, and a match that reaches back before the start of the
  // data. After an exception the decompressor can only be reset.
  //
  // For example:
  //
  // decompressor d;
  // char out[4096];
  //
  // while (!d.finished ())
  // {
  //   string_view in (...); // Next part of the data.
  //   bool end (...);       // Whether it is the last part.
  //
  //   for (progress p {0, sizeof (out)};
  //        !d.finished () && (!in.empty () || p.produced == sizeof (out)); )
  //   {
  //     p = d.next (in, out, end);
  //     in.remove_prefix (p.consumed);
  //
  //     ... // Write the first p.produced bytes of out.
  //   }
  // }
  //
  class LIBDEFLATE_SYMEXPORT decompressor
  {
  public:
    decompressor ();

    // Decompress the next part of the data from the input into the output.
    // Stop when the input runs out, the output fills up, or the data ends,
    // and return how much of each was used. Throw invalid_data if the data
    // is invalid.
    //
    // If end is true, then the input is the last part of the data, which
    // must therefore end in it. In this case throw invalid_data if the input
    // runs out first.
    //
    // Note that the decompressor never takes input past the end of the data,
    // so whatever follows it remains unconsumed. A call after the end of the
    // data does nothing.
    //
    progress
    next (std::string_view in, std::span<char> out, bool end);

    // Return true if the end of the data has been reached.
    //
    bool
    finished () const noexcept {return state_ == state::finished;}

    // Reset to the default-constructed state, to decompress other data.
    //
    void
    reset () noexcept;

  private:
    bool
    invariant () const noexcept;

    // Take the next byte of the input into the bits. Return false if the
    // input has run out.
    //
    bool
    pull () noexcept;

    // Make sure there are at least n bits, taking only as many bytes as that
    // needs. Return false if the input runs out first.
    //
    bool
    fill (unsigned n) noexcept;

    // Remove and return the first n bits, of which there must be at least n.
    //
    std::uint64_t
    take (unsigned n) noexcept;

    // Decode the codeword that starts skip bits into the bits, taking only
    // as many bytes as that needs. Return false if the input runs out first.
    // Leave the bits as they are.
    //
    bool
    decode (const huffman_decoder&,
            unsigned skip,
            huffman_decoder::result&) noexcept;

    // Write the byte to the output and the window. There must be room in the
    // output.
    //
    void
    put (char) noexcept;

  private:
    // Where we are in the data.
    //
    enum class state
    {
      block_header,  // Final flag and block type.
      stored_header, // Length and its complement.
      stored_data,   // Bytes of a stored block.
      code_counts,   // Code counts of a dynamic block.
      length_code,   // Code length code lengths.
      code_lengths,  // Literal/length and distance code lengths.
      block_data,    // Literals, matches, and the end of the block.
      match,         // Bytes of a match.
      finished
    };

    state state_;
    bool  final_; // Current block is the last.

    // Bits taken from the input and not yet used, first bit least
    // significant.
    //
    std::uint64_t bits_;
    unsigned      bit_count_;

    // Input and output of the next() call in progress.
    //
    std::string_view in_;
    std::size_t      in_pos_;
    std::span<char>  out_;
    std::size_t      out_pos_;

    // The last window_size bytes of the output, which matches refer to, and
    // the size of the output so far.
    //
    std::unique_ptr<char[]> window_;
    std::uint64_t           total_;

    // Bytes left of the stored block or of the match, and the match
    // distance.
    //
    std::size_t remaining_;
    std::size_t distance_;

    // Codes of the current block. For a block compressed with the fixed codes
    // these are unused (see the implementation for details).
    //
    bool            fixed_;
    huffman_decoder literal_code_;
    huffman_decoder distance_code_;
    bool            distance_empty_;

    // Header of the dynamic block being read: the code counts, the code
    // length code, and the code lengths read so far.
    //
    std::size_t     literal_count_;
    std::size_t     distance_count_;
    std::size_t     length_count_;
    std::size_t     index_;
    huffman_decoder length_code_;
    std::uint8_t    lengths_[literal_symbols + max_distance_codes];
  };

  // Decompress the whole data and return the result. Throw invalid_data if
  // the data is invalid, incomplete, or followed by anything.
  //
  LIBDEFLATE_SYMEXPORT std::string
  decompress (std::string_view);
}
