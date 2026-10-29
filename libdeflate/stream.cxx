// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <libdeflate/stream.hxx>

#include <ios>         // ios_base
#include <span>
#include <algorithm>   // min(), max()
#include <exception>   // uncaught_exceptions()
#include <string_view>

#include <libdeflate/contract.hxx>

using namespace std;

namespace deflate
{
  // istreambuf
  //

  void istreambuf::
  open (std::istream& is, bool end)
  {
    if (ib_ == nullptr)
    {
      ib_ = make_unique_for_overwrite<char[]> (buffer_size);
      ob_ = make_unique_for_overwrite<char[]> (buffer_size);
    }

    is_   = &is;
    end_  = end;
    eof_  = false;
    done_ = false;
    in_   = 0;
    ip_   = 0;

    d_.reset ();
    setg (ob_.get (), ob_.get (), ob_.get ());
  }

  void istreambuf::
  close () noexcept
  {
    is_ = nullptr;
    setg (nullptr, nullptr, nullptr);
  }

  istreambuf::int_type istreambuf::
  underflow ()
  {
    if (gptr () != egptr ())
      return traits_type::to_int_type (*gptr ());

    if (!is_open () || done_)
      return traits_type::eof ();

    // Decompress until we have some output or the data ends. Each round
    // either uses some input or, having used it all, takes more, so we get
    // somewhere.
    //
    while (!d_.finished ())
    {
      if (ip_ == in_ && !eof_)
        eof_ = !load ();

      const progress p (
        d_.next (string_view (ib_.get () + ip_, in_ - ip_),
                 span<char> (ob_.get (), buffer_size),
                 eof_));

      ip_ += p.consumed;

      if (p.produced != 0)
      {
        setg (ob_.get (), ob_.get (), ob_.get () + p.produced);
        return traits_type::to_int_type (*gptr ());
      }
    }

    finish ();
    return traits_type::eof ();
  }

  bool istreambuf::
  load ()
  {
    std::streambuf& b (*is_->rdbuf ());

    if (traits_type::eq_int_type (b.sgetc (), traits_type::eof ()))
      return false;

    // Note that an unbuffered stream buffer may report nothing available
    // even though it has just told us what its next character is, in which
    // case we take that character.
    //
    const streamsize size (static_cast<streamsize> (buffer_size));
    const streamsize n (max<streamsize> (1, min (b.in_avail (), size)));

    in_ = static_cast<size_t> (b.sgetn (ib_.get (), n));
    ip_ = 0;

    return in_ != 0;
  }

  void istreambuf::
  finish ()
  {
    done_ = true;

    std::streambuf& b (*is_->rdbuf ());

    if (end_)
    {
      const auto eof = [&b] ()
      {
        return traits_type::eq_int_type (b.sgetc (), traits_type::eof ());
      };

      if (ip_ != in_ || (!eof_ && !eof ()))
        throw invalid_data ("junk after deflate data");

      return;
    }

    // Put back what we took and didn't use, last character first.
    //
    for (; in_ != ip_; --in_)
    {
      if (traits_type::eq_int_type (b.sputbackc (ib_[in_ - 1]),
                                    traits_type::eof ()))
        throw ios_base::failure ("unable to put back data after deflate data");
    }
  }

  // istream
  //

  istream::
  istream (iostate e)
    : std::istream (&buf_)
  {
    LIBDEFLATE_PRE ((e & badbit) != 0);
    exceptions (e);
  }

  istream::
  istream (std::istream& is, bool end, iostate e)
    : istream (e)
  {
    open (is, end);
  }

  // ostreambuf
  //

  void ostreambuf::
  open (std::ostream& os, compression_level l)
  {
    LIBDEFLATE_PRE ((os.exceptions () & (std::ostream::badbit |
                                         std::ostream::failbit)) ==
                    (std::ostream::badbit | std::ostream::failbit));

    if (ib_ == nullptr)
    {
      ib_ = make_unique_for_overwrite<char[]> (buffer_size);
      ob_ = make_unique_for_overwrite<char[]> (buffer_size);
    }

    os_ = &os;
    c_.emplace (l);

    // Keep a character of the put area for the one that overflow() gets.
    //
    setp (ib_.get (), ib_.get () + buffer_size - 1);
  }

  void ostreambuf::
  close ()
  {
    if (!is_open ())
      return;

    // Whether or not finishing the data succeeds, the buffer ends up closed
    // since there is no getting back to a consistent state.
    //
    try
    {
      save (true);
    }
    catch (...)
    {
      os_ = nullptr;
      throw;
    }

    os_ = nullptr;
  }

  ostreambuf::int_type ostreambuf::
  overflow (int_type c)
  {
    if (!is_open ())
      return traits_type::eof ();

    if (!traits_type::eq_int_type (c, traits_type::eof ()))
    {
      *pptr () = traits_type::to_char_type (c);
      pbump (1);
    }

    save (false);
    return traits_type::not_eof (c);
  }

  void ostreambuf::
  save (bool end)
  {
    string_view in (pbase (), static_cast<size_t> (pptr () - pbase ()));

    // Compress until all of the input is taken and the compressor has
    // nothing more to write (which we know when it doesn't fill the output)
    // or, at the end, until the compressed data is finished.
    //
    for (;;)
    {
      const progress p (
        c_->next (in, span<char> (ob_.get (), buffer_size), end));

      in.remove_prefix (p.consumed);
      os_->write (ob_.get (), static_cast<streamsize> (p.produced));

      if (end ? c_->finished () : in.empty () && p.produced != buffer_size)
        break;
    }

    setp (ib_.get (), ib_.get () + buffer_size - 1);
  }

  // ostream
  //

  ostream::
  ostream (iostate e)
    : std::ostream (&buf_)
  {
    LIBDEFLATE_PRE ((e & badbit) != 0);
    exceptions (e);
  }

  ostream::
  ostream (std::ostream& os, compression_level l, iostate e)
    : ostream (e)
  {
    open (os, l);
  }

  ostream::
  ~ostream ()
  {
    // Make sure close() was called (see the class for details).
    //
    LIBDEFLATE_PRE (!is_open () || !good () || uncaught_exceptions () != 0);
  }
}
