// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "components/river.h"
#include <math.h>
#include <memory>
#include "component_library/rendermesh.h"
#include "component_library/transform.h"
#include "components/rail_denizen.h"
#include "components/rail_node.h"
#include "components/services.h"
#include "components_generated.h"
#include "config_generated.h"
#include "fplbase/utilities.h"
#include "world_editor/editor_event.h"

using mathfu::vec2;
using mathfu::vec3;
using mathfu::quat;
using mathfu::kAxisZ3f;

FPL_ENTITY_DEFINE_COMPONENT(fpl::fpl_project::RiverComponent,
                            fpl::fpl_project::RiverData)

namespace fpl {
namespace fpl_project {

using fpl::component_library::RenderMeshComponent;
using fpl::component_library::RenderMeshData;

static const size_t kNumIndicesPerQuad = 6;

void RiverComponent::Init() {
  event_manager_ =
      entity_manager_->GetComponent<ServicesComponent>()->event_manager();
  event_manager_->RegisterListener(EventSinkUnion_EditorEvent, this);
}

void RiverComponent::AddFromRawData(entity::EntityRef& entity,
                                    const void* raw_data) {
  auto river_def = static_cast<const RiverDef*>(raw_data);
  RiverData* river_data = AddEntity(entity);
  river_data->rail_name = river_def->rail_name()->c_str();
  river_data->random_seed = river_def->random_seed();

  entity_manager_->AddEntityToComponent<RenderMeshComponent>(entity);

  CreateRiverMesh(entity);
}

entity::ComponentInterface::RawDataUniquePtr RiverComponent::ExportRawData(
    entity::EntityRef& entity) const {
  const RiverData* data = GetComponentData(entity);
  if (data == nullptr) return nullptr;

  flatbuffers::FlatBufferBuilder fbb;
  auto rail_name =
      data->rail_name != "" ? fbb.CreateString(data->rail_name) : 0;

  RiverDefBuilder builder(fbb);
  if (rail_name.o != 0) {
    builder.add_rail_name(rail_name);
  }
  builder.add_random_seed(data->random_seed);

  fbb.Finish(builder.Finish().Union());
  return fbb.ReleaseBufferPointer();
}

// Generates the actual mesh for the river, and adds it to this entitiy's
// rendermesh component.
void RiverComponent::CreateRiverMesh(entity::EntityRef& entity) {
  static const Attribute kMeshFormat[] = {kPosition3f, kTexCoord2f, kNormal3f,
                                          kTangent4f, kEND};
  std::vector<mathfu::vec3_packed> track;
  const RiverConfig* river = entity_manager_->GetComponent<ServicesComponent>()
                                 ->config()
                                 ->river_config();

  RiverData* river_data = Data<RiverData>(entity);

  Rail* rail = entity_manager_->GetComponent<ServicesComponent>()
                   ->rail_manager()
                   ->GetRailFromComponents(river_data->rail_name.c_str(),
                                           entity_manager_);

  // Generate the spline data and store it in our track vector:
  rail->Positions(river->spline_stepsize(), &track);

  const size_t num_bank_contours = river->banks()->Length();
  const size_t num_bank_quads = num_bank_contours - 2;
  const size_t river_idx = river->river_index();
  const size_t segment_count = track.size();
  const size_t river_vert_max = segment_count * 2;
  const size_t river_index_max = (segment_count - 1) * kNumIndicesPerQuad;
  const size_t bank_vert_max = segment_count * num_bank_contours;
  const size_t bank_index_max =
      (segment_count - 1) * kNumIndicesPerQuad * num_bank_quads;
  assert(num_bank_contours >= 2 && river_idx < num_bank_contours - 1);

  // Need to allocate some space to plan out our mesh in:
  std::vector<NormalMappedVertex> river_verts(river_vert_max);
  river_verts.clear();
  std::vector<unsigned short> river_indices(river_index_max);
  river_indices.clear();

  std::vector<NormalMappedVertex> bank_verts(bank_vert_max);
  bank_verts.clear();
  std::vector<unsigned short> bank_indices(bank_index_max);
  bank_indices.clear();

  // TODO: Use a local random number generator. Resetting the global random
  //       number generator is not a nice. Also, there's no guarantee that
  //       mathfu::Random will continue to use rand().
  srand(river_data->random_seed);

  // Construct the actual mesh data for the river:
  std::vector<vec2> offsets(num_bank_contours);
  for (size_t i = 0; i < segment_count; i++) {
    // River track is circular.
    const size_t prev_i = i == 0 ? segment_count - 1 : i - 1;

    // Get the current position on the track, and the normal (to the side).
    const vec3 track_delta = vec3(track[i]) - vec3(track[prev_i]);
    const vec3 track_normal =
        vec3::CrossProduct(track_delta, kAxisZ3f).Normalized();
    const vec3 track_position =
        vec3(track[i]) + river->track_height() * kAxisZ3f;

    // The river texture is tiled several times along the course of the river.
    // TODO: Change this from tile count to actual physical size for a tile.
    //       Requires that we know the total path distance.
    const float texture_v = river->texture_tile_size() * static_cast<float>(i) /
                            static_cast<float>(segment_count);

    // Get the (side, up) offsets of the bank vertices.
    // The offsets are relative to `track_position`.
    // side == distance along `track_normal`
    // up == distance along kAxisZ3f
    for (size_t j = 0; j < num_bank_contours; ++j) {
      const RiverBankContour* b = river->banks()->Get(j);
      offsets[j] =
          vec2(mathfu::Lerp(b->x_min(), b->x_max(), mathfu::Random<float>()),
               mathfu::Lerp(b->z_min(), b->z_max(), mathfu::Random<float>()));
    }

    // Create the bank vertices for this segment.
    for (size_t j = 0; j < num_bank_contours; ++j) {
      const vec2 off = offsets[j];
      const vec3 vertex =
          track_position + off.x() * track_normal + off.y() * kAxisZ3f;

      // The texture is stretched from the side of the river to the far end
      // of the bank. There are two banks, however, separated by the river.
      // We need to know the width of the bank to caluate the `texture_u`
      // coordinate.
      const bool left_bank = j <= river_idx;
      const size_t bank_start = left_bank ? 0 : river_idx + 1;
      const size_t bank_end = left_bank ? river_idx : num_bank_contours - 1;
      const float bank_width = offsets[bank_start].x() - offsets[bank_end].x();
      const float texture_u = (off.x() - offsets[bank_end].x()) / bank_width;

      bank_verts.push_back(NormalMappedVertex{
          vec3_packed(vertex), vec2_packed(vec2(texture_u, texture_v)),
          vec3_packed(vec3(0, 1, 0)), vec4_packed(vec4(1, 0, 0, 1)),
      });
    }

    // Ensure vertices don't go behind previous vertices on the inside of
    // a tight corner.
    if (i > 0) {
      const NormalMappedVertex* prev_verts =
          &bank_verts[bank_verts.size() - 2 * num_bank_contours];
      NormalMappedVertex* cur_verts =
          &bank_verts[bank_verts.size() - num_bank_contours];
      for (size_t j = 0; j < num_bank_contours; j++) {
        const vec3 vert_delta =
            vec3(cur_verts[j].pos) - vec3(prev_verts[j].pos);
        const float dot = vec3::DotProduct(vert_delta, track_delta);
        const bool cur_vert_goes_backwards_along_track = dot <= 0.0f;
        if (cur_vert_goes_backwards_along_track) {
          cur_verts[j].pos = vec3(prev_verts[j].pos) + 0.000001f * track_delta;
        }
      }
    }

    // Force the beginning and end to line up in their geometry:
    if (i == segment_count - 1) {
      for (size_t j = 0; j < num_bank_contours; j++)
        bank_verts[bank_verts.size() - (8 - j)].pos = bank_verts[j].pos;
    }

    // The river has two of the middle vertices of the bank.
    // The texture coordinates are different, however.
    const size_t river_vert = bank_verts.size() - num_bank_contours + river_idx;
    river_verts.push_back(bank_verts[river_vert]);
    river_verts.back().tc = vec2(0.0f, texture_v);
    river_verts.push_back(bank_verts[river_vert + 1]);
    river_verts.back().tc = vec2(1.0f, texture_v);
  }

  // Not counting the first segment, create triangles in our index
  // list to represent this segment.
  //
  for (size_t i = 0; i < segment_count - 1; i++) {
    auto make_quad = [&](std::vector<unsigned short>& indices, int base_index,
                         int off1, int off2) {
      indices.push_back(base_index + off1);
      indices.push_back(base_index + off1 + 1);
      indices.push_back(base_index + off2);

      indices.push_back(base_index + off2);
      indices.push_back(base_index + off1 + 1);
      indices.push_back(base_index + off2 + 1);
    };

    // River only has one quad per segment.
    make_quad(river_indices, 2 * i, 0, 2);

    // Case when kNumBankCountours = 8, and river_idx = 3;
    //
    //  0___1___2___3   4___5___6___7
    //  | _/| _/| _/|   | _/| _/| _/|
    //  |/__|/__|/__|   |/__|/__|/__|
    //  8   9  10  11  12  13  14  15
    for (size_t j = 0; j <= num_bank_quads; ++j) {
      // Do not create bank geo for the river.
      if (j == river_idx) continue;
      make_quad(bank_indices, i * num_bank_contours, j, num_bank_contours + j);
    }
  }

  // Make sure we used as much data as expected, and no more.
  assert(river_indices.size() == river_index_max);
  assert(river_verts.size() == river_vert_max);
  assert(bank_indices.size() == bank_index_max);
  assert(bank_verts.size() == bank_vert_max);

  Mesh::ComputeNormalsTangents(bank_verts.data(), bank_indices.data(),
                               bank_verts.size(), bank_indices.size());

  AssetManager* asset_manager =
      entity_manager_->GetComponent<ServicesComponent>()->asset_manager();

  // Load the material from files.
  Material* river_material =
      asset_manager->LoadMaterial(river->material()->c_str());
  Material* bank_material =
      asset_manager->LoadMaterial("materials/ground_material.fplmat");

  // Create the actual mesh objects, and stuff all the data we just
  // generated into it.
  Mesh* river_mesh = new Mesh(river_verts.data(), river_verts.size(),
                              sizeof(NormalMappedVertex), kMeshFormat);

  river_mesh->AddIndices(river_indices.data(), river_indices.size(),
                         river_material);

  Mesh* bank_mesh = new Mesh(bank_verts.data(), bank_verts.size(),
                             sizeof(NormalMappedVertex), kMeshFormat);

  bank_mesh->AddIndices(bank_indices.data(), bank_indices.size(),
                        bank_material);

  // Add the river mesh to the river entity.
  RenderMeshData* mesh_data = Data<RenderMeshData>(entity);
  mesh_data->shader = asset_manager->LoadShader(river->shader()->c_str());
  mesh_data->mesh = river_mesh;
  mesh_data->ignore_culling = true;  // Never cull the river.
  mesh_data->pass_mask = 1 << RenderPass_kOpaque;

  if (!river_data->bank) {
    // Now we make a new entity to hold the bank mesh.
    river_data->bank = entity_manager_->AllocateNewEntity();
    entity_manager_->AddEntityToComponent<RenderMeshComponent>(
        river_data->bank);

    // Then we stick it as a child of the river entity, so it always moves
    // with it and stays aligned:
    auto transform_component =
        GetComponent<component_library::TransformComponent>();
    transform_component->AddChild(river_data->bank, entity);
  }

  RenderMeshData* child_render_data = Data<RenderMeshData>(river_data->bank);
  child_render_data->shader =
      asset_manager->LoadShader("shaders/textured_opaque");
  child_render_data->mesh = bank_mesh;
  child_render_data->ignore_culling = true;  // Never cull the banking.
  child_render_data->pass_mask = 1 << RenderPass_kOpaque;
}

void RiverComponent::OnEvent(const event::EventPayload& event_payload) {
  switch (event_payload.id()) {
    case EventSinkUnion_EditorEvent: {
      auto* editor_event = event_payload.ToData<editor::EditorEventPayload>();
      if (editor_event->action == EditorEventAction_EntityUpdated &&
          editor_event->entity) {
        const RailNodeData* node_data =
            entity_manager_->GetComponentData<RailNodeData>(
                editor_event->entity);
        if (node_data != nullptr) {
          // For now, update all rivers. In the future only update rivers that
          // have the same rail_name that just changed.
          for (auto iter = begin(); iter != end(); ++iter) {
            CreateRiverMesh(iter->entity);
          }
        }
      }
      break;
    }
    default: { assert(0); }
  }
}

}  // fpl_project
}  // fpl