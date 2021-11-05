#include "cloud.h"

#include <fstream>
#include <memory>
#include <stack>
#include <thread>

#include "bvh.h"
#include "cloud/manager.h"
#include "core/parallel.h"
#include "core/paramset.h"
#include "core/primitive.h"
#include "lights/diffuse.h"
#include "materials/matte.h"
#include "messages/lite.h"
#include "messages/serdes.h"
#include "messages/serialization.h"
#include "messages/utils.h"
#include "pbrt.pb.h"
#include "shapes/triangle.h"

using namespace std;

namespace pbrt {

static auto &_manager = global::manager;

STAT_COUNTER("BVH/Total nodes", nNodes);
STAT_COUNTER("BVH/Visited nodes", nNodesVisited);
STAT_COUNTER("BVH/Visited primitives", nPrimitivesVisited);

struct membuf : streambuf {
    membuf(char *begin, char *end) { this->setg(begin, begin, end); }
};

CloudBVH::CloudBVH(const uint32_t bvh_root, const bool preload_all,
                   const bool load_materials)
    : bvh_root_(bvh_root), load_materials_(load_materials) {
    ProfilePhase _(Prof::AccelConstruction);

    if (MaxThreadIndex() > 1 && !preload_all) {
        throw runtime_error(
            "Cannot use lazy-loading CloudBVH with multiple threads");
    }

    // let's load all the area lights in the case that they are used by our
    // meshes
    auto reader = _manager.GetReader(ObjectType::AreaLights);
    while (!reader->eof()) {
        protobuf::AreaLight proto;
        reader->read(&proto);
        area_light_params_.emplace(
            proto.id(),
            make_pair(from_protobuf(proto.light().paramset()),
                      from_protobuf(proto.light().light_to_world())));
    }

    if (preload_all && !load_materials_) {
        Error("CloudBVH: load_materials is always active when preloading");
    }

    if (preload_all) {
        /* (1) load all the treelets in parallel */
        const auto treelet_count = _manager.treeletCount();

        treelets_.resize(treelet_count + 1);

        ParallelFor([&](int64_t treelet_id) { loadTreeletBase(treelet_id); },
                    treelet_count);

        /* (2.A) load all the necessary materials */
        set<MaterialKey> required_materials;

        for (size_t i = 0; i < treelet_count; i++) {
            required_materials.insert(treelets_[i]->required_materials.begin(),
                                      treelets_[i]->required_materials.end());
        }

        for (const auto mkey : required_materials) {
            if (not mkey.id) {
                materials_[mkey.id] = nullptr;
                continue;
            }

            materials_[mkey.id] =
                treelets_[mkey.treelet]->included_material[mkey.id];
        }

        /* (2.B) create all the necessary external instances */
        set<uint64_t> required_instances;

        for (size_t i = 0; i < treelet_count; i++) {
            required_instances.insert(treelets_[i]->required_instances.begin(),
                                      treelets_[i]->required_instances.end());
        }

        for (const auto rid : required_instances) {
            if (not bvh_instances_.count(rid)) {
                bvh_instances_[rid] =
                    make_shared<ExternalInstance>(*this, (uint16_t)(rid >> 32));
            }
        }

        /* (3) finish loading the treelets */
        ParallelFor(
            [&](int64_t treelet_id) { finalizeTreeletLoad(treelet_id); },
            treelet_count);

        preloading_done_ = true;
    }
}

CloudBVH::~CloudBVH() {}

shared_ptr<Material> CloudBVH::GetMaterial(const uint32_t material_id) const {
    if (not material_id) return nullptr;
    return treelets_.at(bvh_root_)->included_material.at(material_id);
}

Bounds3f CloudBVH::WorldBound() const {
    // The correctness of this function is only guaranteed for the root treelet
    CHECK_EQ(bvh_root_, 0);

    LoadTreelet(bvh_root_);
    return treelets_[bvh_root_]->nodes[0].bounds;
}

// Sums the full surface area for each root. Does not account for overlap
// between roots
Float CloudBVH::RootSurfaceAreas(Transform txfm) const {
    LoadTreelet(bvh_root_);
    CHECK_EQ(treelets_.size(), 1);

    Float area = 0;

    vector<Bounds3f> roots;

    for (const TreeletNode &node : treelets_[bvh_root_]->nodes) {
        auto cur = txfm(node.bounds);

        bool newRoot = true;
        for (const Bounds3f &root : roots) {
            auto u = Union(root, cur);
            if (u == root) {
                newRoot = false;
                break;
            }
        }

        if (newRoot) {
            roots.push_back(cur);
            area += cur.SurfaceArea();
        }
    }

    return area;
}

Float CloudBVH::SurfaceAreaUnion() const {
    LoadTreelet(bvh_root_);
    CHECK_EQ(treelets_.size(), 1);

    Bounds3f boundUnion;
    for (const TreeletNode &node : treelets_[bvh_root_]->nodes) {
        boundUnion = Union(boundUnion, node.bounds);
    }

    return boundUnion.SurfaceArea();
}

void CloudBVH::LoadTreelet(const uint32_t root_id, const char *buffer,
                           const size_t length) const {
    if (preloading_done_ or
        (treelets_.size() > root_id && treelets_[root_id] != nullptr)) {
        return; /* this tree is already loaded */
    }

    if (treelets_.size() <= root_id) {
        treelets_.resize(root_id + 1);
    }

    loadTreeletBase(root_id, buffer, length);

    auto &treelet = *treelets_[root_id];

    /* create the placeholder materials */
    for (const auto mkey : treelet.required_materials) {
        materials_[mkey.id] = make_shared<PlaceholderMaterial>(mkey);
    }

    /* create the instances */
    for (const auto rid : treelet.required_instances) {
        if (not bvh_instances_.count(rid)) {
            bvh_instances_[rid] =
                make_shared<ExternalInstance>(*this, (uint16_t)(rid >> 32));
        }
    }

    finalizeTreeletLoad(root_id);
}

void CloudBVH::finalizeTreeletLoad(const uint32_t root_id) const {
    auto &treelet = *treelets_[root_id];

    /* fill in unfinished primitives */
    for (auto &u : treelet.unfinished_transformed) {
        treelet.primitives[u.primitive_index] =
            make_unique<TransformedPrimitive>(bvh_instances_[u.instance_ref],
                                              move(u.primitive_to_world));
    }

    MediumInterface medium_interface{};

    for (auto &u : treelet.unfinished_geometric) {
        /* do we need to make an area light for this guy? */
        shared_ptr<AreaLight> area_light;

        if (u.area_light_id) {
            auto &light_data = area_light_params_.at(u.area_light_id);
            area_light = CreateDiffuseAreaLight(light_data.second,
                                                medium_interface.outside,
                                                light_data.first, u.shape);
            area_light->SetID(u.area_light_id + u.triangle_idx);
        }

        treelet.primitives[u.primitive_index] = make_unique<GeometricPrimitive>(
            move(u.shape), materials_[u.material_key.id], area_light,
            medium_interface);
    }

    treelet.required_instances.clear();
    treelet.required_materials.clear();
    treelet.unfinished_geometric.clear();
    treelet.unfinished_transformed.clear();
}

void CloudBVH::loadTreeletBase(const uint32_t root_id, const char *buffer,
                               size_t length) const {
    ProfilePhase _(Prof::LoadTreelet);

    treelets_[root_id] = make_unique<Treelet>();

    auto &treelet = *treelets_[root_id];
    auto &nodes = treelet.nodes;
    auto &tree_meshes = treelet.meshes;
    auto &tree_primitives = treelet.primitives;
    auto &tree_transforms = treelet.transforms;
    auto &tree_instances = treelet.instances;

    vector<char> treelet_buffer;
    if (!buffer) {
        const string treelet_path =
            _manager.getScenePath() + "/" +
            _manager.getFileName(ObjectType::Treelet, root_id);

        ifstream fin{treelet_path, ios::binary | ios::ate};

        if (!fin.good()) {
            throw runtime_error("Could not open treelet file: " + treelet_path);
        }

        streamsize size = fin.tellg();
        fin.seekg(0, ios::beg);

        treelet_buffer.resize(size);
        fin.read(treelet_buffer.data(), size);

        buffer = treelet_buffer.data();
        length = treelet_buffer.size();
    }

    LiteRecordReader reader{buffer, length};

    /* read in the textures & materials included in this treelet */
    uint32_t included_texture_count = 0;
    reader.read(&included_texture_count);

    // PTEX TEXTURES
    for (size_t i = 0; i < included_texture_count; i++) {
        uint32_t id;
        reader.read(&id);

        const char *data;
        size_t len;
        reader.read(&data, &len);

        shared_ptr<char> storage{new char[len], default_delete<char[]>()};
        memcpy(storage.get(), data, len);

        _manager.addInMemoryTexture(
            _manager.getFileName(ObjectType::Texture, id), move(storage), len);
    }

    std::map<uint64_t, std::shared_ptr<Texture<Float>>> ftexes;
    std::map<uint64_t, std::shared_ptr<Texture<Spectrum>>> stexes;

    // SPECTRUM TEXTURES
    uint32_t included_spectrum_count = 0;
    reader.read(&included_spectrum_count);

    for (size_t i = 0; i < included_spectrum_count; i++) {
        uint32_t id;
        reader.read(&id);

        const char *data;
        size_t len;
        reader.read(&data, &len);

        protobuf::SpectrumTexture stex_proto;
        stex_proto.ParseFromArray(data, len);
        stexes.emplace(id, spectrum_texture::from_protobuf(stex_proto));
    }

    // FLOAT TEXTURES
    uint32_t included_float_count = 0;
    reader.read(&included_float_count);

    for (size_t i = 0; i < included_float_count; i++) {
        uint32_t id;
        reader.read(&id);

        const char *data;
        size_t len;
        reader.read(&data, &len);

        protobuf::FloatTexture ftex_proto;
        ftex_proto.ParseFromArray(data, len);
        ftexes.emplace(id, float_texture::from_protobuf(ftex_proto));
    }

    // MATERIALS
    uint32_t included_material_count = 0;
    reader.read(&included_material_count);

    for (size_t i = 0; i < included_material_count; i++) {
        uint32_t id;
        reader.read(&id);

        const char *data;
        size_t len;
        reader.read(&data, &len);

        protobuf::Material material;
        material.ParseFromArray(data, len);

        treelet.included_material.emplace(
            id, material::from_protobuf(material, ftexes, stexes));
    }

    map<uint32_t, MaterialKey> mesh_material_ids;
    map<uint32_t, uint32_t> mesh_area_light_id;

    /* read in the triangle meshes for this treelet */
    uint32_t num_triangle_meshes = 0;
    reader.read(&num_triangle_meshes);

    const char *tm_buff_start = reader.cur();
    const char *tm_buff_end = tm_buff_start;

    // find the start and the end of the buffer for meshes
    for (int i = 0; i < num_triangle_meshes; ++i) {
        uint64_t tm_id;
        MaterialKey material_key;
        uint32_t area_light_id;

        reader.read(&tm_id);
        reader.read(&material_key);
        reader.read(&area_light_id);

        const char *tm_buffer;
        size_t tm_buffer_len;
        reader.read(&tm_buffer, &tm_buffer_len);
        tm_buff_end = tm_buffer + tm_buffer_len;
    }

    {
        const size_t len = static_cast<size_t>(tm_buff_end - tm_buff_start);
        treelet.mesh_storage =
            shared_ptr<char>(new char[len], default_delete<char[]>());
        memcpy(treelet.mesh_storage.get(), tm_buff_start, len);

        char *mesh_storage_start = treelet.mesh_storage.get();

        LiteRecordReader tm_reader{treelet.mesh_storage.get(), len};

        for (int i = 0; i < num_triangle_meshes; i++) {
            uint64_t tm_id;
            MaterialKey material_key;
            uint32_t area_light_id;

            tm_reader.read(&tm_id);
            tm_reader.read(&material_key);
            tm_reader.read(&area_light_id);

            const char *tm_buffer;
            size_t tm_buffer_len;
            tm_reader.read(&tm_buffer, &tm_buffer_len);

            auto p = tree_meshes.emplace(
                tm_id,
                std::make_shared<TriangleMesh>(treelet.mesh_storage,
                                               tm_buffer - mesh_storage_start));

            CHECK_EQ(p.second, true);
            mesh_material_ids[tm_id] = material_key;

            if (area_light_id) {
                mesh_area_light_id[tm_id] = area_light_id;
            }
        }
    }

    uint32_t node_count;
    uint32_t primitive_count;

    reader.read(&node_count);
    reader.read(&primitive_count);

    if (node_count == 0) {
        return;
    }

    nodes.resize(node_count);
    tree_primitives.reserve(primitive_count);

    const char *nodes_buffer;
    size_t nodes_buf_len;

    reader.read(&nodes_buffer, &nodes_buf_len);
    memcpy(&nodes[0], nodes_buffer, nodes_buf_len);

    for (auto &node : nodes) {
        const serdes::cloudbvh::TransformedPrimitive *serdes_primitive;
        const serdes::cloudbvh::Triangle *serdes_triangle;

        uint32_t transformed_primitives_count;
        uint32_t triangles_count;

        reader.read(&transformed_primitives_count);
        reader.read(&triangles_count);

        for (int i = 0; i < transformed_primitives_count; i++) {
            reader.read(reinterpret_cast<const char **>(&serdes_primitive),
                        nullptr);

            tree_transforms.push_back(move(
                make_unique<Transform>(serdes_primitive->start_transform)));
            const Transform *start = tree_transforms.back().get();

            const Transform *end;
            if (start->GetMatrix() != serdes_primitive->end_transform) {
                tree_transforms.push_back(move(
                    make_unique<Transform>(serdes_primitive->end_transform)));
                end = tree_transforms.back().get();
            } else {
                end = start;
            }

            AnimatedTransform primitive_to_world{
                start, serdes_primitive->start_time, end,
                serdes_primitive->end_time};

            uint64_t instance_ref = serdes_primitive->root_ref;

            uint16_t instance_group = (uint16_t)(instance_ref >> 32);
            uint32_t instance_node = (uint32_t)instance_ref;

            if (instance_group == root_id) {
                if (not tree_instances.count(instance_ref)) {
                    tree_instances[instance_ref] =
                        make_shared<IncludedInstance>(&treelet, instance_node);
                }

                tree_primitives.push_back(make_unique<TransformedPrimitive>(
                    tree_instances[instance_ref], primitive_to_world));
            } else {
                treelet.required_instances.insert(instance_ref);

                treelet.unfinished_transformed.emplace_back(
                    tree_primitives.size(), instance_ref,
                    move(primitive_to_world));

                tree_primitives.push_back(nullptr);
            }
        }

        for (int i = 0; i < triangles_count; i++) {
            reader.read(reinterpret_cast<const char **>(&serdes_triangle),
                        nullptr);

            const auto mesh_id = serdes_triangle->mesh_id;
            const auto tri_number = serdes_triangle->tri_number;
            const auto material_key = mesh_material_ids[mesh_id];
            const auto area_light_id = mesh_area_light_id.count(mesh_id)
                                           ? mesh_area_light_id.at(mesh_id)
                                           : 0;

            treelet.required_materials.insert(material_key);

            auto shape = make_shared<Triangle>(
                &identity_transform_, &identity_transform_, false,
                tree_meshes.at(mesh_id), tri_number);

            treelet.unfinished_geometric.emplace_back(
                tree_primitives.size(), material_key, area_light_id,
                move(shape), i);

            tree_primitives.push_back(nullptr);
        }

        nNodes++;
    }
}

void CloudBVH::Trace(RayState &rayState) const {
    SurfaceInteraction isect;

    RayDifferential ray = rayState.ray;
    Vector3f invDir{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
    int dirIsNeg[3] = {invDir.x < 0, invDir.y < 0, invDir.z < 0};

    const uint32_t currentTreelet = rayState.toVisitTop().treelet;
    LoadTreelet(currentTreelet); /* we don't load any other treelets */

    bool hasTransform = false;
    bool transformChanged = false;

    while (true) {
        auto &top = rayState.toVisitTop();
        if (currentTreelet != top.treelet) {
            break;
        }

        RayState::TreeletNode current = move(top);
        rayState.toVisitPop();
        nNodesVisited++;

        auto &treelet = *treelets_[current.treelet];
        auto &node = treelet.nodes[current.node];

        /* prepare the ray */
        if (current.transformed != hasTransform || transformChanged) {
            transformChanged = false;

            ray = current.transformed
                      ? Inverse(rayState.rayTransform)(rayState.ray)
                      : rayState.ray;

            invDir = Vector3f{1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z};
            dirIsNeg[0] = invDir.x < 0;
            dirIsNeg[1] = invDir.y < 0;
            dirIsNeg[2] = invDir.z < 0;
        }

        hasTransform = current.transformed;

        // Check ray against BVH node
        if (node.bounds.IntersectP(ray, invDir, dirIsNeg)) {
            if (node.is_leaf()) {
                auto &primitives = treelet.primitives;

                for (int i = node.primitive_offset + current.primitive;
                     i < node.primitive_offset + node.primitive_count; i++) {
                    nPrimitivesVisited++;

                    if (primitives[i]->GetType() ==
                        PrimitiveType::Transformed) {
                        TransformedPrimitive *tp =
                            dynamic_cast<TransformedPrimitive *>(
                                primitives[i].get());

                        shared_ptr<ExternalInstance> cbvh =
                            dynamic_pointer_cast<ExternalInstance>(
                                tp->GetPrimitive());

                        if (cbvh) {
                            if (current.primitive + 1 < node.primitive_count) {
                                RayState::TreeletNode next_primitive = current;
                                next_primitive.primitive++;
                                rayState.toVisitPush(move(next_primitive));
                            }

                            Transform txfm;
                            tp->GetTransform().Interpolate(ray.time, &txfm);

                            RayState::TreeletNode next;
                            next.treelet = cbvh->RootID();
                            next.node = 0;

                            if (txfm.IsIdentity()) {
                                next.transformed = false;
                            } else {
                                rayState.rayTransform = txfm;
                                next.transformed = true;
                            }
                            rayState.toVisitPush(move(next));
                            break;
                        }

                        shared_ptr<IncludedInstance> included =
                            dynamic_pointer_cast<IncludedInstance>(
                                tp->GetPrimitive());
                        if (included) {
                            if (tp->Intersect(ray, &isect)) {
                                if (isect.primitive->GetMaterial()->GetType() !=
                                    MaterialType::Placeholder) {
                                    throw runtime_error(
                                        "Trace() only works with placeholder "
                                        "material");
                                }

                                const auto mat_key =
                                    dynamic_cast<const PlaceholderMaterial *>(
                                        isect.primitive->GetMaterial())
                                        ->GetMaterialKey();

                                const auto arealight =
                                    isect.primitive->GetAreaLight()
                                        ? isect.primitive->GetAreaLight()
                                              ->GetID()
                                        : 0;

                                rayState.ray.tMax = ray.tMax;
                                rayState.SetHit(current, isect, mat_key,
                                                arealight);
                            }
                        }
                    } else if (primitives[i]->Intersect(ray, &isect)) {
                        if (isect.primitive->GetMaterial()->GetType() !=
                            MaterialType::Placeholder) {
                            throw runtime_error(
                                "Trace() only works with placeholder "
                                "material");
                        }

                        const auto mat_key =
                            dynamic_cast<const PlaceholderMaterial *>(
                                isect.primitive->GetMaterial())
                                ->GetMaterialKey();

                        const auto arealight =
                            isect.primitive->GetAreaLight()
                                ? isect.primitive->GetAreaLight()->GetID()
                                : 0;

                        rayState.ray.tMax = ray.tMax;
                        rayState.SetHit(current, isect, mat_key, arealight);
                    }

                    current.primitive++;
                }

                if (rayState.toVisitEmpty()) break;
            } else {
                RayState::TreeletNode children[2];
                for (int i = 0; i < 2; i++) {
                    children[i].treelet = node.child_treelet[i];
                    children[i].node = node.child_node[i];
                    children[i].transformed = current.transformed;
                }

                if (dirIsNeg[node.axis]) {
                    rayState.toVisitPush(move(children[LEFT]));
                    rayState.toVisitPush(move(children[RIGHT]));
                } else {
                    rayState.toVisitPush(move(children[RIGHT]));
                    rayState.toVisitPush(move(children[LEFT]));
                }
            }
        } else {
            if (rayState.toVisitEmpty()) break;
        }
    }
}

bool CloudBVH::Intersect(const Ray &ray, SurfaceInteraction *isect) const {
    return Intersect(ray, isect, bvh_root_);
}

bool CloudBVH::Intersect(const Ray &ray, SurfaceInteraction *isect,
                         const uint32_t bvh_root) const {
    ProfilePhase _(Prof::AccelIntersect);

    bool hit = false;
    Vector3f invDir(1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z);
    int dirIsNeg[3] = {invDir.x < 0, invDir.y < 0, invDir.z < 0};

    // Follow ray through BVH nodes to find primitive intersections
    pair<uint32_t, uint32_t> toVisit[64];
    uint8_t toVisitOffset = 0;

    uint32_t startTreelet = bvh_root;
    if (bvh_root == 0) {
        startTreelet = ComputeIdx(ray.d);
    }

    pair<uint32_t, uint32_t> current(startTreelet, 0);

    uint32_t prevTreelet = startTreelet;
    while (true) {
        LoadTreelet(current.first);
        auto &treelet = *treelets_[current.first];
        auto &node = treelet.nodes[current.second];

        // Check ray against BVH node
        if (node.bounds.IntersectP(ray, invDir, dirIsNeg)) {
            if (node.is_leaf()) {
                auto &primitives = treelet.primitives;
                for (int i = node.primitive_offset;
                     i < node.primitive_offset + node.primitive_count; i++) {
                    if (primitives[i]->Intersect(ray, isect)) hit = true;
                }

                if (toVisitOffset == 0) break;
                current = toVisit[--toVisitOffset];
            } else {
                pair<uint32_t, uint32_t> children[2];
                for (int i = 0; i < 2; i++) {
                    children[i].first = node.child_treelet[i];
                    children[i].second = node.child_node[i];
                }

                if (dirIsNeg[node.axis]) {
                    toVisit[toVisitOffset++] = children[LEFT];
                    current = children[RIGHT];
                } else {
                    toVisit[toVisitOffset++] = children[RIGHT];
                    current = children[LEFT];
                }
            }
        } else {
            if (toVisitOffset == 0) break;
            current = toVisit[--toVisitOffset];
        }

        prevTreelet = current.first;
    }

    return hit;
}

bool CloudBVH::IntersectP(const Ray &ray) const {
    return IntersectP(ray, bvh_root_);
}

bool CloudBVH::IntersectP(const Ray &ray, const uint32_t bvh_root) const {
    ProfilePhase _(Prof::AccelIntersectP);

    Vector3f invDir(1.f / ray.d.x, 1.f / ray.d.y, 1.f / ray.d.z);
    int dirIsNeg[3] = {invDir.x < 0, invDir.y < 0, invDir.z < 0};

    // Follow ray through BVH nodes to find primitive intersections
    uint8_t toVisitOffset = 0;
    pair<uint32_t, uint32_t> toVisit[64];

    uint32_t startTreelet = bvh_root;
    if (bvh_root == 0) {
        startTreelet = ComputeIdx(ray.d);
    }

    pair<uint32_t, uint32_t> current(startTreelet, 0);

    uint32_t prevTreelet = startTreelet;
    while (true) {
        LoadTreelet(current.first);
        auto &treelet = *treelets_[current.first];
        auto &node = treelet.nodes[current.second];

        // Check ray against BVH node
        if (node.bounds.IntersectP(ray, invDir, dirIsNeg)) {
            if (node.is_leaf()) {
                auto &primitives = treelet.primitives;
                for (int i = node.primitive_offset;
                     i < node.primitive_offset + node.primitive_count; i++) {
                    if (primitives[i]->IntersectP(ray)) return true;
                }

                if (toVisitOffset == 0) break;
                current = toVisit[--toVisitOffset];
            } else {
                pair<uint32_t, uint32_t> children[2];
                for (int i = 0; i < 2; i++) {
                    children[i].first = node.child_treelet[i];
                    children[i].second = node.child_node[i];
                }

                if (dirIsNeg[node.axis]) {
                    toVisit[toVisitOffset++] = children[LEFT];
                    current = children[RIGHT];
                } else {
                    toVisit[toVisitOffset++] = children[RIGHT];
                    current = children[LEFT];
                }
            }
        } else {
            if (toVisitOffset == 0) break;
            current = toVisit[--toVisitOffset];
        }

        prevTreelet = current.first;
    }

    return false;
}

vector<Bounds3f> CloudBVH::getTreeletNodeBounds(
    const uint32_t treelet_id, const int recursionLimit) const {
    return vector<Bounds3f>();
#if 0
    LoadTreelet(treelet_id);

    vector<Bounds3f> treeletBounds;

    const int depth = 0;
    const int idx = 1;

    // load base node bounds
    auto &currTreelet = *treelets_.at(treelet_id);
    auto &currNode = currTreelet.nodes[0];

    // size reflects indexing starting at 1
    const size_t size = pow(2, recursionLimit);
    treeletBounds.resize(size);
    recurseBVHNodes(depth, recursionLimit, idx, currTreelet, currNode,
                    treeletBounds);

    return treeletBounds;
#endif
}

void CloudBVH::recurseBVHNodes(const int depth, const int recursionLimit,
                               const int idx, const Treelet &currTreelet,
                               const TreeletNode &currNode,
                               vector<Bounds3f> &treeletBounds) const {
// FIXME Update for multi root treelets
#if 0
    if (depth == recursionLimit) {
        return;
    }

    // save the current node
    treeletBounds[idx] = currNode.bounds;

    // save left value, if there's one
    if (currNode.has[0]) {
        const uint32_t left = currNode.child[0];
        recurseBVHNodes(depth + 1, recursionLimit, 2 * idx, currTreelet,
                        currTreelet.nodes[left], treeletBounds);
    }

    // save right value, if there's one
    if (currNode.has[1]) {
        const uint32_t right = currNode.child[1];
        recurseBVHNodes(depth + 1, recursionLimit, 2 * idx + 1, currTreelet,
                        currTreelet.nodes[right], treeletBounds);
    }
#endif
}

void CloudBVH::clear() const {
    treelets_.clear();
    bvh_instances_.clear();
    materials_.clear();
}

shared_ptr<CloudBVH> CreateCloudBVH(const ParamSet &ps) {
    const bool preload = ps.FindOneBool("preload", false);
    return make_shared<CloudBVH>(0, preload, true);
}

Bounds3f CloudBVH::IncludedInstance::WorldBound() const {
    return treelet_->nodes[nodeIdx_].bounds;
}

bool CloudBVH::IncludedInstance::Intersect(const Ray &ray,
                                           SurfaceInteraction *isect) const {
    bool hit = false;
    Vector3f invDir(1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z);
    int dirIsNeg[3] = {invDir.x < 0, invDir.y < 0, invDir.z < 0};

    // Follow ray through BVH nodes to find primitive intersections
    int toVisitOffset = 0, currentNodeIndex = nodeIdx_;
    int nodesToVisit[64];
    while (true) {
        const CloudBVH::TreeletNode *node = &treelet_->nodes[currentNodeIndex];
        // Check ray against BVH node
        if (node->bounds.IntersectP(ray, invDir, dirIsNeg)) {
            if (node->is_leaf()) {
                // Intersect ray with primitives in leaf BVH node
                for (int i = 0; i < node->primitive_count; ++i)
                    if (treelet_->primitives[node->primitive_offset + i]
                            ->Intersect(ray, isect))
                        hit = true;
                if (toVisitOffset == 0) break;
                currentNodeIndex = nodesToVisit[--toVisitOffset];
            } else {
                // Put far BVH node on _nodesToVisit_ stack, advance to near
                // node
                if (dirIsNeg[node->axis]) {
                    nodesToVisit[toVisitOffset++] = node->child_node[LEFT];
                    currentNodeIndex = node->child_node[RIGHT];
                } else {
                    nodesToVisit[toVisitOffset++] = node->child_node[RIGHT];
                    currentNodeIndex = node->child_node[LEFT];
                }
            }
        } else {
            if (toVisitOffset == 0) break;
            currentNodeIndex = nodesToVisit[--toVisitOffset];
        }
    }
    return hit;
}

bool CloudBVH::IncludedInstance::IntersectP(const Ray &ray) const {
    Vector3f invDir(1 / ray.d.x, 1 / ray.d.y, 1 / ray.d.z);
    int dirIsNeg[3] = {invDir.x < 0, invDir.y < 0, invDir.z < 0};
    int toVisitOffset = 0, currentNodeIndex = nodeIdx_;
    int nodesToVisit[64];

    while (true) {
        const CloudBVH::TreeletNode *node = &treelet_->nodes[currentNodeIndex];
        // Check ray against BVH node
        if (node->bounds.IntersectP(ray, invDir, dirIsNeg)) {
            if (node->is_leaf()) {
                // Intersect ray with primitives in leaf BVH node
                for (int i = 0; i < node->primitive_count; ++i)
                    if (treelet_->primitives[node->primitive_offset + i]
                            ->IntersectP(ray)) {
                        return true;
                    }
                if (toVisitOffset == 0) break;
                currentNodeIndex = nodesToVisit[--toVisitOffset];
            } else {
                // Put far BVH node on _nodesToVisit_ stack, advance to near
                // node
                if (dirIsNeg[node->axis]) {
                    nodesToVisit[toVisitOffset++] = node->child_node[LEFT];
                    currentNodeIndex = node->child_node[RIGHT];
                } else {
                    nodesToVisit[toVisitOffset++] = node->child_node[RIGHT];
                    currentNodeIndex = node->child_node[LEFT];
                }
            }
        } else {
            if (toVisitOffset == 0) break;
            currentNodeIndex = nodesToVisit[--toVisitOffset];
        }
    }

    return false;
}

Vector3f ComputeRayDir(unsigned idx) {
    unsigned x = idx & (1 << 0);
    unsigned y = idx & (1 << 1);
    unsigned z = idx & (1 << 2);

    return Vector3f(x ? 1 : -1, y ? 1 : -1, z ? 1 : -1);
}

unsigned ComputeIdx(const Vector3f &dir) {
    if (PbrtOptions.directionalTreelets) {
        return (dir.x >= 0 ? 1 : 0) + ((dir.y >= 0 ? 1 : 0) << 1) +
               ((dir.z >= 0 ? 1 : 0) << 2);
    } else {
        return 0;
    }
}

}  // namespace pbrt
