/* SPDX-FileCopyrightText: 2023 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "usd_skel_convert.hh"

#include "usd_armature_utils.hh"
#include "usd_blend_shape_utils.hh"
#include "usd_hash_types.hh"

#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShape.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/utils.h>

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_key_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_action.h"
#include "BKE_armature.hh"
#include "BKE_deform.hh"
#include "BKE_fcurve.hh"
#include "BKE_key.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object_deform.h"
#include "BKE_report.hh"

#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "ED_armature.hh"
#include "ED_mesh.hh"
#include "ED_object_vgroup.hh"

#include "ANIM_animdata.hh"
#include "ANIM_fcurve.hh"

#include <string>
#include <vector>

#include "CLG_log.h"
static CLG_LogRef LOG = {"io.usd"};

namespace {

/* Utility: return the magnitude of the largest component
 * of the given vector. */
inline float max_mag_component(const pxr::GfVec3d &vec)
{
  return pxr::GfMax(pxr::GfAbs(vec[0]), pxr::GfAbs(vec[1]), pxr::GfAbs(vec[2]));
}

/* Utility: create curve at the given array index. */
FCurve *create_fcurve(const int array_index, const std::string &rna_path)
{
  FCurve *fcu = BKE_fcurve_create();
  fcu->flag = (FCURVE_VISIBLE | FCURVE_SELECTED);
  fcu->rna_path = BLI_strdup(rna_path.c_str());
  fcu->array_index = array_index;
  return fcu;
}

/* Utility: create curve at the given array index and
 * add it as a channel to a group. */
FCurve *create_chan_fcurve(bAction *act,
                           bActionGroup *grp,
                           const int array_index,
                           const std::string &rna_path,
                           const int totvert)
{
  FCurve *fcu = create_fcurve(array_index, rna_path);
  fcu->totvert = totvert;
  action_groups_add_channel(act, grp, fcu);
  return fcu;
}

/* Utility: add curve sample. */
void add_bezt(FCurve *fcu,
              const float frame,
              const float value,
              const eBezTriple_Interpolation ipo = BEZT_IPO_LIN)
{
  BezTriple bez;
  memset(&bez, 0, sizeof(BezTriple));
  bez.vec[1][0] = frame;
  bez.vec[1][1] = value;
  bez.ipo = ipo; /* use default interpolation mode here... */
  bez.f1 = bez.f2 = bez.f3 = SELECT;
  bez.h1 = bez.h2 = HD_AUTO;
  blender::animrig::insert_bezt_fcurve(fcu, &bez, INSERTKEY_NOFLAGS);
}

/**
 * Import a USD skeleton animation as an action on the given armature object.
 * This assumes bones have already been created on the armature.
 *
 * \param bmain: Main pointer
 * \param arm_obj: Armature object to which the action will be added
 * \param skel_query: The USD skeleton query for reading the animation
 * \param joint_to_bone_map: Map a USD skeleton joint name to a bone name
 * \param reports: the storage for potential warning or error reports (generated using BKE_report
 *                 API).
 */
void import_skeleton_curves(Main *bmain,
                            Object *arm_obj,
                            const pxr::UsdSkelSkeletonQuery &skel_query,
                            const blender::Map<pxr::TfToken, std::string> &joint_to_bone_map,
                            ReportList *reports)

{
  if (!(bmain && arm_obj && skel_query)) {
    return;
  }

  if (joint_to_bone_map.is_empty()) {
    return;
  }

  const pxr::UsdSkelAnimQuery &anim_query = skel_query.GetAnimQuery();

  if (!anim_query) {
    /* No animation is defined. */
    return;
  }

  std::vector<double> samples;
  anim_query.GetJointTransformTimeSamples(&samples);

  if (samples.empty()) {
    return;
  }

  const size_t num_samples = samples.size();

  /* Create the action on the armature. */
  bAction *act = blender::animrig::id_action_ensure(bmain, (ID *)&arm_obj->id);

  /* Create the curves. */

  /* Get the joint paths. */
  pxr::VtTokenArray joint_order = skel_query.GetJointOrder();

  blender::Vector<FCurve *> loc_curves;
  blender::Vector<FCurve *> rot_curves;
  blender::Vector<FCurve *> scale_curves;

  /* Iterate over the joints and create the corresponding curves for the bones. */
  for (const pxr::TfToken &joint : joint_order) {
    const std::string *name = joint_to_bone_map.lookup_ptr(joint);

    if (name == nullptr) {
      /* This joint doesn't correspond to any bone we created.
       * Add null placeholders for the channel curves. */
      loc_curves.append(nullptr);
      loc_curves.append(nullptr);
      loc_curves.append(nullptr);
      rot_curves.append(nullptr);
      rot_curves.append(nullptr);
      rot_curves.append(nullptr);
      rot_curves.append(nullptr);
      scale_curves.append(nullptr);
      scale_curves.append(nullptr);
      scale_curves.append(nullptr);
      continue;
    }

    bActionGroup *grp = action_groups_add_new(act, name->c_str());

    /* Add translation curves. */
    std::string rna_path = "pose.bones[\"" + *name + "\"].location";
    loc_curves.append(create_chan_fcurve(act, grp, 0, rna_path, num_samples));
    loc_curves.append(create_chan_fcurve(act, grp, 1, rna_path, num_samples));
    loc_curves.append(create_chan_fcurve(act, grp, 2, rna_path, num_samples));

    /* Add rotation curves. */
    rna_path = "pose.bones[\"" + *name + "\"].rotation_quaternion";
    rot_curves.append(create_chan_fcurve(act, grp, 0, rna_path, num_samples));
    rot_curves.append(create_chan_fcurve(act, grp, 1, rna_path, num_samples));
    rot_curves.append(create_chan_fcurve(act, grp, 2, rna_path, num_samples));
    rot_curves.append(create_chan_fcurve(act, grp, 3, rna_path, num_samples));

    /* Add scale curves. */
    rna_path = "pose.bones[\"" + *name + "\"].scale";
    scale_curves.append(create_chan_fcurve(act, grp, 0, rna_path, num_samples));
    scale_curves.append(create_chan_fcurve(act, grp, 1, rna_path, num_samples));
    scale_curves.append(create_chan_fcurve(act, grp, 2, rna_path, num_samples));
  }

  /* Sanity checks: make sure we have a curve entry for each joint. */
  if (loc_curves.size() != joint_order.size() * 3) {
    CLOG_ERROR(&LOG, "Location curve count mismatch");
    return;
  }

  if (rot_curves.size() != joint_order.size() * 4) {
    CLOG_ERROR(&LOG, "Rotation curve count mismatch");
    return;
  }

  if (scale_curves.size() != joint_order.size() * 3) {
    CLOG_ERROR(&LOG, "Scale curve count mismatch");
    return;
  }

  /* The curve for each joint represents the transform relative
   * to the bind transform in joint-local space. I.e.,
   *
   * `jointLocalTransform * inv(jointLocalBindTransform)`
   *
   * There doesn't appear to be a way to query the joint-local
   * bind transform through the API, so we have to compute it
   * ourselves from the world bind transforms and the skeleton
   * topology.
   */

  /* Get the world space joint transforms at bind time. */
  pxr::VtMatrix4dArray bind_xforms;
  if (!skel_query.GetJointWorldBindTransforms(&bind_xforms)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get world bind transforms for skeleton %s",
                __func__,
                skel_query.GetSkeleton().GetPrim().GetPath().GetAsString().c_str());
    return;
  }

  if (bind_xforms.size() != joint_order.size()) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Number of bind transforms doesn't match the number of joints for skeleton %s",
                __func__,
                skel_query.GetSkeleton().GetPrim().GetPath().GetAsString().c_str());
    return;
  }

  const pxr::UsdSkelTopology &skel_topology = skel_query.GetTopology();

  pxr::VtMatrix4dArray joint_local_bind_xforms(bind_xforms.size());
  for (int i = 0; i < bind_xforms.size(); ++i) {
    const int parent_id = skel_topology.GetParent(i);

    if (parent_id >= 0) {
      /* This is a non-root joint.  Compute the bind transform of the joint
       * relative to its parent. */
      joint_local_bind_xforms[i] = bind_xforms[i] * bind_xforms[parent_id].GetInverse();
    }
    else {
      /* This is the root joint. */
      joint_local_bind_xforms[i] = bind_xforms[i];
    }
  }

  /* Set the curve samples. */
  for (const double frame : samples) {
    pxr::VtMatrix4dArray joint_local_xforms;
    if (!skel_query.ComputeJointLocalTransforms(&joint_local_xforms, frame)) {
      CLOG_WARN(&LOG, "Couldn't compute joint local transforms on frame %f", frame);
      continue;
    }

    if (joint_local_xforms.size() != joint_order.size()) {
      CLOG_WARN(
          &LOG,
          "Number of joint local transform entries %zu doesn't match the number of joints %zu",
          joint_local_xforms.size(),
          joint_order.size());
      continue;
    }

    for (int i = 0; i < joint_local_xforms.size(); ++i) {
      pxr::GfMatrix4d bone_xform = joint_local_xforms[i] * joint_local_bind_xforms[i].GetInverse();

      pxr::GfVec3f t;
      pxr::GfQuatf qrot;
      pxr::GfVec3h s;

      if (!pxr::UsdSkelDecomposeTransform(bone_xform, &t, &qrot, &s)) {
        CLOG_WARN(&LOG, "Error decomposing matrix on frame %f", frame);
        continue;
      }

      const float re = qrot.GetReal();
      const pxr::GfVec3f &im = qrot.GetImaginary();

      for (int j = 0; j < 3; ++j) {
        const int k = 3 * i + j;
        if (k >= loc_curves.size()) {
          CLOG_ERROR(&LOG, "Out of bounds translation curve index %d", k);
          break;
        }
        if (FCurve *fcu = loc_curves[k]) {
          add_bezt(fcu, frame, t[j]);
        }
      }

      for (int j = 0; j < 4; ++j) {
        const int k = 4 * i + j;
        if (k >= rot_curves.size()) {
          CLOG_ERROR(&LOG, "Out of bounds rotation curve index %d", k);
          break;
        }
        if (FCurve *fcu = rot_curves[k]) {
          if (j == 0) {
            add_bezt(fcu, frame, re);
          }
          else {
            add_bezt(fcu, frame, im[j - 1]);
          }
        }
      }

      for (int j = 0; j < 3; ++j) {
        const int k = 3 * i + j;
        if (k >= scale_curves.size()) {
          CLOG_ERROR(&LOG, "Out of bounds scale curve index %d", k);
          break;
        }
        if (FCurve *fcu = scale_curves[k]) {
          add_bezt(fcu, frame, s[j]);
        }
      }
    }
  }

  /* Recalculate curve handles. */
  auto recalc_handles = [](FCurve *fcu) { BKE_fcurve_handles_recalc(fcu); };
  std::for_each(loc_curves.begin(), loc_curves.end(), recalc_handles);
  std::for_each(rot_curves.begin(), rot_curves.end(), recalc_handles);
  std::for_each(scale_curves.begin(), scale_curves.end(), recalc_handles);
}

/* Set the skeleton path and bind transform on the given mesh. */
void add_skinned_mesh_bindings(const pxr::UsdSkelSkeleton &skel,
                               pxr::UsdPrim &mesh_prim,
                               pxr::UsdGeomXformCache &xf_cache)
{
  pxr::UsdSkelBindingAPI skel_api = pxr::UsdSkelBindingAPI::Apply(mesh_prim);

  if (!skel_api) {
    CLOG_WARN(&LOG,
              "Couldn't apply UsdSkelBindingAPI to skinned mesh prim %s",
              mesh_prim.GetPath().GetAsString().c_str());
    return;
  }

  /* Specify the path to the skeleton. */
  pxr::SdfPath skel_path = skel.GetPath();
  skel_api.CreateSkeletonRel().SetTargets(pxr::SdfPathVector({skel_path}));

  /* Set the mesh's bind transform. */
  if (pxr::UsdAttribute geom_bind_attr = skel_api.CreateGeomBindTransformAttr()) {
    /* The bind matrix is the mesh transform relative to the skeleton transform. */
    pxr::GfMatrix4d mesh_xf = xf_cache.GetLocalToWorldTransform(mesh_prim);
    pxr::GfMatrix4d skel_xf = xf_cache.GetLocalToWorldTransform(skel.GetPrim());
    pxr::GfMatrix4d bind_xf = mesh_xf * skel_xf.GetInverse();
    geom_bind_attr.Set(bind_xf);
  }
  else {
    CLOG_WARN(&LOG,
              "Couldn't create geom bind transform attribute for skinned mesh %s",
              mesh_prim.GetPath().GetAsString().c_str());
  }
}

}  // End anonymous namespace.

namespace blender::io::usd {

void import_blendshapes(Main *bmain,
                        Object *mesh_obj,
                        const pxr::UsdPrim &prim,
                        ReportList *reports,
                        const bool import_anim)
{
  if (!(mesh_obj && mesh_obj->data && mesh_obj->type == OB_MESH && prim)) {
    return;
  }

  if (prim.IsInstanceProxy()) {
    /* Attempting to create a UsdSkelBindingAPI for
     * instance proxies generates USD errors. */
    return;
  }

  pxr::UsdSkelBindingAPI skel_api(prim);

  /* Get the blend shape targets, which are the USD paths to the
   * blend shape primitives. */

  if (!skel_api.GetBlendShapeTargetsRel().HasAuthoredTargets()) {
    /* No targets. */
    return;
  }

  pxr::SdfPathVector targets;
  if (!skel_api.GetBlendShapeTargetsRel().GetTargets(&targets)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get blendshape targets for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  if (targets.empty()) {
    return;
  }

  if (!skel_api.GetBlendShapesAttr().HasAuthoredValue()) {
    return;
  }

  /* Get the blend shape name tokens. */
  pxr::VtTokenArray blendshapes;
  if (!skel_api.GetBlendShapesAttr().Get(&blendshapes)) {
    return;
  }

  if (blendshapes.empty()) {
    return;
  }

  /* Sanity check. */
  if (targets.size() != blendshapes.size()) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Number of blendshapes doesn't match number of blendshape targets for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  pxr::UsdStageRefPtr stage = prim.GetStage();

  if (!stage) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get stage for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  Mesh *mesh = static_cast<Mesh *>(mesh_obj->data);

  /* Insert key to source mesh. */
  Key *key = BKE_key_add(bmain, (ID *)mesh);
  key->type = KEY_RELATIVE;

  mesh->key = key;

  /* Insert basis key. */
  KeyBlock *kb = BKE_keyblock_add(key, "Basis");
  BKE_keyblock_convert_from_mesh(mesh, key, kb);

  /* Keep track of the shape-keys we're adding,
   * for validation when creating curves later. */
  blender::Set<pxr::TfToken> shapekey_names;

  for (int i = 0; i < targets.size(); ++i) {
    /* Get USD path to blend shape. */
    const pxr::SdfPath &path = targets[i];
    pxr::UsdSkelBlendShape blendshape(stage->GetPrimAtPath(path));

    if (!blendshape) {
      continue;
    }

    /* Get the blend shape offsets. */
    if (!blendshape.GetOffsetsAttr().HasAuthoredValue()) {
      /* Blend shape has no authored offsets. */
      continue;
    }

    pxr::VtVec3fArray offsets;
    if (!blendshape.GetOffsetsAttr().Get(&offsets)) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s: Couldn't get offsets for blend shape %s",
                  __func__,
                  path.GetAsString().c_str());
      continue;
    }

    if (offsets.empty()) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s: No offsets for blend shape %s",
                  __func__,
                  path.GetAsString().c_str());
      continue;
    }

    shapekey_names.add(blendshapes[i]);

    /* Add the key block. */
    kb = BKE_keyblock_add(key, blendshapes[i].GetString().c_str());
    BKE_keyblock_convert_from_mesh(mesh, key, kb);

    /* if authored, point indices are indices into the original mesh
     * that correspond to the values in the offsets array. */
    pxr::VtArray<int> point_indices;
    if (blendshape.GetPointIndicesAttr().HasAuthoredValue()) {
      blendshape.GetPointIndicesAttr().Get(&point_indices);
    }

    float *fp = static_cast<float *>(kb->data);

    if (point_indices.empty()) {
      /* Iterate over all key block elements and add the corresponding
       * offset to the key block point. */
      for (int a = 0; a < kb->totelem; ++a, fp += 3) {
        if (a >= offsets.size()) {
          BKE_reportf(
              reports,
              RPT_WARNING,
              "%s: Number of offsets greater than number of mesh vertices for blend shape %s",
              __func__,
              path.GetAsString().c_str());
          break;
        }
        add_v3_v3(fp, offsets[a].data());
      }
    }
    else {
      /* Iterate over the point indices and add the offset to the corresponding
       * key block point. */
      int a = 0;
      for (int i : point_indices) {
        if (i < 0 || i > kb->totelem) {
          CLOG_WARN(&LOG,
                    "Out of bounds point index %d for blendshape %s",
                    i,
                    path.GetAsString().c_str());
          ++a;
          continue;
        }
        if (a >= offsets.size()) {
          BKE_reportf(
              reports,
              RPT_WARNING,
              "%s: Number of offsets greater than number of mesh vertices for blend shape %s",
              __func__,
              path.GetAsString().c_str());
          break;
        }
        add_v3_v3(&fp[3 * i], offsets[a].data());
        ++a;
      }
    }
  }

  if (!import_anim) {
    /* We're not importing animation, so we are done. */
    return;
  }

  /* Get the blend animation source from the skeleton. */

  pxr::UsdSkelSkeleton skel_prim = skel_api.GetInheritedSkeleton();

  if (!skel_prim) {
    return;
  }

  skel_api = pxr::UsdSkelBindingAPI(skel_prim.GetPrim());

  pxr::UsdPrim anim_prim = skel_api.GetInheritedAnimationSource();

  if (!anim_prim) {
    /* Querying the directly bound animation source may be necessary
     * if the prim does not have an applied skel binding API schema. */
    skel_api.GetAnimationSource(&anim_prim);
  }

  if (!anim_prim) {
    return;
  }

  pxr::UsdSkelAnimation skel_anim(anim_prim);

  if (!skel_anim) {
    return;
  }

  /* Check if a blend shape weight animation was authored. */
  if (!skel_anim.GetBlendShapesAttr().HasAuthoredValue()) {
    return;
  }

  pxr::UsdAttribute weights_attr = skel_anim.GetBlendShapeWeightsAttr();

  if (!(weights_attr && weights_attr.HasAuthoredValue())) {
    return;
  }

  /* Get the animation time samples. */
  std::vector<double> times;
  if (!weights_attr.GetTimeSamples(&times)) {
    return;
  }

  if (times.empty()) {
    return;
  }

  /* Get the blend shape name tokens. */
  if (!skel_anim.GetBlendShapesAttr().Get(&blendshapes)) {
    return;
  }

  if (blendshapes.empty()) {
    return;
  }

  const size_t num_samples = times.size();

  /* Create the animation and curves. */
  bAction *act = blender::animrig::id_action_ensure(bmain, (ID *)&key->id);
  blender::Vector<FCurve *> curves;

  for (auto blendshape_name : blendshapes) {
    if (!shapekey_names.contains(blendshape_name)) {
      /* We didn't create a shape-key for this blend-shape, so we don't
       * create a curve and insert a null placeholder in the curve array. */
      curves.append(nullptr);
      continue;
    }

    /* Create the curve for this shape key. */
    std::string rna_path = "key_blocks[\"" + blendshape_name.GetString() + "\"].value";
    FCurve *fcu = create_fcurve(0, rna_path);
    fcu->totvert = num_samples;
    curves.append(fcu);
    BLI_addtail(&act->curves, fcu);
  }

  /* Add the weight time samples to the curves. */
  for (double frame : times) {
    pxr::VtFloatArray weights;
    if (!weights_attr.Get(&weights, frame)) {
      CLOG_WARN(&LOG, "Couldn't get blendshape weights for time %f", frame);
      continue;
    }

    if (weights.size() != curves.size()) {
      CLOG_WARN(
          &LOG,
          "Number of weight samples doesn't match number of shapekey curve entries for frame %f",
          frame);
      continue;
    }

    for (int wi = 0; wi < weights.size(); ++wi) {
      if (curves[wi] != nullptr) {
        add_bezt(curves[wi], frame, weights[wi]);
      }
    }
  }

  /* Recalculate curve handles. */
  auto recalc_handles = [](FCurve *fcu) { BKE_fcurve_handles_recalc(fcu); };
  std::for_each(curves.begin(), curves.end(), recalc_handles);
}

void import_skeleton(Main *bmain,
                     Object *arm_obj,
                     const pxr::UsdSkelSkeleton &skel,
                     ReportList *reports,
                     const bool import_anim)
{
  if (!(arm_obj && arm_obj->data && arm_obj->type == OB_ARMATURE)) {
    return;
  }

  pxr::UsdSkelCache skel_cache;
  pxr::UsdSkelSkeletonQuery skel_query = skel_cache.GetSkelQuery(skel);

  if (!skel_query.IsValid()) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't query skeleton %s",
                __func__,
                skel.GetPath().GetAsString().c_str());
    return;
  }

  const pxr::UsdSkelTopology &skel_topology = skel_query.GetTopology();

  pxr::VtTokenArray joint_order = skel_query.GetJointOrder();

  if (joint_order.size() != skel_topology.size()) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Topology and joint order size mismatch for skeleton %s",
                __func__,
                skel.GetPath().GetAsString().c_str());
    return;
  }

  bArmature *arm = static_cast<bArmature *>(arm_obj->data);

  /* Set the armature to edit mode when creating the bones. */
  ED_armature_to_edit(arm);

  /* The bones we create, stored in the skeleton's joint order. */
  blender::Vector<EditBone *> edit_bones;

  /* Keep track of the bones we create for each joint.
   * We'll need this when creating animation curves
   * later. */
  blender::Map<pxr::TfToken, std::string> joint_to_bone_map;

  /* Create the bones. */
  for (const pxr::TfToken &joint : joint_order) {
    std::string name = pxr::SdfPath(joint).GetName();
    EditBone *bone = ED_armature_ebone_add(arm, name.c_str());
    if (!bone) {
      BKE_reportf(reports,
                  RPT_WARNING,
                  "%s: Couldn't add bone for joint %s",
                  __func__,
                  joint.GetString().c_str());
      edit_bones.append(nullptr);
      continue;
    }
    joint_to_bone_map.add(joint, bone->name);
    edit_bones.append(bone);
  }

  /* Sanity check: we should have created a bone for each joint. */
  const size_t num_joints = skel_topology.GetNumJoints();
  if (edit_bones.size() != num_joints) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Mismatch in bone and joint counts for skeleton %s",
                __func__,
                skel.GetPath().GetAsString().c_str());
    return;
  }

  /* Get the world space joint transforms at bind time. */
  pxr::VtMatrix4dArray bind_xforms;
  if (!skel_query.GetJointWorldBindTransforms(&bind_xforms)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Couldn't get world bind transforms for skeleton %s",
                __func__,
                skel.GetPath().GetAsString().c_str());
    return;
  }

  if (bind_xforms.size() != num_joints) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s:  Mismatch in bind xforms and joint counts for skeleton %s",
                __func__,
                skel.GetPath().GetAsString().c_str());
    return;
  }

  /* Check if any bone matrices have negative determinants,
   * indicating negative scales, possibly due to mirroring
   * operations.  Such matrices can't be properly converted
   * to Blender's axis/roll bone representation (see
   * https://projects.blender.org/blender/blender/issues/82930).
   * If we detect such matrices, we will flag an error and won't
   * try to import the animation, since the rotations would
   * be incorrect in such cases. Unfortunately, the Pixar
   * `UsdSkel` examples of the "HumanFemale" suffer from
   * this issue. */
  bool negative_determinant = false;

  /* Set bone rest transforms. */
  for (size_t i = 0; i < num_joints; ++i) {
    EditBone *ebone = edit_bones[i];

    if (!ebone) {
      continue;
    }

    pxr::GfMatrix4f mat(bind_xforms[i]);

    float mat4[4][4];
    mat.Get(mat4);

    pxr::GfVec3f head(0.0f, 0.0f, 0.0f);
    pxr::GfVec3f tail(0.0f, 1.0f, 0.0f);

    copy_v3_v3(ebone->head, head.data());
    copy_v3_v3(ebone->tail, tail.data());

    ED_armature_ebone_from_mat4(ebone, mat4);

    if (mat.GetDeterminant() < 0.0) {
      negative_determinant = true;
    }
  }

  bool valid_skeleton = true;
  if (negative_determinant) {
    valid_skeleton = false;
    BKE_reportf(
        reports,
        RPT_WARNING,
        "USD Skeleton Import: bone matrices with negative determinants detected in prim %s. "
        "Such matrices may indicate negative scales, possibly due to mirroring operations, "
        "and can't currently be converted to Blender's bone representation. "
        "The skeletal animation won't be imported",
        skel.GetPath().GetAsString().c_str());
  }

  /* Set bone parenting.  In addition, scale bones to account
   * for separation between parents and children, so that the
   * bone size is in proportion with the overall skeleton hierarchy.
   * USD skeletons are composed of joints which we imperfectly
   * represent as bones. */

  /* This will record the child bone indices per parent bone,
   * to simplify accessing children when computing lengths. */
  blender::Vector<blender::Vector<int>> child_bones(num_joints);

  for (size_t i = 0; i < num_joints; ++i) {
    const int parent_idx = skel_topology.GetParent(i);
    if (parent_idx < 0) {
      continue;
    }
    if (parent_idx >= edit_bones.size()) {
      CLOG_WARN(&LOG,
                "Out of bounds parent index for bone %s on skeleton %s",
                pxr::SdfPath(joint_order[i]).GetAsString().c_str(),
                skel.GetPath().GetAsString().c_str());
      continue;
    }

    child_bones[parent_idx].append(i);
    if (edit_bones[i] && edit_bones[parent_idx]) {
      edit_bones[i]->parent = edit_bones[parent_idx];
    }
  }

  float avg_len_scale = 0;
  for (size_t i = 0; i < num_joints; ++i) {

    /* If the bone has any children, scale its length
     * by the distance between this bone's head
     * and the average head location of its children. */

    if (child_bones[i].is_empty()) {
      continue;
    }

    EditBone *parent = edit_bones[i];
    if (!parent) {
      continue;
    }

    pxr::GfVec3f avg_child_head(0);
    for (int j : child_bones[i]) {
      EditBone *child = edit_bones[j];
      if (!child) {
        continue;
      }
      pxr::GfVec3f child_head(child->head);
      avg_child_head += child_head;
    }

    avg_child_head /= child_bones[i].size();

    pxr::GfVec3f parent_head(parent->head);
    pxr::GfVec3f parent_tail(parent->tail);

    const float new_len = (avg_child_head - parent_head).GetLength();

    /* Check for epsilon relative to the parent head before scaling. */
    if (new_len > .00001 * max_mag_component(parent_head)) {
      parent_tail = parent_head + (parent_tail - parent_head).GetNormalized() * new_len;
      copy_v3_v3(parent->tail, parent_tail.data());
      avg_len_scale += new_len;
    }
  }

  /* Scale terminal bones by the average length scale. */
  avg_len_scale /= num_joints;

  for (size_t i = 0; i < num_joints; ++i) {
    if (!child_bones[i].is_empty()) {
      /* Not a terminal bone. */
      continue;
    }
    EditBone *bone = edit_bones[i];
    if (!bone) {
      continue;
    }
    pxr::GfVec3f head(bone->head);

    /* Check for epsilon relative to the head before scaling. */
    if (avg_len_scale > .00001 * max_mag_component(head)) {
      pxr::GfVec3f tail(bone->tail);
      tail = head + (tail - head).GetNormalized() * avg_len_scale;
      copy_v3_v3(bone->tail, tail.data());
    }
  }

  /* Get out of edit mode. */
  ED_armature_from_edit(bmain, arm);
  ED_armature_edit_free(arm);

  if (import_anim && valid_skeleton) {
    import_skeleton_curves(bmain, arm_obj, skel_query, joint_to_bone_map, reports);
  }
}

void import_mesh_skel_bindings(Main *bmain,
                               Object *mesh_obj,
                               const pxr::UsdPrim &prim,
                               ReportList *reports)
{
  if (!(bmain && mesh_obj && mesh_obj->type == OB_MESH && prim)) {
    return;
  }

  if (prim.IsInstanceProxy()) {
    /* Attempting to create a UsdSkelBindingAPI for
     * instance proxies generates USD errors. */
    return;
  }

  pxr::UsdSkelBindingAPI skel_api(prim);

  pxr::UsdSkelSkeleton skel = skel_api.GetInheritedSkeleton();

  if (!skel) {
    return;
  }

  /* Get the joint identifiers from the skeleton. We will
   * need these to construct deform groups. */
  pxr::VtArray<pxr::TfToken> joints;

  if (skel_api.GetJointsAttr().HasAuthoredValue()) {
    skel_api.GetJointsAttr().Get(&joints);
  }
  else if (skel.GetJointsAttr().HasAuthoredValue()) {
    skel.GetJointsAttr().Get(&joints);
  }

  if (joints.empty()) {
    return;
  }

  /* Get the joint indices, which specify which joints influence a given point. */
  pxr::UsdGeomPrimvar joint_indices_primvar = skel_api.GetJointIndicesPrimvar();
  if (!(joint_indices_primvar && joint_indices_primvar.HasAuthoredValue())) {
    return;
  }

  /* Get the weights, which specify the weight of a joint on a given point. */
  pxr::UsdGeomPrimvar joint_weights_primvar = skel_api.GetJointWeightsPrimvar();
  if (!(joint_weights_primvar && joint_weights_primvar.HasAuthoredValue())) {
    return;
  }

  /* Element size specifies the number of joints that might influence a given point.
   * This is the stride we take when accessing the indices and weights for a given point. */
  int joint_indices_elem_size = joint_indices_primvar.GetElementSize();
  int joint_weights_elem_size = joint_weights_primvar.GetElementSize();

  /* We expect the element counts to match. */
  if (joint_indices_elem_size != joint_weights_elem_size) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Joint weights and joint indices element size mismatch for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  /* Get the joint indices and weights. */
  pxr::VtIntArray joint_indices;
  joint_indices_primvar.ComputeFlattened(&joint_indices);

  pxr::VtFloatArray joint_weights;
  joint_weights_primvar.ComputeFlattened(&joint_weights);

  if (joint_indices.empty() || joint_weights.empty()) {
    return;
  }

  if (joint_indices.size() != joint_weights.size()) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Joint weights and joint indices size mismatch for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  Mesh *mesh = static_cast<Mesh *>(mesh_obj->data);

  const pxr::TfToken interp = joint_weights_primvar.GetInterpolation();

  /* Sanity check: we expect only vertex or constant interpolation. */
  if (!ELEM(interp, pxr::UsdGeomTokens->vertex, pxr::UsdGeomTokens->constant)) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Unexpected joint weights interpolation type %s for prim %s",
                __func__,
                interp.GetString().c_str(),
                prim.GetPath().GetAsString().c_str());
    return;
  }

  /* Sanity check: make sure we have the expected number of values for the interpolation type. */
  if (interp == pxr::UsdGeomTokens->vertex &&
      joint_weights.size() != mesh->verts_num * joint_weights_elem_size)
  {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Joint weights of unexpected size for vertex interpolation for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  if (interp == pxr::UsdGeomTokens->constant && joint_weights.size() != joint_weights_elem_size) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Joint weights of unexpected size for constant interpolation for prim %s",
                __func__,
                prim.GetPath().GetAsString().c_str());
    return;
  }

  /* Determine which joint indices are used for skinning this prim. */
  blender::Vector<int> used_indices;
  for (int index : joint_indices) {
    if (std::find(used_indices.begin(), used_indices.end(), index) == used_indices.end()) {
      /* We haven't accounted for this index yet. */
      if (index < 0 || index >= joints.size()) {
        CLOG_WARN(&LOG, "Out of bound joint index %d", index);
        continue;
      }
      used_indices.append(index);
    }
  }

  if (used_indices.is_empty()) {
    return;
  }

  if (BKE_object_defgroup_data_create(static_cast<ID *>(mesh_obj->data)) == nullptr) {
    BKE_reportf(reports,
                RPT_WARNING,
                "%s: Error creating deform group data for mesh %s",
                __func__,
                mesh_obj->id.name + 2);
    return;
  }

  /* Add the armature modifier, if one doesn't exist. */
  if (!BKE_modifiers_findby_type(mesh_obj, eModifierType_Armature)) {
    ModifierData *md = BKE_modifier_new(eModifierType_Armature);
    BLI_addtail(&mesh_obj->modifiers, md);
    BKE_modifiers_persistent_uid_init(*mesh_obj, *md);
  }

  /* Create a deform group per joint. */
  blender::Vector<bDeformGroup *> joint_def_grps(joints.size(), nullptr);

  for (int idx : used_indices) {
    std::string joint_name = pxr::SdfPath(joints[idx]).GetName();
    if (!BKE_object_defgroup_find_name(mesh_obj, joint_name.c_str())) {
      bDeformGroup *def_grp = BKE_object_defgroup_add_name(mesh_obj, joint_name.c_str());
      joint_def_grps[idx] = def_grp;
    }
  }

  /* Set the deform group verts and weights. */
  for (int i = 0; i < mesh->verts_num; ++i) {
    /* Offset into the weights array, which is
     * always 0 for constant interpolation. */
    int offset = 0;
    if (interp == pxr::UsdGeomTokens->vertex) {
      offset = i * joint_weights_elem_size;
    }
    for (int j = 0; j < joint_weights_elem_size; ++j) {
      const int k = offset + j;
      const float w = joint_weights[k];
      if (w < .00001) {
        /* No deform group if zero weight. */
        continue;
      }
      const int joint_idx = joint_indices[k];
      if (bDeformGroup *def_grp = joint_def_grps[joint_idx]) {
        blender::ed::object::vgroup_vert_add(mesh_obj, def_grp, i, w, WEIGHT_REPLACE);
      }
    }
  }
}

void skel_export_chaser(pxr::UsdStageRefPtr stage,
                        const ObjExportMap &armature_export_map,
                        const ObjExportMap &skinned_mesh_export_map,
                        const ObjExportMap &shape_key_mesh_export_map,
                        const Depsgraph *depsgraph)
{
  /* We may need to compute the world transforms of certain primitives when
   * setting skinning data.  Using a shared transform cache can make computing
   * the transforms more efficient. */
  pxr::UsdGeomXformCache xf_cache(1.0);
  skinned_mesh_export_chaser(
      stage, armature_export_map, skinned_mesh_export_map, xf_cache, depsgraph);
  shape_key_export_chaser(stage, shape_key_mesh_export_map);
}

void skinned_mesh_export_chaser(pxr::UsdStageRefPtr stage,
                                const ObjExportMap &armature_export_map,
                                const ObjExportMap &skinned_mesh_export_map,
                                pxr::UsdGeomXformCache &xf_cache,
                                const Depsgraph *depsgraph)
{
  /* Finish creating skinned mesh bindings. */
  for (const auto &item : skinned_mesh_export_map.items()) {
    const Object *mesh_obj = item.key;
    const pxr::SdfPath &mesh_path = item.value;

    /* Get the mesh prim from the stage. */
    pxr::UsdPrim mesh_prim = stage->GetPrimAtPath(mesh_path);
    if (!mesh_prim) {
      CLOG_WARN(&LOG,
                "Invalid export map prim path %s for mesh object %s",
                mesh_path.GetAsString().c_str(),
                mesh_obj->id.name + 2);
      continue;
    }

    /* Get the armature bound to the mesh's armature modifier. */
    const Object *arm_obj = get_armature_modifier_obj(*mesh_obj, depsgraph);
    if (!arm_obj) {
      CLOG_WARN(&LOG, "Invalid armature modifier for skinned mesh %s", mesh_obj->id.name + 2);
      continue;
    }
    /* Look up the USD skeleton corresponding to the armature object. */
    const pxr::SdfPath *path = armature_export_map.lookup_ptr(arm_obj);
    if (!path) {
      CLOG_WARN(&LOG, "No export map entry for armature object %s", mesh_obj->id.name + 2);
      continue;
    }
    /* Get the skeleton prim. */
    pxr::UsdPrim skel_prim = stage->GetPrimAtPath(*path);
    pxr::UsdSkelSkeleton skel(skel_prim);
    if (!skel) {
      CLOG_WARN(&LOG, "Invalid USD skeleton for armature object %s", arm_obj->id.name + 2);
      continue;
    }

    add_skinned_mesh_bindings(skel, mesh_prim, xf_cache);
  }
}

void shape_key_export_chaser(pxr::UsdStageRefPtr stage,
                             const ObjExportMap &shape_key_mesh_export_map)
{
  Map<pxr::SdfPath, pxr::SdfPathSet> skel_to_mesh;

  /* We will keep track of the mesh primitives to clean up the temporary
   * weights attribute at the end. */
  Vector<pxr::UsdPrim> mesh_prims;

  /* Finish creating blend shape bindings. */
  for (const auto &item : shape_key_mesh_export_map.items()) {
    const Object *mesh_obj = item.key;
    const pxr::SdfPath &mesh_path = item.value;

    /* Get the mesh prim from the stage. */
    pxr::UsdPrim mesh_prim = stage->GetPrimAtPath(mesh_path);
    if (!mesh_prim) {
      CLOG_WARN(&LOG,
                "Invalid export map prim path %s for mesh object %s",
                mesh_path.GetAsString().c_str(),
                mesh_obj->id.name + 2);
      continue;
    }

    /* Keep track of all the mesh primitives with blend shapes, for cleanup below. */
    mesh_prims.append(mesh_prim);

    pxr::UsdSkelBindingAPI skel_api = pxr::UsdSkelBindingAPI::Apply(mesh_prim);

    if (!skel_api) {
      CLOG_WARN(&LOG,
                "Couldn't apply UsdSkelBindingAPI to prim %s",
                mesh_prim.GetPath().GetAsString().c_str());
      return;
    }

    pxr::UsdSkelSkeleton skel;
    if (skel_api.GetSkeleton(&skel)) {
      /* We have a bound skeleton, so we add it to the map. */
      pxr::SdfPathSet *mesh_paths = skel_to_mesh.lookup_ptr(skel.GetPath());
      if (!mesh_paths) {
        skel_to_mesh.add_new(skel.GetPath(), pxr::SdfPathSet());
        mesh_paths = skel_to_mesh.lookup_ptr(skel.GetPath());
      }
      if (mesh_paths) {
        mesh_paths->insert(mesh_prim.GetPath());
      }
      continue;
    }

    /* The mesh is not bound to a skeleton, so we must create one for it. */
    ensure_blend_shape_skeleton(stage, mesh_prim);
  }

  if (skel_to_mesh.is_empty()) {
    return;
  }

  for (const auto &item : skel_to_mesh.items()) {
    remap_blend_shape_anim(stage, item.key, item.value);
  }

  /* Finally, delete the temp blendshape weights attributes. */
  for (pxr::UsdPrim &prim : mesh_prims) {
    pxr::UsdGeomPrimvarsAPI(prim).RemovePrimvar(TempBlendShapeWeightsPrimvarName);
  }
}

void export_deform_verts(const Mesh *mesh,
                         const pxr::UsdSkelBindingAPI &skel_api,
                         const Span<std::string> bone_names)
{
  BLI_assert(mesh);
  BLI_assert(skel_api);

  /* Map a deform vertex group index to the
   * index of the corresponding joint.  I.e.,
   * joint_index[n] is the joint index of the
   * n-th vertex group. */
  Vector<int> joint_index;

  /* Build the index mapping. */
  LISTBASE_FOREACH (const bDeformGroup *, def, &mesh->vertex_group_names) {
    int bone_idx = -1;
    /* For now, n-squared search is acceptable. */
    for (int i = 0; i < bone_names.size(); ++i) {
      if (bone_names[i] == def->name) {
        bone_idx = i;
        break;
      }
    }

    joint_index.append(bone_idx);
  }

  if (joint_index.is_empty()) {
    return;
  }

  const Span<MDeformVert> dverts = mesh->deform_verts();

  int max_totweight = 1;
  for (const int i : dverts.index_range()) {
    const MDeformVert &vert = dverts[i];
    if (vert.totweight > max_totweight) {
      max_totweight = vert.totweight;
    }
  }

  /* elem_size will specify the number of
   * joints that can influence a given point. */
  const int element_size = max_totweight;
  int num_points = mesh->verts_num;

  pxr::VtArray<int> joint_indices(num_points * element_size, 0);
  pxr::VtArray<float> joint_weights(num_points * element_size, 0.0f);

  /* Current offset into the indices and weights arrays. */
  int offset = 0;

  for (const int i : dverts.index_range()) {
    const MDeformVert &vert = dverts[i];

    for (int j = 0; j < element_size; ++j, ++offset) {

      if (offset >= joint_indices.size()) {
        BLI_assert_unreachable();
        return;
      }

      if (j >= vert.totweight) {
        continue;
      }

      int def_nr = int(vert.dw[j].def_nr);

      if (def_nr >= joint_index.size()) {
        BLI_assert_unreachable();
        continue;
      }

      if (joint_index[def_nr] == -1) {
        continue;
      }

      joint_indices[offset] = joint_index[def_nr];
      joint_weights[offset] = vert.dw[j].weight;
    }
  }

  pxr::UsdSkelNormalizeWeights(joint_weights, element_size);

  skel_api.CreateJointIndicesPrimvar(false, element_size).GetAttr().Set(joint_indices);
  skel_api.CreateJointWeightsPrimvar(false, element_size).GetAttr().Set(joint_weights);
}

}  // namespace blender::io::usd
