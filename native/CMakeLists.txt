# The shim: each area's C API, native/<area>/fbl_<area>.h, with its .cpp files beside it, compiled
# once into luau_native, the package's library, and into fbl_internal, a static library of the
# same code for the C++ tests, which reach what the library doesn't export.
file(GLOB fbl_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/*.cpp")

add_library(fbl_objects OBJECT ${fbl_sources})
fbl_set_warnings(fbl_objects)
target_include_directories(fbl_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fbl_objects PUBLIC FBL_BUILDING
                           PRIVATE FBL_VERSION="${FBL_VERSION}"
                                   FBL_LUAU_VERSION="${FBL_LUAU_VERSION}")
target_link_libraries(fbl_objects PUBLIC ${fbl_luau_libraries})
# The stack guard asks pthreads where a thread's stack ends, which glibc before 2.34 has in
# libpthread: whatever links the shim, the library, its tests, or the fuzzers, links it too.
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
  find_package(Threads REQUIRED)
  target_link_libraries(fbl_objects PUBLIC Threads::Threads)
endif()
set_target_properties(fbl_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT WIN32)
  set_target_properties(fbl_objects PROPERTIES CXX_VISIBILITY_PRESET hidden
                        VISIBILITY_INLINES_HIDDEN ON)
endif()

fbl_add_shared(luau_native fbl_ "${PROJECT_SOURCE_DIR}/luau/native/${FBL_TARGET}"
               $<TARGET_OBJECTS:fbl_objects>)
target_link_libraries(luau_native PRIVATE ${fbl_luau_libraries})
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
  target_link_libraries(luau_native PRIVATE Threads::Threads)
endif()

add_library(fbl_internal STATIC $<TARGET_OBJECTS:fbl_objects>)
target_link_libraries(fbl_internal PUBLIC fbl_objects)
