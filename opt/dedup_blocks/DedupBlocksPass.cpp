/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DedupBlocksPass.h"

#include <atomic>
#include <boost/optional.hpp>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexOutput.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "ParallelWalkers.h"
#include "Resolver.h"
#include "Transform.h"

/*
 * This pass removes blocks that are duplicates in a method.
 *
 * If a method has multiple blocks with the same code and the same successors,
 * delete all but one of the blocks. Naming one of them the canonical block.
 *
 * Then, reroute all the predecessors of all the blocks to that canonical block.
 */

namespace {

using hash_t = std::size_t;

struct BlockAsKey {
  IRCode* code;
  Block* block;

  BlockAsKey(IRCode* c, Block* b) : code(c), block(b) {}

  bool operator==(const BlockAsKey& other) const {
    return same_successors(other) && same_try_regions(other) &&
           same_code(other);
  }

  // Structural equality of opcodes except branch targets are ignored
  // because they are unknown until we sync back to DexInstructions.
  bool same_code(const BlockAsKey& other) const {
    return InstructionIterable(block).structural_equals(
        InstructionIterable(other.block));
  }

  // The blocks must also have the exact same successors
  // (and reached in the same ways)
  bool same_successors(const BlockAsKey& other) const {
    const auto& b1_succs = this->block->succs();
    const auto& b2_succs = other.block->succs();
    if (b1_succs.size() != b2_succs.size()) {
      return false;
    }
    for (Block* b1_succ : b1_succs) {
      const auto& in_b2 = std::find(b2_succs.begin(), b2_succs.end(), b1_succ);
      if (in_b2 == b2_succs.end()) {
        // b1 has a succ that b2 doesn't
        return false;
      }

      if (code->cfg().edge(this->block, b1_succ) !=
          code->cfg().edge(other.block, *in_b2)) {
        // successors are the same but the type of connecting
        // edges are different.
        // Like so:
        //
        // B0: if-eqz v0, B3
        // B1: return
        // B2: if eqz v0, B1
        // B3: add-int v1, 1
        //
        // B0's succs are B1 (fallthru) and B3 (branch)
        // B2's succs are B3 (fallthru) and B1 (branch)
        return false;
      }
    }
    return true;
  }

  // return true if in the same try region (or both in no try region at all)
  bool same_try_regions(const BlockAsKey& other) const {
    return transform::find_active_catch(code, this->block->begin()) ==
           transform::find_active_catch(code, other.block->begin());
  }
};

struct BlockHasher {
  hash_t operator()(const BlockAsKey& key) const {
    hash_t result = 0;
    for (auto& mie : InstructionIterable(key.block)) {
      result ^= mie.insn->hash();
    }
    return result;
  }
};

class DedupBlocksImpl {
 public:
  DedupBlocksImpl(const std::vector<DexClass*>& scope,
                  PassManager& mgr,
                  const DedupBlocksPass::Config& config)
      : m_scope(scope), m_mgr(mgr), m_config(config) {}

  void run() {
    walk_methods_parallel_simple(m_scope, [this](DexMethod* method) {
      if (m_config.method_black_list.count(method) != 0) {
        return;
      }

      IRCode* code = method->get_code();
      if (code == nullptr) {
        return;
      }
      code->build_cfg();

      duplicates_t dups = collect_duplicates(code);
      if (dups.size() > 0) {
        record_stats(dups);
        deduplicate(dups, method);
      }
    });
    report_stats();
  }

 private:
  using duplicates_t =
      std::unordered_map<BlockAsKey, std::unordered_set<Block*>, BlockHasher>;
  const char* METRIC_BLOCKS_REMOVED = "blocks_removed";
  const char* METRIC_ELIGIBLE_BLOCKS = "eligible_blocks";
  const std::vector<DexClass*>& m_scope;
  PassManager& m_mgr;
  const DedupBlocksPass::Config& m_config;

  // Stats
  std::mutex lock;
  std::atomic_int m_num_eligible_blocks{0};
  std::atomic_int m_num_blocks_removed{0};
  // map from block size to number of blocks with that size
  std::unordered_map<size_t, size_t> m_dup_sizes;

  // Find blocks with the same exact code
  duplicates_t collect_duplicates(IRCode* code) {
    const auto& blocks = code->cfg().blocks();
    duplicates_t duplicates;

    for (Block* block : blocks) {
      if (should_remove(code->cfg(), block)) {
        duplicates[BlockAsKey{code, block}].insert(block);
        ++m_num_eligible_blocks;
      }
    }

    remove_singletons(duplicates);
    return duplicates;
  }

  // remove all but one of a duplicate set. Reroute the predecessors to the
  // canonical block
  void deduplicate(const duplicates_t& dups, DexMethod* method) {
    const auto& code = method->get_code();
    for (const auto& entry : dups) {
      std::unordered_set<Block*> blocks = entry.second;

      // canon is block with lowest id.
      Block* canon = nullptr;
      size_t canon_id = std::numeric_limits<size_t>::max();
      for (Block* block : blocks) {
        size_t id = block->id();
        if (id <= canon_id) {
          canon_id = id;
          canon = block;
        }
      }

      for (Block* block : blocks) {
        if (block->id() == canon_id) {
          // We remove the debug line information because it will be incorrect
          // for every block we reroute here. When there's no debug info,
          // the jvm will report the error on the closing brace of the function.
          // It's not perfect but it's better than incorrect information.
          code->remove_debug_line_info(canon);
        } else {
          transform::replace_block(code, block, canon);
          ++m_num_blocks_removed;
        }
      }
    }
  }

  static bool should_remove(const ControlFlowGraph& cfg, Block* block) {
    if (!has_opcodes(block)) {
      return false;
    }

    if (is_catch(block)) {
      // TODO. Should be possible. Skip now for simplicity
      return false;
    }

    // Deal with a verification error like this
    //
    // A: new-instance v0
    //    add-int v1, v2, v3       (this is here to clarify that A != C)
    // B: v0 <init>
    //
    //    ...
    //
    // C: new-instance v0
    // D: v0 <init>
    //
    // B == D. Coalesce!
    //
    // A: new-instance v0
    //    add-int v1, v2, v3
    // B: v0 <init>
    //
    // C: new-instance v0
    //    goto B
    //
    // But the verifier doesn't like this. When it merges v0 on B,
    // it declares it to be a conflict because they were instantiated
    // on different lines.
    // See androidxref.com/6.0.1_r10/xref/art/runtime/verifier/reg_type.cc#684
    //
    // It would be impossible to write this in java, but if you tried it would
    // look like this
    //
    // if (someCondition) {
    //   Foo a;
    // } else {
    //   Foo b;
    // }
    // (a or b) = new Foo();
    //
    // We try to avoid this situation by not considering blocks that call
    // constructors, but this isn't bulletproof. FIXME
    if (calls_constructor(block)) {
      return false;
    }

    // We can't replace blocks that are involved in a fallthrough because they
    // depend on their position in the list of instructions. Deduplicating
    // will involve sending control to a block in a different place.
    for (Block* pred : block->preds()) {
      if (!has_opcodes(pred)) {
        // Skip this case because it's complicated. It should be fixed by
        // upcoming changes to the CFG
        return false;
      }

      if (is_fallthrough(cfg, pred, block)) {
        return false;
      }
    }

    for (Block* succ : block->succs()) {
      if (is_fallthrough(cfg, block, succ)) {
        return false;
      }
    }

    return true;
  }

  static bool calls_constructor(Block* block) {
    for (const auto& mie : InstructionIterable(block)) {
      if (is_invoke(mie.insn->opcode())) {
        auto meth =
            resolve_method(mie.insn->get_method(), opcode_to_search(mie.insn));
        if (meth != nullptr && is_init(meth)) {
          return true;
        }
      }
    }
    return false;
  }

  // `pred` falls through to `succ` iff their connecting edge in the CFG is a
  // goto but `pred`'s last instruction isn't a goto
  static bool is_fallthrough(const ControlFlowGraph& cfg,
                             Block* pred,
                             Block* succ) {
    always_assert_log(has_opcodes(pred), "need opcodes");
    const auto& flags = cfg.edge(pred, succ);
    const auto& last_of_pred = last_opcode(pred);

    return flags[EDGE_GOTO] && !is_goto(last_of_pred->insn->opcode());
  }

  void record_stats(const duplicates_t& duplicates) {
    std::lock_guard<std::mutex> guard{lock};
    for (const auto& entry : duplicates) {
      std::unordered_set<Block*> blocks = entry.second;
      // all blocks have the same number of opcodes
      Block* block = *blocks.begin();
      m_dup_sizes[num_opcodes(block)] += blocks.size();
    }
  }

  void report_stats() {
    int eligible_blocks = m_num_eligible_blocks.load();
    int removed = m_num_blocks_removed.load();
    m_mgr.incr_metric(METRIC_ELIGIBLE_BLOCKS, eligible_blocks);
    m_mgr.incr_metric(METRIC_BLOCKS_REMOVED, removed);
    TRACE(DEDUP_BLOCKS, 2, "%d eligible_blocks\n", eligible_blocks);

    for (const auto& entry : m_dup_sizes) {
      TRACE(DEDUP_BLOCKS,
            2,
            "found %d duplicate blocks with %d instructions\n",
            entry.second,
            entry.first);
    }

    TRACE(DEDUP_BLOCKS, 1, "%d blocks removed\n", removed);
  }

  // remove sets with only one block
  static void remove_singletons(duplicates_t& duplicates) {
    for (auto it = duplicates.begin(); it != duplicates.end();) {
      if (it->second.size() <= 1) {
        it = duplicates.erase(it);
      } else {
        ++it;
      }
    }
  }

  static boost::optional<MethodItemEntry&> last_opcode(Block* block) {
    for (auto it = block->rbegin(); it != block->rend(); it++) {
      if (it->type == MFLOW_OPCODE) {
        return *it;
      }
    }
    return boost::none;
  }

  static bool has_opcodes(Block* block) {
    const auto& it = InstructionIterable(block);
    return it.begin() != it.end();
  }

  static size_t num_opcodes(Block* block) {
    size_t result = 0;
    const auto& iterable = InstructionIterable(block);
    for (auto it = iterable.begin(); it != iterable.end(); it++) {
      result++;
    }
    return result;
  }

  static void print_dups(duplicates_t dups) {
    TRACE(DEDUP_BLOCKS, 4, "duplicate blocks set: {\n");
    for (const auto& entry : dups) {
      TRACE(DEDUP_BLOCKS, 4, "  hash = %lu\n", BlockHasher{}(entry.first));
      for (Block* b : entry.second) {
        TRACE(DEDUP_BLOCKS, 4, "    block %d\n", b->id());
        for (const MethodItemEntry& mie : *b) {
          TRACE(DEDUP_BLOCKS, 4, "      %s\n", SHOW(mie));
        }
      }
    }
    TRACE(DEDUP_BLOCKS, 4, "} end duplicate blocks set\n");
  }
};
}

void DedupBlocksPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  const auto& scope = build_class_scope(stores);
  DedupBlocksImpl impl(scope, mgr, m_config);
  impl.run();
}

static DedupBlocksPass s_pass;
