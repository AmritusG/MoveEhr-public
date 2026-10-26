# MoveEhr W2 A4: a CC0 walk WITH root travel (none of the three CC0 packs' walks travel: Knight / Witch / Zombie Walk move their
# centroid <= 0.08 fitted over a cycle; measured). Headless Blender: the CC0 Knight_Golden_Male.fbx (Quaternius, CC0), its Walk action
# only, + the armature object keyed forward 1.6 m per cycle (linear) -> test_models/cc0/derived/knight_walk_travel.fbx (gitignored).
# usage: /Applications/Blender.app/Contents/MacOS/Blender -b -P tools/make_root_walk.py -- <src.fbx> <dst.fbx>
import bpy, sys
src, dst = sys.argv[sys.argv.index('--') + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
walk = [a for a in bpy.data.actions if a.name.split('|')[-1] == 'Walk'][0]
for a in list(bpy.data.actions):
    if a != walk:
        bpy.data.actions.remove(a)
arm.animation_data.action = walk
f0, f1 = walk.frame_range
sc = bpy.context.scene
sc.frame_start, sc.frame_end = int(f0), int(f1)
base = arm.location.copy()
bpy.context.preferences.edit.keyframe_new_interpolation_type = 'LINEAR'  # Blender 4.4+ layered actions: set before keying
for f, d in ((f0, 0.0), (f1, 1.6)):
    arm.location = (base.x, base.y - d, base.z)  # Blender -Y = the character's forward on export with axis_forward -Z
    arm.keyframe_insert('location', frame=f)
print('WALK action', walk.name, 'frames', f0, f1)
bpy.ops.export_scene.fbx(filepath=dst, use_selection=False, bake_anim=True, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False,
                         add_leaf_bones=False, object_types={'ARMATURE', 'MESH', 'EMPTY'})
