#include "tiger/liveness/flowgraph.h"

namespace fg {

void FlowGraphFactory::AssemFlowGraph() {
  std::vector<FNodePtr> nodes;
  for (auto instr : instr_list_->GetList()) {
    FNodePtr node = flowgraph_->NewNode(instr);
    nodes.push_back(node);

    if (auto label = dynamic_cast<assem::LabelInstr *>(instr))
      label_map_->Enter(label->label_, node);
  }

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    auto instr = nodes[i]->NodeInfo();
    bool has_jump = false;
    bool unconditional_jump = false;

    if (auto oper = dynamic_cast<assem::OperInstr *>(instr);
        oper && oper->jumps_) {
      has_jump = true;
      unconditional_jump = oper->assem_.rfind("jmp", 0) == 0;
      for (auto label : *oper->jumps_->labels_) {
        FNodePtr target = label_map_->Look(label);
        if (target)
          flowgraph_->AddEdge(nodes[i], target);
      }
    }

    if (i + 1 < nodes.size() && (!has_jump || !unconditional_jump))
      flowgraph_->AddEdge(nodes[i], nodes[i + 1]);
  }
}

} // namespace fg

namespace assem {

temp::TempList *LabelInstr::Def() const {
  return new temp::TempList();
}

temp::TempList *MoveInstr::Def() const {
  return dst_ ? dst_ : new temp::TempList();
}

temp::TempList *OperInstr::Def() const {
  return dst_ ? dst_ : new temp::TempList();
}

temp::TempList *LabelInstr::Use() const {
  return new temp::TempList();
}

temp::TempList *MoveInstr::Use() const {
  return src_ ? src_ : new temp::TempList();
}

temp::TempList *OperInstr::Use() const {
  return src_ ? src_ : new temp::TempList();
}
} // namespace assem
