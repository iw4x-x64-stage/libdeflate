// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <span>
#include <print>
#include <string>
#include <vector>
#include <cstddef>   // size_t
#include <cstdint>   // uint8_t, uint64_t
#include <sstream>
#include <iostream>
#include <iterator>  // istreambuf_iterator
#include <optional>
#include <algorithm> // ranges::all_of(), min()
#include <stdexcept> // invalid_argument
#include <string_view>

#include <libdeflate/types.hxx>
#include <libdeflate/decompressor.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace deflate;

namespace
{
  // Assembly field that we cannot make sense of.
  //
  struct invalid_assembly
  {
    string field;
  };

  // Data being assembled bit by bit.
  //
  class bit_writer
  {
  public:
    void
    append (bool b)
    {
      if (n_ % 8 == 0)
        data_.push_back ('\0');

      if (b)
        data_.back () = static_cast<char> (
          static_cast<unsigned char> (data_.back ()) | (1u << (n_ % 8)));

      ++n_;
    }

    bool
    aligned () const noexcept {return n_ % 8 == 0;}

    const string&
    data () const noexcept {return data_;}

  private:
    string data_;
    size_t n_ = 0;
  };

  // Return the value of the field part as a number in the specified base or
  // throw invalid_assembly.
  //
  uint64_t
  parse_number (const string& f, string_view s, int base)
  {
    if (s.empty ())
      throw invalid_assembly {f};

    try
    {
      size_t n (0);
      const uint64_t r (stoull (string (s), &n, base));

      if (n != s.size ())
        throw invalid_assembly {f};

      return r;
    }
    catch (const invalid_argument&)
    {
      throw invalid_assembly {f};
    }
    catch (const out_of_range&)
    {
      throw invalid_assembly {f};
    }
  }

  // Assemble the fields starting from i, stopping at the end or, if nested,
  // at the closing ']*<n>' field, which is left for the caller.
  //
  void
  assemble (const vector<string>& fs, size_t& i, bit_writer& w, bool nested)
  {
    for (; i != fs.size (); ++i)
    {
      const string& f (fs[i]);

      if (f.starts_with ("]*"))
      {
        if (!nested)
          throw invalid_assembly {f};

        return;
      }

      if (f == "[")
      {
        const size_t b (++i);

        bit_writer t;
        assemble (fs, i, t, true);

        if (i == fs.size ())
          throw invalid_assembly {"["};

        // Assemble the group again once for each repetition so that its
        // fields land at the right bit positions.
        //
        const string& r (fs[i]);
        const uint64_t n (parse_number (r, string_view (r).substr (2), 10));

        for (uint64_t k (0); k != n; ++k)
        {
          size_t j (b);
          assemble (fs, j, w, true);
        }

        continue;
      }

      if (f == "align")
      {
        while (!w.aligned ())
          w.append (false);

        continue;
      }

      if (f[0] == 'c')
      {
        const string_view s (string_view (f).substr (1));

        if (s.empty () ||
            !ranges::all_of (s, [] (char c) {return c == '0' || c == '1';}))
          throw invalid_assembly {f};

        for (char c: s)
          w.append (c == '1');

        continue;
      }

      if (f[0] == 'x')
      {
        const string_view s (string_view (f).substr (1));

        if (!w.aligned () || s.size () % 2 != 0)
          throw invalid_assembly {f};

        for (size_t p (0); p != s.size (); p += 2)
        {
          const uint64_t v (parse_number (f, s.substr (p, 2), 16));

          for (unsigned k (0); k != 8; ++k)
            w.append ((v >> k) & 1);
        }

        continue;
      }

      // <n>:<v>
      //
      const size_t p (f.find (':'));

      if (p == string::npos)
        throw invalid_assembly {f};

      const uint64_t n (parse_number (f, string_view (f).substr (0, p), 10));
      const uint64_t v (parse_number (f, string_view (f).substr (p + 1), 10));

      if (n == 0 || n > 32 || v >> n != 0)
        throw invalid_assembly {f};

      for (unsigned k (0); k != n; ++k)
        w.append ((v >> k) & 1);
    }
  }

  // Decompress the data feeding the decompressor n bytes at a time with
  // room for n bytes of output. Throw invalid_data the way decompress()
  // does.
  //
  string
  decompress_chunks (string_view d, size_t n)
  {
    decompressor z;
    string r;

    string buf (n, '\0');

    for (size_t p (0); !z.finished ();)
    {
      const string_view in (d.substr (p, n));
      const bool end (p + in.size () == d.size ());

      const progress g (z.next (in, span<char> (buf), end));

      assert (g.consumed <= in.size () && g.produced <= n);

      r.append (buf, 0, g.produced);
      p += g.consumed;

      if (z.finished () && p != d.size ())
        throw invalid_data ("junk after deflate data");
    }

    return r;
  }
}

// Usage: argv[0] [--chunk <n>] [--size | --tail <n>]
//
// Read deflate data in the assembly form described below from stdin,
// decompress it with decompress(), and print the result to stdout.
//
// --chunk <n>
//
//   Decompress with a decompressor object instead, passing it n bytes of
//   the data at a time with room for n bytes of output.
//
// --size
//
//   Print the size of the result instead of the result.
//
// --tail <n>
//
//   Print the last n bytes of the result instead of the result.
//
// The assembly form is a sequence of whitespace-separated fields, each of
// which appends bits to the data, starting from the least significant bit
// of the first byte. A '#' starts a comment that runs to the end of the
// line, and the last byte is padded with zero bits.
//
// <n>:<v>
//
//   The value v in n bits, least significant bit first, which is how the
//   format packs its header fields and extra bits.
//
// c<bits>
//
//   The bits of a Huffman codeword, first bit first, which is how the format
//   packs Huffman codes.
//
// align
//
//   Zero bits up to the next byte boundary.
//
// x<hex>
//
//   Bytes written in hex, starting at a byte boundary.
//
// [ <fields> ]*<n>
//
//   The fields repeated n times.
//
int
main (int argc, char* argv[])
{
  optional<size_t> chunk;
  optional<size_t> tail;
  bool size (false);

  for (int i (1); i != argc; ++i)
  {
    const string_view a (argv[i]);

    if (a == "--size")
      size = true;
    else if (a == "--chunk" || a == "--tail")
    {
      assert (i + 1 != argc);
      (a == "--chunk" ? chunk : tail) = stoul (argv[++i]);
    }
    else
      assert (false);
  }

  assert (!chunk || *chunk != 0);

  cin.exceptions (ios::badbit);

  // Read and assemble the data, dropping the comments.
  //
  string d;
  try
  {
    vector<string> fs;

    for (string l; getline (cin, l); )
    {
      istringstream is (l.substr (0, l.find ('#')));

      for (string f; is >> f; )
        fs.push_back (move (f));
    }

    bit_writer w;
    size_t i (0);
    assemble (fs, i, w, false);
    d = w.data ();
  }
  catch (const invalid_assembly& e)
  {
    println (cerr, "error: invalid assembly field '{}'", e.field);
    return 1;
  }

  try
  {
    const string r (chunk ? decompress_chunks (d, *chunk) : decompress (d));

    if (size)
      println ("{}", r.size ());
    else if (tail)
      print ("{}", string_view (r).substr (r.size () - min (*tail, r.size ())));
    else
      print ("{}", r);
  }
  catch (const invalid_data& e)
  {
    println (cerr, "error: {}", e.what ());
    return 1;
  }
}
