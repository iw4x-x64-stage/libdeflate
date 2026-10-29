// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <ios>       // ios_base
#include <print>
#include <string>
#include <cstdio>    // stdin, stdout
#include <iostream>
#include <iterator>  // istreambuf_iterator
#include <string_view>

#ifdef _WIN32
#  include <io.h>    // _setmode(), _fileno()
#  include <fcntl.h> // _O_BINARY
#endif

#include <libdeflate/types.hxx>
#include <libdeflate/stream.hxx>
#include <libdeflate/compressor.hxx>

#undef NDEBUG
#include <cassert>

using namespace std;
using namespace deflate;

// Usage: argv[0] -c <level>
//        argv[0] -d [--rest]
//
// -c  compress stdin at the level by writing it a character at a time to a
//     deflate::ostream over stdout
//
// -d  decompress stdin by reading it a character at a time from a
//     deflate::istream over stdin, which ends with the compressed data, and
//     write the result to stdout
//
// --rest
//
//     open the deflate::istream with the data not ending stdin, and after
//     the result write '|' followed by the rest of stdin
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

  cin.exceptions (ios_base::badbit);
  cout.exceptions (ios_base::badbit | ios_base::failbit);

  const string_view m (argv[1]);

  if (m == "-c")
  {
    assert (argc == 3);

    const compression_level l (
      static_cast<compression_level> (stoul (argv[2])));

    deflate::ostream os (cout, l);

    for (char c; cin.get (c); )
      os.put (c);

    os.close ();
    cout.flush ();
    return 0;
  }

  assert (m == "-d" && (argc == 2 || argv[2] == string_view ("--rest")));

  const bool rest (argc == 3);

  try
  {
    deflate::istream is (cin, !rest, ios_base::badbit);

    string r;
    for (char c; is.get (c); )
      r += c;

    is.close ();

    if (rest)
      r += '|' + string (istreambuf_iterator<char> (cin),
                         istreambuf_iterator<char> {});

    print ("{}", r);
  }
  catch (const invalid_data& e)
  {
    println (cerr, "error: {}", e.what ());
    return 1;
  }
}
