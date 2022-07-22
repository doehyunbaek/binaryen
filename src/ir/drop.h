/*
 * Copyright 2022 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_ir_drop_h
#define wasm_ir_drop_h

#include "ir/branch-utils.h"
#include "ir/effects.h"
#include "ir/iteration.h"
#include "wasm-builder.h"
#include "wasm.h"

namespace wasm {

// Given an expression, returns a new expression that drops the given
// expression's children that cannot be removed outright due to their side
// effects. This is useful if we know the node is not needed but may need to
// keep the children around; this utility will automatically remove any children
// we do not actually need to keep, based on their effects.
//
// The caller must also pass in a last item to append to the output (which is
// typically what the original expression is replaced with).
inline Expression* getDroppedChildrenAndAppend(Expression* curr,
                                               Module& wasm,
                                               const PassOptions& options,
                                               Expression* last) {
  Builder builder(wasm);
  std::vector<Expression*> contents;
  for (auto* child : ChildIterator(curr)) {
    if (!EffectAnalyzer(options, wasm, child).hasUnremovableSideEffects()) {
      continue;
    }
    if (child->type.isConcrete()) {
      contents.push_back(builder.makeDrop(child));
    } else {
      // The child is unreachable, or none (none is possible as a child of a
      // block or loop, etc.); in both cases we do not need a drop.
      contents.push_back(child);
    }
  }
  contents.push_back(last);
  if (contents.size() == 1) {
    return contents[0];
  }
  return builder.makeBlock(contents);
}

// As the above, but only operates on children that execute unconditionally.
// That is the case in almost all expressions, except for those with
// conditional execution, like if, which unconditionally executes the condition
// but then conditionally executes one of the two arms. The above function
// simply returns all children in order, so it does this to if:
//
//  (if
//    (condition)
//    (arm-A)
//    (arm-B)
//  )
// =>
//  (drop
//    (condition)
//  )
//  (drop
//    (arm-A)
//  )
//  (drop
//    (arm-B)
//  )
//  (appended last item)
//
// This is dangerous as it executes what were conditional children in an
// unconditional way. To avoid that issue, this function will only operate on
// unconditional children, and keep conditional ones as they were. That means
// it will not split up and drop the children of an if, for example. All we do
// in that case is drop the entire if and append the last item:
//
//  (drop
//    (if
//      (condition)
//      (arm-A)
//      (arm-B)
//    )
//  )
//  (appended last item)
//
//  Also this function preserves other unremovable expressions like trys and
//  pops.
inline Expression*
getDroppedUnconditionalChildrenAndAppend(Expression* curr,
                                         Module& wasm,
                                         const PassOptions& options,
                                         Expression* last) {
  // We check for shallow effects here, since we may be able to remove |curr|
  // itself but keep its children around - we don't want effects in the children
  // to stop us from improving the code. Note that there are cases where the
  // combined curr+children has fewer effects than curr itself, such as if curr
  // is a block and the child branches to it, but in such cases we cannot remove
  // curr anyhow (those cases are ruled out below), so looking at non-shallow
  // effects would never help us (and would be slower to run).
  ShallowEffectAnalyzer effects(options, wasm, curr);
  // Ignore a trap, as the unreachable replacement would trap too.
  if (last->type == Type::unreachable) {
    effects.trap = false;
  }

  // We cannot remove
  // 1. Expressions with unremovable side effects
  // 2. if: 'if's contains conditional expressions
  // 3. try: Removing a try could leave a pop without a proper parent
  // 4. pop: Pops are struturally necessary in catch bodies
  // 5. Branch targets: We will need the target for the branches to it to
  //                    validate.
  if (effects.hasUnremovableSideEffects() || curr->is<If>() ||
      curr->is<Try>() || curr->is<Pop>() ||
      BranchUtils::getDefinedName(curr).is()) {
    Builder builder(wasm);
    return builder.makeSequence(builder.makeDrop(curr), last);
  }
  return getDroppedChildrenAndAppend(curr, wasm, options, last);
}

} // namespace wasm

#endif // wasm_ir_drop_h