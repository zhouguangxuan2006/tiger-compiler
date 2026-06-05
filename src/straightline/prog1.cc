#include "straightline/prog1.h"

// a = 5 + 3; b = (print(a, a-1), 10*a); print b;
absyn::Stm *Prog() {
  // a = 5 + 3;
  absyn::Stm *ass_stm1 = new absyn::AssignStm(
      "a", new absyn::OpExp(new absyn::NumExp(5), absyn::PLUS,
                            new absyn::NumExp(3)));

  // b = (print(a, a-1), 10*a);
  absyn::Exp *op_exp1 = new absyn::OpExp(new absyn::IdExp("a"), absyn::MINUS,
                                         new absyn::NumExp(1));

  auto exp_list1 = new absyn::ExpList{new absyn::IdExp("a"), op_exp1};
  absyn::Stm *pr_stm1 = new absyn::PrintStm(exp_list1);

  absyn::Exp *op_exp2 = new absyn::OpExp(new absyn::NumExp(10), absyn::TIMES,
                                         new absyn::IdExp("a"));
  absyn::Stm *ass_stm2 =
      new absyn::AssignStm("b", new absyn::EseqExp(pr_stm1, op_exp2));

  // print b;
  absyn::Stm *pr_stm2 =
      new absyn::PrintStm(new absyn::ExpList(new absyn::IdExp("b")));

  // b = (print(a, a-1), 10*a); print b;
  absyn::Stm *com_stm = new absyn::CompoundStm(ass_stm2, pr_stm2);

  return new absyn::CompoundStm(ass_stm1, com_stm);
}

// a = 5 + 3; b = (print(a, a-1), 10*a); print b;
// a = 5 + b; b = (print(a, a, a-1), 10*a); print b;
absyn::Stm *ProgProg() {
  // a = 5 + 3; b = (print(a, a-1), 10*a); print b;
  absyn::Stm *stm1 = Prog();

  // a = 5 + b;
  absyn::Stm *ass_stm1 = new absyn::AssignStm(
      "a", new absyn::OpExp(new absyn::NumExp(5), absyn::PLUS,
                            new absyn::IdExp("b")));

  // print(a, a, a-1)
  absyn::Exp *exp1 = new absyn::OpExp(new absyn::IdExp("a"), absyn::MINUS,
                                      new absyn::NumExp(1));

  auto exp_list2 =
      new absyn::ExpList{new absyn::IdExp("a"), new absyn::IdExp("a"), exp1};

  // 10 * a
  absyn::Exp *exp2 = new absyn::OpExp(new absyn::NumExp(10), absyn::TIMES,
                                      new absyn::IdExp("a"));

  // b = (print(a, a, a-1), 10*a);
  absyn::Stm *ass_stm2 = new absyn::AssignStm(
      "b", new absyn::EseqExp(new absyn::PrintStm(exp_list2), exp2));

  // print b;
  absyn::Stm *pr_stm2 =
      new absyn::PrintStm(new absyn::ExpList(new absyn::IdExp("b")));

  return new absyn::CompoundStm(
      stm1, new absyn::CompoundStm(ass_stm1,
                                   new absyn::CompoundStm(ass_stm2, pr_stm2)));
}

// a = 5 + 3; b = (print(a, a-1), 10*a); print b;
// a = 5 + b; b = (print(a, a, a-1), 10*a); print b;
// a = (a = a+b, a);
absyn::Stm *RightProg() {
  absyn::Stm *stm1 = ProgProg();
  return new absyn::CompoundStm(
      stm1,
      new absyn::AssignStm(
          "a", new absyn::EseqExp(
                   new absyn::AssignStm(
                       "a", new absyn::OpExp(new absyn::IdExp("a"), absyn::PLUS,
                                             new absyn::IdExp("b"))),
                   new absyn::IdExp("a"))));
}
