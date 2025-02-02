#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/operators/conv2d.h>
#include <torch/csrc/jit/tensorexpr/operators/misc.h>
#include <torch/csrc/jit/tensorexpr/tensor.h>

namespace torch {
namespace jit {
namespace tensorexpr {

namespace {

void assert_dims_constant(const BufHandle& buf) {
  for (auto const& dim : buf.node()->dims()) {
    TORCH_INTERNAL_ASSERT(dim->isConstant());
  }
}

using InitFunc = std::function<ExprHandle(const std::vector<VarHandle>&)>;

Tensor conv2d_depthwise_static(
    BufHandle input,
    BufHandle weight,
    const InitFunc& init_func,
    int stride,
    int pad,
    int groups) {
  TORCH_INTERNAL_ASSERT(input.ndim() == 4);
  TORCH_INTERNAL_ASSERT(weight.ndim() == 4);

  assert_dims_constant(input);
  assert_dims_constant(weight);

  auto const& N = immediateAs<int>(input.dim(0));
  auto const& C = immediateAs<int>(input.dim(1));
  auto const& H = immediateAs<int>(input.dim(2));
  auto const& W = immediateAs<int>(input.dim(3));

  auto const& K = immediateAs<int>(weight.dim(0));
  auto const& CperG = immediateAs<int>(weight.dim(1));
  auto const& R = immediateAs<int>(weight.dim(2));
  auto const& S = immediateAs<int>(weight.dim(3));

  TORCH_INTERNAL_ASSERT(C == K && K == groups && CperG == 1);
  TORCH_INTERNAL_ASSERT(R == S);

  auto OH = (H - R + 2 * pad) / stride + 1;
  auto OW = (W - S + 2 * pad) / stride + 1;

  Tensor conv = Reduce(
      "conv2d_depthwise",
      {{N, "n"}, {K, "k"}, {OH, "oh"}, {OW, "ow"}},
      Sum(),
      [&](const std::vector<VarHandle>& v) { return init_func(v); },
      [&](const std::vector<VarHandle>& v) {
        auto const& n = v[0];
        auto const& k = v[1];
        auto const& oh = v[2];
        auto const& ow = v[3];
        auto const& c = v[4];
        auto const& r = v[5];
        auto const& s = v[6];
        auto cond = CompareSelect::make(oh * stride - pad + r, 0, 1, 0, kLT);
        cond = CompareSelect::make(ow * stride - pad + s, 0, 1, cond, kLT);
        cond = CompareSelect::make(oh * stride - pad + r, H, 1, cond, kGE);
        cond = CompareSelect::make(ow * stride - pad + s, W, 1, cond, kGE);
        auto in = ifThenElse(
            cond,
            0.f,
            input.load(n, k, oh * stride - pad + r, ow * stride - pad + s));
        return in * weight.load(k, c, r, s);
      },
      {{C / groups, "c"}, {R, "r"}, {S, "s"}});

  LoopNest nest({conv});

  constexpr int kLoopH = 2, kLoopW = 3;
  if (R == 3 && stride == 2 && pad == 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    ForPtr head, tail;
    auto loops = nest.getLoopStmtsFor(conv);
    nest.sliceHead(loops[kLoopW], 2, &head, &tail);
    loops = nest.getLoopStmtsFor(conv);
    nest.sliceHead(loops[kLoopH], 2, &head, &tail);
  } else if (R == 3 && stride == 1 && pad == 1) {
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    ForPtr main, peeled;
    auto loops = nest.getAllLoopNestsWritingToBuf(conv.buf());
    main = loops[1][kLoopW];
    nest.sliceHead(main, 1, &peeled, &main);
    nest.sliceTail(main, 1, &main, &peeled);
    main = LoopNest::getParentLoop(main);
    nest.sliceHead(main, 1, &peeled, &main);
    nest.sliceTail(main, 1, &main, &peeled);
  }

  return Tensor(conv.buf(), nest.root_stmt());
}

Tensor conv2d_depthwise_dynamic(
    BufHandle input,
    BufHandle weight,
    const InitFunc& init_func,
    ExprHandle N,
    ExprHandle C,
    ExprHandle H,
    ExprHandle W,
    ExprHandle K,
    ExprHandle CperG,
    ExprHandle R,
    ExprHandle S,
    ExprHandle stride,
    ExprHandle pad,
    ExprHandle groups) {
  TORCH_INTERNAL_ASSERT(input.ndim() == 4);
  TORCH_INTERNAL_ASSERT(weight.ndim() == 4);

  auto OH = (H - R + pad * 2) / stride + 1;
  auto OW = (W - S + pad * 2) / stride + 1;

  return Reduce(
      "conv2d_depthwise",
      {{N, "n"}, {K, "k"}, {OH, "oh"}, {OW, "ow"}},
      Sum(),
      [&](const std::vector<VarHandle>& v) { return init_func(v); },
      [&](const std::vector<VarHandle>& v) {
        auto const& n = v[0];
        auto const& k = v[1];
        auto const& oh = v[2];
        auto const& ow = v[3];
        auto const& c = v[4];
        auto const& r = v[5];
        auto const& s = v[6];
        auto cond = CompareSelect::make(oh * stride - pad + r, 0, 1, 0, kLT);
        cond = CompareSelect::make(ow * stride - pad + s, 0, 1, cond, kLT);
        cond = CompareSelect::make(oh * stride - pad + r, H, 1, cond, kGE);
        cond = CompareSelect::make(ow * stride - pad + s, W, 1, cond, kGE);
        auto in = ifThenElse(
            cond,
            0.f,
            input.load(n, k, oh * stride - pad + r, ow * stride - pad + s));
        return in * weight.load(k, c, r, s);
      },
      {{C / groups, "c"}, {R, "r"}, {S, "s"}});
}

} // namespace

Tensor conv2d_depthwise(
    BufHandle input,
    BufHandle weight,
    BufHandle bias,
    int stride,
    int pad,
    int groups) {
  assert_dims_constant(bias);
  auto init_func = [&](const std::vector<VarHandle>& v) {
    return bias.load(v[1]);
  };
  return conv2d_depthwise_static(input, weight, init_func, stride, pad, groups);
}

Tensor conv2d_depthwise(
    BufHandle input,
    BufHandle weight,
    int stride,
    int pad,
    int groups) {
  auto init_func = [](const std::vector<VarHandle>& v) {
    return ExprHandle(Sum().initializer());
  };
  return conv2d_depthwise_static(input, weight, init_func, stride, pad, groups);
}

Tensor conv2d_depthwise(
    BufHandle input,
    BufHandle weight,
    BufHandle bias,
    ExprHandle N,
    ExprHandle C,
    ExprHandle H,
    ExprHandle W,
    ExprHandle K,
    ExprHandle CperG,
    ExprHandle R,
    ExprHandle S,
    ExprHandle stride,
    ExprHandle pad,
    ExprHandle groups) {
  assert_dims_constant(bias);
  auto init_func = [&](const std::vector<VarHandle>& v) {
    return bias.load(v[1]);
  };
  return conv2d_depthwise_dynamic(
      input,
      weight,
      init_func,
      N,
      C,
      H,
      W,
      K,
      CperG,
      R,
      S,
      stride,
      pad,
      groups);
}

Tensor conv2d_depthwise(
    BufHandle input,
    BufHandle weight,
    ExprHandle N,
    ExprHandle C,
    ExprHandle H,
    ExprHandle W,
    ExprHandle K,
    ExprHandle CperG,
    ExprHandle R,
    ExprHandle S,
    ExprHandle stride,
    ExprHandle pad,
    ExprHandle groups) {
  auto init_func = [](const std::vector<VarHandle>& v) {
    return ExprHandle(Sum().initializer());
  };
  return conv2d_depthwise_dynamic(
      input,
      weight,
      init_func,
      N,
      C,
      H,
      W,
      K,
      CperG,
      R,
      S,
      stride,
      pad,
      groups);
}

std::vector<int64_t> _pair_int(ArgValue v) {
  if (auto t = c10::get_if<IntList>(&v)) {
    return {(*t)[0], (*t)[1]};
  }
  auto i = c10::get<int64_t>(v);
  return {i, i};
}

bool conv2dIsSupported(
    const TensorInfo& input,
    const TensorInfo& weight,
    const TensorInfo& bias,
    const std::vector<int64_t>& stride,
    const std::vector<int64_t>& pad,
    const std::vector<int64_t>& dilation,
    int64_t groups) {
  if (input.dtype != c10::ScalarType::Float ||
      weight.dtype != c10::ScalarType::Float ||
      bias.dtype != c10::ScalarType::Float) {
    GRAPH_DEBUG("conv2dIsSupported: only float32 allowed");
    return false;
  }
  if (input.dims.size() != 4 || weight.dims.size() != 4 ||
      bias.dims.size() != 1) {
    GRAPH_DEBUG("conv2dIsSupported: inputs are the wrong size");
    return false;
  }
  auto Cin = input.dims[1];
  auto Cout = weight.dims[0];
  auto CperG = weight.dims[1];
  if (Cin != Cout || Cin != groups || CperG != 1) {
    GRAPH_DEBUG("conv2dIsSupported: not depthwise");
    return false;
  }
  auto KH = weight.dims[2];
  auto KW = weight.dims[3];
  if (KH != 3 || KW != 3) {
    GRAPH_DEBUG("conv2dIsSupported: not 3x3");
    return false;
  }
  if (stride.size() != 2 || stride[0] != stride[1]) {
    GRAPH_DEBUG("conv2dIsSupported: unsupported stride");
    return false;
  }
  if (pad.size() != 2 || pad[0] != pad[1]) {
    GRAPH_DEBUG("conv2dIsSupported: unsupported pad");
    return false;
  }
  if (dilation.size() != 2 || dilation[0] != 1 || dilation[1] != 1) {
    GRAPH_DEBUG("conv2dIsSupported: unsupported dilation");
    return false;
  }
  return true;
}

Tensor computeConv2d(
    const std::vector<ArgValue>& inputs,
    const std::vector<ExprHandle>& outputShape,
    const c10::optional<ScalarType>& outputType,
    at::Device device) {
  Dtype dtype = kFloat;
  if (outputType) {
    dtype = Dtype(*outputType);
  }

  BufHandle ResultBuf("conv", outputShape, dtype);
  const BufHandle& inp = c10::get<BufHandle>(inputs[0]);
  const BufHandle& w = c10::get<BufHandle>(inputs[1]);
  const BufHandle& b = c10::get<BufHandle>(inputs[2]);

  auto strides = _pair_int(inputs[3]);
  auto padding = _pair_int(inputs[4]);
  auto dilation = _pair_int(inputs[5]);

  int groups = c10::get<int64_t>(inputs[6]);

  auto inpInfo = getTensorInfo(inp);
  auto wInfo = getTensorInfo(w);
  auto bInfo = getTensorInfo(b);
  // Generate TE for depthwise convolutions.
  if (inpInfo && wInfo && bInfo &&
      conv2dIsSupported(
          *inpInfo, *wInfo, *bInfo, strides, padding, dilation, groups)) {
    return conv2d_depthwise(inp, w, b, strides[0], padding[0], groups);
  }

  // Once we have a performant TE representation for conv2d, we could use it
  // here instead of the external call!
  StmtPtr s = ExternalCall::make(
      ResultBuf,
      "nnc_aten_conv2d",
      {inp, w, b},
      {strides[0],
       strides[1],
       padding[0],
       padding[1],
       dilation[0],
       dilation[1],
       groups});
  return Tensor(ResultBuf.node(), s);
}

Tensor computePrepackedConv2dClampRun(
    const std::vector<ArgValue>& inputs,
    const std::vector<ExprHandle>& outputShape,
    const c10::optional<ScalarType>& outputType,
    at::Device device) {
  Dtype dtype = kFloat;
  if (outputType) {
    dtype = Dtype(*outputType);
  }

  BufHandle ResultBuf("prepacked_conv2d_clamp_run", outputShape, dtype);
  const BufHandle& inp = c10::get<BufHandle>(inputs[0]);
  const BufHandle& prepacked = c10::get<BufHandle>(inputs[1]);
  StmtPtr s = ExternalCall::make(
      ResultBuf, "nnc_prepacked_conv2d_clamp_run", {inp, prepacked}, {});
  return Tensor(ResultBuf.node(), s);
}

Tensor computePrepackedLinearClampRun(
    const std::vector<ArgValue>& inputs,
    const std::vector<ExprHandle>& outputShape,
    const c10::optional<ScalarType>& outputType,
    at::Device device) {
  Dtype dtype = kFloat;
  if (outputType) {
    dtype = Dtype(*outputType);
  }

  BufHandle ResultBuf("prepacked_linear_clamp_run", outputShape, dtype);
  const BufHandle& inp = c10::get<BufHandle>(inputs[0]);
  const BufHandle& prepacked = c10::get<BufHandle>(inputs[1]);
  StmtPtr s = ExternalCall::make(
      ResultBuf, "nnc_prepacked_linear_clamp_run", {inp, prepacked}, {});
  return Tensor(ResultBuf.node(), s);
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch
