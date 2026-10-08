# Benchmark suite (audit issue-1/task-0.2; policy: ADR-0005).
#
# One executable per audited component, named pdtk_bench_<component>.
# Which components have a benchmark at any point in time is decided by one
# rule: a benchmark exists exactly when the component it measures exists —
# benchmarks never fake numbers for unbuilt engines (audit's task 7.1
# honest-stub principle, applied to the pipeline itself; see P-012).
#
#   task 1.2 (BumpArena)      -> pdtk_bench_arena   (landed with task 0.2)
#   task 1.1 (MmapHandle)     -> pdtk_bench_mmap    (lands with task 1.1)
#   task 1.3 (PageSlab)       -> pdtk_bench_slab    (lands with task 1.3)
#   task 2.3 (ZeroCopyLexer)  -> pdtk_bench_lexer   (lands with task 2.3)
#   task 4.2 (BlockMaxWand)   -> pdtk_bench_wand    (lands with task 4.2)

# Every benchmark links pdftoolkit_core (reuse: no parallel implementation
# of engine logic inside benchmarks) and, crucially, the pdtk_native_arch
# profile: the measured code (e.g. BumpArena::alloc_slice) is header-inline,
# so the benchmark TU's optimization flags decide the measured codegen —
# not just the prebuilt library's.
add_executable(pdtk_bench_arena bench_arena.cpp)
target_link_libraries(pdtk_bench_arena
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_arena PRIVATE pdtk_native_arch)
endif()

# task 1.1 (MmapHandle) -> pdtk_bench_mmap (component-exists rule above)
add_executable(pdtk_bench_mmap bench_mmap.cpp)
target_link_libraries(pdtk_bench_mmap
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_mmap PRIVATE pdtk_native_arch)
endif()
