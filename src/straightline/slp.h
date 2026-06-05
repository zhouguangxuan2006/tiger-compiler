#ifndef STRAIGHTLINE_SLP_H_
#define STRAIGHTLINE_SLP_H_

#include <algorithm>
#include <cassert>
#include <list>
#include <string>

namespace absyn {

class Stm;
class Exp;
class ExpList;

enum BinOp { PLUS = 0, MINUS, TIMES, DIV };

// some data structures used by interp
class Table;
class IntAndTable;

class Stm {
public:
  virtual int MaxArgs() const = 0;
  virtual Table *Interp(Table *) const = 0;
};

class CompoundStm : public Stm {
public:
  CompoundStm(Stm *stm1, Stm *stm2) : stm1(stm1), stm2(stm2) {}
  int MaxArgs() const override;
  Table *Interp(Table *) const override;

private:
  Stm *stm1, *stm2;
};

class AssignStm : public Stm {
public:
  AssignStm(std::string id, Exp *exp) : id(std::move(id)), exp(exp) {}
  int MaxArgs() const override;
  Table *Interp(Table *) const override;

private:
  std::string id;
  Exp *exp;
};

class PrintStm : public Stm {
public:
  explicit PrintStm(ExpList *exps) : exps(exps) {}
  int MaxArgs() const override;
  Table *Interp(Table *) const override;

private:
  ExpList *exps;
};

class Exp {
public:
  virtual int MaxArgs() const = 0;
  virtual IntAndTable *Interp(Table *) const = 0;
};

class IdExp : public Exp {
public:
  explicit IdExp(std::string id) : id(std::move(id)) {}
  int MaxArgs() const override;
  IntAndTable *Interp(Table *) const override;

private:
  std::string id;
};

class NumExp : public Exp {
public:
  explicit NumExp(int num) : num(num) {}
  int MaxArgs() const override;
  IntAndTable *Interp(Table *) const override;

private:
  int num;
};

class OpExp : public Exp {
public:
  OpExp(Exp *left, BinOp oper, Exp *right)
      : left_(left), oper_(oper), right_(right) {}

  int MaxArgs() const override;
  IntAndTable *Interp(Table *) const override;

private:
  Exp *left_;
  BinOp oper_;
  Exp *right_;
};

class EseqExp : public Exp {
public:
  EseqExp(Stm *stm, Exp *exp) : stm(stm), exp(exp) {}

  int MaxArgs() const override;
  IntAndTable *Interp(Table *) const override;

private:
  Stm *stm;
  Exp *exp;
};

class ExpList {
public:
  ExpList(std::initializer_list<Exp *> exp_list) : exp_list_(exp_list) {}
  explicit ExpList(Exp *exp) : exp_list_({exp}) {}

  IntAndTable *Interp(Table *) const;
  int NumExps() const;

private:
  std::list<Exp *> exp_list_;
};

class Table {
public:
  Table(std::string id, int value, const Table *tail)
      : id(std::move(id)), value(value), tail(tail) {}
  int Lookup(const std::string &key) const;
  Table *Update(const std::string &key, int val) const;

private:
  std::string id;
  int value;
  const Table *tail;
};

struct IntAndTable {
  int i;
  Table *t;

  IntAndTable(int i, Table *t) : i(i), t(t) {}
};

} // namespace absyn

#endif // STRAIGHTLINE_SLP_H_
