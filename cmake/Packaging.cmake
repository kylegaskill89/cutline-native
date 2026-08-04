# Turning the build into something somebody else can install.
#
# Only meaningful under the `ui` preset, which is the one that produces the
# actual application. Under any other preset there is no `cutline` target and
# this does nothing at all rather than producing an installer for a subset of
# the editor, which would be worse than producing none.

if(NOT TARGET cutline)
  return()
endif()

# The application is a directory, not a file: the executable, the FFmpeg and
# Skia runtimes beside it, and the compiled shaders the compositor opens by
# name at startup. Missing any one of them is a build that starts and then
# cannot do the thing it exists for -- which is exactly how the shaders were
# discovered missing from the window in the first place.
install(TARGETS cutline RUNTIME DESTINATION .)

# Globbed at install time rather than listed. The runtime DLLs are put beside
# the executable by vcpkg's own deployment during the build, and which ones
# there are depends on how FFmpeg was configured -- so a hand-written list is a
# list that goes stale silently, and the failure it produces is a missing codec
# at export time rather than a build error.
install(CODE "
  file(GLOB _runtime
    \"$<TARGET_FILE_DIR:cutline>/*.dll\"
    \"$<TARGET_FILE_DIR:cutline>/*.cso\")
  if(NOT _runtime)
    message(FATAL_ERROR
      \"No runtime files beside cutline.exe. The package would start and then \"
      \"fail to open a shader or a codec, which is worse than not packaging.\")
  endif()
  file(INSTALL DESTINATION \"\${CMAKE_INSTALL_PREFIX}\" TYPE FILE FILES \${_runtime})
")

install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" DESTINATION .)

# ------------------------------------------------------------------- CPack --

set(CPACK_GENERATOR NSIS)
set(CPACK_PACKAGE_NAME "Cutline")
set(CPACK_PACKAGE_VENDOR "Kyle Gaskill")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")

# The name the updater's manifest points at, and the name the release workflow
# uploads. Written here so there is one place it is decided.
set(CPACK_PACKAGE_FILE_NAME "Cutline-${PROJECT_VERSION}-Setup")

set(CPACK_PACKAGE_INSTALL_DIRECTORY "Cutline")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

set(CPACK_NSIS_PACKAGE_NAME "Cutline")
set(CPACK_NSIS_DISPLAY_NAME "Cutline")
set(CPACK_NSIS_INSTALLED_ICON_NAME "cutline.exe")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/kylegaskill89/cutline-native")
set(CPACK_NSIS_HELP_LINK "https://github.com/kylegaskill89/cutline-native")

# Updating is installing over the top, so the old files go first. Without this
# a version with one fewer DLL than the last leaves the old one behind, and the
# loader is perfectly happy to pick it up.
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

# The installer's own icon, and the uninstaller's. `CPACK_NSIS_INSTALLED_ICON_NAME`
# above only decides what Add/Remove Programs shows; without these the setup
# executable somebody downloads from the release page is the generic NSIS one,
# which is the first thing they see of this and says nothing about it.
#
# NSIS wants a native path, so the slashes are turned round here rather than
# left for it to misread.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/tools/cutline/cutline.ico" CUTLINE_NSIS_ICON)
string(REPLACE "\\" "\\\\" CUTLINE_NSIS_ICON "${CUTLINE_NSIS_ICON}")
set(CPACK_NSIS_MUI_ICON "${CUTLINE_NSIS_ICON}")
set(CPACK_NSIS_MUI_UNIICON "${CUTLINE_NSIS_ICON}")

# So the editor comes back after an update. The whole flow is: the editor
# downloads, verifies, starts this and closes itself -- and an update that left
# the user staring at a desktop would be one they had to think about.
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "cutline.exe")

set(CPACK_NSIS_CREATE_ICONS_EXTRA
  "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Cutline.lnk' '$INSTDIR\\\\cutline.exe'")
set(CPACK_NSIS_DELETE_ICONS_EXTRA
  "Delete '$SMPROGRAMS\\\\$START_MENU\\\\Cutline.lnk'")

include(CPack)
