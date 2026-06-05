#include "tiger/translate/translate.h"

#include <tiger/absyn/absyn.h>

#include "tiger/env/env.h"
#include "tiger/errormsg/errormsg.h"
#include "tiger/frame/x64frame.h"
#include "tiger/frame/temp.h"
#include "tiger/frame/frame.h"

extern frame::Frags *frags;
extern frame::RegManager *reg_manager;

// 循环标签栈：用于 break 语句跳转到正确的循环结束位置
static std::vector<temp::Label *> loop_stack;

namespace {

class Cx {
public:
  tr::PatchList trues_;
  tr::PatchList falses_;
  tree::Stm *stm_;

  Cx() : trues_(tr::PatchList()), falses_(tr::PatchList()), stm_(nullptr) {}
  Cx(tr::PatchList trues, tr::PatchList falses, tree::Stm *stm)
      : trues_(trues), falses_(falses), stm_(stm) {}
};

frame::ProcFrag *ProcEntryExit(tr::Level *level, tr::Exp *body);
}

namespace tr {

Access *Access::AllocLocal(Level *level, bool escape) {
  frame::Access *access = level->frame_->AllocLocal(escape);
  return new Access(level, access);
}

class Exp {
public:
  [[nodiscard]] virtual tree::Exp *UnEx() const = 0;
  [[nodiscard]] virtual tree::Stm *UnNx() const = 0;
  [[nodiscard]] virtual Cx UnCx(err::ErrorMsg *errormsg) const = 0;
};

class ExpAndTy {
public:
  tr::Exp *exp_;
  type::Ty *ty_;

  ExpAndTy(tr::Exp *exp, type::Ty *ty) : exp_(exp), ty_(ty) {}
};

class ExExp : public Exp {
public:
  tree::Exp *exp_;

  explicit ExExp(tree::Exp *exp) : exp_(exp) {}

  [[nodiscard]] tree::Exp *UnEx() const override { 
    return exp_;
  }
  [[nodiscard]] tree::Stm *UnNx() const override {
    return new tree::ExpStm(exp_);
  }
  [[nodiscard]] Cx UnCx(err::ErrorMsg *errormsg) const override {
    temp::Label *true_label = temp::LabelFactory::NewLabel();
    temp::Label *false_label = temp::LabelFactory::NewLabel();
    tree::Stm *stm = new tree::CjumpStm(tree::NE_OP, exp_, new tree::ConstExp(0),
                                        true_label, false_label);
    tr::PatchList trues({&true_label});
    tr::PatchList falses({&false_label});
    return Cx(trues, falses, stm);
  }
};

class NxExp : public Exp {
public:
  tree::Stm *stm_;

  explicit NxExp(tree::Stm *stm) : stm_(stm) {}

  [[nodiscard]] tree::Exp *UnEx() const override {
    return new tree::EseqExp(stm_, new tree::ConstExp(0));
  }
  [[nodiscard]] tree::Stm *UnNx() const override { 
    return stm_;
  }
  [[nodiscard]] Cx UnCx(err::ErrorMsg *errormsg) const override {
    errormsg->Error(0, "cannot convert NxExp to Cx");
    return Cx();
  }
};

class CxExp : public Exp {
public:
  Cx cx_;

  CxExp(PatchList trues, PatchList falses, tree::Stm *stm)
      : cx_(trues, falses, stm) {}
  
  [[nodiscard]] tree::Exp *UnEx() const override {
    temp::Label *t = temp::LabelFactory::NewLabel();
    temp::Label *f = temp::LabelFactory::NewLabel();
    temp::Temp *r = temp::TempFactory::NewTemp();
    cx_.trues_.DoPatch(t);
    cx_.falses_.DoPatch(f);
    tree::Stm *stm = new tree::SeqStm(
      new tree::SeqStm(new tree::LabelStm(t),
                       new tree::MoveStm(new tree::TempExp(r), new tree::ConstExp(1))),
      new tree::SeqStm(new tree::LabelStm(f),
                       new tree::MoveStm(new tree::TempExp(r), new tree::ConstExp(0))));
    return new tree::EseqExp(stm, new tree::TempExp(r));
  }
  [[nodiscard]] tree::Stm *UnNx() const override {
    return cx_.stm_;
  }
  [[nodiscard]] Cx UnCx(err::ErrorMsg *errormsg) const override { 
    return cx_;
  }
};

void ProgTr::Translate() {
  FillBaseTEnv();
  FillBaseVEnv();
  absyn_tree_->Translate(venv_.get(), tenv_.get(), main_level_.get(),
                         temp::LabelFactory::NamedLabel("tigermain"),
                         errormsg_.get());
}

} // namespace tr

namespace {

frame::ProcFrag *ProcEntryExit(tr::Level *level, tr::Exp *body) {
  tree::Stm *body_stm = body->UnNx();
  tree::Stm *stm = frame::ProcEntryExit1(level->frame_, body_stm);
  return new frame::ProcFrag(stm, level->frame_);
}

} // namespace

namespace absyn {

tr::ExpAndTy *AbsynTree::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   tr::Level *level, temp::Label *label,
                                   err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *result = root_->Translate(venv, tenv, level, label, errormsg);
  frame::ProcFrag *frag = ProcEntryExit(level, result->exp_);
  frags->PushBack(frag);
  return result;
}

tr::ExpAndTy *SimpleVar::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   tr::Level *level, temp::Label *label,
                                   err::ErrorMsg *errormsg) const {
  env::EnvEntry *entry = venv->Look(sym_);
  if (!entry) {
    errormsg->Error(pos_, "undefined variable " + sym_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::IntTy::Instance());
  }
  env::VarEntry *var_entry = dynamic_cast<env::VarEntry *>(entry);
  if (!var_entry) {
    errormsg->Error(pos_, "undefined variable " + sym_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::IntTy::Instance());
  }
  tr::Access *access = var_entry->access_;
  
  // 静态链追溯：从当前 level 追溯到变量定义的 level
  tree::Exp *curr_fp = new tree::TempExp(reg_manager->FramePointer());
  tr::Level *var_level = access->level_;
  while (level != var_level) {
    if (level->parent_ == nullptr) break;
    // 自定义函数（有 parent_）的第一个参数必然是静态链，不需要检查 empty()
    curr_fp = level->Formals()->front()->access_->ToExp(curr_fp);
    level = level->parent_;
  }
  
  return new tr::ExpAndTy(new tr::ExExp(access->access_->ToExp(curr_fp)),
                          var_entry->ty_);
}

tr::ExpAndTy *FieldVar::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                  tr::Level *level, temp::Label *label,
                                  err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *exp_ty = var_->Translate(venv, tenv, level, label, errormsg);
  tr::Exp *exp = exp_ty->exp_;
  type::Ty *ty = exp_ty->ty_->ActualTy();

  if (typeid(*ty) != typeid(type::RecordTy)) {
    errormsg->Error(pos_, "not a record type");
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }

  if (typeid(*exp) != typeid(tr::ExExp)) {
    errormsg->Error(pos_, "field var's exp must be an expression");
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }
  auto record_ty = static_cast<type::RecordTy *>(ty);
  type::FieldList *field_list = record_ty->fields_;
  int order = 0;
  for (auto field : field_list->GetList()) {
    if (field->name_ == sym_) {
      tree::Exp *texp = new tree::MemExp(new tree::BinopExp(
          tree::PLUS_OP, exp->UnEx(),
          new tree::ConstExp(order * reg_manager->WordSize())));
      return new tr::ExpAndTy(new tr::ExExp(texp), field->ty_->ActualTy());
    }
    order++;
  }
  errormsg->Error(pos_, "field %s doesn't exist", sym_->Name().data());
  return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                          type::IntTy::Instance());
}

tr::ExpAndTy *SubscriptVar::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                      tr::Level *level, temp::Label *label,
                                      err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *var_exp_ty = var_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *sub_exp_ty = subscript_->Translate(venv, tenv, level, label, errormsg);
  
  tree::Exp *var_exp = var_exp_ty->exp_->UnEx();
  tree::Exp *sub_exp = sub_exp_ty->exp_->UnEx();
  
  tree::Exp *elem_exp = new tree::MemExp(new tree::BinopExp(
      tree::PLUS_OP, var_exp,
      new tree::BinopExp(tree::MUL_OP, sub_exp, new tree::ConstExp(reg_manager->WordSize()))));
  
  // 使用数组元素的真实类型，而不是硬编码为 IntTy
  type::Ty *ty = var_exp_ty->ty_->ActualTy();
  type::ArrayTy *array_ty = dynamic_cast<type::ArrayTy *>(ty);
  if (!array_ty) {
    errormsg->Error(pos_, "subscript on non-array type");
    return new tr::ExpAndTy(new tr::ExExp(elem_exp), type::IntTy::Instance());
  }
  
  return new tr::ExpAndTy(new tr::ExExp(elem_exp), array_ty->ty_->ActualTy());
}

tr::ExpAndTy *VarExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  return var_->Translate(venv, tenv, level, label, errormsg);
}

tr::ExpAndTy *NilExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                          type::NilTy::Instance());
}

tr::ExpAndTy *IntExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(val_)),
                          type::IntTy::Instance());
}

tr::ExpAndTy *StringExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   tr::Level *level, temp::Label *label,
                                   err::ErrorMsg *errormsg) const {
  temp::Label *str_label = temp::LabelFactory::NewLabel();
  
  // str_ 已经是 std::string 类型，直接使用即可
  frags->PushBack(new frame::StringFrag(str_label, str_));
  return new tr::ExpAndTy(new tr::ExExp(new tree::NameExp(str_label)),
                          type::StringTy::Instance());
}

tr::ExpAndTy *CallExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                 tr::Level *level, temp::Label *label,
                                 err::ErrorMsg *errormsg) const {
  env::EnvEntry *entry = venv->Look(func_);
  if (!entry) {
    errormsg->Error(pos_, "undefined function " + func_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::IntTy::Instance());
  }
  env::FunEntry *fun_entry = dynamic_cast<env::FunEntry *>(entry);
  if (!fun_entry) {
    errormsg->Error(pos_, "undefined function " + func_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::IntTy::Instance());
  }
  
  tree::ExpList *args = new tree::ExpList();
  for (Exp *arg : args_->GetList()) {
    tr::ExpAndTy *arg_exp_ty = arg->Translate(venv, tenv, level, label, errormsg);
    args->Append(arg_exp_ty->exp_->UnEx());
  }
  
  // 情况一：如果是内置标准库函数（没有定义 level_）
  if (fun_entry->level_ == nullptr) {
    tree::Exp *call_exp = frame::ExternalCall(func_->Name(), args);
    return new tr::ExpAndTy(new tr::ExExp(call_exp), fun_entry->result_);
  }

  // 情况二：如果是用户自定义函数（fun_entry->level_ != nullptr）
  // 必须严格传入自定义函数的 label_，并正确传递静态链作为第一个参数
  if (fun_entry->level_->parent_) {
    tr::Level *parent_level = fun_entry->level_->parent_;
    tree::Exp *static_link = new tree::TempExp(reg_manager->FramePointer());
    tr::Level *curr_level = level;
    
    // 沿着静态链向上追溯，直到当前 level 匹配到被调函数的父 level
    // 注意：即使 curr_level == parent_level（递归或同级调用），也需要继续追溯
    // 因为被调函数需要的是其 parent_level 的 FP，而不是当前 level 的 FP
    while (curr_level != parent_level && curr_level != nullptr) {
      static_link = curr_level->Formals()->front()->access_->ToExp(static_link);
      curr_level = curr_level->parent_;
    }
    
    // 将计算出的静态链指针作为第一个参数插入到参数列表的最前面
    args->Insert(static_link);
  }
  
  // 生成标准的内部函数调用节点
  tree::Exp *call_exp = new tree::CallExp(new tree::NameExp(fun_entry->label_), args);
  return new tr::ExpAndTy(new tr::ExExp(call_exp), fun_entry->result_);
}

tr::ExpAndTy *OpExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                               tr::Level *level, temp::Label *label,
                               err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *left_exp_ty = left_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *right_exp_ty = right_->Translate(venv, tenv, level, label, errormsg);
  
  tree::Exp *left_exp = left_exp_ty->exp_->UnEx();
  tree::Exp *right_exp = right_exp_ty->exp_->UnEx();
  
  // 处理关系运算符（返回 Cx 表达式）
  if (oper_ == EQ_OP || oper_ == NEQ_OP || oper_ == LT_OP || 
      oper_ == LE_OP || oper_ == GT_OP || oper_ == GE_OP) {
    tree::RelOp relop;
    switch (oper_) {
      case EQ_OP: relop = tree::EQ_OP; break;
      case NEQ_OP: relop = tree::NE_OP; break;
      case LT_OP: relop = tree::LT_OP; break;
      case LE_OP: relop = tree::LE_OP; break;
      case GT_OP: relop = tree::GT_OP; break;
      case GE_OP: relop = tree::GE_OP; break;
      default: relop = tree::EQ_OP; break;
    }
    
    // 创建 Cx 表达式（条件跳转）
    temp::Label *t = temp::LabelFactory::NewLabel();
    temp::Label *f = temp::LabelFactory::NewLabel();
    tree::Stm *cjump = new tree::CjumpStm(relop, left_exp, right_exp, t, f);
    tr::PatchList trues({&t});
    tr::PatchList falses({&f});
    return new tr::ExpAndTy(new tr::CxExp(trues, falses, cjump), type::IntTy::Instance());
  }
  
  // 处理逻辑运算符 AND/OR（短路求值）
  if (oper_ == AND_OP || oper_ == OR_OP) {
    // A & B 相当于 if A then B else 0
    // A | B 相当于 if A then 1 else B
    temp::Label *true_label = temp::LabelFactory::NewLabel();
    temp::Label *false_label = temp::LabelFactory::NewLabel();
    temp::Label *end_label = temp::LabelFactory::NewLabel();
    temp::Temp *result_temp = temp::TempFactory::NewTemp();
    
    Cx left_cx = left_exp_ty->exp_->UnCx(errormsg);
    
    tree::Stm *result_stm = nullptr;
    if (oper_ == AND_OP) {
      // A & B: if A then B else 0
      left_cx.trues_.DoPatch(true_label);
      left_cx.falses_.DoPatch(false_label);
      
      tr::ExpAndTy *right_exp_ty = right_->Translate(venv, tenv, level, label, errormsg);
      tree::Stm *right_stm = new tree::MoveStm(new tree::TempExp(result_temp), right_exp_ty->exp_->UnEx());
      
      tree::Stm *true_stm = new tree::SeqStm(new tree::LabelStm(true_label), right_stm);
      tree::Stm *false_stm = new tree::SeqStm(new tree::LabelStm(false_label),
                                               new tree::MoveStm(new tree::TempExp(result_temp), new tree::ConstExp(0)));
      tree::Stm *end_stm = new tree::LabelStm(end_label);
      
      result_stm = new tree::SeqStm(left_cx.stm_,
                     new tree::SeqStm(true_stm,
                     new tree::SeqStm(new tree::JumpStm(new tree::NameExp(end_label), 
                                                        new std::vector<temp::Label *>{end_label}),
                     new tree::SeqStm(false_stm, end_stm))));
    } else {
      // A | B: if A then 1 else B
      left_cx.trues_.DoPatch(true_label);
      left_cx.falses_.DoPatch(false_label);
      
      tree::Stm *true_stm = new tree::SeqStm(new tree::LabelStm(true_label),
                                             new tree::MoveStm(new tree::TempExp(result_temp), new tree::ConstExp(1)));
      
      tr::ExpAndTy *right_exp_ty = right_->Translate(venv, tenv, level, label, errormsg);
      tree::Stm *right_stm = new tree::MoveStm(new tree::TempExp(result_temp), right_exp_ty->exp_->UnEx());
      tree::Stm *false_stm = new tree::SeqStm(new tree::LabelStm(false_label), right_stm);
      tree::Stm *end_stm = new tree::LabelStm(end_label);
      
      result_stm = new tree::SeqStm(left_cx.stm_,
                     new tree::SeqStm(true_stm,
                     new tree::SeqStm(new tree::JumpStm(new tree::NameExp(end_label),
                                                        new std::vector<temp::Label *>{end_label}),
                     new tree::SeqStm(false_stm, end_stm))));
    }
    
    return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(result_stm, new tree::TempExp(result_temp))),
                            type::IntTy::Instance());
  }
  
  // 处理算术运算符
  tree::BinOp binop;
  switch (oper_) {
    case PLUS_OP: binop = tree::PLUS_OP; break;
    case MINUS_OP: binop = tree::MINUS_OP; break;
    case TIMES_OP: binop = tree::MUL_OP; break;
    case DIVIDE_OP: binop = tree::DIV_OP; break;
    default: binop = tree::PLUS_OP; break;
  }
  
  tree::Exp *result = new tree::BinopExp(binop, left_exp, right_exp);
  return new tr::ExpAndTy(new tr::ExExp(result), type::IntTy::Instance());
}

tr::ExpAndTy *RecordExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   tr::Level *level, temp::Label *label,      
                                   err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(typ_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + typ_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }
  
  type::RecordTy *record_ty = dynamic_cast<type::RecordTy *>(ty->ActualTy());
  if (!record_ty) {
    errormsg->Error(pos_, "not a record type");
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }
  
  int field_count = record_ty->fields_->GetList().size();
  temp::Temp *record_temp = temp::TempFactory::NewTemp();
  
  tree::ExpList *args = new tree::ExpList();
  args->Append(new tree::ConstExp(field_count * reg_manager->WordSize()));
  tree::Stm *alloc_stm = new tree::MoveStm(
      new tree::TempExp(record_temp),
      new tree::CallExp(new tree::NameExp(temp::LabelFactory::NamedLabel("allocRecord")),
                        args));
  
  tree::Stm *init_stm = nullptr;
  int order = 0;
  for (EField *efield : fields_->GetList()) {
    tr::ExpAndTy *field_exp_ty = efield->exp_->Translate(venv, tenv, level, label, errormsg);
    tree::Exp *field_exp = field_exp_ty->exp_->UnEx();
    
    tree::Stm *move_stm = new tree::MoveStm(
        new tree::MemExp(new tree::BinopExp(tree::PLUS_OP,
                                            new tree::TempExp(record_temp),
                                            new tree::ConstExp(order * reg_manager->WordSize()))),
        field_exp);
    
    if (!init_stm) {
      init_stm = move_stm;
    } else {
      init_stm = new tree::SeqStm(init_stm, move_stm);
    }
    order++;
  }
  
  tree::Stm *all_stm = new tree::SeqStm(alloc_stm, init_stm);
  return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(all_stm, new tree::TempExp(record_temp))),
                          ty);
}

tr::ExpAndTy *SeqExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  std::list<Exp *> exp_list = seq_->GetList();
  if (exp_list.empty()) {
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }

  tree::Stm *seq_stm = nullptr;
  auto it = exp_list.begin();
  
  // 仅遍历到倒数第二个元素，作为有副作用的控制流语句
  for (size_t i = 0; i < exp_list.size() - 1; ++i) {
    tr::ExpAndTy *exp_ty = (*it)->Translate(venv, tenv, level, label, errormsg);
    tree::Stm *exp_stm = exp_ty->exp_->UnNx();
    if (!seq_stm) {
      seq_stm = exp_stm;
    } else {
      seq_stm = new tree::SeqStm(seq_stm, exp_stm);
    }
    it++;
  }

  // 最后一个元素：只在这里翻译并充当返回值，避免双重求值
  tr::ExpAndTy *last_exp_ty = (*it)->Translate(venv, tenv, level, label, errormsg);
  if (seq_stm) {
    return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(seq_stm, last_exp_ty->exp_->UnEx())),
                            last_exp_ty->ty_);
  } else {
    return last_exp_ty;
  }
}

tr::ExpAndTy *AssignExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   tr::Level *level, temp::Label *label,                       
                                   err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *var_exp_ty = var_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *exp_exp_ty = exp_->Translate(venv, tenv, level, label, errormsg);
  
  tree::Exp *var_exp = var_exp_ty->exp_->UnEx();
  tree::Exp *exp_exp = exp_exp_ty->exp_->UnEx();
  
  tree::Stm *assign_stm = new tree::MoveStm(var_exp, exp_exp);
  return new tr::ExpAndTy(new tr::NxExp(assign_stm), type::VoidTy::Instance());
}

tr::ExpAndTy *IfExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                               tr::Level *level, temp::Label *label,
                               err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *test_exp_ty = test_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *then_exp_ty = then_->Translate(venv, tenv, level, label, errormsg);
  
  temp::Label *true_label = temp::LabelFactory::NewLabel();
  temp::Label *false_label = temp::LabelFactory::NewLabel();
  temp::Label *end_label = temp::LabelFactory::NewLabel();
  
  // 使用 Cx 处理条件表达式
  Cx test_cx = test_exp_ty->exp_->UnCx(errormsg);
  test_cx.trues_.DoPatch(true_label);
  test_cx.falses_.DoPatch(false_label);
  
  tree::Stm *then_stm = then_exp_ty->exp_->UnNx();
  tree::Stm *then_label_stm = new tree::LabelStm(true_label);
  
  tree::Stm *result_stm = nullptr;
  type::Ty *result_ty = then_exp_ty->ty_;
  
  // 判断是否需要处理返回值
  bool has_return_value = (typeid(*then_exp_ty->ty_) != typeid(type::VoidTy));
  
  if (elsee_) {
    tr::ExpAndTy *else_exp_ty = elsee_->Translate(venv, tenv, level, label, errormsg);
    tree::Stm *else_stm = else_exp_ty->exp_->UnNx();
    tree::Stm *else_label_stm = new tree::LabelStm(false_label);
    tree::Stm *end_label_stm = new tree::LabelStm(end_label);
    
    if (has_return_value) {
      // 有返回值：需要分配临时寄存器存储结果
      temp::Temp *r = temp::TempFactory::NewTemp();
      
      // 在 then 分支末尾添加赋值
      tree::Stm *then_move = new tree::MoveStm(new tree::TempExp(r), then_exp_ty->exp_->UnEx());
      tree::Stm *then_with_move = new tree::SeqStm(then_stm, then_move);
      
      // 在 else 分支末尾添加赋值
      tree::Stm *else_move = new tree::MoveStm(new tree::TempExp(r), else_exp_ty->exp_->UnEx());
      tree::Stm *else_with_move = new tree::SeqStm(else_stm, else_move);
      
      auto *jump_labels = new std::vector<temp::Label *>();
      jump_labels->push_back(end_label);
      tree::Stm *jump_stm = new tree::JumpStm(new tree::NameExp(end_label), jump_labels);
      
      result_stm = new tree::SeqStm(test_cx.stm_,
                     new tree::SeqStm(then_label_stm,
                     new tree::SeqStm(then_with_move,
                     new tree::SeqStm(jump_stm,
                     new tree::SeqStm(else_label_stm,
                     new tree::SeqStm(else_with_move, end_label_stm))))));
      result_ty = else_exp_ty->ty_;
      
      return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(result_stm, new tree::TempExp(r))),
                              result_ty);
    } else {
      // 无返回值（VoidTy）
      auto *jump_labels = new std::vector<temp::Label *>();
      jump_labels->push_back(end_label);
      tree::Stm *jump_stm = new tree::JumpStm(new tree::NameExp(end_label), jump_labels);
      
      result_stm = new tree::SeqStm(test_cx.stm_,
                     new tree::SeqStm(then_label_stm,
                     new tree::SeqStm(then_stm,
                     new tree::SeqStm(jump_stm,
                     new tree::SeqStm(else_label_stm,
                     new tree::SeqStm(else_stm, end_label_stm))))));
      result_ty = else_exp_ty->ty_;
    }
  } else {
    tree::Stm *else_label_stm = new tree::LabelStm(false_label);
    
    if (has_return_value) {
      // 没有 else 分支但有返回值，这是语义错误，但我们在 IR 层处理
      // 在 false 分支赋值 0 作为默认值
      temp::Temp *r = temp::TempFactory::NewTemp();
      tree::Stm *then_move = new tree::MoveStm(new tree::TempExp(r), then_exp_ty->exp_->UnEx());
      tree::Stm *then_with_move = new tree::SeqStm(then_stm, then_move);
      
      // 在 false 分支也赋值 0，并确保有正确的跳转
      tree::Stm *else_move = new tree::MoveStm(new tree::TempExp(r), new tree::ConstExp(0));
      
      auto *jump_labels = new std::vector<temp::Label *>();
      jump_labels->push_back(end_label);
      tree::Stm *jump_stm = new tree::JumpStm(new tree::NameExp(end_label), jump_labels);
      
      // then 分支执行后跳转到 end_label
      tree::Stm *then_with_jump = new tree::SeqStm(then_with_move, jump_stm);
      
      // false 分支：标签 + 赋值
      tree::Stm *else_block = new tree::SeqStm(else_label_stm, else_move);
      
      result_stm = new tree::SeqStm(test_cx.stm_,
                     new tree::SeqStm(then_label_stm,
                     new tree::SeqStm(then_with_jump, else_block)));
      
      // EseqExp 需要 (Stm*, Exp*)，使用 tree::TempExp(r) 作为返回值（r 已存储 then 分支或 0 的值）
      return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(result_stm, new tree::TempExp(r))),
                              result_ty);
    } else {
      // 无 else 分支且无返回值（纯语句）
      result_stm = new tree::SeqStm(test_cx.stm_,
                     new tree::SeqStm(then_label_stm,
                     new tree::SeqStm(then_stm, else_label_stm)));
    }
  }
  
  return new tr::ExpAndTy(new tr::NxExp(result_stm), result_ty);
}

tr::ExpAndTy *WhileExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                  tr::Level *level, temp::Label *label,            
                                  err::ErrorMsg *errormsg) const {
  temp::Label *test_label = temp::LabelFactory::NewLabel();
  temp::Label *body_label = temp::LabelFactory::NewLabel();
  temp::Label *done_label = temp::LabelFactory::NewLabel();
  
  tr::ExpAndTy *test_exp_ty = test_->Translate(venv, tenv, level, label, errormsg);
  
  // 使用 Cx 处理条件表达式，避免冗余的 NE_OP 比较
  Cx test_cx = test_exp_ty->exp_->UnCx(errormsg);
  test_cx.trues_.DoPatch(body_label);
  test_cx.falses_.DoPatch(done_label);
  
  // 将 done_label 压入循环栈
  loop_stack.push_back(done_label);
  
  tr::ExpAndTy *body_exp_ty = body_->Translate(venv, tenv, level, label, errormsg);
  tree::Stm *body_stm = body_exp_ty->exp_->UnNx();
  
  // 弹出循环栈
  loop_stack.pop_back();
  
  tree::Stm *test_label_stm = new tree::LabelStm(test_label);
  tree::Stm *body_label_stm = new tree::LabelStm(body_label);
  
  auto *jump_labels = new std::vector<temp::Label *>();
  jump_labels->push_back(test_label);
  tree::Stm *jump_back = new tree::JumpStm(new tree::NameExp(test_label), jump_labels);
  tree::Stm *done_label_stm = new tree::LabelStm(done_label);
  
  tree::Stm *while_stm = new tree::SeqStm(test_label_stm,
                            new tree::SeqStm(test_cx.stm_,
                            new tree::SeqStm(body_label_stm,
                            new tree::SeqStm(body_stm,
                            new tree::SeqStm(jump_back, done_label_stm)))));
  
  return new tr::ExpAndTy(new tr::NxExp(while_stm), type::VoidTy::Instance());
}

tr::ExpAndTy *ForExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *lo_exp_ty = lo_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *hi_exp_ty = hi_->Translate(venv, tenv, level, label, errormsg);
  
  // 为 for 循环变量分配访问（不逃逸），并注册到 venv
  tr::Access *i_access = tr::Access::AllocLocal(level, false);
  tree::Exp *i_exp = i_access->access_->ToExp(
      new tree::TempExp(reg_manager->FramePointer()));
  venv->Enter(var_, new env::VarEntry(i_access, type::IntTy::Instance(), true));
  
  temp::Label *test_label = temp::LabelFactory::NewLabel();
  temp::Label *body_label = temp::LabelFactory::NewLabel();
  temp::Label *done_label = temp::LabelFactory::NewLabel();
  
  tree::Stm *init_stm = new tree::MoveStm(i_exp, lo_exp_ty->exp_->UnEx());
  
  tree::Exp *hi_exp = hi_exp_ty->exp_->UnEx();
  tree::Stm *test_cjump = new tree::CjumpStm(tree::LE_OP, i_exp,
                                             hi_exp, body_label, done_label);
  
  loop_stack.push_back(done_label);
  
  tree::Stm *body_stm = body_->Translate(venv, tenv, level, label, errormsg)->exp_->UnNx();
  
  loop_stack.pop_back();
  
  tree::Stm *test_label_stm = new tree::LabelStm(test_label);
  tree::Stm *body_label_stm = new tree::LabelStm(body_label);
  tree::Stm *inc_stm = new tree::MoveStm(i_exp,
                                         new tree::BinopExp(tree::PLUS_OP,
                                                            i_exp,
                                                            new tree::ConstExp(1)));
  
  auto *jump_labels = new std::vector<temp::Label *>();
  jump_labels->push_back(test_label);
  tree::Stm *jump_back = new tree::JumpStm(new tree::NameExp(test_label), jump_labels);
  tree::Stm *done_label_stm = new tree::LabelStm(done_label);
  
  tree::Stm *for_stm = new tree::SeqStm(init_stm,
                           new tree::SeqStm(test_label_stm,
                           new tree::SeqStm(test_cjump,
                           new tree::SeqStm(body_label_stm,
                           new tree::SeqStm(body_stm,
                           new tree::SeqStm(inc_stm,
                           new tree::SeqStm(jump_back, done_label_stm)))))));
  
  return new tr::ExpAndTy(new tr::NxExp(for_stm), type::VoidTy::Instance());
}

tr::ExpAndTy *BreakExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                  tr::Level *level, temp::Label *label,
                                  err::ErrorMsg *errormsg) const {
  // 从循环栈中获取最近一层循环的 done_label
  if (loop_stack.empty()) {
    errormsg->Error(pos_, "break statement outside of loop");
    return new tr::ExpAndTy(new tr::NxExp(new tree::ExpStm(new tree::ConstExp(0))),
                            type::VoidTy::Instance());
  }
  temp::Label *done_label = loop_stack.back();
  
  auto *jump_labels = new std::vector<temp::Label *>();
  jump_labels->push_back(done_label);
  tree::Stm *break_stm = new tree::JumpStm(new tree::NameExp(done_label), jump_labels);
  return new tr::ExpAndTy(new tr::NxExp(break_stm), type::VoidTy::Instance());
}

tr::ExpAndTy *LetExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  venv->BeginScope();
  tenv->BeginScope();
  
  tree::Stm *dec_stm = nullptr;
  for (Dec *dec : decs_->GetList()) {
    tr::Exp *dec_exp = dec->Translate(venv, tenv, level, label, errormsg);
    tree::Stm *dec_translate = dec_exp->UnNx();
    if (!dec_stm) {
      dec_stm = dec_translate;
    } else {
      dec_stm = new tree::SeqStm(dec_stm, dec_translate);
    }
  }
  
  tr::ExpAndTy *body_exp_ty = body_->Translate(venv, tenv, level, label, errormsg);
  
  venv->EndScope();
  tenv->EndScope();
  
  // 只有存在声明时才需要 EseqExp 包装，且 dec_stm 后面直接跟 body 的 UnEx()
  if (dec_stm) {
    return new tr::ExpAndTy(new tr::ExExp(new tree::EseqExp(dec_stm, body_exp_ty->exp_->UnEx())),
                            body_exp_ty->ty_);
  } else {
    return body_exp_ty;
  }
}

tr::ExpAndTy *ArrayExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                  tr::Level *level, temp::Label *label,                    
                                  err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(typ_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + typ_->Name());
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }
  
  type::ArrayTy *array_ty = dynamic_cast<type::ArrayTy *>(ty->ActualTy());
  if (!array_ty) {
    errormsg->Error(pos_, "array type required");
    return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                            type::VoidTy::Instance());
  }
  
  tr::ExpAndTy *size_exp_ty = size_->Translate(venv, tenv, level, label, errormsg);
  tr::ExpAndTy *init_exp_ty = init_->Translate(venv, tenv, level, label, errormsg);
  
  tree::ExpList *args = new tree::ExpList();
  args->Append(size_exp_ty->exp_->UnEx());
  args->Append(init_exp_ty->exp_->UnEx());
  
  tree::Exp *array_exp = new tree::CallExp(
      new tree::NameExp(temp::LabelFactory::NamedLabel("initArray")), args);
  
  return new tr::ExpAndTy(new tr::ExExp(array_exp), ty);
}

tr::ExpAndTy *VoidExp::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                 tr::Level *level, temp::Label *label,
                                 err::ErrorMsg *errormsg) const {
  return new tr::ExpAndTy(new tr::ExExp(new tree::ConstExp(0)),
                          type::VoidTy::Instance());
}

tr::Exp *FunctionDec::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                                tr::Level *level, temp::Label *label,
                                err::ErrorMsg *errormsg) const {
  for (FunDec *fun_dec : functions_->GetList()) {
    type::TyList *formals = fun_dec->params_->MakeFormalTyList(tenv, errormsg);
    type::Ty *result = type::VoidTy::Instance();
    if (fun_dec->result_) {
      result = tenv->Look(fun_dec->result_);
      if (!result) {
        errormsg->Error(fun_dec->pos_, "undefined result type");
        result = type::VoidTy::Instance();
      }
    }
    
    // 使用逃逸分析的结果，而不是全部写死为 true
    std::list<bool> formal_escapes;
    for (Field *field : fun_dec->params_->GetList()) {
      formal_escapes.push_back(field->escape_);
    }
    
    tr::Level *fun_level = tr::Level::NewLevel(level, temp::LabelFactory::NamedLabel(fun_dec->name_->Name()),
                                                formal_escapes);
    
    temp::Label *fun_label = temp::LabelFactory::NamedLabel(fun_dec->name_->Name());
    venv->Enter(fun_dec->name_, new env::FunEntry(fun_level, fun_label, formals, result));
  }
  
  for (FunDec *fun_dec : functions_->GetList()) {
    env::EnvEntry *entry = venv->Look(fun_dec->name_);
    env::FunEntry *fun_entry = dynamic_cast<env::FunEntry *>(entry);
    tr::Level *fun_level = fun_entry->level_;
    
    venv->BeginScope();
    tenv->BeginScope();
    
    auto formal_acc_it = fun_level->Formals()->begin();
    // 跳过第一个形式参数（静态链），它是专门留给静态链的
    if (formal_acc_it != fun_level->Formals()->end()) {
      formal_acc_it++;
    }
    
    for (Field *field : fun_dec->params_->GetList()) {
      type::Ty *ty = tenv->Look(field->typ_);
      if (!ty) {
        errormsg->Error(field->pos_, "undefined type");
        ty = type::VoidTy::Instance();
      }
      tr::Access *access = *formal_acc_it++;
      venv->Enter(field->name_, new env::VarEntry(access, ty));
    }
    
    tr::ExpAndTy *body_exp_ty = fun_dec->body_->Translate(venv, tenv, fun_level, nullptr, errormsg);
    frame::ProcFrag *frag = ProcEntryExit(fun_level, body_exp_ty->exp_);
    frags->PushBack(frag);
    
    venv->EndScope();
    tenv->EndScope();
  }
  
  return new tr::NxExp(new tree::ExpStm(new tree::ConstExp(0)));
}

tr::Exp *VarDec::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                           tr::Level *level, temp::Label *label,
                           err::ErrorMsg *errormsg) const {
  tr::ExpAndTy *init_exp_ty =
      init_->Translate(venv, tenv, level, label, errormsg);
  type::Ty *init_ty = init_exp_ty->ty_;

  if (typ_) {
    type::Ty *ty = tenv->Look(typ_);
    if (!ty) {
      errormsg->Error(pos_, "undefined type %s", typ_->Name().data());
    }

    if (!ty->IsSameType(init_ty)) {
      errormsg->Error(pos_, "type and init type mismatch");
    }
  } else {
    auto actual_init_ty = init_ty->ActualTy();
    if (typeid(*actual_init_ty) == typeid(type::NilTy)) {
      errormsg->Error(pos_, "init should not be nil without type specified");
    }
  }

  tr::Access *access = tr::Access::AllocLocal(level, escape_);
  venv->Enter(var_, new env::VarEntry(access, init_ty));

  return new tr::NxExp(
      new tree::MoveStm(access->access_->ToExp(new tree::TempExp(
                            reg_manager->FramePointer())),
                        init_exp_ty->exp_->UnEx()));
}

tr::Exp *TypeDec::Translate(env::VEnvPtr venv, env::TEnvPtr tenv,
                            tr::Level *level, temp::Label *label,
                            err::ErrorMsg *errormsg) const {
  // 第一遍：为每个类型名创建 NameTy 占位符，支持递归类型
  for (NameAndTy *nat : types_->GetList()) {
    tenv->Enter(nat->name_, new type::NameTy(nat->name_, nullptr));
  }
  // 第二遍：解析实际类型
  for (NameAndTy *nat : types_->GetList()) {
    type::Ty *ty = nat->ty_->Translate(tenv, errormsg);
    type::NameTy *name_ty = dynamic_cast<type::NameTy *>(tenv->Look(nat->name_));
    if (name_ty) {
      name_ty->ty_ = ty;
    }
  }
  return new tr::NxExp(new tree::ExpStm(new tree::ConstExp(0)));
}

type::Ty *NameTy::Translate(env::TEnvPtr tenv, err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(name_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + name_->Name());
    return type::VoidTy::Instance();
  }
  return ty;
}

type::Ty *RecordTy::Translate(env::TEnvPtr tenv,
                              err::ErrorMsg *errormsg) const {
  type::FieldList *fields = record_->MakeFieldList(tenv, errormsg);
  return new type::RecordTy(fields);
}

type::Ty *ArrayTy::Translate(env::TEnvPtr tenv, err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(array_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + array_->Name());
    return type::VoidTy::Instance();
  }
  return new type::ArrayTy(ty);
}

} // namespace absyn
