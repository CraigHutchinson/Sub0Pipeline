# VTune threading profile

- Captured: 2026-10-05T20:53:16.117092+00:00
- Host: Windows-11-10.0.26220-SP0; Intel(R) VTune(TM) Profiler 2026.4.0 (build 632893) Command Line Tool
- Benchmark: `Sub0Pipeline_Bench.exe` at `7a373d2+harness`
- 10.0 s per case; ranked by Wait Time (self time, summed over threads)

## desktop: 10-job fan-out

672 iterations, 14,898,088 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 9.852 | 100.0 |
| `ntdll.dll` | 0.000 | 0.0 |
| `[Unknown]` | 0.000 | 0.0 |
| `KERNELBASE.dll` | 0.000 | 0.0 |
| `KERNEL32.DLL` | 0.000 | 0.0 |
| `pincrt.dll` | 0.000 | 0.0 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `sub0pipeline::DesktopExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 9.847 | 100.0 |
| `sub0pipeline::PriorityExecutor::'scalar deleting destructor'` | `Sub0Pipeline_Bench.exe` | 0.005 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::thread::join` | `Sub0Pipeline_Bench.exe` | 9.847 | 99.9 |
| `std::vector<std::jthread,std::allocator<std::jthread> >::clear` | `Sub0Pipeline_Bench.exe` | 0.005 | 0.1 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.001 | 0.0 |

## priority(4): 10-job linear chain

127,897 iterations, 78,188 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 7.094 | 99.9 |
| `[Unknown]` | 0.008 | 0.1 |
| `MSVCP140.dll` | 0.000 | 0.0 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 4.912 | 69.2 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 2.181 | 30.7 |
| `[Unknown]` | `[Unknown]` | 0.008 | 0.1 |
| `sub0pipeline::PriorityExecutor::'scalar deleting destructor'` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 2.435 | 34.3 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 2.300 | 32.4 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 1.801 | 25.4 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.380 | 5.3 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.178 | 2.5 |
| `[Unknown]` | `[Unknown]` | 0.008 | 0.1 |
| `std::vector<std::jthread,std::allocator<std::jthread> >::clear` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `std::jthread::request_stop` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

## priority(4): 300-job fan-out

5,233 iterations, 1,911,026 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 12.296 | 99.9 |
| `[Unknown]` | 0.008 | 0.1 |
| `pincrt.dll` | 0.000 | 0.0 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 8.299 | 67.5 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 3.997 | 32.5 |
| `[Unknown]` | `[Unknown]` | 0.008 | 0.1 |
| `sub0pipeline::PriorityExecutor::'scalar deleting destructor'` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 5.815 | 47.3 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 3.711 | 30.2 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 2.429 | 19.7 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.286 | 2.3 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.056 | 0.5 |
| `[Unknown]` | `[Unknown]` | 0.008 | 0.1 |
| `std::vector<std::jthread,std::allocator<std::jthread> >::clear` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `std::jthread::request_stop` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

## priority(4): 1000-job layered DAG (20x50, fan-in 4)

1,887 iterations, 5,301,007 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 21.618 | 100.0 |
| `[Unknown]` | 0.007 | 0.0 |
| `pincrt.dll` | 0.000 | 0.0 |
| `ucrtbase.dll` | 0.000 | 0.0 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 11.413 | 52.8 |
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 10.202 | 47.2 |
| `[Unknown]` | `[Unknown]` | 0.007 | 0.0 |
| `sub0pipeline::PriorityExecutor::'scalar deleting destructor'` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 11.122 | 51.4 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 9.840 | 45.5 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.291 | 1.4 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.222 | 1.0 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 0.140 | 0.7 |
| `[Unknown]` | `[Unknown]` | 0.007 | 0.0 |
| `std::vector<std::jthread,std::allocator<std::jthread> >::clear` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |
| `std::jthread::request_stop` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

