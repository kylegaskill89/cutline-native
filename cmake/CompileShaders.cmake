# Locates DXC and compiles HLSL to DXIL at build time.
#
# The Windows SDK ships dxc under a versioned bin directory that is not on the
# PATH, so it is found by globbing the SDK and taking the newest.

function(cutline_find_dxc OUT_VAR)
  if(DEFINED CACHE{CUTLINE_DXC})
    set(${OUT_VAR} "${CUTLINE_DXC}" PARENT_SCOPE)
    return()
  endif()

  set(_candidates "")
  foreach(_root "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin" "$ENV{ProgramFiles}/Windows Kits/10/bin")
    file(GLOB _versions "${_root}/*")
    foreach(_version ${_versions})
      if(EXISTS "${_version}/x64/dxc.exe")
        list(APPEND _candidates "${_version}/x64/dxc.exe")
      endif()
    endforeach()
  endforeach()

  if(NOT _candidates)
    message(FATAL_ERROR "dxc.exe not found; install the Windows SDK's shader compiler")
  endif()

  list(SORT _candidates)
  list(REVERSE _candidates)  # newest SDK first
  list(GET _candidates 0 _dxc)

  set(CUTLINE_DXC "${_dxc}" CACHE FILEPATH "Path to the DirectX shader compiler")
  set(${OUT_VAR} "${_dxc}" PARENT_SCOPE)
endfunction()

# Compiles one entry point of an HLSL file, placing the result next to the
# target's executable so it can be found at runtime without a search path.
function(cutline_compile_shader TARGET SOURCE ENTRY PROFILE OUTPUT_NAME)
  cutline_find_dxc(_dxc)

  set(_output "$<TARGET_FILE_DIR:${TARGET}>/${OUTPUT_NAME}")

  # A single list-valued generator expression, expanded by COMMAND_EXPAND_LISTS.
  # Two separate expressions would each survive as an empty quoted argument in
  # non-Debug builds, which dxc reads as an empty input filename.
  add_custom_command(
    TARGET ${TARGET} POST_BUILD
    COMMAND "${_dxc}"
            -T ${PROFILE}
            -E ${ENTRY}
            "$<$<CONFIG:Debug>:-Zi;-Qembed_debug>"
            -Fo "${_output}"
            "${SOURCE}"
    MAIN_DEPENDENCY "${SOURCE}"
    COMMENT "Compiling ${ENTRY} -> ${OUTPUT_NAME}"
    COMMAND_EXPAND_LISTS
    VERBATIM)
endfunction()
