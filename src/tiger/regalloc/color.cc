#include "tiger/regalloc/color.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

extern frame::RegManager *reg_manager;

namespace col {
bool Color::Precolored(live::INodePtr node) const {
  return initial_->Look(node->NodeInfo()) != nullptr;
}

Result Color::Coloring() {
  const int color_count = registers_->GetList().size();
  std::set<live::INodePtr> remaining;
  std::vector<live::INodePtr> select_stack;
  std::map<live::INodePtr, std::string> assigned;

  for (auto node : live_graph_.interf_graph->Nodes()->GetList()) {
    if (Precolored(node)) {
      assigned[node] = *initial_->Look(node->NodeInfo());
    } else {
      remaining.insert(node);
    }
  }

  auto degree = [&](live::INodePtr node) {
    int result = 0;
    for (auto adj : node->Adj()->GetList()) {
      if (Precolored(adj) || remaining.count(adj))
        ++result;
    }
    return result;
  };

  while (!remaining.empty()) {
    auto chosen = remaining.end();
    for (auto it = remaining.begin(); it != remaining.end(); ++it) {
      if (degree(*it) < color_count) {
        chosen = it;
        break;
      }
    }

    if (chosen == remaining.end()) {
      chosen = std::max_element(
          remaining.begin(), remaining.end(),
          [&](live::INodePtr left, live::INodePtr right) {
            return degree(left) < degree(right);
          });
    }

    select_stack.push_back(*chosen);
    remaining.erase(chosen);
  }

  auto *coloring = temp::Map::Empty();
  auto *spills = new live::INodeList();

  for (auto it = select_stack.rbegin(); it != select_stack.rend(); ++it) {
    live::INodePtr node = *it;
    std::set<std::string> forbidden;
    for (auto adj : node->Adj()->GetList()) {
      auto color_it = assigned.find(adj);
      if (color_it != assigned.end())
        forbidden.insert(color_it->second);
    }

    bool colored = false;
    for (auto reg : registers_->GetList()) {
      std::string reg_name = *initial_->Look(reg);
      if (!forbidden.count(reg_name)) {
        assigned[node] = reg_name;
        coloring->Enter(node->NodeInfo(), new std::string(reg_name));
        colored = true;
        break;
      }
    }

    if (!colored)
      spills->Append(node);
  }

  return Result(coloring, spills);
}
} // namespace col
