# Luau, at the release the package is built with, which the library says too. A new one is taken
# by hand: this tag, its conformance scripts run again through the package, and docs/decisions.md
# says what changed.
set(FBL_LUAU_VERSION "0.746")
# The commit the tag named when the package was pinned to it: a tag can be moved, and the configure
# stops when this one has been.
set(FBL_LUAU_COMMIT "12a6ba75883272c79b66b59815efef4c574196f6")

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
include("${CMAKE_CURRENT_LIST_DIR}/luau_commit.cmake")
fbl_check_luau_commit("${luau_SOURCE_DIR}" "${FBL_LUAU_COMMIT}")

# What luau_native is made of, in the order they link.
set(fbl_luau_libraries Luau.Require Luau.Analysis Luau.Config Luau.CodeGen Luau.Compiler
    Luau.Inliner Luau.Bytecode Luau.Ast Luau.VM Luau.Common)
foreach(library IN LISTS fbl_luau_libraries)
  set_target_properties(${library} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  # Luau casts a double to an int and keeps what the processor gives, a NaN's or a huge one's too
  # (luaH_get, lua_tointegerx, the VM's loops): its design, which UBSan's float-cast-overflow calls
  # undefined. The check stays on for the shim's own code.
  if(FBL_SANITIZE MATCHES "undefined")
    target_compile_options(${library} PRIVATE -fno-sanitize=float-cast-overflow)
  endif()
endforeach()
# Luau's source isn't patched where a script's ints overflow in it (Luau 0.746, findings 139 and
# 141); these files are built with -fwrapv, in every build, so what the script gets is defined, a
# wrapped int, and UBSan's signed-integer-overflow, GCC's and Clang's, has nothing there to report:
# - loslib.cpp: os_time takes 1 from a script's month and 1900 from its year (:193, :194),
#   os_timegm adds and multiplies them (:37 to :40), and os_date adds 1900 to gmtime's year
#   (:142): os.time({ month = -2147483648, ... }).
# - lbitlib.cpp: b_rshift and b_rrot negate a count (:99, :137), b_arshift and b_shift negate it
#   again (:107, :73), so bit32.rshift(x, -2147483648) negates INT_MIN, and fieldargs adds a
#   field and its width (:152).
# - ltablib.cpp: table.concat takes 1 from its first index (:307), and table.find's loop goes past
#   an index of 2147483647 (:645), as ltable.cpp's luaH_rawfind does (:419), which table.find
#   calls instead with LuauTableFindImprovement, one of the flags the shim turns on.
# - lbuflib.cpp: buffer.copy's and buffer.fill's sizes take an offset from a length (:257, :278),
#   given a size or not.
# - lutf8lib.cpp: utf8.codes' iterator takes 1 from what it's given (:244).
# - lbaselib.cpp: ipairs' iterator adds 1 to what it's given (:241).
# What follows a wrapped count in lbitlib.cpp, a shift by a negative or by more than 31 (:78, and
# b_extract's and b_replace's by a wrapped field, :163, :176), -fwrapv doesn't define; the
# processor takes it modulo 32. So UBSan's shift checks are off for that file alone in the
# sanitizers' builds, the whole group, as GCC 15 keeps checking an exponent with shift-exponent
# alone off. The checks stay on for the rest of Luau and for the shim.
set(fbl_luau_wrapped loslib lbitlib ltablib ltable lbuflib lutf8lib lbaselib)
list(TRANSFORM fbl_luau_wrapped PREPEND "${luau_SOURCE_DIR}/VM/src/")
list(TRANSFORM fbl_luau_wrapped APPEND ".cpp")
set_source_files_properties(${fbl_luau_wrapped} TARGET_DIRECTORY Luau.VM
                            PROPERTIES COMPILE_OPTIONS "-fwrapv")
if(FBL_SANITIZE MATCHES "undefined")
  set_property(
    SOURCE "${luau_SOURCE_DIR}/VM/src/lbitlib.cpp" TARGET_DIRECTORY Luau.VM APPEND
    PROPERTY COMPILE_OPTIONS "-fno-sanitize=shift")
endif()
