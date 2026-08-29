# Build-time: stamp the service worker's cache version with the content hash of
# the freshly-built cairns_web.data, so a rebuilt bundle auto-invalidates the
# Cache API entry (no manual version bump). Run via `cmake -P` as a POST_BUILD
# step -- file(MD5) reads the .data as it exists at build time. Args: -DSRC (the
# sw.js.in template), -DOUT (the sw.js to write next to cairns_web.html), -DDATA
# (the built cairns_web.data).
file(MD5 "${DATA}" _data_hash)
string(SUBSTRING "${_data_hash}" 0 12 _short_hash)
file(READ "${SRC}" _template)
string(REPLACE "@DATA_HASH@" "${_short_hash}" _stamped "${_template}")
file(WRITE "${OUT}" "${_stamped}")
message(STATUS "stamped sw.js cache version cairns-${_short_hash}")
