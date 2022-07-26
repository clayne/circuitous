/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */

#pragma once

#include <circuitous/IR/Circuit.hpp>
#include <circuitous/Support/Check.hpp>

#include <deque>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "IR.hpp"

namespace circ {

template<typename T, typename ...Ts>
bool IsOneOf(Operation *op) {
  if (op->op_code == T::kind) {
    return true;
  }

  if constexpr (sizeof...(Ts) == 0) {
    return false;
  } else {
    return IsOneOf<Ts...>(op);
  }
}

template < typename T, typename ...Ts >
bool IsOneOfType(Operation *op) {
    if ( isa<T>(op) ) {
        return true;
    }

    if constexpr ( sizeof...( Ts ) == 0 ) {
        return false;
    } else {
        return IsOneOf< Ts... >( op );
    }
}
static inline bool IsLeafNode(Operation *op) {
  switch(op->op_code) {
    case InputRegister::kind:
    case OutputRegister::kind:
    case Constant::kind:
    case Advice::kind:
    case Undefined::kind:
    case InputInstructionBits::kind:
      return true;
    default:
      return false;
  }
}

template<typename T>
bool Is(Operation *op) {
  return op->op_code == T::kind;
}

using operation_set_t = std::unordered_set<Operation *>;

template<typename T>
struct SubtreeCollector {
  std::unordered_multiset<T *> collected;

  template<typename C>
  SubtreeCollector<T> &Run(const C &ops) {
    for (auto op : ops) {
      Run(op);
    }
    return *this;
  }

  SubtreeCollector<T> &Run(Operation *o) {
    if (o->op_code == T::kind) {
      collected.insert(dynamic_cast<T *>(o));
    }
    for (auto op : o->operands) {
      Run(op);
    }
    return *this;
  }

  template<typename CB>
  auto Apply(CB cb) {
    using res_t = decltype(cb(*collected.begin()));
    std::vector<res_t> out;
    for (auto x : collected) {
      out.push_back(cb(x));
    }
    return out;
  }
};



namespace print {
  template<typename Derived>
  struct Topology {
    std::stringstream ss;
    using hash_t = std::string;

    Derived &Self() { return static_cast<Derived &>(*this); }

    template<typename C>
    std::string Hash(const C& ops) {
      std::stringstream hash;
      for (auto op : ops) {
        hash << Hash(op);
        hash << " | ";
      }
      return hash.str();
    }

    std::string Hash(Operation *op) {
      return Self().Print(op, 0);
    }

    std::string Children(Operation *op, uint8_t depth) {
      std::string out;
      for (auto o : op->operands) {
        out += Self().Print(o, depth + 1);
        out += Self().separator;
      }
      return out;
    }

    std::string Print(Operation *op) {
      return Self().Print(op, 0);
    }

    std::string Print(Operation *op, uint8_t depth) {
      auto indent = Self().Indent(depth);
      std::string out;
      out += indent;
      out += Self().Op(op);
      out += "( ";
      out += Self().Children(op, depth);
      out += indent + ")";
      return out;
    }

    std::string Get() { return ss.str(); }
    std::string Indent(uint8_t) { return {}; }
  };

  template<typename Next>
  struct WithCache : Next {
    using parent_t = Next;
    using hash_t = typename parent_t::hash_t;

    using Next::Hash;

    std::unordered_map<Operation *, hash_t > op_to_hash;

    std::string Print(Operation *op, uint8_t depth) {
      auto it = op_to_hash.find(op);
      if (it != op_to_hash.end()) {
        return it->second;
      }
      auto x = this->parent_t::Print(op, depth + 1);
      op_to_hash[op] = x;
      return x;
    }
  };

  template<typename Next>
  struct FullNames_ : Next {
    static inline constexpr const char separator = ' ';
    std::string Op(Operation *op) { return op->name(); }
  };

  struct FullNames : FullNames_<WithCache<Topology<FullNames>>> {};

  struct PrettyPrinter : FullNames {
    std::string Indent(uint8_t depth) {
      return std::string(depth * 2, ' ');
    }
  };

} // namespace print



namespace collect {
  struct Ctxs {
    using ctxs_t = std::unordered_set<Operation *>;
    using ctxs_map_t = std::unordered_map<Operation *, ctxs_t>;

    ctxs_map_t op_to_ctxs;

    void Root(Operation *op) {
      op_to_ctxs[op] = { op };
    }

    void Update(Operation *node, Operation *user) {
      if (user) {
        auto &ctxs = op_to_ctxs[node];
        auto &user_ctxs = op_to_ctxs[user];
        ctxs.insert(user_ctxs.begin(), user_ctxs.end());
      }
    }
  };

  struct Hashes : print::FullNames {
    void Root(Operation *op) {
      Hash(op);
    }

    void Update(Operation *node, Operation *user) {
      check(op_to_hash.count(node));
    }
  };

  struct AllowsUndef {
    std::optional< bool > allows;

    void Root(Operation *op) {}

    void Update(Operation *node, Operation *user) {
      if (node->op_code == Undefined::kind)
        allows = true;
    }
  };


  template< typename ...Ts >
  struct UpTree {
    std::unordered_set<Operation *> collected;

    void Run(Operation *op) {
      if (IsOneOf<Ts...>(op)) {
        collected.insert(op);
      }
      for (auto o : op->users) {
        Run(o);
      }
    }
  };


  template< typename ...Ts >
  struct DownTree {
      std::unordered_set<Operation *> collected;

      void Run(Operation *op) {
          if (IsOneOfType<Ts...>(op)) {
              collected.insert(op);
          }
          for (auto o : op->operands) {
              Run(o);
          }
      }
  };
} // namespace collect

template<typename ...Collectors>
struct Collector : Collectors ... {
  using self_t = Collector<Collectors...>;

  using entry_t = std::pair<Operation *, Operation *>;
  std::deque<entry_t> todo;

  self_t &Run(Circuit *circuit) {
    for (auto x : circuit->attr<VerifyInstruction>()) {
      (Collectors::Root(x), ...);
      todo.push_back({x, nullptr});
    }

    while (!todo.empty()) {
      const auto &[x, y] = todo.front();
      todo.pop_front();
      Update(x, y);
    }
    return *this;
  }

  void Update(Operation *node, Operation *user) {
    (Collectors::Update(node, user), ...);
    for (auto op : node->operands) {
      todo.emplace_back(op, node);
    }
  }
};

using CtxCollector = Collector<collect::Ctxs>;

static inline bool allows_undef_(Operation *op, std::unordered_set< Operation * > &seen)
{
  if (seen.count(op)) return false;
  seen.insert(op);

  if (op->op_code == Undefined::kind)
    return true;

  for (auto x : op->operands)
    if (allows_undef_(x, seen))
      return true;
  return false;
}
static inline bool allows_undef(Operation *op) {
  if (op->op_code != RegConstraint::kind ||
      op->operands[1]->op_code != OutputRegister::kind)
  {
    return false;
  }
  std::unordered_set< Operation * > seen;
  return allows_undef_(op, seen);
}

static inline Operation *GetContext(Operation *op) {
  collect::UpTree<VerifyInstruction> collector;
  collector.Run(op);
  auto &ctxs = collector.collected;
  check(ctxs.size() == 1);
  return *(ctxs.begin());
}

static inline std::unordered_set<Operation *> GetContexts(Operation *op) {
  collect::UpTree<VerifyInstruction> up_collector;
  up_collector.Run( op);

  SubtreeCollector< VerifyInstruction > down_collector;
  auto down_collected = std::move(down_collector.Run( op ).collected);

  up_collector.collected.insert(down_collected.begin(), down_collected.end());
  return std::move(up_collector.collected);
}

static inline std::unordered_set<Operation *> GetLeafNodes(Operation *op) {
    collect::DownTree<leaf_values_ts> down_collector;
    down_collector.Run( op);

    return std::move(down_collector.collected);
}



struct RunTreeUp{
    virtual ~RunTreeUp() {};
    virtual void Execute(Operation* op) = 0;
    bool should_continue = true;
    void Run(Operation* op) {
        Execute( op );
        if(should_continue){
            for (auto o: op->users) {
                Run( o );
            }
        }
        should_continue = true;
    }
};

struct RunTreeDown{
    virtual ~RunTreeDown() {};

    void Run(Operation* op) {
        Execute( op );
        for (auto o: op->operands) {
            Run( o );
        }
    }
private:
    virtual void Execute(Operation* op) = 0;
};

template<typename TL>
struct TypedTreeRunner{
    virtual ~TypedTreeRunner() {};
    virtual void Execute(Operation* op) = 0;
    virtual void Run_(Operation* op) = 0;
    void Run(Operation* op) {
        collect::DownTree<TL> down_collector;
        down_collector.Run(op);
        std::cout << "collected: " << down_collector.collected.size() << std::endl;
        for(auto& o : down_collector.collected) {
            Run_( o );
        }
    }
};


struct DownTreeCollecterMetaData : RunTreeDown{
    DownTreeCollecterMetaData(const std::string &key, const std::string &value)
            : key( key ), value( value ) {}

    void Execute(Operation* op){
        if(op->has_meta(key) && op->get_meta(key) == value){
            collected.insert(op);
        }
    }

    std::unordered_set<Operation*> collected;
    // metadata _can_ be any type, but on Operations they are string
    // Ideally we would make these types linked explicitly
    std::string key;
    std::string value;
};


template<typename TL>
struct RunTreeDownTyped : TypedTreeRunner<TL>{
    virtual ~RunTreeDown() {};
    virtual void Execute(Operation* op) = 0;
    void Run_(Operation* op) {
        Execute( op );
        for (auto o: op->operands) {
            Run_( o );
        }
    }
};

template<typename TL>
struct RunTreeUpTyped : TypedTreeRunner<TL>{
    virtual ~RunTreeUpTyped() {};
    virtual void Execute(Operation* op) = 0;
    void Run_(Operation* op) {
        Execute( op );
        for (auto o: op->users) {
            Run_( o );
        }
    }
};




} // namespace circ


