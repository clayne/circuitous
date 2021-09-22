/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

#include <circuitous/ADT/UnionFind.hpp>
#include <circuitous/Util/Overloads.hpp>

namespace circ::eqsat {

  using Id = UnionFind::Id;

  using Children = std::vector< Id >;

  template< typename T, template < typename... > typename Template >
  struct is_specialization : std::false_type {};

  template< template < typename... > typename Template, typename... Args >
  struct is_specialization< Template<Args...>, Template > : std::true_type {};

  struct NodeBase
  {
    template< typename Fn >
    void update_children(Fn &&fn)
    {
      for (auto &child : children)
        child = fn(child);
    }

    auto operator==(const NodeBase &other) const
    {
      return children == other.children;
    }

    Children children;
  };


  template< typename Storage >
  struct StorageNode : NodeBase
  {
    using Base = NodeBase;

    template< typename ...Args >
    explicit StorageNode(Args && ...args)
      : storage( std::forward< Args >(args)... )
    {}

    explicit StorageNode(const Storage &s) : storage(s) {}

    Storage& get() { return storage; }
    const Storage& get() const { return storage; }

    using Base::children;

    std::string name() const { return node_name(storage); }

  private:
    Storage storage;
  };


  template< typename Storage >
  using ENodeBase = std::variant< StorageNode< Storage > >;

  template< typename Storage >
  struct ENode : ENodeBase< Storage >
  {
    using Base = ENodeBase< Storage >;
    using StorageNode = StorageNode< Storage >;

    using Base::Base;

    ENode(const Storage &storage) : Base(StorageNode(storage)) {}
    ENode(Storage &&storage) : Base(StorageNode(std::move(storage))) {}

    template< typename Fn >
    decltype(auto) visit(Fn &&fn)
    {
      return std::visit(std::forward<Fn>(fn), get());
    }

    template< typename Fn >
    decltype(auto) visit(Fn &&fn) const
    {
      return std::visit(std::forward<Fn>(fn), get());
    }

    const Children& children() const
    {
      return visit([] (const auto &n) -> const Children& { return n.children; });
    }

    Children &children()
    {
      return visit([] (auto &n) -> Children& { return n.children; });
    }

    Id child(std::size_t idx) const { return children()[idx]; }

    const Storage &data() const { return std::get< StorageNode >(*this).get(); }
    Storage &data() { return std::get< StorageNode >(*this).get(); }

    const Base& get() const { return *this; }
    Base& get() { return *this; }
  };

  namespace detail
  {
    static inline auto name = [] (const auto &n) { return n.name(); };
  }

  template< typename Storage >
  std::string node_name(const ENode< Storage > &node)
  {
    return node.visit(detail::name);
  }

  template< /* typename Storage, */ typename Fn >
  void update_children(auto /* ENode< Storage > */ &node, Fn &&fn)
  {
    node.visit([fn = std::forward< Fn >(fn)] (auto &n) {
      return n.update_children(fn);
    });
  }

  // Equivalence class of term nodes
  template< typename ENode >
  struct EClass
  {
    using Nodes   = std::vector< ENode* >;
    using Parents = Nodes;

    bool empty() const { return nodes.empty(); }
    auto size() const { return nodes.size(); }

    void add(ENode *enode) { nodes.push_back(enode); }

    void merge(EClass &&other)
    {
      std::move(other.nodes.begin(), other.nodes.end(), std::back_inserter(nodes));
      std::move(other.parents.begin(), other.parents.end(), std::back_inserter(parents));
    }

    friend bool operator==(const EClass &a, const EClass &b)
    {
      return std::tie(a.nodes, a.parents) == std::tie(b.nodes, b.parents);
    }

    Nodes nodes;
    Parents parents;
  };


  template< typename ENode >
  struct EGraph
  {
    using Node = ENode;
    using EClass  = EClass< ENode >;
    using Parents = typename EClass::Parents;

    Id create_singleton_eclass(ENode *enode)
    {
      auto id = _unions.make_set();
      _ids.emplace(enode, id);

      EClass eclass;
      eclass.add(enode);
      _classes.emplace(id, eclass);

      return id;
    }

    void canonicalize(ENode &node)
    {
      update_children(node, [&](auto child) { return _unions.find_compress(child); });
    }

    Parents& parents(Id id) { return _classes.at(id).parents; }
    const Parents& parents(Id id) const { return _classes.at(id).parents; }

    std::pair< Id, ENode* > add(ENode &&node)
    {
      canonicalize(node);
      // allocate new egraph node
      _nodes.push_back(std::make_unique< ENode >(std::move(node)));
      auto &enode = _nodes.back();
      auto node_ptr = enode.get();

      auto id = create_singleton_eclass(node_ptr);

      // add children - parent links
      for (auto child : enode->children()) {
        parents(child).push_back(node_ptr);
      }

      return {id, node_ptr};
    }

    Id find(Id id) const { return _unions.find(id); }
    Id find(const ENode *enode) const { return _unions.find( _ids.at(enode) ); }

    Id merge(Id a, Id b)
    {
      a = _unions.find_compress(a);
      b = _unions.find_compress(b);

      if (a == b)
        return a;

      // make sure that second eclass has fewer parents
      if ( parents(a).size() < parents(b).size() ) {
        std::swap(a, b);
      }

      auto new_id = _unions.merge(a, b);
      CHECK_EQ(new_id, a);

      _pending.push_back(new_id);

      // TODO(Heno) maybe can be moved to rebuild?
      auto class_b = _classes[b];
      _classes.erase(b);
      _classes[a].merge(std::move(class_b));

      return new_id;
    }

    EClass& eclass(const ENode *enode) { return _classes.at( _ids.at(enode) ); }
    const EClass& eclass(const ENode *enode) const { return _classes.at( _ids.at(enode) ); }

    EClass& eclass(Id id) { return _classes[ _unions.find(id) ]; }
    const EClass& eclass(Id id) const { return _classes.at( _unions.find(id) ); }

    // Restores the egraph invariants, i.e, congruence equality and enode uniqueness
    void rebuild()
    {
      // canonicalize and deduplicate the eclass references
      // to save calls to repair
      for (auto id : _pending)
        for (auto *node : _classes[id].nodes)
          canonicalize(*node);

      std::unordered_set< Id > changed_classes;
      for (auto id : _pending)
        changed_classes.insert( _unions.find_compress(id) );

      for (auto id : changed_classes)
        repair(_classes[id]);

      _pending.clear();
    }

    void repair(EClass &eclass)
    {
      // update the '_ids' so it always points
      // canonical enodes to canonical eclasses
      for (auto *node : eclass.nodes) {
        canonicalize(*node);
        _ids[node] = _unions.find_compress(_ids[node]);
      }

      // deduplicate the parents, noting that equal
      // parents get merged and put on the worklist
      std::unordered_map< Id, ENode* > new_parents;
      for (auto *node : eclass.parents) {
        auto id = _unions.find_compress( _ids[node] );
        new_parents.try_emplace(id, node);
      }

      eclass.parents.clear();
      for (auto[id, node] : new_parents)
        eclass.parents.push_back(node);

      // obliterate empty classes
      for (auto it = _classes.begin(); it != _classes.end(); ) {
        auto &[_, eclass] = *it;
        it = eclass.empty() ? _classes.erase(it) : std::next(it);
      }
    }

    const auto& classes() const { return _classes; }
    const auto& nodes() const { return _ids; }

  private:
    // stores heap allocated nodes of egraph
    std::vector< std::unique_ptr< ENode > > _nodes;

    // stores equivalence relation between equaltity classes
    UnionFind _unions;

    // all equavalent ids  map to the same class
    std::unordered_map< Id, EClass > _classes;

    // stores equality ids of enodes
    std::unordered_map< const ENode*, Id > _ids;

    // modified eclasses that needs to be rebuild
    std::vector< Id > _pending;
  };

  template< typename Graph >
  void to_dot(const Graph &egraph, std::ostream &out)
  {
    auto enumerate = [] (const auto &container, auto &&fn) {
      std::size_t i = 0;
      for (const auto &val : container) {
          fn(i++, val);
      }
    };

    out << "digraph egraph {\n";
    out << "  compound=true\n";
    out << "  clusterrank=local\n";

    for (const auto &[id_, eclass] : egraph.classes()) {
      out << "  subgraph cluster_" << id_ << " {\n";
      out << "    style=dotted\n";

      auto id = id_; // to allow lambda capture
      enumerate(eclass.nodes, [&] (auto node_idx, const auto &enode) {
        out << "    " << id << '.' << node_idx << " [label = \"" << name(enode) << "\" ]\n";
      });

      out << "  }\n";
    }

    for (const auto &[id_, eclass] : egraph.classes()) {
      auto id = id_; // to allow lambda capture
      enumerate(eclass.nodes, [&] (auto node_idx, const auto &enode) {
        enumerate(enode->children(), [&] (auto child_idx, const auto &child) {
          auto child_class = egraph.find(child);
          out << id << '.' << node_idx << " -> ";
          if (id == child_class) {
            out << id << '.' << node_idx << ":n ";
          } else {
            out << child_class << ".0 ";
          }
          out << "[lhead = cluster_"<< child_class << "]\n";
        });
      });
    }

    out << "}\n";
  }

} // namespace circ::eqsat
