# Sanitizer instrumentation for the whisper.cpp subtree (ggml included).
#
# Included as the LAST step of project("whisper.cpp" C CXX) (third_party/whisper.cpp/CMakeLists.txt
# line 2) through CMAKE_PROJECT_whisper.cpp_INCLUDE. That timing is the point of this file. It runs
# inside whisper.cpp's own directory scope, after C and C++ are enabled and before whisper adds ggml
# (line 168) and src (line 188).
#
# WHO ATTACHES IT
# ---------------
# The top-level CMakeLists.txt sets CMAKE_PROJECT_whisper.cpp_INCLUDE to this file right before
# add_subdirectory(third_party/whisper.cpp ...) whenever LWS_SANITIZER is not off. No preset names the
# hook, so `cmake --preset tsan` and a preset-less `cmake -DLWS_SANITIZER=tsan` take the same route.
#
# WHY NOT PRESET cacheVariables
# -----------------------------
# The Conan toolchain contributes the profile's flags through the *_FLAGS_INIT variables, which only
# populate a cache entry that does not exist yet. A compiler-flags cache variable set by a preset
# creates the entry first and discards the toolchain's contribution wholesale (measured in
# consigliere). So no preset carries a compiler-flags cache variable.
#
# WHAT IT DOES
# ------------
# add_compile_options and add_link_options here set directory properties that whisper.cpp's targets
# and the ggml subdirectory inherit and that nothing outside that subtree sees. No CMAKE_<LANG>_FLAGS
# variable is touched, so Conan's flags stay intact and the wording holds literally. The compile
# flags apply to C and C++ sources only: ggml enables ASM, and the flags are meaningless to it.
#
# lws targets get the same lists through the lws_sanitizers INTERFACE target in the root
# CMakeLists.txt, and scripts/sanitizer-gate.cmake proves every compile entry carries them.
#
# Measured scope: measurements/sanitizer-scope.md, Outcome A under the post-hoc amended H1 (the amendment
# and the outcome are recorded there). The tsan test preset sets no label filter, so real-model and ggml-mt
# tests run on tsan with ggml instrumented (scope all); label smoke is never filtered. The measurement covers
# our code and its boundary with whisper.cpp only: five runs per configuration and thread count. It does not
# prove that ggml's internals are race-free.

if(NOT "${PROJECT_NAME}" STREQUAL "whisper.cpp")
    message(FATAL_ERROR
        "lws: EnableSanitizers.cmake was attached to project '${PROJECT_NAME}', not whisper.cpp")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/SanitizerFlags.cmake)
lws_sanitizer_flags("${LWS_SANITIZER}" _lws_san_compile _lws_san_link)

if("${_lws_san_compile}" STREQUAL "")
    return()
endif()

# Measurement-only switch (LWS_SANITIZER_SCOPE, root CMakeLists.txt): uninstrumented ggml under TSan.
if("${LWS_SANITIZER_SCOPE}" STREQUAL "lws-only")
    message(STATUS "lws: [sanitizer] whisper.cpp left uninstrumented (measurement-only scope lws-only)")
    return()
endif()

# Subtree-only extras (SanitizerFlags.cmake documents the one entry and why), after the main flags.
lws_sanitizer_whisper_extras("${LWS_SANITIZER}" _lws_san_extras)
set(_lws_san_all "${_lws_san_compile};${_lws_san_extras}")
list(FILTER _lws_san_all EXCLUDE REGEX "^$")
set(_lws_san_wrapped "${_lws_san_all}")
list(TRANSFORM _lws_san_wrapped PREPEND "$<$<COMPILE_LANGUAGE:C,CXX>:")
list(TRANSFORM _lws_san_wrapped APPEND ">")
add_compile_options(${_lws_san_wrapped})
add_link_options(${_lws_san_link})

list(JOIN _lws_san_all " " _lws_san_text)
message(STATUS "lws: [sanitizer] whisper.cpp subtree instrumented: ${_lws_san_text}")
