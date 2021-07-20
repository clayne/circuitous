/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#include <z3++.h>
#include <circuitous/IR/SMT.hpp>

namespace circ
{
  void PrintSMT(std::ostream &os, Circuit *circuit)
  {
    try {
      IRToSMTVisitor visitor;
      auto expr = visitor.Visit(circuit);
      z3::solver solver(visitor.ctx);
      solver.add(expr);

      os << solver.to_smt2() << '\n';
    } catch (const z3::exception &e) {
      LOG(FATAL) << e.what() << '\n';
    }
  }
}  // namespace circ
