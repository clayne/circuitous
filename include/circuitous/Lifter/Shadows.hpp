/*
 * Copyright (c) 2021 Trail of Bits, Inc.
 */
#pragma once

#include <cstdint>
#include <cmath>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <remill/Arch/Instruction.h>

#include <circuitous/Support/Check.hpp>
#include <circuitous/Support/Log.hpp>

namespace circ::shadowinst
{

    static inline std::string to_binary(const std::string &bytes)
    {
        std::stringstream ss;
        for (char byte_ : bytes)
        {
            auto byte = static_cast< uint8_t >(byte_);
            for (int b = 7; b >= 0; --b)
                ss << static_cast< uint16_t >(byte >> b & 1u);
        }
        return ss.str();
    }

    static inline std::string to_hex(const std::string &bytes)
    {
        std::stringstream ss;
        for (auto byte : bytes) {
            ss << std::hex;
            if (static_cast<uint8_t>(byte) < 16)
                ss << "0";

            ss << static_cast<unsigned>(static_cast<uint8_t>(byte));
        }
        return ss.str();
    }

    // This describes a region of decoded bytes where
    // it makes sense to talk about an "entity" -- register, immediate operand,
    // etc. While it will usually be one contignous entry, we don't need to constraint
    // ourselves yet.
    // {from, size}
    using region_t = std::map<uint64_t, uint64_t>;
    using maybe_region_t = std::optional<region_t>;

    static inline region_t Invert(region_t what, uint64_t lenght)
    {
        region_t out;
        uint64_t current = 0;
        for (auto &[from, size] : what) {
            if (current != from)
                out[current] = from - current;

            current = from + size;
        }

        if (current != lenght)
            out[current] = lenght - current;
        return out;
    }

    static inline region_t FromToFormat(const region_t &region)
    {
        region_t out;
        for (auto &[from, size] : region)
            out[from] = from + size;
        return out;
    }

    using bits_t = std::vector< bool >;

    struct ordered_bits_t
    {
        bits_t data;

        explicit ordered_bits_t(const bits_t &data_)
        {
            data.reserve(data_.size());
            data.insert(data.end(), data_.rbegin(), data_.rend());
        }
    };

    struct has_regions
    {
        region_t regions;

        has_regions() = default;

        has_regions(const ordered_bits_t &bits_)
        {
            auto &bits = bits_.data;
            for (std::size_t i = 0; i < bits.size(); ++i)
            {
                if (!bits[i])
                    continue;

                uint64_t offset = i;
                uint32_t count = 0;
                for(; i < bits.size() && bits[i]; ++i)
                    ++count;

                regions.emplace(offset, count);
            }
        }

        has_regions(const bits_t &bits) : has_regions(ordered_bits_t(bits)) {}
        has_regions(has_regions &&) = default;
        has_regions(const has_regions &) = default;

        has_regions &operator=(has_regions &&) = default;
        has_regions &operator=(const has_regions &) = default;

        has_regions(region_t o) : regions(std::move(o)) {}

        bool operator==(const has_regions &) const = default;

        std::size_t region_bitsize() const
        {
            std::size_t acc = 0;
            for (auto &[from, size] : regions)
                acc += size;
            return acc;
        }

        std::vector< uint64_t > region_idxs() const
        {
            std::vector<uint64_t> idxs;
            for (auto &[from, size] : regions)
                for (uint64_t i = 0; i < size; ++i)
                    idxs.push_back(from + i);
            return idxs;
        }

        auto begin() const { return regions.begin(); }
        auto end() const { return regions.end(); }
        auto size() const { return regions.size(); }
        auto empty() const { return size() == 0; }

        auto present(std::size_t idx) const
        {
            for (auto &[from, size] : regions)
                if (idx >= from && idx < from + size)
                    return true;

            return false;
        }

        std::string to_string(uint8_t indent=0) const
        {
            std::stringstream ss;
            for (auto [from, size] : regions)
                ss << std::string(indent * 2, ' ') << from << " , " << size << std::endl;

            return ss.str();
        }

        auto biggest_chunk() const
        {
            std::tuple< uint64_t, uint64_t > out{ 0, 0 };
            for (auto &[from, size] : regions)
                if (size > std::get<1>(out))
                    out = {from, size};

            return out;
        }

        std::optional< uint64_t > get_hole() const
        {
            std::vector< uint64_t > out;
            for (auto &[from, size] : regions)
            {
                auto it = regions.find(from + 2);
                if (it == regions.end() || size != 1)
                    continue;

                if (it->second == 1)
                    out.push_back(from + 1);
            }
            if (out.size() == 1)
                return { out[0] };
            return {};
        }

        template< typename T >
        static auto is_empty(const std::optional< T > &x)
        -> std::enable_if_t< std::is_base_of_v< has_regions, T >, bool >
        {
            return !x || x->empty();
        }

        void pre(uint64_t anchor, uint64_t from, uint64_t size)
        {
            auto original_end = anchor + regions[anchor];
            regions.erase(anchor);
            regions[from] = std::max(original_end - from, size);
        }

        void post(uint64_t anchor, uint64_t from, uint64_t size)
        {
            regions[anchor] = std::max(regions[anchor], from + size - anchor);
        }

        void add(uint64_t from, uint64_t size)
        {
            for (const auto &[x, y] : regions)
            {
                if (x <= from && x + y >= from)
                    return post(x, from, size);
                if (from <= x && from + size >= x)
                    return pre(x, from, size);
            }
            regions[from] = size;
        }

        void add(const has_regions &other)
        {
            // It is expected both are rather small
            for (const auto &[from, size] : other.regions)
                add(from, size);
        }
    };

    struct Reg : has_regions
    {
        // Actual instances for given reg
        using materialized_t = std::vector< bool >;
        using materializations_t = std::unordered_set< materialized_t >;
        using reg_t = std::string;

        using has_regions::has_regions;

        // NOTE(lukas): We want them ordererd.
        std::map< reg_t, materializations_t > translation_map;
        std::unordered_set< reg_t > dirty;

        Reg(region_t o) : has_regions(std::move(o)) {}

        std::string to_string(uint8_t indent=0) const
        {
            std::stringstream ss;
            std::string _indent(indent * 2, ' ');

            ss << _indent << "Regions:" << std::endl;
            ss << this->has_regions::to_string(indent + 1);
            ss << _indent << "Translation map:" << std::endl;

            for (auto &[reg, all_mats] : translation_map)
            {
                ss << std::string((indent + 1) * 2, ' ' ) << reg
                   << (is_dirty(reg) ? " (dirty)" : "") << std::endl;

                for (auto &mat : all_mats)
                {
                    ss << std::string((indent + 2) * 2, ' ');

                    if (mat.empty()) {
                        ss << "( none )";
                    } else {
                        for (auto b : mat)
                            ss << b;
                    }
                    ss << std::endl;
                }
            }
            return ss.str();
        }

        bool is_dirty(const reg_t &reg) const
        {
            return dirty.count(reg);
        }

        void mark_dirty(const reg_t &reg)
        {
            check(translation_map.count(reg));
            dirty.insert(reg);
        }

        uint64_t translation_entries_count() const
        {
            uint64_t acc = 0;
            for (auto &[_, mats] : translation_map)
                acc += mats.size();
            return acc;
        }

        static std::string make_bitstring(const std::vector< bool > &from)
        {
            std::string out;
            for (std::size_t i = 0; i < from.size(); ++i)
                out += (from[i]) ? '1' : '0';

            return out;
        }

        std::map< std::string, reg_t > translation_bytes_map() const
        {
            std::map<std::string, reg_t> out;
            for (auto &[reg, mats] : translation_map)
                for (auto &encoding : mats)
                    out[make_bitstring(encoding)] = reg;
            return out;
        }

        bool is_saturated_by_zeroes() const
        {
            if (translation_map.size() != 1)
                return false;
            const auto &[key, encodings] = *translation_map.begin();
            if (!std::string_view(key).starts_with("__remill_zero_i"))
                return false;

            return encodings.size() ==
                   static_cast< std::size_t >(std::pow(2, this->region_bitsize()));
        }
    };

    struct Immediate : has_regions
    {
      using has_regions::has_regions;
    };

    struct Address
    {
        std::optional< Reg > base_reg;
        std::optional< Reg > index_reg;
        std::optional< Reg > segment;
        std::optional< Immediate > scale;
        std::optional< Immediate > displacement;

        Address() = default;

        bool operator==(const Address &) const = default;

        bool empty() const
        {
            return has_regions::is_empty(base_reg) &&
                   has_regions::is_empty(index_reg) &&
                   has_regions::is_empty(scale) &&
                   has_regions::is_empty(displacement);
        }

        template< typename CB >
        void ForEach(CB &cb)
        {
            auto invoke = [&](auto &on_what) { if (on_what) cb(*on_what); };

            invoke(base_reg);
            invoke(index_reg);
            invoke(segment);
            invoke(scale);
            invoke(displacement);
        }

        template< typename CB >
        void ForEach(CB &cb) const
        {
            auto invoke = [&](const auto &on_what) { if (on_what) cb(*on_what); };

            invoke(base_reg);
            invoke(index_reg);
            invoke(segment);
            invoke(scale);
            invoke(displacement);
        }

        has_regions flatten_significant_regs() const
        {
            has_regions out;
            auto exec = [&](const auto &other) { out.add(other); };
            auto invoke = [&](const auto &on_what) { if (on_what) exec(*on_what); };
            invoke(base_reg);
            invoke(index_reg);
            return out;
        }

        bool present(std::size_t idx) const
        {
            bool out = false;
            auto collect = [&](auto &op) { out |= op.present(idx); };
            return ForEach(collect), out;
        }

        std::string to_string(uint8_t indent) const
        {
            auto make_indent = [&](auto count) { return std::string(count * 2, ' '); };

            std::stringstream ss;
            auto format = [&](const auto &what, const std::string &prefix) {
                ss << make_indent(indent) << prefix << ": " << std::endl;
                if (what)
                    ss << what->to_string(indent + 1);
                else
                    ss << make_indent(indent + 1u) << "( not set )\n";
            };

            format(base_reg, "Base");
            format(index_reg, "Index");
            format(segment, "Segment");
            format(scale, "Scale");
            format(displacement, "Displacement");

            return ss.str();
        }
    };

    struct Shift : has_regions
    {
        using has_regions::has_regions;
    };

    struct Operand
    {
        using op_type = remill::Operand::Type;

        std::optional<Immediate> immediate;
        std::optional<Reg> reg;
        std::optional<Address> address;
        std::optional<Shift> shift;

        bool operator==(const Operand &) const = default;

        template<typename T, typename ... Args>
        static auto As(Args && ... args) {
            static_assert(std::is_same_v<T, Immediate>
                          || std::is_same_v<T, Reg>
                          || std::is_same_v<T, Address>);

            Operand op;
            if constexpr (std::is_same_v<T, Immediate>)
              op.immediate = Immediate(std::forward<Args>(args)...);
            else if (std::is_same_v<T, Reg>)
              op.reg = Reg(std::forward<Args>(args)...);
            else if (std::is_same_v<T, Address>)
              op.address = Address();

            return op;
        }

        template< typename CB >
        void for_each_existing(CB &cb)
        {
            auto invoke = [&](auto &on_what) { if (on_what) cb(*on_what); };

            invoke(immediate);
            invoke(reg);
            invoke(address);
            invoke(shift);
        }
        template< typename CB >
        void for_each_existing(CB &cb) const
        {
            auto invoke = [&](const auto &on_what) { if (on_what) cb(*on_what); };

            invoke(immediate);
            invoke(reg);
            invoke(address);
            invoke(shift);
        }

        bool present(std::size_t idx) const
        {
            bool out = false;
            auto collect = [&](auto &op) { out |= op.present(idx); };
            return for_each_existing(collect), out;
        }

        bool IsHusk() const
        {
            // No operand is specified, therefore this is not a husk but a hardcoded op
            if (!reg && !immediate && !shift && !address)
                return false;

            check(!shift.has_value()) << "Cannot handle shift";
            check(  static_cast<uint8_t>(reg.has_value())
                  + static_cast<uint8_t>(immediate.has_value())
                  + static_cast<uint8_t>(address.has_value())
                  + static_cast<uint8_t>(shift.has_value())
            ) << "shadowinst::operand is of multiple types!";

            return (reg && reg->empty())     || (immediate && immediate->empty()) ||
                   (shift && shift->empty()) || (address && address->empty());
        }

        bool empty() const
        {
            auto is_empty = [](const auto &op) { return op && op->size(); };
            return is_empty(immediate) && is_empty(reg);
        }
    };

    struct Instruction
    {
        // We need to fullfil that is pointers to operands are never invalidated!
        // TODO(lukas): See if ^ cannot be relaxed, as it is a propbable source of errors.
        std::deque<Operand> operands;

        // Some instructions can depend on the same parts
        // of encoding - we want to keep track of those
        using operand_ctx_t = std::tuple< std::size_t, Operand * >;
        std::vector< std::vector< operand_ctx_t > > deps;
        std::vector< std::set< uint32_t > > dirt;

        Instruction() = default;
        Instruction(const Instruction &other) : operands(other.operands)
        {
            for (const auto &o_cluster : other.deps)
            {
                std::vector< operand_ctx_t > cluster;
                for (const auto &[idx, _] : o_cluster)
                    cluster.emplace_back(idx, &operands[idx]);
                deps.push_back(std::move(cluster));
            }
        }

        Instruction(Instruction &&) = default;

        Instruction &operator=(const Instruction&) = delete;
        Instruction &operator=(Instruction &&) = default;

        auto size() const { return operands.size(); }
        const auto &operator[](std::size_t idx) const { return operands[idx]; }
        auto &operator[](std::size_t idx) { return operands[idx]; }

        bool operator==(const Instruction &o) const
        {
            if (o.operands != operands)
                return false;

            auto cmp_op_ctx = [](const operand_ctx_t &ctx, const operand_ctx_t &octx) {
                return std::get< 0 >(ctx) == std::get< 0 >(octx);
            };

            auto cmp_deps = [&](const auto &cluster, const auto &ocluster) {
                return std::equal(cluster.begin(), cluster.end(), ocluster.begin(), cmp_op_ctx);
            };

            return std::equal(deps.begin(), deps.end(), o.deps.begin(), cmp_deps);
        }

        bool present(std::size_t idx) const
        {
            for (auto &op : operands)
                if (op.present(idx))
                    return true;

            return false;
        }

        bool validate()
        {
            uint32_t dirty_count = 0;
            for (const auto &op : operands)
              if (op.reg && !op.reg->dirty.empty())
                ++dirty_count;

            return dirty_count <= 1;
        }

        template< typename T, typename ...Args >
        auto &Add(Args && ... args)
        {
            return operands.emplace_back(Operand::As<T>(std::forward<Args>(args)...));
        }

        template< typename ...Args >
        auto &Replace(std::size_t idx, remill::Operand::Type type, Args && ...args)
        {
            switch(type)
            {
                case remill::Operand::Type::kTypeRegister: {
                    operands[idx] = Operand::As<Reg>(std::forward<Args>(args)...);
                    return operands[idx];
                }
                case remill::Operand::Type::kTypeImmediate : {
                    operands[idx] = Operand::As<Immediate>(std::forward<Args>(args)...);
                    return operands[idx];
                }
                case remill::Operand::Type::kTypeAddress : {
                    operands[idx] = Operand::As<Address>(std::forward<Args>(args)...);
                    return operands[idx];
                }
                default :
                    unreachable()
                        << "Cannot replace shadow operand with"
                        << "type that is neither reg, addr nor imm.";
            }
        }

        region_t IdentifiedRegions() const
        {
            region_t out;

            for (const auto &op : operands)
            {
                // TODO(lukas): Sanity check may be in order here
                if (op.immediate)
                    for (auto [from, size] : *op.immediate)
                        out[from] = size;

                // TODO(lukas): Looks like copy pasta, but eventually these
                //              attributes will have different structure
                if (op.reg)
                    for (auto [from, size] : op.reg->regions)
                        out[from] = size;

                if (op.shift)
                  for (auto [from, size] : *op.shift)
                    out[from] = size;

                if (op.address)
                {
                    if (op.address->base_reg)
                        for (auto [from, size] : op.address->base_reg->regions)
                            out[from] = size;

                    if (op.address->index_reg)
                        for (auto [from, size] : op.address->index_reg->regions)
                            out[from] = size;

                    if (op.address->scale)
                        for (auto [from, size] : op.address->scale->regions)
                            out[from] = size;

                    if (op.address->displacement)
                        for (auto [from, size] : op.address->displacement->regions)
                            out[from] = size;
                }
            }
            return out;
        }

        // We need lenght of the entire region to be able to calculate last region
        region_t UnknownRegions(uint64_t lenght) const
        {
            return Invert(IdentifiedRegions(), lenght);
        }

        std::string to_string() const
        {
            std::stringstream ss;
            ss << "Shadowinst:" << std::endl;

            for (const auto &cluster : deps) {
                ss << "Deps cluster: [ ";
                for (const auto &[idx, _] : cluster)
                    ss << idx << " ";
                ss << "]" << std::endl;
            }

            for (const auto &dirts : dirt)
            {
                ss << "dirt( ";
                for (auto d : dirts)
                    ss << d << " ";
                ss << ")" << std::endl;
            }

            for (const auto &op : operands) {
                ss << " OP" << std::endl;

                // TODO(lukas): Sanity check may be in order here
                if (op.immediate) {
                    ss << "  Immediate:" << std::endl;
                    for (auto [from, size] : *op.immediate)
                        ss << "    " << from << " , " << size << std::endl;;
                }
                // TODO(lukas): Looks like copy pasta, but eventually these
                //              attributes will have different structure
                if (op.reg) {
                    ss << "  Reg:" << std::endl;
                    ss << op.reg->to_string(2);
                }
                if (op.shift) {
                    ss << "  Shift:" << std::endl;
                    for (auto [from, size] : *op.shift)
                        ss << " " << from << " , " << size << std::endl;
                }
                if (op.address) {
                    ss << "  Address" << std::endl;
                    ss << op.address->to_string(2);
                }
            }
            ss << "  (done)" << std::endl;
            return ss.str();
        }
    };

    static inline std::string as_str(uint64_t from, uint64_t size)
    {
        std::stringstream ss;
        ss << "[ " << from << ", " << size << " ]";
        return ss.str();
    }

    static inline std::string as_str(const std::tuple< uint64_t, uint64_t > &interval)
    {
        std::stringstream ss;
        auto [from, size] = interval;
        ss << "[ " << from << ", " << size << " ]";
        return ss.str();
    }

    static inline std::string remove_shadowed(const Instruction &s_inst,
                                              const std::string &bytes)
    {
        std::string out;
        for (std::size_t i = 0; i < bytes.size(); ++i)
            if (s_inst.present(i))
                out += bytes[bytes.size() - 1 - i];
        return out;
    }
} // namespace circ::shadowinst
