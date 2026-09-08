# contextsnap::warnings — interface target carrying the project warning set,
# sanitizer flags and coverage instrumentation.

add_library(contextsnap_warnings INTERFACE)
add_library(contextsnap::warnings ALIAS contextsnap_warnings)

set(_csnap_msvc_warnings
    /W4
    /permissive-
    /w14242 # conversion, possible loss of data
    /w14254 # larger bit field assigned to smaller
    /w14263 # member function does not override any base class virtual
    /w14265 # class has virtual functions but non-virtual destructor
    /w14287 # unsigned/negative constant mismatch
    /w14296 # expression is always false
    /w14311 # pointer truncation
    /w14545 /w14546 /w14547 /w14549 /w14555
    /w14619 # unknown pragma warning
    /w14640 # thread-unsafe static member initialization
    /w14826 # conversion is sign-extended
    /w14905 /w14906 /w14928
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8)

set(_csnap_clang_warnings
    -Wall
    -Wextra
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough)

set(_csnap_gcc_warnings ${_csnap_clang_warnings} -Wmisleading-indentation -Wduplicated-cond
                        -Wduplicated-branches -Wlogical-op -Wuseless-cast)

if(MSVC)
  set(_csnap_warnings ${_csnap_msvc_warnings})
elseif(CMAKE_CXX_COMPILER_ID MATCHES ".*Clang")
  set(_csnap_warnings ${_csnap_clang_warnings})
else()
  set(_csnap_warnings ${_csnap_gcc_warnings})
endif()

if(CONTEXTSNAP_WARNINGS_AS_ERRORS)
  if(MSVC)
    list(APPEND _csnap_warnings /WX)
  else()
    list(APPEND _csnap_warnings -Werror)
  endif()
endif()

target_compile_options(contextsnap_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:${_csnap_warnings}>)

if(CONTEXTSNAP_ENABLE_SANITIZERS AND NOT MSVC)
  target_compile_options(contextsnap_warnings INTERFACE -fsanitize=address,undefined
                                                        -fno-omit-frame-pointer)
  target_link_options(contextsnap_warnings INTERFACE -fsanitize=address,undefined)
endif()

if(CONTEXTSNAP_ENABLE_COVERAGE AND NOT MSVC)
  target_compile_options(contextsnap_warnings INTERFACE --coverage -O0 -g)
  target_link_options(contextsnap_warnings INTERFACE --coverage)
endif()
