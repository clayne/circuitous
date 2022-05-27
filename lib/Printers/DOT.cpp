/*
 * Copyright (c) 2020 Trail of Bits, Inc.
 */

#include <circuitous/IR/Circuit.hpp>
#include <circuitous/IR/Visitors.hpp>

#include <circuitous/Util/Warnings.hpp>
#include <circuitous/Support/Check.hpp>

#include <ostream>
#include <unordered_map>


CIRCUITOUS_RELAX_WARNINGS
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
CIRCUITOUS_UNRELAX_WARNINGS

namespace circ
{
    namespace
    {

        static const char *const kBeginDOTNode =
            "[label=<<TABLE cellpadding=\"0\" cellspacing=\"0\" border=\"1\"><TR>";
        static const char *const kEndDOTNode = "</TR></TABLE>>];\n";

        class DOTPrinter : public UniqueVisitor<DOTPrinter> {
            using value_map_t = std::unordered_map<Operation *, std::string>;
            public:
            explicit DOTPrinter(std::ostream &os_, const value_map_t &vals)
                : os(os_), node_values(vals) {}

            void PrintOperands(Operation *op) {
                if (!op->operands.empty()) {
                    os << "</TR><TR>";
                    for (auto sub_op : op->operands) {
                        os << "<TD port=\"s";
                        os << sub_op->id();
                        os << "\"> &nbsp; </TD>";
                    }
                }
                os << kEndDOTNode;
                for (auto sub_op : op->operands) {
                    os << 'o' << op->id() << ":s" << sub_op->id()
                       << " -> o" << sub_op->id() << ":id;\n";
                }
            }

            void PrintNodeName(Operation *op) {
                os << "o" << op->id() << " " << kBeginDOTNode << "<TD port=\"id\"";
                if (!op->operands.empty()) {
                    os << " colspan=\"" << op->operands.size() << "\"";
                }
                os << ">" << op->name();
                if (node_values.count(op)) {
                    os << " = " << node_values.find(op)->second;
                }
                os << "</TD>";
            }

            void visit(Operation *op) {
                op->traverse(*this);
                PrintNodeName(op);
                PrintOperands(op);
            }

            void visit(Circuit *op) {
                os << "digraph {\n"
                    << "node [shape=plain];\n";
                op->traverse(*this);
                PrintNodeName(op);
                PrintOperands(op);
                os << "}\n";
            }

            private:
            std::ostream &os;
            const value_map_t &node_values;
        };

    }  // namespace
} // namespace circ

namespace circ::dot
{
    struct Printer : UniqueVisitor<Printer> {
        using value_map_t = std::unordered_map<Operation *, std::string>;
        using highlight_names_t = std::vector<std::string>;
        explicit Printer(std::ostream &os_, const value_map_t &vals, const highlight_names_t &highlight_nodes)
            : os(os_), node_values(vals), highlight_nodes(highlight_nodes) {}


        std::string Operand(Operation *of, std::size_t i) {
            return NodeID(of) + ':' + NodeID(of) + std::to_string(i);
        }

        void Edge(Operation *from, Operation *to, std::size_t i) {
            os << Operand(from, i)
                << " -> "
                << NodeID(to)
                << ";\n";
        }

        std::string NodeID(Operation *op) {
            return "v" + std::to_string(op->id()) + "v";
        }

        std::string AsID(const std::string &what) {
            return "<" + what + ">";
        }

        void Node(Operation *op) {
            os << NodeID(op) << "[";
            if(should_highlight(op)) {
                os << "fillcolor=red;style=filled;";
            }
            os << "label = \" { " << AsID(NodeID(op)) << " " << op->name();
            if (node_values.count(op)) {
                os << " " << node_values.find(op)->second << " ";
            }

            if (op->operands.size() == 0) {
                os << " }" << '"' << "];\n";
                return;
            }

            os << "| {";
            for (std::size_t i = 0; i < op->operands.size(); ++i) {
                os << AsID(NodeID(op) + std::to_string(i));
                if (i != op->operands.size() - 1) {
                    os << " | ";
                }
            }
            os << " }}" << '"' << "];\n";
        }

        void Init() {
            os << "digraph {" << std::endl;
            os << "node [shape=record];";
        }

        void visit(Operation *op) {
            op->traverse(*this);
            Node(op);
            for (std::size_t i = 0; i < op->operands.size(); ++i) {
                Edge(op, op->operands[i], i);
            }
        }

        void visit(Circuit *op) {
            Init();
            op->traverse(*this);
            Node(op);
            for (std::size_t i = 0; i < op->operands.size(); ++i) {
                Edge(op, op->operands[i], i);
            }
            os << "}";
        }

        std::ostream &os;
        const value_map_t &node_values;

       private:
          highlight_names_t highlight_nodes;

          bool should_highlight(Operation* op){
            auto case_insensitive_cmp = [](const std::string& str1, const std::string& str2){ return str1.size() == str2.size() && std::equal(str1.begin(), str1.end(), str2.begin(), [](auto a, auto b){return std::tolower(a)==std::tolower(b);}); };
            auto remove_suffixes = [](const std::string& s) { return s.substr(0, s.find("."));};
            auto target = remove_suffixes(op->name());
            auto highlight = std::find_if(highlight_nodes.begin(), highlight_nodes.end(), [&](std::string hl) {return case_insensitive_cmp(hl, target);} ) != highlight_nodes.end();
            return highlight;
          }
    };

} // namespace circ::dot

namespace circ
{
    void print_dot(std::ostream &os, Circuit *circuit,
                  const std::unordered_map<Operation *, std::string> &node_values, const std::vector<std::string> &highlights = std::vector<std::string>())
    {
      circ::dot::Printer dot_os(os, node_values, highlights);
      dot_os.visit(circuit);
    }
} // namespace circ
