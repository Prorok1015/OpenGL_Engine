#pragma once
#include "desc_base.hpp"
#include "ecs_entity.h"
#include "scn_mesh_nodes.hpp"

#include <string>
#include <vector>

namespace scn {

class animations_desc : public desc::desc_base {
public:
  static constexpr std::string_view __type = "animations_desc";

  using base_type = desc::desc_base;

  animations_desc() = default;
  ~animations_desc() = default;
  animations_desc(const animations_desc &) = default;
  animations_desc(animations_desc &&) = default;
  animations_desc &operator=(const animations_desc &) = default;
  animations_desc &operator=(animations_desc &&) = default;

  virtual void copy_to(desc_base &other) const override {
    animations_desc &other_desc = static_cast<animations_desc &>(other);
    other_desc = *this;
  }
  virtual void deserialize(desc::desc_system &desc_system,
                           const json::object &) override;
  virtual void serialize(json::object &) const override;

  std::vector<scn::animation> animations;
};

void assemble_animations(entt::registry &reg, entt::entity e,
                         const animations_desc &desc, std::string_view name);
} // namespace scn
