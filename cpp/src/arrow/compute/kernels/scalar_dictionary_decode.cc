// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/compute/cast.h"

#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "arrow/compute/cast.h"
#include "arrow/compute/cast_internal.h"
#include "arrow/compute/exec.h"
#include "arrow/compute/function_internal.h"
#include "arrow/compute/kernel.h"
#include "arrow/compute/kernels/codegen_internal.h"
#include "arrow/compute/registry.h"
#include "arrow/util/logging.h"
#include "arrow/util/reflection_internal.h"

namespace arrow {

using internal::ToTypeName;

namespace compute {
namespace internal {

const FunctionDoc dictionary_decode_doc{"decode a dictionary array to normal array",
                                        ("The input must be a dictionary encoded array."
                                         "The function decode a dictionary array to "
                                         "normal array, which is implemented by cast"),
                                        {"dictionary_array"}};
class DictionaryDecodeMetaFunction : public MetaFunction {
 public:
  DictionaryDecodeMetaFunction()
      : MetaFunction("dictionary_decode", Arity::Unary(), dictionary_decode_doc) {}

  Result<Datum> ExecuteImpl(const std::vector<Datum>& args,
                            const FunctionOptions* options,
                            ExecContext* ctx) const override {
    if (args[0].type() == nullptr || args[0].type()->id() != Type::DICTIONARY) {
      return Status::Invalid("Invalid input type for function 'dictonary decode': ",
                             args[0].ToString());
    }
    CastOptions castOption(true);  // safe cast
    if (args[0].is_array()) {
      ARROW_CHECK_NE(args[0].array()->dictionary, nullptr);
      TypeHolder to_type(args[0].array()->dictionary->type);
      castOption.to_type = to_type;
      return CallFunction("cast", args, &castOption, ctx);
    } else if (args[0].is_chunked_array()) {
      ARROW_CHECK_NE(args[0].chunked_array()->chunk(0), nullptr);
      ARROW_CHECK_NE(args[0].chunked_array()->chunk(0)->data(), nullptr);
      ARROW_CHECK_NE(args[0].chunked_array()->chunk(0)->data()->dictionary, nullptr);
      TypeHolder to_type(args[0].chunked_array()->chunk(0)->data()->dictionary->type);
      castOption.to_type = to_type;
      return CallFunction("cast", args, &castOption, ctx);
    } else {
      return Status::Invalid("Invalid input type for function 'dictonary decode': ",
                             args[0].ToString());
    }
  }
};
}  // namespace internal

}  // namespace compute
}  // namespace arrow