
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was alligatorConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

macro(set_and_check _var _file)
  set(${_var} "${_file}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "File or directory ${_file} referenced by variable ${_var} does not exist !")
  endif()
endmacro()

macro(check_required_components _NAME)
  foreach(comp ${${_NAME}_FIND_COMPONENTS})
    if(NOT ${_NAME}_${comp}_FOUND)
      if(${_NAME}_FIND_REQUIRED_${comp})
        set(${_NAME}_FOUND FALSE)
      endif()
    endif()
  endforeach()
endmacro()

####################################################################################
# find_dependency() below rewrites PACKAGE_PREFIX_DIR, so this package's install prefix is saved first.
set(BUFFETALLIGATOR_PREFIX_DIR "${PACKAGE_PREFIX_DIR}")
include(CMakeFindDependencyMacro)
find_dependency(Threads)
find_dependency(OpenSSL)
if(NOT APPLE)
    find_dependency(OpenMP COMPONENTS CXX)
endif()
if(NOT TARGET alligator::vulkan)
    add_library(alligator::vulkan SHARED IMPORTED)
    set_target_properties(alligator::vulkan PROPERTIES
        IMPORTED_LOCATION "${BUFFETALLIGATOR_PREFIX_DIR}/lib/alligator/libMoltenVK.dylib"
        INTERFACE_INCLUDE_DIRECTORIES "${BUFFETALLIGATOR_PREFIX_DIR}/include")
endif()
if(NOT TARGET alligator::shaderc)
    add_library(alligator::shaderc STATIC IMPORTED)
    set_target_properties(alligator::shaderc PROPERTIES
        IMPORTED_LOCATION "${BUFFETALLIGATOR_PREFIX_DIR}/lib/alligator/libshaderc_combined.a")
endif()
foreach(dependency IN ITEMS uv sodium fabric)
    if(NOT TARGET alligator::${dependency})
        add_library(alligator::${dependency} STATIC IMPORTED)
        set_target_properties(alligator::${dependency} PROPERTIES
            IMPORTED_LOCATION "${BUFFETALLIGATOR_PREFIX_DIR}/lib/alligator/lib${dependency}.a")
    endif()
endforeach()
set_property(TARGET alligator::uv PROPERTY INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_property(TARGET alligator::uv APPEND PROPERTY INTERFACE_LINK_LIBRARIES rt m)
endif()
set_property(TARGET alligator::fabric PROPERTY INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};")
if(NOT "../logging/loggingTargets.cmake" STREQUAL "")
    include("${CMAKE_CURRENT_LIST_DIR}/../logging/loggingTargets.cmake")
elseif(NOT TARGET logging::logging)
    find_dependency(logging CONFIG)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/alligatorTargets.cmake")
check_required_components(alligator)
