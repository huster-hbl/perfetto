/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/proto/heap_graph_walker.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace trace_processor {
namespace {

uint64_t Gcd(uint64_t one, uint64_t other) {
  if (one == 1)
    return 1;
  if (other == 1)
    return 1;

  while (other != 0) {
    uint64_t tmp = other;
    other = one % other;
    one = tmp;
  }
  return one;
}

uint64_t Lcm(uint64_t one, uint64_t other) {
  if (one > other)
    return other * (one / Gcd(one, other));
  return one * (other / Gcd(one, other));
}

void AddChild(std::map<int64_t, int64_t>* component_to_node,
              uint64_t count,
              int64_t child_component_id,
              int64_t last_node_row) {
  if (count > 1) {
    // We have multiple edges from this component to the target component.
    // This cannot possibly be uniquely retained by one node in this
    // component.
    (*component_to_node)[child_component_id] = -1;
  } else {
    // Check if the node that owns grand_component via child_component_id
    // is the same as the node that owns it through all other
    // child_component_ids.
    auto it = component_to_node->find(child_component_id);
    if (it == component_to_node->end())
      (*component_to_node)[child_component_id] = last_node_row;
    else if (it->second != last_node_row)
      it->second = -1;
  }
}

bool IsUniqueOwner(const std::map<int64_t, int64_t>& component_to_node,
                   uint64_t count,
                   int64_t child_component_id,
                   int64_t last_node_row) {
  if (count > 1)
    return false;

  auto it = component_to_node.find(child_component_id);
  return it == component_to_node.end() || it->second == last_node_row;
}

}  // namespace

Fraction::Fraction(uint64_t numerator, uint64_t denominator)
    : numerator_(numerator), denominator_(denominator) {
  PERFETTO_CHECK(denominator_ > 0);
  Reduce();
}

void Fraction::Reduce() {
  if (numerator_ == 0) {
    denominator_ = 1;
    return;
  }

  if (numerator_ == 1)
    return;

  uint64_t new_gcd = Gcd(numerator_, denominator_);
  numerator_ /= new_gcd;
  denominator_ /= new_gcd;
  PERFETTO_CHECK(denominator_ > 0);
}

Fraction& Fraction::operator+=(const Fraction& other) {
  uint64_t new_denominator = Lcm(denominator_, other.denominator_);
  uint64_t new_numerator =
      numerator_ * (new_denominator / denominator_) +
      other.numerator_ * (new_denominator / other.denominator_);
  numerator_ = new_numerator;
  denominator_ = new_denominator;
  Reduce();
  return *this;
}

Fraction Fraction::operator*(const Fraction& other) {
  return Fraction(numerator_ * other.numerator_,
                  denominator_ * other.denominator_);
}

bool Fraction::operator==(uint64_t other) const {
  return numerator_ == denominator_ * other;
}

HeapGraphWalker::Delegate::~Delegate() = default;

void HeapGraphWalker::AddNode(int64_t row, uint64_t size) {
  if (static_cast<size_t>(row) >= nodes_.size())
    nodes_.resize(static_cast<size_t>(row) + 1);
  Node& node = GetNode(row);
  node.self_size = size;
  node.row = row;
}

void HeapGraphWalker::AddEdge(int64_t owner_row, int64_t owned_row) {
  GetNode(owner_row).children.emplace(&GetNode(owned_row));
  GetNode(owned_row).parents.emplace(&GetNode(owner_row));
}

void HeapGraphWalker::MarkRoot(int64_t row) {
  Node& n = GetNode(row);
  ReachableNode(&n);
  if (n.node_index == 0)
    FindSCC(&n);
}

void HeapGraphWalker::CalculateRetained() {
  for (Node& n : nodes_) {
    if (n.node_index == 0)
      FindSCC(&n);
  }

  // Sanity check that we have processed all edges.
  for (const auto& c : components_)
    PERFETTO_CHECK(c.incoming_edges == 0);
}

void HeapGraphWalker::ReachableNode(Node* node) {
  if (node->reachable)
    return;
  delegate_->MarkReachable(node->row);
  node->reachable = true;
  for (Node* child : node->children)
    ReachableNode(child);
}

int64_t HeapGraphWalker::RetainedSize(const Component& component) {
  int64_t retained_size = static_cast<int64_t>(component.unique_retained_size);
  for (const auto& p : component.children_components) {
    int64_t child_component_id = p.first;
    const Component& child_component =
        components_[static_cast<size_t>(child_component_id)];
    retained_size += child_component.unique_retained_size;
  }
  return retained_size;
}

void HeapGraphWalker::FoundSCC(Node* node) {
  // We have discovered a new connected component.
  int64_t component_id = static_cast<int64_t>(components_.size());
  components_.emplace_back();
  Component& component = components_.back();
  component.lowlink = node->lowlink;

  std::vector<Node*> component_nodes;

  // A struct representing all direct children from this component.
  struct DirectChild {
    // Number of edges from current component_id to this component.
    size_t edges_from_current_component = 0;
    // If edges_from_current_component == 1, this is the row of the node that
    // has an outgoing edge to it.
    int64_t last_node_row = 0;
  };
  std::map<int64_t, DirectChild> direct_children_rows;

  Node* stack_elem;
  do {
    stack_elem = node_stack_.back();
    component_nodes.emplace_back(stack_elem);
    node_stack_.pop_back();
    for (Node* child : stack_elem->children) {
      if (!child->on_stack) {
        // If the node is not on the stack, but is a child of a node on the
        // stack, it must have already been explored (and assigned a
        // component).
        PERFETTO_CHECK(child->component != -1);
        if (child->component != component_id) {
          DirectChild& dc = direct_children_rows[child->component];
          dc.edges_from_current_component++;
          dc.last_node_row = stack_elem->row;
        }
      }
      // If the node is on the stack, it must be part of this SCC and will be
      // handled by the loop.
      // This node being on the stack means there exist a path from it to the
      // current node. If it also is a child of this node, there is a loop.
    }
    stack_elem->on_stack = false;
    // A node can never be part of two components.
    PERFETTO_CHECK(stack_elem->component == -1);
    stack_elem->component = component_id;
  } while (stack_elem != node);

  for (Node* elem : component_nodes) {
    component.unique_retained_size += elem->self_size;
    for (Node* parent : elem->parents) {
      // We do not count intra-component edges.
      if (parent->component != component_id)
        component.incoming_edges++;
    }
    component.orig_incoming_edges = component.incoming_edges;
  }

  std::map<int64_t, int64_t> unique_retained_by_node;
  // Map from child component to node in this component that uniquely owns it,
  // or -1 if non-unique.
  std::map<int64_t, int64_t> component_to_node;
  for (const auto& p : direct_children_rows) {
    int64_t child_component_id = p.first;
    const DirectChild& dc = p.second;
    size_t count = dc.edges_from_current_component;
    PERFETTO_CHECK(child_component_id != component_id);

    AddChild(&component_to_node, count, child_component_id, dc.last_node_row);

    Component& child_component =
        components_[static_cast<size_t>(child_component_id)];

    child_component.incoming_edges -= count;
    for (auto& comp_p : child_component.children_components) {
      int64_t grand_component_id = comp_p.first;
      const Fraction& child_ownership_fraction = comp_p.second;
      const Component& grand_component =
          components_[static_cast<size_t>(grand_component_id)];
      AddChild(&component_to_node, count, child_component_id, dc.last_node_row);

      Fraction multiplier(count, child_component.orig_incoming_edges);
      component.children_components[grand_component_id] +=
          multiplier * child_ownership_fraction;
      if (component.children_components[grand_component_id] == 1) {
        component.unique_retained_size += grand_component.unique_retained_size;
        if (IsUniqueOwner(component_to_node, count, grand_component_id,
                          dc.last_node_row)) {
          unique_retained_by_node[dc.last_node_row] +=
              grand_component.unique_retained_size;
        }
        component.children_components.erase(grand_component_id);
      }
    }

    if (child_component.orig_incoming_edges == count) {
      // Has already been decremented above.
      PERFETTO_CHECK(child_component.incoming_edges == 0);
      component.unique_retained_size += child_component.unique_retained_size;
      if (count == 1) {
        unique_retained_by_node[dc.last_node_row] +=
            child_component.unique_retained_size;
      }
    } else {
      component.children_components[child_component_id] +=
          Fraction(count, child_component.orig_incoming_edges);

      if (component.children_components[child_component_id] == 1) {
        component.unique_retained_size += child_component.unique_retained_size;
        if (IsUniqueOwner(component_to_node, count, dc.last_node_row,
                          child_component_id)) {
          unique_retained_by_node[dc.last_node_row] +=
              child_component.unique_retained_size;
        }
        component.children_components.erase(child_component_id);
      }
    }

    if (child_component.incoming_edges == 0)
      child_component.children_components.clear();
  }

  int64_t retained_size = RetainedSize(component);
  for (Node* n : component_nodes) {
    int64_t unique_retained_size = 0;
    auto it = unique_retained_by_node.find(n->row);
    if (it != unique_retained_by_node.end())
      unique_retained_size = it->second;

    delegate_->SetRetained(
        n->row, static_cast<int64_t>(retained_size),
        static_cast<int64_t>(n->self_size) + unique_retained_size);
  }
}

void HeapGraphWalker::FindSCC(Node* node) {
  node->node_index = node->lowlink = next_node_index_++;
  node_stack_.push_back(node);
  node->on_stack = true;
  for (Node* child : node->children) {
    if (child->node_index == 0) {
      FindSCC(child);
      if (child->lowlink < node->lowlink)
        node->lowlink = child->lowlink;
    } else if (child->on_stack) {
      if (child->node_index < node->lowlink)
        node->lowlink = child->node_index;
    }
  }

  if (node->lowlink == node->node_index)
    FoundSCC(node);
}

}  // namespace trace_processor
}  // namespace perfetto