/*
 * Copyright (c) 2022 Trail of Bits, Inc.
 */

#pragma once

#include <eqsat/pattern/rewrite_rule.hpp>
#include <eqsat/core/egraph.hpp>

#include <gap/core/generator.hpp>
#include <gap/core/dense_map.hpp>

#include <spdlog/spdlog.h>
#include <fmt/ranges.h>
#include <iostream>
#include <span>

namespace eqsat
{
    using matched_places_t = gap::dense_map< std::uint32_t, graph::node_handle >;
    struct match_result {
        graph::node_handle root;
        matched_places_t matched_places;
    };

    template< typename stream >
    stream& operator<<(stream& os, const match_result& m) {
        os << "match " << m.root.id.ref();
        for (auto p : m.matched_places) {
            os << fmt::format(" {} -> {}", p.first, p.second.id.ref());
        }
        return os;
    }

    using match_generator = gap::recursive_generator< match_result >;

} // namespace eqsat

namespace eqsat {
    //
    // match constant node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const constant_t &c
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        if (auto con = extract_constant(node); con && (con.value() == c.ref())) {
            co_yield { graph.find(&node), matched_places };
        }
    }

    //
    // match operation node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const operation_t &o
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        if (node_name(node) == o.ref()) {
            co_yield { graph.find(&node), matched_places };
        }
    }

    auto place_index(const place_t &place, const places_t &places) {
        return std::distance(places.begin(), std::find(places.begin(), places.end(), place));
    }

    //
    // match place node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const place_t &p
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        auto id = std::uint32_t(place_index(p, places));
        auto handle = graph.find(&node);

        if (auto it = matched_places.find(id); it != matched_places.end() && it->second != handle) {
            co_return;
        }

        match_result result = { handle, matched_places };
        result.matched_places.emplace(id, handle);
        co_yield result;
    }

    //
    // match label node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const label_t &p
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        spdlog::error("not implemented match label");
        __builtin_abort();
    }

    //
    // match atom node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const atom_t &atom
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        co_yield std::visit([&] (const auto &a) -> match_generator {
            co_yield match(a, node, graph, places, matched_places);
        }, atom);
    }

    static inline auto tail(const auto &vec) {
        return std::span(&(*std::next(vec.begin())), &(*vec.end()));
    }

    template< gap::graph::graph_like egraph >
    match_generator match_children(
          const auto &pattern_children
        , const auto &node_children
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        using eclass_type = typename egraph::eclass_type;

        auto match_child = [&] () -> match_generator {
            eclass_type child_class = graph.eclass(node_children.front());
            co_yield match(pattern_children.front(), child_class, graph, places, matched_places);
        };

        if (pattern_children.size() == 1) {
            co_yield match_child();
        } else {
            for (auto m : match_child()) {
                co_yield match_children(
                    tail(pattern_children), tail(node_children), graph, places, m.matched_places
                );
            }
        }
    }

    template< gap::graph::graph_like egraph >
    match_generator match(
          const expr_list &list
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        for (auto head : match(list.front(), node, graph, places, matched_places)) {
            auto pattern_children = tail(list);

            if (pattern_children.empty()) {
                co_yield head;
            } else {
                if (pattern_children.size() != node.num_of_children()) {
                    co_return;
                }

                std::vector< graph::node_handle > node_children;
                for (auto &&ch : node.children()) {
                    node_children.push_back(std::move(ch));
                }

                for (auto m : match_children(pattern_children, node_children, graph, places, head.matched_places)) {
                    co_yield { head.root, m.matched_places };
                }
            }
        }
    }

    //
    // match simple expr node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const simple_expr &expr
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        co_yield std::visit([&] (const auto &a) -> match_generator {
            co_yield match(a, node, graph, places, matched_places);
        }, expr);
    }

    template< gap::graph::graph_like egraph >
    match_generator match(
          const simple_expr &expr
        , const typename egraph::eclass_type &eclass
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        for (const auto &node : eclass.nodes) {
            co_yield match(expr, *node, graph, places, matched_places);
        }
    }

    //
    // match match expr node
    //
    template< gap::graph::graph_like egraph >
    match_generator match(
          const match_expr &expr
        , const typename egraph::node_type &node
        , const egraph &graph
        , const places_t &places
        , const matched_places_t &matched_places
    ) {
        spdlog::error("not implemented match expr");
        __builtin_abort();
    }

    template< gap::graph::graph_like egraph >
    match_generator match(
          const match_pattern &pattern
        , const typename egraph::eclass_type &eclass
        , const egraph &graph
        , const places_t &places
    ) {
        auto match_by_action = [&] (const auto &node) -> match_generator {
            co_yield std::visit([&] (const auto &a) -> match_generator {
                matched_places_t matched_places;
                co_yield match(a, node, graph, places, matched_places);
            }, pattern.action);
        };

        for (const auto &node : eclass.nodes) {
            co_yield match_by_action(*node);
        }
    }

    template< gap::graph::graph_like egraph >
    match_generator match(const match_pattern &pattern, const egraph &graph) {
        auto places = gather_places(pattern);
        for (const auto &[_, eclass] : graph.eclasses()) {
            for (auto m : match(pattern, eclass, graph, places)) {
                if (m.matched_places.size() == places.size()) {
                    co_yield m;
                }
            }
        }
    }


    template< gap::graph::graph_like egraph >
    match_generator match(const rewrite_rule &rule, const egraph &graph) {
        co_yield match(rule.lhs, graph);
    }

} // namespace eqsat
