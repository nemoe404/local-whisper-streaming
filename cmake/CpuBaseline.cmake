# cmake/CpuBaseline.cmake: the one owner of the ggml x86 CPU baseline.
#
# Why this exists: ggml's CPU backend picks its instruction set at COMPILE time. ggml defaults
# GGML_NATIVE ON (third_party/whisper.cpp/ggml/CMakeLists.txt:123), which compiles ggml-cpu with
# -march=native (ggml/src/ggml-cpu/CMakeLists.txt:347). A binary built on one host then dies with
# SIGILL on a CPU without that host's extensions. When SOURCE_DATE_EPOCH is set or the build cross-
# compiles, every ISA default flips OFF instead (ggml/CMakeLists.txt:104-108), which silently builds
# an SSE2-only binary. Hence an explicit list, pinned with CACHE ... FORCE.
#
# ggml CPU baseline x86-64-v3 class, SSE42 AVX AVX2
# FMA F16C BMI2 ON; AVX_VNNI, the AVX512 family and AMX OFF. Why: it is the baseline consigliere ships
# on the same fork pin (operator decision 2026-10-01, its cmake/CpuBaseline.cmake), STACK.md built
# exactly this with clang 21 here (ggml-cpu variant line -msse4.2 -mf16c -mfma -mbmi2 -mavx -mavx2),
# and an SSE4.2-only build would make every later throughput number unrepresentative. Sources:
# .planning/research/STACK.md lines 83, 86, 113 and 114; .planning/research/PITFALLS.md line 221.
# STACK.md's wording is "x86-64-v3-class": this is the ggml option set, not the full psABI x86-64-v3
# level. Cost: no AVX2 means SIGILL on pre-Haswell x86; the README must state the minimum CPU.
# Reversibility: reversible now, costly after v0.1.0.
#
# Script-mode safe: including this file only defines variables and functions, so `cmake -P` on it
# is a no-op that exits 0.

# THE ONE OWNER OF THE ggml x86 BASELINE: ggml's per-ISA options that become -msse4.2 -mf16c -mfma
# -mbmi2 -mavx -mavx2 on ggml-cpu. Anything above it would need runtime dispatch, which is out of scope.
set(LWS_CPU_BASELINE_X86_64_V3 SSE42 AVX AVX2 FMA F16C BMI2)

# Every ggml x86 option ABOVE the baseline. Pinned OFF, not merely left at ggml's OFF default, so a
# -D or a stale cache cannot turn one back on.
set(LWS_GGML_CPU_ABOVE_BASELINE
    AVX_VNNI AVX512 AVX512_VBMI AVX512_VNNI AVX512_BF16 AMX_TILE AMX_INT8 AMX_BF16)

# lws_select_cpu_baseline(<out_list> <out_reason> <name>)
# Sets <out_list> in the caller to the ISA list <name> selects and <out_reason> to "". An unknown
# name, empty included, yields an empty list and a `[isa]` reason: there is no default, so a
# configure without a preset stops. Pure: reads only its arguments and the owned list.
function(lws_select_cpu_baseline out_list out_reason name)
    if("${name}" STREQUAL "x86-64-v3")
        set(_list ${LWS_CPU_BASELINE_X86_64_V3})
        set(_reason "")
    else()
        set(_list "")
        set(_reason "[isa] unknown LWS_CPU_BASELINE '${name}' (known: x86-64-v3)")
    endif()
    set(${out_list} "${_list}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

# lws_isa_flag_refusal(<out_var> <option>)
# Sets <out_var> in the caller to one `[isa]` reason for <option>, or to "" when the option holds the
# baseline. The one rule for a single option, shared by lws_ggml_isa_verdict (configure) and the
# compile-database sweep (scripts/isa-gate.cmake):
#   - any -march=, -mtune= or -mcpu= whatever its value (a generator expression still counts);
#   - any above-baseline ISA flag (-mavx512*, -mavxvnni*, -mamx*, -mavx10*).
# The boundary keeps -DX-march=native from matching: the flag must start the argument or follow a
# character that is not a name character or a dash. Pure: reads only its arguments.
function(lws_isa_flag_refusal out_var option)
    set(_reason "")
    set(_boundary "(^|[^-A-Za-z0-9_.])")
    if("${option}" MATCHES "${_boundary}(-m(arch|tune|cpu)=[^ >;]*)")
        set(_reason "[isa] host/arch flag ${CMAKE_MATCH_2}")
    elseif("${option}" MATCHES "${_boundary}(-m(avx512|avxvnni|amx|avx10)[^ >;]*)")
        set(_reason "[isa] above-baseline flag ${CMAKE_MATCH_2}")
    endif()
    set(${out_var} "${_reason}" PARENT_SCOPE)
endfunction()

# lws_ggml_isa_verdict(<out_var> <compile-option>...)
# Sets <out_var> in the caller to a list of `[isa]` reasons, one per finding, or to "" when the
# options hold the baseline:
#   - every option that lws_isa_flag_refusal refuses;
#   - no -mavx2 item at all: non-vacuity, the caller must have read ggml-cpu's real options.
# Pure: reads only its arguments.
function(lws_ggml_isa_verdict out_var)
    set(_reasons "")
    foreach(_opt IN LISTS ARGN)
        lws_isa_flag_refusal(_reason "${_opt}")
        if(NOT "${_reason}" STREQUAL "")
            list(APPEND _reasons "${_reason}")
        endif()
    endforeach()
    if(NOT "-mavx2" IN_LIST ARGN)
        list(APPEND _reasons
            "[isa] no -mavx2 among the options: the check did not read ggml-cpu's real flags")
    endif()
    set(${out_var} "${_reasons}" PARENT_SCOPE)
endfunction()

# lws_pin_ggml_cpu_baseline()
# Macro, so the cache writes land in the caller's scope. Reads LWS_SELECTED_CPU_ISAS, which the
# caller sets with lws_select_cpu_baseline. Every entry is CACHE ... FORCE, so neither a -D option
# nor a stale cache can flip the baseline: a pre-existing cache holding GGML_NATIVE=ON would
# otherwise survive a plain option().
macro(lws_pin_ggml_cpu_baseline)
    set(GGML_NATIVE OFF CACHE BOOL "ggml: -march=native, pinned OFF (cmake/CpuBaseline.cmake)" FORCE)
    set(GGML_BACKEND_DL OFF CACHE BOOL "ggml: dynamic backends, pinned OFF (cmake/CpuBaseline.cmake)" FORCE)
    set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "ggml: runtime CPU variants, pinned OFF (cmake/CpuBaseline.cmake)" FORCE)
    foreach(_lws_isa IN LISTS LWS_SELECTED_CPU_ISAS)
        set(GGML_${_lws_isa} ON CACHE BOOL "ggml: ${_lws_isa} is in the baseline (cmake/CpuBaseline.cmake)" FORCE)
    endforeach()
    foreach(_lws_isa IN LISTS LWS_GGML_CPU_ABOVE_BASELINE)
        set(GGML_${_lws_isa} OFF CACHE BOOL "ggml: ${_lws_isa} is above the baseline (cmake/CpuBaseline.cmake)" FORCE)
    endforeach()
    unset(_lws_isa)
endmacro()

# lws_check_ggml_cpu_baseline()
# Runs the verdict on the flags ggml-cpu was actually given. FATAL_ERROR with `[isa]` reasons on
# any finding; otherwise one STATUS line listing the realised -m options.
function(lws_check_ggml_cpu_baseline)
    if(NOT TARGET ggml-cpu)
        message(FATAL_ERROR
            "lws: [isa] no ggml-cpu target after add_subdirectory(third_party/whisper.cpp); the baseline check cannot run")
    endif()
    get_target_property(_opts ggml-cpu COMPILE_OPTIONS)
    if(NOT _opts)
        set(_opts "")
    endif()
    lws_ggml_isa_verdict(_reasons ${_opts})
    if(NOT "${_reasons}" STREQUAL "")
        list(JOIN _reasons "\n  " _text)
        message(FATAL_ERROR
            "lws: ggml-cpu breaks the CPU baseline (cmake/CpuBaseline.cmake):\n  ${_text}")
    endif()
    set(_isa_flags "")
    foreach(_opt IN LISTS _opts)
        if(_opt MATCHES "^-m")
            list(APPEND _isa_flags "${_opt}")
        endif()
    endforeach()
    list(JOIN _isa_flags " " _isa_text)
    message(STATUS "lws: [isa] ggml-cpu baseline holds: ${_isa_text}")
endfunction()
