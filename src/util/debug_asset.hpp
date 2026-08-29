#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

#include <string>

namespace cairns {

static constexpr uint32_t kDebugGlbsToParseStart = 3;
static constexpr uint32_t kDebugGlbsToParse = 9;

static constexpr std::array kDebugGlbs = {
    "die.glb",
    "viking_room.glb",
    "bistro.glb",
    "aatrox.glb",
    "aatrox_blood_moon.glb",
    "aatrox_drx.glb",
    "aatrox_justicar.glb",
    "aatrox_lunar_eclipse.glb",
    "aatrox_mecha.glb",
    "aatrox_odyessy.glb",
    "aatrox_prestige_blood_moon.glb",
    "aatrox_prestige_blood_moon2.glb",
    "aatrox_prestige_drx.glb",
    "aatrox_primordian.glb",
    "aatrox_sea_hunter.glb",
    "aatrox_victorious.glb",
    "ahri.glb",
    "ahri_(2022)_prestige_k_da.glb",
    "ahri_academy.glb",
    "ahri_after_hours_spirit_blossom_springs.glb",
    "ahri_arcade.glb",
    "ahri_arcana.glb",
    "ahri_challenger.glb",
    "ahri_coven.glb",
    "ahri_dynasty.glb",
    "ahri_elderwood.glb",
    "ahri_foxfire.glb",
    "ahri_immortalized_legend.glb",
    "ahri_k_da.glb",
    "ahri_k_da_all_out.glb",
    "ahri_midnight.glb",
    "ahri_popstar.glb",
    "ahri_prestige_k_da.glb",
    "ahri_risen_legend.glb",
    "ahri_snow_moon.glb",
    "ahri_spirit_blossom.glb",
    "ahri_spirit_blossom_springs.glb",
    "ahri_star_guardian.glb",
    "akali.glb",
    "akali_(2022)_prestige_k_da.glb",
    "akali_all-star.glb",
    "akali_blood_moon.glb",
    "akali_coven.glb",
    "akali_crime_city_nightmare.glb",
    "akali_drx.glb",
    "akali_empyrean.glb",
    "akali_headhunter.glb",
    "akali_infernal.glb",
    "akali_k_da.glb",
    "akali_k_da_all_out.glb",
    "akali_nurse.glb",
    "akali_prestige_coven.glb",
    "akali_prestige_k_da.glb",
    "akali_project_.glb",
    "akali_sashimi.glb",
    "akali_silverfang.glb",
    "akali_spirit_blossom.glb",
    "akali_star_guardian.glb",
    "akali_stinger.glb",
    "akali_true_damage.glb",
    "akshan.glb",
    "akshan_crystal_rose.glb",
    "akshan_cyber_pop.glb",
    "akshan_three_honors.glb",
    "alistar.glb",
    "alistar_black.glb",
    "alistar_blackfrost.glb",
    "alistar_conqueror.glb",
    "alistar_elderwood.glb",
    "alistar_golden.glb",
    "alistar_hextech.glb",
    "alistar_infernal.glb",
    "alistar_longhorn.glb",
    "alistar_lunar_beast.glb",
    "alistar_marauder.glb",
    "alistar_matador.glb",
    "alistar_moo_cow.glb",
    "alistar_skt_t1.glb",
    "alistar_sweeper.glb",
    "alistar_unchained.glb",
    "ambessa.glb",
    "ambessa_chosen_of_the_wolf.glb",
    "amumu.glb",
    "amumu_almost-prom_king.glb",
    "amumu_dumpling_darlings.glb",
    "amumu_emumu.glb",
    "amumu_heartache.glb",
    "amumu_hextech.glb",
    "amumu_infernal.glb",
    "amumu_little_knight.glb",
    "amumu_pharaoh.glb",
    "amumu_pumpkin_prince.glb",
    "amumu_re-gifted.glb",
    "amumu_sad_robot.glb",
    "amumu_surprise_party.glb",
    "amumu_vancouver.glb",
    "anivia_bird_of_prey.glb",
    "anivia_blackfrost.glb",
    "anivia_cosmic_flight.glb",
    "anivia_divine_phoenix.glb",
    "anivia_festival_queen.glb",
    "anivia_hextech.glb",
    "anivia_noxus_hunter.glb",
};

// for 9 debug assets:
// x0 = -1, dx = 1
// y0 = -1, dy = 1
// z0 = -3, no dz
// scale = 0.005
// for a thousand assets:
// x0 = -1.25, dx = 0.25
// y0 = 2.5, dy = 0.5
// x0 = -3, dz = -0.25
// scale = 0.001
//[[maybe_unused]] static const std::array<glm::mat4, kDebugGlbs.size()> kDebugSceneTransforms = {
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-1, 1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(1, 1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-1, 0, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(1, 0, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(-1, -1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0, -1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//    glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(1, -1, -3)), glm::vec3(0.005, 0.005, 0.005)),
//};

inline std::vector<glm::mat4> GenerateDebugGridTransforms(
        glm::vec3 startPos,
        int N,
        float dx, float dy, float dz,
        float scale,
        int totalCount
    ) {
        std::vector<glm::mat4> transforms;
        transforms.reserve(totalCount);

        for (int i = 0; i < totalCount; ++i) {
            // 1. Calculate the layer (z), the row (y), and the column (x)
            // A "slice" of the grid is N * N elements.
            int layer = i / (N * N);
            int remaining = i % (N * N);
            int row = remaining / N;
            int col = remaining % N;

            // 2. Calculate displacement
            glm::vec3 translation = startPos + glm::vec3(
                (float)col * dx,
                (float)row * dy,
                -(float)layer * dz
            );

            // 3. Build the Matrix (Translate * Scale)
            glm::mat4 model = glm::translate(glm::mat4(1.0f), translation);
            model = glm::scale(model, glm::vec3(scale));

            transforms.push_back(model);
        }

        return transforms;
    }

} // namespace cairns
