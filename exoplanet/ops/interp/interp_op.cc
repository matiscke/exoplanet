#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/platform/default/logging.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/util/work_sharder.h"

#include <algorithm>

using namespace tensorflow;

REGISTER_OP("Interp")
  .Attr("T: {float, double}")
  .Input("t: T")
  .Input("p: T")
  .Input("x: T")
  .Input("y: T")
  .Output("v: T")
  .Output("a: T")
  .Output("inds: int64")
  .SetShapeFn([](shape_inference::InferenceContext* c) {
    shape_inference::ShapeHandle t, p, x, y;
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(0), 1, &t));
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(1), 1, &p));
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(2), 1, &x));
    TF_RETURN_IF_ERROR(c->WithRankAtLeast(c->input(3), 1, &y));

    TF_RETURN_IF_ERROR(c->Merge(x, y, &x));

    TF_RETURN_IF_ERROR(c->Subshape(t, 0, -1, &t));
    TF_RETURN_IF_ERROR(c->Subshape(x, 0, -1, &x));
    TF_RETURN_IF_ERROR(c->Merge(t, x, &t));
    TF_RETURN_IF_ERROR(c->Merge(t, p, &t));

    c->set_output(0, c->input(0));
    c->set_output(1, c->input(0));
    c->set_output(2, c->input(0));
    return Status::OK();
  });

// Adapted from https://academy.realm.io/posts/how-we-beat-cpp-stl-binary-search/
template <typename T>
inline int64 search_sorted (int64 N, const T* const x, const T& value) {
  int64 low = -1;
  int64 high = N;
  while (high - low > 1) {
    int64 probe = (low + high) / 2;
    T v = x[probe];
    if (v > value)
      high = probe;
    else
      low = probe;
  }
  return high;
}

template <typename T>
class InterpOp : public OpKernel {
 public:
  explicit InterpOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    // Inputs
    const Tensor& t_tensor = context->input(0);
    const Tensor& p_tensor = context->input(1);
    const Tensor& x_tensor = context->input(2);
    const Tensor& y_tensor = context->input(3);

    // Check that the dimensions are consistent
    int64 ndim = t_tensor.dims();
    OP_REQUIRES(context, ndim >= 1,
        errors::InvalidArgument("t must be at least 1D"));
    OP_REQUIRES(context, p_tensor.dims() == ndim - 1,
        errors::InvalidArgument("p must have the dimension len(t.shape) - 1"));
    OP_REQUIRES(context, x_tensor.dims() == ndim,
        errors::InvalidArgument("x and t must have the same number of dimensions"));
    OP_REQUIRES(context, y_tensor.shape() == x_tensor.shape(),
        errors::InvalidArgument("x and y must be the same shape"));

    // Compute the full size of the inner dimensions
    int64 size = 1;
    for (int64 k = 0; k < ndim - 1; ++k) {
      int64 dim = t_tensor.dim_size(k);
      size *= dim;
      OP_REQUIRES(context, (x_tensor.dim_size(k) == dim && p_tensor.dim_size(k) == dim),
          errors::InvalidArgument("incompatible dimensions"));
    }

    // The outer dimensions
    const int64 N = t_tensor.dim_size(ndim - 1);
    const int64 M = x_tensor.dim_size(ndim - 1);

    // Output
    Tensor* v_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, t_tensor.shape(), &v_tensor));
    Tensor* a_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(1, t_tensor.shape(), &a_tensor));
    Tensor* inds_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(2, t_tensor.shape(), &inds_tensor));

    // Access the data
    const auto t = t_tensor.template flat_inner_dims<T, 2>();
    const auto p = p_tensor.template flat_inner_dims<T, 1>();
    const auto x = x_tensor.template flat_inner_dims<T, 2>();
    const auto y = y_tensor.template flat_inner_dims<T, 2>();
    auto v       = v_tensor->template flat_inner_dims<T, 2>();
    auto a       = a_tensor->template flat_inner_dims<T, 2>();
    auto inds    = inds_tensor->flat_inner_dims<int64, 2>();

    for (int64 k = 0; k < size; ++k) {
      T period = p(k);
      bool flag = (period > T(0));

      const T* const tk = &(t(k, 0));
      const T* const xk = &(x(k, 0));
      const T* const yk = &(y(k, 0));
      T* vk = &(v(k, 0));
      T* ak = &(a(k, 0));
      int64* indsk = &(inds(k, 0));

      auto work = [flag, M, period, &tk, &xk, &yk, &vk, &ak, &indsk](int64 begin, int64 end) {
        for (int64 n = begin; n < end; ++n) {
          // Wrap into the required period
          T value = tk[n];
          if (flag) {
            T trunc_mod = std::fmod(value, period);
            value = (value >= T(0)) ? trunc_mod : std::fmod(trunc_mod + period, period);
          }

          if (value <= xk[0]) {
            vk[n] = yk[0];
            indsk[n] = 0;
          } else if (value >= xk[M-1]) {
            vk[n] = yk[M-1];
            indsk[n] = M+1;
          } else {
            int64 ind = indsk[n] = search_sorted(M, xk, value);
            T a0 = ak[n] = (value - xk[ind-1]) / (xk[ind] - xk[ind-1]);
            vk[n] = a0 * yk[ind] + (1.0 - a0) * yk[ind-1];
          }
        }
      };

      auto worker_threads = *context->device()->tensorflow_cpu_worker_threads();
      int64 cost = 5*M;
      Shard(worker_threads.num_threads, worker_threads.workers, N, cost, work);
      //work(0, size);
    }
  }
};


#define REGISTER_KERNEL(type)                                              \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("Interp").Device(DEVICE_CPU).TypeConstraint<type>("T"),         \
      InterpOp<type>)

REGISTER_KERNEL(float);
REGISTER_KERNEL(double);

#undef REGISTER_KERNEL