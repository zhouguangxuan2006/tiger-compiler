#include "straightline/slp.h"

#include <iostream>

namespace absyn {
int absyn::CompoundStm::MaxArgs() const {
  int args1 = stm1->MaxArgs();
  int args2 = stm2->MaxArgs();
  return args1 > args2 ? args1 : args2;
}

Table *absyn::CompoundStm::Interp(Table *t) const {
  return stm2->Interp(stm1->Interp(t));
}

int absyn::AssignStm::MaxArgs() const { return exp->MaxArgs(); }

Table *absyn::AssignStm::Interp(Table *t) const {
  IntAndTable *expValue = exp->Interp(t);
  return expValue->t->Update(id, expValue->i);
}

int absyn::PrintStm::MaxArgs() const { return exps->NumExps(); }

Table *absyn::PrintStm::Interp(Table *t) const { return exps->Interp(t)->t; }

int absyn::IdExp::MaxArgs() const { return 0; }

IntAndTable *absyn::IdExp::Interp(Table *t) const {
  return new IntAndTable(t->Lookup(id), t);
}

int absyn::NumExp::MaxArgs() const { return 0; }

IntAndTable *absyn::NumExp::Interp(Table *t) const {
  return new IntAndTable(num, t);
}

int absyn::OpExp::MaxArgs() const {
  int args1 = left_->MaxArgs();
  int args2 = right_->MaxArgs();
  return args1 > args2 ? args1 : args2;
}

IntAndTable *absyn::OpExp::Interp(Table *t) const {
  IntAndTable *expValue1 = left_->Interp(t);
  IntAndTable *expValue2 = right_->Interp(t);
  switch (oper_) {
  case PLUS:
    return new IntAndTable(expValue1->i + expValue2->i, expValue2->t);
  case MINUS:
    return new IntAndTable(expValue1->i - expValue2->i, expValue2->t);
  case TIMES:
    return new IntAndTable(expValue1->i * expValue2->i, expValue2->t);
  case DIV:
    return new IntAndTable(expValue1->i / expValue2->i, expValue2->t);
  }
}

int absyn::EseqExp::MaxArgs() const {
  int args1 = stm->MaxArgs();
  int args2 = exp->MaxArgs();
  return args1 > args2 ? args1 : args2;
}

IntAndTable *absyn::EseqExp::Interp(Table *t) const {
  return exp->Interp(stm->Interp(t));
}

IntAndTable *absyn::ExpList::Interp(Table *t) const {
  IntAndTable *ret;
  for (auto it : exp_list_) {
    ret = it->Interp(t);
    std::cout << ret->i << " ";
  }
  std::cout << std::endl;
  return ret;
}

int absyn::ExpList::NumExps() const {
  int exps = 0;
  for (__attribute__((unused)) auto it : exp_list_)
    exps++;
  return exps;
}

int Table::Lookup(const std::string &key) const {
  if (id == key) {
    return value;
  } else if (tail != nullptr) {
    return tail->Lookup(key);
  } else {
    assert(false);
  }
}

Table *Table::Update(const std::string &key, int val) const {
  return new Table(key, val, this);
}
} // namespace absyn
