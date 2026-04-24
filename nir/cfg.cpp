#include "nir/cfg.hpp"

#include <utility>

namespace nebula::nir {

namespace {

struct Builder {
  Cfg cfg;
  struct LoopTargets {
    NodeId continue_target{};
    std::vector<NodeId> break_fallthroughs;
  };
  std::vector<LoopTargets> loop_stack;

  NodeId new_node(NodeKind kind, const Stmt* stmt, std::vector<std::string> regions,
                  std::vector<std::string> ann) {
    NodeId id = cfg.nodes.size();
    CfgNode n;
    n.id = id;
    n.kind = kind;
    n.stmt = stmt;
    n.region_stack = std::move(regions);
    n.annotation_stack = std::move(ann);
    cfg.nodes.push_back(std::move(n));
    return id;
  }

  void add_edge(NodeId from, NodeId to) { cfg.nodes[from].succ.push_back(to); }

  // Build returns fallthrough predecessor nodes after the statement/block.
  std::vector<NodeId> build_block(const Block& b, std::vector<NodeId> preds,
                                  std::vector<std::string> regions,
                                  std::vector<std::string> ann) {
    for (const auto& s : b.stmts) {
      preds = build_stmt(s, std::move(preds), regions, ann);
    }
    return preds;
  }

  std::vector<std::string> extend_ann(const std::vector<std::string>& base,
                                      const std::vector<std::string>& add) {
    std::vector<std::string> out = base;
    out.insert(out.end(), add.begin(), add.end());
    return out;
  }

  std::vector<NodeId> build_stmt(const Stmt& s, std::vector<NodeId> preds,
                                 std::vector<std::string> regions,
                                 std::vector<std::string> ann) {
    // Statement annotations scope over the statement's subtree (including loop/region bodies).
    const std::vector<std::string> ann_here = extend_ann(ann, s.annotations);

    // Helper: create a node for this statement and connect preds to it.
    auto mk_stmt_node = [&](const Stmt* sptr) -> NodeId {
      NodeId id = new_node(NodeKind::Stmt, sptr, regions, ann_here);
      for (NodeId p : preds) add_edge(p, id);
      return id;
    };

    // Dispatch
    return std::visit(
        [&](auto&& n) -> std::vector<NodeId> {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Stmt::Declare> || std::is_same_v<N, Stmt::Let> ||
                        std::is_same_v<N, Stmt::ExprStmt> ||
                        std::is_same_v<N, Stmt::AssignVar> ||
                        std::is_same_v<N, Stmt::AssignField>) {
            NodeId id = mk_stmt_node(&s);
            return {id};
          } else if constexpr (std::is_same_v<N, Stmt::Return>) {
            NodeId id = mk_stmt_node(&s);
            add_edge(id, cfg.exit);
            return {}; // no fallthrough
          } else if constexpr (std::is_same_v<N, Stmt::Region>) {
            // Region introduces region context for nested statements.
            std::vector<std::string> regions_in = regions;
            regions_in.push_back(n.name);
            // Region itself doesn't create a CFG node; body is the control-flow.
            return build_block(n.body, std::move(preds), std::move(regions_in), ann_here);
          } else if constexpr (std::is_same_v<N, Stmt::Unsafe>) {
            // Unsafe introduces no control-flow change; preserve subtree traversal.
            return build_block(n.body, std::move(preds), regions, ann_here);
          } else if constexpr (std::is_same_v<N, Stmt::If>) {
            NodeId header = mk_stmt_node(&s);
            std::vector<NodeId> then_tail =
                build_block(n.then_body, {header}, regions, ann_here);
            std::vector<NodeId> out = std::move(then_tail);
            if (n.else_body.has_value()) {
              std::vector<NodeId> else_tail =
                  build_block(*n.else_body, {header}, regions, ann_here);
              out.insert(out.end(), else_tail.begin(), else_tail.end());
            } else {
              out.push_back(header);
            }
            return out;
          } else if constexpr (std::is_same_v<N, Stmt::For>) {
            // Use the For statement itself as the loop header node.
            NodeId header = mk_stmt_node(&s);

            // Body CFG
            loop_stack.push_back(LoopTargets{header, {}});
            std::vector<NodeId> body_preds = {header};
            std::vector<NodeId> body_fallthrough =
                build_block(n.body, std::move(body_preds), regions, ann_here);
            std::vector<NodeId> break_fallthroughs = std::move(loop_stack.back().break_fallthroughs);
            loop_stack.pop_back();

            // Backedge(s): end of body goes back to header
            for (NodeId bt : body_fallthrough) add_edge(bt, header);

            // Loop exit: header also falls through to next statement; represent this by returning
            // the header plus any `break` nodes as fallthrough predecessors.
            break_fallthroughs.push_back(header);
            return break_fallthroughs;
          } else if constexpr (std::is_same_v<N, Stmt::While>) {
            NodeId header = mk_stmt_node(&s);
            loop_stack.push_back(LoopTargets{header, {}});
            std::vector<NodeId> body_fallthrough =
                build_block(n.body, {header}, regions, ann_here);
            std::vector<NodeId> break_fallthroughs = std::move(loop_stack.back().break_fallthroughs);
            loop_stack.pop_back();
            for (NodeId bt : body_fallthrough) add_edge(bt, header);
            break_fallthroughs.push_back(header);
            return break_fallthroughs;
          } else if constexpr (std::is_same_v<N, Stmt::Break>) {
            NodeId id = mk_stmt_node(&s);
            if (!loop_stack.empty()) loop_stack.back().break_fallthroughs.push_back(id);
            return {};
          } else if constexpr (std::is_same_v<N, Stmt::Continue>) {
            NodeId id = mk_stmt_node(&s);
            if (!loop_stack.empty()) add_edge(id, loop_stack.back().continue_target);
            return {};
          } else {
            return preds;
          }
        },
        s.node);
  }
};

} // namespace

Cfg build_cfg(const Function& f) {
  Builder b;
  b.cfg.nodes.reserve(64);

  b.cfg.entry = b.new_node(NodeKind::Entry, nullptr, {}, {});
  b.cfg.exit = b.new_node(NodeKind::Exit, nullptr, {}, {});

  std::vector<NodeId> preds = {b.cfg.entry};
  if (f.body.has_value()) {
    preds = b.build_block(*f.body, std::move(preds), /*regions*/ {}, /*ann*/ f.annotations);
  }

  // Any fallthrough at end of function body reaches exit (void-return path).
  for (NodeId p : preds) b.add_edge(p, b.cfg.exit);

  return std::move(b.cfg);
}

} // namespace nebula::nir
