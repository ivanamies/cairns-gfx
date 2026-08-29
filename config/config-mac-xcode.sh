#!/bin/bash
cd "${0%/*}"

cmake -G "Xcode" -B ../build/metal -DCAIRNS_GFX_BACKEND=metal -S ..