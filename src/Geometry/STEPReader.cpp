#include "Geometry/STEPReader.h"
#include "Core/Logger.h"
#include "Math/MathUtils.h"

#include <opencascade/STEPCAFControl_Reader.hxx>
#include <opencascade/STEPControl_Reader.hxx>
#include <opencascade/IGESControl_Reader.hxx>
#include <opencascade/XCAFDoc_ShapeTool.hxx>
#include <opencascade/XCAFDoc_DocumentTool.hxx>
#include <opencascade/TDocStd_Document.hxx>
#include <opencascade/TopoDS_Shape.hxx>
#include <opencascade/TopoDS_Compound.hxx>
#include <opencascade/BRep_Builder.hxx>
#include <opencascade/TDF_Label.hxx>
#include <opencascade/TDF_LabelSequence.hxx>
#include <opencascade/TopExp_Explorer.hxx>
#include <opencascade/TopAbs_ShapeEnum.hxx>
#include <opencascade/BRepTools.hxx>
#include <opencascade/gp_Trsf.hxx>
#include <opencascade/gp_Ax3.hxx>
#include <opencascade/Quantity_Color.hxx>
#include <opencascade/XCAFDoc_ColorTool.hxx>
#include <opencascade/XCAFDoc_ColorType.hxx>
#include <opencascade/TDataStd_Name.hxx>
#include <opencascade/TCollection_ExtendedString.hxx>
#include <opencascade/TDF_Tool.hxx>
#include <opencascade/Message_ProgressRange.hxx>
#include <opencascade/Message_ProgressIndicator.hxx>
#include <opencascade/IFSelect_ReturnStatus.hxx>
#include <opencascade/StepData_StepModel.hxx>
#include <opencascade/Interface_EntityIterator.hxx>
#include <opencascade/Interface_InterfaceModel.hxx>
#include <opencascade/Standard_Integer.hxx>
#include <opencascade/Standard_Failure.hxx>
#include <opencascade/TopLoc_Location.hxx>
#include <opencascade/BRepTools.hxx>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <stack>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <clocale>
#include <cstdint>
#include <cwchar>

namespace mf {

static constexpr int kBRepCacheSchemaVersion = 5;

// ------------------------------------------------------------------
// Helper: convert gp_Trsf to Mat4
// ------------------------------------------------------------------
static Mat4 toMat4(const gp_Trsf& trsf) {
    Mat4 m(1.0f);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[j][i] = static_cast<float>(trsf.Value(i + 1, j + 1));
        }
    }
    return m;
}

// ------------------------------------------------------------------
// Helper: read name from TDF_Label
// ------------------------------------------------------------------
static void appendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp == 0 || (cp < 0x20 && cp != '\t')) {
        out.push_back(' ');
    } else if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static std::string toUtf8(const TCollection_ExtendedString& extStr) {
    std::string result;
    result.reserve(static_cast<size_t>(extStr.Length()) * 3);
    for (int i = 1; i <= extStr.Length(); ++i) {
        uint32_t cp = static_cast<uint32_t>(extStr.Value(i));
        if (cp >= 0xD800 && cp <= 0xDBFF && i < extStr.Length()) {
            const uint32_t low = static_cast<uint32_t>(extStr.Value(i + 1));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        appendUtf8Codepoint(result, cp);
    }
    auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "Unnamed";
    auto last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

static bool containsCjk(const std::wstring& text) {
    for (wchar_t ch : text) {
        const auto cp = static_cast<uint32_t>(ch);
        if ((cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0xF900 && cp <= 0xFAFF)) {
            return true;
        }
    }
    return false;
}

static std::string wideToUtf8(const std::wstring& text) {
    std::string result;
    result.reserve(text.size() * 3);
    for (size_t i = 0; i < text.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) {
                const uint32_t low = static_cast<uint32_t>(text[i + 1]);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    ++i;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
        }
        appendUtf8Codepoint(result, cp);
    }
    auto first = result.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "Unnamed";
    auto last = result.find_last_not_of(" \t\r\n");
    return result.substr(first, last - first + 1);
}

static bool tryDecodeLocaleBytes(const std::string& bytes, const char* localeName, std::string& out) {
    const char* previousPtr = std::setlocale(LC_CTYPE, nullptr);
    const std::string previous = previousPtr ? previousPtr : "";
    if (!std::setlocale(LC_CTYPE, localeName)) return false;

    std::mbstate_t state{};
    const char* src = bytes.c_str();
    size_t wideLen = std::mbsrtowcs(nullptr, &src, 0, &state);
    if (wideLen == static_cast<size_t>(-1)) {
        if (!previous.empty()) std::setlocale(LC_CTYPE, previous.c_str());
        return false;
    }

    std::wstring wide(wideLen, L'\0');
    state = std::mbstate_t{};
    src = bytes.c_str();
    size_t converted = std::mbsrtowcs(wide.data(), &src, wide.size(), &state);
    if (!previous.empty()) std::setlocale(LC_CTYPE, previous.c_str());
    if (converted == static_cast<size_t>(-1)) return false;
    if (converted < wide.size()) wide.resize(converted);
    if (!containsCjk(wide)) return false;

    out = wideToUtf8(wide);
    return !out.empty();
}

static std::string decodeLegacyByteName(const TCollection_ExtendedString& extStr,
                                        const std::string& fallback) {
    std::string bytes;
    bytes.reserve(static_cast<size_t>(extStr.Length()));
    bool hasHighByte = false;
    for (int i = 1; i <= extStr.Length(); ++i) {
        const auto cp = static_cast<uint32_t>(extStr.Value(i));
        if (cp > 0xFF) return fallback;
        if (cp >= 0x80) hasHighByte = true;
        bytes.push_back(static_cast<char>(cp & 0xFF));
    }
    if (!hasHighByte) return fallback;

    std::string decoded;
    for (const char* localeName : {".936", "Chinese_China.936", "zh_CN.GBK", "zh_CN.gb18030"}) {
        if (tryDecodeLocaleBytes(bytes, localeName, decoded)) return decoded;
    }
    return fallback;
}

static std::string readName(const TDF_Label& label) {
    Handle(TDataStd_Name) nameAttr;
    if (label.FindAttribute(TDataStd_Name::GetID(), nameAttr)) {
        TCollection_ExtendedString extStr = nameAttr->Get();
        return decodeLegacyByteName(extStr, toUtf8(extStr));
    }
    return "Unnamed";
}

static std::string labelEntryKey(const TDF_Label& label) {
    TCollection_AsciiString entry;
    TDF_Tool::Entry(label, entry);

    std::string key = entry.ToCString();
    for (char& c : key) {
        if (c == ':' || c == ' ' || c == '/' || c == '\\') {
            c = '_';
        }
    }
    return key;
}

static std::string lowerExtension(const std::string& filepath) {
    auto pos = filepath.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string ext = filepath.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

static bool isIGESFile(const std::string& filepath) {
    std::string ext = lowerExtension(filepath);
    return ext == "igs" || ext == "iges";
}

// ------------------------------------------------------------------
// Pimpl
// ------------------------------------------------------------------
class STEPReader::Impl {
public:
    std::shared_ptr<STEPResult> result;

    std::shared_ptr<STEPResult> read(const std::string& filepath, const std::string& cacheDir);
    std::shared_ptr<STEPResult> readIGESFlat(const std::string& filepath);
    void traverseAssembly(const Handle(XCAFDoc_ShapeTool)& shapeTool,
                          const TDF_Label& label,
                          AssemblyNodePtr parent,
                          const Mat4& parentTransform);

    // BRep binary cache
    bool loadFromCache(const std::string& cacheDir);
    void saveToCache(const std::string& cacheDir);
};

STEPReader::STEPReader() : m_impl(std::make_unique<Impl>()) {}
STEPReader::~STEPReader() = default;

std::shared_ptr<STEPResult> STEPReader::read(const std::string& filepath,
                                              const std::string& cacheDir) {
    return m_impl->read(filepath, cacheDir);
}

bool STEPReader::probe(const std::string& filepath, size_t& outEntityCount) {
    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    outEntityCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("=") != std::string::npos && line.find("#") != std::string::npos) {
            ++outEntityCount;
        }
    }
    return true;
}

std::shared_ptr<STEPResult> STEPReader::Impl::read(const std::string& filepath,
                                                    const std::string& cacheDir) {
    // IGES files go through a flat reader (different format, no XCAF assembly)
    if (isIGESFile(filepath)) {
        MF_INFO("Detected IGES file, using flat reader: {}", filepath);
        return readIGESFlat(filepath);
    }

    // Try BRep binary cache first (instant load, STEP only)
    if (!cacheDir.empty() && loadFromCache(cacheDir)) {
        MF_INFO("STEP loaded from BRep cache: {} parts, {} shapes",
                result->partsById.size(), result->shapesByKey.size());
        return result;
    }

    result = std::make_shared<STEPResult>();

    // Use STEPCAFControl_Reader to preserve assembly hierarchy
    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_False);
    reader.SetLayerMode(Standard_False);
    reader.SetPropsMode(Standard_False);
    reader.SetMetaMode(Standard_False);
    reader.SetProductMetaMode(Standard_False);
    reader.SetSHUOMode(Standard_False);
    reader.SetGDTMode(Standard_False);
    reader.SetMatMode(Standard_False);
    reader.SetViewMode(Standard_False);
    reader.SetNameMode(Standard_True);

    auto readStart = std::chrono::steady_clock::now();
    IFSelect_ReturnStatus status = reader.ReadFile(filepath.c_str());
    auto readEnd = std::chrono::steady_clock::now();
    if (status != IFSelect_RetDone) {
        MF_ERROR("Failed to read STEP file: {}", filepath);
        return nullptr;
    }
    MF_INFO("STEP ReadFile: {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(readEnd - readStart).count());

    result->entityCount = reader.NbRootsForTransfer();
    MF_INFO("STEP entities: {}", result->entityCount);

    // Transfer to XCAF document (preserves assembly tree).
    // Some STEP files have bad geometry that causes OCC's ShapeFix to hang or
    // throw (e.g. Geom_RectangularTrimmedSurface parameter out of range).
    // We use a two-path strategy:
    //   1. Try STEPCAFControl_Reader (with shape healing, preserves assembly)
    //   2. Fall back to STEPControl_Reader (no shape healing, flat structure)
    Handle(TDocStd_Document) doc = new TDocStd_Document("MeshForge");
    bool transferOk = false;
    bool usedFallback = false;

    try {
        auto transferStart = std::chrono::steady_clock::now();
        transferOk = reader.Transfer(doc);
        auto transferEnd = std::chrono::steady_clock::now();
        MF_INFO("STEP XCAF Transfer: {} ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(transferEnd - transferStart).count());
    } catch (const Standard_Failure& e) {
        MF_WARN("STEPCAF Transfer exception: {} — falling back to STEPControl_Reader",
                e.GetMessageString() ? e.GetMessageString() : "unknown");
    } catch (...) {
        MF_WARN("STEPCAF Transfer unknown exception — falling back to STEPControl_Reader");
    }

    if (!transferOk) {
        // Salvage: check if shapes were partially transferred
        MF_WARN("STEPCAF Transfer failed, attempting salvage...");
        try {
            Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
            if (!shapeTool.IsNull()) {
                TDF_LabelSequence salvageLabels;
                shapeTool->GetFreeShapes(salvageLabels);
                if (salvageLabels.Length() > 0) {
                    MF_INFO("Salvage: found {} shapes, using them", salvageLabels.Length());
                    transferOk = true;
                }
            }
        } catch (...) {
            MF_WARN("Salvage attempt threw exception");
        }
    }

    if (!transferOk) {
        MF_WARN("Salvage also failed, using STEPControl_Reader (flat structure)...");

        // Fallback: STEPControl_Reader reads geometry without XCAF assembly
        // and importantly, WITHOUT shape processing/ShapeFix.
        STEPControl_Reader basicReader;
        IFSelect_ReturnStatus basicStatus = basicReader.ReadFile(filepath.c_str());
        if (basicStatus != IFSelect_RetDone) {
            MF_ERROR("Fallback reader also failed to read STEP file");
            return nullptr;
        }

        // Fallback: STEPControl_Reader transfers shapes without XCAF overhead.
        // Transfer roots one by one, skip any that throw during shape healing.
        Standard_Integer nbRoots = basicReader.NbRootsForTransfer();
        if (nbRoots <= 0) {
            MF_ERROR("Fallback reader found no roots");
            return nullptr;
        }

        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        int transferred = 0;

        for (Standard_Integer i = 1; i <= nbRoots; ++i) {
            try {
                basicReader.TransferRoot(i);
                TopoDS_Shape shape = basicReader.Shape(i);
                if (!shape.IsNull()) {
                    builder.Add(compound, shape);
                    ++transferred;
                }
            } catch (const Standard_Failure& e) {
                MF_WARN("Fallback: skipping root {} — {}", i,
                        e.GetMessageString() ? e.GetMessageString() : "exception");
            } catch (...) {
                MF_WARN("Fallback: skipping root {} due to unknown exception", i);
            }
        }

        if (transferred == 0) {
            MF_ERROR("Fallback reader transferred 0 shapes");
            return nullptr;
        }

        // Build a flat shape structure manually (no assembly tree)
        std::string flatKey = "shape_fallback";
        result->shapesByKey[flatKey] = compound;
        result->root = std::make_shared<AssemblyNode>();
        result->root->id = "root";
        result->root->name = "Root";
        result->root->type = AssemblyNode::Type::Root;

        auto part = std::make_shared<AssemblyNode>();
        part->id = "part_fallback";
        part->name = filepath.substr(filepath.find_last_of("/\\") + 1);
        part->type = AssemblyNode::Type::Part;
        part->shapeKey = flatKey;
        part->localTransform = Mat4(1.0f);
        result->root->children.push_back(part);
        part->parent = result->root;
        result->partsById[part->id] = part;

        result->entityCount = transferred;
        usedFallback = true;

        MF_INFO("Fallback: loaded {} roots as flat assembly", transferred);
    }

    if (!usedFallback) {
        Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());

        TDF_LabelSequence rootLabels;
        shapeTool->GetFreeShapes(rootLabels);

        result->root = std::make_shared<AssemblyNode>();
        result->root->id = "root";
        result->root->name = "Root";
        result->root->type = AssemblyNode::Type::Root;

        for (Standard_Integer i = 1; i <= rootLabels.Length(); ++i) {
            traverseAssembly(shapeTool, rootLabels.Value(i), result->root, Mat4(1.0f));
        }

        // Post-process: detect instances by shape key
        std::unordered_map<std::string, std::string> shapeToPrototype;
        for (auto& [id, node] : result->partsById) {
            if (node->type == AssemblyNode::Type::Part && !node->isInstance) {
                if (shapeToPrototype.find(node->shapeKey) == shapeToPrototype.end()) {
                    shapeToPrototype[node->shapeKey] = node->id;
                } else {
                    node->isInstance = true;
                    node->prototypeId = shapeToPrototype[node->shapeKey];
                    result->instances[node->prototypeId].push_back(node);
                }
            }
        }

        MF_INFO("Assembly: {} root shapes, {} total parts, {} instances",
                rootLabels.Length(), result->partsById.size(), result->instances.size());
    }

    // Save to BRep binary cache for next time (instant reload)
    if (!cacheDir.empty()) {
        saveToCache(cacheDir);
    }

    return result;
}

std::shared_ptr<STEPResult> STEPReader::Impl::readIGESFlat(const std::string& filepath) {
    result = std::make_shared<STEPResult>();

    IGESControl_Reader reader;
    auto readStart = std::chrono::steady_clock::now();
    IFSelect_ReturnStatus status = reader.ReadFile(filepath.c_str());
    auto readEnd = std::chrono::steady_clock::now();
    if (status != IFSelect_RetDone) {
        MF_ERROR("Failed to read IGES file: {}", filepath);
        return nullptr;
    }
    MF_INFO("IGES ReadFile: {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(readEnd - readStart).count());

    auto transferStart = std::chrono::steady_clock::now();
    Standard_Integer transferred = 0;
    try {
        transferred = reader.TransferRoots();
    } catch (const Standard_Failure& e) {
        MF_ERROR("IGES transfer failed: {}", e.GetMessageString() ? e.GetMessageString() : "unknown");
        return nullptr;
    } catch (...) {
        MF_ERROR("IGES transfer failed with unknown exception");
        return nullptr;
    }
    auto transferEnd = std::chrono::steady_clock::now();
    MF_INFO("IGES TransferRoots: {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(transferEnd - transferStart).count());

    result->root = std::make_shared<AssemblyNode>();
    result->root->id = "root";
    result->root->name = "Root";
    result->root->type = AssemblyNode::Type::Root;
    result->entityCount = static_cast<size_t>(std::max<Standard_Integer>(transferred, 0));

    Standard_Integer shapeCount = reader.NbShapes();
    if (shapeCount <= 0) {
        MF_ERROR("IGES reader transferred 0 shapes");
        return nullptr;
    }

    for (Standard_Integer i = 1; i <= shapeCount; ++i) {
        TopoDS_Shape shape = reader.Shape(i);
        if (shape.IsNull()) continue;

        std::string shapeKey = "iges_shape_" + std::to_string(i) + "_" + std::to_string(shape.ShapeType());
        result->shapesByKey[shapeKey] = shape;

        auto part = std::make_shared<AssemblyNode>();
        part->id = "iges_part_" + std::to_string(i);
        part->name = "IGES Part " + std::to_string(i);
        part->type = AssemblyNode::Type::Part;
        part->shapeKey = shapeKey;
        part->localTransform = Mat4(1.0f);
        part->parent = result->root;
        result->root->children.push_back(part);
        result->partsById[part->id] = part;
    }

    MF_INFO("IGES flat import: {} transferred roots, {} shapes, {} parts",
            transferred, result->shapesByKey.size(), result->partsById.size());
    return result;
}

void STEPReader::Impl::traverseAssembly(const Handle(XCAFDoc_ShapeTool)& shapeTool,
                                        const TDF_Label& label,
                                        AssemblyNodePtr parent,
                                        const Mat4& parentTransform) {
    if (label.IsNull()) return;

    auto node = std::make_shared<AssemblyNode>();
    node->name = readName(label);

    static uint64_t genId = 1;
    node->id = node->name + "_" + std::to_string(genId++);

    // Get local transform (OCCT 7.9 returns TopLoc_Location by value)
    // For a component label in an assembly, this returns the placement of
    // the component relative to its parent assembly.
    TopLoc_Location loc = XCAFDoc_ShapeTool::GetLocation(label);
    gp_Trsf trsf = loc.Transformation();
    node->localTransform = toMat4(trsf);

    Mat4 world = parentTransform * node->localTransform;

    // Determine the "effective" label for type checking:
    // For a reference label (e.g., an assembly component that points to
    // a sub-assembly or a shape definition), we must check the referred
    // label's type, not the reference label's type.
    TDF_Label refLabel;
    TDF_Label effectiveLabel = label;
    if (shapeTool->GetReferredShape(label, refLabel)) {
        effectiveLabel = refLabel;
    }

    if (shapeTool->IsAssembly(effectiveLabel)) {
        node->type = AssemblyNode::Type::Assembly;
        parent->children.push_back(node);
        node->parent = parent;

        TDF_LabelSequence components;
        shapeTool->GetComponents(effectiveLabel, components);
        for (Standard_Integer i = 1; i <= components.Length(); ++i) {
            traverseAssembly(shapeTool, components.Value(i), node, world);
        }
    } else if (shapeTool->IsSimpleShape(effectiveLabel) || shapeTool->IsShape(effectiveLabel)) {
        node->type = AssemblyNode::Type::Part;

        // Get the shape from the effective label (the referred label, not the
        // component reference). The component placement is stored on the
        // assembly node; any top-level location carried by the shape is folded
        // into that node so tessellation always sees a local shape.
        TopoDS_Shape shp;
        shapeTool->GetShape(effectiveLabel, shp);
        if (!shp.IsNull()) {
            TopLoc_Location shapeLoc = shp.Location();
            if (!shapeLoc.IsIdentity()) {
                node->localTransform = node->localTransform * toMat4(shapeLoc.Transformation());
                shp.Location(TopLoc_Location());
            }
        }
        node->shapeKey = "shape_" + labelEntryKey(effectiveLabel) + "_" + std::to_string(shp.ShapeType());
        result->shapesByKey[node->shapeKey] = shp;
        parent->children.push_back(node);
        node->parent = parent;
        result->partsById[node->id] = node;
    }
}

// ------------------------------------------------------------------
// BRep binary cache: load assembly tree + shapes from disk
// ------------------------------------------------------------------
bool STEPReader::Impl::loadFromCache(const std::string& cacheDir) {
    std::string jsonPath = cacheDir + "/assembly.json";
    if (!std::filesystem::exists(jsonPath)) return false;

    try {
        std::ifstream jin(jsonPath);
        if (!jin) return false;
        nlohmann::json j;
        jin >> j;

        if (j.value("schemaVersion", 0) != kBRepCacheSchemaVersion) {
            MF_INFO("BRep cache schema changed, reparsing STEP");
            return false;
        }

        result = std::make_shared<STEPResult>();
        result->entityCount = j.value("entityCount", size_t(0));
        result->fileScale = j.value("fileScale", 1.0);

        const nlohmann::json& cachedNodes = j.contains("nodes") ? j["nodes"] : j["parts"];

        // Reconstruct all assembly nodes. Older caches only contain parts and
        // are intentionally supported as a fallback, but new caches preserve
        // the full assembly hierarchy needed for GLB export.
        std::unordered_map<std::string, AssemblyNodePtr> nodeMap;
        for (auto& [pid, pj] : cachedNodes.items()) {
            auto node = std::make_shared<AssemblyNode>();
            node->id = pj.value("id", pid);
            node->name = pj.value("name", "Unnamed");
            node->type = static_cast<AssemblyNode::Type>(pj.value("type", 0));
            node->shapeKey = pj.value("shapeKey", "");
            node->isInstance = pj.value("isInstance", false);
            node->prototypeId = pj.value("prototypeId", "");
            if (pj.contains("transform")) {
                auto& tj = pj["transform"];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        node->localTransform[c][r] = tj[r * 4 + c].get<float>();
            }
            nodeMap[node->id] = node;
            if (node->type == AssemblyNode::Type::Part) {
                result->partsById[node->id] = node;
            }
        }

        // Rebuild parent-child links
        for (auto& [pid, pj] : cachedNodes.items()) {
            auto& node = nodeMap[pid];
            std::string pId = pj.value("parentId", "");
            if (!pId.empty() && pId != "root") {
                auto pit = nodeMap.find(pId);
                if (pit != nodeMap.end()) {
                    node->parent = pit->second;
                    pit->second->children.push_back(node);
                }
            }
        }

        // Root node
        result->root = std::make_shared<AssemblyNode>();
        result->root->id = "root";
        result->root->name = "Root";
        result->root->type = AssemblyNode::Type::Root;
        for (auto& rid : j.value("rootChildren", std::vector<std::string>{})) {
            auto it = nodeMap.find(rid);
            if (it != nodeMap.end()) {
                result->root->children.push_back(it->second);
                it->second->parent = result->root;
            }
        }

        // Instances
        if (j.contains("instances")) {
            for (auto& [protoId, ids] : j["instances"].items()) {
                for (auto& id : ids) {
                    auto it = nodeMap.find(id.get<std::string>());
                    if (it != nodeMap.end())
                        result->instances[protoId].push_back(it->second);
                }
            }
        }

        // Load shapes from .brep files
        std::string shapesDir = cacheDir + "/shapes";
        size_t n = 0;
        for (auto& [key, node] : result->partsById) {
            if (node->shapeKey.empty()) continue;
            std::string brepPath = shapesDir + "/" + node->shapeKey + ".brep";
            TopoDS_Shape shp;
            BRep_Builder b;
            if (BRepTools::Read(shp, brepPath.c_str(), b)) {
                result->shapesByKey[node->shapeKey] = shp;
                ++n;
            }
        }
        MF_INFO("BRep cache loaded: {} parts, {} shapes", result->partsById.size(), n);
        return n > 0;
    } catch (...) {
        MF_WARN("BRep cache load failed, falling back to STEP parse");
        return false;
    }
}

// ------------------------------------------------------------------
// BRep binary cache: save assembly tree + shapes to disk
// ------------------------------------------------------------------
void STEPReader::Impl::saveToCache(const std::string& cacheDir) {
    if (!result) return;
    try {
        std::filesystem::create_directories(cacheDir);
        std::string shapesDir = cacheDir + "/shapes";
        std::filesystem::create_directories(shapesDir);

        // Assembly tree JSON
        nlohmann::json j;
        j["schemaVersion"] = kBRepCacheSchemaVersion;
        j["entityCount"] = result->entityCount;
        j["fileScale"] = result->fileScale;

        std::vector<std::string> rc;
        for (auto& c : result->root->children) rc.push_back(c->id);
        j["rootChildren"] = rc;

        nlohmann::json nodesJson;
        std::function<void(const AssemblyNodePtr&)> writeNode = [&](const AssemblyNodePtr& node) {
            if (!node || node->type == AssemblyNode::Type::Root) return;
            nlohmann::json n;
            n["id"] = node->id;
            n["name"] = node->name;
            n["type"] = static_cast<int>(node->type);
            n["shapeKey"] = node->shapeKey;
            n["isInstance"] = node->isInstance;
            n["prototypeId"] = node->prototypeId;
            if (node->parent && node->parent->type != AssemblyNode::Type::Root)
                n["parentId"] = node->parent->id;
            std::vector<float> tf(16);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    tf[r * 4 + c] = node->localTransform[c][r];
            n["transform"] = tf;
            nodesJson[node->id] = n;
            for (const auto& child : node->children) {
                writeNode(child);
            }
        };
        for (auto& child : result->root->children) {
            writeNode(child);
        }
        j["nodes"] = nodesJson;

        nlohmann::json partsJson;
        for (auto& [pid, node] : result->partsById) {
            partsJson[pid] = nodesJson[node->id];
        }
        j["parts"] = partsJson;

        nlohmann::json ij;
        for (auto& [protoId, nodes] : result->instances) {
            std::vector<std::string> ids;
            for (auto& nd : nodes) ids.push_back(nd->id);
            ij[protoId] = ids;
        }
        j["instances"] = ij;

        std::ofstream jout(cacheDir + "/assembly.json");
        jout << j.dump(2);

        // Shapes as BRep binary
        size_t ns = 0;
        for (auto& [key, shape] : result->shapesByKey) {
            if (BRepTools::Write(shape, (shapesDir + "/" + key + ".brep").c_str()))
                ++ns;
        }
        MF_INFO("BRep cache saved: {} shapes to {}", ns, cacheDir);
    } catch (...) {
        MF_WARN("BRep cache save failed");
    }
}

} // namespace mf
