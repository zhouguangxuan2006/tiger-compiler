#include "tiger/liveness/liveness.h"

#include <set>
#include <vector>

extern frame::RegManager *reg_manager;

namespace live {
namespace {

std::set<temp::Temp *> ToSet(temp::TempList *list) {
  std::set<temp::Temp *> result;
  if (!list)
    return result;
  for (auto temp : list->GetList())
    result.insert(temp);
  return result;
}

temp::TempList *ToList(const std::set<temp::Temp *> &temps) {
  auto *result = new temp::TempList();
  for (auto temp : temps)
    result->Append(temp);
  return result;
}

bool SameSet(temp::TempList *left, temp::TempList *right) {
  return ToSet(left) == ToSet(right);
}

std::set<temp::Temp *> SetUnion(const std::set<temp::Temp *> &left,
                                const std::set<temp::Temp *> &right) {
  std::set<temp::Temp *> result = left;
  result.insert(right.begin(), right.end());
  return result;
}

std::set<temp::Temp *> SetDiff(const std::set<temp::Temp *> &left,
                               const std::set<temp::Temp *> &right) {
  std::set<temp::Temp *> result;
  for (auto item : left) {
    if (!right.count(item))
      result.insert(item);
  }
  return result;
}

} // namespace

bool MoveList::Contain(INodePtr src, INodePtr dst) {
  return std::any_of(move_list_.cbegin(), move_list_.cend(),
                     [src, dst](std::pair<INodePtr, INodePtr> move) {
                       return move.first == src && move.second == dst;
                     });
}

void MoveList::Delete(INodePtr src, INodePtr dst) {
  assert(src && dst);
  auto move_it = move_list_.begin();
  for (; move_it != move_list_.end(); move_it++) {
    if (move_it->first == src && move_it->second == dst) {
      break;
    }
  }
  move_list_.erase(move_it);
}

MoveList *MoveList::Union(MoveList *list) {
  auto *res = new MoveList();
  for (auto move : move_list_) {
    res->move_list_.push_back(move);
  }
  for (auto move : list->GetList()) {
    if (!res->Contain(move.first, move.second))
      res->move_list_.push_back(move);
  }
  return res;
}

MoveList *MoveList::Intersect(MoveList *list) {
  auto *res = new MoveList();
  for (auto move : list->GetList()) {
    if (Contain(move.first, move.second))
      res->move_list_.push_back(move);
  }
  return res;
}

void LiveGraphFactory::LiveMap() {
  std::vector<fg::FNodePtr> nodes;
  for (auto node : flowgraph_->Nodes()->GetList()) {
    nodes.push_back(node);
    in_->Enter(node, new temp::TempList());
    out_->Enter(node, new temp::TempList());
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
      fg::FNodePtr node = *it;
      temp::TempList *old_in = in_->Look(node);
      temp::TempList *old_out = out_->Look(node);

      std::set<temp::Temp *> out_set;
      for (auto succ : node->Succ()->GetList())
        out_set = SetUnion(out_set, ToSet(in_->Look(succ)));

      std::set<temp::Temp *> use_set = ToSet(node->NodeInfo()->Use());
      std::set<temp::Temp *> def_set = ToSet(node->NodeInfo()->Def());
      std::set<temp::Temp *> in_set =
          SetUnion(use_set, SetDiff(out_set, def_set));

      auto *new_in = ToList(in_set);
      auto *new_out = ToList(out_set);
      if (!SameSet(old_in, new_in) || !SameSet(old_out, new_out))
        changed = true;
      in_->Set(node, new_in);
      out_->Set(node, new_out);
    }
  }
}

void LiveGraphFactory::InterfGraph() {
  auto get_node = [this](temp::Temp *temp) -> INodePtr {
    INodePtr node = temp_node_map_->Look(temp);
    if (!node) {
      node = live_graph_.interf_graph->NewNode(temp);
      temp_node_map_->Enter(temp, node);
    }
    return node;
  };

  for (auto reg : reg_manager->Registers()->GetList())
    get_node(reg);
  for (auto reg : reg_manager->CallerSaves()->GetList())
    get_node(reg);

  for (auto flow_node : flowgraph_->Nodes()->GetList()) {
    assem::Instr *instr = flow_node->NodeInfo();
    auto def_set = ToSet(instr->Def());
    auto use_set = ToSet(instr->Use());
    auto live_set = ToSet(out_->Look(flow_node));
    bool is_move = dynamic_cast<assem::MoveInstr *>(instr) != nullptr;

    for (auto temp : def_set)
      get_node(temp);
    for (auto temp : use_set)
      get_node(temp);
    for (auto temp : live_set)
      get_node(temp);

    if (is_move && !def_set.empty() && !use_set.empty()) {
      live_graph_.moves->Append(get_node(*use_set.begin()),
                                get_node(*def_set.begin()));
      live_set = SetDiff(live_set, use_set);
    }

    for (auto def : def_set) {
      INodePtr def_node = get_node(def);
      for (auto live : live_set) {
        if (def == live)
          continue;
        INodePtr live_node = get_node(live);
        live_graph_.interf_graph->AddEdge(def_node, live_node);
        live_graph_.interf_graph->AddEdge(live_node, def_node);
      }
    }
  }
}

void LiveGraphFactory::Liveness() {
  LiveMap();
  InterfGraph();
}

} // namespace live
