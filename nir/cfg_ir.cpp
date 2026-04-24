#include "nir/cfg_ir.hpp"

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace nebula::nir::cfgir {

namespace {

std::vector<std::string> split_field_path(std::string_view path) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t dot = path.find('.', start);
    const std::size_t end = (dot == std::string_view::npos) ? path.size() : dot;
    if (end > start) out.push_back(std::string(path.substr(start, end - start)));
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return out;
}

struct Builder {
  FunctionCFG out;
  struct LoopTargets {
    BlockId break_target{};
    BlockId continue_target{};
  };

  // current lexical stacks
  std::vector<std::string> region_stack;
  std::vector<std::string> ann_stack;
  std::vector<LoopTargets> loop_stack;

  // SSA environment: VarId -> ValueId (current)
  std::unordered_map<VarId, ValueId> env;

  ValueId next_value = 1;
  BlockId cur = 0;

  BlockId new_block() {
    BlockId id = static_cast<BlockId>(out.blocks.size());
    BasicBlock bb;
    bb.id = id;
    bb.region_stack = region_stack;
    bb.annotation_stack = ann_stack;
    out.blocks.push_back(std::move(bb));
    return id;
  }

  BasicBlock& bb(BlockId id) { return out.blocks.at(static_cast<std::size_t>(id)); }

  void add_edge(BlockId from, BlockId to) {
    auto& a = bb(from);
    auto& b = bb(to);
    a.succs.push_back(to);
    b.preds.push_back(from);
  }

  bool terminated(BlockId id) const {
    return out.blocks.at(static_cast<std::size_t>(id)).term.has_value();
  }

  void set_term(BlockId id, Terminator t) {
    auto& b = bb(id);
    if (b.term.has_value()) return;
    // Record edges from terminator
    std::visit(
        [&](auto&& n) {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, Terminator::Jump>) {
            add_edge(id, n.target);
          } else if constexpr (std::is_same_v<N, Terminator::Branch>) {
            add_edge(id, n.then_bb);
            add_edge(id, n.else_bb);
          } else if constexpr (std::is_same_v<N, Terminator::Ret>) {
            // no succ
          }
        },
        t.node);
    b.term = std::move(t);
  }

  ValueId emit(Op op, Ty ty, Span span, std::string debug_name = {}) {
    ValueId id = next_value++;
    Instr ins;
    ins.result = id;
    ins.ty = std::move(ty);
    ins.span = span;
    ins.debug_name = std::move(debug_name);
    ins.op = std::move(op);
    bb(cur).instrs.push_back(std::move(ins));
    return id;
  }

  ValueId emit_undef(Span span) {
    Op o;
    o.node = Op::Undef{};
    return emit(std::move(o), Ty::Unknown(), span);
  }

  ValueId lower_expr(const nir::Expr& e) {
    return std::visit(
        [&](auto&& n) -> ValueId {
          using N = std::decay_t<decltype(n)>;
          if constexpr (std::is_same_v<N, nir::Expr::IntLit>) {
            Op o;
            o.node = Op::ConstInt{n.value};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::BoolLit>) {
            Op o;
            o.node = Op::ConstBool{n.value};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::FloatLit>) {
            Op o;
            o.node = Op::ConstFloat{n.value};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::StringLit>) {
            Op o;
            o.node = Op::ConstString{n.value};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::VarRef>) {
            auto it = env.find(n.var);
            if (it == env.end()) return emit_undef(e.span);
            return it->second;
          } else if constexpr (std::is_same_v<N, nir::Expr::FieldRef>) {
            auto it = env.find(n.base_var);
            if (it == env.end()) return emit_undef(e.span);
            ValueId cur = it->second;
            const auto path = split_field_path(n.field);
            for (std::size_t i = 0; i < path.size(); ++i) {
              Op o;
              o.node = Op::LoadField{cur, path[i]};
              cur = emit(std::move(o), (i + 1 == path.size()) ? e.ty : frontend::Ty::Unknown(), e.span);
            }
            return cur;
          } else if constexpr (std::is_same_v<N, nir::Expr::TempFieldRef>) {
            ValueId cur = lower_expr(*n.base);
            const auto path = split_field_path(n.field);
            for (std::size_t i = 0; i < path.size(); ++i) {
              Op o;
              o.node = Op::LoadField{cur, path[i]};
              cur = emit(std::move(o), (i + 1 == path.size()) ? e.ty : frontend::Ty::Unknown(), e.span);
            }
            return cur;
          } else if constexpr (std::is_same_v<N, nir::Expr::EnumIsVariant>) {
            ValueId subject = lower_expr(*n.subject);
            Op o;
            o.node = Op::EnumIsVariant{subject, n.variant_name, n.variant_index};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::EnumPayload>) {
            ValueId subject = lower_expr(*n.subject);
            Op o;
            o.node = Op::EnumPayload{subject, n.variant_name, n.variant_index};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Binary>) {
            ValueId a = lower_expr(*n.lhs);
            ValueId b = lower_expr(*n.rhs);
            Op o;
            o.node = Op::Bin{n.op, a, b};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Unary>) {
            ValueId inner = lower_expr(*n.inner);
            Op o;
            o.node = Op::Unary{n.op, inner};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Call>) {
            std::vector<ValueId> args;
            args.reserve(n.args.size());
            for (const auto& a : n.args) args.push_back(lower_expr(*a));
            Op o;
            o.node = Op::Call{n.callee, std::move(args)};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Construct>) {
            std::vector<ValueId> args;
            args.reserve(n.args.size());
            for (const auto& a : n.args) args.push_back(lower_expr(*a));
            Op o;
            o.node = Op::Construct{n.type_name, std::move(args)};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Prefix>) {
            // v0.1: typecheck enforces prefix applies to constructor and forbids chains.
            if (!std::holds_alternative<nir::Expr::Construct>(n.inner->node)) {
              return lower_expr(*n.inner);
            }
            const auto& c = std::get<nir::Expr::Construct>(n.inner->node);
            std::vector<ValueId> args;
            args.reserve(c.args.size());
            for (const auto& a : c.args) args.push_back(lower_expr(*a));
            Op o;
            o.node = Op::AllocHint{n.kind, c.type_name, std::move(args)};
            return emit(std::move(o), e.ty, e.span);
          } else if constexpr (std::is_same_v<N, nir::Expr::Match>) {
            const ValueId subject = lower_expr(*n.subject);
            const BlockId after = new_block();
            std::vector<std::pair<BlockId, ValueId>> incoming_values;
            BlockId decision = cur;

            auto emit_arm_bindings = [&](const nir::Expr::Match::Arm& arm) {
              if (arm.payload_binding.has_value()) {
                Op payload_op;
                payload_op.node = Op::EnumPayload{subject, arm.variant_name, arm.variant_index};
                ValueId payload_value = emit(std::move(payload_op), arm.payload_ty, arm.span);
                env[arm.payload_binding->var] = payload_value;
              } else if (!arm.payload_struct_bindings.empty()) {
                Op payload_op;
                payload_op.node = Op::EnumPayload{subject, arm.variant_name, arm.variant_index};
                ValueId payload_value = emit(std::move(payload_op), arm.payload_ty, arm.span);
                for (const auto& field : arm.payload_struct_bindings) {
                  ValueId current_value = payload_value;
                  const auto path = split_field_path(field.field_name);
                  for (std::size_t i = 0; i < path.size(); ++i) {
                    Op load;
                    load.node = Op::LoadField{current_value, path[i]};
                    current_value = emit(std::move(load),
                                         (i + 1 == path.size()) ? field.binding.ty
                                                                : frontend::Ty::Unknown(),
                                         field.field_span);
                  }
                  env[field.binding.var] = current_value;
                }
              }
            };

            auto emit_match_condition = [&](const nir::Expr::Match::Arm& arm) -> ValueId {
              if (arm.kind == nir::Expr::Match::Arm::Kind::Bool) {
                if (arm.bool_value) return subject;
                Op negate;
                negate.node = Op::Unary{UnaryOp::Not, subject};
                return emit(std::move(negate), frontend::Ty::Bool(), arm.span);
              }
              Op variant_check;
              variant_check.node = Op::EnumIsVariant{subject, arm.variant_name, arm.variant_index};
              return emit(std::move(variant_check), frontend::Ty::Bool(), arm.span);
            };

            for (std::size_t i = 0; i < n.arms.size(); ++i) {
              const auto& arm = *n.arms[i];
              const bool unconditional =
                  arm.kind == nir::Expr::Match::Arm::Kind::Wildcard ||
                  (n.exhaustive && i + 1 == n.arms.size());

              BlockId arm_bb = decision;
              BlockId next_decision = 0;
              if (!unconditional) {
                arm_bb = new_block();
                next_decision = new_block();

                cur = decision;
                bb(decision).region_stack = region_stack;
                bb(decision).annotation_stack = ann_stack;
                const ValueId cond = emit_match_condition(arm);
                Terminator t;
                t.span = arm.span;
                t.node = Terminator::Branch{cond, arm_bb, next_decision};
                set_term(decision, std::move(t));

                bb(arm_bb).region_stack = region_stack;
                bb(arm_bb).annotation_stack = ann_stack;
                bb(next_decision).region_stack = region_stack;
                bb(next_decision).annotation_stack = ann_stack;
              }

              cur = arm_bb;
              auto saved_env = env;
              emit_arm_bindings(arm);
              if (!terminated(cur)) {
                const ValueId arm_value = lower_expr(*arm.value);
                incoming_values.push_back({cur, arm_value});
              }
              if (!terminated(cur)) {
                Terminator jump_to_after;
                jump_to_after.span = arm.span;
                jump_to_after.node = Terminator::Jump{after};
                set_term(cur, std::move(jump_to_after));
              }
              env = std::move(saved_env);

              if (unconditional) {
                break;
              }
              decision = next_decision;
            }

            if (!n.exhaustive && !terminated(decision)) {
              cur = decision;
              bb(decision).region_stack = region_stack;
              bb(decision).annotation_stack = ann_stack;
              const ValueId fallback = emit_undef(e.span);
              incoming_values.push_back({decision, fallback});
              Terminator jump_to_after;
              jump_to_after.span = e.span;
              jump_to_after.node = Terminator::Jump{after};
              set_term(decision, std::move(jump_to_after));
            }

            cur = after;
            bb(after).region_stack = region_stack;
            bb(after).annotation_stack = ann_stack;
            const ValueId phi_result = next_value++;
            Phi phi;
            phi.result = phi_result;
            phi.ty = e.ty;
            phi.debug_name = "__match_result";
            phi.incomings = std::move(incoming_values);
            bb(after).phis.push_back(std::move(phi));
            return phi_result;
          } else {
            return emit_undef(e.span);
          }
        },
        e.node);
  }

  // Lower a structured block into the current CFG, starting in current block.
  void lower_block(const nir::Block& b) {
    for (const auto& s : b.stmts) {
      if (terminated(cur)) return;
      lower_stmt(s);
    }
  }

  void lower_stmt(const nir::Stmt& s) {
    // Extend annotation stack for this statement's subtree.
    const std::size_t ann_sz = ann_stack.size();
    ann_stack.insert(ann_stack.end(), s.annotations.begin(), s.annotations.end());

    std::visit(
        [&](auto&& st) {
          using S = std::decay_t<decltype(st)>;
          if constexpr (std::is_same_v<S, nir::Stmt::Declare>) {
            env[st.var] = emit_undef(s.span);
          } else if constexpr (std::is_same_v<S, nir::Stmt::Let>) {
            ValueId v = lower_expr(*st.value);
            env[st.var] = v;
            // Attach debug name if this value is defined in current block (best-effort).
            for (auto& ins : bb(cur).instrs) {
              if (ins.result == v && ins.debug_name.empty()) {
                ins.debug_name = st.name;
                break;
              }
            }
          } else if constexpr (std::is_same_v<S, nir::Stmt::AssignVar>) {
            ValueId v = lower_expr(*st.value);
            env[st.var] = v;
            for (auto& ins : bb(cur).instrs) {
              if (ins.result == v && ins.debug_name.empty()) {
                ins.debug_name = st.name;
                break;
              }
            }
          } else if constexpr (std::is_same_v<S, nir::Stmt::AssignField>) {
            ValueId base = emit_undef(s.span);
            auto it = env.find(st.base_var);
            if (it != env.end()) base = it->second;
            ValueId value = lower_expr(*st.value);
            const auto path = split_field_path(st.field);
            if (path.empty()) return;
            ValueId cur = base;
            for (std::size_t i = 0; i + 1 < path.size(); ++i) {
              Op load;
              load.node = Op::LoadField{cur, path[i]};
              cur = emit(std::move(load), frontend::Ty::Unknown(), s.span);
            }
            Op o;
            o.node = Op::StoreField{cur, path.back(), value};
            (void)emit(std::move(o), frontend::Ty::Void(), s.span);
          } else if constexpr (std::is_same_v<S, nir::Stmt::ExprStmt>) {
            (void)lower_expr(*st.expr);
          } else if constexpr (std::is_same_v<S, nir::Stmt::Return>) {
            Terminator t;
            t.span = s.span;
            if (st.value && st.value->ty.kind != frontend::Ty::Kind::Void) {
              ValueId v = lower_expr(*st.value);
              t.node = Terminator::Ret{v};
            } else {
              t.node = Terminator::Ret{std::nullopt};
            }
            set_term(cur, std::move(t));
          } else if constexpr (std::is_same_v<S, nir::Stmt::Region>) {
            region_stack.push_back(st.name);
            // Update current block metadata for any newly created blocks.
            lower_block(st.body);
            region_stack.pop_back();
          } else if constexpr (std::is_same_v<S, nir::Stmt::Unsafe>) {
            // Keep unsafe subtree in analysis IR; no runtime control-flow effect.
            lower_block(st.body);
          } else if constexpr (std::is_same_v<S, nir::Stmt::If>) {
            const ValueId cond = lower_expr(*st.cond);
            const BlockId then_bb = new_block();
            const BlockId else_bb = st.else_body.has_value() ? new_block() : 0;
            const BlockId after_bb = new_block();

            Terminator t;
            t.span = s.span;
            t.node = Terminator::Branch{cond, then_bb, st.else_body.has_value() ? else_bb : after_bb};
            set_term(cur, std::move(t));

            cur = then_bb;
            bb(then_bb).region_stack = region_stack;
            bb(then_bb).annotation_stack = ann_stack;
            lower_block(st.then_body);
            if (!terminated(cur)) {
              Terminator j;
              j.span = s.span;
              j.node = Terminator::Jump{after_bb};
              set_term(cur, std::move(j));
            }

            if (st.else_body.has_value()) {
              cur = else_bb;
              bb(else_bb).region_stack = region_stack;
              bb(else_bb).annotation_stack = ann_stack;
              lower_block(*st.else_body);
              if (!terminated(cur)) {
                Terminator j;
                j.span = s.span;
                j.node = Terminator::Jump{after_bb};
                set_term(cur, std::move(j));
              }
            }

            cur = after_bb;
            bb(after_bb).region_stack = region_stack;
            bb(after_bb).annotation_stack = ann_stack;
          } else if constexpr (std::is_same_v<S, nir::Stmt::For>) {
            // Lower `for i in a..b { body }` into:
            // preheader(cur) -> header(phi i; cmp i<end; br) -> body -> latch(i+1; jmp header)
            // header -> after
            const BlockId preheader = cur;
            const ValueId start_v = lower_expr(*st.start);
            const ValueId end_v = lower_expr(*st.end);

            const BlockId header = new_block();
            const BlockId body = new_block();
            const BlockId latch = new_block();
            const BlockId after = new_block();

            // preheader jumps to header
            {
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{header};
              set_term(preheader, std::move(t));
            }

            // header phi for loop var
            cur = header;
            bb(header).region_stack = region_stack;
            bb(header).annotation_stack = ann_stack;

            ValueId phi_i = next_value++;
            Phi phi;
            phi.result = phi_i;
            phi.ty = st.var_ty;
            phi.debug_name = st.var_name;
            phi.incomings.push_back({preheader, start_v});
            // latch incoming patched later
            phi.incomings.push_back({latch, 0});
            bb(header).phis.push_back(std::move(phi));

            env[st.var] = phi_i;

            // cond = (i < end)
            {
              Op o;
              o.node = Op::CmpLt{phi_i, end_v};
              (void)emit(std::move(o), frontend::Ty::Int(), s.span);
              const ValueId cond = bb(header).instrs.back().result;
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Branch{cond, body, after};
              set_term(header, std::move(t));
            }

            // body
            cur = body;
            bb(body).region_stack = region_stack;
            bb(body).annotation_stack = ann_stack;
            loop_stack.push_back(LoopTargets{after, latch});
            lower_block(st.body);
            loop_stack.pop_back();
            const bool body_terminated = terminated(cur);

            if (!body_terminated) {
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{latch};
              set_term(cur, std::move(t));
            }

            // latch (only reachable if body didn't return)
            cur = latch;
            bb(latch).region_stack = region_stack;
            bb(latch).annotation_stack = ann_stack;
            if (!body_terminated) {
              // next = i + 1
              Op c1;
              c1.node = Op::ConstInt{1};
              ValueId one = emit(std::move(c1), frontend::Ty::Int(), s.span);
              Op add;
              add.node = Op::Bin{BinOp::Add, phi_i, one};
              ValueId next_i = emit(std::move(add), frontend::Ty::Int(), s.span);

              // patch phi incoming for latch
              if (!bb(header).phis.empty()) {
                auto& p0 = bb(header).phis.back();
                for (auto& [pred, val] : p0.incomings) {
                  if (pred == latch) val = next_i;
                }
              }

              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{header};
              set_term(latch, std::move(t));
            } else {
              // Unreachable latch: just jump to header to keep structure, but no preds.
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{header};
              set_term(latch, std::move(t));
            }

            // continue after loop
            cur = after;
            bb(after).region_stack = region_stack;
            bb(after).annotation_stack = ann_stack;
          } else if constexpr (std::is_same_v<S, nir::Stmt::While>) {
            const BlockId preheader = cur;
            const BlockId header = new_block();
            const BlockId body = new_block();
            const BlockId after = new_block();

            {
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{header};
              set_term(preheader, std::move(t));
            }

            cur = header;
            bb(header).region_stack = region_stack;
            bb(header).annotation_stack = ann_stack;
            const ValueId cond = lower_expr(*st.cond);
            {
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Branch{cond, body, after};
              set_term(header, std::move(t));
            }

            cur = body;
            bb(body).region_stack = region_stack;
            bb(body).annotation_stack = ann_stack;
            loop_stack.push_back(LoopTargets{after, header});
            lower_block(st.body);
            loop_stack.pop_back();
            if (!terminated(cur)) {
              Terminator t;
              t.span = s.span;
              t.node = Terminator::Jump{header};
              set_term(cur, std::move(t));
            }

            cur = after;
            bb(after).region_stack = region_stack;
            bb(after).annotation_stack = ann_stack;
          } else if constexpr (std::is_same_v<S, nir::Stmt::Break>) {
            Terminator t;
            t.span = s.span;
            t.node = Terminator::Jump{loop_stack.back().break_target};
            set_term(cur, std::move(t));
          } else if constexpr (std::is_same_v<S, nir::Stmt::Continue>) {
            Terminator t;
            t.span = s.span;
            t.node = Terminator::Jump{loop_stack.back().continue_target};
            set_term(cur, std::move(t));
          }
        },
        s.node);

    // restore annotations
    ann_stack.resize(ann_sz);
  }
};

static std::string ty_short(const Ty& t) {
  return frontend::ty_to_string(t);
}

static std::string op_name(const Op& op) {
  return std::visit(
      [&](auto&& n) -> std::string {
        using N = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<N, Op::Param>) return "param";
        if constexpr (std::is_same_v<N, Op::ConstInt>) return "const_i64";
        if constexpr (std::is_same_v<N, Op::ConstBool>) return "const_bool";
        if constexpr (std::is_same_v<N, Op::ConstFloat>) return "const_f64";
        if constexpr (std::is_same_v<N, Op::ConstString>) return "const_str";
        if constexpr (std::is_same_v<N, Op::Bin>) return "binop";
        if constexpr (std::is_same_v<N, Op::Unary>) return "unary";
        if constexpr (std::is_same_v<N, Op::CmpLt>) return "cmp_lt";
        if constexpr (std::is_same_v<N, Op::Call>) return "call";
        if constexpr (std::is_same_v<N, Op::LoadField>) return "load_field";
        if constexpr (std::is_same_v<N, Op::EnumIsVariant>) return "enum_is_variant";
        if constexpr (std::is_same_v<N, Op::EnumPayload>) return "enum_payload";
        if constexpr (std::is_same_v<N, Op::StoreField>) return "store_field";
        if constexpr (std::is_same_v<N, Op::Construct>) return "construct";
        if constexpr (std::is_same_v<N, Op::AllocHint>) return "alloc_hint";
        if constexpr (std::is_same_v<N, Op::Undef>) return "undef";
        return "op";
      },
      op.node);
}

static std::string binop_str(BinOp op) {
  switch (op) {
  case BinOp::Add: return "+";
  case BinOp::Sub: return "-";
  case BinOp::Mul: return "*";
  case BinOp::Div: return "/";
  case BinOp::Mod: return "%";
  case BinOp::Eq: return "==";
  case BinOp::Ne: return "!=";
  case BinOp::Lt: return "<";
  case BinOp::Lte: return "<=";
  case BinOp::Gt: return ">";
  case BinOp::Gte: return ">=";
  case BinOp::And: return "&&";
  case BinOp::Or: return "||";
  }
  return "+";
}

} // namespace

FunctionCFG lower_to_cfg_ir(const nir::Function& fn) {
  Builder b;
  b.out.name = fn.name;

  // entry block
  b.out.entry = b.new_block();
  b.cur = b.out.entry;
  b.ann_stack = fn.annotations;

  // Params become SSA values.
  for (const auto& p : fn.params) {
    Op o;
    o.node = Op::Param{p.var, p.name};
    ValueId v = b.emit(std::move(o), p.ty, p.span, p.name);
    b.env[p.var] = v;
  }

  if (fn.body.has_value()) b.lower_block(*fn.body);

  // If function falls off end, emit `ret void` for void functions.
  if (!b.terminated(b.cur)) {
    Terminator t;
    t.span = fn.span;
    t.node = Terminator::Ret{std::nullopt};
    b.set_term(b.cur, std::move(t));
  }

  return std::move(b.out);
}

std::string dump_cfg_ir(const FunctionCFG& f) {
  std::ostringstream os;
  os << "cfg_ir: fn " << f.name << " entry=bb" << f.entry << "\n";
  for (const auto& bb : f.blocks) {
    os << "  bb" << bb.id << " preds=[";
    for (std::size_t i = 0; i < bb.preds.size(); ++i) {
      if (i) os << ",";
      os << "bb" << bb.preds[i];
    }
    os << "] succs=[";
    for (std::size_t i = 0; i < bb.succs.size(); ++i) {
      if (i) os << ",";
      os << "bb" << bb.succs[i];
    }
    os << "]";
    if (!bb.region_stack.empty()) {
      os << " regions=[";
      for (std::size_t i = 0; i < bb.region_stack.size(); ++i) {
        if (i) os << "/";
        os << bb.region_stack[i];
      }
      os << "]";
    }
    if (!bb.annotation_stack.empty()) {
      os << " ann=[";
      for (std::size_t i = 0; i < bb.annotation_stack.size(); ++i) {
        if (i) os << ",";
        os << "@" << bb.annotation_stack[i];
      }
      os << "]";
    }
    os << "\n";

    for (const auto& p : bb.phis) {
      os << "    %" << p.result;
      if (!p.debug_name.empty()) os << "(" << p.debug_name << ")";
      os << " : " << ty_short(p.ty) << " = phi ";
      for (std::size_t i = 0; i < p.incomings.size(); ++i) {
        const auto& [pred, val] = p.incomings[i];
        if (i) os << ", ";
        os << "[bb" << pred << " -> %" << val << "]";
      }
      os << "\n";
    }

    for (const auto& ins : bb.instrs) {
      os << "    %" << ins.result;
      if (!ins.debug_name.empty()) os << "(" << ins.debug_name << ")";
      os << " : " << ty_short(ins.ty) << " = " << op_name(ins.op) << " ";

      std::visit(
          [&](auto&& n) {
            using N = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<N, Op::Param>) {
              os << "v" << n.var << " " << n.name;
            } else if constexpr (std::is_same_v<N, Op::ConstInt>) {
              os << n.value;
            } else if constexpr (std::is_same_v<N, Op::ConstBool>) {
              os << (n.value ? "true" : "false");
            } else if constexpr (std::is_same_v<N, Op::ConstFloat>) {
              os << n.value;
            } else if constexpr (std::is_same_v<N, Op::ConstString>) {
              os << "\"...\"";
            } else if constexpr (std::is_same_v<N, Op::Bin>) {
              os << "%" << n.lhs << " " << binop_str(n.op) << " %" << n.rhs;
            } else if constexpr (std::is_same_v<N, Op::Unary>) {
              os << "!";
              os << "%" << n.value;
            } else if constexpr (std::is_same_v<N, Op::CmpLt>) {
              os << "%" << n.lhs << " < %" << n.rhs;
            } else if constexpr (std::is_same_v<N, Op::Call>) {
              os << n.callee << "(";
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                if (i) os << ", ";
                os << "%" << n.args[i];
              }
              os << ")";
            } else if constexpr (std::is_same_v<N, Op::LoadField>) {
              os << "%" << n.base << "." << n.field;
            } else if constexpr (std::is_same_v<N, Op::EnumIsVariant>) {
              os << "%" << n.subject << " is " << n.variant_name << "#" << n.variant_index;
            } else if constexpr (std::is_same_v<N, Op::EnumPayload>) {
              os << "%" << n.subject << " payload " << n.variant_name << "#" << n.variant_index;
            } else if constexpr (std::is_same_v<N, Op::StoreField>) {
              os << "%" << n.base << "." << n.field << " = %" << n.value;
            } else if constexpr (std::is_same_v<N, Op::Construct>) {
              os << n.type_name << "(";
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                if (i) os << ", ";
                os << "%" << n.args[i];
              }
              os << ")";
            } else if constexpr (std::is_same_v<N, Op::AllocHint>) {
              os << "hint ";
              switch (n.kind) {
              case PrefixKind::Shared: os << "shared "; break;
              case PrefixKind::Unique: os << "unique "; break;
              case PrefixKind::Heap: os << "heap "; break;
              case PrefixKind::Promote: os << "promote "; break;
              }
              os << n.type_name << "(";
              for (std::size_t i = 0; i < n.args.size(); ++i) {
                if (i) os << ", ";
                os << "%" << n.args[i];
              }
              os << ")";
            } else if constexpr (std::is_same_v<N, Op::Undef>) {
              os << "";
            }
          },
          ins.op.node);

      os << "\n";
    }

    if (bb.term.has_value()) {
      os << "    term: ";
      std::visit(
          [&](auto&& t) {
            using T = std::decay_t<decltype(t)>;
            if constexpr (std::is_same_v<T, Terminator::Jump>) {
              os << "jmp bb" << t.target;
            } else if constexpr (std::is_same_v<T, Terminator::Branch>) {
              os << "br %" << t.cond << " ? bb" << t.then_bb << " : bb" << t.else_bb;
            } else if constexpr (std::is_same_v<T, Terminator::Ret>) {
              os << "ret";
              if (t.value.has_value()) os << " %" << *t.value;
            }
          },
          bb.term->node);
      os << "\n";
    }
  }
  return os.str();
}

} // namespace nebula::nir::cfgir
