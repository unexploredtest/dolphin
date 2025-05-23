add_library(videod3dcommon
  D3DCommon.cpp
  D3DCommon.h
  Shader.cpp
  Shader.h
  SwapChain.cpp
  SwapChain.h
)

target_link_libraries(videod3dcommon
PUBLIC
  common
  videocommon
  spirv_cross
)

<<<<<<< HEAD
=======
target_include_directories(videod3dcommon
SYSTEM PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Externals/glslang/glslang/Public
SYSTEM PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Externals/glslang/StandAlone
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Externals/glslang/SPIRV
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Externals/glslang
)

>>>>>>> b61a398e18 (Use CMAKE_CURRENT_SOURCE_DIR instead of CMAKE_SOURCE_DIR)
if(MSVC)
  # Add precompiled header
  target_link_libraries(videod3dcommon PRIVATE use_pch)
endif()
