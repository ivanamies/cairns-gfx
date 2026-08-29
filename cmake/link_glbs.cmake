# Build-time: symlink every champion GLB in the staging roster into the web
# output dir (next to cairns_web.html), so the dev http server serves them as
# loose files for lazy fetch-on-demand (platform_web.cpp ReadAsset). Symlinks =
# no disk duplication; the GLBs are NOT bundled into the .data. Args: -DSTAGING
# (the champion staging dir), -DOUT (the cairns_web output dir).
file(GLOB _glbs "${STAGING}/*.glb")
foreach(_g ${_glbs})
    get_filename_component(_n "${_g}" NAME)
    if(NOT EXISTS "${OUT}/${_n}")
        file(CREATE_LINK "${_g}" "${OUT}/${_n}" SYMBOLIC)
    endif()
endforeach()
list(LENGTH _glbs _count)
message(STATUS "linked ${_count} loose champion GLBs into ${OUT}")
