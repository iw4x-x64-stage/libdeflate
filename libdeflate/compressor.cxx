// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libdeflate/compressor.hxx>

#include <vector>
#include <limits>    // numeric_limits
#include <iterator>  // size()
#include <cstring>   // memcpy(), memmove()
#include <algorithm> // min(), max(), ranges::*

#include <libdeflate/huffman.hxx>
#include <libdeflate/contract.hxx>

using namespace std;

namespace deflate
{
  namespace
  {
    // How hard each level looks for matches.
    //
    // A level with lazy matching looks for a longer match at the next
    // position before it records the match at the current one (see RFC 1951
    // section 4). The values are those that zlib uses for its levels.
    //
    struct search_parameters
    {
      unsigned good;  // Search a quarter of the chain if the previous match
                      // is at least this long.
      unsigned lazy;  // Look further unless the match is at least this
                      // long (0 for no lazy matching).
      unsigned nice;  // Stop searching at a match at least this long.
      unsigned chain; // Follow at most this many chain positions.
    };

    const search_parameters levels[]
    {
      {0,  0,   0,   0},    // 0: Store.
      {4,  0,   8,   4},    // 1
      {4,  0,   16,  8},    // 2
      {4,  0,   32,  32},   // 3
      {4,  4,   16,  16},   // 4
      {8,  16,  32,  32},   // 5
      {8,  16,  128, 128},  // 6
      {8,  32,  128, 256},  // 7
      {32, 128, 258, 1024}, // 8
      {32, 258, 258, 4096}  // 9
    };

    // Data that must be ahead of a position to find the longest match there
    // and hash the string that follows it.
    //
    constexpr size_t min_lookahead (max_match + min_match + 1);

    // Largest distance that we use. Matches may be up to window_size away,
    // but we slide the window when there is less than min_lookahead left
    // of it, and a match at the previous position must remain in the window
    // after that (RFC 1951 section 3.3 allows a compressor this limit).
    //
    constexpr size_t max_distance (window_size - min_lookahead - 1);

    // Hash of the strings of min_match bytes.
    //
    constexpr unsigned hash_bits (15);
    constexpr size_t   hash_size (static_cast<size_t> (1) << hash_bits);

    // Return the index of the symbol for the match length or the distance
    // in the length or distance tables (see format.hxx).
    //
    size_t
    length_index (size_t l) noexcept
    {
      LIBDEFLATE_PRE (l >= min_match && l <= max_match);
      return static_cast<size_t> (ranges::upper_bound (length_base, l) -
                                  ranges::begin (length_base)) - 1;
    }

    size_t
    distance_index (size_t d) noexcept
    {
      LIBDEFLATE_PRE (d != 0 && d <= window_size);
      return static_cast<size_t> (ranges::upper_bound (distance_base, d) -
                                  ranges::begin (distance_base)) - 1;
    }

    // Return the encoders of the fixed codes (3.2.6).
    //
    huffman_encoder
    make_fixed_literal_code ()
    {
      uint8_t l[fixed_literal_symbols];

      for (size_t s (0); s != fixed_literal_symbols; ++s)
        l[s] = fixed_literal_length (s);

      huffman_encoder r;
      r.build (l);
      return r;
    }

    huffman_encoder
    make_fixed_distance_code ()
    {
      uint8_t l[max_distance_codes];
      ranges::fill (l, fixed_distance_length);

      huffman_encoder r;
      r.build (l);
      return r;
    }

    const huffman_encoder&
    fixed_literal_code ()
    {
      static const huffman_encoder r (make_fixed_literal_code ());
      return r;
    }

    const huffman_encoder&
    fixed_distance_code ()
    {
      static const huffman_encoder r (make_fixed_distance_code ());
      return r;
    }

    // Number of extra bits of the code length symbols (3.2.7).
    //
    unsigned
    code_length_extra (uint8_t s) noexcept
    {
      switch (s)
      {
        case 16: return 2;
        case 17: return 3;
        case 18: return 7;
      }

      return 0;
    }

    // Header of a block with dynamic codes (3.2.7): the codes, the run of
    // code lengths that describes them, and the header size in bits.
    //
    struct dynamic_header
    {
      // Code length symbol and the value of its extra bits.
      //
      struct run
      {
        uint8_t symbol;
        uint8_t extra;
      };

      uint8_t     literal_lengths[literal_symbols];
      uint8_t     distance_lengths[distance_symbols];
      uint8_t     code_length_lengths[code_length_symbols];
      size_t      literal_count;
      size_t      distance_count;
      size_t      code_length_count;
      vector<run> runs;
      uint64_t    bits;
    };

    // Plan the header of a block with dynamic codes for the frequencies of
    // its literal/length and distance symbols.
    //
    dynamic_header
    plan_dynamic (span<const uint32_t, literal_symbols> lf,
                  span<const uint32_t, distance_symbols> df)
    {
      dynamic_header h;

      // Make the codes.
      //
      // The literal/length code must be complete, but with no literals or
      // matches the end of the block is its only symbol. In this case we
      // give literal 0 a code as well, which costs nothing since it never
      // occurs. The distance code may be empty or a single code of length
      // one (3.2.7), which is what huffman_lengths() makes of no distances
      // and of one.
      //
      {
        uint32_t f[literal_symbols];
        ranges::copy (lf, f);

        if (ranges::count_if (f, [] (uint32_t x) {return x != 0;}) == 1)
          f[0] = 1;

        huffman_lengths (f, huffman_max_length, h.literal_lengths);
        huffman_lengths (df, huffman_max_length, h.distance_lengths);
      }

      // Leave out the trailing lengths of zero, keeping at least the
      // minimum counts.
      //
      h.literal_count = literal_symbols;
      while (h.literal_count > first_length_symbol &&
             h.literal_lengths[h.literal_count - 1] == 0)
        --h.literal_count;

      h.distance_count = distance_symbols;
      while (h.distance_count > 1 &&
             h.distance_lengths[h.distance_count - 1] == 0)
        --h.distance_count;

      // Encode the lengths of both codes as one sequence, runs of zeros
      // with 17 and 18 and repeats of other lengths with 16. Note that runs
      // may cross from one code to the other.
      //
      vector<uint8_t> ls (h.literal_lengths,
                          h.literal_lengths + h.literal_count);
      ls.insert (ls.end (),
                 h.distance_lengths,
                 h.distance_lengths + h.distance_count);

      auto add = [&h] (uint8_t s, size_t x)
      {
        h.runs.push_back (dynamic_header::run {s, static_cast<uint8_t> (x)});
      };

      for (size_t i (0); i != ls.size ();)
      {
        const uint8_t l (ls[i]);

        size_t r (1);
        while (i + r != ls.size () && ls[i + r] == l)
          ++r;

        i += r;

        if (l == 0)
        {
          for (; r >= 11; r -= min<size_t> (r, 138))
            add (18, min<size_t> (r, 138) - 11);

          if (r >= 3)
          {
            add (17, r - 3);
            r = 0;
          }
        }
        else
        {
          add (l, 0);
          --r;

          for (; r >= 3; r -= min<size_t> (r, 6))
            add (16, min<size_t> (r, 6) - 3);
        }

        for (; r != 0; --r)
          add (l, 0);
      }

      // Make the code length code, which must be complete as well. So if
      // only one length symbol occurs, then we give another one a code.
      //
      {
        uint32_t f[code_length_symbols] {};

        for (const dynamic_header::run& r: h.runs)
          ++f[r.symbol];

        if (ranges::count_if (f, [] (uint32_t x) {return x != 0;}) == 1)
          ++f[f[0] == 0 ? 0 : 1];

        huffman_lengths (f, max_code_length_length, h.code_length_lengths);
      }

      h.code_length_count = code_length_symbols;
      while (h.code_length_count > 4 &&
             h.code_length_lengths[
               code_length_order[h.code_length_count - 1]] == 0)
        --h.code_length_count;

      // Calculate the header size.
      //
      h.bits = 5 + 5 + 4 + 3 * h.code_length_count;

      for (const dynamic_header::run& r: h.runs)
        h.bits += h.code_length_lengths[r.symbol] +
                  code_length_extra (r.symbol);

      return h;
    }
  }

  compressor::
  compressor (compression_level l)
    : level_ (l),
      window_ (make_unique_for_overwrite<char[]> (buffer_size)),
      head_ (make_unique_for_overwrite<int32_t[]> (hash_size)),
      chain_ (make_unique_for_overwrite<int32_t[]> (window_size)),
      tokens_ (make_unique_for_overwrite<token[]> (block_tokens))
  {
    LIBDEFLATE_PRE (static_cast<size_t> (l) < size (levels));

    reset ();
  }

  void compressor::
  reset () noexcept
  {
    LIBDEFLATE_PRE (window_ != nullptr); // Not moved from.

    ending_          = false;
    final_written_   = false;
    end_             = 0;
    pos_             = 0;
    covered_         = 0;
    block_start_     = 0;
    block_lost_      = false;
    previous_length_ = min_match - 1;
    previous_start_  = 0;
    pending_literal_ = false;
    token_count_     = 0;
    bits_            = 0;
    bit_count_       = 0;
    pending_pos_     = 0;
    pending_.clear ();

    ranges::fill (span (head_.get (), hash_size), -1);
    ranges::fill (span (chain_.get (), window_size), -1);

    LIBDEFLATE_ASSERT (invariant ());
  }

  progress compressor::
  next (string_view in, span<char> out, bool end)
  {
    LIBDEFLATE_PRE (window_ != nullptr); // Not moved from.
    LIBDEFLATE_PRE (!ending_ || end);
    LIBDEFLATE_INVARIANT (invariant ());

    ending_ = end;

    size_t ip (0);
    size_t op (0);

    // Write as much of the pending compressed data to the output as fits.
    //
    auto drain = [this, &out, &op] ()
    {
      const size_t n (min (pending_.size () - pending_pos_, out.size () - op));

      memcpy (out.data () + op, pending_.data () + pending_pos_, n);
      op += n;
      pending_pos_ += n;

      if (pending_pos_ == pending_.size ())
      {
        pending_.clear ();
        pending_pos_ = 0;
      }
    };

    // Alternate between taking the input into the window, compressing what
    // we can of it, and writing the result, until the output fills up or
    // nothing more can be done without more input.
    //
    for (;;)
    {
      drain ();

      if (!pending_.empty () || final_written_)
        break;

      // Take as much of the input as fits into the window.
      //
      {
        const size_t n (min (in.size () - ip, buffer_size - end_));

        memcpy (window_.get () + end_, in.data () + ip, n);
        end_ += n;
        ip += n;
      }

      // If this is all of the data, then compress it to the end and finish
      // with the final block.
      //
      const bool flush (end && ip == in.size ());

      search (flush);

      if (flush)
      {
        LIBDEFLATE_ASSERT (pos_ == end_);

        write_block (true);
        align ();
        final_written_ = true;
        continue;
      }

      if (pending_.empty () && ip == in.size ())
        break;
    }

    return progress {ip, op};
  }

  void compressor::
  search (bool flush)
  {
    const search_parameters& sp (levels[static_cast<size_t> (level_)]);

    // Move past a match that we have recorded, inserting the strings within
    // it except for the first, which is already in.
    //
    auto skip = [this] (size_t e)
    {
      for (++pos_; pos_ != e; ++pos_)
      {
        if (end_ - pos_ >= min_match)
          insert (pos_);
      }
    };

    for (;;)
    {
      // Make room for more data once we are close to the end of the window
      // buffer. We do it at a set position, which keeps the result
      // independent of how the data is split into parts.
      //
      if (pos_ >= buffer_size - min_lookahead)
        slide ();

      const size_t lookahead (end_ - pos_);

      if (lookahead < min_lookahead && !flush)
        return;

      if (lookahead == 0)
        break;

      // Level 0 only stores the data.
      //
      if (level_ == compression_level::store)
      {
        record_literal (pos_++);
        continue;
      }

      // Insert the string at this position and look for the longest match
      // among the earlier strings of the same hash.
      //
      const int32_t head (lookahead >= min_match ? insert (pos_) : -1);

      size_t length (min_match - 1);
      size_t start (0);

      if (head >= 0 && (sp.lazy == 0 || previous_length_ < sp.lazy))
        length = longest_match (head,
                                sp.lazy == 0 ? min_match - 1 : previous_length_,
                                start);

      if (length < min_match)
        length = min_match - 1;

      // Without lazy matching, record the match or the literal right away.
      //
      if (sp.lazy == 0)
      {
        if (length >= min_match)
        {
          record_match (pos_, length, pos_ - start);
          skip (pos_ + length);
        }
        else
          record_literal (pos_++);

        continue;
      }

      // With lazy matching, if the previous position's match is at least
      // as long as this one's, then record it. Otherwise, the previous
      // position is a literal, and this one's match waits for the next
      // position to compare with.
      //
      if (previous_length_ >= min_match && length <= previous_length_)
      {
        const size_t p (pos_ - 1);

        record_match (p, previous_length_, p - previous_start_);
        skip (p + previous_length_);

        pending_literal_ = false;
        previous_length_ = min_match - 1;
      }
      else
      {
        if (pending_literal_)
          record_literal (pos_ - 1);

        pending_literal_ = true;
        previous_length_ = length;
        previous_start_  = start;
        ++pos_;
      }
    }

    // We are at the end of the data. A match at the previous position would
    // have had nowhere to extend to, so all that can be left is its
    // literal.
    //
    if (pending_literal_)
    {
      record_literal (pos_ - 1);
      pending_literal_ = false;
    }

    previous_length_ = min_match - 1;
  }

  size_t compressor::
  longest_match (int32_t head, size_t shortest, size_t& start) const noexcept
  {
    const search_parameters& sp (levels[static_cast<size_t> (level_)]);

    const char* const w (window_.get ());
    const char* const a (w + pos_);

    const size_t longest (min (max_match, end_ - pos_));
    const size_t nice (min<size_t> (sp.nice, longest));
    const size_t lowest (pos_ > max_distance ? pos_ - max_distance : 0);

    size_t r (shortest);

    if (r >= longest)
      return r;

    unsigned chain (r >= sp.good ? sp.chain / 4 : sp.chain);

    for (int32_t c (head);
         c >= 0 && static_cast<size_t> (c) >= lowest && chain != 0;
         c = chain_[static_cast<size_t> (c) & (window_size - 1)], --chain)
    {
      const char* const b (w + c);

      // Skip the candidates that can't be longer before comparing them in
      // full.
      //
      if (b[r] != a[r] || b[0] != a[0] || b[1] != a[1])
        continue;

      size_t n (2);
      while (n != longest && a[n] == b[n])
        ++n;

      if (n > r)
      {
        r = n;
        start = static_cast<size_t> (c);

        if (r >= nice)
          break;
      }
    }

    return r;
  }

  int32_t compressor::
  insert (size_t p) noexcept
  {
    LIBDEFLATE_PRE (end_ - p >= min_match);

    const auto at = [this, p] (size_t i)
    {
      return static_cast<size_t> (static_cast<unsigned char> (window_[p + i]));
    };

    const size_t h (((at (0) << 10) ^ (at (1) << 5) ^ at (2)) &
                    (hash_size - 1));

    const int32_t r (head_[h]);
    chain_[p & (window_size - 1)] = r;
    head_[h] = static_cast<int32_t> (p);
    return r;
  }

  void compressor::
  slide () noexcept
  {
    LIBDEFLATE_PRE (pos_ >= window_size && covered_ >= window_size);
    LIBDEFLATE_PRE (previous_length_ < min_match ||
                    previous_start_ >= window_size);

    memmove (window_.get (), window_.get () + window_size, end_ - window_size);

    end_     -= window_size;
    pos_     -= window_size;
    covered_ -= window_size;

    if (previous_length_ >= min_match)
      previous_start_ -= window_size;

    // If the block started in the first half, then its data is gone and it
    // can no longer be stored.
    //
    if (block_start_ >= window_size)
      block_start_ -= window_size;
    else
    {
      block_start_ = 0;
      block_lost_  = true;
    }

    const int32_t ws (static_cast<int32_t> (window_size));
    const auto rebase = [ws] (int32_t& p) {p = p >= ws ? p - ws : -1;};

    ranges::for_each (span (head_.get (), hash_size), rebase);
    ranges::for_each (span (chain_.get (), window_size), rebase);
  }

  void compressor::
  record_literal (size_t p)
  {
    LIBDEFLATE_PRE (p == covered_);

    tokens_[token_count_++] = token {
      static_cast<uint16_t> (static_cast<unsigned char> (window_[p])), 0};

    covered_ = p + 1;

    if (token_count_ == block_tokens)
      write_block (false);
  }

  void compressor::
  record_match (size_t p, size_t length, size_t distance)
  {
    LIBDEFLATE_PRE (p == covered_);
    LIBDEFLATE_PRE (length >= min_match && length <= max_match);
    LIBDEFLATE_PRE (distance != 0 && distance <= max_distance);

    tokens_[token_count_++] = token {static_cast<uint16_t> (length),
                                     static_cast<uint16_t> (distance)};

    covered_ = p + length;

    if (token_count_ == block_tokens)
      write_block (false);
  }

  void compressor::
  write_block (bool final)
  {
    const span<const token> ts (tokens_.get (), token_count_);

    // Count the symbols and the extra bits, which are the same whatever the
    // codes.
    //
    uint32_t lf[literal_symbols]  {};
    uint32_t df[distance_symbols] {};
    uint64_t extra (0);

    for (const token& t: ts)
    {
      if (t.distance == 0)
      {
        ++lf[t.value];
        continue;
      }

      const size_t li (length_index (t.value));
      const size_t di (distance_index (t.distance));

      ++lf[first_length_symbol + li];
      ++df[di];
      extra += length_extra[li] + distance_extra[di];
    }

    lf[end_of_block] = 1;

    // Calculate the size of the block in each form, in bits.
    //
    auto data_bits = [&lf, &df, extra] (auto literal_length,
                                        auto distance_length)
    {
      uint64_t r (extra);

      for (size_t s (0); s != literal_symbols; ++s)
        r += static_cast<uint64_t> (lf[s]) * literal_length (s);

      for (size_t s (0); s != distance_symbols; ++s)
        r += static_cast<uint64_t> (df[s]) * distance_length (s);

      return r;
    };

    const huffman_encoder& fl (fixed_literal_code ());
    const huffman_encoder& fd (fixed_distance_code ());

    const uint64_t fixed_bits (
      3 + data_bits ([&fl] (size_t s) {return fl.length (s);},
                     [&fd] (size_t s) {return fd.length (s);}));

    const dynamic_header h (plan_dynamic (lf, df));

    const uint64_t dynamic_bits (
      3 + h.bits +
      data_bits ([&h] (size_t s) {return h.literal_lengths[s];},
                 [&h] (size_t s) {return h.distance_lengths[s];}));

    // A stored block holds up to max_stored bytes, so the data may take
    // several of them, each with its header padded to a byte boundary.
    //
    const size_t n (covered_ - block_start_);

    uint64_t stored_bits (numeric_limits<uint64_t>::max ());

    if (!block_lost_)
    {
      const size_t blocks (max<size_t> (1, (n + max_stored - 1) / max_stored));
      const uint64_t pad ((8 - (bit_count_ + 3) % 8) % 8);

      stored_bits = 3 + pad + 32 + (blocks - 1) * (3 + 5 + 32) + 8 * n;
    }

    // Write the block in the smallest form, preferring the simpler one if
    // two are as small. Level 0 only stores.
    //
    if (level_ == compression_level::store ||
        (stored_bits <= fixed_bits && stored_bits <= dynamic_bits))
    {
      LIBDEFLATE_ASSERT (!block_lost_);

      size_t p (block_start_);

      do
      {
        const size_t m (min (covered_ - p, max_stored));
        const bool last (p + m == covered_);

        write_bits (final && last ? 1 : 0, 1);
        write_bits (0, 2);
        align ();
        write_bits (m, 16);
        write_bits (m ^ 0xffff, 16);

        pending_.append (window_.get () + p, m);
        p += m;
      }
      while (p != covered_);
    }
    else
    {
      const bool fixed (fixed_bits <= dynamic_bits);

      write_bits (final ? 1 : 0, 1);
      write_bits (fixed ? 1 : 2, 2);

      huffman_encoder dl;
      huffman_encoder dd;

      if (!fixed)
      {
        // Write the header and build the codes it describes.
        //
        huffman_encoder cl;
        cl.build (h.code_length_lengths);

        write_bits (h.literal_count - first_length_symbol, 5);
        write_bits (h.distance_count - 1, 5);
        write_bits (h.code_length_count - 4, 4);

        for (size_t i (0); i != h.code_length_count; ++i)
          write_bits (h.code_length_lengths[code_length_order[i]], 3);

        for (const dynamic_header::run& r: h.runs)
        {
          write_bits (cl.code (r.symbol), cl.length (r.symbol));
          write_bits (r.extra, code_length_extra (r.symbol));
        }

        dl.build (h.literal_lengths);
        dd.build (h.distance_lengths);
      }

      const huffman_encoder& lc (fixed ? fl : dl);
      const huffman_encoder& dc (fixed ? fd : dd);

      // Write the literals and matches, and the end of the block.
      //
      for (const token& t: ts)
      {
        if (t.distance == 0)
        {
          write_bits (lc.code (t.value), lc.length (t.value));
          continue;
        }

        const size_t li (length_index (t.value));
        const size_t ls (first_length_symbol + li);

        write_bits (lc.code (ls), lc.length (ls));
        write_bits (static_cast<uint64_t> (t.value - length_base[li]),
                    length_extra[li]);

        const size_t di (distance_index (t.distance));

        write_bits (dc.code (di), dc.length (di));
        write_bits (static_cast<uint64_t> (t.distance - distance_base[di]),
                    distance_extra[di]);
      }

      write_bits (lc.code (end_of_block), lc.length (end_of_block));
    }

    // Start the next block.
    //
    token_count_ = 0;
    block_start_ = covered_;
    block_lost_  = false;
  }

  void compressor::
  write_bits (uint64_t v, unsigned n)
  {
    LIBDEFLATE_PRE (n <= 32 && (n == 32 || v >> n == 0));

    bits_ |= v << bit_count_;
    bit_count_ += n;

    for (; bit_count_ >= 8; bit_count_ -= 8, bits_ >>= 8)
      pending_.push_back (static_cast<char> (bits_ & 0xff));
  }

  void compressor::
  align ()
  {
    if (bit_count_ != 0)
      write_bits (0, 8 - bit_count_);
  }

  bool compressor::
  invariant () const noexcept
  {
    // The window holds the data being searched and the recorded part of it,
    // the block stays within the recorded part, whole bytes are written
    // out as soon as they are complete, and the final block requires the
    // end of the data.
    //
    return covered_ <= pos_ && pos_ <= end_ && end_ <= buffer_size &&
           (block_lost_ || block_start_ <= covered_) &&
           token_count_ < block_tokens &&
           bit_count_ < 8 &&
           pending_pos_ <= pending_.size () &&
           (!final_written_ || ending_);
  }

  string
  compress (string_view in, compression_level l)
  {
    compressor c (l);

    // Compress into the result, growing it as it fills up. Since the
    // compressor is told that the input is all there is, it finishes once
    // it has written everything.
    //
    const size_t initial (4096);

    string r;
    size_t n (0);

    while (!c.finished ())
    {
      if (n == r.size ())
        r.resize (max ({r.size () * 2, in.size () / 2, initial}));

      const progress p (
        c.next (in, span<char> (r.data (), r.size ()).subspan (n), true));

      in.remove_prefix (p.consumed);
      n += p.produced;
    }

    r.resize (n);
    return r;
  }
}
