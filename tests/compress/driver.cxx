// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <span>
#include <print>
#include <string>
#include <cstdio>    // stdin, stdout
#include <cstddef>   // size_t
#include <cstdint>   // uint8_t, uint64_t
#include <iostream>
#include <iterator>  // istreambuf_iterator
#include <optional>
#include <string_view>

#ifdef _WIN32
#  include <io.h>    // _setmode(), _fileno()
#  include <fcntl.h> // _O_BINARY
#endif

#include <libdeflate/types.hxx>
#include <libdeflate/compressor.hxx>
#include <libdeflate/decompressor.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace deflate;

namespace
{
  // Compress the data feeding the compressor n bytes at a time with room
  // for n bytes of output.
  //
  string
  compress_chunks (string_view d, compression_level l, size_t n)
  {
    compressor c (l);
    string r;

    string buf (n, '\0');

    for (size_t p (0); !c.finished ();)
    {
      const string_view in (d.substr (p, n));
      const bool end (p + in.size () == d.size ());

      const progress g (c.next (in, span<char> (buf), end));

      assert (g.consumed <= in.size () && g.produced <= n);

      r.append (buf, 0, g.produced);
      p += g.consumed;
    }

    return r;
  }

  // Return n pseudo-random bytes, the same ones every time.
  //
  string
  generate_random (size_t n)
  {
    string r (n, '\0');

    uint64_t x (0x9e3779b97f4a7c15);
    for (char& c: r)
    {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c = static_cast<char> (static_cast<uint8_t> (x >> 32));
    }

    return r;
  }
}

// Usage: argv[0] -c <level> [--chunk <n>]
//        argv[0] -d
//        argv[0] -r <size>
//
// -c  compress stdin at the level with compress() and write the result to
//     stdout
//
// --chunk <n>
//
//     compress with a compressor object instead, passing it n bytes of the
//     data at a time with room for n bytes of output
//
// -d  decompress stdin with decompress() and write the result to stdout
//
// -r  write size pseudo-random bytes to stdout, the same ones every time
//
int
main (int argc, char* argv[])
{
  assert (argc >= 2);

  // Pass the data through as it is.
  //
#ifdef _WIN32
  _setmode (_fileno (stdin),  _O_BINARY);
  _setmode (_fileno (stdout), _O_BINARY);
#endif

  const string_view m (argv[1]);

  if (m == "-r")
  {
    assert (argc == 3);
    print ("{}", generate_random (stoul (argv[2])));
    return 0;
  }

  cin.exceptions (ios::badbit);

  const string d (istreambuf_iterator<char> (cin),
                  istreambuf_iterator<char> {});

  if (m == "-d")
  {
    assert (argc == 2);

    try
    {
      print ("{}", decompress (d));
    }
    catch (const invalid_data& e)
    {
      println (cerr, "error: {}", e.what ());
      return 1;
    }

    return 0;
  }

  assert (m == "-c" && (argc == 3 || argc == 5));

  const compression_level l (
    static_cast<compression_level> (stoul (argv[2])));

  optional<size_t> chunk;
  if (argc == 5)
  {
    assert (argv[3] == string_view ("--chunk"));
    chunk = stoul (argv[4]);
    assert (*chunk != 0);
  }

  print ("{}", chunk ? compress_chunks (d, l, *chunk) : compress (d, l));
}
