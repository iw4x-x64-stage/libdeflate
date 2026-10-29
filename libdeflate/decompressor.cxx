// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libdeflate/decompressor.hxx>

#include <algorithm> // min(), max(), ranges::fill()

#include <libdeflate/contract.hxx>

using namespace std;

namespace deflate
{
  namespace
  {
    // Return the first n bits of the value.
    //
    uint64_t
    low_bits (uint64_t v, unsigned n) noexcept
    {
      LIBDEFLATE_PRE (n < 64);
      return v & ((static_cast<uint64_t> (1) << n) - 1);
    }

    // Return the decoders of the fixed codes (3.2.6).
    //
    // Note that the literal/length code includes symbols 286 and 287 and the
    // distance code symbols 30 and 31, since they take part in the
    // construction of the codes. We reject them when we decode them, the
    // same way as with the dynamic codes.
    //
    huffman_decoder
    make_fixed_literal_code ()
    {
      uint8_t l[fixed_literal_symbols];

      for (size_t s (0); s != fixed_literal_symbols; ++s)
        l[s] = fixed_literal_length (s);

      huffman_decoder r;
      r.build (l);
      return r;
    }

    huffman_decoder
    make_fixed_distance_code ()
    {
      uint8_t l[max_distance_codes];
      ranges::fill (l, fixed_distance_length);

      huffman_decoder r;
      r.build (l);
      return r;
    }

    const huffman_decoder&
    fixed_literal_code ()
    {
      static const huffman_decoder r (make_fixed_literal_code ());
      return r;
    }

    const huffman_decoder&
    fixed_distance_code ()
    {
      static const huffman_decoder r (make_fixed_distance_code ());
      return r;
    }
  }

  decompressor::
  decompressor ()
    : window_ (make_unique_for_overwrite<char[]> (window_size))
  {
    reset ();
  }

  void decompressor::
  reset () noexcept
  {
    LIBDEFLATE_PRE (window_ != nullptr); // Not moved from.

    state_          = state::block_header;
    final_          = false;
    bits_           = 0;
    bit_count_      = 0;
    in_             = {};
    in_pos_         = 0;
    out_            = {};
    out_pos_        = 0;
    total_          = 0;
    remaining_      = 0;
    distance_       = 0;
    fixed_          = false;
    distance_empty_ = false;
    literal_count_  = 0;
    distance_count_ = 0;
    length_count_   = 0;
    index_          = 0;

    LIBDEFLATE_ASSERT (invariant ());
  }

  progress decompressor::
  next (string_view in, span<char> out, bool end)
  {
    LIBDEFLATE_PRE (window_ != nullptr); // Not moved from.
    LIBDEFLATE_INVARIANT (invariant ());

    in_      = in;
    in_pos_  = 0;
    out_     = out;
    out_pos_ = 0;

    // Move to what follows the end of the current block.
    //
    auto end_block = [this] ()
    {
      state_ = final_ ? state::finished : state::block_header;
    };

    // Go through the data until the input runs out, the output fills up, or
    // the data ends.
    //
    // Each step reads what it needs in one go and only then changes the
    // state. So if the input runs out in the middle of it, then the bits it
    // has gathered so far stay where they are and the step starts over on
    // the next call.
    //
    bool starved (false);
    bool full (false);

    while (!starved && !full && state_ != state::finished)
    {
      switch (state_)
      {
        case state::block_header:
        {
          if (!fill (3))
          {
            starved = true;
            break;
          }

          final_ = take (1) == 1;

          switch (const uint64_t t = take (2))
          {
            case 0:
            {
              // The stored block's length starts at the next byte boundary,
              // so drop the rest of the byte that the header ends in. Since
              // we take bytes only as we need them, that is all the bits we
              // have.
              //
              LIBDEFLATE_ASSERT (bit_count_ < 8);
              take (bit_count_);

              state_ = state::stored_header;
              break;
            }
            case 1:
            {
              fixed_ = true;
              state_ = state::block_data;
              break;
            }
            case 2:
            {
              fixed_ = false;
              state_ = state::code_counts;
              break;
            }
            default:
              throw invalid_data ("invalid block type {}", t);
          }

          break;
        }
        case state::stored_header:
        {
          if (!fill (32))
          {
            starved = true;
            break;
          }

          const uint64_t n (take (16));
          const uint64_t c (take (16));

          if ((n ^ 0xffff) != c)
            throw invalid_data (
              "invalid stored block length complement {} for length {}", c, n);

          remaining_ = n;
          state_ = state::stored_data;
          break;
        }
        case state::stored_data:
        {
          // The block's bytes follow its header at a byte boundary, so we
          // copy them straight from the input.
          //
          LIBDEFLATE_ASSERT (bit_count_ == 0);

          if (remaining_ == 0)
          {
            end_block ();
            break;
          }

          const size_t n (min ({remaining_,
                                in_.size () - in_pos_,
                                out_.size () - out_pos_}));

          if (n == 0)
          {
            if (out_pos_ == out_.size ())
              full = true;
            else
              starved = true;

            break;
          }

          for (size_t i (0); i != n; ++i)
            put (in_[in_pos_++]);

          remaining_ -= n;
          break;
        }
        case state::code_counts:
        {
          if (!fill (14))
          {
            starved = true;
            break;
          }

          literal_count_  = take (5) + 257;
          distance_count_ = take (5) + 1;
          length_count_   = take (4) + 4;

          // The field can count up to 288 literal/length codes, but the
          // format only defines up to 286 (3.2.7). The distance codes, on
          // the other hand, may number up to 32, the two that never occur in
          // the data included.
          //
          if (literal_count_ > literal_symbols)
            throw invalid_data ("invalid literal/length code count {}",
                                literal_count_);

          ranges::fill (lengths_, 0);
          index_ = 0;
          state_ = state::length_code;
          break;
        }
        case state::length_code:
        {
          // Read the lengths of the code length code, which come in their
          // own order, into the first code_length_symbols lengths.
          //
          for (; index_ != length_count_; ++index_)
          {
            if (!fill (3))
            {
              starved = true;
              break;
            }

            const uint8_t l (static_cast<uint8_t> (take (3)));
            lengths_[code_length_order[index_]] = l;
          }

          if (starved)
            break;

          const span<const uint8_t> ls (lengths_, code_length_symbols);

          if (const huffman_shape s = huffman_measure (ls);
              s != huffman_shape::complete)
            throw invalid_data ("invalid code length code: {}", to_string (s));

          length_code_.build (ls);

          ranges::fill (lengths_, 0);
          index_ = 0;
          state_ = state::code_lengths;
          break;
        }
        case state::code_lengths:
        {
          // Read the lengths of both codes as one sequence, since a repeat
          // may cross from one to the other (3.2.7).
          //
          const size_t n (literal_count_ + distance_count_);

          while (index_ != n)
          {
            huffman_decoder::result r;
            if (!decode (length_code_, 0, r))
            {
              starved = true;
              break;
            }

            // The code is complete, so every codeword is used.
            //
            LIBDEFLATE_ASSERT (r.symbol < code_length_symbols);

            // Figure out the extra bits and the shortest run of a repeat.
            //
            unsigned x (0);
            size_t   b (0);

            switch (r.symbol)
            {
              case 16: x = 2; b = 3;  break;
              case 17: x = 3; b = 3;  break;
              case 18: x = 7; b = 11; break;
            }

            if (!fill (r.length + x))
            {
              starved = true;
              break;
            }

            take (r.length);

            if (r.symbol < 16)
            {
              lengths_[index_++] = static_cast<uint8_t> (r.symbol);
              continue;
            }

            const size_t c (b + take (x));

            uint8_t l (0);
            if (r.symbol == 16)
            {
              if (index_ == 0)
                throw invalid_data ("code length repeat with no previous "
                                    "length");

              l = lengths_[index_ - 1];
            }

            if (c > n - index_)
              throw invalid_data ("code length repeat beyond {} code lengths",
                                  n);

            ranges::fill (span (lengths_).subspan (index_, c), l);
            index_ += c;
          }

          if (starved)
            break;

          // Verify and build the codes.
          //
          // The literal/length code must be complete and, since every block
          // ends with it, have the end-of-block code. The distance code may
          // be empty, which means the block has no matches, or be a single
          // code of length one (3.2.7). Otherwise it must be complete as
          // well.
          //
          const span<const uint8_t> ll (lengths_, literal_count_);
          const span<const uint8_t> dl (lengths_ + literal_count_,
                                        distance_count_);

          if (ll[end_of_block] == 0)
            throw invalid_data ("missing end-of-block code");

          if (const huffman_shape s = huffman_measure (ll);
              s != huffman_shape::complete)
            throw invalid_data ("invalid literal/length code: {}",
                                to_string (s));

          const huffman_shape s (huffman_measure (dl));

          if (s == huffman_shape::over_subscribed ||
              (s == huffman_shape::incomplete && !huffman_single (dl)))
            throw invalid_data ("invalid distance code: {}", to_string (s));

          literal_code_.build (ll);

          distance_empty_ = s == huffman_shape::empty;
          if (!distance_empty_)
            distance_code_.build (dl);

          state_ = state::block_data;
          break;
        }
        case state::block_data:
        {
          if (out_pos_ == out_.size ())
          {
            full = true;
            break;
          }

          const huffman_decoder& lc (fixed_
                                     ? fixed_literal_code ()
                                     : literal_code_);

          huffman_decoder::result r;
          if (!decode (lc, 0, r))
          {
            starved = true;
            break;
          }

          // The literal/length codes are complete, so every codeword is
          // used.
          //
          LIBDEFLATE_ASSERT (r.symbol != huffman_decoder::unused);

          if (r.symbol < end_of_block)
          {
            take (r.length);
            put (static_cast<char> (r.symbol));
            break;
          }

          if (r.symbol == end_of_block)
          {
            take (r.length);
            end_block ();
            break;
          }

          if (r.symbol >= literal_symbols)
            throw invalid_data ("invalid literal/length symbol {}", r.symbol);

          // Decode the rest of the match: the length's extra bits, the
          // distance codeword, and the distance's extra bits. We only take
          // the bits once we have all of them (o counts them so far), and
          // since they add up to at most 48 bits, we can hold them all.
          //
          unsigned o (r.length);

          const size_t li (r.symbol - first_length_symbol);
          const unsigned lx (length_extra[li]);

          if (!fill (o + lx))
          {
            starved = true;
            break;
          }

          const size_t length (length_base[li] + low_bits (bits_ >> o, lx));
          o += lx;

          // Each length symbol covers the lengths up to the next one's
          // shortest, except that 284 could reach 258 (see format.hxx).
          //
          if (li + 1 != size (length_base) && length >= length_base[li + 1])
            throw invalid_data ("invalid length {} for length symbol {}",
                                length,
                                r.symbol);

          if (!fixed_ && distance_empty_)
            throw invalid_data ("match in block without distance codes");

          const huffman_decoder& dc (fixed_
                                     ? fixed_distance_code ()
                                     : distance_code_);

          huffman_decoder::result d;
          if (!decode (dc, o, d))
          {
            starved = true;
            break;
          }

          if (d.symbol == huffman_decoder::unused)
            throw invalid_data ("invalid distance codeword");

          if (d.symbol >= distance_symbols)
            throw invalid_data ("invalid distance symbol {}", d.symbol);

          o += d.length;

          const unsigned dx (distance_extra[d.symbol]);

          if (!fill (o + dx))
          {
            starved = true;
            break;
          }

          const size_t distance (distance_base[d.symbol] +
                                 low_bits (bits_ >> o, dx));
          o += dx;

          if (distance > total_)
            throw invalid_data ("invalid distance {} at output offset {}",
                                distance,
                                total_);

          take (o);

          remaining_ = length;
          distance_  = distance;
          state_     = state::match;
          break;
        }
        case state::match:
        {
          // Copy a byte at a time since the match may overlap the bytes it
          // produces.
          //
          for (; remaining_ != 0; --remaining_)
          {
            if (out_pos_ == out_.size ())
            {
              full = true;
              break;
            }

            put (window_[(total_ - distance_) & (window_size - 1)]);
          }

          if (remaining_ == 0)
            state_ = state::block_data;

          break;
        }
        case state::finished:
          LIBDEFLATE_UNREACHABLE ();
      }
    }

    if (starved && end)
      throw invalid_data ("incomplete deflate data");

    const progress r {in_pos_, out_pos_};

    in_  = {};
    out_ = {};

    return r;
  }

  bool decompressor::
  invariant () const noexcept
  {
    // The bits above the ones we hold are zero (decoding relies on that),
    // a stored block's bytes start at a byte boundary, and a match lies
    // within the output so far.
    //
    return bit_count_ < 64 && bits_ >> bit_count_ == 0 &&
           (state_ != state::match ||
            (remaining_ != 0 && distance_ != 0 && distance_ <= total_)) &&
           (state_ != state::stored_data || bit_count_ == 0);
  }

  bool decompressor::
  pull () noexcept
  {
    LIBDEFLATE_PRE (bit_count_ <= 56);

    if (in_pos_ == in_.size ())
      return false;

    bits_ |= static_cast<uint64_t> (static_cast<unsigned char> (in_[in_pos_++]))
             << bit_count_;
    bit_count_ += 8;
    return true;
  }

  bool decompressor::
  fill (unsigned n) noexcept
  {
    LIBDEFLATE_PRE (n <= 56);

    while (bit_count_ < n)
    {
      if (!pull ())
        return false;
    }

    return true;
  }

  uint64_t decompressor::
  take (unsigned n) noexcept
  {
    LIBDEFLATE_PRE (n <= bit_count_);

    const uint64_t r (low_bits (bits_, n));
    bits_ >>= n;
    bit_count_ -= n;
    return r;
  }

  bool decompressor::
  decode (const huffman_decoder& d,
          unsigned skip,
          huffman_decoder::result& r) noexcept
  {
    LIBDEFLATE_PRE (skip <= bit_count_);

    for (;;)
    {
      r = d.decode (bits_ >> skip, bit_count_ - skip);

      if (r.length != 0)
        return true;

      if (!pull ())
        return false;
    }
  }

  void decompressor::
  put (char c) noexcept
  {
    LIBDEFLATE_PRE (out_pos_ != out_.size ());

    out_[out_pos_++] = c;
    window_[total_ & (window_size - 1)] = c;
    ++total_;
  }

  string
  decompress (string_view in)
  {
    decompressor d;

    // Decompress into the result, growing it as it fills up. Since the
    // decompressor is told that the input is all there is, it either gets
    // somewhere or throws.
    //
    const size_t initial (4096);

    string r;
    size_t n (0);

    while (!d.finished ())
    {
      if (n == r.size ())
        r.resize (max ({r.size () * 2, in.size () * 4, initial}));

      const progress p (
        d.next (in, span<char> (r.data (), r.size ()).subspan (n), true));

      in.remove_prefix (p.consumed);
      n += p.produced;
    }

    if (!in.empty ())
      throw invalid_data ("junk after deflate data");

    r.resize (n);
    return r;
  }
}
