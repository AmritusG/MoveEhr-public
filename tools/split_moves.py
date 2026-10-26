# MoveEhr W8 C: one movement per file, the way many rigs ship (one character file + Walk.fbx, Run.fbx, ...). Headless Blender splits a
# CC0 Quaternius character (Ultimate Animated Character Pack, CC0 per its OpenGameArt page, models/CREDITS.md)
# into <dst>/<Name>.fbx = the mesh + armature with NO animation, and <dst>/<Action>.fbx = the same mesh + armature with that action
# only. Every file comes out of the same Blender import, so the bone names are the same in all of them. models/moves/ was made this way from Knight_Male.fbx.
# usage: /Applications/Blender.app/Contents/MacOS/Blender -b -P tools/split_moves.py -- <src.fbx> <dst dir> <Name> [--glb] [Action ...]
#        (no actions listed = every action of the file; --glb writes .glb files instead: the glTF path of the matching)
import bpy, os, sys
args = sys.argv[sys.argv.index('--') + 1:]
src, dst, name, only = args[0], args[1], args[2], args[3:]
glb = '--glb' in only
only = [a for a in only if a != '--glb']
os.makedirs(dst, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
acts = {a.name.split('|')[-1]: a for a in bpy.data.actions}
want = only or sorted(acts)
opts = dict(use_selection=False, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, add_leaf_bones=False,
            object_types={'ARMATURE', 'MESH', 'EMPTY'})


def export(path, anim):
    if glb:
        bpy.ops.export_scene.gltf(filepath=path + '.glb', export_format='GLB', export_animations=anim, export_animation_mode='ACTIVE_ACTIONS',
                                  export_force_sampling=True)
    else:
        bpy.ops.export_scene.fbx(filepath=path + '.fbx', bake_anim=anim, **opts)


if arm.animation_data:
    arm.animation_data.action = None
export(os.path.join(dst, name), False)
print('SPLIT character', name, '(no animation)')
sc = bpy.context.scene
for a in want:
    act = acts[a]
    if not arm.animation_data:
        arm.animation_data_create()
    arm.animation_data.action = act
    f0, f1 = act.frame_range
    sc.frame_start, sc.frame_end = int(f0), int(f1)
    export(os.path.join(dst, a), True)
    print('SPLIT action', a, 'frames', f0, f1)
