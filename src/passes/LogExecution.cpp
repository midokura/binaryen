/*
 * Copyright 2017 WebAssembly Community Group participants
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

//
// Instruments the build with code to log execution at each function
// entry, loop header, and return. This can be useful in debugging, to log out
// a trace, and diff it to another (running in another browser, to
// check for bugs, for example).
//
// The logging is performed by calling an ffi with an id for each
// call site. You need to provide that import on the JS side.
//
// This pass is more effective on flat IR (--flatten) since when it
// instruments say a return, there will be no code run in the return's
// value.
//
// A list of functions not to instrument can be provided with following pass
// argument. This list can be used with a response file (@filename, which is
// then loaded from the file).
//
//   --pass-arg=log-execution-ignorelist@name1,name2,name3
//

#include "asmjs/shared-constants.h"
#include "shared-constants.h"
#include "support/file.h"
#include "support/string.h"
#include <pass.h>
#include <wasm-binary.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

Name LOGGER("log_execution");

enum class LogKind {
  FunctionEntry,
  Return,
  LoopHeader,
  // Up to 4 entries fit in 2 bits.
  // If adding more entries, need to increase the kind bit size below.
};

union LogID {
  int32_t raw;
  struct {
    uint32_t id : 30;
    LogKind kind : 2;
  };
};
static_assert(sizeof(LogID) == sizeof(int32_t), "LogID must fit in 32-bits");

struct LogExecution : public WalkerPass<PostWalker<LogExecution>> {
  // The module name the logger function is imported from.
  IString loggerModule;
  uint32_t nextID = 0;
  std::set<Name> ignoreListNames;

  // Adds calls to new imports.
  bool addsEffects() override { return true; }

  void run(Module* module) override {
    auto& options = getPassOptions();
    loggerModule = options.getArgumentOrDefault("log-execution", "");

    auto ignoreListInput =
      options.getArgumentOrDefault("log-execution-ignorelist", "");
    String::Split ignoreList(
      String::trim(read_possible_response_file(ignoreListInput)),
      String::Split::NewLineOr(","));

    ignoreListNames.clear();
    for (auto& name : ignoreList) {
      ignoreListNames.insert(WasmBinaryReader::escape(name));
    }

    nextID = 0;
    super::run(module);
  }

  void visitLoop(Loop* curr) {
    curr->body = makeLogCall(curr->body, LogKind::LoopHeader);
  }

  void visitReturn(Return* curr) {
    replaceCurrent(makeLogCall(curr, LogKind::Return));
  }

  void visitFunction(Function* curr) {
    if (curr->imported()) {
      return;
    }
    if (auto* block = curr->body->dynCast<Block>()) {
      if (!block->list.empty()) {
        block->list.back() = makeLogCall(block->list.back(), LogKind::Return);
      }
    }
    curr->body = makeLogCall(curr->body, LogKind::FunctionEntry);
  }

  void walkFunction(Function* curr) {
    // If the function name is in the ignore list, don't walk the function or
    // its children so we don't insert log calls.
    bool ignore = ignoreListNames.count(curr->name) > 0;
    if (!ignore) {
      super::walkFunction(curr);
    }
  }

  void visitModule(Module* curr) {
    // Add the import
    auto import =
      Builder::makeFunction(LOGGER, Signature(Type::i32, Type::none), {});

    if (loggerModule != "") {
      import->module = loggerModule;
    } else {
      // Import the log function from import "env" if the module
      // imports other functions from that name.
      for (auto& func : curr->functions) {
        if (func->imported() && func->module == ENV) {
          import->module = func->module;
          break;
        }
      }

      // If not, then pick the import name of the first function we find.
      if (!import->module) {
        for (auto& func : curr->functions) {
          if (func->imported()) {
            import->module = func->module;
            break;
          }
        }
      }

      // If no function was found, use ENV.
      if (!import->module) {
        import->module = ENV;
      }
    }

    import->base = LOGGER;
    curr->addFunction(std::move(import));
  }

private:
  Expression* makeLogCall(Expression* curr, LogKind kind) {
    Builder builder(*getModule());
    LogID id;
    id.id = nextID++;
    id.kind = kind;
    return builder.makeSequence(
      builder.makeCall(LOGGER, {builder.makeConst(id.raw)}, Type::none), curr);
  }
};

Pass* createLogExecutionPass() { return new LogExecution(); }

} // namespace wasm
