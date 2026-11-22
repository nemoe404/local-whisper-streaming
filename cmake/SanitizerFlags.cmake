# The one owner of the sanitizer flag lists. The whisper.cpp project hook (EnableSanitizers.cmake),
# the root build (the lws_sanitizers INTERFACE target) and scripts/sanitizer-gate.cmake all read the
# lists from here, so the build and the gate cannot disagree about what "instrumented" means.
#
# Script-mode safe: defining the function has no side effect.
#
# lws_sanitizer_flags(<sanitizer> <out_compile> <out_link>)
#   off or empty  -> two empty lists
#   asan-ubsan    -> compile: -fsanitize=address,undefined;-fno-omit-frame-pointer;-fno-sanitize-recover=all
#                    link:    -fsanitize=address,undefined
#   tsan          -> compile: -fsanitize=thread;-fno-omit-frame-pointer
#                    link:    -fsanitize=thread
# The first compile element is the token a compile-database entry must carry (the gate looks for it).
# Any other value stops the configure: a typo must never build an uninstrumented leg that still calls
# itself asan-ubsan. That branch is this file's red twin.
function(lws_sanitizer_flags sanitizer out_compile out_link)
    if("${sanitizer}" STREQUAL "" OR "${sanitizer}" STREQUAL "off")
        set(${out_compile} "" PARENT_SCOPE)
        set(${out_link} "" PARENT_SCOPE)
    elseif("${sanitizer}" STREQUAL "asan-ubsan")
        set(${out_compile}
            "-fsanitize=address,undefined;-fno-omit-frame-pointer;-fno-sanitize-recover=all"
            PARENT_SCOPE)
        set(${out_link} "-fsanitize=address,undefined" PARENT_SCOPE)
    elseif("${sanitizer}" STREQUAL "tsan")
        set(${out_compile} "-fsanitize=thread;-fno-omit-frame-pointer" PARENT_SCOPE)
        set(${out_link} "-fsanitize=thread" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "lws: unknown LWS_SANITIZER='${sanitizer}' (expected asan-ubsan, tsan or off)")
    endif()
endfunction()

# lws_sanitizer_whisper_extras(<sanitizer> <out_compile>)
# Compile options added only inside the whisper.cpp subtree, after the lists above. lws targets never get
# them. asan-ubsan switches the UBSan `function` check off there: ggml-cpu.c:1244 calls vec_dot through a
# `ggml_vec_dot_t` pointer (const void * parameters) whose target, ggml_vec_dot_f16 in vec.cpp:264, takes
# ggml_fp16_t * parameters. That is a function-type mismatch inside the fork (call site and callee both in
# ggml), ABI-identical, and with -fno-sanitize-recover=all it aborts every model-backed run. The fix belongs
# in the fork (a commit on the consigliere-v1.9.5 branch); until then this is the narrowest switch: one
# check, one subtree, no suppression file. Recorded in the 01-03 summary as a deviation.
function(lws_sanitizer_whisper_extras sanitizer out_compile)
    if("${sanitizer}" STREQUAL "asan-ubsan")
        set(${out_compile} "-fno-sanitize=function" PARENT_SCOPE)
    else()
        set(${out_compile} "" PARENT_SCOPE)
    endif()
endfunction()
