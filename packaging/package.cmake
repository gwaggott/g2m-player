# cmake -DBIN=<build>/bin -DOUT_DIR=<dir> -DNAME=<zip name> -DSOURCE=<player dir>
#       [-DMODELS=<bundled models>] [-DSHAREABLE=ON] -P package.cmake
#
# Puts the built plugins into a folder per format, with INSTALL.md,
# THIRD_PARTY.md and the player's LICENSE, and zips it as OUT_DIR/NAME.zip. Run by the g2m-package
# target (plugin/CMakeLists.txt) and by CI.
#
# A build that bundles models (G2M_BUNDLED_MODELS) is refused unless
# SHAREABLE is on: local builds bundle private models that must not be
# shared (docs/release-plan.md).

foreach(var BIN OUT_DIR NAME SOURCE)
  if(NOT ${var})
    message(FATAL_ERROR "package.cmake: -D${var}= is required")
  endif()
endforeach()
if(MODELS AND NOT SHAREABLE)
  message(FATAL_ERROR "This build bundles models (${MODELS}). Package it only if every one of them may be "
                      "shared: configure with -DG2M_PACKAGE_SHAREABLE=ON.")
endif()

set(stage "${OUT_DIR}/${NAME}")
file(REMOVE_RECURSE "${stage}" "${OUT_DIR}/${NAME}.zip")
file(MAKE_DIRECTORY "${stage}")

# Copies BIN/<item> into stage/<folder> if it was built; the number copied in `count`
set(count 0)
macro(take folder item)
  if(EXISTS "${BIN}/${item}")
    file(MAKE_DIRECTORY "${stage}/${folder}")
    file(COPY "${BIN}/${item}" DESTINATION "${stage}/${folder}" USE_SOURCE_PERMISSIONS)
    math(EXPR count "${count} + 1")
  endif()
endmacro()

take(LV2 g2m.lv2)
take(VST3 g2m.vst3)
take(CLAP g2m.clap)
take(CLAP g2m-models)  # the CLAP plugin's bundled models, beside it
foreach(standalone g2m.app g2m.exe g2m)
  if(EXISTS "${BIN}/${standalone}")
    take(Standalone ${standalone})
    take(Standalone resources)  # the standalone's bundled models
    break()
  endif()
endforeach()
if(count EQUAL 0)
  message(FATAL_ERROR "Nothing built in ${BIN}")
endif()

file(COPY "${SOURCE}/packaging/INSTALL.md" "${SOURCE}/THIRD_PARTY.md" DESTINATION "${stage}")
configure_file("${SOURCE}/LICENSE" "${stage}/LICENSE.txt" COPYONLY)  # the player's own licence, GPL-3.0
if(EXISTS "${SOURCE}/MODELS.md")
  file(COPY "${SOURCE}/MODELS.md" DESTINATION "${stage}")
endif()

# The licence texts THIRD_PARTY.md names
set(dpf "${SOURCE}/external/dpf")
foreach(pair
    "${dpf}/LICENSE|DPF.txt"
    "${dpf}/dgl/src/pugl-upstream/COPYING|pugl.txt"
    "${dpf}/dgl/src/nanovg/LICENSE.txt|NanoVG.txt"
    "${dpf}/dgl/src/resources/LICENSE-DejaVuSans.ttf.txt|DejaVuSans.txt"
    "${dpf}/distrho/extra/choc/LICENSE.md|choc.md"
    "${SOURCE}/external/lvgl/LICENCE.txt|LVGL.txt")
  string(REPLACE "|" ";" pair "${pair}")
  list(GET pair 0 from)
  list(GET pair 1 to)
  if(NOT EXISTS "${from}")
    message(FATAL_ERROR "${from} not found: are the submodules checked out?")
  endif()
  configure_file("${from}" "${stage}/licences/${to}" COPYONLY)
endforeach()

execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${NAME}.zip" --format=zip "${NAME}"
                WORKING_DIRECTORY "${OUT_DIR}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "zip failed")
endif()
message(STATUS "Packaged ${OUT_DIR}/${NAME}.zip")
