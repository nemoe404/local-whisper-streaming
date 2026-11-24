# scripts/toolchain-gate.cmake: the toolchain gate.
#
#   cmake -DLWS_ROOT={root} [-DLWS_BUILD_ID={id} -DLWS_LEGS={leg}] -P scripts/toolchain-gate.cmake
#
# Two layers, one verdict. The static layer (always runs) reads every non-hidden configure preset in
# CMakePresets.json and, when present, CMakeUserPresets.json; it resolves inherits (the preset's own
# value, else the first parent in inherits order that has one) and checks CMAKE_C_COMPILER,
# CMAKE_CXX_COMPILER and toolchainFile. A preset that turns GGML_CUDA on (anything but OFF, FALSE, 0, NO
# or N; a value that is not readable as a string counts as on) must also pin CMAKE_CUDA_HOST_COMPILER to
# clang++-21, CMAKE_CUDA_COMPILER to an nvcc, and CMAKE_CUDA_ARCHITECTURES to explicit values, never
# native; a preset with CUDA off still has its CUDA host checked when one is set. The configured layer
# (only when LWS_LEGS is given) reads the compiler identity CMake recorded for each leg and the compiler of
# every compile-database entry, and feeds the identity through lws_clang21_verdict, the same function
# configure runs (cmake/Clang21Toolchain.cmake). A leg with CUDA on also has its recorded nvcc and CUDA
# host identity checked, and every nvcc compile-database entry must name a clang 21 host (-ccbin).
#
# Exit: 0 holds; 1 refusal (a preset or a leg resolves to something other than clang 21); 2 missing
# input. A refusal outranks missing input: a leg whose configure stopped at the verdict has compiler-ID
# files but no compile database, and still reports [toolchain] with exit 1.
cmake_minimum_required(VERSION 3.29)

if(NOT DEFINED LWS_ROOT OR "${LWS_ROOT}" STREQUAL "")
    message(FATAL_ERROR "toolchain-gate: -DLWS_ROOT={root} is required")
endif()
include(${LWS_ROOT}/cmake/Clang21Toolchain.cmake)

set(LWS_TG_REFUSALS "")
set(LWS_TG_MISSING "")
set(LWS_TG_PRESETS 0)
set(LWS_TG_LEGS 0)
set(LWS_TG_ENTRIES 0)

# ---------------------------------------------------------------------------------------------
# JSON helpers. Each takes a document text and never reads a file.

# lws_tg_preset_index(<out_var> <doc> <name>): index into <doc>'s configurePresets whose name is
# <name>, or -1. A document with no configurePresets array has no presets.
function(lws_tg_preset_index out_var doc name)
    set(${out_var} -1 PARENT_SCOPE)
    string(JSON _count ERROR_VARIABLE _err LENGTH "${doc}" configurePresets)
    if(_err OR _count EQUAL 0)
        return()
    endif()
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _n ERROR_VARIABLE _nerr GET "${doc}" configurePresets ${_i} name)
        if(NOT _nerr AND "${_n}" STREQUAL "${name}")
            set(${out_var} ${_i} PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# lws_tg_own_field(<out_found> <out_value> <doc> <index> <key>): the preset's own member at <key>,
# a member path such as "toolchainFile" or "cacheVariables;CMAKE_C_COMPILER". A cacheVariables entry
# is either a string or an object whose "value" is the string. Non-string values are found with an
# empty value, which every comparison below refuses.
function(lws_tg_own_field out_found out_value doc index key)
    set(${out_found} FALSE PARENT_SCOPE)
    set(${out_value} "" PARENT_SCOPE)
    string(JSON _type ERROR_VARIABLE _err TYPE "${doc}" configurePresets ${index} ${key})
    if(_err OR "${_type}" STREQUAL "NULL")
        return()
    endif()
    set(${out_found} TRUE PARENT_SCOPE)
    if("${_type}" STREQUAL "STRING")
        string(JSON _v GET "${doc}" configurePresets ${index} ${key})
        set(${out_value} "${_v}" PARENT_SCOPE)
    elseif("${_type}" STREQUAL "OBJECT")
        string(JSON _vt ERROR_VARIABLE _verr TYPE "${doc}" configurePresets ${index} ${key} value)
        if(NOT _verr AND "${_vt}" STREQUAL "STRING")
            string(JSON _v GET "${doc}" configurePresets ${index} ${key} value)
            set(${out_value} "${_v}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

# lws_tg_resolve_field(<out_found> <out_value> <name> <key> <depth> <docA> <docB>): the value of
# <key> for preset <name>, resolved through inherits. The preset is looked up in <docA> first, then
# <docB> (CMakeUserPresets.json). The depth guard stops a cycle that CMake itself would reject.
function(lws_tg_resolve_field out_found out_value name key depth docA docB)
    set(${out_found} FALSE PARENT_SCOPE)
    set(${out_value} "" PARENT_SCOPE)
    if(depth GREATER 16)
        return()
    endif()
    lws_tg_preset_index(_idx "${docA}" "${name}")
    set(_doc "${docA}")
    if(_idx LESS 0)
        lws_tg_preset_index(_idx "${docB}" "${name}")
        set(_doc "${docB}")
    endif()
    if(_idx LESS 0)
        return()
    endif()

    lws_tg_own_field(_own _val "${_doc}" ${_idx} "${key}")
    if(_own)
        set(${out_found} TRUE PARENT_SCOPE)
        set(${out_value} "${_val}" PARENT_SCOPE)
        return()
    endif()

    string(JSON _it ERROR_VARIABLE _ierr TYPE "${_doc}" configurePresets ${_idx} inherits)
    if(_ierr OR "${_it}" STREQUAL "NULL")
        return()
    endif()
    set(_parents "")
    if("${_it}" STREQUAL "STRING")
        string(JSON _p GET "${_doc}" configurePresets ${_idx} inherits)
        list(APPEND _parents "${_p}")
    elseif("${_it}" STREQUAL "ARRAY")
        string(JSON _pn LENGTH "${_doc}" configurePresets ${_idx} inherits)
        if(_pn GREATER 0)
            math(EXPR _plast "${_pn} - 1")
            foreach(_j RANGE ${_plast})
                string(JSON _p GET "${_doc}" configurePresets ${_idx} inherits ${_j})
                list(APPEND _parents "${_p}")
            endforeach()
        endif()
    endif()

    math(EXPR _next "${depth} + 1")
    foreach(_parent IN LISTS _parents)
        lws_tg_resolve_field(_f _v "${_parent}" "${key}" ${_next} "${docA}" "${docB}")
        if(_f)
            set(${out_found} TRUE PARENT_SCOPE)
            set(${out_value} "${_v}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# lws_tg_is_clang21(<out_var> <path>): TRUE when <path> names clang-21 or clang++-21 by file name, or
# resolves to a file under the LLVM 21 install directory.
function(lws_tg_is_clang21 out_var path)
    set(${out_var} FALSE PARENT_SCOPE)
    if("${path}" STREQUAL "")
        return()
    endif()
    get_filename_component(_base "${path}" NAME)
    if("${_base}" STREQUAL "clang-${LWS_LLVM_MAJOR}" OR "${_base}" STREQUAL "clang++-${LWS_LLVM_MAJOR}")
        set(${out_var} TRUE PARENT_SCOPE)
    elseif("${path}" MATCHES "/")
        get_filename_component(_real "${path}" REALPATH)
        if("${_real}" MATCHES "^/usr/lib/llvm-${LWS_LLVM_MAJOR}/")
            set(${out_var} TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# Static layer: the declared presets.

set(LWS_TG_EXPECTED_C "clang-${LWS_LLVM_MAJOR}")
set(LWS_TG_EXPECTED_CXX "clang++-${LWS_LLVM_MAJOR}")
# Literal text as written in CMakePresets.json: ${sourceDir} and $penv{} are preset macros, not CMake
# variables, so they must not expand here.
set(LWS_TG_EXPECTED_TOOLCHAIN "\${sourceDir}/build/\$penv{LWS_BUILD_ID}/conan/conan_toolchain.cmake")

set(_tg_present_a FALSE)
set(_tg_present_b FALSE)
set(_tg_doc_a "")
set(_tg_doc_b "{}")
if(EXISTS "${LWS_ROOT}/CMakePresets.json")
    set(_tg_present_a TRUE)
    file(READ "${LWS_ROOT}/CMakePresets.json" _tg_doc_a)
else()
    list(APPEND LWS_TG_MISSING "CMakePresets.json not found under ${LWS_ROOT}")
endif()
if(EXISTS "${LWS_ROOT}/CMakeUserPresets.json")
    set(_tg_present_b TRUE)
    file(READ "${LWS_ROOT}/CMakeUserPresets.json" _tg_doc_b)
endif()

foreach(_tg_slot IN ITEMS a b)
    if("${_tg_slot}" STREQUAL "a")
        set(_tg_label "CMakePresets.json")
        set(_tg_doc "${_tg_doc_a}")
        set(_tg_on ${_tg_present_a})
    else()
        set(_tg_label "CMakeUserPresets.json")
        set(_tg_doc "${_tg_doc_b}")
        set(_tg_on ${_tg_present_b})
    endif()
    if(NOT _tg_on)
        continue()
    endif()

    string(JSON _tg_count ERROR_VARIABLE _tg_err LENGTH "${_tg_doc}" configurePresets)
    if(_tg_err)
        list(APPEND LWS_TG_REFUSALS
            "[toolchain][presets] ${_tg_label} is not valid JSON or has no configurePresets array: refused")
        continue()
    endif()
    if(_tg_count EQUAL 0)
        continue()
    endif()
    math(EXPR _tg_last "${_tg_count} - 1")
    foreach(_tg_i RANGE ${_tg_last})
        string(JSON _tg_name ERROR_VARIABLE _tg_nerr GET "${_tg_doc}" configurePresets ${_tg_i} name)
        if(_tg_nerr)
            list(APPEND LWS_TG_REFUSALS
                "[toolchain][presets] ${_tg_label} entry ${_tg_i} has no name: refused")
            continue()
        endif()

        set(_tg_hidden FALSE)
        string(JSON _tg_hv ERROR_VARIABLE _tg_herr GET "${_tg_doc}" configurePresets ${_tg_i} hidden)
        if(NOT _tg_herr AND _tg_hv)
            set(_tg_hidden TRUE)
        endif()
        if(_tg_hidden)
            continue()
        endif()

        math(EXPR LWS_TG_PRESETS "${LWS_TG_PRESETS} + 1")
        set(_p "[toolchain][preset ${_tg_name}]")

        lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;CMAKE_C_COMPILER" 0 "${_tg_doc_a}" "${_tg_doc_b}")
        if(NOT _f)
            list(APPEND LWS_TG_REFUSALS "${_p} CMAKE_C_COMPILER is not set (expected ${LWS_TG_EXPECTED_C})")
        elseif(NOT "${_v}" STREQUAL "${LWS_TG_EXPECTED_C}")
            list(APPEND LWS_TG_REFUSALS "${_p} CMAKE_C_COMPILER is '${_v}', not ${LWS_TG_EXPECTED_C}")
        endif()

        lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;CMAKE_CXX_COMPILER" 0 "${_tg_doc_a}" "${_tg_doc_b}")
        if(NOT _f)
            list(APPEND LWS_TG_REFUSALS "${_p} CMAKE_CXX_COMPILER is not set (expected ${LWS_TG_EXPECTED_CXX})")
        elseif(NOT "${_v}" STREQUAL "${LWS_TG_EXPECTED_CXX}")
            list(APPEND LWS_TG_REFUSALS "${_p} CMAKE_CXX_COMPILER is '${_v}', not ${LWS_TG_EXPECTED_CXX}")
        endif()

        # CUDA. A preset that turns GGML_CUDA on must pin the CUDA host, the CUDA compiler and the GPU
        # architecture; a preset that leaves it off keeps the rule that a CUDA host, when set, is clang.
        # A GGML_CUDA that is not found is off. One that is found but holds no readable non-empty string
        # (a JSON boolean, an object without a string value) is treated as on, so the pins are required.
        lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;GGML_CUDA" 0 "${_tg_doc_a}" "${_tg_doc_b}")
        set(_tg_cuda_on FALSE)
        if(_f)
            string(STRIP "${_v}" _tg_cuda_val)
            if("${_tg_cuda_val}" STREQUAL "" OR NOT "${_tg_cuda_val}" MATCHES "^([Oo][Ff][Ff]|[Ff][Aa][Ll][Ss][Ee]|0|[Nn][Oo]|[Nn])$")
                set(_tg_cuda_on TRUE)
            endif()
        endif()

        lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;CMAKE_CUDA_HOST_COMPILER" 0 "${_tg_doc_a}" "${_tg_doc_b}")
        if(_tg_cuda_on)
            if(NOT _f)
                list(APPEND LWS_TG_REFUSALS
                    "${_p} CMAKE_CUDA_HOST_COMPILER is not set (expected ${LWS_TG_EXPECTED_CXX} for a CUDA preset)")
            elseif(NOT "${_v}" STREQUAL "${LWS_TG_EXPECTED_CXX}")
                list(APPEND LWS_TG_REFUSALS
                    "${_p} CMAKE_CUDA_HOST_COMPILER is '${_v}', not ${LWS_TG_EXPECTED_CXX} (CUDA with a non-clang host)")
            endif()

            lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;CMAKE_CUDA_COMPILER" 0 "${_tg_doc_a}" "${_tg_doc_b}")
            get_filename_component(_tg_nvcc_base "${_v}" NAME)
            if(NOT _f OR "${_v}" STREQUAL "")
                list(APPEND LWS_TG_REFUSALS
                    "${_p} CMAKE_CUDA_COMPILER is not set (expected an nvcc for a CUDA preset)")
            elseif(NOT "${_tg_nvcc_base}" STREQUAL "nvcc")
                list(APPEND LWS_TG_REFUSALS "${_p} CMAKE_CUDA_COMPILER is '${_v}', not an nvcc")
            endif()

            lws_tg_resolve_field(_f _v "${_tg_name}" "cacheVariables;CMAKE_CUDA_ARCHITECTURES" 0 "${_tg_doc_a}" "${_tg_doc_b}")
            string(STRIP "${_v}" _tg_arch)
            if(NOT _f OR "${_tg_arch}" STREQUAL "")
                list(APPEND LWS_TG_REFUSALS
                    "${_p} CMAKE_CUDA_ARCHITECTURES is not set (expected explicit architectures for a CUDA preset)")
            elseif("${_tg_arch}" MATCHES "[Nn][Aa][Tt][Ii][Vv][Ee]")
                list(APPEND LWS_TG_REFUSALS
                    "${_p} CMAKE_CUDA_ARCHITECTURES is '${_v}': native is refused, name the architectures")
            endif()
        elseif(_f AND NOT "${_v}" STREQUAL "${LWS_TG_EXPECTED_CXX}")
            list(APPEND LWS_TG_REFUSALS
                "${_p} CMAKE_CUDA_HOST_COMPILER is '${_v}', not ${LWS_TG_EXPECTED_CXX} (CUDA with a non-clang host)")
        endif()

        lws_tg_resolve_field(_f _v "${_tg_name}" "toolchainFile" 0 "${_tg_doc_a}" "${_tg_doc_b}")
        if(NOT _f)
            list(APPEND LWS_TG_REFUSALS "${_p} toolchainFile is not set (expected the Conan toolchain)")
        elseif(NOT "${_v}" STREQUAL "${LWS_TG_EXPECTED_TOOLCHAIN}")
            list(APPEND LWS_TG_REFUSALS
                "${_p} toolchainFile is '${_v}', not the Conan toolchain ${LWS_TG_EXPECTED_TOOLCHAIN}")
        endif()
    endforeach()
endforeach()

if(LWS_TG_PRESETS EQUAL 0 AND _tg_present_a)
    list(APPEND LWS_TG_MISSING "no non-hidden configure preset in CMakePresets.json (vacuous)")
endif()

# ---------------------------------------------------------------------------------------------
# Configured layer: what each leg actually recorded and built.

if(DEFINED LWS_LEGS AND NOT "${LWS_LEGS}" STREQUAL "")
    if(NOT DEFINED LWS_BUILD_ID OR "${LWS_BUILD_ID}" STREQUAL "")
        message(FATAL_ERROR "toolchain-gate: configured mode needs -DLWS_BUILD_ID={id}")
    endif()

    foreach(_tg_leg IN LISTS LWS_LEGS)
        math(EXPR LWS_TG_LEGS "${LWS_TG_LEGS} + 1")
        set(_tg_bdir "${LWS_ROOT}/build/${LWS_BUILD_ID}/${_tg_leg}")
        set(_tg_cdir "${_tg_bdir}/CMakeFiles")
        set(_tg_lp "[toolchain][leg ${_tg_leg}]")

        # A CUDA leg: GGML_CUDA is on in the leg's cache (any cache type: a configure that stopped at the
        # root verdict leaves the preset's value untyped), or a CUDA compiler identity was recorded at
        # all (fail closed: a recorded identity is always checked). The identity goes through the same
        # verdict configure runs, together with the C and C++ identities, with the CUDA legs mandatory.
        set(_tg_cuda_leg FALSE)
        if(EXISTS "${_tg_bdir}/CMakeCache.txt")
            file(STRINGS "${_tg_bdir}/CMakeCache.txt" _tg_cuda_lines REGEX "^GGML_CUDA:[A-Z]*=")
            foreach(_tg_cuda_line IN LISTS _tg_cuda_lines)
                string(REGEX REPLACE "^GGML_CUDA:[A-Z]*=" "" _tg_cuda_val "${_tg_cuda_line}")
                string(STRIP "${_tg_cuda_val}" _tg_cuda_val)
                if(NOT "${_tg_cuda_val}" MATCHES "^([Oo][Ff][Ff]|[Ff][Aa][Ll][Ss][Ee]|0|[Nn][Oo]|[Nn]|)$")
                    set(_tg_cuda_leg TRUE)
                endif()
            endforeach()
        endif()
        # Exactly one CMake version directory: its compiler-ID files are the recorded identity.
        set(_tg_versions "")
        if(IS_DIRECTORY "${_tg_cdir}")
            file(GLOB _tg_entries LIST_DIRECTORIES true "${_tg_cdir}/*")
            foreach(_tg_entry IN LISTS _tg_entries)
                get_filename_component(_tg_base "${_tg_entry}" NAME)
                if(IS_DIRECTORY "${_tg_entry}" AND "${_tg_base}" MATCHES "^[0-9]+\\.[0-9]+")
                    list(APPEND _tg_versions "${_tg_entry}")
                endif()
            endforeach()
        endif()
        list(LENGTH _tg_versions _tg_nv)
        if(NOT _tg_nv EQUAL 1)
            list(APPEND LWS_TG_MISSING
                "leg ${_tg_leg}: expected one CMake version directory under ${_tg_cdir}, found ${_tg_nv}")
        else()
            list(GET _tg_versions 0 _tg_vdir)
            set(_tg_cudac "${_tg_vdir}/CMakeCUDACompiler.cmake")
            if(EXISTS "${_tg_cudac}")
                set(_tg_cuda_leg TRUE)
            endif()
            set(_tg_cuda_args "")
            if(_tg_cuda_leg AND NOT EXISTS "${_tg_cudac}")
                list(APPEND LWS_TG_REFUSALS
                    "${_tg_lp} GGML_CUDA is on but no CUDA compiler identity was recorded")
            elseif(_tg_cuda_leg)
                file(READ "${_tg_cudac}" _tg_cudatext)
                set(_tg_cu_id "")
                set(_tg_cu_ver "")
                set(_tg_cuh_id "")
                set(_tg_cuh_ver "")
                if(_tg_cudatext MATCHES "set\\(CMAKE_CUDA_COMPILER_ID \"([^\"]*)\"\\)")
                    set(_tg_cu_id "${CMAKE_MATCH_1}")
                endif()
                if(_tg_cudatext MATCHES "set\\(CMAKE_CUDA_COMPILER_VERSION \"([^\"]*)\"\\)")
                    set(_tg_cu_ver "${CMAKE_MATCH_1}")
                endif()
                if(_tg_cudatext MATCHES "set\\(CMAKE_CUDA_HOST_COMPILER_ID \"([^\"]*)\"\\)")
                    set(_tg_cuh_id "${CMAKE_MATCH_1}")
                endif()
                if(_tg_cudatext MATCHES "set\\(CMAKE_CUDA_HOST_COMPILER_VERSION \"([^\"]*)\"\\)")
                    set(_tg_cuh_ver "${CMAKE_MATCH_1}")
                endif()
                set(_tg_cuda_args CUDA
                    CUDA_ID "${_tg_cu_id}" CUDA_VERSION "${_tg_cu_ver}"
                    CUDA_HOST_ID "${_tg_cuh_id}" CUDA_HOST_VERSION "${_tg_cuh_ver}"
                    EXPECTED_CUDA_MAJOR "${LWS_CUDA_MAJOR}")
            endif()
            set(_tg_cc "${_tg_vdir}/CMakeCCompiler.cmake")
            set(_tg_cxxc "${_tg_vdir}/CMakeCXXCompiler.cmake")
            if(NOT EXISTS "${_tg_cc}" OR NOT EXISTS "${_tg_cxxc}")
                list(APPEND LWS_TG_MISSING "leg ${_tg_leg}: compiler-ID files missing under ${_tg_vdir}")
            else()
                file(READ "${_tg_cc}" _tg_ctext)
                file(READ "${_tg_cxxc}" _tg_cxxtext)
                # Each capture is read only inside its own MATCHES branch: a failed match must not
                # leave an earlier CMAKE_MATCH_1 behind as the recorded identity.
                set(_tg_c_id "")
                set(_tg_c_ver "")
                set(_tg_cxx_id "")
                set(_tg_cxx_ver "")
                if(_tg_ctext MATCHES "set\\(CMAKE_C_COMPILER_ID \"([^\"]*)\"\\)")
                    set(_tg_c_id "${CMAKE_MATCH_1}")
                endif()
                if(_tg_ctext MATCHES "set\\(CMAKE_C_COMPILER_VERSION \"([^\"]*)\"\\)")
                    set(_tg_c_ver "${CMAKE_MATCH_1}")
                endif()
                if(_tg_cxxtext MATCHES "set\\(CMAKE_CXX_COMPILER_ID \"([^\"]*)\"\\)")
                    set(_tg_cxx_id "${CMAKE_MATCH_1}")
                endif()
                if(_tg_cxxtext MATCHES "set\\(CMAKE_CXX_COMPILER_VERSION \"([^\"]*)\"\\)")
                    set(_tg_cxx_ver "${CMAKE_MATCH_1}")
                endif()
                if("${_tg_c_id}" STREQUAL "" OR "${_tg_cxx_id}" STREQUAL "")
                    list(APPEND LWS_TG_MISSING
                        "leg ${_tg_leg}: CMAKE_C/CXX_COMPILER_ID not recorded under ${_tg_vdir}")
                else()
                    lws_clang21_verdict(_tg_reasons
                        C_ID "${_tg_c_id}" C_VERSION "${_tg_c_ver}"
                        CXX_ID "${_tg_cxx_id}" CXX_VERSION "${_tg_cxx_ver}"
                        ${_tg_cuda_args}
                        EXPECTED_MAJOR "${LWS_LLVM_MAJOR}")
                    foreach(_tg_reason IN LISTS _tg_reasons)
                        list(APPEND LWS_TG_REFUSALS "${_tg_lp} ${_tg_reason}")
                    endforeach()
                endif()
            endif()
        endif()

        # Every compile-database entry must be compiled by clang 21 (a ccache or sccache launcher
        # in front of the compiler is skipped, as ggml enables ccache by default). An nvcc entry is
        # judged by its host option instead.
        set(_tg_db "${_tg_bdir}/compile_commands.json")
        if(NOT EXISTS "${_tg_db}")
            list(APPEND LWS_TG_MISSING "leg ${_tg_leg}: no compile database at ${_tg_db}")
            continue()
        endif()
        file(READ "${_tg_db}" _tg_dbtext)
        string(JSON _tg_dbn ERROR_VARIABLE _tg_dberr LENGTH "${_tg_dbtext}")
        if(_tg_dberr)
            list(APPEND LWS_TG_REFUSALS "${_tg_lp} ${_tg_db} is not valid JSON: refused")
            continue()
        endif()
        if(_tg_dbn EQUAL 0)
            list(APPEND LWS_TG_MISSING "leg ${_tg_leg}: compile database holds no entries (vacuous)")
            continue()
        endif()

        math(EXPR _tg_dblast "${_tg_dbn} - 1")
        foreach(_tg_e RANGE ${_tg_dblast})
            math(EXPR LWS_TG_ENTRIES "${LWS_TG_ENTRIES} + 1")
            string(JSON _tg_file ERROR_VARIABLE _tg_ferr GET "${_tg_dbtext}" ${_tg_e} file)
            if(_tg_ferr)
                set(_tg_file "<entry ${_tg_e}>")
            endif()

            set(_tg_args "")
            string(JSON _tg_at ERROR_VARIABLE _tg_aterr TYPE "${_tg_dbtext}" ${_tg_e} arguments)
            if(NOT _tg_aterr AND "${_tg_at}" STREQUAL "ARRAY")
                string(JSON _tg_an LENGTH "${_tg_dbtext}" ${_tg_e} arguments)
                if(_tg_an GREATER 0)
                    math(EXPR _tg_alast "${_tg_an} - 1")
                    foreach(_tg_k RANGE ${_tg_alast})
                        string(JSON _tg_a GET "${_tg_dbtext}" ${_tg_e} arguments ${_tg_k})
                        list(APPEND _tg_args "${_tg_a}")
                    endforeach()
                endif()
            else()
                string(JSON _tg_cmd ERROR_VARIABLE _tg_cmderr GET "${_tg_dbtext}" ${_tg_e} command)
                if(_tg_cmderr)
                    set(_tg_cmd "")
                endif()
                separate_arguments(_tg_args UNIX_COMMAND "${_tg_cmd}")
            endif()

            set(_tg_comp "")
            foreach(_tg_a IN LISTS _tg_args)
                get_filename_component(_tg_ab "${_tg_a}" NAME)
                if(NOT "${_tg_ab}" MATCHES "^(ccache|sccache)$")
                    set(_tg_comp "${_tg_a}")
                    break()
                endif()
            endforeach()

            get_filename_component(_tg_cbase "${_tg_comp}" NAME)
            if("${_tg_cbase}" STREQUAL "nvcc")
                # An nvcc unit is compiled by the host named in its -ccbin (or --compiler-bindir) option.
                set(_tg_host "")
                set(_tg_host_found FALSE)
                list(LENGTH _tg_args _tg_nargs)
                math(EXPR _tg_nlast "${_tg_nargs} - 1")
                foreach(_tg_k RANGE ${_tg_nlast})
                    list(GET _tg_args ${_tg_k} _tg_a)
                    if("${_tg_a}" MATCHES "^(-ccbin|--compiler-bindir)=(.+)$")
                        set(_tg_host "${CMAKE_MATCH_2}")
                        set(_tg_host_found TRUE)
                        break()
                    elseif("${_tg_a}" STREQUAL "-ccbin" OR "${_tg_a}" STREQUAL "--compiler-bindir")
                        math(EXPR _tg_kn "${_tg_k} + 1")
                        if(_tg_kn LESS _tg_nargs)
                            list(GET _tg_args ${_tg_kn} _tg_host)
                            set(_tg_host_found TRUE)
                        endif()
                        break()
                    endif()
                endforeach()
                if(NOT _tg_host_found)
                    list(APPEND LWS_TG_REFUSALS
                        "${_tg_lp} ${_tg_file}: nvcc entry without a host compiler option")
                else()
                    lws_tg_is_clang21(_tg_host_ok "${_tg_host}")
                    if(NOT _tg_host_ok)
                        list(APPEND LWS_TG_REFUSALS
                            "${_tg_lp} ${_tg_file}: nvcc host '${_tg_host}' is not clang-${LWS_LLVM_MAJOR} or clang++-${LWS_LLVM_MAJOR}")
                    endif()
                endif()
                continue()
            endif()

            set(_tg_ok FALSE)
            if("${_tg_comp}" STREQUAL "")
                set(_tg_ok FALSE)
            elseif("${_tg_cbase}" STREQUAL "${LWS_TG_EXPECTED_C}" OR "${_tg_cbase}" STREQUAL "${LWS_TG_EXPECTED_CXX}")
                set(_tg_ok TRUE)
            else()
                lws_tg_is_clang21(_tg_ok "${_tg_comp}")
            endif()
            if(NOT _tg_ok)
                list(APPEND LWS_TG_REFUSALS
                    "${_tg_lp} ${_tg_file}: compiler '${_tg_comp}' is not clang-${LWS_LLVM_MAJOR} or clang++-${LWS_LLVM_MAJOR}")
            endif()
        endforeach()
    endforeach()
endif()

# ---------------------------------------------------------------------------------------------
# Exit: refusal outranks missing input.

if(LWS_TG_REFUSALS)
    foreach(_tg_line IN LISTS LWS_TG_REFUSALS)
        message(STATUS "${_tg_line}")
    endforeach()
    message(STATUS "TOOLCHAIN-REFUSED presets=${LWS_TG_PRESETS} legs=${LWS_TG_LEGS} entries=${LWS_TG_ENTRIES}")
    cmake_language(EXIT 1)
endif()
if(LWS_TG_MISSING)
    foreach(_tg_line IN LISTS LWS_TG_MISSING)
        message(STATUS "[toolchain] missing input: ${_tg_line}")
    endforeach()
    cmake_language(EXIT 2)
endif()
message(STATUS "TOOLCHAIN-OK presets=${LWS_TG_PRESETS} legs=${LWS_TG_LEGS} entries=${LWS_TG_ENTRIES}")
cmake_language(EXIT 0)
