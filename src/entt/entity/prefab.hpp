#ifndef ENTT_ENTITY_PREFAB_HPP
#define ENTT_ENTITY_PREFAB_HPP

#include "../config/config.h"
#include "../container/dense_set.hpp"
#include "../core/type_info.hpp"
#include "../core/type_traits.hpp"
#include "../stl/cstdint.hpp"
#include "../stl/functional.hpp"
#include "../stl/iterator.hpp"
#include "../stl/tuple.hpp"
#include "../stl/type_traits.hpp"
#include "../stl/utility.hpp"
#include "../stl/vector.hpp"
#include "entity.hpp"
#include "fwd.hpp"
#include "registry.hpp"

namespace entt {

/**
 * @brief Policy to apply when a cloned component refers to an entity that
 * doesn't belong to the selected subgraph.
 */
enum class external_ref_policy : stl::uint8_t {
    /*! References outside the subgraph are kept as they are. */
    preserve = 0u,
    /*! References outside the subgraph make the instantiation fail. */
    reject = 1u
};

/**
 * @brief Reasons for which a prefab instantiation can fail.
 */
enum class prefab_error : stl::uint8_t {
    /*! The instantiation succeeded. */
    success = 0u,
    /*! A selected identifier doesn't refer to a valid entity. */
    invalid_entity = 1u,
    /*! A declared component is missing on a selected entity. */
    missing_component = 2u,
    /*! An override targets an entity outside the subgraph. */
    external_entity = 3u,
    /*! A component reference outside the subgraph was rejected. */
    external_reference = 4u,
    /*! A component copy or an override threw an exception. */
    aborted = 5u
};

/**
 * @brief Result of a prefab instantiation.
 * @tparam Entity A valid entity type.
 */
template<typename Entity>
struct prefab_result {
    /**
     * @brief Maps identifiers of the source subgraph to their clones.
     *
     * A null identifier is returned for unknown or unmapped entities.
     */
    class entity_mapper {
        template<typename>
        friend class basic_prefab;

        using map_allocator = typename stl::allocator_traits<stl::allocator<Entity>>::template rebind_alloc<stl::pair<const Entity, Entity>>;
        using map_type = dense_map<Entity, Entity, stl::identity, stl::equal_to<>, map_allocator>;

    public:
        /*! @brief Underlying entity identifier. */
        using entity_type = Entity;
        /*! @brief Unsigned integer type. */
        using size_type = stl::size_t;

        /*! @brief Default constructor. */
        entity_mapper() = default;

        /**
         * @brief Tests whether the mapper knows a given source entity.
         * @param entt A valid identifier.
         * @return True if the identifier has a counterpart, false otherwise.
         */
        [[nodiscard]] bool contains(const entity_type entt) const noexcept {
            return mapping.contains(entt);
        }

        /**
         * @brief Returns the counterpart of an entity, if any.
         * @param entt A valid identifier.
         * @return The cloned identifier if any, the null entity otherwise.
         */
        [[nodiscard]] entity_type map(const entity_type entt) const noexcept {
            if(const auto it = mapping.find(entt); it != mapping.cend()) {
                return it->second;
            }

            return null;
        }

        /**
         * @brief Returns the number of mapped entities.
         * @return The number of mapped entities.
         */
        [[nodiscard]] size_type size() const noexcept {
            return mapping.size();
        }

        /*! @brief Clears the mapper. */
        void clear() noexcept {
            mapping.clear();
        }

    private:
        map_type mapping{};
    };

    /*! @brief Underlying entity identifier. */
    using entity_type = Entity;
    /*! @brief Mapper type. */
    using entity_mapper_type = entity_mapper;

    /*! @brief Outcome of the instantiation. */
    prefab_error error{prefab_error::aborted};
    /*! @brief Entity that triggered the failure, the null entity on success. */
    entity_type reason{null};
    /*! @brief Old-to-new identifier map, populated only on success. */
    entity_mapper entities{};

    /*! @brief True in case of success, false otherwise. */
    explicit constexpr operator bool() const noexcept {
        return error == prefab_error::success;
    }
};

/**
 * @brief Utility class to instantiate independent copies of entity subgraphs.
 *
 * A prefab selects a set of entities (the _subgraph_) and a set of component
 * types to duplicate. Internal references, that is references to entities that
 * are part of the subgraph, are remapped to the newly created identifiers while
 * references to entities outside the subgraph are preserved or rejected
 * according to the configured policy.<br/>
 * The instantiation is all-or-nothing: in case of failure, every entity
 * created for the instance is destroyed and the registry is left untouched.
 *
 * @tparam Registry Basic registry type.
 */
template<typename Registry>
class basic_prefab {
    static_assert(!stl::is_const_v<Registry>, "Non-const registry type required");

public:
    /*! Basic registry type. */
    using registry_type = Registry;
    /*! @brief Underlying entity identifier. */
    using entity_type = typename Registry::entity_type;
    /*! @brief Unsigned integer type. */
    using size_type = typename Registry::size_type;
    /*! @brief Result type of an instantiation. */
    using result_type = prefab_result<entity_type>;
    /*! @brief Mapper type returned on success. */
    using entity_mapper = typename result_type::entity_mapper;

    /*! @brief Signature of the link callbacks invoked on cloned components. */
    using link_callback = stl::function<void(registry_type &, entity_type, const entity_mapper &, external_ref_policy, bool &, entity_type &)>;
    /*! @brief Signature of the override callbacks invoked on cloned components. */
    using override_callback = stl::function<void(registry_type &, entity_type)>;

private:
    using allocator_type = typename registry_type::allocator_type;

    /*! @brief Per-component-type operations and user configuration. */
    struct component_entry {
        id_type type{};
        bool required{true};
        bool (*contains)(const registry_type &, entity_type) = nullptr;
        void (*copy)(registry_type &, const registry_type &, entity_type, entity_type) = nullptr;
        stl::vector<link_callback> links{};
        stl::vector<stl::pair<entity_type, override_callback>> overrides{};
    };

    using selection_allocator = typename stl::allocator_traits<allocator_type>::template rebind_alloc<entity_type>;
    using entry_allocator = typename stl::allocator_traits<allocator_type>::template rebind_alloc<component_entry>;

public:
    /**
     * @brief Constructs a prefab bound to a given registry.
     * @param source A valid reference to a registry.
     */
    explicit basic_prefab(registry_type &source) noexcept
        : reg{&source} {}

    /*! @brief Default copy constructor, deleted on purpose. */
    basic_prefab(const basic_prefab &) = delete;

    /*! @brief Default move constructor. */
    basic_prefab(basic_prefab &&) noexcept = default;

    /*! @brief Default destructor. */
    ~basic_prefab() = default;

    /*! @brief Default copy assignment operator, deleted on purpose. */
    basic_prefab &operator=(const basic_prefab &) = delete;

    /*! @brief Default move assignment operator. */
    basic_prefab &operator=(basic_prefab &&) noexcept = default;

    /**
     * @brief Returns the registry to which the prefab is bound.
     * @return The registry to which the prefab is bound.
     */
    [[nodiscard]] registry_type &registry() const noexcept {
        return *reg;
    }

    /**
     * @brief Adds an entity to the subgraph to instantiate.
     *
     * Adding the same identifier more than once has no effect, duplicates are
     * silently merged. Identifiers of destroyed entities are kept and reported
     * as invalid when the subgraph is instantiated.
     *
     * @param entt An identifier, valid or destroyed.
     * @return A non-const reference to this prefab.
     */
    basic_prefab &entity(const entity_type entt) {
        selection.emplace(entt);
        return *this;
    }

    /**
     * @brief Adds all entities in a range to the subgraph to instantiate.
     * @tparam It Type of input iterator.
     * @param first An iterator to the first entity of the range.
     * @param last An iterator past the last entity of the range.
     * @return A non-const reference to this prefab.
     */
    template<stl::input_iterator It>
    basic_prefab &entity(It first, It last) {
        for(; first != last; ++first) {
            entity(*first);
        }

        return *this;
    }

    /**
     * @brief Tests whether an entity is part of the selected subgraph.
     * @param entt A valid identifier.
     * @return True if the entity is selected, false otherwise.
     */
    [[nodiscard]] bool contains(const entity_type entt) const noexcept {
        return selection.contains(entt);
    }

    /**
     * @brief Returns the number of selected entities.
     * @return The number of selected entities.
     */
    [[nodiscard]] size_type size() const noexcept {
        return selection.size();
    }

    /*! @brief Clears the whole prefab configuration. */
    void clear() noexcept {
        selection.clear();
        entries.clear();
        created.clear();
    }

    /**
     * @brief Declares a component type to duplicate for every selected entity.
     *
     * Components that are missing on a selected entity make the instantiation
     * fail rather than being silently skipped. Use `optional_component` to copy
     * a component only where it is present.
     *
     * @tparam Type Type of component to duplicate.
     * @return A non-const reference to this prefab.
     */
    template<typename Type>
    basic_prefab &component() {
        register_component<Type>(true);
        return *this;
    }

    /**
     * @brief Declares an optional component type to duplicate.
     *
     * The component is copied on the selected entities that own it and simply
     * skipped on the others, without making the instantiation fail.
     *
     * @tparam Type Type of component to duplicate.
     * @return A non-const reference to this prefab.
     */
    template<typename Type>
    basic_prefab &optional_component() {
        register_component<Type>(false);
        return *this;
    }

    /**
     * @brief Declares all the given component types at once.
     * @tparam Type Types of components to duplicate.
     * @return A non-const reference to this prefab.
     */
    template<typename... Type>
    basic_prefab &components() {
        return (component<Type>(), ...);
    }

    /**
     * @brief Declares a data member that contains an entity reference.
     *
     * The member is visited on every cloned instance of the given component and
     * remapped when it points to an entity of the subgraph. Members that point
     * outside the subgraph follow the configured external reference policy.
     *
     * @tparam Type Component type that owns the member.
     * @tparam Other Type that actually declares the member.
     * @tparam Member Type of the data member.
     * @param member The data member to remap.
     * @return A non-const reference to this prefab.
     */
    template<typename Type, typename Other, typename Member>
    basic_prefab &link(Member Other::*member) {
        optional_component<Type>();
        entry_for<Type>().links.push_back([member](registry_type &instance, const entity_type dst, const entity_mapper &mapper, const external_ref_policy policy, bool &rejected, entity_type &offender) {
            update_member(static_cast<Other &>(instance.template get<Type>(dst)).*member, mapper, policy, rejected, offender);
        });
        return *this;
    }

    /**
     * @brief Declares a custom function that visits a cloned component.
     *
     * The function object is invoked for every cloned instance of the given
     * component and receives the component, the mapper and the active policy.
     *
     * @tparam Type Component type to visit.
     * @tparam Func Type of the function object.
     * @param func A valid function object.
     * @return A non-const reference to this prefab.
     */
    template<typename Type, typename Func>
    basic_prefab &link(Func func) {
        optional_component<Type>();
        entry_for<Type>().links.push_back([fn = stl::move(func)](registry_type &instance, const entity_type dst, const entity_mapper &mapper, const external_ref_policy policy, bool &rejected, entity_type &offender) mutable {
            fn(instance.template get<Type>(dst), mapper, policy, rejected, offender);
        });
        return *this;
    }

    /**
     * @brief Overrides a component on a cloned entity with a new value.
     * @tparam Type Component type to override.
     * @param src Source entity whose clone receives the override.
     * @param value Value to assign to the cloned component.
     * @return A non-const reference to this prefab.
     */
    template<typename Type>
    basic_prefab & override(const entity_type src, Type value) {
        optional_component<Type>();
        add_override<Type>(src, [elem = stl::move(value)](registry_type &instance, const entity_type dst) {
            instance.template emplace_or_replace<Type>(dst, elem);
        });
        return *this;
    }

    /**
     * @brief Overrides a component on a cloned entity by patching it in-place.
     * @tparam Type Component component type to override.
     * @tparam Func Type of the function object.
     * @param src Source entity whose clone receives the override.
     * @param func Function object invoked with the cloned component.
     * @return A non-const reference to this prefab.
     */
    template<typename Type, typename Func>
    requires stl::is_invocable_v<Func, Type &>
    basic_prefab & override(const entity_type src, Func func) {
        optional_component<Type>();
        add_override<Type>(src, [fn = stl::move(func)](registry_type &instance, const entity_type dst) {
            instance.template patch<Type>(dst, fn);
        });
        return *this;
    }

    /**
     * @brief Sets the policy for references to entities outside the subgraph.
     * @param policy The required policy.
     * @return A non-const reference to this prefab.
     */
    basic_prefab &on_external_reference(const external_ref_policy policy) noexcept {
        external = policy;
        return *this;
    }

    /**
     * @brief Returns the policy for references to entities outside the subgraph.
     * @return The active policy.
     */
    [[nodiscard]] external_ref_policy on_external_reference() const noexcept {
        return external;
    }

    /**
     * @brief Applies the active external-reference policy to an identifier.
     *
     * Internal references are remapped through the mapper. External references
     * are returned unchanged under the preserve policy while the reject policy
     * reports them through the rejected flag and leaves the value untouched.
     *
     * @param entt The identifier to resolve.
     * @param mapper The old-to-new identifier map.
     * @param policy The external-reference policy.
     * @param rejected Set to true when an external reference is rejected.
     * @return The remapped identifier or the original one when preserved.
     */
    [[nodiscard]] static entity_type resolve(const entity_type entt, const entity_mapper &mapper, const external_ref_policy policy, bool &rejected) noexcept {
        if(mapper.contains(entt)) {
            return mapper.map(entt);
        }

        if(policy == external_ref_policy::reject && entt != null) {
            rejected = true;
        }

        return entt;
    }

    /**
     * @brief Instantiates the selected subgraph.
     *
     * On success, the returned object contains the old-to-new identifier map.
     * On failure, `error` describes the problem, `reason` points at the entity
     * that triggered it and no partially created entity is left behind.
     *
     * @return The result of the instantiation.
     */
    result_type instantiate() {
        result_type result{};
        created.clear();

        // phase 0: validate the selection, no entity is created on failure
        for(const auto src: selection) {
            if(!reg->valid(src)) {
                result.error = prefab_error::invalid_entity;
                result.reason = src;
                return result;
            }

            for(const auto &entry: entries) {
                if(entry.required && !entry.contains(*reg, src)) {
                    result.error = prefab_error::missing_component;
                    result.reason = src;
                    return result;
                }
            }
        }

        for(const auto &entry: entries) {
            for(const auto &[src, fn]: entry.overrides) {
                if(!selection.contains(src)) {
                    result.error = prefab_error::external_entity;
                    result.reason = src;
                    return result;
                }
            }
        }

        bool rejected = false;
        entity_type offender = null;

        // phases 1 to 3: create, copy, override and remap, all or nothing
        ENTT_TRY {
            // phase 1: create the clones and fill the identifier map
            created.reserve(selection.size());
            result.entities.mapping.reserve(selection.size());

            for(const auto src: selection) {
                const auto dst = reg->create();
                // track the clone before touching the map so a throw still rolls it back
                created.push_back(dst);
                result.entities.mapping.emplace(src, dst);
            }

            for(const auto src: selection) {
                const auto dst = result.entities.map(src);

                for(const auto &entry: entries) {
                    if(entry.contains(*reg, src)) {
                        entry.copy(*reg, *reg, src, dst);
                    }
                }
            }

            for(const auto &entry: entries) {
                for(const auto &[src, fn]: entry.overrides) {
                    fn(*reg, result.entities.map(src));
                }
            }

            for(const auto src: selection) {
                const auto dst = result.entities.map(src);

                for(auto &entry: entries) {
                    if(entry.contains(*reg, src)) {
                        for(auto &fn: entry.links) {
                            fn(*reg, dst, result.entities, external, rejected, offender);
                        }
                    }
                }
            }
        }
        ENTT_CATCH {
            rollback();
            result.entities.clear();
            result.error = prefab_error::aborted;
            result.reason = null;
            return result;
        }

        if(rejected) {
            rollback();
            result.entities.clear();
            result.error = prefab_error::external_reference;
            result.reason = offender;
            return result;
        }

        created.clear();
        result.error = prefab_error::success;
        result.reason = null;
        return result;
    }

private:
    template<typename Type>
    void register_component(const bool required) {
        static_assert(!stl::is_same_v<Type, entity_type>, "Entity storage not supported");
        static_assert(stl::is_copy_constructible_v<Type>, "Components must be copy constructible");

        const auto hash = type_hash<Type>::value();

        if(auto it = find_entry(hash); it != entries.end()) {
            // an optional registration cannot downgrade an existing required one
            it->required = it->required || required;
        } else {
            entries.push_back(component_entry{
                hash,
                required,
                [](const registry_type &instance, const entity_type entt) { return instance.template all_of<Type>(entt); },
                [](registry_type &instance, const registry_type &source, const entity_type src, const entity_type dst) {
                    if constexpr(stl::tuple_size_v<decltype(source.template storage<Type>()->get_as_tuple(src))> == 0u) {
                        // empty (no-instance) components carry no payload to copy
                        instance.template emplace<Type>(dst);
                    } else {
                        instance.template emplace<Type>(dst, source.template get<Type>(src));
                    }
                },
                {},
                {}});
        }
    }

    [[nodiscard]] auto find_entry(const id_type hash) {
        for(auto it = entries.begin(); it != entries.end(); ++it) {
            if(it->type == hash) {
                return it;
            }
        }

        return entries.end();
    }

    template<typename Type>
    [[nodiscard]] component_entry &entry_for() {
        const auto hash = type_hash<Type>::value();

        for(auto &entry: entries) {
            if(entry.type == hash) {
                return entry;
            }
        }

        // component<Type>() always runs first and inserts the entry
        ENTT_ASSERT(false, "Component not registered");
        return entries.front();
    }

    template<typename Type>
    void add_override(const entity_type src, override_callback callback) {
        auto &overrides = entry_for<Type>().overrides;

        for(auto &[curr, fn]: overrides) {
            if(curr == src) {
                fn = stl::move(callback);
                return;
            }
        }

        overrides.push_back({src, stl::move(callback)});
    }

    template<typename Type>
    static void update_member(Type &member, const entity_mapper &mapper, const external_ref_policy policy, bool &rejected, entity_type &offender) {
        if constexpr(stl::is_same_v<Type, entity_type>) {
            if(member != null) {
                const auto before = member;
                member = resolve(member, mapper, policy, rejected);

                if(rejected) {
                    offender = before;
                }
            }
        } else {
            for(auto &curr: member) {
                if(curr != null) {
                    const auto before = curr;
                    curr = resolve(curr, mapper, policy, rejected);

                    if(rejected) {
                        offender = before;
                    }
                }
            }
        }
    }

    void rollback() noexcept {
        for(auto it = created.rbegin(); it != created.rend(); ++it) {
            reg->destroy(*it);
        }

        created.clear();
    }

    dense_set<entity_type, stl::identity, stl::equal_to<>, selection_allocator> selection{};
    stl::vector<component_entry, entry_allocator> entries{};
    stl::vector<entity_type, typename stl::allocator_traits<allocator_type>::template rebind_alloc<entity_type>> created{};
    external_ref_policy external{external_ref_policy::preserve};
    registry_type *reg;
};

} // namespace entt

#endif
