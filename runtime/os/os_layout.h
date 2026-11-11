// Guest layouts of the nn::os objects, recovered from the lifted target's sdk (v262144; sdk is identical in all five
// revisions). Offsets are bytes. "sdk+0x..." cites the instruction that proves the field (module offsets of
// analysis/import/v262144-sdk.elf). Written for this project; no SDK code or headers are copied.
#pragma once
#include <stdint.h>

namespace oslay {

// ---- Thread Local Region: 0x200 bytes, tpidrro_el0 points at it (one per thread, kernel-allocated on Horizon)
constexpr uint32_t TLR_SIZE = 0x200;
constexpr uint32_t TLR_IPC_BUFFER = 0x000;       // 0x100 bytes; nn::sf::hipc::GetMessageBufferOnTls sdk+0x37e790
constexpr uint32_t TLR_DISABLE_COUNT = 0x100;    // u16, BusyMutex sdk+0x33e800, PrepareAbort sdk+0x146500
constexpr uint32_t TLR_INTERRUPT_FLAG = 0x102;   // u16, sdk+0x146508 (then SynchronizePreemptionState)
constexpr uint32_t TLR_LOCALE = 0x1d0;           // __nnmusl_LocaleAddr sdk+0x565fc8
constexpr uint32_t TLR_ERRNO = 0x1d8;            // __nnmusl_ErrnoLocation sdk+0x565fd8
constexpr uint32_t TLR_TP = 0x1f0;               // ELF TLS thread pointer: __get_tp/__set_tp sdk+0x565fe8;
                                                 // copied into tpidr_el0 at thread start sdk+0x348eb4
constexpr uint32_t TLR_THREAD = 0x1f8;           // ThreadType*: GetCurrentThread sdk+0x341e80; set at entry sdk+0x348e8c

// ---- nn::os::ThreadType: 0x1c0 bytes (memset in ThreadManager::CreateThread sdk+0x3444b0)
constexpr uint32_t TT_SIZE = 0x1c0;
constexpr uint32_t TT_ALL_NODE = 0x00;           // intrusive list node, self-linked at create sdk+0x344518
constexpr uint32_t TT_WAITLIST = 0x10;           // MultiWait holder list head, self-linked sdk+0x3444fc
constexpr uint32_t TT_STATE = 0x40;              // u8: see TS_*; 1 at create sdk+0x344514, 4 at exit sdk+0x34473c
constexpr uint32_t TT_STACK_ALIASED = 0x41;      // u8 sdk+0x3444f8
constexpr uint32_t TT_AUTO_REGISTERED = 0x42;    // u8 (strh wzr 0x42 sdk+0x344504 clears 0x42..0x43)
constexpr uint32_t TT_SUSPEND_COUNT = 0x43;      // u8
constexpr uint32_t TT_BASE_PRIORITY = 0x44;      // s16 nn::os priority: GetThreadPriority sdk+0x341ef0
constexpr uint32_t TT_VERSION = 0x46;            // s16 = 1 sdk+0x3444d4
constexpr uint32_t TT_ORIGINAL_STACK = 0x48;     // sdk+0x344520
constexpr uint32_t TT_STACK = 0x50;              // (alias) stack, sdk+0x344520; read at entry sdk+0x348ec0
constexpr uint32_t TT_STACK_SIZE = 0x58;         // sdk+0x34451c
constexpr uint32_t TT_ARGUMENT = 0x60;           // sdk+0x34451c; InvokeThread loads x0 from it sdk+0x344700
constexpr uint32_t TT_FUNCTION = 0x68;           // sdk+0x344524; called sdk+0x344704
constexpr uint32_t TT_CURRENT_FIBER = 0x70;      // GetCurrentFiber sdk+0x33f8c0; SwitchToFiber sdk+0x33f7d0
constexpr uint32_t TT_THREAD_FIBER = 0x78;       // the thread's own context as a fiber, SwitchToFiber sdk+0x33f838
constexpr uint32_t TT_TLS = 0x80;                // u64[32]: GetTlsValue sdk+0x34218c
constexpr uint32_t TT_NAME_BUFFER = 0x180;       // char[32]: SetThreadName sdk+0x341e0c
constexpr uint32_t TT_NAME_POINTER = 0x1a0;      // const char*: sdk+0x34450c
constexpr uint32_t TT_CS = 0x1a8;                // InternalCriticalSection (u32 lock word) sdk+0x3444d0
constexpr uint32_t TT_CV = 0x1ac;                // InternalConditionVariable (u32) sdk+0x3444dc
constexpr uint32_t TT_HANDLE = 0x1b0;            // u32 kernel thread handle sdk+0x348da0; used as lock tag sdk+0x34a490
constexpr uint32_t TT_THREAD_ID = 0x1b8;         // u64: GetThreadId sdk+0x342020, svcGetThreadId sdk+0x348db8
enum : uint8_t { TS_NOT_INITIALIZED = 0, TS_INITIALIZED = 1, TS_DESTROYED_BEFORE_STARTED = 2, TS_STARTED = 3,
                 TS_TERMINATED = 4 };          // InvokeThread waits while 1 (sdk+0x3446d4), runs when 3 (sdk+0x3446f4)
constexpr int TLS_SLOTS = 32;
constexpr int TLS_USER_FIRST = 0, TLS_USER_LAST = 15;   // searched upwards  (SearchUnusedTlsSlotUnsafe sdk+0x344d30)
constexpr int TLS_SDK_FIRST = 31, TLS_SDK_LAST = 17;    // searched downwards (sdk+0x344cac)
constexpr uint32_t RESULT_OUT_OF_RESOURCE = 0x1203;     // os ResultOutOfResource: AllocateTlsSlot sdk+0x342108

// ---- nn::os::FiberType: 0x58 bytes
constexpr uint32_t FB_SIZE = 0x58;
constexpr uint32_t FB_STATE = 0x20;              // u8: 1 ready, 2 running (sdk+0x33f628, sdk+0x33f7e0)
constexpr uint32_t FB_STACK_ALIASED = 0x21;      // u8 sdk+0x33f6b0
constexpr uint32_t FB_FUNCTION = 0x28;           // FiberType* (*)(void*) sdk+0x33f624
constexpr uint32_t FB_ARGUMENT = 0x30;
constexpr uint32_t FB_ORIGINAL_STACK = 0x38;     // sdk+0x33f62c
constexpr uint32_t FB_STACK = 0x40;
constexpr uint32_t FB_STACK_SIZE = 0x48;         // sdk+0x33f620
constexpr uint32_t FB_CONTEXT = 0x50;            // saved-sp of DispatchFiberContext sdk+0x3483a0 (host: unused)
enum : uint8_t { FS_NONE = 0, FS_READY = 1, FS_RUNNING = 2 };

// ---- lock word of InternalCriticalSection: owner thread handle | waiters bit (Horizon arbitration encoding)
constexpr uint32_t LOCK_WAITERS = 0x40000000u;

// ---- nn::os::MutexType: 0x20 bytes (InitializeMutex sdk+0x340ff0)
constexpr uint32_t MX_STATE = 0x00;              // u8 1 = initialized
constexpr uint32_t MX_RECURSIVE = 0x01;          // u8
constexpr uint32_t MX_LOCK_LEVEL = 0x04;         // s32
constexpr uint32_t MX_NEST_COUNT = 0x08;         // s32 (LockMutex sdk+0x341084)
constexpr uint32_t MX_OWNER = 0x10;              // ThreadType* (sdk+0x341090)
constexpr uint32_t MX_CS = 0x18;                 // u32 lock word (sdk+0x34107c)

// ---- nn::os::ConditionVariableType: 8 bytes (sdk+0x33e9e0)
constexpr uint32_t CV_STATE = 0x00;              // u8
constexpr uint32_t CV_KEY = 0x04;                // u32 InternalConditionVariable

// ---- nn::os::EventType: 0x28 bytes (InitializeEvent sdk+0x33ed20)
constexpr uint32_t EV_HOLDERS = 0x00;            // MultiWait object list head
constexpr uint32_t EV_SIGNALED = 0x10;           // u8
constexpr uint32_t EV_INITIALLY_SIGNALED = 0x11; // u8
constexpr uint32_t EV_CLEAR_MODE = 0x12;         // u8: 1 = AutoClear (SignalEvent sdk+0x33edc0)
constexpr uint32_t EV_STATE = 0x13;              // u8
constexpr uint32_t EV_BROADCAST = 0x14;          // u64 broadcast counter, unaligned (sdk+0x33ee08)
constexpr uint32_t EV_CS = 0x1c;
constexpr uint32_t EV_CV = 0x20;
constexpr uint32_t EV_SIZE = 0x28;

// ---- nn::os::SystemEventType: 0x30 bytes = EventType (or InterProcessEventType) + state byte
constexpr uint32_t SE_STATE = 0x28;              // 1 = intra-process Event, 2 = inter-process (sdk+0x33ffd8, +0x33fff0)

// ---- nn::os::LightEventType: 0x0c bytes (sdk+0x33f040)
constexpr uint32_t LE_WORD = 0x00;               // s32: 2 = signaled (initial), 0 = not; host also uses 1 = waiters
constexpr uint32_t LE_AUTOCLEAR = 0x04;          // u8 (clear mode == 1)
constexpr uint32_t LE_STATE = 0x05;              // u8

// ---- nn::os::SemaphoreType: 0x28 bytes (sdk+0x3417e0)
constexpr uint32_t SM_HOLDERS = 0x00;
constexpr uint32_t SM_STATE = 0x10;
constexpr uint32_t SM_COUNT = 0x14;              // s32 (AcquireSemaphore sdk+0x341860)
constexpr uint32_t SM_MAX = 0x18;
constexpr uint32_t SM_CS = 0x1c;
constexpr uint32_t SM_CV = 0x20;

// ---- nn::os::BarrierType (sdk+0x33e6e0)
constexpr uint32_t BR_MAX = 0x00;                // u16
constexpr uint32_t BR_ARRIVED = 0x02;            // u16
constexpr uint32_t BR_GENERATION = 0x04;         // u64, unaligned
constexpr uint32_t BR_CS = 0x0c;
constexpr uint32_t BR_CV = 0x10;

// ---- nn::os::ReaderWriterLockType (sdk+0x3411f0; counters at 0x04..0x0f are host-defined)
constexpr uint32_t RW_CS = 0x00;
constexpr uint32_t RW_READERS = 0x04;            // u32 read locks held (host)
constexpr uint32_t RW_WRITE_COUNT = 0x08;        // u32 recursive write count (host)
constexpr uint32_t RW_WAITING_WRITERS = 0x0c;    // u32 (host)
constexpr uint32_t RW_STATE = 0x10;              // u8 sdk+0x341220
constexpr uint32_t RW_OWNER = 0x18;              // ThreadType* of the writer: IsWriteLockHeldByCurrentThread sdk+0x3412e4
constexpr uint32_t RW_CV_READ = 0x24;
constexpr uint32_t RW_CV_WRITE = 0x28;

// ---- nn::os::TimerEventType: 0x40 bytes (sdk+0x33f8d0, StartPeriodicTimerEvent sdk+0x33f9c0)
constexpr uint32_t TE_HOLDERS = 0x00;
constexpr uint32_t TE_STATE = 0x10;
constexpr uint32_t TE_CLEAR_MODE = 0x11;
constexpr uint32_t TE_SIGNALED = 0x12;
constexpr uint32_t TE_TIMER_STATE = 0x13;        // 0 stopped, 1 one-shot, 2 periodic (sdk+0x33fa08)
constexpr uint32_t TE_BROADCAST = 0x14;          // u64, unaligned
constexpr uint32_t TE_NEXT = 0x20;               // TimeSpan ns of the next expiry (sdk+0x33f9f8)
constexpr uint32_t TE_FIRST = 0x28;
constexpr uint32_t TE_INTERVAL = 0x30;
constexpr uint32_t TE_CS = 0x38;
constexpr uint32_t TE_CV = 0x3c;

// ---- nn::os::MessageQueueType: 0x48 bytes (sdk+0x3402f0)
constexpr uint32_t MQ_HOLDERS_NOT_FULL = 0x00;
constexpr uint32_t MQ_HOLDERS_NOT_EMPTY = 0x10;
constexpr uint32_t MQ_BUFFER = 0x20;             // uintptr_t*
constexpr uint32_t MQ_MAX = 0x28;                // s32
constexpr uint32_t MQ_COUNT = 0x2c;              // s32
constexpr uint32_t MQ_OFFSET = 0x30;             // s32
constexpr uint32_t MQ_STATE = 0x34;
constexpr uint32_t MQ_CS = 0x38;
constexpr uint32_t MQ_CV_NOT_FULL = 0x3c;
constexpr uint32_t MQ_CV_NOT_EMPTY = 0x40;

// ---- nn::os::MultiWaitType: 0x30 bytes (sdk+0x340de0) and MultiWaitHolderType: 0x40 bytes (sdk+0x33f010)
constexpr uint32_t MW_STATE = 0x00;
constexpr uint32_t MW_LIST = 0x08;               // holder list head (host keeps its own order)
constexpr uint32_t MW_CS = 0x28;
constexpr uint32_t MH_KIND = 0x00;               // guest: vtable; host: HolderKind tag (guest never dispatches on it)
constexpr uint32_t MH_MW_NODE = 0x08;
constexpr uint32_t MH_OBJ_NODE = 0x18;
constexpr uint32_t MH_MULTIWAIT = 0x28;          // owning MultiWait (LinkMultiWaitHolder sdk+0x340e8c)
constexpr uint32_t MH_OBJECT = 0x30;
constexpr uint32_t MH_USER_DATA = 0x38;          // GetMultiWaitHolderUserData sdk+0x340fb0
enum HolderKind : uint64_t { HK_NONE = 0, HK_EVENT = 1, HK_SYSTEM_EVENT = 2, HK_SEMAPHORE = 3, HK_THREAD = 4,
                             HK_TIMER = 5, HK_MQ_NOT_FULL = 6, HK_MQ_NOT_EMPTY = 7 };
constexpr int MQ_WAIT_NOT_FULL = 1, MQ_WAIT_NOT_EMPTY = 2;   // MessageQueueWaitType (sdk+0x340d70)

// ---- SdkMutexType: u32 lock word; SdkRecursiveMutexType: lock word + u32 count (sdk+0x341460);
//      SdkConditionVariableType: u32 key
constexpr uint32_t SRM_COUNT = 0x04;

constexpr uint32_t RESULT_TIMED_OUT = 0xea01;    // svc::ResultTimedOut (kernel)

}  // namespace oslay
