#ifndef CAFFE2_OPERATORS_LENGTHS_REDUCER_FUSED_8BIT_ROWWISE_OPS_H_
#define CAFFE2_OPERATORS_LENGTHS_REDUCER_FUSED_8BIT_ROWWISE_OPS_H_

#include "caffe2/core/context.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/operator.h"
#include "caffe2/operators/fused_rowwise_8bit_conversion_ops.h"
#include "caffe2/operators/reducer_functors.h"
#include "caffe2/perfkernels/fused_8bit_rowwise_embedding_lookup.h"
#include "caffe2/utils/math.h"
#include "fbgemm/Fbgemm.h"

namespace caffe2 {

template <class Context, bool with_weights = 0, bool is_mean = 0>
class SparseLengthsFused8BitRowwiseOp : public Operator<Context> {
 public:
  static_assert(
      !(with_weights && is_mean),
      "Cannot have with_weights and is_mean a the same time");

  USE_OPERATOR_CONTEXT_FUNCTIONS;
  USE_SIMPLE_CTOR_DTOR(SparseLengthsFused8BitRowwiseOp)

  bool RunOnDevice() override {
    return DispatchHelper<TensorTypes<int32_t, int64_t>>::call(
        this, Input(INDICES));
  }

  template <typename IndexType>
  bool DoRunWithType() {
    const auto& data = Input(DATA);
    const auto& indices = Input(INDICES);
    const auto& lengths = Input(LENGTHS);

    CAFFE_ENFORCE_EQ(indices.dim(), 1, "INDICES must be a vector");
    CAFFE_ENFORCE_EQ(lengths.dim(), 1, "LENGTHS must be a vector");

    const float* weights = nullptr;
    if (with_weights) {
      const auto& weights_input = Input(WEIGHTS);
      CAFFE_ENFORCE_EQ(weights_input.dim(), 1, "WEIGHTS must be a vector");
      CAFFE_ENFORCE_EQ(
          weights_input.numel(),
          indices.numel(),
          "WEIGHTS should have the same length as INDICES.");
      weights = weights_input.template data<float>();
    }

    CAFFE_ENFORCE_GT(data.size(1), 8, "DATA must have more than 8 columns");
    // Subtract 8 from the #columns of data for the 4 bytes for scale and 4
    // bytes for bias that we use in the fused representation (per row).
    const std::vector<int64_t> shape = {lengths.size(0), data.size(1) - 8};
    auto* output = Output(0, shape, at::dtype<float>());

    // Calling the JITed kernel from FBGEMM
    // Will Remove the call to C2/perfkernels/
    bool success = fbgemm::EmbeddingSpMDM<std::uint8_t, IndexType>(
        /*block_size=*/output->size(1),
        /*output_size=*/output->size(0),
        /*index_size=*/indices.numel(),
        /*data_size=*/data.size(0),
        /*input=*/data.template data<uint8_t>(),
        /*indices=*/indices.template data<IndexType>(),
        /*lengths=*/lengths.template data<int>(),
        /*weights=*/weights,
        /*normalize_by_lengths=*/is_mean,
        /*out=*/output->template mutable_data<float>(),
        /*prefetch distance*/ 16);

    if (success) {
      return true;
    }

    int64_t current = 0;
    auto output_size = output->size(0);
    auto lengths_ = lengths.template data<IndexType>();
    auto index_size = indices.numel();
    auto indices_ = indices.template data<IndexType>();
    auto data_size = data.size(0);

    for (int m = 0; m < output_size; ++m) {
      for (int i = 0; i < lengths_[m]; ++i) {
        CAFFE_ENFORCE_LT(current, index_size);
        IndexType idx = indices_[current];
        CAFFE_ENFORCE(
            0 <= idx && idx < data_size,
            "Index ",
            current,
            " is out of bounds: ",
            idx,
            ", range 0 to ",
            data_size);
        ++current;
      }
    }
    CAFFE_ENFORCE_EQ(
        current,
        index_size,
        "Your input seems to be incorrect: the sum of lengths values should be "
        "the size of the indices tensor, but it appears not.");

    return true;
  }

  enum {
    DATA = 0,
    WEIGHTS = 1,
    INDICES = 1 + with_weights,
    LENGTHS = 2 + with_weights,
  };
};

} // namespace caffe2

#endif // CAFFE2_OPERATORS_LENGTHS_REDUCER_FUSED_8BIT_ROWWISE_OPS_H_
