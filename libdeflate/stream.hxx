// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#pragma once

#include <memory>   // unique_ptr
#include <cstddef>  // size_t
#include <istream>
#include <ostream>
#include <optional>
#include <streambuf>

#include <libdeflate/compressor.hxx>
#include <libdeflate/decompressor.hxx>

#include <libdeflate/export.hxx>

namespace deflate
{
  // Stream buffer that decompresses the deflate data it reads from another
  // stream.
  //
  // To be able to leave what follows the data in the other stream, the
  // buffer only takes what the other stream's buffer already holds, and at
  // the end of the data puts back what it took and didn't use. The other
  // stream's buffer must therefore keep what it has handed out of its
  // current get area available for putting back, as the standard stream
  // buffers do.
  //
  // Note that the buffer reads the other stream's buffer directly, so the
  // other stream's state stays as it is.
  //
  class LIBDEFLATE_SYMEXPORT istreambuf: public std::streambuf
  {
  public:
    // Start decompressing from the stream. If end is true, then the data
    // must end the stream, which is verified at the end of the data.
    // Otherwise, what follows the data is left in the stream.
    //
    void
    open (std::istream&, bool end);

    bool
    is_open () const noexcept {return is_ != nullptr;}

    // Stop decompressing. Nothing more can be read until the buffer is
    // opened again.
    //
    void
    close () noexcept;

  protected:
    // Decompress more of the data. Throw invalid_data if the data is
    // invalid, incomplete, or, if it must end the stream, followed by
    // anything.
    //
    virtual int_type
    underflow () override;

  private:
    // Take what the other stream's buffer holds into the input buffer.
    // Return false if the other stream has ended.
    //
    bool
    load ();

    // Handle the end of the data: verify that the stream ends or put back
    // the unused input.
    //
    void
    finish ();

  private:
    static constexpr std::size_t buffer_size = 16384;

    std::istream*           is_ = nullptr;
    bool                    end_;
    bool                    eof_;     // Other stream has ended.
    bool                    done_;    // End of the data handled.
    decompressor            d_;
    std::unique_ptr<char[]> ib_;      // Input (compressed data).
    std::size_t             in_;      // Input size.
    std::size_t             ip_;      // Input used.
    std::unique_ptr<char[]> ob_;      // Output (get area).
  };

  // Input stream of the data that deflate data in another stream
  // decompresses into. For example:
  //
  // try
  // {
  //   ifstream ifs (..., ios::binary);
  //   ifs.exceptions (ifstream::badbit);
  //
  //   deflate::istream is (ifs, true /* end */);
  //   ... // Read from is.
  // }
  // catch (const deflate::invalid_data& e)
  // {
  //   ... // Invalid deflate data, see e.what () for the description.
  // }
  //
  // The badbit exception must be enabled, since that is how the errors of
  // the decompression and of the other stream come through.
  //
  class LIBDEFLATE_SYMEXPORT istream: public std::istream
  {
  public:
    explicit
    istream (iostate = badbit | failbit);

    istream (std::istream&, bool end, iostate = badbit | failbit);

    // See istreambuf::open() for details.
    //
    void
    open (std::istream& is, bool end) {buf_.open (is, end);}

    bool
    is_open () const noexcept {return buf_.is_open ();}

    void
    close () noexcept {buf_.close ();}

  private:
    istreambuf buf_;
  };

  // Stream buffer that compresses what is written to it into deflate data
  // that it writes to another stream.
  //
  // Note that syncing the buffer does nothing, since the compressor only
  // writes what it has compressed in whole blocks.
  //
  class LIBDEFLATE_SYMEXPORT ostreambuf: public std::streambuf
  {
  public:
    // Start compressing into the stream at the level, which must be in the
    // range from 0 to 9. The stream must have the badbit and failbit
    // exceptions enabled.
    //
    void
    open (std::ostream&, compression_level);

    bool
    is_open () const noexcept {return os_ != nullptr;}

    // Compress the rest of what was written and write the end of the
    // compressed data to the other stream.
    //
    void
    close ();

  protected:
    virtual int_type
    overflow (int_type) override;

  private:
    // Compress what is in the put area into the other stream, finishing the
    // compressed data if end is true.
    //
    void
    save (bool end);

  private:
    static constexpr std::size_t buffer_size = 16384;

    std::ostream*             os_ = nullptr;
    std::optional<compressor> c_;
    std::unique_ptr<char[]>   ib_; // Input (put area).
    std::unique_ptr<char[]>   ob_; // Output (compressed data).
  };

  // Output stream that compresses what is written to it into deflate data
  // in another stream. For example:
  //
  // ofstream ofs (..., ios::binary);
  // ofs.exceptions (ofstream::badbit | ofstream::failbit);
  //
  // deflate::ostream os (ofs, compression_level::smallest);
  // ... // Write to os.
  //
  // os.close ();
  // ofs.close ();
  //
  // Note that close() must be called explicitly, since the compressed data
  // is incomplete until then: the stream must not be open when it is
  // destroyed unless it is not good or an exception is being thrown.
  //
  class LIBDEFLATE_SYMEXPORT ostream: public std::ostream
  {
  public:
    explicit
    ostream (iostate = badbit | failbit);

    ostream (std::ostream&,
             compression_level = compression_level::balanced,
             iostate = badbit | failbit);

    ~ostream () override;

    // See ostreambuf::open() for details.
    //
    void
    open (std::ostream& os,
          compression_level l = compression_level::balanced)
    {
      buf_.open (os, l);
    }

    bool
    is_open () const noexcept {return buf_.is_open ();}

    // Write the end of the compressed data (see ostreambuf::close() for
    // details).
    //
    void
    close () {buf_.close ();}

  private:
    ostreambuf buf_;
  };
}
