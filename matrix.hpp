#ifndef MATRIX_HPP_
#define MATRIX_HPP_

#define __CL_ENABLE_EXCEPTIONS
#include "cl.hpp"

#include <random>
#include <functional>

namespace matrix {
  typedef unsigned int dim_t;

  template <const dim_t W, const dim_t H>
  class Matrix {
  public:
    Matrix() {
      static_assert(W > 0, "width must be > 0");
      static_assert(H > 0, "height must be > 0");
      matrix = new float[W*H];
    }

    virtual ~Matrix() {
      if (matrix) {
        delete[] matrix;
        matrix = nullptr;
      }
    }

    Matrix(const Matrix&) = delete;
    Matrix& operator=(const Matrix&) = delete;

    Matrix(Matrix&& rhs): matrix(rhs.matrix) {
      rhs.matrix = nullptr;
    }

    Matrix& operator=(Matrix&& rhs) {
      this->matrix = rhs.matrix;
      rhs.matrix = nullptr;
      return *this;
    }

    cl::Buffer createBuffer(cl::Context& context, const cl_mem_flags flags) const {
      if (flags != CL_MEM_WRITE_ONLY) {
        return cl::Buffer(context, flags, this->size() * sizeof(float), matrix);
      } else {
        return cl::Buffer(context, flags, this->size() * sizeof(float));
      }
    }

    float* get() {
      return matrix;
    }

    dim_t getHeight() const {
      return H;
    }

    dim_t getWidth() const {
      return W;
    }

    dim_t size() const {
      return W*H;
    }

    void print() const {
      for (int i = 0; i < H*W; i++){
        std::cout << this->matrix[i] << " ";
        if ((i != 0) && ((i-1) % W == 0)) {
          std::cout << "\n";
        }
      }
      std::cout << std::endl;
    }

  private:
    float *matrix;
  };

  template <const dim_t W, const dim_t H>
  inline Matrix<W, H> randmat() {
    Matrix<W, H> m;
    float* const __restrict__ p = m.get();
    auto gen = std::bind(std::uniform_real_distribution<float>(0.0f, 1.0f), std::default_random_engine());

    for (auto i = 0; i < m.size(); ++i) {
      p[i] = gen();
    }
    return m;
  }

  template <const dim_t W, const dim_t H>
  inline Matrix<W, H> zeromat() {
    Matrix<W, H> m;
    std::fill_n(m.get(), m.size(), 0);
    return m;
  }

  template <const dim_t DIM>
  inline Matrix<DIM, 1> randvec() {
    return matrix::randmat<DIM, 1>();
  }

  template <const dim_t DIM>
  inline Matrix<DIM, 1> zerovec() {
    return matrix::zeromat<DIM, 1>();
  }

  namespace op {

    namespace {
      void buildProgram(cl::Context& context, cl::Program& program) {
        try {
          program.build();
        } catch (cl::Error error) {
          if (error.err() == CL_BUILD_PROGRAM_FAILURE) {
            std::vector<cl::Device> devices;
            devices = context.getInfo<CL_CONTEXT_DEVICES>();
            std::string built = program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[0]);
            std::cerr << built << "\n";
          }
          throw error;
        }
      }

      class Context {
      public:
        Context():
          context(DEVICE),
          program_mat(context, "matmul_kernel.cl"),
          program_vec(context, "matvec_mul.cl"),
          queue(context) {
          buildProgram(context, program_mat);
          buildProgram(context, program_vec);
        }

        cl::Context context;
        cl::Program program_mat;
        cl::Program program_vec;
        cl::CommandQueue queue;
      };
    } // enclosed

    static Context g_ctx;

    template<const dim_t AW, const dim_t AH, const dim_t BW, const dim_t BH>
    matrix::Matrix<AW, BH> multiply(const matrix::Matrix<AW, AH>& matA, const matrix::Matrix<BW, BH>& matB) {
      try {
        auto& context = g_ctx.context;
        auto& program = g_ctx.program_mat;
        auto& queue = g_ctx.queue;
        auto mmul = cl::make_kernel<int, cl::Buffer, cl::Buffer, cl::Buffer,
                                    cl::LocalSpaceArg, cl::LocalSpaceArg>(program, "mmul");

        auto result = matrix::zeromat<AW, BH>();

        cl::Buffer cl_matA = matA.createBuffer(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR);
        cl::Buffer cl_matB = matB.createBuffer(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR);
        cl::Buffer cl_result = result.createBuffer(context, CL_MEM_WRITE_ONLY);

        int blocksize = 16;
        cl::LocalSpaceArg A_block = cl::Local(sizeof(float) * blocksize*blocksize);
        cl::LocalSpaceArg B_block = cl::Local(sizeof(float) * blocksize*blocksize);

        mmul(
          cl::EnqueueArgs(queue,
                          cl::NDRange(matA.getWidth(),
                                      matA.getHeight()),
                          cl::NDRange(blocksize, blocksize)),
          matA.getWidth(),
          cl_matA,
          cl_matB,
          cl_result,
          A_block,
          B_block);

        queue.enqueueReadBuffer(cl_result, CL_TRUE, 0,
                                matA.size() * sizeof(float), result.get());
        return result;
      } catch (cl::Error err) {
        std::cout << "Exception\n";
        std::cerr
          << "ERROR: "
          << err.what()
          << "(" << err.err() << ")"
          << std::endl;
        throw;
      }
    }

    template<const dim_t AW, const dim_t AH, const dim_t BDIM>
    matrix::Matrix<AW, 1> multiply(const matrix::Matrix<AW, AH>& mat, const matrix::Matrix<BDIM, 1>& vec) {
      try {
        auto& context = g_ctx.context;
        auto& program = g_ctx.program_vec;
        auto& queue = g_ctx.queue;
        auto mmul =
          cl::make_kernel<cl::Buffer, cl::Buffer, cl::Buffer, int>(program, "matrixVectorMul");

        auto result_vector = matrix::zerovec<AW>();

        cl::Buffer cl_mat = mat.createBuffer(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR);
        cl::Buffer cl_vec = vec.createBuffer(context, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR);
        cl::Buffer cl_result_vector = result_vector.createBuffer(context, CL_MEM_WRITE_ONLY);

        mmul(
          cl::EnqueueArgs(queue, cl::NDRange(mat.getHeight())),
          cl_result_vector,
          cl_mat,
          cl_vec,
          mat.getWidth()
          );

        queue.enqueueReadBuffer(cl_result_vector, CL_TRUE, 0, mat.getWidth() * sizeof(float), result_vector.get());
        result_vector.print();
        return result_vector;
      } catch (cl::Error err) {
        std::cout << "Exception\n";
        std::cerr
          << "ERROR: "
          << err.what()
          << std::endl;
        throw;
      }
    }
  } // namespace op

} // namespace matrix

#endif // MATRIX_HPP_
