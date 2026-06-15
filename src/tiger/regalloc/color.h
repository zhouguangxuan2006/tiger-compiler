#ifndef TIGER_COMPILER_COLOR_H
#define TIGER_COMPILER_COLOR_H

#include "tiger/codegen/assem.h"
#include "tiger/frame/temp.h"
#include "tiger/liveness/liveness.h"
#include "tiger/util/graph.h"

namespace col {
struct Result {
  Result() : coloring(nullptr), spills(nullptr) {}
  Result(temp::Map *coloring, live::INodeListPtr spills)
      : coloring(coloring), spills(spills) {}
  temp::Map *coloring;
  live::INodeListPtr spills;
};

class Color {
public:
  Color(live::LiveGraph live_graph, temp::Map *initial,
        temp::TempList *registers)
      : live_graph_(live_graph), initial_(initial), registers_(registers) {}

  Result Coloring();

private:
  live::LiveGraph live_graph_;
  temp::Map *initial_;
  temp::TempList *registers_;

  bool Precolored(live::INodePtr node) const;
};
} // namespace col

#endif // TIGER_COMPILER_COLOR_H
