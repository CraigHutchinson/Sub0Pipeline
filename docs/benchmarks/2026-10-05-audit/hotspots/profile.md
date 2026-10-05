# VTune hotspots profile

- Captured: 2026-10-05T20:36:10.284678+00:00
- Host: Windows-11-10.0.26220-SP0; Intel(R) VTune(TM) Profiler 2026.4.0 (build 632893) Command Line Tool
- Benchmark: `Sub0Pipeline_Bench.exe` at `7a373d2+harness`
- 10.0 s per case; ranked by CPU Time (self time, summed over threads)

## construct 10-job linear chain

4,990,976 iterations, 2,004 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 5.862 | 59.6 |
| `ucrtbase.dll` | 3.973 | 40.4 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `malloc_base` | `ucrtbase.dll` | 3.820 | 38.8 |
| `std::vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >::~vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >` | `Sub0Pipeline_Bench.exe` | 2.075 | 21.1 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::emplace_back` | `Sub0Pipeline_Bench.exe` | 0.944 | 9.6 |
| `'anonymous namespace'::buildChain` | `Sub0Pipeline_Bench.exe` | 0.708 | 7.2 |
| `sub0pipeline::Pipeline::Impl::addNode` | `Sub0Pipeline_Bench.exe` | 0.578 | 5.9 |
| `std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >::operator=` | `Sub0Pipeline_Bench.exe` | 0.315 | 3.2 |
| `sub0pipeline::Pipeline::emplace_void` | `Sub0Pipeline_Bench.exe` | 0.303 | 3.1 |
| `std::function<void (void)>::function<void (void)>` | `Sub0Pipeline_Bench.exe` | 0.247 | 2.5 |
| `aligned_malloc` | `ucrtbase.dll` | 0.153 | 1.6 |
| `sub0pipeline::Pipeline::Node::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.123 | 1.2 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `std::make_unique<struct sub0pipeline::Pipeline::Impl,0>` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |
| `sub0pipeline::Job::name` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |
| `[Import thunk memcpy]` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `std::default_delete<struct sub0pipeline::Pipeline::Impl>::operator()` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `[Import thunk memset]` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `malloc_base` | `ucrtbase.dll` | 3.820 | 38.8 |
| `std::vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >::~vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >` | `Sub0Pipeline_Bench.exe` | 2.075 | 21.1 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::emplace_back` | `Sub0Pipeline_Bench.exe` | 1.019 | 10.4 |
| `std::operator+` | `Sub0Pipeline_Bench.exe` | 0.585 | 6.0 |
| `std::operator+` | `Sub0Pipeline_Bench.exe` | 0.322 | 3.3 |
| `std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >::operator=` | `Sub0Pipeline_Bench.exe` | 0.315 | 3.2 |
| `std::function<void (void)>::function<void (void)>` | `Sub0Pipeline_Bench.exe` | 0.247 | 2.5 |
| `aligned_malloc` | `ucrtbase.dll` | 0.153 | 1.6 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.138 | 1.4 |
| `std::function<std::expected<void,enum sub0pipeline::PipelineError> __cdecl(std::stop_token)>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.132 | 1.3 |
| `sub0pipeline::Pipeline::Impl::addNode` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.8 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::operator class std::basic_string_view<char,struct std::char_traits<char> >` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `sub0pipeline::PoolSuccessors::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `std::to_string` | `Sub0Pipeline_Bench.exe` | 0.059 | 0.6 |
| `sub0pipeline::Pipeline::Node::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |
| `std::make_unique<struct sub0pipeline::Pipeline::Impl,0>` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |
| `[Import thunk memcpy]` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `std::default_delete<struct sub0pipeline::Pipeline::Impl>::operator()` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |

## 10-job linear chain

14,369,792 iterations, 696 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 8.197 | 83.4 |
| `ucrtbase.dll` | 1.633 | 16.6 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 2.774 | 28.2 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 2.362 | 24.0 |
| `malloc_base` | `ucrtbase.dll` | 1.633 | 16.6 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.844 | 8.6 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.796 | 8.1 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.714 | 7.3 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.386 | 3.9 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.140 | 1.4 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.060 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `sub0pipeline::Pipeline::Impl::joinOrphans` | `Sub0Pipeline_Bench.exe` | 0.016 | 0.2 |
| `sub0pipeline::Pipeline::run` | `Sub0Pipeline_Bench.exe` | 0.016 | 0.2 |
| `'anonymous namespace'::Runner::run<'main'::'35'::<lambda_12> >` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 2.591 | 26.4 |
| `malloc_base` | `ucrtbase.dll` | 1.633 | 16.6 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 1.209 | 12.3 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 1.074 | 10.9 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.676 | 6.9 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.551 | 5.6 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.430 | 4.4 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 0.166 | 1.7 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.140 | 1.4 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.120 | 1.2 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.109 | 1.1 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.107 | 1.1 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.9 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.9 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.9 |
| `std::stop_token::stop_requested` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.8 |
| `std::atomic_flag::test_and_set` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.8 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.060 | 0.6 |
| `std::_Func_class<void>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |

## 10-job fan-out (1 root + 9 leaves)

14,540,288 iterations, 688 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 8.334 | 85.0 |
| `ucrtbase.dll` | 1.477 | 15.1 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 3.160 | 32.2 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 2.316 | 23.6 |
| `malloc_base` | `ucrtbase.dll` | 1.477 | 15.1 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 1.069 | 10.9 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.901 | 9.2 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.472 | 4.8 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.154 | 1.6 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.9 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.063 | 0.6 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.2 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.2 |
| `sub0pipeline::Pipeline::run` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 2.434 | 24.8 |
| `malloc_base` | `ucrtbase.dll` | 1.477 | 15.1 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 1.393 | 14.2 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 1.028 | 10.5 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.838 | 8.5 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.749 | 7.6 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.523 | 5.3 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.167 | 1.7 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.140 | 1.4 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.123 | 1.2 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.108 | 1.1 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.9 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::operator[]` | `Sub0Pipeline_Bench.exe` | 0.078 | 0.8 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.063 | 0.6 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.047 | 0.5 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.045 | 0.5 |

## construct 1000-job layered DAG (20x50, fan-in 4)

73,476 iterations, 136,100 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 6.531 | 66.4 |
| `ucrtbase.dll` | 3.299 | 33.6 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `malloc_base` | `ucrtbase.dll` | 3.283 | 33.4 |
| `std::vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >::~vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >` | `Sub0Pipeline_Bench.exe` | 2.069 | 21.0 |
| `sub0pipeline::Pipeline::emplace_void` | `Sub0Pipeline_Bench.exe` | 0.966 | 9.8 |
| `sub0pipeline::Pipeline::Impl::addNode` | `Sub0Pipeline_Bench.exe` | 0.671 | 6.8 |
| `sub0pipeline::Pipeline::Node::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.615 | 6.3 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::emplace_back` | `Sub0Pipeline_Bench.exe` | 0.439 | 4.5 |
| `std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >::operator=` | `Sub0Pipeline_Bench.exe` | 0.385 | 3.9 |
| `std::to_string` | `Sub0Pipeline_Bench.exe` | 0.340 | 3.5 |
| `sub0pipeline::PoolSuccessors::push_back` | `Sub0Pipeline_Bench.exe` | 0.293 | 3.0 |
| `sub0pipeline::Job::succeed` | `Sub0Pipeline_Bench.exe` | 0.246 | 2.5 |
| `std::function<void (void)>::function<void (void)>` | `Sub0Pipeline_Bench.exe` | 0.123 | 1.3 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.107 | 1.1 |
| `'anonymous namespace'::buildLayered` | `Sub0Pipeline_Bench.exe` | 0.076 | 0.8 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.075 | 0.8 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.032 | 0.3 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.030 | 0.3 |
| `aligned_malloc` | `ucrtbase.dll` | 0.016 | 0.2 |
| `'anonymous namespace'::Runner::run<'main'::'2'::<lambda_14> >` | `Sub0Pipeline_Bench.exe` | 0.016 | 0.2 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.2 |
| `std::vector<sub0pipeline::Job,std::allocator<sub0pipeline::Job> >::push_back` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.2 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `malloc_base` | `ucrtbase.dll` | 3.283 | 33.4 |
| `std::vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >::~vector<struct sub0pipeline::Pipeline::Node,class std::allocator<struct sub0pipeline::Pipeline::Node> >` | `Sub0Pipeline_Bench.exe` | 2.069 | 21.0 |
| `sub0pipeline::PoolSuccessors::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.489 | 5.0 |
| `std::atomic<enum sub0pipeline::JobStatus>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.476 | 4.8 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::emplace_back` | `Sub0Pipeline_Bench.exe` | 0.470 | 4.8 |
| `std::operator+` | `Sub0Pipeline_Bench.exe` | 0.397 | 4.0 |
| `std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> >::operator=` | `Sub0Pipeline_Bench.exe` | 0.385 | 3.9 |
| `std::to_string` | `Sub0Pipeline_Bench.exe` | 0.371 | 3.8 |
| `sub0pipeline::PoolSuccessors::push_back` | `Sub0Pipeline_Bench.exe` | 0.278 | 2.8 |
| `std::function<std::expected<void,enum sub0pipeline::PipelineError> __cdecl(std::stop_token)>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.201 | 2.0 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.184 | 1.9 |
| `sub0pipeline::Pipeline::Node::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.183 | 1.9 |
| `sub0pipeline::Job::succeed` | `Sub0Pipeline_Bench.exe` | 0.170 | 1.7 |
| `std::function<void (void)>::function<void (void)>` | `Sub0Pipeline_Bench.exe` | 0.123 | 1.3 |
| `sub0pipeline::Pipeline::Impl::addNode` | `Sub0Pipeline_Bench.exe` | 0.108 | 1.1 |
| `sub0pipeline::Pipeline::emplace_void` | `Sub0Pipeline_Bench.exe` | 0.081 | 0.8 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::operator[]` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.8 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.075 | 0.8 |
| `std::function<std::expected<void,enum sub0pipeline::PipelineError> __cdecl(std::stop_token)>::operator=` | `Sub0Pipeline_Bench.exe` | 0.059 | 0.6 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |

## 200-job linear chain

684,819 iterations, 14,602 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 7.761 | 79.2 |
| `ucrtbase.dll` | 2.037 | 20.8 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 2.758 | 28.1 |
| `malloc_base` | `ucrtbase.dll` | 2.037 | 20.8 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 1.713 | 17.5 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 1.073 | 10.9 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.952 | 9.7 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.520 | 5.3 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.498 | 5.1 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.108 | 1.1 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.030 | 0.3 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.016 | 0.2 |
| `'anonymous namespace'::Runner::run<'main'::'35'::<lambda_12> >` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.2 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::emplace_back` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 2.122 | 21.6 |
| `malloc_base` | `ucrtbase.dll` | 2.037 | 20.8 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 1.186 | 12.1 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.841 | 8.6 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.814 | 8.3 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.692 | 7.1 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.456 | 4.7 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.229 | 2.3 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.166 | 1.7 |
| `std::_Func_class<void>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.109 | 1.1 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.108 | 1.1 |
| `std::expected<void,enum sub0pipeline::PipelineError>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.108 | 1.1 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.107 | 1.1 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.107 | 1.1 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::operator[]` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.9 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.9 |
| `std::_Func_class<void>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.076 | 0.8 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.060 | 0.6 |

## 300-job fan-out (1 root + 299 leaves)

487,636 iterations, 20,507 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 8.027 | 81.6 |
| `ucrtbase.dll` | 1.812 | 18.4 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 3.149 | 32.0 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 2.070 | 21.0 |
| `malloc_base` | `ucrtbase.dll` | 1.797 | 18.3 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.903 | 9.2 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.800 | 8.1 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.660 | 6.7 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.218 | 2.2 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.9 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.016 | 0.2 |
| `malloc` | `ucrtbase.dll` | 0.015 | 0.1 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |
| `sub0pipeline::Pipeline::Impl::joinOrphans` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |
| `sub0pipeline::PriorityExecutor::'scalar deleting destructor'` | `Sub0Pipeline_Bench.exe` | 0.013 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 2.652 | 26.9 |
| `malloc_base` | `ucrtbase.dll` | 1.797 | 18.3 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 1.469 | 14.9 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 1.032 | 10.5 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.796 | 8.1 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.633 | 6.4 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.508 | 5.2 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.093 | 0.9 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.9 |
| `std::stop_token::stop_requested` | `Sub0Pipeline_Bench.exe` | 0.076 | 0.8 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.075 | 0.8 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.064 | 0.7 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `std::expected<void,enum sub0pipeline::PipelineError>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.045 | 0.5 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |

## 1000-job layered DAG (20x50, fan-in 4)

126,273 iterations, 79,194 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 8.468 | 85.8 |
| `ucrtbase.dll` | 1.398 | 14.2 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 4.184 | 42.4 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 1.439 | 14.6 |
| `malloc_base` | `ucrtbase.dll` | 1.398 | 14.2 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.974 | 9.9 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.814 | 8.2 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.522 | 5.3 |
| `'anonymous namespace'::InlineExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.291 | 3.0 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.107 | 1.1 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.042 | 0.4 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.032 | 0.3 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.015 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 2.754 | 27.9 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 1.915 | 19.4 |
| `malloc_base` | `ucrtbase.dll` | 1.398 | 14.2 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.738 | 7.5 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.737 | 7.5 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.680 | 6.9 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.543 | 5.5 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.154 | 1.6 |
| `sub0pipeline::Pipeline::runImpl::__l2::DispatchContext::dispatchJob::__l30::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.093 | 0.9 |
| `sub0pipeline::Pipeline::Impl::execute` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.9 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.078 | 0.8 |
| `std::_Func_class<void>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.8 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `std::expected<void,enum sub0pipeline::PipelineError>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.062 | 0.6 |
| `std::_Func_class<void>::operator()` | `Sub0Pipeline_Bench.exe` | 0.061 | 0.6 |
| `operator delete` | `Sub0Pipeline_Bench.exe` | 0.046 | 0.5 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.045 | 0.5 |
| `operator new` | `Sub0Pipeline_Bench.exe` | 0.042 | 0.4 |
| `[Import thunk malloc]` | `Sub0Pipeline_Bench.exe` | 0.032 | 0.3 |
| `std::stop_source::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.031 | 0.3 |

## desktop: 10-job fan-out

759 iterations, 13,178,679 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `ntdll.dll` | 0.003 | 98.6 |
| `KERNELBASE.dll` | 0.000 | 0.9 |
| `[Unknown]` | 0.000 | 0.5 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `NtWaitForSingleObject` | `ntdll.dll` | 0.003 | 98.6 |
| `OpenThread` | `KERNELBASE.dll` | 0.000 | 0.9 |
| `[Outside any known module]` | `[Unknown]` | 0.000 | 0.5 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `NtWaitForSingleObject` | `ntdll.dll` | 0.003 | 98.6 |
| `OpenThread` | `KERNELBASE.dll` | 0.000 | 0.9 |
| `[Outside any known module]` | `[Unknown]` | 0.000 | 0.5 |

## priority(4): 10-job linear chain

590,707 iterations, 16,929 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 18.129 | 99.7 |
| `ucrtbase.dll` | 0.052 | 0.3 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 12.133 | 66.7 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 2.716 | 14.9 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 1.594 | 8.8 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.696 | 3.8 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 0.555 | 3.0 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.118 | 0.7 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.073 | 0.4 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 0.055 | 0.3 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.052 | 0.3 |
| `malloc_base` | `ucrtbase.dll` | 0.052 | 0.3 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 0.044 | 0.2 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.030 | 0.2 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.018 | 0.1 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |
| `main` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |
| `_security_check_cookie` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |
| `std::function<void __cdecl(void)>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.005 | 0.0 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.005 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 13.337 | 73.4 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 3.265 | 18.0 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.743 | 4.1 |
| `std::condition_variable::notify_all` | `Sub0Pipeline_Bench.exe` | 0.263 | 1.4 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.103 | 0.6 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.075 | 0.4 |
| `malloc_base` | `ucrtbase.dll` | 0.052 | 0.3 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.048 | 0.3 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.039 | 0.2 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 0.030 | 0.2 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.030 | 0.2 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.029 | 0.2 |
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.019 | 0.1 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.018 | 0.1 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.017 | 0.1 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.013 | 0.1 |
| `std::function<void __cdecl(void)>::operator=` | `Sub0Pipeline_Bench.exe` | 0.012 | 0.1 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.012 | 0.1 |
| `std::vector<sub0pipeline::Pipeline::Node,std::allocator<sub0pipeline::Pipeline::Node> >::operator[]` | `Sub0Pipeline_Bench.exe` | 0.007 | 0.0 |
| `sub0pipeline::PriorityExecutor::{ctor}::__l4::<lambda_1>::()::__l7::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.006 | 0.0 |

## priority(4): 300-job fan-out

59,784 iterations, 167,271 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 34.296 | 99.9 |
| `ucrtbase.dll` | 0.020 | 0.1 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 15.464 | 45.1 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 7.052 | 20.6 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 4.315 | 12.6 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 2.046 | 6.0 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 1.140 | 3.3 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 1.038 | 3.0 |
| `std::unique_lock<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.582 | 1.7 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::pop` | `Sub0Pipeline_Bench.exe` | 0.560 | 1.6 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.470 | 1.4 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 0.413 | 1.2 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.283 | 0.8 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.207 | 0.6 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.158 | 0.5 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.143 | 0.4 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.116 | 0.3 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.2 |
| `sub0pipeline::PriorityExecutor::QueuedJob::operator=` | `Sub0Pipeline_Bench.exe` | 0.077 | 0.2 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 0.043 | 0.1 |
| `[Import thunk Mtx_lock]` | `Sub0Pipeline_Bench.exe` | 0.039 | 0.1 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::push` | `Sub0Pipeline_Bench.exe` | 0.027 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 17.975 | 52.4 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 7.510 | 21.9 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 2.176 | 6.3 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 1.183 | 3.5 |
| `std::unique_lock<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.844 | 2.5 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.715 | 2.1 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::pop` | `Sub0Pipeline_Bench.exe` | 0.714 | 2.1 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.688 | 2.0 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.339 | 1.0 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.270 | 0.8 |
| `std::condition_variable::notify_all` | `Sub0Pipeline_Bench.exe` | 0.208 | 0.6 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 0.204 | 0.6 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.193 | 0.6 |
| `sub0pipeline::PriorityExecutor::{ctor}::__l4::<lambda_1>::()::__l7::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.128 | 0.4 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.118 | 0.3 |
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.116 | 0.3 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.104 | 0.3 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.103 | 0.3 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.091 | 0.3 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.079 | 0.2 |

## priority(4): 1000-job layered DAG (20x50, fan-in 4)

20,391 iterations, 490,428 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 34.968 | 99.8 |
| `ucrtbase.dll` | 0.077 | 0.2 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 12.906 | 36.8 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 7.382 | 21.1 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 6.240 | 17.8 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 1.801 | 5.1 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 1.448 | 4.1 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 1.359 | 3.9 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 1.024 | 2.9 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.593 | 1.7 |
| `std::unique_lock<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.355 | 1.0 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.353 | 1.0 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.320 | 0.9 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::pop` | `Sub0Pipeline_Bench.exe` | 0.288 | 0.8 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.248 | 0.7 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.184 | 0.5 |
| `std::_Func_class<class std::expected<void,enum sub0pipeline::PipelineError>,class std::stop_token>::operator()` | `Sub0Pipeline_Bench.exe` | 0.131 | 0.4 |
| `sub0pipeline::Pipeline::runImpl` | `Sub0Pipeline_Bench.exe` | 0.082 | 0.2 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::push` | `Sub0Pipeline_Bench.exe` | 0.082 | 0.2 |
| `sub0pipeline::PriorityExecutor::QueuedJob::operator=` | `Sub0Pipeline_Bench.exe` | 0.080 | 0.2 |
| `malloc_base` | `ucrtbase.dll` | 0.077 | 0.2 |
| `[Import thunk Mtx_unlock]` | `Sub0Pipeline_Bench.exe` | 0.026 | 0.1 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 12.463 | 35.6 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 7.774 | 22.2 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 6.649 | 19.0 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 1.814 | 5.2 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.742 | 2.1 |
| `'sub0pipeline::Pipeline::runImpl'::'2'::DispatchContext::dispatchJob` | `Sub0Pipeline_Bench.exe` | 0.733 | 2.1 |
| `std::unique_lock<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.705 | 2.0 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.566 | 1.6 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::pop` | `Sub0Pipeline_Bench.exe` | 0.525 | 1.5 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.470 | 1.3 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.428 | 1.2 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.379 | 1.1 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.293 | 0.8 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::push` | `Sub0Pipeline_Bench.exe` | 0.200 | 0.6 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.185 | 0.5 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.168 | 0.5 |
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 0.153 | 0.4 |
| `std::stop_token::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.104 | 0.3 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.092 | 0.3 |
| `std::_Atomic_storage<enum sub0pipeline::JobStatus,1>::compare_exchange_strong` | `Sub0Pipeline_Bench.exe` | 0.079 | 0.2 |

## priority(4): on-demand trigger and wait

1,210,393 iterations, 8,262 ns/op under the profiler (attribution only, not a timing).

| Module | Seconds | % |
|---|---:|---:|
| `Sub0Pipeline_Bench.exe` | 9.388 | 98.9 |
| `ucrtbase.dll` | 0.079 | 0.8 |
| `MSVCP140.dll` | 0.022 | 0.2 |

| Function (inlined callees included) | Module | Seconds | % |
|---|---|---:|---:|
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 4.846 | 51.1 |
| `'sub0pipeline::PriorityExecutor::PriorityExecutor(unsigned int,class std::function<void (void)>)'::'4'::<lambda_1>::operator()` | `Sub0Pipeline_Bench.exe` | 3.590 | 37.8 |
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 0.547 | 5.8 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.217 | 2.3 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 0.096 | 1.0 |
| `malloc_base` | `ucrtbase.dll` | 0.079 | 0.8 |
| `'anonymous namespace'::Runner::run<'main'::'41'::<lambda_27> >` | `Sub0Pipeline_Bench.exe` | 0.033 | 0.3 |
| `Mtx_unlock` | `MSVCP140.dll` | 0.022 | 0.2 |
| `sub0pipeline::PriorityExecutor::dispatch` | `Sub0Pipeline_Bench.exe` | 0.018 | 0.2 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.018 | 0.2 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.017 | 0.2 |
| `sub0pipeline::Pipeline::Impl::invoke` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `[Import thunk Mtx_unlock]` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `sub0pipeline::Pipeline::Impl::addNode` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

| Inlined frame | Module | Seconds | % |
|---|---|---:|---:|
| `std::condition_variable::wait` | `Sub0Pipeline_Bench.exe` | 5.385 | 56.8 |
| `std::condition_variable_any::wait` | `Sub0Pipeline_Bench.exe` | 3.642 | 38.4 |
| `std::condition_variable_any::notify_one` | `Sub0Pipeline_Bench.exe` | 0.235 | 2.5 |
| `malloc_base` | `ucrtbase.dll` | 0.079 | 0.8 |
| `std::condition_variable::notify_all` | `Sub0Pipeline_Bench.exe` | 0.038 | 0.4 |
| `std::chrono::steady_clock::now` | `Sub0Pipeline_Bench.exe` | 0.025 | 0.3 |
| `Mtx_unlock` | `MSVCP140.dll` | 0.022 | 0.2 |
| `std::stop_source::operator=` | `Sub0Pipeline_Bench.exe` | 0.018 | 0.2 |
| `std::lock_guard<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.017 | 0.2 |
| `'anonymous-namespace'::Runner::loop` | `Sub0Pipeline_Bench.exe` | 0.009 | 0.1 |
| `sub0pipeline::PriorityExecutor::wait_all` | `Sub0Pipeline_Bench.exe` | 0.008 | 0.1 |
| `std::unique_lock<std::mutex>::{ctor}` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::priority_queue<sub0pipeline::PriorityExecutor::QueuedJob,std::vector<sub0pipeline::PriorityExecutor::QueuedJob,std::allocator<sub0pipeline::PriorityExecutor::QueuedJob> >,std::less<sub0pipeline::PriorityExecutor::QueuedJob> >::empty` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::stop_source::get_token` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::function<void __cdecl(void)>::operator=` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `[Import thunk Mtx_unlock]` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::lock_guard<std::mutex>::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.002 | 0.0 |
| `std::basic_string<char,std::char_traits<char>,std::allocator<char> >::{dtor}` | `Sub0Pipeline_Bench.exe` | 0.000 | 0.0 |

