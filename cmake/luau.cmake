# Luau, at the release the package is built with, which the library says too. A new one is taken
# by hand: this tag, its conformance scripts run again through the package, and docs/decisions.md
# says what changed.
set(FBL_LUAU_VERSION "0.746")

# Its libraries alone, static and position-independent, to be linked into luau_native. As C++,
# Luau's default: LUAU_EXTERN_C would make its errors longjmps, which skip C++ destructors.
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LUAU_EXTERN_C OFF CACHE BOOL "" FORCE)
set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
# Its vector is 3 Float32s, std.math's Vec3, which native code needs.
set(LUAU_VECTOR_DOUBLE OFF CACHE BOOL "" FORCE)
set(LUAU_VECTOR_FOUR_WIDE OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

FetchContent_Declare(
  luau
  GIT_REPOSITORY https://github.com/luau-lang/luau.git
  GIT_TAG ${FBL_LUAU_VERSION}
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(luau)

# What luau_native is made of, in the order they link.
set(fbl_luau_libraries Luau.Require Luau.Analysis Luau.Config Luau.CodeGen Luau.Compiler
    Luau.Inliner Luau.Bytecode Luau.Ast Luau.VM Luau.Common)
foreach(library IN LISTS fbl_luau_libraries)
  set_target_properties(${library} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()
