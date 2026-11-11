# svcGetInfo (SVC 0x29) in os_svc

Raw ABI at the `svc #0x29`: x1 = info type, x2 = handle, x3 = subtype; returns w0 = result, x1 = value. The sdk stub is
`sdk+0x399de8` (`str x0,[sp,-16]!`, `svc`, store x1 to the saved out pointer); the C-level call is `stub(out*, type, handle, subtype)`.
Handle `0xffff8001` is the current-process pseudo handle. Implemented in `os_svc` (`os_kernel.cpp`, case 0x29); memory
values come from `mem_get_info` in `os_memory.cpp`.

## Call sites (found by scanning sdk .text for `bl sdk+0x399de8`; main has no `svc` and no direct caller, it reaches these through sdk)

| Caller (sdk) | Site | type, handle, sub | Use of the result |
|---|---|---|---|
| nn::diag::IsDebuggerAttached | 0x1463cc | 8, 0, 0 | returns the value as bool |
| nn::os::GetIdleTickCount | 0x33ec2c | 0xa, 0, -1 | returns the tick count (overridden by `ov_GetIdleTickCount`, returns 0) |
| nn::os::SetDyingMessageRegion | 0x33ecd8 | 0x14, 0xffff8001, 0 | stores `{addr,size}` at `[ctx+0x1c0]`, so the value must be writable guest memory |
| VammManagerImplByHorizon::GetReservedRegionImpl | 0x346ca8, 0x346cc4 | 2 then 3 | alias region base and size |
| VammManager::InitializeIfEnabled (overridden) | 0x346d58 | 0x10 | nonzero and result 0 = VAMM enabled |
| VammManager::IsVirtualAddressMemoryEnabled | 0x346dbc | 0x10 | enabled = (result 0 and value != 0) |
| VammManager::GetVirtualAddressMemoryResourceUsage (overridden) | 0x347adc, 0x347afc | 0x10, 0x11 | total and used system resource size, usage = min(used+8 MiB, total) |
| DebugImplByHorizon::QueryMemoryInfo | 0x348234, 0x348254 | 0x16, 0x15 | used and total non-system memory |
| ThreadManagerImplByHorizon::GetThreadAvailableCoreMask (overridden) | 0x34943c | 0, 0xffff8001, 0 | core mask |
| OsResourceManager ctor | 0x34a25c, 0x34a298 | 0xc, 0xe | aslr base, stack base, handed to region managers (size 0x4000 granule) |
| AslrSpaceManagerImplByHorizon::GetAslrSpaceEndAddress | 0x34a360, 0x34a380 | 0xc then 0xd | end = base + size |
| StackGuardManagerImplByHorizon::GetStackGuardEndAddress | 0x34a3f0, 0x34a410 | 0xe then 0xf | end = base + size |
| nn::os::detail::IsApplication | 0x34a63c | 0x17 | bool |
| RngManager ctor | 0x34a6b4, 0x34a6c8 | 0xb, 0, subtypes 2 and 3 | 16 bytes seed for the TinyMt |
| InitializeRandomImpl | 0x34a720, 0x34a734 | 0xb, 0, subtypes 0 and 1 | 16 bytes seed for the TinyMt |

(Handle values are `mov w2,#-0x7fff` = 0xffff8001 everywhere except types 8, 0xa, 0xb which pass 0.)

## Implemented values

Types from the Horizon (Atmosphere/switchbrew) InfoType enum. Types 8, 10, 11 require handle 0; all others require
`0xffff8001` (else 0xe401 InvalidHandle). Subtype must be 0 except types 10 (core -1 or < cores, else 0xe001) and 11 (0..3, else 0xe001).

| Type | Name | Value | Evidence |
|---|---|---|---|
| 0 | CoreMask | `(1<<cores)-1`, 7 by default | GetThreadAvailableCoreMask override test expects 7 |
| 1 | PriorityMask | all ones | not requested; harmless |
| 2, 3 | AliasRegion addr, size | a real 2 GiB `MEM_RESERVE` block, created on first query | GetReservedRegionImpl; VAMM does not allocate from it (host VAMM is VirtualAlloc) |
| 4, 5 | HeapRegion addr, size | the 16 GiB reservation that `SetMemoryHeapSize` commits from, reserved on first query | same block as `GetMemoryHeapAddress`; tested |
| 6 | TotalMemorySize | 4 GiB + 1 GiB system resource | arbitrary budget |
| 7 | UsedMemorySize | type 0x16 + type 0x11 | |
| 8 | DebuggerAttached | 0 | IsDebuggerAttached caller |
| 10 | IdleTickCount | 0 | same as the override; no idle accounting |
| 11 | RandomEntropy[0..3] | 4 fixed u64 from `std::random_device`, per run | RngManager and InitializeRandomImpl |
| 12, 13 | AslrRegion addr, size | host min/max application address (`GetSystemInfo`) | AslrSpaceManager; host addresses are guest addresses |
| 14, 15 | StackRegion addr, size | a real 1 GiB `MEM_RESERVE` block, created on first query | StackGuardManager |
| 16 | SystemResourceSizeTotal | 1 GiB (must be nonzero, VAMM-enabled test at sdk+0x346dbc) | |
| 17 | SystemResourceSizeUsed | min(VAMM committed bytes, 16) | consistent with `ov_VamUsage` |
| 18 | ProgramId | 0x0100F4300BF2C000 | task; not requested by the game |
| 20 | UserExceptionContextAddress | one zeroed 4 KiB `VirtualAlloc` block | SetDyingMessageRegion writes `[ctx+0x1c0]` |
| 21 | TotalNonSystemMemorySize | 4 GiB | QueryMemoryInfo |
| 22 | UsedNonSystemMemorySize | heap size + VAMM committed bytes | QueryMemoryInfo |
| 23 | IsApplication | 1 | |

Everything else (9 ResourceLimit, 19, 24 and unknown values) returns 0x7801 (InvalidEnumValue, module 1 desc 60) and logs
`svcGetInfo: unknown type ...`. None of those is requested by sdk or main.

## Other SVCs added to os_svc
- 0x27 OutputDebugString(str, len): logs up to 1 KiB, returns 0.
- 0x26 Break(reason, info, size): logs; fatal unless bit 31 of reason is set (notification only).
- 0x1e GetSystemTick already existed. The boot logs in `analysis/boot/` show no other SVC on the backstop path besides GetInfo.

## Stopgap in host/boot.cpp
`svc_dispatch` calls `os_svc` first, so the type-20 branch is now dead. It can be removed (leave `return os_svc(c, imm);`). Not edited here.

Tests: `tests/os/os_test.cpp` `test_getinfo` (227 checks total, 0 failed).
