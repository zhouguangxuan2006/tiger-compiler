#include "tiger/absyn/absyn.h"
#include "tiger/semant/semant.h"
#include <set>
#include <vector>

namespace absyn {

namespace {
  std::vector<sym::Symbol *> readonly_vars_stack;
}

void AbsynTree::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                           err::ErrorMsg *errormsg) const {
  root_->SemAnalyze(venv, tenv, 0, errormsg);
}

type::Ty *SimpleVar::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                                int labelcount, err::ErrorMsg *errormsg) const {
  env::EnvEntry *entry = venv->Look(sym_);
  if (!entry) {
    errormsg->Error(pos_, "undefined variable " + sym_->Name());
    return type::IntTy::Instance();
  }
  env::VarEntry *var_entry = dynamic_cast<env::VarEntry *>(entry);
  if (!var_entry) {
    errormsg->Error(pos_, "undefined variable " + sym_->Name());
    return type::IntTy::Instance();
  }
  return var_entry->ty_;
}

type::Ty *FieldVar::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                               int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *var_ty = var_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::RecordTy *record_ty = dynamic_cast<type::RecordTy *>(var_ty->ActualTy());
  if (!record_ty) {
    errormsg->Error(pos_, "not a record type");
    return type::VoidTy::Instance();
  }
  for (type::Field *field : record_ty->fields_->GetList()) {
    if (field->name_->Name() == sym_->Name()) {
      return field->ty_;
    }
  }
  errormsg->Error(pos_, "field nam doesn't exist");
  return type::VoidTy::Instance();
}

type::Ty *SubscriptVar::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                                   int labelcount,
                                   err::ErrorMsg *errormsg) const {
  type::Ty *var_ty = var_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::ArrayTy *array_ty = dynamic_cast<type::ArrayTy *>(var_ty->ActualTy());
  if (!array_ty) {
    errormsg->Error(pos_, "array type required");
    return type::VoidTy::Instance();
  }
  return array_ty->ty_;
}

type::Ty *VarExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  return var_->SemAnalyze(venv, tenv, labelcount, errormsg);
}

type::Ty *NilExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  return type::NilTy::Instance();
}

type::Ty *IntExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  return type::IntTy::Instance();
}

type::Ty *StringExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                                int labelcount, err::ErrorMsg *errormsg) const {
  return type::StringTy::Instance();
}

type::Ty *CallExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                              int labelcount, err::ErrorMsg *errormsg) const {
  env::EnvEntry *entry = venv->Look(func_);
  if (!entry) {
    std::string msg = "undefined function " + func_->Name();
    errormsg->Error(pos_, msg);
    return type::IntTy::Instance();
  }
  env::FunEntry *fun_entry = dynamic_cast<env::FunEntry *>(entry);
  if (!fun_entry) {
    std::string msg = "undefined function " + func_->Name();
    errormsg->Error(pos_, msg);
    return type::IntTy::Instance();
  }
  const std::list<type::Ty *> &formals = fun_entry->formals_->GetList();
  const std::list<Exp *> &args = args_->GetList();
  
  bool param_cnt_ok = true;
  if (formals.size() != args.size()) {
    param_cnt_ok = false;
    if (formals.size() < args.size()) {
      errormsg->Error(pos_, "too many params in function " + func_->Name());
    } else {
      errormsg->Error(pos_, "too few params in function " + func_->Name());
    }
  }
  
  auto formal_it = formals.begin();
  auto arg_it = args.begin();
  for (; formal_it != formals.end() && arg_it != args.end();
       ++formal_it, ++arg_it) {
    type::Ty *arg_ty = (*arg_it)->SemAnalyze(venv, tenv, labelcount, errormsg);
    if (arg_ty->ActualTy() != type::VoidTy::Instance()) {
      if (!(*formal_it)->IsSameType(arg_ty)) {
        errormsg->Error(pos_, "para type mismatch");
      }
    }
  }
  return fun_entry->result_;
}

type::Ty *OpExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                            int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *left_ty = left_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::Ty *right_ty = right_->SemAnalyze(venv, tenv, labelcount, errormsg);
  
  switch (oper_) {
    case PLUS_OP:
    case MINUS_OP:
    case TIMES_OP:
    case DIVIDE_OP:
    case AND_OP:
    case OR_OP:
      if (!left_ty->IsSameType(type::IntTy::Instance())) {
        errormsg->Error(left_->pos_, "integer required");
      }
      if (!right_ty->IsSameType(type::IntTy::Instance())) {
        errormsg->Error(right_->pos_, "integer required");
      }
      return type::IntTy::Instance();
      
    case EQ_OP:
    case NEQ_OP:
      if (!left_ty->IsSameType(right_ty)) {
        errormsg->Error(pos_, "same type required");
      } else {
        if (left_ty->ActualTy() == type::NilTy::Instance() &&
            right_ty->ActualTy() == type::NilTy::Instance()) {
          errormsg->Error(pos_, "same type required");
        }
      }
      return type::IntTy::Instance();
      
    case LT_OP:
    case LE_OP:
    case GT_OP:
    case GE_OP:
      if (!left_ty->IsSameType(right_ty)) {
        errormsg->Error(pos_, "same type required");
      } else {
        type::Ty *actual = left_ty->ActualTy();
        if (!dynamic_cast<type::IntTy *>(actual) && !dynamic_cast<type::StringTy *>(actual)) {
          errormsg->Error(left_->pos_, "integer required");
        }
      }
      return type::IntTy::Instance();
      
    default:
      errormsg->Error(pos_, "unknown operator");
      return type::VoidTy::Instance();
  }
}

type::Ty *RecordExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                                int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(typ_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + typ_->Name());
    return type::VoidTy::Instance();
  }
  type::RecordTy *record_ty = dynamic_cast<type::RecordTy *>(ty->ActualTy());
  if (!record_ty) {
    errormsg->Error(pos_, "not a record type");
    return type::VoidTy::Instance();
  }
  const std::list<type::Field *> &fields = record_ty->fields_->GetList();
  const std::list<EField *> &efields = fields_->GetList();
  auto field_it = fields.begin();
  auto efield_it = efields.begin();
  for (; field_it != fields.end() && efield_it != efields.end();
       ++field_it, ++efield_it) {
    if ((*field_it)->name_->Name() != (*efield_it)->name_->Name()) {
      errormsg->Error(pos_, "field name mismatch");
    }
  }
  if (fields.size() != efields.size()) {
    errormsg->Error(pos_, "field number mismatch");
  }
  return record_ty;
}

type::Ty *SeqExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *result = type::VoidTy::Instance();
  for (Exp *exp : seq_->GetList()) {
    result = exp->SemAnalyze(venv, tenv, labelcount, errormsg);
  }
  return result;
}

type::Ty *IfExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                            int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *test_ty = test_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (!test_ty->IsSameType(type::IntTy::Instance())) {
    errormsg->Error(pos_, "if test must be int");
  }
  type::Ty *then_ty = then_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::Ty *else_ty = nullptr;
  bool has_else = elsee_ != nullptr;
  if (has_else) {
    else_ty = elsee_->SemAnalyze(venv, tenv, labelcount, errormsg);
  } else {
    else_ty = type::VoidTy::Instance();
  }
  if (!has_else) {
    if (!then_ty->IsSameType(type::VoidTy::Instance())) {
      errormsg->Error(pos_, "if-then exp's body must produce no value");
    }
    return type::VoidTy::Instance();
  }
  if (!then_ty->IsSameType(else_ty)) {
    errormsg->Error(pos_, "then exp and else exp type mismatch");
  }
  return then_ty;
}

type::Ty *WhileExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                               int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *test_ty = test_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (!test_ty->IsSameType(type::IntTy::Instance())) {
    errormsg->Error(pos_, "while test must be int");
  }
  type::Ty *body_ty = body_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (!body_ty->IsSameType(type::VoidTy::Instance())) {
    errormsg->Error(pos_, "while body must produce no value");
  }
  return type::VoidTy::Instance();
}

type::Ty *ForExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *lo_ty = lo_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::Ty *hi_ty = hi_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (!lo_ty->IsSameType(type::IntTy::Instance())) {
    errormsg->Error(pos_, "for exp's range type is not integer");
  }
  if (!hi_ty->IsSameType(type::IntTy::Instance())) {
    errormsg->Error(pos_, "for exp's range type is not integer");
  }
  venv->BeginScope();
  venv->Enter(var_, new env::VarEntry(type::IntTy::Instance()));
  readonly_vars_stack.push_back(var_);
  type::Ty *body_ty = body_->SemAnalyze(venv, tenv, labelcount, errormsg);
  readonly_vars_stack.pop_back();
  venv->EndScope();
  if (!body_ty->IsSameType(type::VoidTy::Instance())) {
    errormsg->Error(pos_, "for body must produce no value");
  }
  return type::VoidTy::Instance();
}

type::Ty *BreakExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                               int labelcount, err::ErrorMsg *errormsg) const {
  return type::VoidTy::Instance();
}

type::Ty *LetExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  venv->BeginScope();
  tenv->BeginScope();
  for (Dec *dec : decs_->GetList()) {
    dec->SemAnalyze(venv, tenv, labelcount, errormsg);
  }
  type::Ty *body_ty = body_->SemAnalyze(venv, tenv, labelcount, errormsg);
  venv->EndScope();
  tenv->EndScope();
  return body_ty;
}

type::Ty *ArrayExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                               int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(typ_);
  if (!ty) {
    errormsg->Error(pos_, "undefined type " + typ_->Name());
    return type::VoidTy::Instance();
  }
  type::ArrayTy *array_ty = dynamic_cast<type::ArrayTy *>(ty->ActualTy());
  if (!array_ty) {
    errormsg->Error(pos_, "array type required");
    return type::VoidTy::Instance();
  }
  type::Ty *size_ty = size_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::Ty *init_ty = init_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (!array_ty->ty_->IsSameType(init_ty)) {
    errormsg->Error(pos_, "type mismatch");
  }
  return array_ty;
}

type::Ty *VoidExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                              int labelcount, err::ErrorMsg *errormsg) const {
  return type::VoidTy::Instance();
}

type::Ty *AssignExp::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                                int labelcount, err::ErrorMsg *errormsg) const {
  type::Ty *var_ty = var_->SemAnalyze(venv, tenv, labelcount, errormsg);
  type::Ty *exp_ty = exp_->SemAnalyze(venv, tenv, labelcount, errormsg);

  // 修复：禁止给 for 循环变量赋值
  if (auto simple_var = dynamic_cast<SimpleVar*>(var_)) {
    for (auto it = readonly_vars_stack.rbegin(); it != readonly_vars_stack.rend(); ++it) {
      if (*it == simple_var->sym_) {
        errormsg->Error(pos_, "loop variable can't be assigned");
        break;
      }
    }
  }

  if (!var_ty->IsSameType(exp_ty)) {
    errormsg->Error(pos_, "unmatched assign exp");
  }
  return type::VoidTy::Instance();
}

void FunctionDec::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv,
                             int labelcount, err::ErrorMsg *errormsg) const {
  std::set<std::string> current_func_names;
  for (FunDec *fun_dec : functions_->GetList()) {
    if (current_func_names.count(fun_dec->name_->Name())) {
      errormsg->Error(fun_dec->pos_, "two functions have the same name");
      continue;
    }
    current_func_names.insert(fun_dec->name_->Name());
    type::TyList *formals = fun_dec->params_->MakeFormalTyList(tenv, errormsg);
    type::Ty *result = type::VoidTy::Instance();
    if (fun_dec->result_) {
      result = tenv->Look(fun_dec->result_);
      if (!result) {
        errormsg->Error(fun_dec->pos_, "undefined result type");
        result = type::VoidTy::Instance();
      }
    }
    venv->Enter(fun_dec->name_, new env::FunEntry(formals, result));
  }
  for (FunDec *fun_dec : functions_->GetList()) {
    venv->BeginScope();
    for (Field *field : fun_dec->params_->GetList()) {
      type::Ty *ty = tenv->Look(field->typ_);
      if (!ty) {
        errormsg->Error(field->pos_, "undefined type");
        ty = type::VoidTy::Instance();
      }
      venv->Enter(field->name_, new env::VarEntry(ty));
    }
    type::Ty *body_ty = fun_dec->body_->SemAnalyze(venv, tenv, labelcount, errormsg);
    env::FunEntry *fe = dynamic_cast<env::FunEntry *>(venv->Look(fun_dec->name_));
    if (fe && fe->result_->ActualTy() == type::VoidTy::Instance() && 
        body_ty->ActualTy() != type::VoidTy::Instance()) {
      errormsg->Error(fun_dec->pos_, "procedure returns value");
    }
    venv->EndScope();
  }
}

void VarDec::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv, int labelcount,
                        err::ErrorMsg *errormsg) const {
  type::Ty *ty = nullptr;
  if (typ_) {
    ty = tenv->Look(typ_);
    if (!ty) {
      errormsg->Error(pos_, "undefined type " + typ_->Name());
      ty = type::VoidTy::Instance();
    }
  }
  type::Ty *init_ty = init_->SemAnalyze(venv, tenv, labelcount, errormsg);
  if (init_ty == type::NilTy::Instance() && !typ_) {
    errormsg->Error(pos_, "init should not be nil without type specified");
  }
  if (!ty) {
    ty = init_ty;
  } else {
    type::Ty *actual_ty = ty->ActualTy();
    if (init_ty == type::NilTy::Instance() &&
        dynamic_cast<type::RecordTy *>(actual_ty)) {
    } else if (!ty->IsSameType(init_ty)) {
      errormsg->Error(pos_, "type mismatch");
    }
  }
  venv->Enter(var_, new env::VarEntry(ty));
}

void TypeDec::SemAnalyze(env::VEnvPtr venv, env::TEnvPtr tenv, int labelcount,
                         err::ErrorMsg *errormsg) const {
  std::set<std::string> current_names;
  for (NameAndTy *nat : types_->GetList()) {
    if (current_names.count(nat->name_->Name())) {
      errormsg->Error(nat->ty_->pos_, "two types have the same name");
    } else {
      current_names.insert(nat->name_->Name());
      tenv->Enter(nat->name_, new type::NameTy(nat->name_, nullptr));
    }
  }
  for (NameAndTy *nat : types_->GetList()) {
    type::Ty *res_ty = nat->ty_->SemAnalyze(tenv, errormsg);
    type::NameTy *name_ty = dynamic_cast<type::NameTy *>(tenv->Look(nat->name_));
    if (name_ty) {
      name_ty->ty_ = res_ty;
    }
  }
  for (NameAndTy *nat : types_->GetList()) {
    type::Ty *initial = tenv->Look(nat->name_);
    type::Ty *curr = initial;
    while (curr) {
      auto next = dynamic_cast<type::NameTy *>(curr);
      if (!next || !next->ty_) break;
      curr = next->ty_;
      if (curr == initial) {
        errormsg->Error(nat->ty_->pos_, "illegal type cycle");
        if (auto name_ty = dynamic_cast<type::NameTy *>(initial)) {
          name_ty->ty_ = type::VoidTy::Instance();
        }
        break;
      }
    }
  }
}

type::Ty *NameTy::SemAnalyze(env::TEnvPtr tenv, err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(name_);
  if (!ty) {
    std::string msg = "undefined type " + name_->Name();
    errormsg->Error(pos_, msg);
    return type::VoidTy::Instance();
  }
  return new type::NameTy(name_, ty);
}

type::Ty *RecordTy::SemAnalyze(env::TEnvPtr tenv,
                               err::ErrorMsg *errormsg) const {
  type::FieldList *fields = record_->MakeFieldList(tenv, errormsg);
  return new type::RecordTy(fields);
}

type::Ty *ArrayTy::SemAnalyze(env::TEnvPtr tenv,
                              err::ErrorMsg *errormsg) const {
  type::Ty *ty = tenv->Look(array_);
  if (!ty) {
    std::string msg = "undefined type " + array_->Name();
    errormsg->Error(pos_, msg);
    return type::VoidTy::Instance();
  }
  return new type::ArrayTy(ty);
}

} // namespace absyn

namespace sem {

void ProgSem::SemAnalyze() {
  FillBaseTEnv();
  FillBaseVEnv();
  absyn_tree_->SemAnalyze(venv_.get(), tenv_.get(), errormsg_.get());
}

} // namespace sem