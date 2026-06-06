#include "tiger/codegen/codegen.h"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <string>
#include <vector>

extern frame::RegManager *reg_manager;

namespace {

constexpr int maxlen = 1024;
frame::Frame *current_frame = nullptr;

temp::TempList *TL() { return new temp::TempList(); }

temp::TempList *TL(temp::Temp *a) { return new temp::TempList({a}); }

temp::TempList *TL(temp::Temp *a, temp::Temp *b) {
  return new temp::TempList({a, b});
}

temp::TempList *TL(temp::Temp *a, temp::Temp *b, temp::Temp *c) {
  return new temp::TempList({a, b, c});
}

bool IsFramePointer(tree::Exp *exp) {
  auto temp_exp = dynamic_cast<tree::TempExp *>(exp);
  return temp_exp && temp_exp->temp_ == reg_manager->FramePointer();
}

bool IsConst(tree::Exp *exp, int *value) {
  auto const_exp = dynamic_cast<tree::ConstExp *>(exp);
  if (!const_exp)
    return false;
  *value = const_exp->consti_;
  return true;
}

std::string FrameOffset(int offset, std::string_view fs) {
  std::ostringstream out;
  if (offset == 0)
    out << fs;
  else if (offset > 0)
    out << offset << "+" << fs;
  else
    out << fs << offset;
  out << "(%rsp)";
  return out.str();
}

struct MemOperand {
  std::string assem_;
  temp::TempList *src_;
};

MemOperand MunchMemOperand(tree::Exp *exp, assem::InstrList &instr_list,
                           std::string_view fs) {
  if (IsFramePointer(exp))
    return {FrameOffset(0, fs), TL()};

  if (auto binop = dynamic_cast<tree::BinopExp *>(exp);
      binop && binop->op_ == tree::PLUS_OP) {
    int offset = 0;
    if (IsFramePointer(binop->left_) && IsConst(binop->right_, &offset))
      return {FrameOffset(offset, fs), TL()};
    if (IsFramePointer(binop->right_) && IsConst(binop->left_, &offset))
      return {FrameOffset(offset, fs), TL()};
  }

  temp::Temp *addr = exp->Munch(instr_list, fs);
  return {"(`s0)", TL(addr)};
}

std::string RelopAssem(tree::RelOp op) {
  switch (op) {
  case tree::EQ_OP:
    return "je";
  case tree::NE_OP:
    return "jne";
  case tree::LT_OP:
  case tree::ULT_OP:
    return "jl";
  case tree::GT_OP:
  case tree::UGT_OP:
    return "jg";
  case tree::LE_OP:
  case tree::ULE_OP:
    return "jle";
  case tree::GE_OP:
  case tree::UGE_OP:
    return "jge";
  default:
    assert(false);
  }
}

} // namespace

namespace cg {

void CodeGen::Codegen() {
  fs_ = frame_->GetLabel() + "_framesize";
  current_frame = frame_;
  auto *instr_list = new assem::InstrList();
  for (auto stm : traces_->GetStmList()->GetList())
    stm->Munch(*instr_list, fs_);
  current_frame = nullptr;
  assem_instr_ = std::make_unique<AssemInstr>(instr_list);
}

void AssemInstr::Print(FILE *out, temp::Map *map) const {
  for (auto instr : instr_list_->GetList())
    instr->Print(out, map);
  fprintf(out, "\n");
}
} // namespace cg

namespace tree {

/**
 * Generate code for passing arguments
 * @param args argument list
 * @param instr_holder instruction holder
 * @return temp list to hold arguments
 */
temp::TempList *ExpList::MunchArgs(assem::InstrList &instr_list, std::string_view fs) {
  std::vector<temp::Temp *> arg_values;
  auto iter = exp_list_.begin();
  if (iter != exp_list_.end()) {
    auto magic_name = dynamic_cast<tree::NameExp *>(*iter);
    if (magic_name &&
        temp::LabelFactory::LabelString(magic_name->name_) == "staticLink")
      ++iter;
  }

  for (; iter != exp_list_.end(); ++iter)
    arg_values.push_back((*iter)->Munch(instr_list, fs));

  temp::TempList *arg_regs = reg_manager->ArgRegs();
  auto reg_iter = arg_regs->GetList().begin();
  auto *used_regs = new temp::TempList();
  int stack_args = 0;
  for (auto arg : arg_values) {
    if (reg_iter != arg_regs->GetList().end()) {
      temp::Temp *reg = *reg_iter++;
      instr_list.Append(
          new assem::MoveInstr("movq `s0, `d0", TL(reg), TL(arg)));
      used_regs->Append(reg);
    } else {
      std::ostringstream out;
      out << "movq `s0, " << stack_args * reg_manager->WordSize() << "(%rsp)";
      instr_list.Append(
          new assem::MoveInstr(out.str(), nullptr, TL(arg)));
      ++stack_args;
    }
  }

  if (current_frame) {
    int outgo_size = stack_args * reg_manager->WordSize();
    current_frame->AllocOutgoSpace(
        std::max(current_frame->outgoing_size_, outgo_size));
  }
  return used_regs;
}

void SeqStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  assert(false);
}

void LabelStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  instr_list.Append(new assem::LabelInstr(label_->Name(), label_));
}

void JumpStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  instr_list.Append(new assem::OperInstr(
      "jmp `j0", nullptr, nullptr, new assem::Targets(jumps_)));
}

void CjumpStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::Temp *left = left_->Munch(instr_list, fs);
  temp::Temp *right = right_->Munch(instr_list, fs);
  instr_list.Append(
      new assem::OperInstr("cmpq `s0, `s1", nullptr, TL(right, left), nullptr));
  instr_list.Append(new assem::OperInstr(
      RelopAssem(op_) + " `j0", nullptr, nullptr,
      new assem::Targets(new std::vector<temp::Label *>({true_label_}))));
}

void MoveStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  if (auto dst_temp = dynamic_cast<tree::TempExp *>(dst_)) {
    temp::Temp *src = src_->Munch(instr_list, fs);
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0",
                                           TL(dst_temp->temp_), TL(src)));
    return;
  }

  if (auto dst_mem = dynamic_cast<tree::MemExp *>(dst_)) {
    temp::Temp *src = src_->Munch(instr_list, fs);
    MemOperand mem = MunchMemOperand(dst_mem->exp_, instr_list, fs);
    if (mem.src_->GetList().empty()) {
      instr_list.Append(
          new assem::MoveInstr("movq `s0, " + mem.assem_, nullptr, TL(src)));
    } else {
      temp::Temp *addr = mem.src_->NthTemp(0);
      instr_list.Append(new assem::MoveInstr("movq `s0, (`s1)", nullptr,
                                             TL(src, addr)));
    }
    return;
  }

  assert(false);
}

void ExpStm::Munch(assem::InstrList &instr_list, std::string_view fs) {
  exp_->Munch(instr_list, fs);
}

temp::Temp *BinopExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::Temp *dst = temp::TempFactory::NewTemp();
  temp::Temp *left = left_->Munch(instr_list, fs);
  temp::Temp *right = right_->Munch(instr_list, fs);

  switch (op_) {
  case PLUS_OP:
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0", TL(dst), TL(left)));
    instr_list.Append(
        new assem::OperInstr("addq `s0, `d0", TL(dst), TL(right), nullptr));
    return dst;
  case MINUS_OP:
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0", TL(dst), TL(left)));
    instr_list.Append(
        new assem::OperInstr("subq `s0, `d0", TL(dst), TL(right), nullptr));
    return dst;
  case MUL_OP:
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0",
                                           TL(reg_manager->ReturnValue()),
                                           TL(left)));
    instr_list.Append(new assem::OperInstr(
        "imulq `s0", TL(reg_manager->ReturnValue()),
        TL(right, reg_manager->ReturnValue()), nullptr));
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0", TL(dst),
                                           TL(reg_manager->ReturnValue())));
    return dst;
  case DIV_OP:
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0",
                                           TL(reg_manager->ReturnValue()),
                                           TL(left)));
    instr_list.Append(new assem::OperInstr(
        "cqto", TL(reg_manager->GetRegister(frame::X64RegManager::RDX)),
        TL(reg_manager->ReturnValue()), nullptr));
    instr_list.Append(new assem::OperInstr(
        "idivq `s0",
        TL(reg_manager->ReturnValue(),
           reg_manager->GetRegister(frame::X64RegManager::RDX)),
        TL(right, reg_manager->ReturnValue(),
           reg_manager->GetRegister(frame::X64RegManager::RDX)),
        nullptr));
    instr_list.Append(new assem::MoveInstr("movq `s0, `d0", TL(dst),
                                           TL(reg_manager->ReturnValue())));
    return dst;
  default:
    assert(false);
  }
}

temp::Temp *MemExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::Temp *dst = temp::TempFactory::NewTemp();
  MemOperand mem = MunchMemOperand(exp_, instr_list, fs);
  instr_list.Append(
      new assem::OperInstr("movq " + mem.assem_ + ", `d0", TL(dst), mem.src_,
                           nullptr));
  return dst;
}

temp::Temp *TempExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  if (temp_ == reg_manager->FramePointer()) {
    temp::Temp *dst = temp::TempFactory::NewTemp();
    instr_list.Append(new assem::OperInstr("leaq " + std::string(fs) +
                                               "(%rsp), `d0",
                                           TL(dst), nullptr, nullptr));
    return dst;
  }
  return temp_;
}

temp::Temp *EseqExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  assert(false);
}

temp::Temp *NameExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::Temp *dst = temp::TempFactory::NewTemp();
  instr_list.Append(new assem::OperInstr(
      "leaq " + temp::LabelFactory::LabelString(name_) + "(%rip), `d0",
      TL(dst), nullptr, nullptr));
  return dst;
}

temp::Temp *ConstExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::Temp *dst = temp::TempFactory::NewTemp();
  instr_list.Append(new assem::OperInstr("movq $" + std::to_string(consti_) +
                                             ", `d0",
                                         TL(dst), nullptr, nullptr));
  return dst;
}

temp::Temp *CallExp::Munch(assem::InstrList &instr_list, std::string_view fs) {
  temp::TempList *args = args_->MunchArgs(instr_list, fs);
  auto name = dynamic_cast<tree::NameExp *>(fun_);
  assert(name);
  instr_list.Append(new assem::OperInstr(
      "callq " + temp::LabelFactory::LabelString(name->name_),
      reg_manager->CallerSaves(), args, nullptr));
  temp::Temp *dst = temp::TempFactory::NewTemp();
  instr_list.Append(new assem::MoveInstr("movq `s0, `d0", TL(dst),
                                         TL(reg_manager->ReturnValue())));
  return dst;
}

} // namespace tree
