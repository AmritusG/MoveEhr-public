cmake_minimum_required(VERSION 3.20)
project(MoveEhr C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)

set(TP "${CMAKE_SOURCE_DIR}/third_party")
set(FFGL_LIB "${TP}/ffgl/lib")
file(GLOB FFGL_SOURCES "${FFGL_LIB}/ffgl/*.cpp" "${FFGL_LIB}/ffglex/*.cpp")

# Vendored single-file libraries, compiled once. ufbx is C.
add_library(loadehr_thirdparty STATIC
    "${TP}/ufbx/ufbx.c"
    "${TP}/meshoptimizer/simplifier.cpp"
    "${TP}/meshoptimizer/allocator.cpp"
    "${TP}/clipper2/src/clipper.engine.cpp"
    "${TP}/clipper2/src/clipper.offset.cpp"
    src/ThirdPartyImpl.cpp
)
target_include_directories(loadehr_thirdparty PUBLIC "${TP}/ufbx" "${TP}/cgltf" "${TP}/stb" "${TP}/meshoptimizer" "${TP}/clipper2/include" "${TP}/earcut" "${TP}/nanosvg")
set_target_properties(loadehr_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(loadehr_thirdparty PRIVATE -O3 -w)

# W3: DXVKit (our clean-room MIT DXV decoder), a pinned submodule: the C core only, static, universal (no tools, no Swift).
set(DXVK_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/dxvkit" dxvkit EXCLUDE_FROM_ALL)
set_target_properties(dxvkit PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Everything that does not depend on the FFGL SDK: loader, GPU model, renderer.
add_library(loadehr_core STATIC
    src/Log.cpp
    src/Perf.cpp
    src/ModelLoader.cpp
    src/Anim.cpp
    src/LoadJob.cpp
    src/GpuModel.cpp
    src/Renderer.cpp
    src/Pose.cpp
    src/EnvFile.cpp
    src/TestModel.cpp
    src/Voxel.cpp
    src/Volume.cpp
    src/Sim.cpp
    src/Wow.cpp
    src/Projector.cpp
    src/Liquid.cpp
    src/Wire.cpp
    src/Points.cpp
    src/Lines.cpp
    src/Build.cpp
    src/Surface.cpp
    src/Morph.cpp
    src/Lightning.cpp
    src/Swarm.cpp
    src/Crack.cpp
    src/Caustic.cpp
    src/SdfBuilder.cpp
    src/Post.cpp
    src/Shape.cpp
    src/EyeFinder.cpp
    src/Generate.cpp
    src/Video.mm
)
set_source_files_properties(src/Video.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
target_include_directories(loadehr_core PUBLIC src "${FFGL_LIB}")
target_link_libraries(loadehr_core PUBLIC loadehr_thirdparty dxvkit "-framework OpenGL" "-framework CoreText" "-framework CoreGraphics" "-framework CoreFoundation"
    "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework IOSurface" "-framework Foundation")
target_compile_definitions(loadehr_core PUBLIC GL_SILENCE_DEPRECATION)
set_target_properties(loadehr_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(ARENA_EFFECTS "$ENV{HOME}/Documents/Resolume Arena/Extra Effects")

function(loadehr_bundle NAME PLIST FX WOW)
    set(EXTRA "")
    if(FX)
        # face tracking (Apple Vision) exists in the FX build only
        set(EXTRA src/FaceTracker.mm)
    endif()
    add_library(${NAME} MODULE src/LoadEhrPlugin.cpp ${EXTRA} ${FFGL_SOURCES})
    if(FX)
        set_source_files_properties(src/FaceTracker.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(${NAME} PRIVATE "-framework Vision" "-framework CoreGraphics" "-framework Foundation")
    endif()
    target_include_directories(${NAME} PRIVATE src "${FFGL_LIB}" "${FFGL_LIB}/ffgl")
    target_link_libraries(${NAME} PRIVATE loadehr_core)
    target_compile_definitions(${NAME} PRIVATE LOADEHR_FX=${FX} LOADEHR_WOW=${WOW})
    set_target_properties(${NAME} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/${PLIST}"
        OUTPUT_NAME "${NAME}"
    )
endfunction()

# MoveEhr W1: the four MoveEhr bundles (IDs MVhr / MVfx / MVwo / MVwf). Internal target / file names keep LoadEhr's
# (loadehr_core, LoadEhrPlugin.cpp, loadehr_harness) so LoadEhr fixes cherry-pick cleanly; only the shipped identity changes.
loadehr_bundle(MoveEhr      bundle/MoveEhr-Info.plist      0 0)
loadehr_bundle(MoveEhrFX    bundle/MoveEhrFX-Info.plist    1 0)
# W26: Resolume shows at most 512 FFGL params, so the W26 showpieces (+ Spots, Silhouette) live in two more bundles
loadehr_bundle(MoveEhrWow   bundle/MoveEhrWow-Info.plist   0 1)
loadehr_bundle(MoveEhrWowFX bundle/MoveEhrWowFX-Info.plist 1 1)
# Arena shows the plugin name from the FFGL info block, not the bundle name.

# Offline harness: headless CGL 4.1 host that dlopens a bundle and renders PNGs.
add_executable(loadehr_harness tools/harness/harness.cpp)
target_include_directories(loadehr_harness PRIVATE "${FFGL_LIB}/ffgl" "${TP}/stb")
target_link_libraries(loadehr_harness PRIVATE "-framework OpenGL")
target_compile_definitions(loadehr_harness PRIVATE GL_SILENCE_DEPRECATION)

# W11 diagnostic: how Arena presents FFGL colour param layouts (not installed by the normal steps).
add_library(LoadEhrColorProbe MODULE tools/probe/ColorProbe.cpp ${FFGL_SOURCES})
target_include_directories(LoadEhrColorProbe PRIVATE "${FFGL_LIB}" "${FFGL_LIB}/ffgl")
target_compile_definitions(LoadEhrColorProbe PRIVATE GL_SILENCE_DEPRECATION)
target_link_libraries(LoadEhrColorProbe PRIVATE "-framework OpenGL")
set_target_properties(LoadEhrColorProbe PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "bundle"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/tools/probe/Info.plist" OUTPUT_NAME "LoadEhrColorProbe")

# W21 diagnostic: which FFGL text params get Arena's multi-line editor (not installed by the normal steps).
add_library(LoadEhrTextProbe MODULE tools/probe/TextProbe.cpp ${FFGL_SOURCES})
target_include_directories(LoadEhrTextProbe PRIVATE "${FFGL_LIB}" "${FFGL_LIB}/ffgl")
target_compile_definitions(LoadEhrTextProbe PRIVATE GL_SILENCE_DEPRECATION)
target_link_libraries(LoadEhrTextProbe PRIVATE "-framework OpenGL")
set_target_properties(LoadEhrTextProbe PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION "bundle"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/tools/probe/TextProbe.plist" OUTPUT_NAME "LoadEhrTextProbe")

# MoveEhr W1: the loader probe (ModelData fingerprints, CPU poses, ufbx reference poses; not installed)
add_executable(moveehr_animprobe tools/probe/AnimProbe.cpp)
target_link_libraries(moveehr_animprobe PRIVATE loadehr_core)
