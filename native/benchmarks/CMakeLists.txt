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
#   task 1.3 (PageSlab)       -> pdtk_bench_slab    (landed with task 1.3)
#   task 2.3 (ZeroCopyLexer)  -> pdtk_bench_lexer   (landed with task 2.3)
#   task 2.4 (Flate codec)    -> pdtk_bench_flate   (lands with task 2.4;
#                              exists only when PDTK_HAVE_FLATE - the stub
#                              measures nothing)
#   task 3.1 (CMap resolver)  -> pdtk_bench_cmap    (lands with task 3.1)
#   task 3.2 (operator state
#                 machine)    -> pdtk_bench_evaluator (lands with task 3.2;
#                              precision gate is the test suite's golden
#                              vectors — this measures the per-stream cost
#                              3.3 builds on)
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

# task 1.3 (PageSlab) -> pdtk_bench_slab (component-exists rule above)
add_executable(pdtk_bench_slab bench_slab.cpp)
target_link_libraries(pdtk_bench_slab
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_slab PRIVATE pdtk_native_arch)
endif()

# task 2.3 (ZeroCopyLexer) -> pdtk_bench_lexer (component-exists rule above)
add_executable(pdtk_bench_lexer bench_lexer.cpp)
target_link_libraries(pdtk_bench_lexer
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_lexer PRIVATE pdtk_native_arch)
endif()

# task 2.4 (Flate codec) -> pdtk_bench_flate (component-exists rule: the
# benchmark exists exactly when the real codec does - PDTK_ENABLE_FLATE=OFF
# compiles a stub that must never produce numbers).
if(PDTK_HAVE_FLATE)
  add_executable(pdtk_bench_flate bench_flate.cpp)
  target_link_libraries(pdtk_bench_flate
    PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings
            libdeflate_static)
  if(TARGET pdtk_native_arch)
    target_link_libraries(pdtk_bench_flate PRIVATE pdtk_native_arch)
  endif()
endif()

# task 3.1 (CMap cache & Unicode resolver) -> pdtk_bench_cmap
# (component-exists rule above; measures parse/lookup/intern — the
# Flate path is pdtk_bench_flate's subject, so no libdeflate here).
add_executable(pdtk_bench_cmap bench_cmap.cpp)
target_link_libraries(pdtk_bench_cmap
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_cmap PRIVATE pdtk_native_arch)
endif()

# task 3.2 (operator state machine) -> pdtk_bench_evaluator
# (component-exists rule above; no gate on this task — the benchmark
# gives task 3.3 its front-half baseline and reports the U-012
# same-run traversal control).
add_executable(pdtk_bench_evaluator bench_evaluator.cpp)
target_link_libraries(pdtk_bench_evaluator
  PRIVATE pdftoolkit_core benchmark::benchmark pdtk_warnings)
if(TARGET pdtk_native_arch)
  target_link_libraries(pdtk_bench_evaluator PRIVATE pdtk_native_arch)
endif()
