// SPDX-License-Identifier: MIT
// NVN ABI facts used by the NVN->Vulkan layer (our own definitions, not a copy of any SDK header).
//
// Provenance legend, used in the comments below:
//   [CALL 0xADDR]  evidence from the game binary (analysis/import/v262144-main.elf, module offsets):
//                  argument setup at the call site, or an allocation size next to the call.
//   [TOTK-NVN]     value cross-checked against tools/ref/totk/runtime/nvn.h (MIT, h1k421 / TotK recomp).
//                  That file was reconstructed by the community from public DWARF of a shipped game,
//                  not from the Nintendo SDK; we read it as a reference and re-typed the values.
//   [LAS-NVN]      tools/ref/las/runtime/nvn/formats.c (MIT), format block sizes and groups.
// No Nintendo SDK header was obtained or copied.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nvn {

using Boolean = uint8_t;           // [CALL 0x90d6xx] callers test the result with `tst w0, #0xff`
using TextureHandle = uint64_t;    // texture id | sampler id << 20  (unverified for ids > 0xfffff)
using CommandHandle = uint64_t;    // opaque; an id into the command list table (nvn_life.cpp) [CALL 0x903e0x: stored as a 64-bit slot]
using NativeWindow = void*;
using GenericProc = void (*)(void);

// ---- Guest-visible object sizes (the game allocates these; we only need >= 16 bytes) ----------------
// [CALL 0x90d590] Device 0x3000 (mov w0,#0x3000), Queue 0x2000, CommandBuffer 0xa0, MemoryPool 0x100,
// Sync 0x40 (mov w0,#0x40 before SyncInitialize), Window 0x180 (0x985790). Builders live on the guest stack.
// [TOTK-NVN] builder sizes: device/queue/window/memoryPool 0x40, texture 0x80. Texture object 0x80.
constexpr size_t kBuilderMax = 0x40;       // our builders must fit in the smallest builder (0x40)
constexpr size_t kTextureBuilderMax = 0x80;

// ---- Enums (values: [TOTK-NVN] unless a [CALL] tag says otherwise) -------------------------------------
enum DeviceInfo : int {            // game queries 0..0x5f in a loop [CALL 0x90d6f0: cmp x22,#0x60]
  DI_API_MAJOR_VERSION = 0x00, DI_API_MINOR_VERSION = 0x01,
  DI_UNIFORM_BUFFER_BINDINGS_PER_STAGE = 0x02, DI_MAX_UNIFORM_BUFFER_SIZE = 0x03,
  DI_UNIFORM_BUFFER_ALIGNMENT = 0x04, DI_COLOR_BUFFER_BINDINGS = 0x05, DI_VERTEX_BUFFER_BINDINGS = 0x06,
  DI_TRANSFORM_FEEDBACK_BUFFER_BINDINGS = 0x07, DI_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE = 0x08,
  DI_TEXTURE_BINDINGS_PER_STAGE = 0x09, DI_COUNTER_ALIGNMENT = 0x0A,
  DI_TRANSFORM_FEEDBACK_BUFFER_ALIGNMENT = 0x0B, DI_TRANSFORM_FEEDBACK_CONTROL_ALIGNMENT = 0x0C,
  DI_INDIRECT_DRAW_ALIGNMENT = 0x0D, DI_VERTEX_ATTRIBUTES = 0x0E, DI_TEXTURE_DESCRIPTOR_SIZE = 0x0F,
  DI_SAMPLER_DESCRIPTOR_SIZE = 0x10, DI_RESERVED_TEXTURE_DESCRIPTORS = 0x11,
  DI_RESERVED_SAMPLER_DESCRIPTORS = 0x12,
  DI_COMMAND_BUFFER_COMMAND_ALIGNMENT = 0x13,   // [CALL 0x90e5xx] game reads 0x13,0x14,0x16 around CB setup
  DI_COMMAND_BUFFER_CONTROL_ALIGNMENT = 0x14, DI_COMMAND_BUFFER_MIN_COMMAND_SIZE = 0x15,
  DI_COMMAND_BUFFER_MIN_CONTROL_SIZE = 0x16, DI_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM = 0x17,
  DI_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED = 0x18, DI_SHADER_SCRATCH_MEMORY_ALIGNMENT = 0x19,
  DI_SHADER_SCRATCH_MEMORY_GRANULARITY = 0x1A, DI_MAX_TEXTURE_ANISOTROPY = 0x1B,
  DI_MAX_COMPUTE_WORK_GROUP_SIZE_X = 0x1C, DI_MAX_COMPUTE_WORK_GROUP_SIZE_Y = 0x1D,
  DI_MAX_COMPUTE_WORK_GROUP_SIZE_Z = 0x1E, DI_MAX_COMPUTE_WORK_GROUP_SIZE_THREADS = 0x1F,
  DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_X = 0x20, DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Y = 0x21,
  DI_MAX_COMPUTE_DISPATCH_WORK_GROUPS_Z = 0x22, DI_IMAGE_BINDINGS_PER_STAGE = 0x23,
  DI_MAX_TEXTURE_POOL_SIZE = 0x24, DI_MAX_SAMPLER_POOL_SIZE = 0x25, DI_MAX_VIEWPORTS = 0x26,
  DI_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT = 0x27, DI_SUPPORTS_MIN_MAX_FILTERING = 0x28,
  DI_SUPPORTS_STENCIL8_FORMAT = 0x29, DI_SUPPORTS_ASTC_FORMATS = 0x2A, DI_L2_SIZE = 0x2B,
  DI_MAX_TEXTURE_LEVELS = 0x2C, DI_MAX_TEXTURE_LAYERS = 0x2D,
  DI_GLSLC_MAX_GPU_CODE_MAJOR = 0x2E, DI_GLSLC_MIN_GPU_CODE_MAJOR = 0x2F,   // [CALL 0x90d6ac] read first
  DI_GLSLC_MAX_GPU_CODE_MINOR = 0x30, DI_GLSLC_MIN_GPU_CODE_MINOR = 0x31,
  DI_SUBPIXEL_BITS = 0x33, DI_INDIRECT_DISPATCH_ALIGNMENT = 0x35,
  DI_LINEAR_TEXTURE_STRIDE_ALIGNMENT = 0x38, DI_LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT = 0x39,
  DI_MEMORY_POOL_PAGE_SIZE = 0x3A, DI_UNIFORM_BUFFER_UPDATE_ALIGNMENT = 0x3C, DI_MAX_TEXTURE_SIZE = 0x3D,
  DI_MAX_BUFFER_TEXTURE_SIZE = 0x3E, DI_MAX_3D_TEXTURE_SIZE = 0x3F, DI_MAX_CUBE_MAP_TEXTURE_SIZE = 0x40,
  DI_MAX_RECTANGLE_TEXTURE_SIZE = 0x41, DI_MAX_PRESENT_INTERVAL = 0x46,
  DI_QUEUE_COMMAND_MEMORY_GRANULARITY = 0x50, DI_QUEUE_COMMAND_MEMORY_MIN_SIZE = 0x51,
  DI_QUEUE_COMMAND_MEMORY_DEFAULT_SIZE = 0x52, DI_QUEUE_COMPUTE_MEMORY_GRANULARITY = 0x53,
  DI_QUEUE_COMPUTE_MEMORY_MIN_SIZE = 0x54, DI_QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE = 0x55,
  DI_QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD = 0x56, DI_MAX_TEXTURES_PER_WINDOW = 0x58,
  DI_MIN_TEXTURES_PER_WINDOW = 0x59, DI_SUPPORTS_DEBUG_LAYER = 0x5A,
  DI_QUEUE_CONTROL_MEMORY_MIN_SIZE = 0x5B, DI_QUEUE_CONTROL_MEMORY_DEFAULT_SIZE = 0x5C,
  DI_QUEUE_CONTROL_MEMORY_GRANULARITY = 0x5D,
  DI_COUNT = 0x62
};

enum WindowOriginMode : int { ORIGIN_LOWER_LEFT = 0, ORIGIN_UPPER_LEFT = 1 };   // [CALL 0x90d6f4: mov w1,#1]
enum DepthMode : int { DEPTH_NEAR_IS_MINUS_W = 0, DEPTH_NEAR_IS_ZERO = 1 };      // [CALL 0x90d708: mov w1,#1]

enum WindowAcquireTextureResult : int { ACQUIRE_SUCCESS = 0, ACQUIRE_NATIVE_ERROR = 1 };
enum SyncWaitResult : int { SYNC_ALREADY_SIGNALED = 0, SYNC_CONDITION_SATISFIED = 1, SYNC_TIMEOUT_EXPIRED = 2,
                            SYNC_FAILED = 3 };
enum SyncCondition : int { SYNC_ALL_GPU_COMMANDS_COMPLETE = 0, SYNC_GRAPHICS_WORLD_SPACE_COMPLETE = 1 };
enum CommandBufferMemoryEvent : int { CBEVENT_OUT_OF_COMMAND_MEMORY = 0, CBEVENT_OUT_OF_CONTROL_MEMORY = 1 };

enum MemoryPoolFlags : int {       // [CALL 0x90df80: SetFlags(0xa2) = CPU_UNCACHED|GPU_CACHED|COMPRESSIBLE]
  MP_CPU_NO_ACCESS = 0x1, MP_CPU_UNCACHED = 0x2, MP_CPU_CACHED = 0x4, MP_GPU_NO_ACCESS = 0x8,
  MP_GPU_UNCACHED = 0x10, MP_GPU_CACHED = 0x20, MP_SHADER_CODE = 0x40, MP_COMPRESSIBLE = 0x80,
  MP_PHYSICAL = 0x100, MP_VIRTUAL = 0x200
};

enum TextureFlags : int { TF_DISPLAY = 0x1, TF_VIDEO_DECODE = 0x2, TF_IMAGE = 0x4, TF_COMPRESSIBLE = 0x8,
                          TF_LINEAR = 0x10,   // [CALL 0x8f8700: `tbz w23,#4` guards TextureBuilderSetStride]
                          TF_SPARSE = 0x20, TF_LINEAR_RENDER_TARGET = 0x40 };
enum TextureTarget : int { TT_1D = 0, TT_2D = 1, TT_3D = 2, TT_1D_ARRAY = 3, TT_2D_ARRAY = 4,
                           TT_2D_MULTISAMPLE = 5, TT_2D_MULTISAMPLE_ARRAY = 6, TT_RECTANGLE = 7,
                           TT_CUBEMAP = 8, TT_CUBEMAP_ARRAY = 9, TT_BUFFER = 10 };

enum Format : int {                // subset; [TOTK-NVN] values, groups cross-checked with [LAS-NVN]
  FMT_NONE = 0, FMT_R8 = 1, FMT_R8SN, FMT_R8UI, FMT_R8I, FMT_R16F, FMT_R16, FMT_R16SN, FMT_R16UI, FMT_R16I,
  FMT_R32F, FMT_R32UI, FMT_R32I, FMT_RG8, FMT_RG8SN, FMT_RG8UI, FMT_RG8I, FMT_RG16F, FMT_RG16, FMT_RG16SN,
  FMT_RG16UI, FMT_RG16I, FMT_RG32F, FMT_RG32UI, FMT_RG32I,                   // 0x01..0x18
  FMT_RGBA8 = 0x25, FMT_RGBA8SN, FMT_RGBA8UI, FMT_RGBA8I, FMT_RGBA16F, FMT_RGBA16, FMT_RGBA16SN,
  FMT_RGBA16UI, FMT_RGBA16I, FMT_RGBA32F, FMT_RGBA32UI, FMT_RGBA32I,         // 0x25..0x30
  FMT_STENCIL8 = 0x31, FMT_DEPTH16, FMT_DEPTH24, FMT_DEPTH32F, FMT_DEPTH24_STENCIL8, FMT_DEPTH32F_STENCIL8,
  FMT_RGBX8_SRGB = 0x37,
  FMT_RGBA8_SRGB = 0x38,           // [CALL 0x90d750: RegisterFastClearColor(dev, color, 0x38)]
  FMT_RGB10A2 = 0x3D, FMT_R11G11B10F = 0x3F,
  FMT_BGRX8 = 0x99, FMT_BGRA8 = 0x9A, FMT_BGRX8_SRGB = 0x9B, FMT_BGRA8_SRGB = 0x9C
};

enum Barrier : int { BARRIER_ORDER_PRIMITIVES = 1, BARRIER_ORDER_FRAGMENTS = 2, BARRIER_ORDER_FRAGMENTS_TILED = 4,
                     BARRIER_ORDER_INDIRECT_DATA = 8, BARRIER_INVALIDATE_TEXTURE = 0x10,
                     BARRIER_INVALIDATE_SHADER = 0x20, BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR = 0x40 };

// Memory callback: (cb, event, minSize, userData). [CALL 0x908e20/0x90d590: callback = 0x9101a0, data set via
// SetMemoryCallbackData]; signature [TOTK-NVN].
struct CommandBuffer;
using CommandBufferMemoryCallback = void (*)(CommandBuffer*, CommandBufferMemoryEvent, size_t, void*);

}  // namespace nvn
