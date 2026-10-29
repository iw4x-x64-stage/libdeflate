// Copyright (c) the IW4x authors (see the AUTHORS file).
// SPDX-License-Identifier: GPL-3.0-only WITH AdditionRef-IW4x-Exception-1.1

#include <print>
#include <iostream>
#include <algorithm> // ranges::find()
#include <string_view>

// Check at the audit level so that the postcondition and invariant checks
// are compiled in as well.
//
#define LIBDEFLATE_CONTRACT 2
#include <libdeflate/contract.hxx>

using namespace std;
using namespace deflate;

namespace
{
  [[noreturn]] void
  throwing_handler (const contract_violation& v)
  {
    throw v;
  }

  void
  violate_precondition ()
  {
    const int n (0);
    LIBDEFLATE_PRE (n != 0);
  }

  void
  violate_postcondition ()
  {
    int r (1);
    LIBDEFLATE_POST (r != 0);
    r = 0;
  }

  // Counter that must never go negative.
  //
  class counter
  {
  public:
    void
    decrement ()
    {
      LIBDEFLATE_INVARIANT (invariant ());
      --n_;
    }

  private:
    bool
    invariant () const noexcept {return n_ >= 0;}

    int n_ = 0;
  };

  void
  violate_invariant ()
  {
    counter c;
    c.decrement ();
  }

  void
  violate_assertion ()
  {
    volatile int n (0);
    LIBDEFLATE_ASSERT (n != 0);
  }

  void
  violate_unreachable ()
  {
    LIBDEFLATE_UNREACHABLE ();
  }

  struct check
  {
    string_view name;
    void (*violate) ();
  };

  const check checks[]
  {
    {"precondition",  &violate_precondition},
    {"postcondition", &violate_postcondition},
    {"invariant",     &violate_invariant},
    {"assertion",     &violate_assertion},
    {"unreachable",   &violate_unreachable}
  };
}

// Usage: argv[0] <check>
//
// Violate the check (precondition, postcondition, invariant, assertion, or
// unreachable) and print the violation that the handler receives as
// '<kind>: <expression>[ (<message>)]'.
//
int
main (int argc, char* argv[])
{
  if (argc != 2)
  {
    println (cerr, "error: expected check name");
    return 1;
  }

  const string_view n (argv[1]);
  const auto c (ranges::find (checks, n, &check::name));

  if (c == ranges::end (checks))
  {
    println (cerr, "error: unknown check '{}'", n);
    return 1;
  }

  set_contract_handler (&throwing_handler);

  try
  {
    c->violate ();
  }
  catch (const contract_violation& v)
  {
    if (v.message != nullptr)
      println ("{}: {} ({})",
               contract_kind_name (v.kind),
               v.expression,
               v.message);
    else
      println ("{}: {}", contract_kind_name (v.kind), v.expression);

    return 0;
  }

  println (cerr, "error: check '{}' was not violated", n);
  return 1;
}
