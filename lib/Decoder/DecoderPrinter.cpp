#include <circuitous/Decoder/DecoderPrinter.hpp>
#include <circuitous/Printers.hpp>

#include <circuitous/IR/Shapes.hpp>
#include <circuitous/IR/Visitors.hpp>
#include <cstdlib>

namespace circ::decoder {
    const Var DecoderPrinter::inner_func_arg1 = Var( "first8bytes", "uint64_t");
    const Var DecoderPrinter::inner_func_arg2 = Var( "second8bytes", "uint64_t");

    bool operator<(const Decode_Requires_Group &x, const Decode_Requires_Group &y) {
        std::size_t min_x = std::min(x.zeros, x.ones).size();
        std::size_t min_y = std::min(y.zeros, y.ones).size();

        return min_x < min_y;
    };

    Expr DecoderPrinter::print_context_decoder_function(const ExtractedCtx &ctx) {
        FunctionDeclaration funcDecl;
        funcDecl.function_name = ctx.generated_name;
        funcDecl.retType = "static int";

        funcDecl.args = {VarDecl( inner_func_arg1 ), VarDecl( inner_func_arg2 )};

        auto args = get_decode_context_function_args( ctx );
        funcDecl.body.emplace_back( get_decode_context_function_body( args, ctx.encoding_size_in_bytes ));
        return funcDecl;
    }

    std::vector< OptionalBitArray<8> > ExtractedCtx::convert_circIR_to_input_type_array() const {
        std::vector< OptionalBitArray<8> > input_checks;
        for (std::size_t i = 0; i < 16; i++) {
            auto val = OptionalBitArray<8>();
            std::fill( val.begin(), val.end(), InputType::ignore);
            for (auto &n: decodeConditions) {
                auto lhsn = dynamic_cast<circ::Constant *>(n->operands[ 0 ]);
                auto extract = dynamic_cast<circ::Extract *>(n->operands[ 1 ]);
                auto low = extract->low_bit_inc;
                auto high_inc = extract->high_bit_exc - 1;

                // out of range of considered byte
                if ( low > (i + 1) * 8 || high_inc < i * 8 || high_inc == 119 ) {
                    continue;
                }

                for (std::size_t c = 0; c < 8; c++) {
                    auto bit_index = (i * 8) + c;
                    if ( low <= bit_index && bit_index <= high_inc ) {
                        auto new_val = ctoit(lhsn->bits[ bit_index - low ]);
                        val[ c ] = new_val;
                    }
                }
            }
            input_checks.push_back( val );
        }
        return input_checks;
    }


    void DecoderPrinter::print_file() {
        os << "#include <array>" << std::endl;
        os << "#include <stdint.h>" << std::endl << std::endl;
        ExpressionPrinter ep(os);
        for (auto &ctx: extracted_ctxs) {
            ep.print( print_context_decoder_function( ctx ));
        }
        ep.print( print_top_level_function());
        os << std::endl;
    }

    void DecoderPrinter::extract_ctx() {
        auto Contexts = GetContexts( circuit.get()->operands[ 0 ] );
        for (auto &vi: Contexts) {
            SubtreeCollector< DecodeCondition > dc_collector;
            dc_collector.Run( vi->operands );
            std::unordered_multiset< DecodeCondition * > decNodes = dc_collector.collected;
            auto x =
                    std::find_if( decNodes.begin(), decNodes.end(), [&](DecodeCondition *dc) {
                        auto rhs = dynamic_cast<circ::Extract *>(dc->operands[ 1 ]);
                        return rhs->high_bit_exc == 120;
                    } );
            if ( x == decNodes.end()) {
                throw std::invalid_argument( "No decode condition that specifies end" );
            }
            auto rhs = dynamic_cast<circ::Extract *>((*x)->operands[ 1 ]);
            auto encoding_length = floor( rhs->low_bit_inc / 8 );
            if ( encoding_length > 15 ) {
                throw std::invalid_argument( "Instruction is longer than 15 bytes" );
            }
            extracted_ctxs.emplace_back( ExtractedCtx(
                    "generated_decoder_prefix_" + std::to_string( vi->id()),
                    static_cast<uint8_t>(encoding_length),
                    std::move( decNodes )));
        }
    }

    Expr DecoderPrinter::print_top_level_function() {
        FunctionDeclaration funcDecl;
        funcDecl.function_name = circuit_decode_function_name;
        funcDecl.retType = "int";
        funcDecl.args.emplace_back(Var(bytes_input_variable, "std::array<uint8_t,15>"));

        funcDecl.body.emplace_back( convert_input_to_uints64());

        std::vector< ExtractedCtx * > to_split;
        for (auto &ctx: extracted_ctxs) {
            to_split.push_back( &ctx );
        }
        auto tree = generate_decoder_selection_tree( to_split,
                                                     std::vector< std::pair< std::size_t, int>>(),
                                                     0 );
        funcDecl.body.emplace_back(tree);

        std::cout << "max depth during gen: " << max_depth << " for size of: "
                  << std::to_string( extracted_ctxs.size()) << std::endl;

        funcDecl.body.emplace_back(Return(Int(-1)));
        return funcDecl;
    }

    Expr DecoderPrinter::convert_input_to_uints64() {
        auto array_input = Var( bytes_input_variable);
        StatementBlock b;
        convert_array_input_to_uint64( array_input, inner_func_arg1, b, false );
        convert_array_input_to_uint64( array_input, inner_func_arg2, b, true );
        return b;
    }

    void DecoderPrinter::convert_array_input_to_uint64(const Var &array_input, const Var &arg,
                                                       StatementBlock &b, bool second_uint) {
        b.emplace_back(Statement(Assign(VarDecl(arg),Int(0))));
        for (uint i = 0; i < 8; i++) {
            auto indexdVar = IndexVar( array_input, i);
            if(second_uint){
                indexdVar = IndexVar( array_input, i+8); // index 8 further
            }
            auto set =Parenthesis(CastToUint64(Shfl(indexdVar, Int( static_cast<const int>(8 * i)))));
            b.emplace_back(Statement(Assign(arg, Plus(arg, set))));
        }
    }


    bool DecoderPrinter::contains_ignore_bit(const OptionalBitArray<64> &byte) {
        for (auto &b: byte) {
            if ( b == InputType::ignore ) {
                return true;
            }
        }
        return false;
    }

    bool DecoderPrinter::contains_only_ignore_bit(const OptionalBitArray<64> &byte) {
        for (auto &b: byte) {
            if ( b != InputType::ignore ) {
                return false;
            }
        }
        return true;
    }

    Expr DecoderPrinter::get_decode_context_function_body(const decode_func_args &args,
                                                          int encoding_size) {
        StatementBlock block;
        block.emplace_back( print_ignore_bits( args.first ));
        block.emplace_back( print_ignore_bits( args.second ));

        std::vector< Expr > compare_exprs;
        if ( !contains_only_ignore_bit( args.first.byte ))
            compare_exprs.push_back( get_comparison( args.first ));

        if ( !contains_only_ignore_bit( args.second.byte ))
            compare_exprs.push_back( get_comparison( args.second ));

        if ( compare_exprs.size() == 1 )
            block.emplace_back( Return( Mul( Parenthesis(compare_exprs[ 0 ]), Int( encoding_size ))));
        else
            block.emplace_back( Return( Mul( Parenthesis(And( compare_exprs[ 0 ], compare_exprs[ 1 ] )),
                                             Int( encoding_size ))));

        return block;
    }

    Expr DecoderPrinter::get_comparison(const decode_context_function_arg &arg ) {
        auto uint = Uint64(arg.byte.to_uint64());
        auto lhs = CastToUint64( Parenthesis( BitwiseXor( arg.var.name, BitwiseNegate( uint ))));

        // negation of ignore bits are the bits we care about
        auto neg_ignore = Uint64(~(arg.byte.ignored_bits_to_uint64()));
        return  Equal( lhs, neg_ignore );
    }

    Expr DecoderPrinter::print_ignore_bits(const decode_context_function_arg &arg) {
        if ( contains_ignore_bit( arg.byte ) && !contains_only_ignore_bit( arg.byte )) {
            auto name = arg.var.name;
            auto ignoringValues = Uint64( arg.byte.ignored_bits_to_uint64() );
            return Statement( Assign( name, BitwiseOr( name, ignoringValues )));
        }
        return Empty();
    }

    /*
     * This function is meant to find the optimal amount of checks required to identify an encoding
     * Instead of calling every <decode_encoding> function once, we do a BST like search
     * to first find the best closest candidate and calling that.
     *
     * This algorithm looks like a regular backtracking algorithm from your standard algos class
     * but it doesn't have any backtracking since the input size can be 1000+,
     * and leaving it naively like this gives decent results (depth 17 instead of the optimal 12)
     *
     * How it works:
     *
     * we will consider for each encoding/context the bit string that represents the instruction
     * with this we will count at every index if the encoding _requires_ a 1 or 0.
     * Lastly, we will make an BST tree that at each check eliminates as many possible encodings
     *
     * Example:
     * index 0 1 2 3 4 5 6
     * zeros 3 0 2 3 1 4 5
     * ones  2 3 4 0 2 3 4
     *
     * We gain the most information by checking what bit is located at index 4
     * The best heuristic I found was looking at a pair of zeros/ones at an index
     * which had the maximal of the minimal value, as this would prevent cases weird cases
     * where you'd had a lot of imbalance.
     *
     * Note that the sum of zeros and ones do not need to sum the total amount of encodings
     * as some encodings accept both a 1 and 0 (don't cares).
     *
     * best case: log(f) with f #decode functions
     * worse case: f
     */
    Expr DecoderPrinter::generate_decoder_selection_tree(std::vector< ExtractedCtx * > to_split,
                                                         std::vector< std::pair< std::size_t, int>> already_chosen_bits,
                                                         int depth) {
        if ( this->max_depth < depth )
            max_depth = depth;

        if ( to_split.empty() ) {
            return Return(Int(-1));
        }

        if ( to_split.size() == 1 ) {
            return Return( call_ctx( *to_split[ 0 ] ));
        }

        std::array< Decode_Requires_Group, 120 > indice_values = get_decode_requirements_per_index( to_split,
                                                                                                    already_chosen_bits );
        auto max = std::max_element( indice_values.begin(), indice_values.end());
        auto candidate_index = static_cast<std::size_t>(std::distance( indice_values.begin(), max ));
        already_chosen_bits.emplace_back( std::make_pair( candidate_index, depth ));


        /*
         * The encodings with dont care on the candidate index can both be a 0 or 1
         * Hence they need to be in both candidates sets from this point onwards
         */
        for (auto &ignored: indice_values[ candidate_index ].ignores) {
            indice_values[ candidate_index ].zeros.push_back( ignored );
            indice_values[ candidate_index ].ones.push_back( ignored );
        }


        std::vector< ExtractedCtx * > ones = indice_values[ candidate_index ].ones;
        std::vector< ExtractedCtx * > zeros = indice_values[ candidate_index ].zeros;

        if ( ones.empty() && zeros.empty() ) {
            return Return(Int(-1));
        }

        auto ci_byte = static_cast<uint32_t>(candidate_index / 8);
        auto lhs = IndexVar(Var(bytes_input_variable), ci_byte);
        auto rhs2 =   Parenthesis(Shfl(Int(1), Int( candidate_index % 8)));
        auto condition = BitwiseAnd( lhs, rhs2); // var & (1<< ci)


        auto lhs_split = generate_decoder_selection_tree( ones, already_chosen_bits, depth + 1 );
        auto rhs_split = generate_decoder_selection_tree( zeros, already_chosen_bits,
                                                          depth + 1 );

        return IfElse( condition, lhs_split, rhs_split);
    }

    std::array< Decode_Requires_Group, 120 >
    DecoderPrinter::get_decode_requirements_per_index(std::vector< ExtractedCtx * > &to_split,
                                                      std::vector< std::pair< std::size_t, int>> &already_chosen_bits) {
        std::array< Decode_Requires_Group, 120 > indice_values;
        for (std::size_t i = 0; i < 120; i++) {
            for (auto &ctx: to_split) {
                if ( std::find_if( already_chosen_bits.begin(), already_chosen_bits.end(),
                                   [&](std::pair< std::size_t, int > p) {
                                       return p.first == i;
                                   } ) != already_chosen_bits.end()) {
                    continue;
                }

                auto val = InputType::ignore;
                for (auto &n: ctx->decodeConditions) {
                    auto lhsn = dynamic_cast<circ::Constant *>(n->operands[ 0 ]);
                    auto extract = dynamic_cast<circ::Extract *>(n->operands[ 1 ]);
                    auto low = extract->low_bit_inc;
                    auto high_inc = extract->high_bit_exc - 1;


                    // out of range of considered byte
                    if ( low > i || high_inc < i || high_inc == 119 ) {
                        continue;
                    }

                    if ( lhsn->bits[ i - low ] == '0' )
                        val = InputType::zero;
                    else
                        val = InputType::one;
                }
                /*
                 * The bit which gets checked should only be in a single decode condition
                 * otherwise multiple decode conditions checking over other ranges would add
                 * way to many ignores
                 */
                if ( val == InputType::zero )
                    indice_values[ i ].zeros.push_back( ctx );
                else if ( val == InputType::one )
                    indice_values[ i ].ones.push_back( ctx );
                else
                    indice_values[ i ].ignores.push_back( ctx );
            }
        }
        return indice_values;
    }

    Expr DecoderPrinter::call_ctx(const ExtractedCtx &ctx) {
        FunctionCall fc;
        fc.function_name = ctx.generated_name;
        fc.args.emplace_back( inner_func_arg1);
        fc.args.emplace_back( inner_func_arg2);
        return fc;
    }

    OptionalBitArray<64> convert_bytes_to_uints64(const std::vector<OptionalBitArray<8>>& bytes, std::size_t offset){
        OptionalBitArray<64> val;
        for (std::size_t i = 0; i < 64; i++) {
            val[ i ] = bytes[ offset + (i / 8) ][ i % 8 ];
        }
        return val;
    }

    auto DecoderPrinter::get_decode_context_function_args(const ExtractedCtx& ectx) -> decode_func_args{
        auto input_checks = ectx.convert_circIR_to_input_type_array();
        OptionalBitArray<64> val = convert_bytes_to_uints64(input_checks, 0);
        OptionalBitArray<64> val2 = convert_bytes_to_uints64(input_checks, 8);

        auto arg1 = decode_context_function_arg( val, inner_func_arg1 );
        auto arg2 = decode_context_function_arg( val2, inner_func_arg2 );
        return {arg1, arg2 };
    }

    uint64_t to_val(const InputType &ty) {
        switch (ty) {
            case InputType::zero:
                return 0;
            case InputType::one:
                return 1;
            case InputType::ignore:
                return 1;
        }
    }

    uint64_t to_val_negated(const InputType &ty) {
        switch (ty) {
            case InputType::zero:
                return 1;
            case InputType::one:
                return 0;
            case InputType::ignore:
                return 0;
        }
    }

    InputType ctoit(const char c){
        switch (c) {
            case '0': return InputType::zero;
            case '1': return InputType::one;
            case '~': return InputType::ignore;
            default: circ::unreachable() << "Converting invalid value to InputType";
        }
    }

}  // namespace circ::disassm