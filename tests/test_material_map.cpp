// tests/test_material_map.cpp
//
// SPEC: the material->texture index mapping (src/util/material_map.hpp). The
// invariant under test is the #229 loader fix: outputs stay 1:1 with glTF
// material indices even when a material has no resolvable baseColorTexture. The
// old code SKIPPED such a material, compacting the list and shifting every
// later slot -> off-by-N texture binds. No roster GLB triggers it today, hence
// this synthetic coverage.
// TAGS: [spec][loader]

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "util/material_map.hpp"

using namespace cairns;

SCENARIO("MapMaterialTextures keeps outputs 1:1 with material indices",
         "[spec][loader]") {
    GIVEN("an untextured material BEFORE a textured one (the desync trigger)") {
        std::vector<MaterialTexRef> mats(2);
        mats[0].has_base_color = false;        // material 0: no baseColorTexture
        mats[1].has_base_color = true;         // material 1: -> texture 0
        mats[1].texture_index = 0;
        std::vector<TextureSlots> texs(1);
        texs[0].image = 7;
        texs[0].sampler = 3;

        std::vector<uint32_t> img;
        std::vector<uint32_t> smp;
        MapMaterialTextures(mats, texs, img, smp);

        THEN("both materials get a slot; the textured one is NOT shifted") {
            REQUIRE(img.size() == 2u);               // 1:1, not compacted to 1
            REQUIRE(smp.size() == 2u);
            REQUIRE(img[0] == kNoMaterialTexture);   // untextured -> sentinel
            REQUIRE(smp[0] == kNoMaterialTexture);
            REQUIRE(img[1] == 7u);                   // material 1 still binds tex 0
            REQUIRE(smp[1] == 3u);
        }
    }

    GIVEN("materials referencing out-of-range or image-less textures") {
        std::vector<MaterialTexRef> mats(3);
        mats[0].has_base_color = true;
        mats[0].texture_index = 5;   // out of range
        mats[1].has_base_color = true;
        mats[1].texture_index = 0;   // valid index, but the texture has no image
        mats[2].has_base_color = true;
        mats[2].texture_index = 1;   // fully resolved
        std::vector<TextureSlots> texs(2);
        // texs[0] stays default (image == sampler == kNoMaterialTexture).
        texs[1].image = 2;
        texs[1].sampler = 1;

        std::vector<uint32_t> img;
        std::vector<uint32_t> smp;
        MapMaterialTextures(mats, texs, img, smp);

        THEN("unresolvable -> sentinel, resolvable -> its index, order kept") {
            REQUIRE(img.size() == 3u);
            REQUIRE(img[0] == kNoMaterialTexture);   // out of range
            REQUIRE(img[1] == kNoMaterialTexture);   // texture carried no image
            REQUIRE(img[2] == 2u);
            REQUIRE(smp[2] == 1u);
        }
    }

    GIVEN("no materials") {
        std::vector<MaterialTexRef> mats;
        std::vector<TextureSlots> texs;
        std::vector<uint32_t> img;
        std::vector<uint32_t> smp;
        MapMaterialTextures(mats, texs, img, smp);
        THEN("outputs are empty") {
            REQUIRE(img.empty());
            REQUIRE(smp.empty());
        }
    }
}
