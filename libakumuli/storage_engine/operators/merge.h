#pragma once

#include "operator.h"

#include <boost/heap/skew_heap.hpp>
#include <boost/range.hpp>
#include <boost/range/iterator_range.hpp>


namespace Akumuli {
namespace StorageEngine {


template<int dir>  // 0 - forward, 1 - backward
struct SeriesOrder {
    typedef std::tuple<aku_Timestamp, aku_ParamId> KeyType;
    typedef std::tuple<KeyType, double, u32> HeapItem;
    typedef std::tuple<aku_ParamId, aku_Timestamp> InvKeyType;
    std::greater<InvKeyType> greater_;
    std::less<InvKeyType> less_;

    bool operator () (HeapItem const& lhs, HeapItem const& rhs) const {
        auto lkey = std::get<0>(lhs);
        InvKeyType ilhs = std::make_tuple(std::get<1>(lkey), std::get<0>(lkey));
        auto rkey = std::get<0>(rhs);
        InvKeyType irhs = std::make_tuple(std::get<1>(rkey), std::get<0>(rkey));
        if (dir == 0) {
            return greater_(ilhs, irhs);
        }
        return less_(ilhs, irhs);
    }
};


template<template <int dir> class CmpPred>
struct MergeOperator : TupleOperator {
    std::vector<std::unique_ptr<RealValuedOperator>> iters_;
    std::vector<aku_ParamId> ids_;
    bool forward_;

    enum {
        RANGE_SIZE=1024
    };

    struct Range {
        std::vector<aku_Timestamp> ts;
        std::vector<double> xs;
        aku_ParamId id;
        size_t size;
        size_t pos;

        Range(aku_ParamId id)
            : id(id)
            , size(0)
            , pos(0)
        {
            ts.resize(RANGE_SIZE);
            xs.resize(RANGE_SIZE);
        }

        void advance() {
            pos++;
        }

        void retreat() {
            assert(pos);
            pos--;
        }

        bool empty() const {
            return !(pos < size);
        }

        std::tuple<aku_Timestamp, aku_ParamId> top_key() const {
            return std::make_tuple(ts.at(pos), id);
        }

        double top_value() const {
            return xs.at(pos);
        }
    };

    std::vector<Range> ranges_;

    MergeOperator(std::vector<aku_ParamId>&& ids, std::vector<std::unique_ptr<RealValuedOperator>>&& it)
        : iters_(std::move(it))
        , ids_(std::move(ids))
    {
        if (!iters_.empty()) {
            forward_ = iters_.front()->get_direction() == RealValuedOperator::Direction::FORWARD;
        }
        if (iters_.size() != ids_.size()) {
            AKU_PANIC("MergeIterator - broken invariant");
        }
    }

    virtual std::tuple<aku_Status, size_t> read(u8* dest, size_t size) override {
        if (forward_) {
            return kway_merge<0>(dest, size);
        }
        return kway_merge<1>(dest, size);
    }

    template<int dir>
    std::tuple<aku_Status, size_t> kway_merge(u8* dest, size_t size) {
        if (iters_.empty()) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        size_t outpos = 0;
        if (ranges_.empty()) {
            // `ranges_` array should be initialized on first call
            for (size_t i = 0; i < iters_.size(); i++) {
                Range range(ids_[i]);
                aku_Status status;
                size_t outsize;
                std::tie(status, outsize) = iters_[i]->read(range.ts.data(), range.xs.data(), RANGE_SIZE);
                if (status == AKU_SUCCESS || (status == AKU_ENO_DATA && outsize != 0)) {
                    range.size = outsize;
                    range.pos  = 0;
                    ranges_.push_back(std::move(range));
                }
                if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
                    return std::make_tuple(status, 0);
                }
            }
        }

        typedef CmpPred<dir> Comp;
        typedef typename Comp::HeapItem HeapItem;
        typedef typename Comp::KeyType KeyType;
        typedef boost::heap::skew_heap<HeapItem, boost::heap::compare<Comp>> Heap;
        Heap heap;

        int index = 0;
        for(auto& range: ranges_) {
            if (!range.empty()) {
                KeyType key = range.top_key();
                heap.push(std::make_tuple(key, range.top_value(), index));
            }
            index++;
        }

        enum {
            KEY = 0,
            VALUE = 1,
            INDEX = 2,
            TIME = 0,
            ID = 1,
        };

        while(!heap.empty()) {
            HeapItem item = heap.top();
            KeyType point = std::get<KEY>(item);
            u32 index = std::get<INDEX>(item);
            aku_Sample sample;
            sample.paramid = std::get<ID>(point);
            sample.timestamp = std::get<TIME>(point);
            sample.payload.type = AKU_PAYLOAD_FLOAT;
            sample.payload.size = sizeof(aku_Sample);
            sample.payload.float64 = std::get<VALUE>(item);
            if (size - outpos >= sizeof(aku_Sample)) {
                memcpy(dest + outpos, &sample, sizeof(sample));
                outpos += sizeof(sample);
            } else {
                // Output buffer is fully consumed
                return std::make_tuple(AKU_SUCCESS, size);
            }
            heap.pop();
            ranges_[index].advance();
            if (ranges_[index].empty()) {
                // Refill range if possible
                aku_Status status;
                size_t outsize;
                std::tie(status, outsize) = iters_[index]->read(ranges_[index].ts.data(), ranges_[index].xs.data(), RANGE_SIZE);
                if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
                    return std::make_tuple(status, 0);
                }
                ranges_[index].size = outsize;
                ranges_[index].pos  = 0;
            }
            if (!ranges_[index].empty()) {
                KeyType point = ranges_[index].top_key();
                heap.push(std::make_tuple(point, ranges_[index].top_value(), index));
            }
        }
        if (heap.empty()) {
            iters_.clear();
            ranges_.clear();
        }
        // All iterators are fully consumed
        return std::make_tuple(AKU_ENO_DATA, outpos);
    }

};

/**
 * Merges several materialized tuple sequences into one
 */
struct MergeJoinOperator : TupleOperator {

    enum {
        RANGE_SIZE=1024
    };

    template<int dir>
    struct OrderByTimestamp {
        typedef std::tuple<aku_Timestamp, aku_ParamId> KeyType;
        struct HeapItem {
            KeyType key;
            aku_Sample const* sample;
            size_t index;
        };
        std::greater<KeyType> greater_;
        std::less<KeyType> less_;

        bool operator () (HeapItem const& lhs, HeapItem const& rhs) const {
            if (dir == 0) {
                return greater_(lhs.key, rhs.key);
            }
            return less_(lhs.key, rhs.key);
        }
    };

    struct Range {
        std::vector<u8> buffer;
        u32 size;
        u32 pos;
        u32 last_advance;

        Range()
            : size(0u)
            , pos(0u)
            , last_advance(0u)
        {
            buffer.resize(RANGE_SIZE*sizeof(aku_Sample));
        }

        void advance(u32 sz) {
            pos += sz;
            last_advance = sz;
        }

        void retreat() {
            assert(pos > last_advance);
            pos -= last_advance;
        }

        bool empty() const {
            return !(pos < size);
        }

        std::tuple<aku_Timestamp, aku_ParamId> top_key() const {
            u8 const* top = buffer.data() + pos;
            aku_Sample const* sample = reinterpret_cast<aku_Sample const*>(top);
            return std::make_tuple(sample->timestamp, sample->paramid);
        }

        aku_Sample const* top() const {
            u8 const* top = buffer.data() + pos;
            return reinterpret_cast<aku_Sample const*>(top);
        }
    };

    std::vector<std::unique_ptr<TupleOperator>> iters_;
    bool forward_;
    std::vector<Range> ranges_;

    MergeJoinOperator(std::vector<std::unique_ptr<TupleOperator>>&& it, bool forward);

    virtual std::tuple<aku_Status, size_t> read(u8* dest, size_t size) override;

    template<int dir>
    std::tuple<aku_Status, size_t> kway_merge(u8* dest, size_t size) {
        if (iters_.empty()) {
            return std::make_tuple(AKU_ENO_DATA, 0);
        }
        size_t outpos = 0;
        if (ranges_.empty()) {
            // `ranges_` array should be initialized on first call
            for (size_t i = 0; i < iters_.size(); i++) {
                Range range;
                aku_Status status;
                size_t outsize;
                std::tie(status, outsize) = iters_[i]->read(range.buffer.data(), range.buffer.size());
                if (status == AKU_SUCCESS || (status == AKU_ENO_DATA && outsize != 0)) {
                    range.size = static_cast<u32>(outsize);
                    range.pos  = 0;
                    ranges_.push_back(std::move(range));
                }
                if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
                    return std::make_tuple(status, 0);
                }
            }
        }

        typedef OrderByTimestamp<dir> Comp;
        typedef typename Comp::HeapItem HeapItem;
        typedef typename Comp::KeyType KeyType;
        typedef boost::heap::skew_heap<HeapItem, boost::heap::compare<Comp>> Heap;
        Heap heap;

        size_t index = 0;
        for(auto& range: ranges_) {
            if (!range.empty()) {
                KeyType key = range.top_key();
                heap.push({key, range.top(), index});
            }
            index++;
        }

        while(!heap.empty()) {
            HeapItem item = heap.top();
            size_t index = item.index;
            aku_Sample const* sample = item.sample;
            if (size - outpos >= sample->payload.size) {
                memcpy(dest + outpos, sample, sample->payload.size);
                outpos += sample->payload.size;
            } else {
                // Output buffer is fully consumed
                return std::make_tuple(AKU_SUCCESS, outpos);
            }
            heap.pop();
            ranges_[index].advance(sample->payload.size);
            if (ranges_[index].empty()) {
                // Refill range if possible
                aku_Status status;
                size_t outsize;
                std::tie(status, outsize) = iters_[index]->read(ranges_[index].buffer.data(), ranges_[index].buffer.size());
                if (status != AKU_SUCCESS && status != AKU_ENO_DATA) {
                    return std::make_tuple(status, 0);
                }
                ranges_[index].size = static_cast<u32>(outsize);
                ranges_[index].pos  = 0;
            }
            if (!ranges_[index].empty()) {
                KeyType point = ranges_[index].top_key();
                heap.push({point, ranges_[index].top(), index});
            }
        }
        if (heap.empty()) {
            iters_.clear();
            ranges_.clear();
        }
        // All iterators are fully consumed
        return std::make_tuple(AKU_ENO_DATA, outpos);
    }

};

}}  // namespace
