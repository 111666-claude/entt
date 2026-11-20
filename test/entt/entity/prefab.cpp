#include <array>
#include <type_traits>
#include <vector>
#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include "../../common/throwing_type.hpp"
#include "../../common/value_type.h"

namespace {

struct turret_body {
    int hp = 100;
};

struct gun_barrel {
    entt::entity mounted_on{entt::null};
    int caliber = 10;
};

struct ammo_slot {
    entt::entity feeds{entt::null};
    std::vector<entt::entity> spare_targets{};
    int rounds = 42;
};

struct position {
    int x = 0;
    int y = 0;
};

struct cyclic_node {
    entt::entity next{entt::null};
};

using test::throwing_type;
using test::throwing_type_exception;

struct BasicPrefab: testing::Test {
    entt::registry registry;

    typename entt::registry::size_type live_entities() const {
        // the in-use prefix of the entity storage holds the live identifiers
        return registry.storage<entt::entity>()->free_list();
    }

    struct turret {
        entt::entity body{entt::null};
        entt::entity barrel{entt::null};
        entt::entity ammo{entt::null};
    };

    turret make_turret(const int hp, const int caliber, const int rounds) {
        const auto body = registry.create();
        const auto barrel = registry.create();
        const auto ammo = registry.create();

        registry.emplace<turret_body>(body, hp);
        registry.emplace<gun_barrel>(barrel, body, caliber);
        registry.emplace<ammo_slot>(ammo, barrel, std::vector<entt::entity>{}, rounds);

        return {body, barrel, ammo};
    }

    entt::prefab configured_prefab() {
        entt::prefab pf{registry};
        pf.optional_component<turret_body>()
            .optional_component<gun_barrel>()
            .optional_component<ammo_slot>()
            .link<gun_barrel>(&gun_barrel::mounted_on)
            .link<ammo_slot>(&ammo_slot::feeds)
            .link<ammo_slot>(&ammo_slot::spare_targets);
        return pf;
    }
};

TEST_F(BasicPrefab, Traits) {
    static_assert(!std::is_default_constructible_v<entt::prefab>, "Default constructible type not allowed");
    static_assert(!std::is_copy_constructible_v<entt::prefab>, "Copy constructible type not allowed");
    static_assert(!std::is_copy_assignable_v<entt::prefab>, "Copy assignable type not allowed");
    static_assert(std::is_move_constructible_v<entt::prefab>, "Move constructible type required");
    static_assert(std::is_move_assignable_v<entt::prefab>, "Move assignable type required");

    static_assert(std::is_same_v<typename entt::prefab::result_type, entt::prefab_result_t<entt::entity>>);
    static_assert(std::is_same_v<typename entt::prefab::entity_type, entt::entity>);
}

TEST_F(BasicPrefab, SelectionDeduplicatesAndKeepsInvalidEntities) {
    const auto valid = registry.create();
    const auto other = registry.create();
    registry.destroy(other);

    entt::prefab pf{registry};
    pf.entity(valid).entity(valid).entity(other);
    const std::array range{valid, valid};
    pf.entity(range.begin(), range.end());

    ASSERT_EQ(pf.size(), 2u);
    ASSERT_TRUE(pf.contains(valid));
    ASSERT_TRUE(pf.contains(other));
}

TEST_F(BasicPrefab, CloneTurretSubgraphAndRemapInternalReferences) {
    const auto tower = make_turret(42, 7, 9);

    auto pf = configured_prefab();
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::success);
    ASSERT_EQ(result.entities.size(), 3u);

    const auto body = result.entities.map(tower.body);
    const auto barrel = result.entities.map(tower.barrel);
    const auto ammo = result.entities.map(tower.ammo);

    ASSERT_NE(body, tower.body);
    ASSERT_NE(barrel, tower.barrel);
    ASSERT_NE(ammo, tower.ammo);
    ASSERT_TRUE(registry.valid(body) && registry.valid(barrel) && registry.valid(ammo));

    ASSERT_EQ(registry.get<turret_body>(body).hp, 42);
    ASSERT_EQ(registry.get<gun_barrel>(barrel).caliber, 7);
    ASSERT_EQ(registry.get<ammo_slot>(ammo).rounds, 9);

    // internal references point at the cloned entities, not the source ones
    ASSERT_EQ(registry.get<gun_barrel>(barrel).mounted_on, body);
    ASSERT_EQ(registry.get<ammo_slot>(ammo).feeds, barrel);

    // unknown entities map to the null entity
    ASSERT_EQ(result.entities.map(registry.create()), entt::null);
}

TEST_F(BasicPrefab, TwoTurretSubgraphsStayIndependent) {
    const auto alpha = make_turret(10, 20, 30);
    const auto beta = make_turret(11, 21, 31);

    auto pf = configured_prefab();
    pf.entity(alpha.body).entity(alpha.barrel).entity(alpha.ammo);
    pf.entity(beta.body).entity(beta.barrel).entity(beta.ammo);
    // an override targets a single source entity and reaches only its clone
    pf.override<turret_body>(alpha.body, turret_body{99});

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(result.entities.size(), 6u);

    const auto alpha_body = result.entities.map(alpha.body);
    const auto alpha_barrel = result.entities.map(alpha.barrel);
    const auto alpha_ammo = result.entities.map(alpha.ammo);
    const auto beta_body = result.entities.map(beta.body);
    const auto beta_barrel = result.entities.map(beta.barrel);
    const auto beta_ammo = result.entities.map(beta.ammo);

    // cross-checked relationships on both instances
    ASSERT_EQ(registry.get<gun_barrel>(alpha_barrel).mounted_on, alpha_body);
    ASSERT_EQ(registry.get<ammo_slot>(alpha_ammo).feeds, alpha_barrel);
    ASSERT_EQ(registry.get<gun_barrel>(beta_barrel).mounted_on, beta_body);
    ASSERT_EQ(registry.get<ammo_slot>(beta_ammo).feeds, beta_barrel);

    // the override lands on the alpha clone only, sources are untouched
    ASSERT_EQ(registry.get<turret_body>(alpha_body).hp, 99);
    ASSERT_EQ(registry.get<turret_body>(beta_body).hp, 11);
    ASSERT_EQ(registry.get<turret_body>(alpha.body).hp, 10);

    // destroying one instance leaves the other and the sources intact
    registry.destroy(alpha_ammo);
    registry.destroy(alpha_barrel);
    registry.destroy(alpha_body);

    ASSERT_FALSE(registry.valid(alpha_body));
    ASSERT_TRUE(registry.valid(beta_body) && registry.valid(beta_barrel) && registry.valid(beta_ammo));
    ASSERT_TRUE(registry.valid(alpha.body) && registry.valid(alpha.barrel) && registry.valid(alpha.ammo));
    ASSERT_EQ(registry.get<ammo_slot>(beta_ammo).feeds, beta_barrel);
}

TEST_F(BasicPrefab, OverridesReplaceAndPatchClonedComponents) {
    const auto tower = make_turret(42, 7, 9);

    auto pf = configured_prefab();
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.override<turret_body>(tower.body, turret_body{77});
    pf.override<gun_barrel>(tower.barrel, [](gun_barrel &barrel) { barrel.caliber = 33; });
    // overriding twice on the same source keeps the last value
    pf.override<turret_body>(tower.body, turret_body{88});

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));

    const auto body = result.entities.map(tower.body);
    const auto barrel = result.entities.map(tower.barrel);

    ASSERT_EQ(registry.get<turret_body>(body).hp, 88);
    ASSERT_EQ(registry.get<gun_barrel>(barrel).caliber, 33);
    // the source is untouched
    ASSERT_EQ(registry.get<turret_body>(tower.body).hp, 42);
}

TEST_F(BasicPrefab, ExternalReferencePreservedByDefault) {
    const auto tower = make_turret(42, 7, 9);
    const auto external = registry.create();
    registry.get<ammo_slot>(tower.ammo).spare_targets.push_back(external);

    auto pf = configured_prefab();
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    ASSERT_EQ(pf.on_external_reference(), entt::external_ref_policy::preserve);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));

    const auto ammo = result.entities.map(tower.ammo);
    const auto &targets = registry.get<ammo_slot>(ammo).spare_targets;
    ASSERT_EQ(targets.size(), 1u);
    ASSERT_EQ(targets[0], external);
}

TEST_F(BasicPrefab, ExternalReferenceRejectedRollsBack) {
    const auto tower = make_turret(42, 7, 9);
    const auto external = registry.create();
    registry.get<ammo_slot>(tower.ammo).spare_targets.push_back(external);

    const auto entities_before = live_entities();

    auto pf = configured_prefab();
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.on_external_reference(entt::external_ref_policy::reject);

    auto result = pf.instantiate();
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::external_reference);
    ASSERT_EQ(result.reason, external);
    ASSERT_EQ(result.entities.size(), 0u);

    // no half-instance is left behind
    ASSERT_EQ(live_entities(), entities_before);
}

TEST_F(BasicPrefab, NullReferencesAreNotExternal) {
    const auto tower = make_turret(42, 7, 9);
    registry.get<gun_barrel>(tower.barrel).mounted_on = entt::null;

    auto pf = configured_prefab();
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.on_external_reference(entt::external_ref_policy::reject);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(registry.get<gun_barrel>(result.entities.map(tower.barrel)).mounted_on, entt::null);
}

TEST_F(BasicPrefab, CyclicReferencesAreRemapped) {
    const auto first = registry.create();
    const auto second = registry.create();
    registry.emplace<cyclic_node>(first, second);
    registry.emplace<cyclic_node>(second, first);

    entt::prefab pf{registry};
    pf.entity(first).entity(second).components<cyclic_node>().link<cyclic_node>(&cyclic_node::next);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));

    const auto clone_first = result.entities.map(first);
    const auto clone_second = result.entities.map(second);

    ASSERT_EQ(registry.get<cyclic_node>(clone_first).next, clone_second);
    ASSERT_EQ(registry.get<cyclic_node>(clone_second).next, clone_first);
}

TEST_F(BasicPrefab, MissingRequiredComponentFailsBeforeCreatingEntities) {
    const auto tower = make_turret(42, 7, 9);
    const auto entities_before = live_entities();

    entt::prefab pf{registry};
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.components<turret_body, gun_barrel, ammo_slot>();

    auto result = pf.instantiate();
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::missing_component);
    // the body is visited first and it doesn't own the barrel component
    ASSERT_EQ(result.reason, tower.body);
    ASSERT_EQ(live_entities(), entities_before);
}

TEST_F(BasicPrefab, OptionalComponentsAreSkippedWhereMissing) {
    const auto tower = make_turret(42, 7, 9);
    registry.emplace<position>(tower.body, 1, 2);

    entt::prefab pf{registry};
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.optional_component<turret_body>()
        .optional_component<gun_barrel>()
        .optional_component<ammo_slot>()
        .optional_component<position>()
        .link<gun_barrel>(&gun_barrel::mounted_on)
        .link<ammo_slot>(&ammo_slot::feeds);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));

    ASSERT_TRUE(registry.all_of<position>(result.entities.map(tower.body)));
    ASSERT_FALSE(registry.all_of<position>(result.entities.map(tower.barrel)));
    ASSERT_EQ(registry.get<position>(result.entities.map(tower.body)).x, 1);
}

TEST_F(BasicPrefab, DestroyedEntityInSelectionFailsCleanly) {
    const auto alive = registry.create();
    const auto dead = registry.create();
    registry.emplace<turret_body>(alive, 1);
    registry.emplace<turret_body>(dead, 2);
    registry.destroy(dead);

    const auto entities_before = live_entities();

    entt::prefab pf{registry};
    pf.entity(alive).entity(dead);
    pf.optional_component<turret_body>();

    auto result = pf.instantiate();
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::invalid_entity);
    ASSERT_EQ(result.reason, dead);
    ASSERT_EQ(live_entities(), entities_before);
}

TEST_F(BasicPrefab, OverrideOnEntityOutsideSubgraphFailsCleanly) {
    const auto inside = registry.create();
    const auto outside = registry.create();
    registry.emplace<turret_body>(inside, 1);
    registry.emplace<turret_body>(outside, 2);

    const auto entities_before = live_entities();

    entt::prefab pf{registry};
    pf.entity(inside);
    pf.optional_component<turret_body>();
    pf.override<turret_body>(outside, turret_body{3});

    auto result = pf.instantiate();
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::external_entity);
    ASSERT_EQ(result.reason, outside);
    ASSERT_EQ(live_entities(), entities_before);
}

TEST_F(BasicPrefab, ExceptionDuringCopyRollsBackHalfInstance) {
    const auto first = registry.create();
    const auto second = registry.create();
    registry.emplace<turret_body>(first, 1);
    registry.emplace<throwing_type>(second, false);

    const auto entities_before = live_entities();

    entt::prefab pf{registry};
    pf.entity(first).entity(second);
    pf.optional_component<turret_body>().optional_component<throwing_type>();

    // make the copy of the second component throw, after the first clone exists
    registry.get<throwing_type>(second).throw_on_copy(true);

    auto result = pf.instantiate();
    ASSERT_FALSE(static_cast<bool>(result));
    ASSERT_EQ(result.error, entt::prefab_error::aborted);
    ASSERT_EQ(result.entities.size(), 0u);
    ASSERT_EQ(live_entities(), entities_before);
}

TEST_F(BasicPrefab, CustomLinkResolvesReferencesThroughTheMapper) {
    const auto tower = make_turret(42, 7, 9);

    entt::prefab pf{registry};
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.optional_component<ammo_slot>();
    pf.link<ammo_slot>([](ammo_slot &slot, const entt::prefab::entity_mapper &mapper, const entt::external_ref_policy, bool &, entt::entity &) {
        slot.feeds = mapper.map(slot.feeds);
    });

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(registry.get<ammo_slot>(result.entities.map(tower.ammo)).feeds, result.entities.map(tower.barrel));
}

TEST_F(BasicPrefab, EmptyComponentsAreDuplicated) {
    static_assert(entt::component_traits<test::empty>::page_size == 0u, "Empty type required");

    const auto tower = make_turret(42, 7, 9);
    registry.emplace<test::empty>(tower.body);

    entt::prefab pf{registry};
    pf.entity(tower.body).entity(tower.barrel).entity(tower.ammo);
    pf.optional_component<test::empty>()
        .optional_component<turret_body>()
        .optional_component<gun_barrel>()
        .optional_component<ammo_slot>();

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));

    ASSERT_TRUE(registry.all_of<test::empty>(result.entities.map(tower.body)));
    ASSERT_FALSE(registry.all_of<test::empty>(result.entities.map(tower.barrel)));
    ASSERT_EQ(registry.get<turret_body>(result.entities.map(tower.body)).hp, 42);
}

TEST_F(BasicPrefab, ClearResetsTheConfiguration) {
    const auto tower = make_turret(42, 7, 9);

    entt::prefab pf{registry};
    pf.entity(tower.body).optional_component<turret_body>();
    ASSERT_EQ(pf.size(), 1u);

    pf.clear();
    ASSERT_EQ(pf.size(), 0u);

    auto result = pf.instantiate();
    ASSERT_TRUE(static_cast<bool>(result));
    ASSERT_EQ(result.entities.size(), 0u);
}

} // namespace
