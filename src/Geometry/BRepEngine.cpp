#include "Geometry/BRepEngine.h"
#include "Core/Logger.h"
#include "Math/MathUtils.h"

#include <opencascade/BRepMesh_IncrementalMesh.hxx>
#include <opencascade/BRep_Tool.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_ShapeEnum.hxx>
#include <opencascade/Poly_Triangulation.hxx>
#include <opencascade/Poly_Array1OfTriangle.hxx>
#include <opencascade/TColgp_Array1OfPnt.hxx>
#include <opencascade/gp_Pnt.hxx>
#include <opencascade/gp_Vec.hxx>
#include <opencascade/BRepGProp.hxx>
#include <opencascade/GProp_GProps.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/BRepTools.hxx>
#include <opencascade/GeomLProp_SLProps.hxx>
#include <opencascade/BRepAdaptor_Surface.hxx>
#include <opencascade/TopoDS_Face.hxx>
#include <opencascade/TopoDS.hxx>
#include <opencascade/BRepBndLib.hxx>
#include <opencascade/Bnd_Box.hxx>
#include <glm/gtx/norm.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace mf {

static bool isUsableCoord(Standard_Real v) {
    constexpr Standard_Real kMaxReasonableCoord = 1.0e12;
    return std::isfinite(v) && std::abs(v) < kMaxReasonableCoord;
}

static bool isUsablePoint(const gp_Pnt& p) {
    return isUsableCoord(p.X()) && isUsableCoord(p.Y()) && isUsableCoord(p.Z());
}

static Vec3 safeNormal(Vec3 normal) {
    if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z) ||
        glm::length2(normal) < 1e-20f) {
        return Vec3(0, 0, 1);
    }
    return glm::normalize(normal);
}

// Compute normal at UV param on surface
static Vec3 computeNormal(const TopoDS_Face& face, const Handle(Poly_Triangulation)& tri,
                          int nodeIndex, const gp_Pnt2d& uv) {
    try {
        BRepAdaptor_Surface surf(face, Standard_False);
        GeomLProp_SLProps props(surf.Surface().Surface(), uv.X(), uv.Y(), 1, 1e-6);
        if (props.IsNormalDefined()) {
            gp_Dir n = props.Normal();
            if (face.Orientation() == TopAbs_REVERSED) n.Reverse();
            return Vec3(static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z()));
        }
    } catch (...) {}

    // Fallback: average face normals from triangles containing this node
    Standard_Integer nbNodes = tri->NbNodes();
    if (nodeIndex >= 1 && nodeIndex <= nbNodes) {
        gp_Vec avg(0, 0, 0);
        int count = 0;
        for (Standard_Integer t = 1; t <= tri->NbTriangles(); ++t) {
            const Poly_Triangle& triangle = tri->Triangle(t);
            Standard_Integer n1, n2, n3;
            triangle.Get(n1, n2, n3);
            if (n1 == nodeIndex || n2 == nodeIndex || n3 == nodeIndex) {
                gp_Pnt p1 = tri->Node(n1);
                gp_Pnt p2 = tri->Node(n2);
                gp_Pnt p3 = tri->Node(n3);
                gp_Vec v1(p2.XYZ() - p1.XYZ());
                gp_Vec v2(p3.XYZ() - p1.XYZ());
                gp_Vec fn = v1.Crossed(v2);
                if (fn.Magnitude() > 1e-12) {
                    avg += fn.Normalized();
                    ++count;
                }
            }
        }
        if (count > 0) {
            avg /= count;
            if (avg.Magnitude() > 1e-12) {
                avg.Normalize();
                return Vec3(static_cast<float>(avg.X()), static_cast<float>(avg.Y()), static_cast<float>(avg.Z()));
            }
        }
    }
    return Vec3(0, 0, 1);
}

class BRepEngine::Impl {
public:
    BRepMesh doTessellate(const TopoDS_Shape& shape, const TopLoc_Location& loc,
                          const TessellationParams& params);
};

BRepEngine::BRepEngine() : m_impl(std::make_unique<Impl>()) {}
BRepEngine::~BRepEngine() = default;

BRepMesh BRepEngine::tessellate(const TopoDS_Shape& shape, const TessellationParams& params) {
    return m_impl->doTessellate(shape, TopLoc_Location(), params);
}

BRepMesh BRepEngine::tessellate(const TopoDS_Shape& shape, const TopLoc_Location& loc,
                                const TessellationParams& params) {
    return m_impl->doTessellate(shape, loc, params);
}

BRepMesh BRepEngine::tessellate(const TopoDS_Shape& shape, const Mat4& transform,
                                const TessellationParams& params) {
    gp_Trsf trsf;
    trsf.SetValues(
        transform[0][0], transform[1][0], transform[2][0], transform[3][0],
        transform[0][1], transform[1][1], transform[2][1], transform[3][1],
        transform[0][2], transform[1][2], transform[2][2], transform[3][2]);
    return m_impl->doTessellate(shape, TopLoc_Location(trsf), params);
}

BRepMesh BRepEngine::Impl::doTessellate(const TopoDS_Shape& shape, const TopLoc_Location& loc,
                                        const TessellationParams& params) {
    BRepMesh mesh;
    if (shape.IsNull()) return mesh;

    float linDefl = params.linearDeflection;
    if (params.relative) {
        GProp_GProps props;
        BRepGProp::VolumeProperties(shape, props);
        if (props.Mass() > 0) {
            Bnd_Box box;
            BRepBndLib::Add(shape, box);
            if (!box.IsVoid()) {
                float diag = static_cast<float>(box.SquareExtent());
                linDefl = std::max(diag * params.linearDeflection, params.minEdgeLength);
            }
        }
    }

    BRepMesh_IncrementalMesh incMesh(shape, linDefl, Standard_False,
                                     static_cast<Standard_Real>(params.angularDeflection),
                                     params.parallelMeshing ? Standard_True : Standard_False);

    TopExp_Explorer faceExp(shape, TopAbs_FACE);
    for (; faceExp.More(); faceExp.Next()) {
        TopoDS_Face face = TopoDS::Face(faceExp.Current());
        TopLoc_Location faceLoc;
        Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, faceLoc);
        if (tri.IsNull()) continue;

        Standard_Integer nbNodes = tri->NbNodes();
        Standard_Integer nbTriangles = tri->NbTriangles();

        TopLoc_Location totalLoc = loc * faceLoc;
        gp_Trsf trsf = totalLoc.Transformation();

        bool hasUV = tri->HasUVNodes();
        std::vector<Index> remap(static_cast<size_t>(nbNodes) + 1,
                                 std::numeric_limits<Index>::max());

        for (Standard_Integer n = 1; n <= nbNodes; ++n) {
            gp_Pnt p = tri->Node(n).Transformed(trsf);
            if (!isUsablePoint(p)) continue;
            Vec3 pos(static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z()));
            mesh.aabb.expand(pos);

            Vec3 normal(0, 0, 1);
            Vec2 uv(0, 0);
            if (hasUV) {
                gp_Pnt2d uvNode = tri->UVNode(n);
                uv = Vec2(static_cast<float>(uvNode.X()), static_cast<float>(uvNode.Y()));
                normal = computeNormal(face, tri, n, uvNode);
            }

            remap[static_cast<size_t>(n)] = static_cast<Index>(mesh.vertices.size());
            Vertex v;
            v.position = pos;
            v.normal = safeNormal(normal);
            v.uv = uv;
            v.tangent = Vec4(1, 0, 0, 1);
            mesh.vertices.push_back(v);
        }

        uint32_t validTriangles = 0;
        for (Standard_Integer t = 1; t <= nbTriangles; ++t) {
            const Poly_Triangle& triangle = tri->Triangle(t);
            Standard_Integer n1, n2, n3;
            triangle.Get(n1, n2, n3);
            if (n1 < 1 || n1 > nbNodes || n2 < 1 || n2 > nbNodes || n3 < 1 || n3 > nbNodes) {
                continue;
            }
            Index i1 = remap[static_cast<size_t>(n1)];
            Index i2 = remap[static_cast<size_t>(n2)];
            Index i3 = remap[static_cast<size_t>(n3)];
            if (i1 == std::numeric_limits<Index>::max() ||
                i2 == std::numeric_limits<Index>::max() ||
                i3 == std::numeric_limits<Index>::max()) {
                continue;
            }
            mesh.indices.push_back(i1);
            mesh.indices.push_back(i2);
            mesh.indices.push_back(i3);
            ++validTriangles;
        }

        if (validTriangles > 0) {
            ++mesh.faceCount;
            mesh.triangleCount += validTriangles;
        }
    }

    MF_INFO("BRepEngine: {} vertices, {} triangles from {} faces",
            mesh.vertices.size(), mesh.indices.size() / 3, mesh.faceCount);
    return mesh;
}

} // namespace mf
