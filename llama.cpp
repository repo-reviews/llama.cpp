#define LLAMA_DEFAULT_COMPUTE_TYPE GGML_TYPE_F32
//#define LLAMA_DEFAULT_COMPUTE_TYPE GGML_TYPE_F16

// Defines fileno on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include <cstddef>
#include <cstdint>
#include <cstdio>
#endif

#include "llama-util.h"
#include "llama.h"

#include "ggml.h"
#if defined(GGML_USE_CLBLAST)
#include "ggml-opencl.h"
#endif

#ifdef GGML_USE_METAL
#include "ggml-metal.h"
#endif

#ifdef GGML_USE_CUDA
#include "ggml-cuda.h"
#endif

#ifdef GGML_USE_K_QUANTS
#ifndef QK_K
#ifdef GGML_QKK_64
#define QK_K 64
#else
#define QK_K 256
#endif
#endif
#endif

#include <array>
#include <ctime>
#include <cinttypes>
#include <fstream>
#include <random>
#include <map>
#include <unordered_map>
#include <queue>
#include <cassert>
#include <cstring>
#include <climits>
#include <memory>
#include <algorithm>
#include <initializer_list>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <numeric>

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

// available llama models
enum e_model {
    MODEL_UNKNOWN,
    MODEL_3B,
    MODEL_7B,
    MODEL_13B,
    MODEL_30B,
    MODEL_65B,
};

static const size_t kB = 1024;
static const size_t MB = 1024*1024;

// computed for n_ctx == 2048
// TODO: dynamically determine these sizes
//       needs modifications in ggml

typedef void (*offload_func_t)(struct ggml_tensor * tensor);

void llama_nop(struct ggml_tensor * tensor) { // don't offload by default
    (void) tensor;
}

//
// ggml helpers
//

static void ggml_graph_compute_helper(std::vector<uint8_t> & buf, ggml_cgraph * graph, int n_threads) {
    struct ggml_cplan plan = ggml_graph_plan(graph, n_threads);

    if (plan.work_size > 0) {
        buf.resize(plan.work_size);
        plan.work_data = buf.data();
    }

    ggml_graph_compute(graph, &plan);
}

//
// memory sizes
//

// 2*n_embd*n_ctx*n_layer*sizeof(float16)
static const std::map<e_model, size_t> & MEM_REQ_KV_SELF() {
    static std::map<e_model, size_t> k_sizes = {
        { MODEL_3B,    682ull * MB },
        { MODEL_7B,   1026ull * MB },
        { MODEL_13B,  1608ull * MB },
        { MODEL_30B,  3124ull * MB },
        { MODEL_65B,  5120ull * MB },
    };
    return k_sizes;
}

// this is mostly needed for temporary mul_mat buffers to dequantize the data
// not actually needed if BLAS is disabled
static const std::map<e_model, size_t> & MEM_REQ_EVAL() {
    static std::map<e_model, size_t> k_sizes = {
        { MODEL_3B,   512ull * MB },
        //{ MODEL_7B,   768ull * MB }, // FIXME: increased until improved memory management
        { MODEL_7B,  2048ull * MB },
        { MODEL_13B, 1024ull * MB },
        { MODEL_30B, 1280ull * MB },
        { MODEL_65B, 1536ull * MB },
    };
    return k_sizes;
}

// default hparams (LLaMA 7B)
struct llama_hparams {
    uint32_t n_vocab = 32000;
    uint32_t n_ctx   = 512;   // this is provided as user input?
    uint32_t n_embd  = 4096;
    uint32_t n_mult  = 256;
    uint32_t n_head  = 32;
    uint32_t n_layer = 32;
    uint32_t n_rot   = 64;

    float rope_freq_base  = 10000.0f;
    float rope_freq_scale = 1.0f;

    enum llama_ftype ftype = LLAMA_FTYPE_MOSTLY_F16;

    bool operator!=(const llama_hparams & other) const {
        return static_cast<bool>(memcmp(this, &other, sizeof(llama_hparams)));
    }
};

struct llama_layer {
    // normalization
    struct ggml_tensor * attention_norm;

    // attention
    struct ggml_tensor * wq;
    struct ggml_tensor * wk;
    struct ggml_tensor * wv;
    struct ggml_tensor * wo;

    // normalization
    struct ggml_tensor * ffn_norm;

    // ff
    struct ggml_tensor * w1;
    struct ggml_tensor * w2;
    struct ggml_tensor * w3;
};

struct llama_kv_cache {
    struct ggml_tensor * k = NULL;
    struct ggml_tensor * v = NULL;

    struct ggml_context * ctx = NULL;

    ggml_buffer * buf;

    int n; // number of tokens currently in the cache

    ~llama_kv_cache() {
        if (ctx) {
            ggml_free(ctx);
        }
    }
};

struct llama_vocab {
    using id    = int32_t;
    using token = std::string;

    struct token_score {
        token tok;
        float score;
    };

    std::unordered_map<token, id> token_to_id;
    std::vector<token_score> id_to_token;
};

struct llama_model {
    e_model type = MODEL_UNKNOWN;

    llama_hparams hparams;

    struct ggml_tensor * tok_embeddings;

    struct ggml_tensor * norm;
    struct ggml_tensor * output;

    std::vector<llama_layer> layers;
    int n_gpu_layers;

    // model memory mapped file
    std::unique_ptr<llama_mmap> mapping;

    // objects representing data potentially being locked in memory
    llama_mlock mlock_buf;
    llama_mlock mlock_mmap;

    // for quantize-stats only
    std::vector<std::pair<std::string, struct ggml_tensor *>> tensors_by_name;

    int64_t t_load_us = 0;
    int64_t t_start_us = 0;

    llama_vocab vocab;

    // backends
    ggml_backend * backend_cpu = NULL;
    ggml_buffer  * buf_cpu = NULL;
    ggml_context * ctx_cpu = NULL;
#ifdef GGML_USE_CUDA
    ggml_backend * backend_cuda = NULL;
    ggml_buffer  * buf_cuda = NULL;
    ggml_context * ctx_cuda = NULL;
#endif
#ifdef GGML_USE_METAL
    ggml_backend * backend_metal = NULL;
    ggml_buffer  * buf_metal = NULL;
    ggml_context * ctx_metal = NULL;
#endif

    // backend assigned to each layer
    ggml_backend * backend_inp = NULL;
    ggml_backend * backend_out = NULL;
    std::vector<ggml_backend *> backend_layers;

    ~llama_model() {
        if (ctx_cpu) {
            ggml_free(ctx_cpu);
            ggml_buffer_free(buf_cpu);
        }
#ifdef GGML_USE_CUDA
        if (ctx_cuda) {
            ggml_free(ctx_cuda);
            ggml_buffer_free(buf_cuda);
        }
#endif
#ifdef GGML_USE_METAL
        if (ctx_metal) {
            ggml_free(ctx_metal);
            ggml_buffer_free(buf_metal);
        }
#endif
    }
};

struct llama_context {
    llama_context(const llama_model & model) : model(model), t_load_us(model.t_load_us), t_start_us(model.t_start_us) {}
    std::mt19937 rng;

    bool has_evaluated_once = false;

    int64_t t_sample_us = 0;
    int64_t t_eval_us   = 0;
    int64_t t_p_eval_us = 0;

    int32_t n_sample = 0; // number of tokens sampled
    int32_t n_eval   = 0; // number of eval calls
    int32_t n_p_eval = 0; // number of tokens in eval calls for the prompt (with batch size > 1)

    const llama_model & model;

    bool model_owner = false;

    int64_t t_load_us;
    int64_t t_start_us;

    // key + value cache for the self attention
    struct llama_kv_cache kv_self;
    ggml_backend * backend_kv = NULL;

    // decode output (2-dimensional array: [n_tokens][n_vocab])
    std::vector<float> logits;
    bool logits_all = false;

    // input embedding (1-dimensional array: [n_embd])
    std::vector<float> embedding;

    // memory buffers used to evaluate the model
    ggml_buffer * buf_compute_cpu;

#ifdef GGML_USE_CUDA
    ggml_buffer * buf_compute_cuda;
#endif
#ifdef GGML_USE_METAL
    ggml_buffer * buf_compute_metal;
#endif

    // input tensors
    struct ggml_tensor * graph_tokens_in = nullptr;
    struct ggml_tensor * graph_embeddings_in = nullptr;

    // output tensors
    struct ggml_tensor * graph_logits = nullptr;
    struct ggml_tensor * graph_embeddings_out = nullptr;

    // buffers to store the inputs and outputs of the graphs
    ggml_buffer * buf_input;
    ggml_buffer * buf_output;

    /*
    ~llama_context() {
        if (model_owner) {
            delete &model;
        }
        if (buf_compute_cpu) {
            ggml_buffer_free(buf_compute_cpu);
        }
    }
    */
};

template <typename T>
static T checked_mul(T a, T b) {
    T ret = a * b;
    if (a != 0 && ret / a != b) {
        throw std::runtime_error(format("overflow multiplying %llu * %llu",
                     (unsigned long long) a, (unsigned long long) b));
    }
    return ret;
}

static size_t checked_div(size_t a, size_t b) {
    if (b == 0 || a % b != 0) {
        throw std::runtime_error(format("error dividing %zu / %zu", a, b));
    }
    return a / b;
}

static std::string llama_format_tensor_shape(const std::vector<uint32_t> & ne) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%5u", ne.at(0));
    for (size_t i = 1; i < ne.size(); i++) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " x %5u", ne.at(i));
    }
    return buf;
}

static size_t llama_calc_tensor_size(const std::vector<uint32_t> & ne, enum ggml_type type) {
    size_t size = ggml_type_size(type);
    for (uint32_t dim : ne) {
        size = checked_mul<size_t>(size, dim);
    }
    return size / ggml_blck_size(type);
}

struct llama_load_tensor {
    std::string name;
    enum ggml_type type = GGML_TYPE_F32;
    std::vector<uint32_t> ne;
    size_t file_off;
    size_t size;
    struct ggml_tensor * ggml_tensor = NULL;
    uint8_t * data;
};

struct llama_load_tensors_map {
    // tensors is kept in a separate vector to preserve file order
    std::vector<llama_load_tensor> tensors;
    std::unordered_map<std::string, size_t> name_to_idx;
};

enum llama_file_version {
    LLAMA_FILE_VERSION_GGML,
    LLAMA_FILE_VERSION_GGMF_V1, // added version field and scores in vocab
    LLAMA_FILE_VERSION_GGJT_V1, // added padding
    LLAMA_FILE_VERSION_GGJT_V2, // changed quantization format
    LLAMA_FILE_VERSION_GGJT_V3, // changed Q4 and Q8 quantization format
};

struct llama_file_loader {
    llama_file file;
    llama_file_version file_version;
    llama_hparams hparams;
    llama_vocab vocab;

    llama_file_loader(const char * fname, llama_load_tensors_map & tensors_map)
        : file(fname, "rb") {
        fprintf(stderr, "llama.cpp: loading model from %s\n", fname);
        read_magic();
        read_hparams();
        read_vocab();
        read_tensor_metadata(tensors_map);
    }
    void read_magic() {
        uint32_t magic = file.read_u32();

        if (magic == LLAMA_FILE_MAGIC_GGML) {
            file_version = LLAMA_FILE_VERSION_GGML;
            return;
        }

        uint32_t version = file.read_u32();

        switch (magic) {
            case LLAMA_FILE_MAGIC_GGMF:
                switch (version) {
                    case 1: file_version = LLAMA_FILE_VERSION_GGMF_V1; return;
                }
                break;
            case LLAMA_FILE_MAGIC_GGJT:
                switch (version) {
                    case 1: file_version = LLAMA_FILE_VERSION_GGJT_V1; return;
                    case 2: file_version = LLAMA_FILE_VERSION_GGJT_V2; return;
                    case 3: file_version = LLAMA_FILE_VERSION_GGJT_V3; return;
                }
        }

        throw std::runtime_error(format("unknown (magic, version) combination: %08x, %08x; is this really a GGML file?",
                     magic, version));
    }
    void read_hparams() {
        hparams.n_vocab = file.read_u32();
        hparams.n_embd = file.read_u32();
        hparams.n_mult = file.read_u32();
        hparams.n_head = file.read_u32();
        hparams.n_layer = file.read_u32();
        hparams.n_rot = file.read_u32();
        hparams.ftype = (enum llama_ftype) file.read_u32();
    }
    void read_vocab() {
        vocab.id_to_token.resize(hparams.n_vocab);

        for (uint32_t i = 0; i < hparams.n_vocab; i++) {
            uint32_t len = file.read_u32();
            std::string word = file.read_string(len);

            float score = 0.0f;
            file.read_raw(&score, sizeof(score));

            vocab.token_to_id[word] = i;

            auto & tok_score = vocab.id_to_token[i];
            tok_score.tok = std::move(word);
            tok_score.score = score;
        }
    }
    void read_tensor_metadata(llama_load_tensors_map & tensors_map) {
        while (file.tell() < file.size) {
            llama_load_tensor tensor;
            uint32_t n_dims = file.read_u32();
            uint32_t name_len = file.read_u32();
            tensor.type = (enum ggml_type) file.read_u32();
            tensor.ne.resize(n_dims);
            file.read_raw(tensor.ne.data(), sizeof(tensor.ne[0]) * n_dims);
            std::string name = file.read_string(name_len);
            if (n_dims < 1 || n_dims > 2) {
                throw std::runtime_error(format("llama.cpp: tensor '%s' should not be %u-dimensional", name.c_str(), n_dims));
            }
            switch (tensor.type) {
                case GGML_TYPE_F32:
                case GGML_TYPE_F16:
                case GGML_TYPE_Q4_0:
                case GGML_TYPE_Q4_1:
                case GGML_TYPE_Q5_0:
                case GGML_TYPE_Q5_1:
                case GGML_TYPE_Q8_0:
                case GGML_TYPE_Q2_K:
                case GGML_TYPE_Q3_K:
                case GGML_TYPE_Q4_K:
                case GGML_TYPE_Q5_K:
                case GGML_TYPE_Q6_K:
                    break;
                default: {
                    throw std::runtime_error(format("unrecognized tensor type %u\n", tensor.type));
                }
            }

            // skip to the next multiple of 32 bytes
            file.seek(-static_cast<ptrdiff_t>(file.tell()) & 31, SEEK_CUR);

            tensor.file_off = file.tell();
            tensor.name = name;
            tensor.size = llama_calc_tensor_size(tensor.ne, tensor.type);
            file.seek(tensor.size, SEEK_CUR);

            tensors_map.tensors.push_back(tensor);
            tensors_map.name_to_idx[name] = tensors_map.tensors.size() - 1;
        }
    }
};

struct llama_file_saver {
    llama_file file;
    llama_file_loader * any_file_loader;
    llama_file_saver(const char * fname, llama_file_loader * any_file_loader, enum llama_ftype new_ftype)
        : file(fname, "wb"), any_file_loader(any_file_loader) {
        fprintf(stderr, "llama.cpp: saving model to %s\n", fname);
        write_magic();
        write_hparams(new_ftype);
        write_vocab();
    }
    void write_magic() {
        file.write_u32(LLAMA_FILE_MAGIC);   // magic
        file.write_u32(LLAMA_FILE_VERSION); // version
    }
    void write_hparams(enum llama_ftype new_ftype) {
        const llama_hparams & hparams = any_file_loader->hparams;
        file.write_u32(hparams.n_vocab);
        file.write_u32(hparams.n_embd);
        file.write_u32(hparams.n_mult);
        file.write_u32(hparams.n_head);
        file.write_u32(hparams.n_layer);
        file.write_u32(hparams.n_rot);
        file.write_u32(new_ftype);
    }
    void write_vocab() {
        if (any_file_loader->file_version == LLAMA_FILE_VERSION_GGML) {
            fprintf(stderr, "llama.cpp: WARNING: input is an old file that doesn't have scores; will add dummy scores\n");
        }
        uint32_t n_vocab = any_file_loader->hparams.n_vocab;
        for (uint32_t i = 0; i < n_vocab; i++) {
            const auto & token_score = any_file_loader->vocab.id_to_token.at(i);
            file.write_u32((uint32_t) token_score.tok.size());
            file.write_raw(token_score.tok.data(), token_score.tok.size());
            file.write_raw(&token_score.score, sizeof(token_score.score));
        }
    }
    void write_tensor(llama_load_tensor & tensor, enum ggml_type new_type, const void * new_data, size_t new_size) {
        switch (new_type) {
            case GGML_TYPE_F32:
            case GGML_TYPE_F16:
            case GGML_TYPE_Q4_0:
            case GGML_TYPE_Q4_1:
            case GGML_TYPE_Q5_0:
            case GGML_TYPE_Q5_1:
            case GGML_TYPE_Q8_0:
            case GGML_TYPE_Q2_K:
            case GGML_TYPE_Q3_K:
            case GGML_TYPE_Q4_K:
            case GGML_TYPE_Q5_K:
            case GGML_TYPE_Q6_K:
                break;
            default: LLAMA_ASSERT(false);
        }
        file.write_u32((uint32_t) tensor.ne.size());
        file.write_u32((uint32_t) tensor.name.size());
        file.write_u32(new_type);
        file.write_raw(tensor.ne.data(), sizeof(tensor.ne[0]) * tensor.ne.size());
        file.write_raw(tensor.name.data(), tensor.name.size());
        file.seek(-static_cast<ptrdiff_t>(file.tell()) & 31, SEEK_CUR);
        LLAMA_ASSERT(new_size == llama_calc_tensor_size(tensor.ne, new_type));
        file.write_raw(new_data, new_size);
    }
};

struct llama_model_loader {
    std::unique_ptr<llama_file_loader> file_loader;
    llama_load_tensors_map tensors_map;
    bool use_mmap;
    size_t num_ggml_tensors_created = 0;
    std::unique_ptr<llama_mmap> mapping;
    llama_model * model;

    llama_model_loader(const std::string & fname_base, bool use_mmap) {
        file_loader = std::unique_ptr<llama_file_loader>(new llama_file_loader(fname_base.c_str(), tensors_map));
        if (!llama_mmap::SUPPORTED) {
            use_mmap = false;
        }
        this->use_mmap = use_mmap;
    }

    void calc_sizes(size_t * ctx_size_p, size_t * mmapped_size_p) const {
        *ctx_size_p = *mmapped_size_p = 0;
        for (const llama_load_tensor & lt : tensors_map.tensors) {
            *ctx_size_p += sizeof(struct ggml_tensor) + GGML_OBJECT_SIZE;
            *(use_mmap ? mmapped_size_p : ctx_size_p) += lt.size + 16;
        }
    }

    struct ggml_tensor * get_tensor(const std::string & name, const std::vector<uint32_t> & ne, ggml_context * ggml_ctx) {
        auto it = tensors_map.name_to_idx.find(name);
        if (it == tensors_map.name_to_idx.end()) {
            throw std::runtime_error(std::runtime_error(format("llama.cpp: tensor '%s' is missing from model", name.c_str())));
        }
        llama_load_tensor & lt = tensors_map.tensors.at(it->second);
        if (lt.ne != ne) {
            throw std::runtime_error(format("llama.cpp: tensor '%s' has wrong shape; expected %s, got %s",
                         name.c_str(), llama_format_tensor_shape(ne).c_str(), llama_format_tensor_shape(lt.ne).c_str()));
        }

        return get_tensor_for(lt, ggml_ctx);
    }

    struct ggml_tensor * get_tensor_for(llama_load_tensor & lt, ggml_context * ggml_ctx) {
        struct ggml_tensor * tensor;
        if (lt.ne.size() == 2) {
            tensor = ggml_new_tensor_2d(ggml_ctx, lt.type, lt.ne.at(0), lt.ne.at(1));
        } else {
            LLAMA_ASSERT(lt.ne.size() == 1);
            tensor = ggml_new_tensor_1d(ggml_ctx, lt.type, lt.ne.at(0));
        }
        ggml_set_name(tensor, lt.name.c_str());
        LLAMA_ASSERT(lt.ggml_tensor == NULL); // if this fails, we called get_tensor twice on the same tensor

        lt.ggml_tensor = tensor;
        num_ggml_tensors_created++;
        return tensor;
    }

    void done_getting_tensors() const {
        if (num_ggml_tensors_created != tensors_map.tensors.size()) {
            throw std::runtime_error(std::string("llama.cpp: file contained more tensors than expected"));
        }
    }

    void load_all_data(llama_progress_callback progress_callback, void * progress_callback_user_data, llama_mlock * lmlock) {
        size_t data_size = 0;
        size_t lock_size = 0;

        if (use_mmap) {
            mapping.reset(new llama_mmap(&file_loader->file, false, ggml_is_numa()));
            if (lmlock) {
                lmlock->init(mapping->addr);
            }
        }

        size_t done_size = 0;
        std::vector<uint8_t> load_buf;
        size_t load_buf_size = 0;
        for (llama_load_tensor & lt : tensors_map.tensors) {
            bool is_cpu = lt.ggml_tensor->backend == model->backend_cpu;
            if (!use_mmap && !is_cpu) {
                load_buf_size = std::max(load_buf_size, lt.size);
            }
            data_size += lt.size;
        }
        if (load_buf_size > 0) {
            load_buf.resize(load_buf_size);
            // may improve CUDA loading speed without mmap
            //ggml_cuda_host_register(load_buf.data(), load_buf.size());
        }

        for (llama_load_tensor & lt : tensors_map.tensors) {
            if (progress_callback) {
                progress_callback((float) done_size / data_size, progress_callback_user_data);
            }
            LLAMA_ASSERT(lt.ggml_tensor); // unused tensors should have been caught by load_data already

            const bool is_ram_shared = lt.ggml_tensor->backend->is_ram_shared;

            // select buffer to load data into
            if (!use_mmap) {
                if (is_ram_shared) {
                    lt.data = (uint8_t *) lt.ggml_tensor->data;
                } else {
                    // read to temporary buffer
                    lt.data = (uint8_t *) load_buf.data();
                }
            }

            load_data_for(lt);

            if (is_ram_shared) {
                if (use_mmap) {
                    lt.ggml_tensor->data = lt.data;
                    // TODO: this assumes that the data to lock is contiguous, which may not always be the case
                    if (lmlock) {
                        lock_size += lt.size;
                        lmlock->grow_to(lock_size);
                    }
                }
            } else {
                ggml_backend_tensor_set(lt.ggml_tensor, lt.data, 0, lt.size);
                if (use_mmap) {
                    // hint the OS that we don't need the data anymore
                    // TODO: this may be a bad idea with devices that use the system memory (Metal?)
                    mapping->discard(lt.data, lt.size);
                }
            }

            done_size += lt.size;
        }
        //if (load_buf_size > 0) {
        //    ggml_cuda_host_unregister(load_buf.data());
        //}
    }

    void load_data_for(llama_load_tensor & lt) const {
        if (use_mmap) {
            lt.data = (uint8_t *) mapping->addr + lt.file_off;
        } else {
            llama_file & file = file_loader->file;
            file.seek(lt.file_off, SEEK_SET);
            file.read_raw(lt.data, lt.size);
        }

        if (0) {
            print_checksum(lt);
        }
    }

    static void print_checksum(llama_load_tensor & lt) {
        uint32_t sum = 0;
        for (size_t i = 0; i < lt.size; i++) {
            uint8_t byte = lt.data[i];
            sum = byte + (sum << 6) + (sum << 16) - sum; // sdbm hash
        }
        fprintf(stderr, "%s checksum: %#08x (%s, size %zu)\n", lt.name.c_str(), sum,
                llama_format_tensor_shape(lt.ne).c_str(), lt.size);
    }

};

//
// kv cache
//

static bool kv_cache_init(
                      ggml_backend * backend,
        const struct llama_hparams & hparams,
             struct llama_kv_cache & cache,
                         ggml_type   wtype,
                               int   n_ctx) {
    const int n_embd  = hparams.n_embd;
    const int n_layer = hparams.n_layer;

    const int64_t n_mem      = n_layer*n_ctx;
    const int64_t n_elements = n_embd*n_mem;

    size_t size = 2u*n_elements*ggml_type_size(wtype) + 2u*MB;

    cache.buf = ggml_buffer_alloc(backend, size, 2);
    cache.n = 0;

    struct ggml_init_params params = ggml_init_params_default();
    params.buffer = cache.buf;

    cache.ctx = ggml_init(params);

    if (!cache.ctx) {
        fprintf(stderr, "%s: failed to allocate memory for kv cache\n", __func__);
        return false;
    }

    cache.k = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);
    cache.v = ggml_new_tensor_1d(cache.ctx, wtype, n_elements);
    ggml_set_name(cache.k, "cache_k");
    ggml_set_name(cache.v, "cache_v");

    return true;
}

struct llama_context_params llama_context_default_params() {
    struct llama_context_params result = {
        /*.seed                        =*/ LLAMA_DEFAULT_SEED,
        /*.n_ctx                       =*/ 512,
        /*.n_batch                     =*/ 512,
        /*.n_gpu_layers                =*/ 0,
        /*.main_gpu                    =*/ 0,
        /*.tensor_split                =*/ {0},
        /*.rope_freq_base              =*/ 10000.0f,
        /*.rope_freq_scale             =*/ 1.0f,
        /*.progress_callback           =*/ nullptr,
        /*.progress_callback_user_data =*/ nullptr,
        /*.low_vram                    =*/ false,
        /*.f16_kv                      =*/ true,
        /*.logits_all                  =*/ false,
        /*.vocab_only                  =*/ false,
        /*.use_mmap                    =*/ true,
        /*.use_mlock                   =*/ false,
        /*.embedding                   =*/ false,
    };

    return result;
}

struct llama_model_quantize_params llama_model_quantize_default_params() {
    struct llama_model_quantize_params result = {
        /*.nthread                     =*/ 0,
        /*.ftype                       =*/ LLAMA_FTYPE_MOSTLY_Q5_1,
        /*.allow_requantize            =*/ false,
        /*.quantize_output_tensor      =*/ true,
    };

    return result;
}

bool llama_mmap_supported() {
    return llama_mmap::SUPPORTED;
}

bool llama_mlock_supported() {
    return llama_mlock::SUPPORTED;
}

void llama_backend_init(bool numa) {
    ggml_time_init();

    // needed to initialize f16 tables
    {
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = NULL;
        struct ggml_context * ctx = ggml_init(params);
        ggml_free(ctx);
    }

    if (numa) {
        ggml_numa_init();
    }

#ifdef GGML_USE_MPI
    ggml_mpi_backend_init();
#endif
}

void llama_backend_free() {
#ifdef GGML_USE_MPI
    ggml_mpi_backend_free();
#endif
}

int64_t llama_time_us() {
    return ggml_time_us();
}

//
// model loading
//

static const char *llama_file_version_name(llama_file_version version) {
    switch (version) {
        case LLAMA_FILE_VERSION_GGML: return "'ggml' (old version with low tokenizer quality and no mmap support)";
        case LLAMA_FILE_VERSION_GGMF_V1: return "ggmf v1 (old version with no mmap support)";
        case LLAMA_FILE_VERSION_GGJT_V1: return "ggjt v1 (pre #1405)";
        case LLAMA_FILE_VERSION_GGJT_V2: return "ggjt v2 (pre #1508)";
        case LLAMA_FILE_VERSION_GGJT_V3: return "ggjt v3 (latest)";
    }

    return "unknown";
}

static const char *llama_ftype_name(enum llama_ftype ftype) {
    switch (ftype) {
        case LLAMA_FTYPE_ALL_F32:     return "all F32";
        case LLAMA_FTYPE_MOSTLY_F16:  return "mostly F16";
        case LLAMA_FTYPE_MOSTLY_Q4_0: return "mostly Q4_0";
        case LLAMA_FTYPE_MOSTLY_Q4_1: return "mostly Q4_1";
        case LLAMA_FTYPE_MOSTLY_Q4_1_SOME_F16:
                                      return "mostly Q4_1, some F16";
        case LLAMA_FTYPE_MOSTLY_Q5_0: return "mostly Q5_0";
        case LLAMA_FTYPE_MOSTLY_Q5_1: return "mostly Q5_1";
        case LLAMA_FTYPE_MOSTLY_Q8_0: return "mostly Q8_0";
        // K-quants
        case LLAMA_FTYPE_MOSTLY_Q2_K: return "mostly Q2_K";
        case LLAMA_FTYPE_MOSTLY_Q3_K_S: return "mostly Q3_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q3_K_M: return "mostly Q3_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q3_K_L: return "mostly Q3_K - Large";
        case LLAMA_FTYPE_MOSTLY_Q4_K_S: return "mostly Q4_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q4_K_M: return "mostly Q4_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q5_K_S: return "mostly Q5_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q5_K_M: return "mostly Q5_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q6_K: return "mostly Q6_K";
        default:                      return "unknown, may not work";
    }
}

static const char *llama_model_type_name(e_model type) {
    switch (type) {
        case MODEL_3B: return "3B";
        case MODEL_7B: return "7B";
        case MODEL_13B: return "13B";
        case MODEL_30B: return "30B";
        case MODEL_65B: return "65B";
        default: LLAMA_ASSERT(false);
    }
}

static void llama_model_load_internal(
        const std::string & fname,
        llama_model & model,
        llama_vocab & vocab,
        int n_ctx,
        int n_batch,
        int n_gpu_layers,
        int main_gpu,
        const float * tensor_split,
        float rope_freq_base,
        float rope_freq_scale,
        bool low_vram,
        ggml_type memory_type,
        bool use_mmap,
        bool use_mlock,
        bool vocab_only,
        llama_progress_callback progress_callback,
        void * progress_callback_user_data) {

    model.t_start_us = ggml_time_us();

    std::unique_ptr<llama_model_loader> ml(new llama_model_loader(fname, use_mmap));
    ml->model = &model;

    vocab = std::move(ml->file_loader->vocab);
    model.hparams = ml->file_loader->hparams;
    model.n_gpu_layers = n_gpu_layers;
    llama_file_version file_version = ml->file_loader->file_version;
    auto & hparams = model.hparams;

    {
        switch (hparams.n_layer) {
            case 26: model.type = e_model::MODEL_3B; break;
            case 32: model.type = e_model::MODEL_7B; break;
            case 40: model.type = e_model::MODEL_13B; break;
            case 60: model.type = e_model::MODEL_30B; break;
            case 80: model.type = e_model::MODEL_65B; break;
            default:
                {
                    if (hparams.n_layer < 32) {
                        model.type = e_model::MODEL_7B;
                    }
                } break;
        }

        hparams.n_ctx = n_ctx;

        hparams.rope_freq_base  = rope_freq_base;
        hparams.rope_freq_scale = rope_freq_scale;
    }

    const uint32_t n_ff = ((2*(4*hparams.n_embd)/3 + hparams.n_mult - 1)/hparams.n_mult)*hparams.n_mult;

    {
        fprintf(stderr, "%s: format     = %s\n",   __func__, llama_file_version_name(file_version));
        fprintf(stderr, "%s: n_vocab    = %u\n",   __func__, hparams.n_vocab);
        fprintf(stderr, "%s: n_ctx      = %u\n",   __func__, hparams.n_ctx);
        fprintf(stderr, "%s: n_embd     = %u\n",   __func__, hparams.n_embd);
        fprintf(stderr, "%s: n_mult     = %u\n",   __func__, hparams.n_mult);
        fprintf(stderr, "%s: n_head     = %u\n",   __func__, hparams.n_head);
        fprintf(stderr, "%s: n_layer    = %u\n",   __func__, hparams.n_layer);
        fprintf(stderr, "%s: n_rot      = %u\n",   __func__, hparams.n_rot);
        fprintf(stderr, "%s: freq_base  = %.1f\n", __func__, hparams.rope_freq_base);
        fprintf(stderr, "%s: freq_scale = %g\n",   __func__, hparams.rope_freq_scale);
        fprintf(stderr, "%s: ftype      = %u (%s)\n", __func__, hparams.ftype, llama_ftype_name(hparams.ftype));
        fprintf(stderr, "%s: n_ff       = %u\n",   __func__, n_ff);
        fprintf(stderr, "%s: model size = %s\n",   __func__, llama_model_type_name(model.type));
    }

    if (file_version < LLAMA_FILE_VERSION_GGJT_V2) {
        if (hparams.ftype != LLAMA_FTYPE_ALL_F32     &&
            hparams.ftype != LLAMA_FTYPE_MOSTLY_F16  &&
            hparams.ftype != LLAMA_FTYPE_MOSTLY_Q8_0) {
            throw std::runtime_error(format("this format is no longer supported (see https://github.com/ggerganov/llama.cpp/pull/1405)"));
        }
    }

    if (file_version < LLAMA_FILE_VERSION_GGJT_V3) {
        if (hparams.ftype == LLAMA_FTYPE_MOSTLY_Q4_0 ||
            hparams.ftype == LLAMA_FTYPE_MOSTLY_Q4_1 ||
            hparams.ftype == LLAMA_FTYPE_MOSTLY_Q8_0) {
            throw std::runtime_error(format("this format is no longer supported (see https://github.com/ggerganov/llama.cpp/pull/1508)"));
        }
    }

    if (vocab_only) {
        return;
    }

    // initialize backends
    const uint32_t n_layer = hparams.n_layer;

    model.backend_cpu = ggml_backend_cpu_init();

    ggml_backend * backend_cpu = model.backend_cpu;
    ggml_backend * backend_gpu = model.backend_cpu; // hack until we have a proper backend selection

#ifdef GGML_USE_CUDA
    if (n_gpu_layers > 0) {
        model.backend_cuda = ggml_backend_cuda_init();
        backend_gpu = model.backend_cuda;
    }
#endif
#ifdef GGML_USE_METAL
    if (n_gpu_layers > 0) {
        model.backend_metal = ggml_backend_metal_init();
        backend_gpu = model.backend_metal;
    }
#endif

    // assign splits to the backends
    const int i_gpu_start = std::max(0, (int)n_layer - n_gpu_layers);

    model.backend_inp = n_gpu_layers > (int)n_layer ? backend_gpu : backend_cpu;
    model.backend_out = n_gpu_layers > 0            ? backend_gpu : backend_cpu;

    model.backend_layers.resize(n_layer);
    std::fill(model.backend_layers.begin(),               model.backend_layers.begin() + i_gpu_start, backend_cpu);
    std::fill(model.backend_layers.begin() + i_gpu_start, model.backend_layers.end(),                 backend_gpu);

    // calculate the size of each context
    std::unordered_map<struct ggml_backend *, size_t> ctx_sizes;
    for (const llama_load_tensor & lt : ml->tensors_map.tensors) {
        if (lt.name == "tok_embeddings.weight") {
            ctx_sizes[model.backend_inp] += lt.size;
        }
        else if (lt.name == "norm.weight" || lt.name == "output.weight") {
            ctx_sizes[model.backend_out] += lt.size;
        }
        else {
            // parse layer number from name
            int layer = -1;
            if (sscanf(lt.name.c_str(), "layers.%d.", &layer) != 1) {
                throw std::runtime_error(format("failed to parse layer number from tensor name '%s'", lt.name.c_str()));
            }
            if (layer < 0 || layer >= (int)n_layer) {
                throw std::runtime_error(format("invalid layer number %d", layer));
            }
            ctx_sizes[model.backend_layers[layer]] += lt.size;
        }
    }

    // TODO: generalize support for mmap
    size_t mmap_size = 0;
    if (ml->use_mmap) {
        for (auto & it : ctx_sizes) {
            if (it.first->is_ram_shared) {
                mmap_size += it.second;
                ctx_sizes[it.first] = 0;
            }
        }
    }

    fprintf(stderr, "%s: ggml ctx sizes:\n", __func__);
    for (const auto & it : ctx_sizes) {
        fprintf(stderr, "%8s = %7.2f MB\n", ggml_backend_name(it.first), it.second / 1024.0 / 1024.0);
    }
    if (mmap_size > 0) {
        fprintf(stderr, "%8s = %7.2f MB\n", "mmap", mmap_size / 1024.0 / 1024.0);
    }

    // create the buffers and contexts
    {
        size_t cpu_num_tensors = ml->tensors_map.tensors.size();
        size_t ctx_size = ctx_sizes[backend_cpu];
        model.buf_cpu = ggml_buffer_alloc(model.backend_cpu, ctx_size, cpu_num_tensors);
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = model.buf_cpu;
        params.no_alloc = ml->use_mmap;
        model.ctx_cpu = ggml_init(params);
        if (!model.ctx_cpu) {
            throw std::runtime_error(format("ggml_init() failed for CPU backend"));
        }
    }

    ggml_context * ctx_gpu = model.ctx_cpu;

#ifdef GGML_USE_CUDA
    if (n_gpu_layers > 0) {
        size_t gpu_num_tensors = ml->tensors_map.tensors.size();
        size_t ctx_size = ctx_sizes[model.backend_cuda];
        model.buf_cuda = ggml_buffer_alloc(model.backend_cuda, ctx_size, gpu_num_tensors);
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = model.buf_cuda;
        model.ctx_cuda = ggml_init(params);
        if (!model.ctx_cuda) {
            throw std::runtime_error(format("ggml_init() failed for CUDA backend"));
        }

        ctx_gpu = model.ctx_cuda;
    }
#endif

#ifdef GGML_USE_METAL
    if (n_gpu_layers > 0) {
        const size_t ctx_size  = ctx_sizes[model.backend_metal];
        const size_t n_tensors = ml->tensors_map.tensors.size();

        model.buf_metal = ggml_buffer_alloc(model.backend_metal, ctx_size, n_tensors);

        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = model.buf_metal;

        model.ctx_metal = ggml_init(params);
        if (!model.ctx_metal) {
            throw std::runtime_error(format("ggml_init() failed for CPU backend"));
        }

        ctx_gpu = model.ctx_metal;
    }
#endif

    // TODO: clean this
    ggml_context * ctx_input  = (model.backend_inp == backend_gpu) ? ctx_gpu : model.ctx_cpu;
    ggml_context * ctx_output = (model.backend_out == backend_gpu) ? ctx_gpu : model.ctx_cpu;

    std::vector<ggml_context *> ctx_layers(n_layer, model.ctx_cpu);
    for (uint32_t i = 0; i < n_layer; ++i) {
        if (model.backend_layers[i] == backend_gpu) {
            ctx_layers[i] = ctx_gpu;
        }
    }

    // prepare memory for the weights
    {
        const uint32_t n_embd  = hparams.n_embd;
        const uint32_t n_vocab = hparams.n_vocab;

        model.tok_embeddings = ml->get_tensor("tok_embeddings.weight", {n_embd, n_vocab}, ctx_input);

        // "output" tensor
        {
            model.norm   = ml->get_tensor("norm.weight",   {n_embd},          ctx_output);
            model.output = ml->get_tensor("output.weight", {n_embd, n_vocab}, ctx_output);
        }

        model.layers.resize(n_layer);
        for (uint32_t i = 0; i < n_layer; ++i) {
            auto & layer = model.layers[i];
            ggml_context * ctx_layer = ctx_layers[i];

            std::string layers_i = "layers." + std::to_string(i);

            layer.attention_norm = ml->get_tensor(layers_i + ".attention_norm.weight", {n_embd}, ctx_layer);

            layer.wq = ml->get_tensor(layers_i + ".attention.wq.weight", {n_embd, n_embd}, ctx_layer);
            layer.wk = ml->get_tensor(layers_i + ".attention.wk.weight", {n_embd, n_embd}, ctx_layer);
            layer.wv = ml->get_tensor(layers_i + ".attention.wv.weight", {n_embd, n_embd}, ctx_layer);
            layer.wo = ml->get_tensor(layers_i + ".attention.wo.weight", {n_embd, n_embd}, ctx_layer);

            layer.ffn_norm = ml->get_tensor(layers_i + ".ffn_norm.weight", {n_embd}, ctx_layer);

            layer.w1 = ml->get_tensor(layers_i + ".feed_forward.w1.weight", {n_embd,   n_ff},   ctx_layer);
            layer.w2 = ml->get_tensor(layers_i + ".feed_forward.w2.weight", {  n_ff,   n_embd}, ctx_layer);
            layer.w3 = ml->get_tensor(layers_i + ".feed_forward.w3.weight", {n_embd,   n_ff},   ctx_layer);
        }
    }

    ml->done_getting_tensors();

    (void) main_gpu;
    (void) tensor_split;
    (void) low_vram;
    (void) n_batch;

    // print memory requirements
    {
        const size_t scale = memory_type == GGML_TYPE_F32 ? 2 : 1;

        // FIXME: this is not very useful without knowing the CPU/GPU memory split

        // this is the total memory required to run the inference
        size_t ctx_sum = mmap_size;
        for (const auto & it : ctx_sizes) {
            ctx_sum += it.second;
        }

        const size_t mem_required = ctx_sum + MEM_REQ_EVAL().at(model.type);

        // this is the memory required by one llama_state
        const size_t mem_required_state = scale*MEM_REQ_KV_SELF().at(model.type);

        fprintf(stderr, "%s: mem required  = %7.2f MB (+ %7.2f MB per state)\n", __func__,
                mem_required / 1024.0 / 1024.0, mem_required_state / 1024.0 / 1024.0);
    }

    // populate tensors_by_name
    for (llama_load_tensor & lt : ml->tensors_map.tensors) {
        model.tensors_by_name.emplace_back(lt.name, lt.ggml_tensor);
    }

    ml->load_all_data(progress_callback, progress_callback_user_data, use_mlock ? &model.mlock_mmap : NULL);

    if (progress_callback) {
        progress_callback(1.0f, progress_callback_user_data);
    }

    model.mapping = std::move(ml->mapping);

    // loading time will be recalculate after the first eval, so
    // we take page faults deferred by mmap() into consideration
    model.t_load_us = ggml_time_us() - model.t_start_us;

}

static bool llama_model_load(
        const std::string & fname,
        llama_model & model,
        llama_vocab & vocab,
        int n_ctx,
        int n_batch,
        int n_gpu_layers,
        int main_gpu,
        float * tensor_split,
        float rope_freq_base,
        float rope_freq_scale,
        bool low_vram,
        ggml_type memory_type,
        bool use_mmap,
        bool use_mlock,
        bool vocab_only,
        llama_progress_callback progress_callback,
        void *progress_callback_user_data) {
    try {
        llama_model_load_internal(fname, model, vocab, n_ctx, n_batch, n_gpu_layers, main_gpu, tensor_split, rope_freq_base, rope_freq_scale, low_vram, memory_type,
                                  use_mmap, use_mlock, vocab_only, progress_callback, progress_callback_user_data);
        return true;
    } catch (const std::exception & err) {
        fprintf(stderr, "error loading model: %s\n", err.what());
        return false;
    }
}

static ggml_graph_splits llama_build_graph(
        llama_context & lctx,
            const int   n_tokens,
            const int   n_past,
                 bool   embeddings_input = false,
            ggml_type   compute_type = LLAMA_DEFAULT_COMPUTE_TYPE) {

    // const int64_t t_start_us = ggml_time_us();

    const int N = n_tokens;

    const auto & model   = lctx.model;
    const auto & hparams = model.hparams;

    const auto & kv_self = lctx.kv_self;

    LLAMA_ASSERT(!!kv_self.ctx);

    const int n_embd  = hparams.n_embd;
    const int n_layer = hparams.n_layer;
    const int n_ctx   = hparams.n_ctx;
    const int n_head  = hparams.n_head;
    const int n_rot   = hparams.n_embd/hparams.n_head;
    const int n_vocab = hparams.n_vocab;

    const float freq_base  = hparams.rope_freq_base;
    const float freq_scale = hparams.rope_freq_scale;


    struct ggml_graph_splits splits = ggml_graph_split_init();

    // initialize contexts for every backend

    struct ggml_context * ctx_cpu = nullptr;

    if (lctx.buf_compute_cpu != nullptr) {
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = lctx.buf_compute_cpu;
        params.compute_type = compute_type;
        ctx_cpu = ggml_init(params);
    }

#ifdef GGML_USE_CUDA
    struct ggml_context * ctx_cuda = nullptr;

    if (lctx.buf_compute_cuda != nullptr) {
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = lctx.buf_compute_cuda;
        params.compute_type = compute_type;
        ctx_cuda = ggml_init(params);
    }
#endif

#ifdef GGML_USE_METAL
    struct ggml_context * ctx_metal = nullptr;

    if (lctx.buf_compute_metal != nullptr) {
        struct ggml_init_params params = ggml_init_params_default();
        params.buffer = lctx.buf_compute_metal;
        params.compute_type = compute_type;
        ctx_metal = ggml_init(params);
    }
#endif

    // TODO: clean this
    struct ggml_context * ctx_i      = nullptr;
    struct ggml_context * ctx_o      = nullptr;
    struct ggml_context * ctx_kv     = nullptr;
    struct ggml_context * ctx_ls[80] = {nullptr};

    if (lctx.model.backend_inp == lctx.model.backend_cpu) ctx_i = ctx_cpu;
    if (lctx.model.backend_out == lctx.model.backend_cpu) ctx_o = ctx_cpu;

#ifdef GGML_USE_CUDA
    if (lctx.model.backend_inp == lctx.model.backend_cuda) ctx_i = ctx_cuda;
    if (lctx.model.backend_out == lctx.model.backend_cuda) ctx_o = ctx_cuda;
#endif
#ifdef GGML_USE_METAL
    if (lctx.model.backend_inp == lctx.model.backend_metal) ctx_i = ctx_metal;
    if (lctx.model.backend_out == lctx.model.backend_metal) ctx_o = ctx_metal;
#endif

    for (int il = 0; il < n_layer; il++) {
        if (lctx.model.backend_layers[il] == lctx.model.backend_cpu) ctx_ls[il] = ctx_cpu;

#ifdef GGML_USE_CUDA
        if (lctx.model.backend_layers[il] == lctx.model.backend_cuda) ctx_ls[il] = ctx_cuda;
#endif
#ifdef GGML_USE_METAL
        if (lctx.model.backend_layers[il] == lctx.model.backend_metal) ctx_ls[il] = ctx_metal;
#endif
    }

    if (lctx.backend_kv == lctx.model.backend_cpu) ctx_kv = ctx_cpu;

#ifdef GGML_USE_CUDA
    if (lctx.backend_kv == lctx.model.backend_cuda) ctx_kv = ctx_cuda;
#endif
#ifdef GGML_USE_METAL
    if (lctx.backend_kv == lctx.model.backend_metal) ctx_kv = ctx_metal;
#endif

    struct ggml_tensor * inpL;

    // reuse the scale tensor for all layers since it requires a memory transfer
    struct ggml_tensor * KQ_scale = ggml_new_f32(ctx_kv, 1.0f/sqrtf(float(n_embd)/n_head));
    ggml_set_name(KQ_scale, "1/sqrt(n_embd/n_head)");

    if (embeddings_input) {
        // use embeddings as input
        struct ggml_tensor * embd_in = lctx.graph_embeddings_in;
        ggml_graph_splits_add(&splits, &embd_in, ctx_i, "input_embd");
        inpL = ggml_view_2d(ctx_i, embd_in, N, n_embd, ggml_element_size(embd_in)*n_embd, 0);
    } else {
        // use tokens as input
        ggml_tensor * token_in = ggml_view_1d(ctx_i, lctx.graph_tokens_in, N, 0);
        ggml_graph_splits_add(&splits, &token_in, ctx_i, "input_tokens");
        inpL = ggml_get_rows(ctx_i, model.tok_embeddings, token_in);
    }

    struct ggml_tensor * cur = nullptr;
    for (int il = 0; il < n_layer; ++il) {
        struct ggml_context * ctx_l = ctx_ls[il];

        ggml_graph_splits_add(&splits, &inpL, ctx_l, "l%d", il);

        struct ggml_tensor * inpSA = inpL;

        // norm
        {
            cur = ggml_rms_norm(ctx_l, inpL);
            ggml_set_name(cur, "rms_norm_0");

            // cur = cur*attention_norm(broadcasted)
            cur = ggml_mul(ctx_l, cur, model.layers[il].attention_norm);
            ggml_set_name(cur, "attention_norm_0");
        }

        // self-attention
        {
            // compute Q and K and RoPE them
            struct ggml_tensor * tmpq = ggml_mul_mat(ctx_l, model.layers[il].wq, cur);
            ggml_set_name(tmpq, "tmpq");

            struct ggml_tensor * tmpk = ggml_mul_mat(ctx_l, model.layers[il].wk, cur);
            ggml_set_name(tmpk, "tmpk");

            // compute the transposed [N, n_embd] V matrix
            struct ggml_tensor * tmpv = ggml_mul_mat(ctx_l, model.layers[il].wv, cur);
            ggml_set_name(tmpv, "tmpv");

            struct ggml_tensor * Kcur = ggml_rope_custom_inplace(ctx_l, ggml_reshape_3d(ctx_l, tmpk, n_embd/n_head, n_head, N), n_past, n_rot, 0, freq_base, freq_scale, 0);
            ggml_set_name(Kcur, "Kcur");

            struct ggml_tensor * Qcur = ggml_rope_custom_inplace(ctx_l, ggml_reshape_3d(ctx_l, tmpq, n_embd/n_head, n_head, N), n_past, n_rot, 0, freq_base, freq_scale, 0);
            ggml_set_name(Qcur, "Qcur");

            struct ggml_tensor * Vcur = ggml_transpose(ctx_l, ggml_reshape_2d(ctx_l, tmpv, n_embd, N));
            ggml_set_name(Vcur, "Vcur");

            ggml_tensor ** attn_inputs[] = {&Kcur, &Vcur, &Qcur, NULL};
            ggml_graph_splits_add_n(&splits, attn_inputs, ctx_kv, "l%d_attn", il);

            struct ggml_tensor * k;
            struct ggml_tensor * v;
            // store key and value to memory
            {
                ggml_tensor * k_v = ggml_view_1d(ctx_kv, kv_self.k, N*n_embd, (ggml_element_size(kv_self.k)*n_embd)*(il*n_ctx + n_past));
                ggml_tensor * v_v = ggml_view_2d(ctx_kv, kv_self.v, N, n_embd,
                        (   n_ctx)*ggml_element_size(kv_self.v),
                        (il*n_ctx)*ggml_element_size(kv_self.v)*n_embd + n_past*ggml_element_size(kv_self.v));
                ggml_set_name(k_v, "k_v");
                ggml_set_name(v_v, "v_v");

                // important: storing RoPE-ed version of K in the KV cache!
                struct ggml_tensor * k_cpy = ggml_cpy(ctx_kv, Kcur, k_v);
                struct ggml_tensor * v_cpy = ggml_cpy(ctx_kv, Vcur, v_v);
                ggml_set_name(k_cpy, "k_cpy");
                ggml_set_name(v_cpy, "v_cpy");

                // TODO: replace with ggml_dependency / ggml_depends_on
                k = ggml_view_tensor(ctx_kv, kv_self.k);
                v = ggml_view_tensor(ctx_kv, kv_self.v);
                k->src[0] = k_cpy;
                v->src[0] = v_cpy;
            }

            struct ggml_tensor * Q =
                ggml_permute(ctx_kv,
                        Qcur,
                        0, 2, 1, 3);
            ggml_set_name(Q, "Q");

            struct ggml_tensor * K =
                ggml_permute(ctx_kv,
                    ggml_reshape_3d(ctx_kv,
                        ggml_view_1d(ctx_kv, k, (n_past + N)*n_embd, il*n_ctx*ggml_element_size(k)*n_embd),
                            n_embd/n_head, n_head, n_past + N),
                        0, 2, 1, 3);
            ggml_set_name(K, "K");

            // K * Q
            struct ggml_tensor * KQ = ggml_mul_mat(ctx_kv, K, Q);
            ggml_set_name(KQ, "KQ");

            // KQ_scaled = KQ / sqrt(n_embd/n_head)
            // KQ_scaled shape [n_past + N, N, n_head, 1]
            struct ggml_tensor * KQ_scaled = ggml_scale_inplace(ctx_kv, KQ, KQ_scale);
            ggml_set_name(KQ_scaled, "KQ_scaled");

            // KQ_masked = mask_past(KQ_scaled)
            struct ggml_tensor * KQ_masked = ggml_diag_mask_inf_inplace(ctx_kv, KQ_scaled, n_past);
            ggml_set_name(KQ_masked, "KQ_masked");

            // KQ = soft_max(KQ_masked)
            struct ggml_tensor * KQ_soft_max = ggml_soft_max_inplace(ctx_kv, KQ_masked);
            ggml_set_name(KQ_soft_max, "KQ_soft_max");

            // split cached V into n_head heads
            struct ggml_tensor * V =
                ggml_view_3d(ctx_kv, v,
                        n_past + N, n_embd/n_head, n_head,
                        n_ctx*ggml_element_size(v),
                        n_ctx*ggml_element_size(v)*n_embd/n_head,
                        il*n_ctx*ggml_element_size(v)*n_embd);
            ggml_set_name(V, "V");

#if 1
            struct ggml_tensor * KQV = ggml_mul_mat(ctx_kv, V, KQ_soft_max);
#else
            // make V contiguous in memory to speed up the matmul, however we waste time on the copy
            // on M1 this is faster for the perplexity computation, but ~5% slower for the single-token generation
            // is there a better way?
            struct ggml_tensor * V_cont = ggml_cpy(ctx0, V, ggml_new_tensor_3d(ctx0, kv_self.v->type, n_past + N, n_embd/n_head, n_head));
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V_cont, KQ_soft_max);
#endif
            ggml_set_name(KQV, "KQV");

            ggml_graph_splits_add(&splits, &KQV, ctx_l, "l%d", il);

            // KQV_merged = KQV.permute(0, 2, 1, 3)
            struct ggml_tensor * KQV_merged = ggml_permute(ctx_l, KQV, 0, 2, 1, 3);
            ggml_set_name(KQV_merged, "KQV_merged");

            // cur = KQV_merged.contiguous().view(n_embd, N)
            cur = ggml_cpy(ctx_l,
                    KQV_merged,
                    //ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));
                    //ggml_new_tensor_2d(ctx0, GGML_TYPE_F16, n_embd, N));
                    ggml_new_tensor_2d(ctx_l, compute_type, n_embd, N)); // support both automatically?
            ggml_set_name(cur, "KQV_merged_contiguous");

            // projection (no bias)
            cur = ggml_mul_mat(ctx_l,
                    model.layers[il].wo,
                    cur);
            ggml_set_name(cur, "result_wo");
        }

        struct ggml_tensor * inpFF = ggml_add(ctx_l, cur, inpSA);
        ggml_set_name(inpFF, "inpFF");

        // feed-forward network
        {
            // norm
            {
                cur = ggml_rms_norm(ctx_l, inpFF);
                ggml_set_name(cur, "rms_norm_1");

                // cur = cur*ffn_norm(broadcasted)
                cur = ggml_mul(ctx_l, cur, model.layers[il].ffn_norm);
                ggml_set_name(cur, "ffn_norm");
            }

            struct ggml_tensor * tmp = ggml_mul_mat(ctx_l,
                    model.layers[il].w3,
                    cur);
            ggml_set_name(tmp, "result_w3");

            cur = ggml_mul_mat(ctx_l,
                    model.layers[il].w1,
                    cur);
            ggml_set_name(cur, "result_w1");

            // SILU activation
            cur = ggml_silu(ctx_l, cur);
            ggml_set_name(cur, "silu");

            cur = ggml_mul(ctx_l, cur, tmp);
            ggml_set_name(cur, "silu_x_result_w3");

            cur = ggml_mul_mat(ctx_l,
                    model.layers[il].w2,
                    cur);
            ggml_set_name(cur, "result_w2");
        }

        cur = ggml_add(ctx_l, cur, inpFF);
        ggml_set_name(cur, "inpFF_+_result_w2");

        // input for next layer
        inpL = cur;

#if defined(LLAMA_1L_GRAPH_DUMP)
        break;
#endif
    }

    ggml_graph_splits_add(&splits, &inpL, ctx_o, "output");

    // norm
    {
        cur = ggml_rms_norm(ctx_o, inpL);
        ggml_set_name(cur, "rms_norm_2");

        // cur = cur*norm(broadcasted)
        cur = ggml_mul(ctx_o, cur, model.norm);
        ggml_set_name(cur, "result_norm");

        // TODO: avoid this copy (and the other output tensors)
        ggml_tensor * embeddings = lctx.graph_embeddings_out;
        if (embeddings != nullptr) {
            // TODO: fix this, only the last embedding has to be copied
            LLAMA_ASSERT(false);
            cur = ggml_cpy(ctx_o, cur, embeddings);
        }
    }

    // TODO: skip output layer when using embeddings?

    // lm_head
    cur = ggml_mul_mat(ctx_o, model.output, cur);
    ggml_set_name(cur, "result_output");

    ggml_tensor * logits = lctx.graph_logits;
    if (logits != nullptr) {
        // copy logits data to out tensor
        if (lctx.logits_all) {
            cur = ggml_cpy(ctx_o, cur, ggml_view_2d(ctx_o, logits, n_vocab, N, ggml_element_size(logits)*n_vocab, 0));
        } else {
            // make a view skipping the first N-1 tokens
            cur = ggml_view_1d(ctx_o, cur, n_vocab, (N-1)*n_vocab*ggml_element_size(cur));
            // copy the logits to the output tensor
            // TODO: avoid this copy
            cur = ggml_cpy(ctx_o, cur, logits);
        }
    }

    ggml_graph_splits_build_forward(&splits, cur);

    // plot the computation graph in dot format (for debugging purposes)
    //if (n_past%100 == 0) {
    //    ggml_graph_dump_dot(&gf, NULL, "llama.dot");
    //}

#ifdef LLAMA_1L_GRAPH_DUMP
    if (N == 1 && n_past == 0) {
        ggml_graph_dump_dot(gf, NULL, "llama.dot");
        printf("graph for N=%i, n_past=%i dumped to llama.dot\n", N, n_past);
        exit(0);
    }
#endif

#if 0
    printf("\n%s: used_mem = %.3f MB, scratch -- %.3f MB %.3f MB\n", __func__,
            ggml_used_mem(ctx0)/1024.0/1024.0,
            lctx.get_buf_max_mem(0)/1024.0/1024.0,
            lctx.get_buf_max_mem(1)/1024.0/1024.0);
#endif

    //int64_t t_end_us = ggml_time_us();
    //fprintf(stderr, "%s: time = %.3f ms\n", __func__, (t_end_us-t_start_us)/1000.0);

    if (ctx_cpu != nullptr) {
        ggml_free(ctx_cpu);
    }
#ifdef GGML_USE_CUDA
    if (ctx_cuda != nullptr) {
        ggml_free(ctx_cuda);
    }
#endif
#ifdef GGML_USE_METAL
    if (ctx_metal != nullptr) {
        ggml_free(ctx_metal);
    }
#endif

    return splits;
}

// evaluate the transformer
//
//   - lctx:      llama context
//   - tokens:    new batch of tokens to process
//   - embd       embeddings input
//   - n_tokens   number of tokens
//   - n_past:    the context size so far
//   - n_threads: number of threads to use
//
static bool llama_eval_internal(
         llama_context & lctx,
     const llama_token * tokens,
           const float * embd,
             const int   n_tokens,
             const int   n_past,
                   int   n_threads) {

    LLAMA_ASSERT((!tokens && embd) || (tokens && !embd));

    bool embd_input = embd != nullptr;

    const int64_t t_start_us = ggml_time_us();

    const auto & model   = lctx.model;
    const auto & hparams = model.hparams;
    const int n_embd     = hparams.n_embd;

    const int N = n_tokens;

    LLAMA_ASSERT(lctx.graph_logits != nullptr);

    // for big prompts, if BLAS is enabled, it is better to use only one thread
    // otherwise, the threads are spin-lock waiting for the BLAS calls and are degrading the performance
    n_threads = N >= 32 && ggml_cpu_has_blas() ? 1 : n_threads;
    ggml_backend_cpu_set_n_threads(const_cast<ggml_backend*>(model.backend_cpu), n_threads);

    struct ggml_graph_splits splits = llama_build_graph(lctx, N, n_past, embd_input);

    if (tokens != nullptr) {
        // copy the tokens to the input tensor
        ggml_backend_tensor_set_async(lctx.graph_tokens_in, tokens, 0, N*ggml_element_size(lctx.graph_tokens_in));
    } else {
        // copy the embeddings to the input tensor
        ggml_backend_tensor_set_async(lctx.graph_embeddings_in, embd, 0, N*n_embd*ggml_element_size(lctx.graph_embeddings_in));
    }

    // run the computation
    ggml_graph_splits_compute(&splits);
    ggml_graph_splits_free(&splits);

    // update kv token count
    lctx.kv_self.n = n_past + N;

    // TODO: this is not easy to do with split graphs - maybe just remove
    //if (cgraph_fname) {
    //    ggml_graph_export(&gf, cgraph_fname);
    //}

#ifdef GGML_PERF
    // print timing information per ggml operation (for debugging purposes)
    // requires GGML_PERF to be defined
    ggml_graph_print(&gf);
#endif

    // extract logits
    {
        const int n_vocab = hparams.n_vocab;
        auto & logits_out = lctx.logits;

        if (lctx.logits_all) {
            logits_out.resize(n_vocab * N);
            ggml_backend_tensor_get_async(lctx.graph_logits, logits_out.data(), 0, N*n_vocab*sizeof(float));
        } else {
            // return result for just the last token
            logits_out.resize(n_vocab);
            ggml_backend_tensor_get_async(lctx.graph_logits, logits_out.data(), 0, n_vocab*sizeof(float));
        }
    }

    // extract embeddings
    if (!lctx.embedding.empty()) {
        auto & embedding_out = lctx.embedding;
        embedding_out.resize(n_embd);
        ggml_backend_tensor_get_async(lctx.graph_embeddings_out, embedding_out.data(), 0, n_embd*sizeof(float));
    }

#ifdef GGML_USE_CUDA
    // wait for the async copy to finish
    if (lctx.model.n_gpu_layers > 0) {
        ggml_backend_synchronize(const_cast<ggml_backend*>(lctx.model.backend_cuda));
    }
#endif

    // measure the performance only for the single-token evals
    if (N == 1) {
        lctx.t_eval_us += ggml_time_us() - t_start_us;
        lctx.n_eval++;
    }
    else if (N > 1) {
        lctx.t_p_eval_us += ggml_time_us() - t_start_us;
        lctx.n_p_eval += N;
    }

    return true;
}

//
// tokenizer
//

static size_t utf8_len(char src) {
    const size_t lookup[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4 };
    uint8_t highbits = static_cast<uint8_t>(src) >> 4;
    return lookup[highbits];
}

struct llama_sp_symbol {
    using index = int;
    index prev;
    index next;
    const char * text;
    size_t n;
};

static_assert(std::is_trivially_copyable<llama_sp_symbol>::value, "llama_sp_symbol is not trivially copyable");

struct llama_sp_bigram {
    struct comparator {
        bool operator()(llama_sp_bigram & l, llama_sp_bigram & r) {
            return (l.score < r.score) || (l.score == r.score && l.left > r.left);
        }
    };
    using queue_storage = std::vector<llama_sp_bigram>;
    using queue = std::priority_queue<llama_sp_bigram, queue_storage, comparator>;
    llama_sp_symbol::index left;
    llama_sp_symbol::index right;
    float score;
    size_t size;
};

// original implementation:
// https://github.com/ggerganov/llama.cpp/commit/074bea2eb1f1349a0118239c4152914aecaa1be4
struct llama_tokenizer {
    llama_tokenizer(const llama_vocab & vocab): vocab_(vocab) {}

    void tokenize(const std::string & text, std::vector<llama_vocab::id> & output) {
        // split string into utf8 chars
        int index = 0;
        size_t offs = 0;
        while (offs < text.size()) {
            llama_sp_symbol sym;
            size_t char_len = std::min(text.size() - offs, utf8_len(text[offs]));
            sym.text = text.c_str() + offs;
            sym.n = char_len;
            offs += char_len;
            sym.prev = index - 1;
            sym.next = offs == text.size() ? -1 : index + 1;
            index++;
            symbols_.emplace_back(sym);
        }

        // seed the work queue with all possible 2-character tokens.
        for (size_t i = 1; i < symbols_.size(); ++i) {
            try_add_bigram(i - 1, i);
        }

        // keep substituting the highest frequency pairs for as long as we can.
        while (!work_queue_.empty()) {
            auto bigram = work_queue_.top();
            work_queue_.pop();

            auto & left_sym = symbols_[bigram.left];
            auto & right_sym = symbols_[bigram.right];

            // if one of the symbols already got merged, skip it.
            if (left_sym.n == 0 || right_sym.n == 0 ||
                left_sym.n + right_sym.n != bigram.size) {
                continue;
            }

            // merge the right sym into the left one
            left_sym.n += right_sym.n;
            right_sym.n = 0;

            //printf("left = '%*s' size = %zu\n", (int) left_sym.n, left_sym.text, bigram.size);

            // remove the right sym from the chain
            left_sym.next = right_sym.next;
            if (right_sym.next >= 0) {
                symbols_[right_sym.next].prev = bigram.left;
            }

            // find more substitutions
            try_add_bigram(left_sym.prev, bigram.left);
            try_add_bigram(bigram.left, left_sym.next);
        }

        for (int i = 0; i != -1; i = symbols_[i].next) {
            auto & symbol = symbols_[i];
            auto token = vocab_.token_to_id.find(std::string(symbol.text, symbol.n));

            if (token == vocab_.token_to_id.end()) {
                // output any symbols that did not form tokens as bytes.
                for (int j = 0; j < (int) symbol.n; ++j) {
                    llama_vocab::id token_id = static_cast<uint8_t>(symbol.text[j]) + 3;
                    output.push_back(token_id);
                }
            } else {
                output.push_back((*token).second);
            }
        }
    }

private:
    void try_add_bigram(int left, int right) {
        if (left == -1 || right == -1) {
            return;
        }

        const std::string text = std::string(symbols_[left].text, symbols_[left].n + symbols_[right].n);
        auto token = vocab_.token_to_id.find(text);

        if (token == vocab_.token_to_id.end()) {
            return;
        }

        if (static_cast<size_t>((*token).second) >= vocab_.id_to_token.size()) {
            return;
        }

        const auto &tok_score = vocab_.id_to_token[(*token).second];

        llama_sp_bigram bigram;
        bigram.left = left;
        bigram.right = right;
        bigram.score = tok_score.score;
        bigram.size = text.size();
        work_queue_.push(bigram);
    }

    const llama_vocab & vocab_;
    std::vector<llama_sp_symbol> symbols_;
    llama_sp_bigram::queue work_queue_;
};

static std::vector<llama_vocab::id> llama_tokenize(const llama_vocab & vocab, const std::string & text, bool bos) {
    llama_tokenizer tokenizer(vocab);
    std::vector<llama_vocab::id> output;

    if (bos) {
        output.push_back(llama_token_bos());
    }

    if (text.empty()) {
        return output;
    }

    tokenizer.tokenize(text, output);
    return output;
}

//
// sampling
//

void llama_sample_softmax(struct llama_context * ctx, llama_token_data_array * candidates) {
    assert(candidates->size > 0);

    const int64_t t_start_sample_us = ggml_time_us();

    // Sort the logits in descending order
    if (!candidates->sorted) {
        std::sort(candidates->data, candidates->data + candidates->size, [](const llama_token_data & a, const llama_token_data & b) {
            return a.logit > b.logit;
        });
        candidates->sorted = true;
    }

    float max_l = candidates->data[0].logit;
    float cum_sum = 0.0f;
    for (size_t i = 0; i < candidates->size; ++i) {
        float p = expf(candidates->data[i].logit - max_l);
        candidates->data[i].p = p;
        cum_sum += p;
    }
    for (size_t i = 0; i < candidates->size; ++i) {
        candidates->data[i].p /= cum_sum;
    }

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_top_k(struct llama_context * ctx, llama_token_data_array * candidates, int k, size_t min_keep) {
    const int64_t t_start_sample_us = ggml_time_us();

    k = std::max(k, (int) min_keep);
    k = std::min(k, (int) candidates->size);

    // Sort scores in descending order
    if (!candidates->sorted) {
        auto comp = [](const llama_token_data & a, const llama_token_data & b) {
            return a.logit > b.logit;
        };
        if (k == (int) candidates->size) {
            std::sort(candidates->data, candidates->data + candidates->size, comp);
        } else {
            std::partial_sort(candidates->data, candidates->data + k, candidates->data + candidates->size, comp);
        }
        candidates->sorted = true;
    }
    candidates->size = k;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_top_p(struct llama_context * ctx, llama_token_data_array * candidates, float p, size_t min_keep) {
    if (p >= 1.0f) {
        return;
    }

    llama_sample_softmax(ctx, candidates);

    const int64_t t_start_sample_us = ggml_time_us();

    // Compute the cumulative probabilities
    float cum_sum = 0.0f;
    size_t last_idx = candidates->size;

    for (size_t i = 0; i < candidates->size; ++i) {
        cum_sum += candidates->data[i].p;

        // Check if the running sum is at least p or if we have kept at least min_keep tokens
        // we set the last index to i+1 to indicate that the current iterate should be included in the set
        if (cum_sum >= p && i + 1 >= min_keep) {
            last_idx = i + 1;
            break;
        }
    }

    // Resize the output vector to keep only the top-p tokens
    candidates->size = last_idx;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_tail_free(struct llama_context * ctx, llama_token_data_array * candidates, float z, size_t min_keep) {
    if (z >= 1.0f || candidates->size <= 2) {
        return;
    }

    llama_sample_softmax(nullptr, candidates);
    const int64_t t_start_sample_us = ggml_time_us();

    // Compute the first and second derivatives
    std::vector<float> first_derivatives(candidates->size - 1);
    std::vector<float> second_derivatives(candidates->size - 2);

    for (size_t i = 0; i < first_derivatives.size(); ++i) {
        first_derivatives[i] = candidates->data[i].p - candidates->data[i + 1].p;
    }
    for (size_t i = 0; i < second_derivatives.size(); ++i) {
        second_derivatives[i] = first_derivatives[i] - first_derivatives[i + 1];
    }

    // Calculate absolute value of second derivatives
    for (size_t i = 0; i < second_derivatives.size(); ++i) {
        second_derivatives[i] = abs(second_derivatives[i]);
    }

    // Normalize the second derivatives
    float second_derivatives_sum = std::accumulate(second_derivatives.begin(), second_derivatives.end(), 0.0f);
    for (float & value : second_derivatives) {
        value /= second_derivatives_sum;
    }

    float cum_sum = 0.0f;
    size_t last_idx = candidates->size;
    for (size_t i = 0; i < second_derivatives.size(); ++i) {
        cum_sum += second_derivatives[i];

        // Check if the running sum is greater than z or if we have kept at least min_keep tokens
        if (cum_sum > z && i >= min_keep) {
            last_idx = i;
            break;
        }
    }

    // Resize the output vector to keep only the tokens above the tail location
    candidates->size = last_idx;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}


void llama_sample_typical(struct llama_context * ctx, llama_token_data_array * candidates, float p, size_t min_keep) {
    // Reference implementation:
    // https://github.com/huggingface/transformers/compare/main...cimeister:typical-sampling:typical-pr
    if (p >= 1.0f) {
        return;
    }

    // Compute the softmax of logits and calculate entropy
    llama_sample_softmax(nullptr, candidates);

    const int64_t t_start_sample_us = ggml_time_us();

    float entropy = 0.0f;
    for (size_t i = 0; i < candidates->size; ++i) {
        entropy += -candidates->data[i].p * logf(candidates->data[i].p);
    }

    // Compute the absolute difference between negative log probability and entropy for each candidate
    std::vector<float> shifted_scores;
    for (size_t i = 0; i < candidates->size; ++i) {
        float shifted_score = fabsf(-logf(candidates->data[i].p) - entropy);
        shifted_scores.push_back(shifted_score);
    }

    // Sort tokens based on the shifted_scores and their corresponding indices
    std::vector<size_t> indices(candidates->size);
    std::iota(indices.begin(), indices.end(), 0);

    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return shifted_scores[a] < shifted_scores[b];
    });

    // Compute the cumulative probabilities
    float cum_sum = 0.0f;
    size_t last_idx = indices.size();

    for (size_t i = 0; i < indices.size(); ++i) {
        size_t idx = indices[i];
        cum_sum += candidates->data[idx].p;

        // Check if the running sum is greater than typical or if we have kept at least min_keep tokens
        if (cum_sum > p && i >= min_keep - 1) {
            last_idx = i + 1;
            break;
        }
    }

    // Resize the output vector to keep only the locally typical tokens
    std::vector<llama_token_data> new_candidates;
    for (size_t i = 0; i < last_idx; ++i) {
        size_t idx = indices[i];
        new_candidates.push_back(candidates->data[idx]);
    }

    // Replace the data in candidates with the new_candidates data
    std::copy(new_candidates.begin(), new_candidates.end(), candidates->data);
    candidates->size = new_candidates.size();

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_temperature(struct llama_context * ctx, llama_token_data_array * candidates_p, float temp) {
    const int64_t t_start_sample_us = ggml_time_us();

    for (size_t i = 0; i < candidates_p->size; ++i) {
        candidates_p->data[i].logit /= temp;
    }

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_repetition_penalty(struct llama_context * ctx, llama_token_data_array * candidates, const llama_token * last_tokens, size_t last_tokens_size, float penalty) {
    if (last_tokens_size == 0 || penalty == 1.0f) {
        return;
    }

    const int64_t t_start_sample_us = ggml_time_us();

    for (size_t i = 0; i < candidates->size; ++i) {
        const auto * token_iter = std::find(last_tokens, last_tokens + last_tokens_size, candidates->data[i].id);
        if (token_iter == last_tokens + last_tokens_size) {
            continue;
        }

        // The academic publication that described this technique actually just only divided, but that would cause tokens with negative logits to become more likely, which is obviously wrong.
        // This is common fix for this problem, which is to multiply by the penalty instead of dividing.
        if (candidates->data[i].logit <= 0) {
            candidates->data[i].logit *= penalty;
        } else {
            candidates->data[i].logit /= penalty;
        }
    }

    candidates->sorted = false;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

void llama_sample_frequency_and_presence_penalties(struct llama_context * ctx, llama_token_data_array * candidates, const llama_token * last_tokens_p, size_t last_tokens_size, float alpha_frequency, float alpha_presence) {
    if (last_tokens_size == 0 || (alpha_frequency == 0.0f && alpha_presence == 0.0f)) {
        return;
    }

    const int64_t t_start_sample_us = ggml_time_us();

    // Create a frequency map to count occurrences of each token in last_tokens
    std::unordered_map<llama_token, int> token_count;
    for (size_t i = 0; i < last_tokens_size; ++i) {
        token_count[last_tokens_p[i]]++;
    }

    // Apply frequency and presence penalties to the candidates
    for (size_t i = 0; i < candidates->size; ++i) {
        auto token_iter = token_count.find(candidates->data[i].id);
        if (token_iter == token_count.end()) {
            continue;
        }

        int count = token_iter->second;
        candidates->data[i].logit -= float(count) * alpha_frequency + float(count > 0) * alpha_presence;
    }

    candidates->sorted = false;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

static void llama_log_softmax(float * array, size_t size) {
    float max_l = *std::max_element(array, array + size);
    float sum = 0.f;
    for (size_t i = 0; i < size; ++i) {
        float p = expf(array[i] - max_l);
        sum += p;
        array[i] = p;
    }

    for (size_t i = 0; i < size; ++i) {
        array[i] = logf(array[i] / sum);
    }
}

void llama_sample_classifier_free_guidance(
          struct llama_context * ctx,
        llama_token_data_array * candidates,
          struct llama_context * guidance_ctx,
                         float   scale,
                         float   smooth_factor) {
    int64_t t_start_sample_us = ggml_time_us();

    assert(ctx);
    auto n_vocab = llama_n_vocab(ctx);
    assert(n_vocab == (int)candidates->size);
    assert(!candidates->sorted);

    std::vector<float> logits_base;
    logits_base.reserve(candidates->size);
    for (size_t i = 0; i < candidates->size; ++i) {
        logits_base.push_back(candidates->data[i].logit);
    }
    llama_log_softmax(logits_base.data(), candidates->size);

    float* logits_guidance = llama_get_logits(guidance_ctx);
    llama_log_softmax(logits_guidance, n_vocab);

    for (int i = 0; i < n_vocab; ++i) {
        float logit_guidance = logits_guidance[i];
        float logit_base = logits_base[i];
        logits_guidance[i] = scale * (logit_base - logit_guidance) + logit_guidance;
    }

    llama_log_softmax(logits_guidance, n_vocab);

    for (int i = 0; i < n_vocab; ++i) {
        float logit_base = logits_base[i];
        float logit_guidance = logits_guidance[i];

        candidates->data[i].logit = smooth_factor * logit_guidance + (1.f - smooth_factor) * logit_base;
    }

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
}

llama_token llama_sample_token_mirostat(struct llama_context * ctx, llama_token_data_array * candidates, float tau, float eta, int m, float * mu) {
    assert(ctx);
    auto N = float(llama_n_vocab(ctx));
    int64_t t_start_sample_us;
    t_start_sample_us = ggml_time_us();

    llama_sample_softmax(nullptr, candidates);

    // Estimate s_hat using the most probable m tokens
    float s_hat = 0.0;
    float sum_ti_bi = 0.0;
    float sum_ti_sq = 0.0;
    for (size_t i = 0; i < size_t(m - 1) && i < candidates->size - 1; ++i) {
        float t_i = logf(float(i + 2) / float(i + 1));
        float b_i = logf(candidates->data[i].p / candidates->data[i + 1].p);
        sum_ti_bi += t_i * b_i;
        sum_ti_sq += t_i * t_i;
    }
    s_hat = sum_ti_bi / sum_ti_sq;

    // Compute k from the estimated s_hat and target surprise value
    float epsilon_hat = s_hat - 1;
    float k = powf((epsilon_hat * powf(2, *mu)) / (1 - powf(N, -epsilon_hat)), 1 / s_hat);

    // Sample the next word X using top-k sampling
    llama_sample_top_k(nullptr, candidates, int(k), 1);
    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
    llama_token X = llama_sample_token(ctx, candidates);
    t_start_sample_us = ggml_time_us();

    // Compute error as the difference between observed surprise and target surprise value
    size_t X_idx = std::distance(candidates->data, std::find_if(candidates->data, candidates->data + candidates->size, [&](const llama_token_data & candidate) {
        return candidate.id == X;
    }));
    float observed_surprise = -log2f(candidates->data[X_idx].p);
    float e = observed_surprise - tau;

    // Update mu using the learning rate and error
    *mu = *mu - eta * e;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
    return X;
}

llama_token llama_sample_token_mirostat_v2(struct llama_context * ctx, llama_token_data_array * candidates, float tau, float eta, float * mu) {
    int64_t t_start_sample_us;
    t_start_sample_us = ggml_time_us();

    llama_sample_softmax(ctx, candidates);

    // Truncate the words with surprise values greater than mu
    candidates->size = std::distance(candidates->data, std::find_if(candidates->data, candidates->data + candidates->size, [&](const llama_token_data & candidate) {
        return -log2f(candidate.p) > *mu;
    }));

    if (candidates->size == 0) {
        candidates->size = 1;
    }

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }

    // Normalize the probabilities of the remaining words
    llama_sample_softmax(ctx, candidates);

    // Sample the next word X from the remaining words
    llama_token X = llama_sample_token(ctx, candidates);
    t_start_sample_us = ggml_time_us();

    // Compute error as the difference between observed surprise and target surprise value
    size_t X_idx = std::distance(candidates->data, std::find_if(candidates->data, candidates->data + candidates->size, [&](const llama_token_data & candidate) {
        return candidate.id == X;
    }));
    float observed_surprise = -log2f(candidates->data[X_idx].p);
    float e = observed_surprise - tau;

    // Update mu using the learning rate and error
    *mu = *mu - eta * e;

    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    }
    return X;
}

llama_token llama_sample_token_greedy(struct llama_context * ctx, llama_token_data_array * candidates) {
    const int64_t t_start_sample_us = ggml_time_us();

    // Find max element
    auto * max_iter = std::max_element(candidates->data, candidates->data + candidates->size, [](const llama_token_data & a, const llama_token_data & b) {
        return a.logit < b.logit;
    });

    llama_token result = max_iter->id;
    if (ctx) {
        ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
        ctx->n_sample++;
    }
    return result;
}

llama_token llama_sample_token(struct llama_context * ctx, llama_token_data_array * candidates) {
    assert(ctx);
    const int64_t t_start_sample_us = ggml_time_us();
    llama_sample_softmax(nullptr, candidates);

    std::vector<float> probs;
    probs.reserve(candidates->size);
    for (size_t i = 0; i < candidates->size; ++i) {
        probs.push_back(candidates->data[i].p);
    }

    std::discrete_distribution<> dist(probs.begin(), probs.end());
    auto & rng = ctx->rng;
    int idx = dist(rng);

    llama_token result = candidates->data[idx].id;

    ctx->t_sample_us += ggml_time_us() - t_start_sample_us;
    ctx->n_sample++;
    return result;
}

//
// quantization
//

static void llama_convert_tensor_internal(const llama_load_tensor & tensor, llama_buffer & output, const int nelements, const int nthread) {
    if (output.size < nelements * sizeof(float)) {
        output.resize(nelements * sizeof(float));
    }
    float * f32_output = (float *) output.addr;

    ggml_type_traits_t qtype;
    if (ggml_is_quantized(tensor.type)) {
        qtype = ggml_internal_get_type_traits(tensor.type);
        if (qtype.to_float == NULL) {
            throw std::runtime_error(format("type %s unsupported for integer quantization: no dequantization available", ggml_type_name(tensor.type)));
        }
    } else if (tensor.type != GGML_TYPE_F16) {
        throw std::runtime_error(format("cannot dequantize/convert tensor type %s", ggml_type_name(tensor.type)));
    }

    if (nthread < 2) {
        if (tensor.type == GGML_TYPE_F16) {
            ggml_fp16_to_fp32_row((ggml_fp16_t *)tensor.data, f32_output, nelements);
        } else if (ggml_is_quantized(tensor.type)) {
            qtype.to_float(tensor.data, f32_output, nelements);
        } else {
            LLAMA_ASSERT(false); // unreachable
        }
        return;
    }

    auto block_size = tensor.type == GGML_TYPE_F16 ? 1 : (size_t)ggml_blck_size(tensor.type);
    auto block_size_bytes = ggml_type_size(tensor.type);

    LLAMA_ASSERT(nelements % block_size == 0);
    auto nblocks = nelements / block_size;
    auto blocks_per_thread = nblocks / nthread;
    auto spare_blocks = nblocks - (blocks_per_thread * nthread); // if blocks aren't divisible by thread count

    std::vector<std::thread> workers;
    for (auto tnum = 0, in_buff_offs = 0, out_buff_offs = 0; tnum < nthread; tnum++) {
        auto thr_blocks = blocks_per_thread + (tnum == nthread - 1 ? spare_blocks : 0); // num blocks for this thread
        auto thr_elems = thr_blocks * block_size; // number of elements for this thread
        auto thr_block_bytes = thr_blocks * block_size_bytes; // number of input bytes for this thread

        auto compute = [qtype] (ggml_type typ, uint8_t * inbuf, float * outbuf, int nels) {
            if (typ == GGML_TYPE_F16) {
                ggml_fp16_to_fp32_row((ggml_fp16_t *)inbuf, outbuf, nels);
            } else {
                qtype.to_float(inbuf, outbuf, nels);
            }
        };
        workers.push_back(std::thread(compute, tensor.type, tensor.data + in_buff_offs, f32_output + out_buff_offs, thr_elems));
        in_buff_offs += thr_block_bytes;
        out_buff_offs += thr_elems;
    }
    for (auto & worker : workers) {
        worker.join();
    }

}

static void llama_model_quantize_internal(const std::string & fname_inp, const std::string & fname_out, const llama_model_quantize_params * params) {
    ggml_type quantized_type;
    llama_ftype ftype = params->ftype;
    int nthread = params->nthread;

    switch (params->ftype) {
        case LLAMA_FTYPE_MOSTLY_Q4_0: quantized_type = GGML_TYPE_Q4_0; break;
        case LLAMA_FTYPE_MOSTLY_Q4_1: quantized_type = GGML_TYPE_Q4_1; break;
        case LLAMA_FTYPE_MOSTLY_Q5_0: quantized_type = GGML_TYPE_Q5_0; break;
        case LLAMA_FTYPE_MOSTLY_Q5_1: quantized_type = GGML_TYPE_Q5_1; break;
        case LLAMA_FTYPE_MOSTLY_Q8_0: quantized_type = GGML_TYPE_Q8_0; break;
        case LLAMA_FTYPE_MOSTLY_F16: quantized_type = GGML_TYPE_F16; break;
        case LLAMA_FTYPE_ALL_F32: quantized_type = GGML_TYPE_F32; break;

#ifdef GGML_USE_K_QUANTS
        // K-quants
        case LLAMA_FTYPE_MOSTLY_Q2_K:   quantized_type = GGML_TYPE_Q2_K; break;
        case LLAMA_FTYPE_MOSTLY_Q3_K_S:
        case LLAMA_FTYPE_MOSTLY_Q3_K_M:
        case LLAMA_FTYPE_MOSTLY_Q3_K_L: quantized_type = GGML_TYPE_Q3_K; break;
        case LLAMA_FTYPE_MOSTLY_Q4_K_S:
        case LLAMA_FTYPE_MOSTLY_Q4_K_M: quantized_type = GGML_TYPE_Q4_K; break;
        case LLAMA_FTYPE_MOSTLY_Q5_K_S:
        case LLAMA_FTYPE_MOSTLY_Q5_K_M: quantized_type = GGML_TYPE_Q5_K; break;
        case LLAMA_FTYPE_MOSTLY_Q6_K:   quantized_type = GGML_TYPE_Q6_K; break;
#endif
        default: throw std::runtime_error(format("invalid output file type %d\n", ftype));
    }

    if (nthread <= 0) {
        nthread = std::thread::hardware_concurrency();
    }

    std::unique_ptr<llama_model_loader> model_loader(new llama_model_loader(fname_inp, /*use_mmap*/ false));
    llama_file_saver file_saver(fname_out.c_str(), model_loader->file_loader.get(), params->ftype);

#ifdef GGML_USE_K_QUANTS
    int n_attention_wv    = 0;
    int n_feed_forward_w2 = 0;
    for (auto& tensor : model_loader->tensors_map.tensors) {
        if (tensor.name.find("attention.wv.weight") != std::string::npos) {
            ++n_attention_wv;
        }
        else if (tensor.name.find("feed_forward.w2.weight") != std::string::npos) {
            ++n_feed_forward_w2;
        }
    }

    int i_attention_wv = 0;
    int i_feed_forward_w2 = 0;
#endif

    size_t total_size_org = 0;
    size_t total_size_new = 0;
    std::vector<int64_t> hist_all(1 << 4, 0);

    std::vector<std::thread> workers;
    std::mutex mutex;

    auto use_more_bits = [] (int i_layer, int num_layers) -> bool {
        return i_layer < num_layers/8 || i_layer >= 7*num_layers/8 || (i_layer - num_layers/8)%3 == 2;
    };

    size_t idx = 0;
    for (llama_load_tensor & tensor : model_loader->tensors_map.tensors) {
        llama_buffer read_data;
        read_data.resize(tensor.size);
        tensor.data = read_data.addr;
        model_loader->load_data_for(tensor);

        printf("[%4zu/%4zu] %36s - %16s, type = %6s, ",
               ++idx, model_loader->tensors_map.tensors.size(),
               tensor.name.c_str(), llama_format_tensor_shape(tensor.ne).c_str(),
               ggml_type_name(tensor.type));

        // This used to be a regex, but <regex> has an extreme cost to compile times.
        bool quantize = tensor.name.rfind("weight") == tensor.name.size() - 6; // ends with 'weight'?

        // quantize only 2D tensors
        quantize &= (tensor.ne.size() == 2);
        quantize &= params->quantize_output_tensor || tensor.name != "output.weight";
        quantize &= quantized_type != tensor.type;

        enum ggml_type new_type;
        void * new_data;
        size_t new_size;
        llama_buffer work;

        if (!quantize) {
            new_type = tensor.type;
            new_data = tensor.data;
            new_size = tensor.size;
            printf("size = %8.3f MB\n", tensor.size/1024.0/1024.0);
        } else {
            new_type = quantized_type;
#ifdef GGML_USE_K_QUANTS
            bool convert_incompatible_tensor = false;
            if (quantized_type == GGML_TYPE_Q2_K || quantized_type == GGML_TYPE_Q3_K || quantized_type == GGML_TYPE_Q4_K ||
                quantized_type == GGML_TYPE_Q5_K || quantized_type == GGML_TYPE_Q6_K) {
                int nx = tensor.ne.at(0);
                int ny = tensor.ne.at(1);
                if (nx % QK_K != 0 || ny % QK_K != 0) {
                    fprintf(stderr, "\n\nTensor sizes %d x %d are not divisible by %d, required for k-quants.\n",nx,ny,QK_K);
                    convert_incompatible_tensor = true;
                }
            }
            if (tensor.name == "output.weight") {
                int nx = tensor.ne.at(0);
                int ny = tensor.ne.at(1);
                if (nx % QK_K == 0 && ny % QK_K == 0) {
                    new_type = GGML_TYPE_Q6_K;
                }
            } else if (tensor.name.find("attention.wv.weight") != std::string::npos) {
                if      (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = GGML_TYPE_Q4_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) new_type = GGML_TYPE_Q5_K;
                else if ((ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q5_K_M) &&
                        use_more_bits(i_attention_wv, n_attention_wv)) new_type = GGML_TYPE_Q6_K;
                else if (QK_K == 64 && (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_S || ftype == LLAMA_FTYPE_MOSTLY_Q3_K_S) &&
                        (i_attention_wv < n_attention_wv/8 || i_attention_wv >= 7*n_attention_wv/8)) new_type = GGML_TYPE_Q6_K;
                ++i_attention_wv;
            } else if (tensor.name.find("feed_forward.w2.weight") != std::string::npos) {
                if      (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = GGML_TYPE_Q4_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) new_type = GGML_TYPE_Q5_K;
                else if ((ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q5_K_M) &&
                         use_more_bits(i_feed_forward_w2, n_feed_forward_w2)) new_type = GGML_TYPE_Q6_K;
                //else if (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_S && i_feed_forward_w2 < n_feed_forward_w2/8) new_type = GGML_TYPE_Q6_K;
                ++i_feed_forward_w2;
            } else if (tensor.name.find("attention.wo.weight") != std::string::npos) {
                if      (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = GGML_TYPE_Q4_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) new_type = GGML_TYPE_Q5_K;
            }
            if (convert_incompatible_tensor) {
                if (tensor.name == "output.weight") {
                    new_type = GGML_TYPE_F16; //fall back to F16 instead of just failing.
                    fprintf(stderr, "F16 will be used for this tensor instead.\n");
                } else if (tensor.name == "tok_embeddings.weight") {
                    new_type = GGML_TYPE_Q4_0; //fall back to Q4_0 instead of just failing.
                    fprintf(stderr, "Q4_0 will be used for this tensor instead.\n");
                } else {
                    throw std::runtime_error("Unsupported tensor size encountered\n");
                }
            }
#endif

            float * f32_data;
            size_t nelements = tensor.ne.at(0) * tensor.ne.at(1);
            llama_buffer f32_conv_buf;

            if (tensor.type == GGML_TYPE_F32) {
                f32_data = (float *) tensor.data;
            } else if (ggml_is_quantized(tensor.type) && !params->allow_requantize) {
                throw std::runtime_error(format("requantizing from type %s is disabled", ggml_type_name(tensor.type)));
            } else {
                llama_convert_tensor_internal(tensor, f32_conv_buf, nelements, nthread);
                f32_data = (float *) f32_conv_buf.addr;
            }

            printf("quantizing .. ");
            fflush(stdout);

            work.resize(nelements * 4); // upper bound on size
            new_data = work.addr;
            std::vector<int64_t> hist_cur(1 << 4, 0);

            int chunk_size = 32 * 512;
            const int nchunk = (nelements + chunk_size - 1)/chunk_size;
            const int nthread_use = nthread > 1 ? std::max(1, std::min(nthread, nchunk)) : 1;
            if (nthread_use < 2) {
                new_size = ggml_quantize_chunk(new_type, f32_data, new_data, 0, nelements, hist_cur.data());
            } else {
                size_t counter = 0;
                new_size = 0;
                auto compute = [&mutex, &counter, &hist_cur, &new_size, new_type, f32_data, new_data, nelements, chunk_size] () {
                    std::vector<int64_t> local_hist;
                    size_t local_size = 0;
                    while (true) {
                        std::unique_lock<std::mutex> lock(mutex);
                        size_t first = counter; counter += chunk_size;
                        if (first >= nelements) {
                            if (!local_hist.empty()) {
                                for (int j=0; j<int(local_hist.size()); ++j) {
                                    hist_cur[j] += local_hist[j];
                                }
                                new_size += local_size;
                            }
                            break;
                        }
                        lock.unlock();
                        size_t last = std::min(nelements, first + chunk_size);
                        if (local_hist.empty()) {
                            local_hist.resize(hist_cur.size(), 0);
                        }
                        local_size += ggml_quantize_chunk(new_type, f32_data, new_data, first, last - first, local_hist.data());
                    }
                };
                if ((int) workers.size() < nthread_use - 1) {
                    workers.resize(nthread_use - 1);
                }
                for (int it = 0; it < nthread_use - 1; ++it) {
                    workers[it] = std::thread(compute);
                }
                compute();
                for (int it = 0; it < nthread_use - 1; ++it) {
                    workers[it].join();
                }
            }

            printf("size = %8.2f MB -> %8.2f MB | hist: ", tensor.size/1024.0/1024.0, new_size/1024.0/1024.0);
            int64_t tot_count = 0;
            for (size_t i = 0; i < hist_cur.size(); i++) {
                hist_all[i] += hist_cur[i];
                tot_count += hist_cur[i];
            }

            if (tot_count > 0) {
                for (size_t i = 0; i < hist_cur.size(); i++) {
                    printf("%5.3f ", hist_cur[i] / float(nelements));
                }
            }
            printf("\n");
        }
        total_size_org += tensor.size;
        total_size_new += new_size;
        file_saver.write_tensor(tensor, new_type, new_data, new_size);
    }

    printf("%s: model size  = %8.2f MB\n", __func__, total_size_org/1024.0/1024.0);
    printf("%s: quant size  = %8.2f MB\n", __func__, total_size_new/1024.0/1024.0);

    {
        int64_t sum_all = 0;
        for (size_t i = 0; i < hist_all.size(); i++) {
            sum_all += hist_all[i];
        }

        if (sum_all > 0) {
            printf("%s: hist: ", __func__);
            for (size_t i = 0; i < hist_all.size(); i++) {
                printf("%5.3f ", hist_all[i] / float(sum_all));
            }
            printf("\n");
        }
    }
}



//
// interface implementation
//

struct llama_model * llama_load_model_from_file(
                             const char * path_model,
            struct llama_context_params   params) {
    ggml_time_init();

    llama_model * model = new llama_model;

    ggml_type memory_type = params.f16_kv ? GGML_TYPE_F16 : GGML_TYPE_F32;

    if (!llama_model_load(path_model, *model, model->vocab, params.n_ctx, params.n_batch, params.n_gpu_layers,
                params.main_gpu, params.tensor_split, params.rope_freq_base, params.rope_freq_scale,params.low_vram,
                memory_type, params.use_mmap, params.use_mlock, params.vocab_only, params.progress_callback,
                params.progress_callback_user_data)) {
        delete model;
        fprintf(stderr, "%s: failed to load model\n", __func__);
        return nullptr;
    }

    return model;
}

void llama_free_model(struct llama_model * model) {
    delete model;
}

struct llama_context * llama_new_context_with_model(
                 struct llama_model * model,
        struct llama_context_params   params) {

    if (!model) {
        return nullptr;
    }

    llama_context * ctx = new llama_context(*model);

    if (params.seed == LLAMA_DEFAULT_SEED) {
        params.seed = time(NULL);
    }

    if (params.n_ctx < 1) {
        fprintf(stderr, "%s: invalid n_ctx = %d\n", __func__, params.n_ctx);
        return nullptr;
    }

    unsigned cur_percentage = 0;
    if (params.progress_callback == NULL) {
        params.progress_callback_user_data = &cur_percentage;
        params.progress_callback = [](float progress, void * ctx) {
            unsigned * cur_percentage_p = (unsigned *) ctx;
            unsigned percentage = (unsigned) (100 * progress);
            while (percentage > *cur_percentage_p) {
                *cur_percentage_p = percentage;
                fprintf(stderr, ".");
                fflush(stderr);
                if (percentage >= 100) {
                    fprintf(stderr, "\n");
                }
            }
        };
    }

    ctx->rng = std::mt19937(params.seed);
    ctx->logits_all = params.logits_all;

    // TODO: choose backend depending on n_layers/low_vram
#ifdef GGML_USE_CUDA
    if ((uint32_t)params.n_gpu_layers >= model->hparams.n_layer/2 && !params.low_vram) {
        ctx->backend_kv = model->backend_cuda;
    } else {
        ctx->backend_kv = model->backend_cpu;
    }
#else
    ctx->backend_kv = model->backend_cpu;
#endif

    ggml_type memory_type = params.f16_kv ? GGML_TYPE_F16 : GGML_TYPE_F32;

    // reserve memory for context buffers
    if (!params.vocab_only) {
        if (!kv_cache_init(ctx->backend_kv, ctx->model.hparams, ctx->kv_self, memory_type, ctx->model.hparams.n_ctx)) {
            fprintf(stderr, "%s: kv_cache_init() failed for self-attention cache\n", __func__);
            llama_free(ctx);
            return nullptr;
        }

        {
            const size_t memory_size = ggml_nbytes(ctx->kv_self.k) + ggml_nbytes(ctx->kv_self.v);
            fprintf(stderr, "%s: kv self size  = %7.2f MB\n", __func__, memory_size / 1024.0 / 1024.0);
        }

        const auto & hparams = ctx->model.hparams;

        if (params.embedding) {
            ctx->embedding.resize(hparams.n_embd);
        }

        // TODO: size the buffers more accurately - depends on improved memory management
        ctx->buf_compute_cpu = ggml_buffer_alloc(model->backend_cpu, MEM_REQ_EVAL().at(ctx->model.type), 2048);

        // TODO: pinned memory for faster host-device transfers
        //ggml_cuda_host_register(*(void**)ctx->buf_compute_cpu.backend_buffer, MEM_REQ_EVAL().at(ctx->model.type) + 128*2048);
#ifdef GGML_USE_CUDA
        if (params.n_gpu_layers > 0) {
            ctx->buf_compute_cuda = ggml_buffer_alloc(model->backend_cuda, MEM_REQ_EVAL().at(ctx->model.type), 2048);
        }
#endif
#ifdef GGML_USE_METAL
        if (params.n_gpu_layers > 0) {
            ctx->buf_compute_metal = ggml_buffer_alloc(model->backend_metal, MEM_REQ_EVAL().at(ctx->model.type), 2048);
        }
#endif

        // initialize the graph input/output buffers
        // input buffer
        {
            size_t buf_input_size = 0;
            buf_input_size += hparams.n_ctx * ggml_type_size(GGML_TYPE_F32); // input tokens
            // TODO: input embeddings should be optional to save memory
            buf_input_size += hparams.n_embd * hparams.n_ctx * ggml_type_size(GGML_TYPE_F32); // input embeddings
            ctx->buf_input = ggml_buffer_alloc(model->backend_inp, buf_input_size, 2);

            struct ggml_init_params ggml_params = ggml_init_params_default();
            ggml_params.buffer = ctx->buf_input;
            ggml_context * ctx0 = ggml_init(ggml_params);

            ctx->graph_tokens_in = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, hparams.n_ctx);
            ggml_set_name(ctx->graph_tokens_in, "tokens_in");
            ctx->graph_embeddings_in = ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, hparams.n_embd, hparams.n_ctx);
            ggml_set_name(ctx->graph_embeddings_in, "embeddings_in");

            ggml_free(ctx0);
        }
        // output buffer
        {
            size_t buf_output_size = 0;
            if (params.logits_all) {
                buf_output_size += hparams.n_ctx * hparams.n_vocab * ggml_type_size(GGML_TYPE_F32);
            } else {
                buf_output_size += hparams.n_vocab * ggml_type_size(GGML_TYPE_F32);
            }
            if (params.embedding) {
                buf_output_size += hparams.n_embd * ggml_type_size(GGML_TYPE_F32);
            }
            ctx->buf_output = ggml_buffer_alloc(model->backend_out, buf_output_size, 2);

            struct ggml_init_params ggml_params = ggml_init_params_default();
            ggml_params.buffer = ctx->buf_output;
            ggml_context * ctx0 = ggml_init(ggml_params);

            ctx->graph_logits = ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, hparams.n_vocab, params.logits_all ? hparams.n_ctx : 1);
            ggml_set_name(ctx->graph_logits, "logits");
            if (params.embedding) {
                ctx->graph_embeddings_out = ggml_new_tensor_1d(ctx0, GGML_TYPE_F32, hparams.n_embd);
                ggml_set_name(ctx->graph_embeddings_out, "embeddings_out");
            }

            ggml_free(ctx0);
        }

        // resized during inference
        if (params.logits_all) {
            ctx->logits.reserve(hparams.n_ctx*hparams.n_vocab);
        } else {
            ctx->logits.reserve(hparams.n_vocab);
        }

        if (params.embedding){
            ctx->embedding.resize(hparams.n_embd);
        }
    }

    fprintf(stderr, "%s: layer backends: ", __func__);
    fprintf(stderr, "input: %s, ", ggml_backend_name(ctx->model.backend_inp));

    int start = 0;
    struct ggml_backend * prev_backend = ctx->model.backend_layers[0];
    for (int i = 1; i <= (int)ctx->model.hparams.n_layer; i++) {
        if (i == (int)ctx->model.hparams.n_layer || ctx->model.backend_layers[i] != prev_backend) {
            if (start == i - 1) {
                fprintf(stderr, "layer %d: %s, ", start, ggml_backend_name(prev_backend));
            } else {
                fprintf(stderr, "layers %d-%d: %s, ", start, i - 1, ggml_backend_name(prev_backend));
            }
            start = i;
            prev_backend = ctx->model.backend_layers[i];
        }
    }
    fprintf(stderr, "output: %s, ", ggml_backend_name(ctx->model.backend_out));
    fprintf(stderr, "kv: %s\n", ggml_backend_name(ctx->backend_kv));

#ifdef GGML_USE_MPI
    ctx->ctx_mpi = ggml_mpi_init();

    if (ggml_mpi_rank(ctx->ctx_mpi) > 0) {
        // Enter a blocking eval loop with dummy input, letting rank=0 drive the process
        const std::vector<llama_token> tmp(ctx->model.hparams.n_ctx, llama_token_bos());
        while (!llama_eval(ctx, tmp.data(), tmp.size(), 0, 0)) {};
        llama_backend_free();
        exit(1);
    }
#endif

    return ctx;
}

struct llama_context * llama_init_from_file(
                             const char * path_model,
            struct llama_context_params   params) {

    struct llama_model * model = llama_load_model_from_file(path_model, params);
    if (!model) {
        return nullptr;
    }
    struct llama_context * ctx = llama_new_context_with_model(model, params);
    ctx->model_owner = true;
    return ctx;
}

void llama_free(struct llama_context * ctx) {
    // TODO: free buffers - move this to destructor like llama_model
    if (ctx->model_owner) {
        delete &ctx->model;
    }
    delete ctx;
}

int llama_model_quantize(
        const char * fname_inp,
        const char * fname_out,
        const llama_model_quantize_params *params) {
    try {
        llama_model_quantize_internal(fname_inp, fname_out, params);
        return 0;
    } catch (const std::exception & err) {
        fprintf(stderr, "%s: failed to quantize: %s\n", __func__, err.what());
        return 1;
    }
}

int llama_apply_lora_from_file_internal(const struct llama_model & model, const char * path_lora, const char * path_base_model, int n_threads) {
    (void) model;
    (void) path_lora;
    (void) path_base_model;
    (void) n_threads;
    LLAMA_ASSERT(false);
#if 0
    fprintf(stderr, "%s: applying lora adapter from '%s' - please wait ...\n", __func__, path_lora);

    const int64_t t_start_lora_us = ggml_time_us();

    auto fin = std::ifstream(path_lora, std::ios::binary);
    if (!fin) {
        fprintf(stderr, "%s: failed to open '%s'\n", __func__, path_lora);
        return 1;
    }

    // verify magic and version
    {
        uint32_t magic;
        fin.read((char *) &magic, sizeof(magic));
        if (magic != LLAMA_FILE_MAGIC_GGLA) {
            fprintf(stderr, "%s: bad file magic\n", __func__);
            return 1;
        }
        uint32_t format_version;
        fin.read((char *) &format_version, sizeof(format_version));

        if (format_version != 1) {
            fprintf(stderr, "%s: unsupported file version\n", __func__ );
            return 1;
        }
    }

    int32_t lora_r;
    int32_t lora_alpha;
    fin.read((char *) &lora_r, sizeof(lora_r));
    fin.read((char *) &lora_alpha, sizeof(lora_alpha));
    float scaling = (float)lora_alpha / (float)lora_r;

    fprintf(stderr, "%s: r = %d, alpha = %d, scaling = %.2f\n", __func__, lora_r, lora_alpha, scaling);


    // create a temporary ggml context to store the lora tensors
    // todo: calculate size from biggest possible tensor
    std::vector<uint8_t> lora_buf(1024ull * 1024ull * 1024ull);
    struct ggml_init_params params = ggml_init_params_default();
    params.mem_size   = lora_buf.size();
    params.mem_buffer = lora_buf.data();
    params.no_alloc   = false;

    ggml_context * lora_ctx = ggml_init(params);
    std::unordered_map<std::string, struct ggml_tensor *> lora_tensors;

    // create a name -> tensor map of the model to accelerate lookups
    std::unordered_map<std::string, struct ggml_tensor*> model_tensors;
    for (const auto & kv: model.tensors_by_name) {
        model_tensors.insert(kv);
    }


    // load base model
    std::unique_ptr<llama_model_loader> model_loader;
    ggml_context * base_ctx = NULL;
    llama_buffer base_buf;
    if (path_base_model) {
        fprintf(stderr, "%s: loading base model from '%s'\n", __func__, path_base_model);
        model_loader.reset(new llama_model_loader(path_base_model, /*use_mmap*/ true));

        size_t ctx_size;
        size_t mmapped_size;
        model_loader->calc_sizes(&ctx_size, &mmapped_size);
        base_buf.resize(ctx_size);

        ggml_init_params base_params = ggml_init_params_default();
        base_params.mem_size   = base_buf.size;
        base_params.mem_buffer = base_buf.addr;
        base_params.no_alloc   = model_loader->use_mmap;

        base_ctx = ggml_init(base_params);

        model_loader->ggml_ctx = base_ctx;

        // maybe this should in llama_model_loader
        if (model_loader->use_mmap) {
            model_loader->mapping.reset(new llama_mmap(&model_loader->file_loader->file, /* prefetch */ 0, ggml_is_numa()));
        }
    }

    // read tensors and apply
    bool warned = false;
    int n_tensors = 0;

    std::vector<uint8_t> work_buffer;

    while (true) {
        int32_t n_dims;
        int32_t length;
        int32_t ftype;

        fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
        fin.read(reinterpret_cast<char *>(&length), sizeof(length));
        fin.read(reinterpret_cast<char *>(&ftype),  sizeof(ftype));
        if (fin.eof()) {
            break;
        }

        int32_t ne[2] = { 1, 1 };
        for (int i = 0; i < n_dims; ++i) {
            fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
        }

        std::string name;
        {
            char buf[1024];
            fin.read(buf, length);
            name = std::string(buf, length);
        }

        // check for lora suffix and get the type of tensor
        const std::string lora_suffix = ".lora";
        size_t pos = name.rfind(lora_suffix);
        if (pos == std::string::npos) {
            fprintf(stderr, "%s: error: '%s' is not a lora tensor\n", __func__, name.c_str());
            return 1;
        }

        std::string lora_type = name.substr(pos + lora_suffix.length());
        std::string base_name = name;
        base_name.erase(pos);
        // fprintf(stderr, "%s: %s => %s (lora type %s) ", __func__, name.c_str(),base_name.c_str(), lora_type.c_str());

        if (model_tensors.find(base_name) == model_tensors.end()) {
            fprintf(stderr, "%s: unknown tensor '%s' in lora adapter\n", __func__, name.data());
            return 1;
        }

        // create ggml tensor
        ggml_type wtype;
        switch (ftype) {
            case 0: wtype = GGML_TYPE_F32;  break;
            case 1: wtype = GGML_TYPE_F16;  break;
            default:
                    {
                        fprintf(stderr, "%s: invalid tensor data type '%d'\n",
                                __func__, ftype);
                        return false;
                    }
        }
        ggml_tensor * lora_tensor;
        if (n_dims == 2) {
            lora_tensor = ggml_new_tensor_2d(lora_ctx, wtype, ne[0], ne[1]);
        }
        else {
            fprintf(stderr, "%s: unsupported tensor dimension %d\n", __func__, n_dims);
            return 1;
        }
        ggml_set_name(lora_tensor, "lora_tensor");

        // load tensor data
        size_t offset = fin.tellg();
        size_t tensor_data_size = ggml_nbytes(lora_tensor);
        offset = (offset + 31) & -32;
        fin.seekg(offset);
        fin.read((char*)lora_tensor->data, tensor_data_size);

        lora_tensors[name] = lora_tensor;

        // check if we have both A and B tensors and apply
        if (lora_tensors.find(base_name + ".loraA") != lora_tensors.end() &&
            lora_tensors.find(base_name + ".loraB") != lora_tensors.end()) {

            ggml_tensor * dest_t = model_tensors[base_name];

            ggml_tensor * base_t;
            if (model_loader) {
                // load from base model
                if (model_loader->tensors_map.name_to_idx.find(base_name) == model_loader->tensors_map.name_to_idx.end()) {
                    fprintf(stderr, "%s: error: tensor '%s' not found in base model\n", __func__, base_name.c_str());
                    return 1;
                }
                size_t idx = model_loader->tensors_map.name_to_idx[base_name];
                llama_load_tensor & lt = model_loader->tensors_map.tensors[idx];
                base_t = model_loader->get_tensor(base_name, { (uint32_t)dest_t->ne[0], (uint32_t)dest_t->ne[1] }, GGML_BACKEND_CPU);
                lt.data = (uint8_t *) lt.ggml_tensor->data;
                model_loader->load_data_for(lt);
                lt.ggml_tensor->data = lt.data;
            }
            else {
                base_t = dest_t;
            }

            if (ggml_is_quantized(base_t->type)) {
                if (!warned) {
                    fprintf(stderr, "%s: warning: using a lora adapter with a quantized model may result in poor quality, "
                                    "use a f16 or f32 base model with --lora-base\n", __func__);
                    warned = true;
                }
            }

            ggml_tensor * loraA = lora_tensors[base_name + ".loraA"];
            GGML_ASSERT(loraA->type == GGML_TYPE_F32);
            ggml_set_name(loraA, "loraA");

            ggml_tensor * loraB = lora_tensors[base_name + ".loraB"];
            GGML_ASSERT(loraB->type == GGML_TYPE_F32);
            ggml_set_name(loraB, "loraB");

            if (base_t->ne[0] != loraA->ne[1] || base_t->ne[1] != loraB->ne[1]) {
                fprintf(stderr, "%s: incompatible tensor dimensions (%" PRId64 " and %" PRId64 ");"
                               " are you sure that this adapter is for this model?\n", __func__, base_t->ne[0], loraA->ne[1]);
                return 1;
            }

            // w = w + BA*s
            ggml_tensor * BA = ggml_mul_mat(lora_ctx, loraA, loraB);
            ggml_set_name(BA, "BA");

            if (scaling != 1.0f) {
                ggml_tensor * scale_tensor = ggml_new_f32(lora_ctx, scaling);
                ggml_set_name(scale_tensor, "scale_tensor");

                BA = ggml_scale_inplace(lora_ctx, BA, scale_tensor);
                ggml_set_name(BA, "BA_scaled");
            }

            ggml_tensor * r;
            if (base_t == dest_t) {
                r = ggml_add_inplace(lora_ctx, dest_t, BA);
                ggml_set_name(r, "r_add_inplace");
            }
            else {
                r = ggml_add(lora_ctx, base_t, BA);
                ggml_set_name(r, "r_add");

                r = ggml_cpy(lora_ctx, r, dest_t);
                ggml_set_name(r, "r_cpy");
            }

            struct ggml_cgraph gf = ggml_build_forward(r);

            ggml_graph_compute_helper(work_buffer, &gf, n_threads);

            // we won't need these tensors again, reset the context to save memory
            ggml_free(lora_ctx);
            lora_ctx = ggml_init(params);
            lora_tensors.clear();

            n_tensors++;
            if (n_tensors % 4 == 0) {
                fprintf(stderr, ".");
            }
        }
    }

    // TODO: this should be in a destructor, it will leak on failure
    ggml_free(lora_ctx);
    if (base_ctx) {
        ggml_free(base_ctx);
    }

    const int64_t t_lora_us = ggml_time_us() - t_start_lora_us;
    fprintf(stderr, " done (%.2f ms)\n", t_lora_us / 1000.0);

#endif
    return 0;
}

int llama_apply_lora_from_file(struct llama_context * ctx, const char * path_lora, const char * path_base_model, int n_threads) {
    try {
        return llama_apply_lora_from_file_internal(ctx->model, path_lora, path_base_model, n_threads);
    } catch (const std::exception & err) {
        fprintf(stderr, "%s: failed to apply lora adapter: %s\n", __func__, err.what());
        return 1;
    }
}

int llama_model_apply_lora_from_file(const struct llama_model * model, const char * path_lora, const char * path_base_model, int n_threads) {
    try {
        return llama_apply_lora_from_file_internal(*model, path_lora, path_base_model, n_threads);
    } catch (const std::exception & err) {
        fprintf(stderr, "%s: failed to apply lora adapter: %s\n", __func__, err.what());
        return 1;
    }
}

int llama_get_kv_cache_token_count(const struct llama_context * ctx) {
    return ctx->kv_self.n;
}

#define LLAMA_MAX_RNG_STATE (64*1024)

void llama_set_rng_seed(struct llama_context * ctx, uint32_t seed) {
    if (seed == LLAMA_DEFAULT_SEED) {
        seed = time(NULL);
    }
    ctx->rng.seed(seed);
}

// Returns the *maximum* size of the state
size_t llama_get_state_size(const struct llama_context * ctx) {
#if 0
    // we don't know size of rng until we actually serialize it. so reserve more than enough memory for its serialized state.
    // for reference, std::mt19937(1337) serializes to 6701 bytes.
    const size_t s_rng_size        = sizeof(size_t);
    const size_t s_rng             = LLAMA_MAX_RNG_STATE;
    const size_t s_logits_capacity = sizeof(size_t);
    const size_t s_logits_size     = sizeof(size_t);
    const size_t s_logits          = ggml_nbytes(ctx->graph_logits);
    const size_t s_embedding_size  = sizeof(size_t);
    const size_t s_embedding       = ctx->embedding.size() * sizeof(float);
    const size_t s_kv_size         = sizeof(size_t);
    const size_t s_kv_ntok         = sizeof(int);
    const size_t s_kv              = ctx->kv_self.buf.size;

    const size_t s_total = (
        + s_rng_size
        + s_rng
        + s_logits_capacity
        + s_logits_size
        + s_logits
        + s_embedding_size
        + s_embedding
        + s_kv_size
        + s_kv_ntok
        + s_kv
    );

    return s_total;
#endif
}

// Copies the state to the specified destination address
size_t llama_copy_state_data(struct llama_context * ctx, uint8_t * dst) {
#if 0
    uint8_t * out = dst;

    // copy rng
    {
        std::stringstream rng_ss;
        rng_ss << ctx->rng;

        const size_t rng_size = rng_ss.str().size();
        char rng_buf[LLAMA_MAX_RNG_STATE];

        memset(&rng_buf[0], 0, LLAMA_MAX_RNG_STATE);
        memcpy(&rng_buf[0], rng_ss.str().data(), rng_ss.str().size());

        memcpy(out, &rng_size,   sizeof(rng_size));    out += sizeof(rng_size);
        memcpy(out, &rng_buf[0], LLAMA_MAX_RNG_STATE); out += LLAMA_MAX_RNG_STATE;
    }

    // copy logits
    {
        const size_t logits_size = ggml_nelements(ctx->graph_logits);

        memcpy(out, &logits_size, sizeof(logits_size)); out += sizeof(logits_size);

        if (logits_size) {
            memcpy(out, ggml_get_data(ctx->graph_logits), logits_size * sizeof(float));
            out += logits_size * sizeof(float);
        }
    }

    // copy embeddings
    {
        const size_t embedding_size = ctx->embedding.size();

        memcpy(out, &embedding_size, sizeof(embedding_size)); out += sizeof(embedding_size);

        if (embedding_size) {
            memcpy(out, ctx->embedding.data(), embedding_size * sizeof(float));
            out += embedding_size * sizeof(float);
        }
    }

    // copy kv cache
    {
        const auto & kv_self = ctx->kv_self;
        const auto & hparams = ctx->model.hparams;
        const int    n_layer = hparams.n_layer;
        const int    n_embd  = hparams.n_embd;
        const int    n_ctx   = hparams.n_ctx;

        const size_t kv_size = kv_self.buf.size;
        const int    kv_ntok = llama_get_kv_cache_token_count(ctx);

        memcpy(out, &kv_size, sizeof(kv_size)); out += sizeof(kv_size);
        memcpy(out, &kv_ntok, sizeof(kv_ntok)); out += sizeof(kv_ntok);

        if (kv_size) {
            const size_t elt_size = ggml_element_size(kv_self.k);

            ggml_init_params params = ggml_init_params_default();
            params.mem_size   = 4096;
            params.mem_buffer = NULL;
            params.no_alloc   = true;
            ggml_context * cpy_ctx = ggml_init(params);
            ggml_cgraph gf{};

            ggml_tensor * kout3d = ggml_new_tensor_3d(cpy_ctx, kv_self.k->type, n_embd, kv_ntok, n_layer);
            kout3d->data = out;
            out += ggml_nbytes(kout3d);

            ggml_tensor * vout3d = ggml_new_tensor_3d(cpy_ctx, kv_self.v->type, kv_ntok, n_embd, n_layer);
            vout3d->data = out;
            out += ggml_nbytes(vout3d);

            ggml_tensor * k3d = ggml_view_3d(cpy_ctx, kv_self.k,
                n_embd, kv_ntok, n_layer,
                elt_size*n_embd, elt_size*n_embd*n_ctx, 0);

            ggml_tensor * v3d = ggml_view_3d(cpy_ctx, kv_self.v,
                kv_ntok, n_embd, n_layer,
                elt_size*n_ctx, elt_size*n_ctx*n_embd, 0);

            ggml_build_forward_expand(&gf, ggml_cpy(cpy_ctx, k3d, kout3d));
            ggml_build_forward_expand(&gf, ggml_cpy(cpy_ctx, v3d, vout3d));
            ggml_graph_compute_helper(ctx->work_buffer, &gf, /*n_threads*/ 1);

            ggml_free(cpy_ctx);
        }
    }

    const size_t written  = out - dst;
    const size_t max_size = llama_get_state_size(ctx);

    LLAMA_ASSERT(written <= max_size);

    return written;
#endif
}

// Sets the state reading from the specified source address
size_t llama_set_state_data(struct llama_context * ctx, uint8_t * src) {
#if 0
    uint8_t * inp = src;

    // set rng
    {
        size_t rng_size;
        char   rng_buf[LLAMA_MAX_RNG_STATE];

        memcpy(&rng_size,   inp, sizeof(rng_size));    inp += sizeof(rng_size);
        memcpy(&rng_buf[0], inp, LLAMA_MAX_RNG_STATE); inp += LLAMA_MAX_RNG_STATE;

        std::stringstream rng_ss;
        rng_ss.str(std::string(&rng_buf[0], rng_size));
        rng_ss >> ctx->rng;

        LLAMA_ASSERT(rng_ss.fail() == false);
    }

    // set logits
    {
        size_t logits_size;

        memcpy(&logits_size, inp, sizeof(logits_size)); inp += sizeof(logits_size);

        LLAMA_ASSERT((size_t)ggml_nelements(ctx->graph_logits) == logits_size);

        if (logits_size) {
            memcpy(ggml_get_data(ctx->graph_logits), inp, logits_size * sizeof(float));
            inp += logits_size * sizeof(float);
        }
    }

    // set embeddings
    {
        size_t embedding_size;

        memcpy(&embedding_size, inp, sizeof(embedding_size)); inp += sizeof(embedding_size);

        LLAMA_ASSERT(ctx->embedding.capacity() == embedding_size);

        if (embedding_size) {
            memcpy(ctx->embedding.data(), inp, embedding_size * sizeof(float));
            inp += embedding_size * sizeof(float);
        }
    }

    // set kv cache
    {
        const auto & kv_self = ctx->kv_self;
        const auto & hparams = ctx->model.hparams;
        const int    n_layer = hparams.n_layer;
        const int    n_embd  = hparams.n_embd;
        const int    n_ctx   = hparams.n_ctx;

        size_t kv_size;
        int kv_ntok;

        memcpy(&kv_size, inp, sizeof(kv_size)); inp += sizeof(kv_size);
        memcpy(&kv_ntok, inp, sizeof(kv_ntok)); inp += sizeof(kv_ntok);

        if (kv_size) {
            LLAMA_ASSERT(kv_self.buf.size == kv_size);

            const size_t elt_size = ggml_element_size(kv_self.k);

            ggml_init_params params = ggml_init_params_default();
            params.mem_size   = 4096;
            params.mem_buffer = NULL;
            params.no_alloc   = true;
            ggml_context * cpy_ctx = ggml_init(params);
            ggml_cgraph gf{};

            ggml_tensor * kin3d = ggml_new_tensor_3d(cpy_ctx, kv_self.k->type, n_embd, kv_ntok, n_layer);
            kin3d->data = (void *) inp;
            inp += ggml_nbytes(kin3d);

            ggml_tensor * vin3d = ggml_new_tensor_3d(cpy_ctx, kv_self.v->type, kv_ntok, n_embd, n_layer);
            vin3d->data = (void *) inp;
            inp += ggml_nbytes(vin3d);

            ggml_tensor * k3d = ggml_view_3d(cpy_ctx, kv_self.k,
                n_embd, kv_ntok, n_layer,
                elt_size*n_embd, elt_size*n_embd*n_ctx, 0);

            ggml_tensor * v3d = ggml_view_3d(cpy_ctx, kv_self.v,
                kv_ntok, n_embd, n_layer,
                elt_size*n_ctx, elt_size*n_ctx*n_embd, 0);

            ggml_build_forward_expand(&gf, ggml_cpy(cpy_ctx, kin3d, k3d));
            ggml_build_forward_expand(&gf, ggml_cpy(cpy_ctx, vin3d, v3d));
            ggml_graph_compute_helper(ctx->work_buffer, &gf, /*n_threads*/ 1);

            ggml_free(cpy_ctx);
        }

        ctx->kv_self.n = kv_ntok;
    }

    const size_t nread    = inp - src;
    const size_t max_size = llama_get_state_size(ctx);

    LLAMA_ASSERT(nread <= max_size);

    return nread;
#endif
}

static bool llama_load_session_file_internal(struct llama_context * ctx, const char * path_session, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    llama_file file(path_session, "rb");

    // sanity checks
    {
        const uint32_t magic   = file.read_u32();
        const uint32_t version = file.read_u32();

        if (magic != LLAMA_SESSION_MAGIC || version != LLAMA_SESSION_VERSION) {
            fprintf(stderr, "%s : unknown (magic, version) for session file: %08x, %08x\n", __func__, magic, version);
            return false;
        }

        llama_hparams session_hparams;
        file.read_raw(&session_hparams, sizeof(llama_hparams));

        if (session_hparams != ctx->model.hparams) {
            fprintf(stderr, "%s : model hparams didn't match from session file!\n", __func__);
            return false;
        }
    }

    // load the prompt
    {
        const uint32_t n_token_count = file.read_u32();

        if (n_token_count > n_token_capacity) {
            fprintf(stderr, "%s : token count in session file exceeded capacity! %u > %zu\n", __func__, n_token_count, n_token_capacity);
            return false;
        }

        file.read_raw(tokens_out, sizeof(llama_token) * n_token_count);
        *n_token_count_out = n_token_count;
    }

    // restore the context state
    {
        const size_t n_state_size_cur = file.size - file.tell();
        const size_t n_state_size_max = llama_get_state_size(ctx);

        if (n_state_size_cur > n_state_size_max) {
            fprintf(stderr, "%s : the state size in session file is too big! max %zu, got %zu\n", __func__, n_state_size_max, n_state_size_cur);
            return false;
        }

        std::vector<uint8_t> state_data(n_state_size_max);
        file.read_raw(state_data.data(), n_state_size_cur);

        llama_set_state_data(ctx, state_data.data());
    }

    return true;
}

bool llama_load_session_file(struct llama_context * ctx, const char * path_session, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    try {
        return llama_load_session_file_internal(ctx, path_session, tokens_out, n_token_capacity, n_token_count_out);
    } catch (const std::exception & err) {
        fprintf(stderr, "error loading session file: %s\n", err.what());
        return false;
    }
}

bool llama_save_session_file(struct llama_context * ctx, const char * path_session, const llama_token * tokens, size_t n_token_count) {
    llama_file file(path_session, "wb");

    file.write_u32(LLAMA_SESSION_MAGIC);
    file.write_u32(LLAMA_SESSION_VERSION);

    file.write_raw(&ctx->model.hparams, sizeof(llama_hparams));

    // save the prompt
    file.write_u32((uint32_t) n_token_count);
    file.write_raw(tokens, sizeof(llama_token) * n_token_count);

    // save the context state
    {
        const size_t n_state_size_max = llama_get_state_size(ctx);

        std::vector<uint8_t> state_data(n_state_size_max);
        const size_t n_state_size_cur = llama_copy_state_data(ctx, state_data.data());

        file.write_raw(state_data.data(), n_state_size_cur);
    }

    return true;
}

int llama_eval(
        struct llama_context * ctx,
           const llama_token * tokens,
                         int   n_tokens,
                         int   n_past,
                         int   n_threads) {
    if (!llama_eval_internal(*ctx, tokens, nullptr, n_tokens, n_past, n_threads)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }

    // get a more accurate load time, upon first eval
    // TODO: fix this
    if (!ctx->has_evaluated_once) {
        ctx->t_load_us = ggml_time_us() - ctx->t_start_us;
        ctx->has_evaluated_once = true;
    }

    return 0;
}


int llama_eval_embd(
            struct llama_context * ctx,
                     const float * embd,
                             int   n_tokens,
                             int   n_past,
                             int   n_threads) {
    if (!llama_eval_internal(*ctx, nullptr, embd, n_tokens, n_past, n_threads)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }

    // get a more accurate load time, upon first eval
    // TODO: fix this
    if (!ctx->has_evaluated_once) {
        ctx->t_load_us = ggml_time_us() - ctx->t_start_us;
        ctx->has_evaluated_once = true;
    }

    return 0;
}

int llama_eval_export(struct llama_context * ctx, const char * fname) {
    // TODO: use llama_build_graph if possible
    LLAMA_ASSERT(false);

    //const int n_batch = 1;
    //const int n_ctx   = 512 - n_batch;

    //const std::vector<llama_token> tmp(n_batch, llama_token_bos());


    //if (!llama_eval_internal(*ctx, tmp.data(), nullptr, tmp.size(), n_ctx, 1, fname)) {
    //    fprintf(stderr, "%s: failed to eval\n", __func__);
    //    return 1;
    //}

    return 0;
}

int llama_tokenize_with_model(
    const struct llama_model * model,
                  const char * text,
                 llama_token * tokens,
                         int   n_max_tokens,
                        bool   add_bos) {
    auto res = llama_tokenize(model->vocab, text, add_bos);

    if (n_max_tokens < (int) res.size()) {
        fprintf(stderr, "%s: too many tokens\n", __func__);
        return -((int) res.size());
    }

    for (size_t i = 0; i < res.size(); i++) {
        tokens[i] = res[i];
    }

    return res.size();
}

int llama_tokenize(
        struct llama_context * ctx,
                  const char * text,
                 llama_token * tokens,
                         int   n_max_tokens,
                        bool   add_bos) {
    return llama_tokenize_with_model(&ctx->model, text, tokens, n_max_tokens, add_bos);
}

int llama_n_vocab_from_model(const struct llama_model * model) {
    return model->vocab.id_to_token.size();
}

int llama_n_ctx_from_model(const struct llama_model * model) {
    return model->hparams.n_ctx;
}

int llama_n_embd_from_model(const struct llama_model * model) {
    return model->hparams.n_embd;
}

int llama_n_vocab(const struct llama_context * ctx) {
    return ctx->model.vocab.id_to_token.size();
}

int llama_n_ctx(const struct llama_context * ctx) {
    return ctx->model.hparams.n_ctx;
}

int llama_n_embd(const struct llama_context * ctx) {
    return ctx->model.hparams.n_embd;
}

int llama_get_vocab_from_model(
        const struct llama_model * model,
        const char * * strings,
        float  * scores,
        int capacity) {
    int n = std::min(capacity, (int) model->vocab.id_to_token.size());
    for (int i = 0; i<n; ++i) {
        strings[i] = model->vocab.id_to_token[i].tok.c_str();
        scores[i]  = model->vocab.id_to_token[i].score;
    }
    return n;
}

int llama_get_vocab(
        const struct llama_context * ctx,
        const char * * strings,
        float  * scores,
        int capacity) {
    return llama_get_vocab_from_model(&ctx->model, strings, scores, capacity);
}

float * llama_get_logits(struct llama_context * ctx) {
    return ctx->logits.data();
}

float * llama_get_embeddings(struct llama_context * ctx) {
    return ctx->embedding.data();
}

const char * llama_token_to_str_with_model(const struct llama_model * model, llama_token token) {
    if (token >= llama_n_vocab_from_model(model)) {
        return nullptr;
    }

    return model->vocab.id_to_token[token].tok.c_str();
}

const char * llama_token_to_str(const struct llama_context * ctx, llama_token token) {
    return llama_token_to_str_with_model(&ctx->model, token);
}

llama_token llama_token_bos() {
    return 1;
}

llama_token llama_token_eos() {
    return 2;
}

llama_token llama_token_nl() {
    return 13;
}

struct llama_timings llama_get_timings(struct llama_context * ctx) {
    struct llama_timings result = {
        /*.t_start_ms  =*/ 1e-3 * ctx->t_start_us,
        /*.t_end_ms    =*/ 1.00 * ggml_time_ms(),
        /*.t_load_ms   =*/ 1e-3 * ctx->t_load_us,
        /*.t_sample_ms =*/ 1e-3 * ctx->t_sample_us,
        /*.t_p_eval_ms =*/ 1e-3 * ctx->t_p_eval_us,
        /*.t_eval_ms   =*/ 1e-3 * ctx->t_eval_us,

        /*.n_sample =*/ std::max(1, ctx->n_sample),
        /*.n_p_eval =*/ std::max(1, ctx->n_p_eval),
        /*.n_eval   =*/ std::max(1, ctx->n_eval),
    };

    return result;
}

void llama_print_timings(struct llama_context * ctx) {
    const llama_timings timings = llama_get_timings(ctx);

    fprintf(stderr, "\n");
    fprintf(stderr, "%s:        load time = %8.2f ms\n", __func__, timings.t_load_ms);
    fprintf(stderr, "%s:      sample time = %8.2f ms / %5d runs   (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, timings.t_sample_ms, timings.n_sample, timings.t_sample_ms / timings.n_sample, 1e3 / timings.t_sample_ms * timings.n_sample);
    fprintf(stderr, "%s: prompt eval time = %8.2f ms / %5d tokens (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, timings.t_p_eval_ms, timings.n_p_eval, timings.t_p_eval_ms / timings.n_p_eval, 1e3 / timings.t_p_eval_ms * timings.n_p_eval);
    fprintf(stderr, "%s:        eval time = %8.2f ms / %5d runs   (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, timings.t_eval_ms, timings.n_eval, timings.t_eval_ms / timings.n_eval, 1e3 / timings.t_eval_ms * timings.n_eval);
    fprintf(stderr, "%s:       total time = %8.2f ms\n", __func__, (timings.t_end_ms - timings.t_start_ms));
}

void llama_reset_timings(struct llama_context * ctx) {
    ctx->t_start_us = ggml_time_us();
    ctx->t_sample_us = ctx->n_sample = 0;
    ctx->t_eval_us   = ctx->n_eval   = 0;
    ctx->t_p_eval_us = ctx->n_p_eval = 0;
}

const char * llama_print_system_info(void) {
    static std::string s;

    s  = "";
    s += "AVX = "         + std::to_string(ggml_cpu_has_avx())         + " | ";
    s += "AVX2 = "        + std::to_string(ggml_cpu_has_avx2())        + " | ";
    s += "AVX512 = "      + std::to_string(ggml_cpu_has_avx512())      + " | ";
    s += "AVX512_VBMI = " + std::to_string(ggml_cpu_has_avx512_vbmi()) + " | ";
    s += "AVX512_VNNI = " + std::to_string(ggml_cpu_has_avx512_vnni()) + " | ";
    s += "FMA = "         + std::to_string(ggml_cpu_has_fma())         + " | ";
    s += "NEON = "        + std::to_string(ggml_cpu_has_neon())        + " | ";
    s += "ARM_FMA = "     + std::to_string(ggml_cpu_has_arm_fma())     + " | ";
    s += "F16C = "        + std::to_string(ggml_cpu_has_f16c())        + " | ";
    s += "FP16_VA = "     + std::to_string(ggml_cpu_has_fp16_va())     + " | ";
    s += "WASM_SIMD = "   + std::to_string(ggml_cpu_has_wasm_simd())   + " | ";
    s += "BLAS = "        + std::to_string(ggml_cpu_has_blas())        + " | ";
    s += "SSE3 = "        + std::to_string(ggml_cpu_has_sse3())        + " | ";
    s += "VSX = "         + std::to_string(ggml_cpu_has_vsx())         + " | ";

    return s.c_str();
}

// For internal test use
const std::vector<std::pair<std::string, struct ggml_tensor *>>& llama_internal_get_tensor_map(struct llama_context * ctx) {
    return ctx->model.tensors_by_name;
}
