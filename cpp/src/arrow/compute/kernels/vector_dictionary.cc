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

#include "arrow/array/array_base.h"
#include "arrow/array/array_dict.h"
#include "arrow/array/builder_primitive.h"
#include "arrow/array/dict_internal.h"
#include "arrow/compute/api_vector.h"
#include "arrow/compute/kernels/common_internal.h"
#include "arrow/result.h"

namespace arrow {

namespace compute {
namespace internal {

namespace {

// Dictionary compaction implementation

const FunctionDoc dictionary_compact_doc{
    "Compact dictionary array",
    ("Return a compacted version of the dictionary array input,\n"
     "which removes unused values in dictionary.\n"),
    {"dictionary_array"}};

class DictionaryCompactKernel : public KernelState {
 public:
  virtual Result<std::shared_ptr<Array>> Exec(std::shared_ptr<Array> dict_array,
                                              ExecContext* ctx) const = 0;
};

template <typename IndexArrowType>
class DictionaryCompactKernelImpl : public DictionaryCompactKernel {
  using BuilderType = NumericBuilder<IndexArrowType>;
  using CType = typename IndexArrowType::c_type;

 public:
  Result<std::shared_ptr<Array>> Exec(std::shared_ptr<Array> dict_array,
                                      ExecContext* ctx) const override {
    const DictionaryArray& casted_dict_array =
        checked_cast<const DictionaryArray&>(*dict_array);
    const std::shared_ptr<Array>& dict = casted_dict_array.dictionary();
    if (dict->length() == 0) {
      return dict_array;
    }
    const std::shared_ptr<Array>& indices = casted_dict_array.indices();
    if (indices->length() == 0) {
      ARROW_ASSIGN_OR_RAISE(auto empty_dict,
                            MakeEmptyArray(dict->type(), ctx->memory_pool()));
      return DictionaryArray::FromArrays(dict_array->type(), indices, empty_dict);
    }
    const CType* indices_data = indices->data()->GetValues<CType>(1);

    // check whether the input is compacted
    std::vector<bool> dict_used(dict->length(), false);
    int64_t dict_used_count = 0;
    for (int64_t i = 0; i < indices->length(); i++) {
      if (indices->IsNull(i)) {
        continue;
      }

      CType current_index = indices_data[i];
      if (current_index < 0 || current_index >= dict->length()) {
        return Status::IndexError("indice out of bound:", current_index);
      }
      if (!dict_used[current_index]) {
        dict_used[current_index] = true;
        dict_used_count++;

        if (dict_used_count == dict->length()) {  // input is already compacted
          return dict_array;
        }
      }
    }

    // dictionary compaction
    if (dict_used_count == 0) {
      ARROW_ASSIGN_OR_RAISE(auto empty_dict,
                            MakeEmptyArray(dict->type(), ctx->memory_pool()));
      return DictionaryArray::FromArrays(dict_array->type(), indices, empty_dict);
    }
    BuilderType dict_indices_builder(ctx->memory_pool());
    bool need_change_indice = false;
    CType len = static_cast<CType>(dict->length());
    for (CType i = 0; i < len; i++) {
      if (dict_used[i]) {
        ARROW_RETURN_NOT_OK(dict_indices_builder.Append(i));
      } else if (i + 1 < len && dict_used[i + 1]) {
        need_change_indice = true;
      }
    }
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> compacted_dict_indices,
                          dict_indices_builder.Finish());
    ARROW_ASSIGN_OR_RAISE(
        auto compacted_dict_res,
        Take(dict, compacted_dict_indices, TakeOptions::NoBoundsCheck(), ctx));
    std::shared_ptr<arrow::Array> compacted_dict = compacted_dict_res.make_array();

    // indices changes
    if (!need_change_indice) {
      return DictionaryArray::FromArrays(dict_array->type(), indices, compacted_dict);
    }
    std::vector<CType> indice_minus_number(dict->length(), 0);
    if (!dict_used[0]) {
      indice_minus_number[0] = 1;
    }
    for (int64_t i = 1; i < dict->length(); i++) {
      indice_minus_number[i] = indice_minus_number[i - 1];
      if (!dict_used[i]) {
        indice_minus_number[i] = indice_minus_number[i] + 1;
      }
    }

    BuilderType indices_builder(ctx->memory_pool());
    for (int64_t i = 0; i < indices->length(); i++) {
      if (indices->IsNull(i)) {
        ARROW_RETURN_NOT_OK(indices_builder.AppendNull());
      } else {
        CType current_index = indices_data[i];
        ARROW_RETURN_NOT_OK(
            indices_builder.Append(current_index - indice_minus_number[current_index]));
      }
    }
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> changed_indice,
                          indices_builder.Finish());

    return DictionaryArray::FromArrays(dict_array->type(), changed_indice,
                                       compacted_dict);
  }
};

Result<std::unique_ptr<KernelState>> DictionaryCompactInit(KernelContext* ctx,
                                                           const KernelInitArgs& args) {
  const auto& dict_type =
      checked_cast<const DictionaryType&>(*(args.inputs[0].owned_type));
  switch (dict_type.index_type()->id()) {
    case Type::UINT8:
      return std::make_unique<DictionaryCompactKernelImpl<UInt8Type>>();
    case Type::INT8:
      return std::make_unique<DictionaryCompactKernelImpl<Int8Type>>();
    case Type::UINT16:
      return std::make_unique<DictionaryCompactKernelImpl<UInt16Type>>();
    case Type::INT16:
      return std::make_unique<DictionaryCompactKernelImpl<Int16Type>>();
    case Type::UINT32:
      return std::make_unique<DictionaryCompactKernelImpl<UInt32Type>>();
    case Type::INT32:
      return std::make_unique<DictionaryCompactKernelImpl<Int32Type>>();
    case Type::UINT64:
      return std::make_unique<DictionaryCompactKernelImpl<UInt64Type>>();
    case Type::INT64:
      return std::make_unique<DictionaryCompactKernelImpl<Int64Type>>();
    default:
      ARROW_CHECK(false) << "unreachable";
      return Status::TypeError("Expected an Indice Type of Int or UInt");
  }
}

Status DictionaryCompactExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  if (batch[0].is_scalar()) {
    return Status::TypeError("Expected an Array or a Chunked Array");
  }

  const DictionaryCompactKernel& Kernel =
      checked_cast<const DictionaryCompactKernel&>(*ctx->state());
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Array> compacted_dict_array,
                        Kernel.Exec(batch[0].array.ToArray(), ctx->exec_context()));
  out->value = compacted_dict_array->data();
  return Status::OK();
}
}  // namespace

void RegisterVectorDictionary(FunctionRegistry* registry) {
  VectorKernel base;
  base.init = DictionaryCompactInit;
  base.exec = DictionaryCompactExec;
  base.signature = KernelSignature::Make({Type::DICTIONARY}, FirstType);

  auto dictionary_compact = std::make_shared<VectorFunction>(
      "dictionary_compact", Arity::Unary(), dictionary_compact_doc);
  DCHECK_OK(dictionary_compact->AddKernel(base));
  DCHECK_OK(registry->AddFunction(std::move(dictionary_compact)));
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
