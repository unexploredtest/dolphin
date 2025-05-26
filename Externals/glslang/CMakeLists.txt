set(ENABLE_PCH OFF)
set(BUILD_EXTERNAL OFF)
set(SKIP_GLSLANG_INSTALL ON)
set(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_HLSL OFF)
set(ENABLE_OPT OFF)
set(ENABLE_CTEST OFF)
set(ENABLE_EXCEPTIONS OFF)
add_subdirectory(glslang)

if (TARGET glslang)
  target_include_directories(glslang
  # Silence warnings on glslang by flagging it as a system include
  SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glslang>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/glslang/Public>
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )
endif()
<<<<<<< HEAD
=======

if(NOT MSVC)
# glslang requires C++11 at a minimum to compile.
add_compile_options(-std=c++11)

# Silence some warnings that occur frequently to reduce noise in build logs.
add_compile_options(-Wno-shadow)
add_compile_options(-Wno-reorder)
add_compile_options(-Wno-sign-compare)
add_compile_options(-Wno-parentheses)
add_compile_options(-Wno-unused-variable)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
	add_compile_options(-Wno-unused-but-set-variable)
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
	add_compile_options(-Wno-missing-variable-declarations)
endif()
endif()

add_library(glslang STATIC ${SRCS})
dolphin_disable_warnings(glslang)

target_include_directories(glslang
PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)
>>>>>>> 6c98e7b538 (Move glslang target_include_directories back to glslang's CMakeLists.txt)
