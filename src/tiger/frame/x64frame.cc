#include "tiger/frame/x64frame.h"
#include <array>
#include <sstream>

extern frame::RegManager *reg_manager;

namespace frame {

X64RegManager::X64RegManager() : RegManager() {
  for (int i = 0; i < REG_COUNT; i++)
    RegManager::regs_.push_back(temp::TempFactory::NewTemp());

  // Note: no frame pointer in tiger compiler
  const char* reg_name[REG_COUNT] = {
      "%rax", "%rbx", "%rcx", "%rdx", "%rsi", "%rdi", "%rbp", "%rsp",
      "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15",
      "%fp"};  // FP is a pseudo-register
  int reg = RAX;
  for (int i = 0; i < REG_COUNT; i++) {
    RegManager::temp_map_->Enter(RegManager::regs_[reg], new std::string(reg_name[i]));
    reg++;
  }
}

temp::TempList *X64RegManager::Registers() {
  const std::array<Reg, 15> reg_array{
      RAX, RBX, RCX, RDX, RSI, RDI, RBP, R8, R9, R10, R11, R12, R13, R14, R15,
  };
  auto *temp_list = new temp::TempList();
  for (auto &reg : reg_array)
    temp_list->Append(RegManager::regs_[reg]);
  return temp_list;
}

temp::TempList *X64RegManager::ArgRegs() {
  const std::array<Reg, 6> reg_array{RDI, RSI, RDX, RCX, R8, R9};
  auto *temp_list = new temp::TempList();
  for (auto &reg : reg_array)
    temp_list->Append(RegManager::regs_[reg]);
  return temp_list;
}

temp::TempList *X64RegManager::CallerSaves() {
  const std::array<Reg, 9> reg_array{RAX, RDI, RSI, RDX, RCX, R8, R9, R10, R11};
  auto *temp_list = new temp::TempList();
  for (auto &reg : reg_array)
    temp_list->Append(RegManager::regs_[reg]);
  return temp_list;
}

temp::TempList *X64RegManager::CalleeSaves() {
  const std::array<Reg, 6> reg_array{RBP, RBX, R12, R13, R14, R15};
  auto *temp_list = new temp::TempList();
  for (auto &reg : reg_array)
    temp_list->Append(RegManager::regs_[reg]);
  return temp_list;
}

temp::TempList *X64RegManager::ReturnSink() {
  temp::TempList *temp_list = CalleeSaves();
  temp_list->Append(RegManager::regs_[SP]);
  temp_list->Append(RegManager::regs_[RV]);
  return temp_list;
}

int X64RegManager::WordSize() { return 8; }

temp::Temp *X64RegManager::FramePointer() { return RegManager::regs_[FP]; }

temp::Temp *X64RegManager::StackPointer() { return RegManager::regs_[SP]; }

temp::Temp *X64RegManager::ReturnValue() { return RegManager::regs_[RV]; }

class InFrameAccess : public Access {
public:
  int offset;

  explicit InFrameAccess(int offset) : offset(offset) {}
  tree::Exp *ToExp(tree::Exp *frame_ptr) const override {
    return new tree::MemExp(new tree::BinopExp(tree::PLUS_OP, frame_ptr,
                                               new tree::ConstExp(offset)));
  }
};


class InRegAccess : public Access {
public:
  temp::Temp *reg;

  explicit InRegAccess(temp::Temp *reg) : reg(reg) {}
  tree::Exp *ToExp(tree::Exp *framePtr) const override {
    return new tree::TempExp(reg);
  }
};

class X64Frame : public Frame {
public:
  tree::Stm *view_shift;

  X64Frame(temp::Label *name, std::list<frame::Access *> *formals)
      : Frame(8, 0, name, formals), view_shift(nullptr) {}

  [[nodiscard]] std::string GetLabel() const override { 
    return name_ ? name_->Name() : ""; 
  }
  [[nodiscard]] temp::Label *Name() const override { return name_; }
  [[nodiscard]] std::list<frame::Access *> *Formals() const override {
    return formals_;
  }
  frame::Access *AllocLocal(bool escape) override {
    frame::Access *access = nullptr;
    if (escape) {
      local_offset_ -= word_size_;
      access = new InFrameAccess(local_offset_);
    } else {
      access = new InRegAccess(temp::TempFactory::NewTemp());
    }
    // 关键修复：分配的局部变量必须加入到 locals_ 列表中，供后端计算栈大小
    locals_->push_back(access);
    return access;
  }
  void AllocOutgoSpace(int size) override {
    outgoing_size_ = size;
  }
};

frame::Frame *NewFrame(temp::Label *name, std::list<bool> formals) {
  auto *access_list = new std::list<frame::Access *>();
  auto *frame = new X64Frame(name, access_list);

  temp::TempList *arg_regs = reg_manager->ArgRegs();
  auto arg_reg_it = arg_regs->GetList().begin();
  int incoming_stack_offset = 8;
  tree::Stm *shift_stm = nullptr;

  for (bool escape : formals) {
    frame::Access *access = nullptr;

    if (arg_reg_it != arg_regs->GetList().end()) {
      temp::Temp *actual_reg = *arg_reg_it++;
      access = frame->AllocLocal(escape);
      tree::Stm *move_formal = new tree::MoveStm(
          access->ToExp(new tree::TempExp(reg_manager->FramePointer())),
          new tree::TempExp(actual_reg));

      if (!shift_stm)
        shift_stm = move_formal;
      else
        shift_stm = new tree::SeqStm(shift_stm, move_formal);
    } else {
      access = new InFrameAccess(incoming_stack_offset);
      incoming_stack_offset += reg_manager->WordSize();
    }

    access_list->push_back(access);
  }

  frame->view_shift = shift_stm;
  return frame;
}

tree::Exp *ExternalCall(const std::string& s, tree::ExpList *args) {
  // Prepend a magic exp at first arg, indicating do not pass static link on
  // stack
  args->Insert(new tree::NameExp(temp::LabelFactory::NamedLabel("staticLink")));
  return new tree::CallExp(new tree::NameExp(temp::LabelFactory::NamedLabel(s)),
                           args);
}

/**
 * Moving incoming formal parameters, the saving and restoring of callee-save
 * Registers
 * @param frame curruent frame
 * @param stm statements
 * @return statements with saving, restoring and view shift
 */
tree::Stm *ProcEntryExit1(frame::Frame *frame, tree::Stm *stm) {
  auto x64_frame = dynamic_cast<frame::X64Frame *>(frame);
  assert(x64_frame);

  if (x64_frame->view_shift == nullptr)
    return stm;
  return new tree::SeqStm(x64_frame->view_shift, stm);
}

assem::Proc *ProcEntryExit3(frame::Frame *frame, assem::InstrList *body) {
  int frame_size = -frame->local_offset_ + frame->outgoing_size_;
  std::string frame_size_label = frame->GetLabel() + "_framesize";

  std::ostringstream prolog;
  prolog << ".set " << frame_size_label << ", " << frame_size << "\n";
  prolog << frame->GetLabel() << ":\n";
  prolog << "subq $" << frame_size << ", %rsp\n";

  std::ostringstream epilog;
  epilog << "addq $" << frame_size << ", %rsp\n";
  epilog << "retq\n";

  return new assem::Proc(prolog.str(), body, epilog.str());
}

} // namespace frame
