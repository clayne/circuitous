/*
 * Copyright (c) 2020 Trail of Bits, Inc.
 */

#include <circuitous/IR/IR.h>
#include <circuitous/Printers.h>
#include <circuitous/Transforms.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <fstream>
#include <iostream>

DECLARE_string(arch);
DECLARE_string(os);
DEFINE_string(binary_in, "",
              "Path to a file containing only machine code instructions.");
DEFINE_string(ir_in, "", "Path to a file containing serialized IR.");
DEFINE_string(ir_out, "", "Path to the output IR file.");
DEFINE_string(dot_out, "", "Path to the output GraphViz DOT file.");
DEFINE_string(python_out, "", "Path to the output Python file.");
DEFINE_string(smt_out, "", "Path to the output SMT-LIB2 file.");
DEFINE_string(json_out, "", "Path to the output JSON file.");

int main(int argc, char *argv[]) {
  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);

  std::unique_ptr<circuitous::Circuit> circuit;

  if (!FLAGS_binary_in.empty()) {
    circuitous::Circuit::CreateFromInstructions(FLAGS_arch, FLAGS_os,
                                                FLAGS_binary_in)
        .swap(circuit);

  } else if (!FLAGS_ir_in.empty()) {
    if (FLAGS_ir_in == "-") {
      FLAGS_ir_in = "/dev/stdin";
    }

    std::ifstream is(FLAGS_ir_in, std::ios::binary);
    circuitous::Circuit::Deserialize(is).swap(circuit);

  } else {
    std::cerr << "Expected one of `--binary_in` or `--ir_in`" << std::endl;
    return EXIT_FAILURE;
  }

  if (!circuit) {
    std::cerr << "Failed to get circuit IR" << std::endl;
    return EXIT_FAILURE;
  }

  ConvertPopCountToParity(circuit.get());
  StrengthReducePopulationCount(circuit.get());

  if (!FLAGS_ir_out.empty()) {
    if (FLAGS_ir_out == "-") {
      FLAGS_ir_out = "/dev/stdout";
    }

    std::ofstream os(FLAGS_ir_out, std::ios::binary | std::ios::trunc);
    circuit->Serialize(os);
  }

  if (!FLAGS_dot_out.empty()) {
    if (FLAGS_dot_out == "-") {
      FLAGS_dot_out = "/dev/stderr";
    }

    std::ofstream os(FLAGS_dot_out);
    circuitous::PrintDOT(os, circuit.get());
  }

  if (!FLAGS_python_out.empty()) {
    if (FLAGS_python_out == "-") {
      FLAGS_python_out = "/dev/stderr";
    }

    std::ofstream os(FLAGS_python_out);
    circuitous::PrintPython(os, circuit.get());
  }

  if (!FLAGS_smt_out.empty()) {
    if (FLAGS_smt_out == "-") {
      FLAGS_smt_out = "/dev/stderr";
    }

    std::ofstream os(FLAGS_smt_out);
    circuitous::PrintSMT(os, circuit.get());
  }

  if (!FLAGS_json_out.empty()) {
    if (FLAGS_json_out == "-") {
      FLAGS_json_out = "/dev/stderr";
    }

    std::ofstream os(FLAGS_json_out);
    circuitous::PrintJSON(os, circuit.get());
  }
  return EXIT_SUCCESS;
}
