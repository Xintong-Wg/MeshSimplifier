#include "Core/Logger.h"
#include "Geometry/STEPReader.h"

#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Bnd_Box.hxx>
#include <opencascade/IFSelect_ReturnStatus.hxx>
#include <opencascade/STEPCAFControl_Reader.hxx>
#include <opencascade/TDF_LabelSequence.hxx>
#include <opencascade/TDocStd_Document.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/XCAFDoc_DocumentTool.hxx>
#include <opencascade/XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

bool isReasonableCoord(double value) {
    constexpr double kMaxReasonableCoord = 1.0e12;
    return std::isfinite(value) && std::abs(value) < kMaxReasonableCoord;
}

mf::Mat4 worldOf(const mf::AssemblyNodePtr& node) {
    mf::Mat4 world(1.0f);
    auto p = node;
    while (p && p->type != mf::AssemblyNode::Type::Root) {
        world = p->localTransform * world;
        p = p->parent;
    }
    return world;
}

mf::Mat4 inverseWorldOf(const mf::AssemblyNodePtr& node) {
    mf::Mat4 world(1.0f);
    auto p = node;
    while (p && p->type != mf::AssemblyNode::Type::Root) {
        world = glm::inverse(p->localTransform) * world;
        p = p->parent;
    }
    return world;
}

void expandTransformed(mf::AABB& out, const mf::AABB& box, const mf::Mat4& trsf) {
    if (box.isEmpty()) return;
    for (int i = 0; i < 8; ++i) {
        mf::Vec3 p(
            (i & 1) ? box.max.x : box.min.x,
            (i & 2) ? box.max.y : box.min.y,
            (i & 4) ? box.max.z : box.min.z);
        out.expand(mf::Vec3(trsf * mf::Vec4(p, 1.0f)));
    }
}

void printBox(const char* label, const mf::AABB& box) {
    auto c = box.center();
    auto e = box.extent();
    std::cout << label
              << " center=(" << c.x << "," << c.y << "," << c.z << ")"
              << " extent=(" << e.x << "," << e.y << "," << e.z << ")"
              << " diag=" << box.diagonal() << "\n";
}

mf::AABB shapeBox(const TopoDS_Shape& shape) {
    mf::AABB result;
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) return result;

    Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    if (!isReasonableCoord(xmin) || !isReasonableCoord(ymin) || !isReasonableCoord(zmin) ||
        !isReasonableCoord(xmax) || !isReasonableCoord(ymax) || !isReasonableCoord(zmax)) {
        return result;
    }
    result.expand(mf::Vec3(static_cast<float>(xmin), static_cast<float>(ymin), static_cast<float>(zmin)));
    result.expand(mf::Vec3(static_cast<float>(xmax), static_cast<float>(ymax), static_cast<float>(zmax)));
    return result;
}

mf::AABB occFreeShapeBox(const std::string& filepath) {
    mf::AABB result;
    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_False);
    reader.SetLayerMode(Standard_False);
    reader.SetPropsMode(Standard_False);
    reader.SetMetaMode(Standard_False);
    reader.SetProductMetaMode(Standard_False);
    reader.SetSHUOMode(Standard_True);
    reader.SetGDTMode(Standard_False);
    reader.SetMatMode(Standard_False);
    reader.SetViewMode(Standard_False);
    reader.SetNameMode(Standard_True);

    if (reader.ReadFile(filepath.c_str()) != IFSelect_RetDone) {
        return result;
    }

    Handle(TDocStd_Document) doc = new TDocStd_Document("Probe");
    if (!reader.Transfer(doc)) {
        return result;
    }

    Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
    TDF_LabelSequence roots;
    shapeTool->GetFreeShapes(roots);
    for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
        TopoDS_Shape shape;
        shapeTool->GetShape(roots.Value(i), shape);
        if (shape.IsNull()) continue;
        Bnd_Box box;
        BRepBndLib::Add(shape, box);
        if (box.IsVoid()) continue;
        Standard_Real xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        if (!isReasonableCoord(xmin) || !isReasonableCoord(ymin) || !isReasonableCoord(zmin) ||
            !isReasonableCoord(xmax) || !isReasonableCoord(ymax) || !isReasonableCoord(zmax)) {
            std::cout << "occ-free root " << i << " has non-finite bbox, skipped\n";
            continue;
        }
        result.expand(mf::Vec3(static_cast<float>(xmin), static_cast<float>(ymin), static_cast<float>(zmin)));
        result.expand(mf::Vec3(static_cast<float>(xmax), static_cast<float>(ymax), static_cast<float>(zmax)));
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    mf::Logger::init();
    if (argc < 2) {
        std::cerr << "Usage: StepProbe <file.step>\n";
        return 2;
    }

    std::string filepath = argv[1];
    auto t0 = std::chrono::steady_clock::now();
    mf::STEPReader reader;
    auto result = reader.read(filepath, "");
    if (!result || !result->root) {
        std::cerr << "STEPReader failed\n";
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();

    std::unordered_map<std::string, mf::AABB> localBoxes;
    localBoxes.reserve(result->shapesByKey.size());
    for (const auto& [shapeKey, shape] : result->shapesByKey) {
        localBoxes.emplace(shapeKey, shapeBox(shape));
    }

    mf::AABB appBox;
    mf::AABB inverseBox;
    size_t printed = 0;
    size_t missing = 0;
    for (const auto& [id, part] : result->partsById) {
        auto shapeIt = result->shapesByKey.find(part->shapeKey);
        if (shapeIt == result->shapesByKey.end()) {
            ++missing;
            continue;
        }

        auto localIt = localBoxes.find(part->shapeKey);
        if (localIt == localBoxes.end() || localIt->second.isEmpty()) {
            ++missing;
            continue;
        }
        mf::AABB localBox = localIt->second;
        mf::Mat4 world = worldOf(part);
        expandTransformed(appBox, localBox, world);
        expandTransformed(inverseBox, localBox, inverseWorldOf(part));

        if (printed < 24) {
            auto t = mf::Vec3(world[3]);
            mf::AABB worldBox;
            expandTransformed(worldBox, localBox, world);
            auto lc = localBox.center();
            auto wc = worldBox.center();
            std::cout << "part " << printed
                      << " id=" << id
                      << " shape=" << part->shapeKey
                      << " worldT=(" << t.x << "," << t.y << "," << t.z << ")"
                      << " localC=(" << lc.x << "," << lc.y << "," << lc.z << ")"
                      << " worldC=(" << wc.x << "," << wc.y << "," << wc.z << ")"
                      << " localDiag=" << localBox.diagonal()
                      << "\n";
        }
        ++printed;
    }

    auto t2 = std::chrono::steady_clock::now();
    mf::AABB occBox = occFreeShapeBox(filepath);
    auto t3 = std::chrono::steady_clock::now();

    std::cout << "parts=" << result->partsById.size()
              << " shapes=" << result->shapesByKey.size()
              << " missing=" << missing << "\n";
    printBox("app-world", appBox);
    printBox("inv-world", inverseBox);
    printBox("occ-free ", occBox);
    std::cout << "timing read_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
              << " app_bbox_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count()
              << " occ_bbox_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count()
              << "\n";

    return 0;
}
