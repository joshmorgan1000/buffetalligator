#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "alligator::alligator" for configuration "Release"
set_property(TARGET alligator::alligator APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(alligator::alligator PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liballigator.a"
  )

list(APPEND _cmake_import_check_targets alligator::alligator )
list(APPEND _cmake_import_check_files_for_alligator::alligator "${_IMPORT_PREFIX}/lib/liballigator.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
