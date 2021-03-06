﻿#include <pybind11/pybind11.h>
#include <pybind11/buffer_info.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <string>
#include <regex>
#include <algorithm>
#include "triton/runtime/function.h"
#include "triton/runtime/arg.h"
#include "triton/lang/code_gen.h"
#include "triton/lang/parser.h"
#include "triton/lang/cpp.h"
#include "triton/driver/device.h"
#include "triton/driver/stream.h"
#include "triton/driver/kernel.h"
#include "triton/driver/module.h"
#include "triton/ir/module.h"
#include "triton/ir/function.h"
#include "triton/tools/bench.hpp"

using namespace triton;

namespace rt = triton::runtime;

typedef std::pair<size_t, size_t> map_key_t;
std::map<map_key_t, std::shared_ptr<rt::function::grid_fn_ty>> id_grid_map;
std::map<map_key_t, std::shared_ptr<rt::function>> id_fn_map;
std::map<size_t, double> fp64scalar_map;
std::map<size_t, int64_t> i64scalar_map;

/* Grid map */

void register_grid(const map_key_t& key,
                   const rt::function::grid_fn_ty& grid_fn) {
  id_grid_map[key].reset(new rt::function::grid_fn_ty(grid_fn));
}

void delete_grid(const map_key_t& key) {
  id_grid_map.erase(key);
}

/* Function map */

void register_fn(const map_key_t& key,
                 const std::string& src,
                 const rt::function::options_space_t& opt,
                 const std::string &cache_ref) {
  id_fn_map[key].reset(new rt::function(src, opt, cache_ref));
}

void delete_fn(const map_key_t& key) {
  id_fn_map.erase(key);
}

void register_cst(const map_key_t& key, const std::string& name, pybind11::buffer& data) {
  pybind11::buffer_info info = data.request();
  id_fn_map[key]->set_cst(name, info.ptr, info.size*info.itemsize);
}

void cleanup() {
  id_grid_map.clear();
  id_fn_map.clear();
  i64scalar_map.clear();
}

size_t make_op_id() {
  return id_fn_map.size();
}


/* TF scalar wrapper */
size_t make_scalar_id() {
  size_t ret = i64scalar_map.size();
  i64scalar_map[ret] = int64_t();
  return ret;
}

bool has_scalar(size_t id) {
  return i64scalar_map.find(id) != i64scalar_map.end();
}

int64_t retrieve_scalar(size_t id) {
  return i64scalar_map.at(id);
}

/* TF source-code generation */

inline std::string to_tf_ty(ir::type *ty) {
  if(ty->is_integer_ty(1))
    return "bool";
  if(ty->is_integer_ty(8))
    return "int8";
  if(ty->is_integer_ty(16))
    return "int16";
  if(ty->is_integer_ty(32))
    return "int32";
  if(ty->is_integer_ty(64))
    return "int64";
  if(ty->is_half_ty())
    return "float16";
  if(ty->is_float_ty())
    return "float";
  if(ty->is_double_ty())
    return "double";
  if(ty->is_pointer_ty())
    return "Tensor";
  throw std::runtime_error("unknown type");
}

inline std::string to_tf_scalar_ty(ir::type *ty) {
  if(ty->is_pointer_ty())
    return to_tf_ty(ty->get_pointer_element_ty());
  else {
    return to_tf_ty(ty);
  }
}

inline std::string ref_to_tf_ty(ir::type *ty) {
  std::string res = to_tf_ty(ty);
  if(ty->is_pointer_ty())
    res = "const " + res + "&";
  return res;
}

std::string tf_normalize(const std::string& name) {
  std::string ret = name;
  auto tolower = [](char c) { return std::tolower(c);};
  std::transform(ret.begin(), ret.end(), ret.begin(), tolower);
  return ret;
}

struct tf_alloc_t{
  enum type_t{
    OUTPUT,
    TEMP
  };

  tf_alloc_t(const std::string& _name, type_t _type)
    : name(_name), type(_type), tf_name(tf_normalize(_name)){ }

  std::string tf_name;
  std::string name;
  type_t type;
  size_t shape_id;
};

typedef std::vector<tf_alloc_t> alloc_map_t;


void gen_extract_inputs(std::ostream &os, const std::vector<ir::argument*>& args, const alloc_map_t& allocs) {
  for(unsigned i = 0; i < args.size(); i++){
    ir::value *arg = args[i];
    const std::string& name = arg->get_name();
    std::string ty = to_tf_ty(arg->get_type());
    if(!arg->get_type()->is_pointer_ty())
      os << "  " << ty << " " << name << " = context->input(" << i << ").scalar<" << ty << ">()();\n  ";
    else if(std::find_if(allocs.begin(), allocs.end(), 
                         [&](tf_alloc_t x) { 
                              return x.name == name; 
                             }) == allocs.end())
      os << "  const Tensor* " << name << " = &context->input(" << i << ");\n  ";
    else
      os << "  Tensor* " << name << " = nullptr;\n  ";
  }
}

void gen_set_outputs(std::ostream &os, const std::vector<ir::argument*>& args, const alloc_map_t& allocs) {
  // initialize shapes
  for(const auto& x: allocs)
    os << "  TensorShape " << x.name << "_shape;\n  ";  
  for(const auto& x: allocs)
    os << "  const Tensor& " << x.name << "_shape_tensor = context->input(" << x.shape_id << ");\n  ";
  for(const auto& x: allocs)
    os << "  const int32* " << x.name << "_shape_data = (const int32*)" << x.name << "_shape_tensor.tensor_data().data();\n  ";
  for(const auto& x: allocs)
    os << "  size_t " << x.name << "_rank = " << x.name << "_shape_tensor.dim_size(0);\n  ";
  for(const auto& x: allocs)
    os << "  for(size_t d = 0; d < " << x.name << "_rank ; d++) "
        <<  x.name << "_shape.AddDim(" << x.name << "_shape_data[d]);\n  ";
  
  // allocate
  int output = 0;
  for(const auto& x: allocs){
    if(x.type == tf_alloc_t::OUTPUT)
      os << "  OP_REQUIRES_OK(context, context->allocate_output(" << output++ << ", " << x.name << "_shape, &" << x.name << "));\n  ";
    else
      os << "  OP_REQUIRES_OK(context, context->allocate_temp(" << x.name << "_type, " << x.name << "_shape, " << x.name << "));\n  ";
  }
}

void gen_make_handles(std::ostream &os, const std::vector<ir::argument*>& args) {
  for(unsigned i = 0; i < args.size(); i++){
    ir::argument *arg = args[i];
    if(!arg->get_type()->is_pointer_ty())
      continue;
    const std::string& name = arg->get_name();
    os << "  drv::cu_buffer cu_" + name + "(ctx, " + name + "->nbytes(), (CUdeviceptr)" + name + "->tensor_data().data(), false);\n  ";
  }
}

void gen_make_launch_function(std::ostream &os, const std::vector<ir::argument*>& args) {
  os << "  std::function<void()> run = [&](){\n  ";
  os << "    (*id_fn_map.at(id_))({";
  for(unsigned i = 0; i < args.size() ; i++){
    ir::argument *arg = args[i];
    std::string name = arg->get_name();
    if(arg->get_type()->is_pointer_ty())
      name = "&cu_" + name;
    if(i > 0)
      os << ", ";
    os << name;
  }
  os << "}, *id_grid_map.at(id_), stream);\n  ";
  os << "  };\n  ";
  os << "  run();\n  ";
  os << "  if(bench_ > 0)\n  ";
  os << "    i64scalar_map[bench_id_] = triton::tools::bench(run, stream);\n  ";
}

void gen_tf_register_kernel_builder(std::ostream &os, const std::string &name,
                                 const std::string &opname,
                                 const std::vector<ir::argument*>& args,
                                 const alloc_map_t& allocs){

  os << "REGISTER_KERNEL_BUILDER(Name(\"" + name + "\").Device(DEVICE_GPU)";
  for(size_t i = 0; i < args.size(); i++){
    ir::argument *arg = args[i];
    std::string name = tf_normalize(arg->get_name());
    if(!arg->get_type()->is_pointer_ty())
      os << ".HostMemory(\"" + name + "\")";
  }
  for(const auto& x: allocs)
    os << ".HostMemory(\"" << x.tf_name << "_shape\")";
  os <<  ", " + opname << ");\n";
}

void gen_tf_register_op(std::ostream &os, const std::string &name,
                     const std::vector<ir::argument*>& args,
                     const alloc_map_t& allocs){
  
  
  os << "REGISTER_OP(\"" << name << "\")\n";
  for(size_t i = 0; i < args.size(); i++)
    os << "  .Attr(\"T" << i << " : {bool, int8, int16, int32, int64, float16, float32, float64}\")" << std::endl;
  for(size_t i = 0; i < args.size(); i++){
    ir::argument *arg = args[i];
    std::string name = tf_normalize(arg->get_name());
    if(std::find_if(allocs.begin(), allocs.end(),
                    [&](tf_alloc_t x) { 
                      return name == x.tf_name;
                    }) == allocs.end())
      os << "  .Input(\"" << name << ": T" << i << "\")\n";
    else
      os << "  .Input(\"" << name << "_shape: int32\")\n";
  }
  for(const auto& x: allocs)
    if(x.type == tf_alloc_t::OUTPUT)
      os << "  .Output(\"" << x.tf_name << ": T" << x.shape_id << "\")\n";
  os << "  .Attr(\"id: int\")\n";
  os << "  .Attr(\"bench: int\")\n";
  os << "  .Attr(\"bench_id: int\")\n";
  os << "  .SetShapeFn([](::tensorflow::shape_inference::InferenceContext* ctx) {\n";
  size_t current = 0;
  for(const auto& x: allocs)
    if(x.type == tf_alloc_t::OUTPUT){
      os << "    shape_inference::ShapeHandle " << x.tf_name << "_handle;\n";
      os << "    ctx->MakeShapeFromShapeTensor(" << x.shape_id << ", &" << x.tf_name << "_handle);\n";
      os << "    ctx->set_output(" << current++ << ", " << x.tf_name << "_handle);\n";
    }
  os << "      return Status::OK();\n";
  os << "  })\n";

  os << ";\n";
}

void make_module(const std::string& src, ir::module* ir,
                 const runtime::function::options_space_t& opt) {
  std::string copy = triton::runtime::function::preheader() + src;
  // pre-process
  TokenSequence tokens;
  Preprocessor cpp(&copy, true);
  for(auto it: opt.defines){
    cpp.AddMacro(it.first, &it.second[0]);
  }
  cpp.Process(tokens);
  // parse
  Parser parser(tokens);
  parser.Parse();
  Generator gen(&parser);
  gen.Gen(ir);
}

std::tuple<std::string,
           std::string> make_tensorflow_src(const std::string& src,
                                const std::vector<std::string>& outputs,
                                const std::vector<std::string>& tmp,
                                const runtime::function::options_space_t& opt)
{
  // triton-ir code-gen
  ir::context ctx;
  auto ir = std::shared_ptr<ir::module>(new ir::module("", ctx));
  make_module(src, &*ir, opt);

  // function
  ir::function* fn = ir->get_function_list().front();
  const std::vector<ir::argument*>& args = fn->args();
  std::string name = fn->get_name();
  std::string cc_name = name;
  cc_name[0] = static_cast<char>(std::toupper(cc_name[0]));
  std::string opname = cc_name + "Op";
  
  // allocation info
  alloc_map_t allocs;
  for(size_t i = 0; i < outputs.size(); i++)
    allocs.push_back(tf_alloc_t(outputs[i], tf_alloc_t::OUTPUT));
  for(size_t i = 0; i < tmp.size(); i++)
    allocs.push_back(tf_alloc_t(tmp[i], tf_alloc_t::TEMP));

  for(auto &x: allocs){
    size_t idx;
    for(idx = 0; idx < args.size(); idx++)
      if(args[idx]->get_name() == x.name)
        break;
    if(idx == args.size())
      throw std::runtime_error("unknown output");
    x.shape_id = idx;
  }

  std::ostringstream oss;
  oss << R"(
#include "triton/driver/buffer.h"
#include "triton/driver/backend.h"
#include "triton/driver/stream.h"
#include "triton/runtime/function.h"
#include "triton/tools/bench.hpp"

#define EIGEN_USE_GPU
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/op_kernel.h"

using namespace tensorflow;
using GPUDevice = Eigen::GpuDevice;
namespace rt = triton::runtime;
namespace drv = triton::driver;

extern std::map<size_t, std::shared_ptr<rt::function::grid_fn_ty>> id_grid_map;
extern std::map<size_t, std::shared_ptr<rt::function>> id_fn_map;
extern std::map<size_t, int64_t> i64scalar_map;

class )" << opname << R"(: public OpKernel {
 public:
  explicit )" << opname << R"((OpKernelConstruction* context)
    : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("id", &id_));
    OP_REQUIRES_OK(context, context->GetAttr("bench", &bench_));
    OP_REQUIRES_OK(context, context->GetAttr("bench_id", &bench_id_));
  )";
for(const auto& alloc: allocs)
  oss << "  OP_REQUIRES_OK(context, context->GetAttr(\"T" << alloc.shape_id << "\", &" << alloc.name << "_type));\n  ";

oss << R"(
  }

  void Compute(OpKernelContext* context){

    // get device/stream
    GPUDevice device =  context->eigen_device<GPUDevice>();
    drv::cu_stream sstream(device.stream(), false);
    drv::context* ctx = sstream.context();
    drv::stream* stream = &sstream;
    
    // extract inputs
    )";
gen_extract_inputs(oss, args, allocs);
oss << R"(
    // set outputs
    )";
gen_set_outputs(oss, args, allocs);
oss << R"(
    // wrap tensors
    )";
gen_make_handles(oss, args);
oss << R"(
    )";
oss << R"(
    // launch function
    )";
gen_make_launch_function(oss, args);
oss << R"(
  }

private:
  int id_;
  int bench_;
  int64 bench_id_;
  )";
for(const auto& alloc: allocs)
  oss << "DataType " << alloc.name << "_type;\n  ";
  
oss << R"(
};

// register kernel builder
)";
gen_tf_register_kernel_builder(oss, cc_name, opname, args, allocs);
oss << R"(
// register op
)";
gen_tf_register_op(oss, cc_name, args, allocs);

  return std::tuple<std::string, std::string>{oss.str(), name};
}


inline std::string to_torch_ty(ir::type *ty) {
  if(ty->is_integer_ty())
    return "int64_t";
  if(ty->is_half_ty())
    return "double";
  if(ty->is_float_ty())
    return "double";
  if(ty->is_double_ty())
    return "double";
  if(ty->is_pointer_ty())
    return "torch::Tensor";
  throw std::runtime_error("unknown type");
}

inline std::string to_torch_ty(rt::arg_type ty){
  switch(ty){
    case rt::INT1_T:   return "int64_t";
    case rt::INT8_T:   return "int64_t";
    case rt::INT16_T:  return "int64_t";
    case rt::INT32_T:  return "int64_t";
    case rt::INT64_T:  return "int64_t";
    case rt::HALF_T:   return "double";
    case rt::FLOAT_T:  return "double";
    case rt::DOUBLE_T: return "double";
    case rt::BUFFER_T: return "torch::Tensor";
    default:           return "UNKNOWN";
  }
}

inline std::string to_c_ty(rt::arg_type ty){
  switch(ty){
    case rt::INT1_T:   return "bool";
    case rt::INT8_T:   return "int8_t";
    case rt::INT16_T:  return "int16_t";
    case rt::INT32_T:  return "int32_t";
    case rt::INT64_T:  return "int64_t";
    case rt::HALF_T:   return "half";
    case rt::FLOAT_T:  return "float";
    case rt::DOUBLE_T: return "double";
    case rt::BUFFER_T: return "drv::cu_buffer";
    default:           return "UNKNOWN";
  }
}


inline std::string to_c_ty(ir::type *ty) {
  if(ty->is_integer_ty(1))
    return "bool";
  if(ty->is_integer_ty(8))
    return "int8_t";
  if(ty->is_integer_ty(16))
    return "int16_t";
  if(ty->is_integer_ty(32))
    return "int32_t";
  if(ty->is_integer_ty(64))
    return "int64_t";
  if(ty->is_half_ty())
    return "half";
  if(ty->is_float_ty())
    return "float";
  if(ty->is_double_ty())
    return "double";
  if(ty->is_pointer_ty())
    return "drv::cu_buffer";
  throw std::runtime_error("unknown type");
}



void gen_torch_signature(std::ostringstream& oss,
                         const std::string& name,
                         const std::vector<rt::arg_type>& args) {
  std::string ret_ty = "void";
  oss << ret_ty << " " << name << "(";
  oss << "int64_t id, ";
  oss << "int64_t dev_id, ";
  oss << "int64_t bench, ";
  oss << "int64_t bench_id,  ";
  for(size_t i = 0; i < args.size(); i++) {
    if(i > 0)
      oss << ", ";
    oss << to_torch_ty(args[i]) << " " << "th_arg_" << i;
  }
  oss << ")";
}

void gen_torch_init_driver(std::ostringstream &oss,
                           const std::vector<rt::arg_type>&args) {
  // Find index of first buffer
  size_t i;
  for(i = 0; i < args.size(); i++)
    if(args[i] == rt::BUFFER_T)
      break;
  oss <<  "  // Wrap CUDA handles" << std::endl;
  oss <<  "  c10::DeviceIndex device = th_arg_" << i << ".storage().device().index();" << std::endl;
  oss <<  "  // Get stream" << std::endl;
  oss <<  "  CUstream custream = (CUstream)at::cuda::getCurrentCUDAStream(device).stream();" << std::endl;
  oss <<  "  triton::driver::cu_stream stream(custream, false);" << std::endl;
  oss <<  "  triton::driver::context* ctx = stream.context();" << std::endl;
}

void gen_torch_make_handles(std::ostream &os,
                            const std::vector<rt::arg_type>& args) {
  for(unsigned i = 0; i < args.size(); i++){
    rt::arg_type arg = args[i];
    const std::string th_name = "th_arg_" + std::to_string(i);
    const std::string name = "arg_" + std::to_string(i);
    if(arg != rt::BUFFER_T)
      os << "  " << to_c_ty(arg) << " " << name << " = " << th_name << ";" << std::endl;
    else{
      os << "  CHECK_INPUT(" << th_name << ");" << std::endl;
      os << "  drv::cu_buffer " + name + "(ctx, " + th_name + ".nbytes(), "
            " (CUdeviceptr)((char*)" + th_name + ".storage().data() + " + th_name + ".storage_offset() * " + th_name + ".itemsize()), false);" << std::endl;
    }
  }
}

std::string get_val_struct_name(rt::arg_type ty){
  switch(ty){
    case rt::INT1_T: return "int1";
    case rt::INT8_T: return "int8";
    case rt::INT16_T: return "int16";
    case rt::INT32_T: return "int32";
    case rt::INT64_T: return "int64";
    case rt::HALF_T: return "fp16";
    case rt::FLOAT_T: return "fp32";
    case rt::DOUBLE_T: return "fp64";
    case rt::BUFFER_T: return "buf";
    default: return "";
  }
}

void gen_torch_make_launch_function(std::ostream &os, 
                                    const std::vector<rt::arg_type>& args) {
  os << "  namespace rt = triton::runtime;\n  ";
  os << "  std::vector<rt::arg> args;\n  ";
  for(unsigned i = 0; i < args.size(); i++){
    std::string name = "arg_" + std::to_string(i);
    if(args[i] == rt::BUFFER_T)
      name = "&" + name;
    if(args[i] == rt::HALF_T)
      name = "*((uint16_t*)&" + name + ")";
    os << "rt::arg_type ty" << i << " = (rt::arg_type)(" << args[i] << ");\n  ";
    os << "rt::arg::value_t val" << i << ";\n  ";
    os << "val" << i << "." << get_val_struct_name(args[i]) << " = " << name << ";\n  ";
    os << "args.push_back(rt::arg(ty" << i << ", val" << i << "));\n  ";
  }
  os << "  std::function<void()> run = [&](){\n  ";
  os << "    (*id_fn_map.at({id, dev_id}))(args , *id_grid_map.at({id, dev_id}), &stream);\n";
  os << "  };\n";
  os << "  run();\n";
  os << "  if(bench > 0)\n  ";
  os << "    i64scalar_map[bench_id] = triton::tools::bench(run, &stream);\n  ";
  }

void gen_torch_ret(std::ostream &os, const std::vector<std::string>& outputs) {
  if(outputs.size() == 1){
    os << "  return " << outputs[0] << ";" << std::endl;
    return;
  }
  os << "  return {";
  for(size_t i = 0; i < outputs.size(); i++){
    if(i > 0)
      os << ", ";
    os << outputs[i];
  }
  os << "};" << std::endl;
}

std::tuple<std::string,
           std::string> make_torch_src(const std::string& name, std::vector<rt::arg_type> args) {
  // generate framework code
  std::ostringstream oss;
  oss << R"(
#include "triton/driver/buffer.h"
#include "triton/driver/stream.h"
#include "triton/runtime/function.h"
#include "triton/tools/bench.hpp"
#include "torch/script.h"
#include "ATen/cuda/CUDAContext.h"

#define CHECK_CUDA(x) AT_ASSERTM(x.type().is_cuda(), #x " must be a CUDA tensor")
#define CHECK_CONTIGUOUS(x) AT_ASSERTM(x.is_contiguous(), #x " must be contiguous")
#define CHECK_INPUT(x) CHECK_CUDA(x);

namespace rt = triton::runtime;
namespace drv = triton::driver;

typedef std::pair<size_t, size_t> map_key_t;
extern std::map<map_key_t, std::shared_ptr<rt::function::grid_fn_ty>> id_grid_map;
extern std::map<map_key_t, std::shared_ptr<rt::function>> id_fn_map;
extern std::map<size_t, int64_t> i64scalar_map;

)";

  gen_torch_signature(oss, name, args);
  oss << " {" << std::endl;
  gen_torch_init_driver(oss, args);
  gen_torch_make_handles(oss, args);
  gen_torch_make_launch_function(oss, args);
  //gen_torch_ret(oss);
  oss << "}" << std::endl;

  oss << std::endl;
  oss << std::endl;
  oss << "static auto registry = torch::RegisterOperators(\"triton::" << name << "\", &" << name << ");" << std::endl;

  return std::tuple<std::string, std::string>{oss.str(), name};
}

/* Function signature */
std::vector<rt::arg_type> get_fn_signature(const std::string& src,
                                           const runtime::function::options_space_t& opt) {
  // triton-ir code-gen
  ir::context ctx;
  auto ir = std::shared_ptr<ir::module>(new ir::module("", ctx));
  make_module(src, &*ir, opt);
  // function
  ir::function* fn = ir->get_function_list().front();
  // extract signature
  std::vector<rt::arg_type> ret;
  ir::function_type* ty = fn->get_fn_type();
  for(size_t i = 0; i < ty->get_num_params(); i++)
    ret.push_back(rt::convert(ty->get_param_ty(i)));
  return ret;
}

typedef triton::runtime::function::options_t options_t;
typedef triton::runtime::function::options_space_t options_space_t;

PYBIND11_MODULE(libtriton, m) {
    m.doc() = "Python bindings to the C++ Triton API";

    // framework binding source code generation
    m.def("make_tensorflow_src", &make_tensorflow_src,
          "Creates C++ source code for a custom Tensorflow op "
          "corresponding to the specified Triton kernel");
    m.def("make_torch_src", &make_torch_src,
          "Creates C++ source code for a custom PyTorch op ");

    // bindings for triton classes
    pybind11::enum_<rt::arg_type>(m, "arg_type")
        .value("int1", rt::INT1_T)
        .value("int8", rt::INT8_T)
        .value("int16", rt::INT16_T)
        .value("int32", rt::INT32_T)
        .value("int64", rt::INT64_T)
        .value("half", rt::HALF_T)
        .value("float", rt::FLOAT_T)
        .value("double", rt::DOUBLE_T)
        .value("buffer", rt::BUFFER_T);

    pybind11::class_<options_t>(m, "options")
        .def(pybind11::init<>())
        .def("d", &options_t::D<int>)
        .def_readonly("num_warps", &options_t::num_warps);

    pybind11::class_<options_space_t>(m, "options_space")
        .def(pybind11::init<>())
        .def_readwrite("defines", &options_space_t::defines)
        .def_readwrite("num_warps", &options_space_t::num_warps);

    // hooks into triton constructs since frameworks may not use pybind11
    m.def("get_fn_signature", &get_fn_signature);
    m.def("register_grid", &register_grid);
    m.def("delete_grid", &delete_grid);
    m.def("register_fn", &register_fn);
    m.def("register_cst", &register_cst);
    m.def("delete_fn", &delete_fn);
    m.def("make_op_id", &make_op_id);
    m.def("make_scalar_id", &make_scalar_id);
    m.def("retrieve_scalar", &retrieve_scalar);
    m.def("cleanup", &cleanup);
    ;
}
