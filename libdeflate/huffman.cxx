// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libdeflate/huffman.hxx>

#include <vector>
#include <algorithm> // ranges::fill(), ranges::count_if(), ranges::sort()

#include <libdeflate/contract.hxx>

using namespace std;

namespace deflate
{
  namespace
  {
    // Return the first n bits of the code in the reverse order.
    //
    unsigned
    reverse (unsigned code, unsigned n) noexcept
    {
      unsigned r (0);

      for (unsigned i (0); i != n; ++i, code >>= 1)
        r = (r << 1) | (code & 1);

      return r;
    }
  }

  const char*
  to_string (huffman_shape s) noexcept
  {
    switch (s)
    {
      case huffman_shape::complete:        return "complete";
      case huffman_shape::incomplete:      return "incomplete";
      case huffman_shape::over_subscribed: return "over-subscribed";
      case huffman_shape::empty:           return "empty";
    }

    LIBDEFLATE_UNREACHABLE ();
  }

  huffman_shape
  huffman_measure (span<const uint8_t> lengths) noexcept
  {
    unsigned count[huffman_max_length + 1] {};

    for (uint8_t l: lengths)
    {
      LIBDEFLATE_PRE (l <= huffman_max_length);
      ++count[l];
    }

    if (count[0] == lengths.size ())
      return huffman_shape::empty;

    // Each code of length l takes 2^-l of the code space. Count what is left
    // in units of the longest code's share, length by length, so that we
    // notice over-subscription before the count could overflow.
    //
    long left (1);

    for (unsigned l (1); l <= huffman_max_length; ++l)
    {
      left = (left << 1) - count[l];

      if (left < 0)
        return huffman_shape::over_subscribed;
    }

    return left == 0 ? huffman_shape::complete : huffman_shape::incomplete;
  }

  bool
  huffman_single (span<const uint8_t> lengths) noexcept
  {
    return ranges::count_if (lengths, [] (uint8_t l) {return l != 0;}) == 1 &&
           ranges::count (lengths, 1) == 1;
  }

  void
  huffman_lengths (span<const uint32_t> frequencies,
                   unsigned limit,
                   span<uint8_t> lengths)
  {
    LIBDEFLATE_PRE (frequencies.size () == lengths.size ());
    LIBDEFLATE_PRE (frequencies.size () <= huffman_max_symbols);
    LIBDEFLATE_PRE (limit != 0 && limit <= huffman_max_length);

    ranges::fill (lengths, 0);

    // Collect the symbols that occur, ordered by frequency and then by
    // symbol, so that the result depends on nothing else.
    //
    struct leaf
    {
      uint64_t weight;
      uint16_t symbol;
    };

    vector<leaf> leaves;
    for (size_t s (0); s != frequencies.size (); ++s)
    {
      if (frequencies[s] != 0)
        leaves.push_back (leaf {frequencies[s], static_cast<uint16_t> (s)});
    }

    const size_t n (leaves.size ());

    LIBDEFLATE_PRE (n <= (static_cast<size_t> (1) << limit));

    if (n == 0)
      return;

    if (n == 1)
    {
      lengths[leaves.front ().symbol] = 1;
      return;
    }

    ranges::sort (leaves,
                  [] (const leaf& x, const leaf& y)
                  {
                    return x.weight != y.weight
                      ? x.weight < y.weight
                      : x.symbol < y.symbol;
                  });

    // Compute the lengths with the package-merge algorithm, which finds the
    // optimal code of the limited length.
    //
    // We build a list of items for each of the limit levels, starting from
    // the longest codes. The first list is the leaves. Each next one is the
    // leaves merged by weight with the packages of the previous list's
    // items, taken in consecutive pairs. Out of the last list we then
    // select the first 2n - 2 items, and the length of a symbol is the
    // number of selected items its leaf takes part in.
    //
    // Note that since the merge keeps the packages in order, the selected
    // packages of a list are always its first ones, and so are the items
    // they are made of in the previous list. So we can count the leaves
    // list by list, going back from the last one.
    //
    struct item
    {
      uint64_t weight;
      int32_t  leaf; // Index of the leaf or -1 if it is a package.
    };

    vector<vector<item>> lists (limit);

    for (size_t i (0); i != n; ++i)
      lists[0].push_back (item {leaves[i].weight, static_cast<int32_t> (i)});

    for (unsigned k (1); k != limit; ++k)
    {
      const vector<item>& p (lists[k - 1]);
      vector<item>& r (lists[k]);

      size_t i (0); // Next leaf.
      size_t j (0); // Next package (pair of items in p).

      while (i != n || j + 1 < p.size ())
      {
        const bool package (j + 1 < p.size ());

        if (i != n &&
            (!package || leaves[i].weight <= p[j].weight + p[j + 1].weight))
        {
          r.push_back (item {leaves[i].weight, static_cast<int32_t> (i)});
          ++i;
        }
        else
        {
          r.push_back (item {p[j].weight + p[j + 1].weight, -1});
          j += 2;
        }
      }
    }

    for (size_t m (2 * n - 2), k (limit); k != 0; --k)
    {
      const vector<item>& l (lists[k - 1]);

      LIBDEFLATE_ASSERT (m <= l.size ());

      size_t packages (0);
      for (size_t i (0); i != m; ++i)
      {
        if (l[i].leaf >= 0)
          ++lengths[leaves[static_cast<size_t> (l[i].leaf)].symbol];
        else
          ++packages;
      }

      m = 2 * packages;
    }
  }

  huffman_encoder::
  huffman_encoder () noexcept
    : code_ {}, length_ {}
  {
  }

  void huffman_encoder::
  build (span<const uint8_t> lengths) noexcept
  {
    LIBDEFLATE_PRE (lengths.size () <= huffman_max_symbols);
    LIBDEFLATE_PRE (huffman_measure (lengths) == huffman_shape::complete ||
                    huffman_measure (lengths) == huffman_shape::empty    ||
                    huffman_single (lengths));

    ranges::fill (code_, 0);
    ranges::fill (length_, 0);

    // Assign the codes the way the format describes it (3.2.2): count the
    // codes of each length, find the first code of each length, and give
    // the symbols of each length consecutive codes in the symbol order.
    //
    unsigned count[huffman_max_length + 1] {};

    for (uint8_t l: lengths)
      ++count[l];

    count[0] = 0;

    unsigned next[huffman_max_length + 1] {};

    for (unsigned code (0), l (1); l <= huffman_max_length; ++l)
    {
      code = (code + count[l - 1]) << 1;
      next[l] = code;
    }

    for (size_t s (0); s != lengths.size (); ++s)
    {
      if (const uint8_t l = lengths[s]; l != 0)
      {
        code_[s]   = static_cast<uint16_t> (reverse (next[l]++, l));
        length_[s] = l;
      }
    }
  }

  huffman_decoder::
  huffman_decoder () noexcept
    : table_ {}, count_ {}, symbol_ {}, max_length_ (0)
  {
  }

  void huffman_decoder::
  build (span<const uint8_t> lengths) noexcept
  {
    LIBDEFLATE_PRE (lengths.size () <= huffman_max_symbols);
    LIBDEFLATE_PRE (huffman_measure (lengths) == huffman_shape::complete ||
                    huffman_measure (lengths) == huffman_shape::empty    ||
                    huffman_single (lengths));

    // Count the codes of each length.
    //
    ranges::fill (count_, 0);

    for (uint8_t l: lengths)
      ++count_[l];

    count_[0] = 0;

    max_length_ = 0;
    for (unsigned l (1); l <= huffman_max_length; ++l)
    {
      if (count_[l] != 0)
        max_length_ = l;
    }

    // Order the symbols by codeword, which in a canonical code is by length
    // and then by symbol.
    //
    {
      uint16_t offset[huffman_max_length + 1] {};

      for (unsigned l (1); l != huffman_max_length; ++l)
        offset[l + 1] = offset[l] + count_[l];

      for (size_t s (0); s != lengths.size (); ++s)
      {
        if (lengths[s] != 0)
          symbol_[offset[lengths[s]]++] = static_cast<uint16_t> (s);
      }
    }

    // Fill the table with the codewords that fit into it. Each codeword of
    // length l appears in every entry whose first l bits are the codeword
    // reversed.
    //
    // The entries that remain are the beginnings of the longer codewords
    // and, in a single code of length one, the unused codeword. Both leave
    // decode() to walk the code.
    //
    ranges::fill (table_, entry {unused, 0});

    unsigned code (0);
    size_t i (0);

    for (unsigned l (1); l <= table_bits; ++l, code <<= 1)
    {
      for (unsigned k (0); k != count_[l]; ++k, ++code)
      {
        const entry e {symbol_[i++], static_cast<uint8_t> (l)};

        for (unsigned j (reverse (code, l)); j < size (table_); j += 1u << l)
          table_[j] = e;
      }
    }
  }

  huffman_decoder::result huffman_decoder::
  decode (uint64_t bits, unsigned n) const noexcept
  {
    LIBDEFLATE_PRE (max_length_ != 0);
    LIBDEFLATE_PRE (n < 64 && bits >> n == 0);

    const entry& e (table_[bits & ((1u << table_bits) - 1)]);

    if (e.length != 0)
      return e.length <= n ? result {e.symbol, e.length} : result {0, 0};

    // Walk the canonical code a bit at a time. The codes of each length are
    // consecutive numbers starting from first, which follows the last code
    // of the previous length shifted by one bit.
    //
    int code (0);
    int first (0);
    int index (0);

    for (unsigned l (1); l <= max_length_; ++l)
    {
      if (l > n)
        return result {0, 0};

      code |= static_cast<int> ((bits >> (l - 1)) & 1);

      const int count (count_[l]);

      if (code - first < count)
        return result {symbol_[index + code - first],
                       static_cast<uint8_t> (l)};

      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }

    // The bits don't match any code, which is only possible with the unused
    // codeword of an incomplete code.
    //
    return result {unused, static_cast<uint8_t> (max_length_)};
  }
}
