# Shared warning profile — the zero-warning policy of audit issue-1/task-0.1.
# Linked as a PRIVATE dependency by every pdftoolkit target.
add_library(pdtk_warnings INTERFACE)

if(MSVC)
  target_compile_options(pdtk_warnings INTERFACE /W4 /permissive-)
  if(PDTK_WERROR)
    target_compile_options(pdtk_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(pdtk_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -fno-omit-frame-pointer)
  if(PDTK_WERROR)
    target_compile_options(pdtk_warnings INTERFACE -Werror)
  endif()
endif()
