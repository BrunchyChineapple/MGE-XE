#include <assert.h>
#include <fstream>
#include <vector>
#include <float.h>
#include <new>

#include "progmesh/ProgMesh.h"
#include "../3rdparty/tootle/src/TootleLib/include/tootlelib.h"

// distantshader.h (in the d3d8.dll/renderer project) defines SIZEOFLANDVERT, the single stride
// constant every land-vertex consumer steps the on-disk stream by. It is a self-contained header
// (enums + two `static const int`, #pragma once, no further includes), so pulling it into this
// MGEfuncs translation unit is cheap and lets task 6.3 tie that stride to the actual struct size in
// a TU that already sees DXCompressedLandVertex (via progmesh/ProgMesh.h -> ../DXVertex.h).
#include "../src/mge/distantshader.h"

using namespace Niflib;
using std::vector;

// Cross-header layout guarantee (task 6.3; Req 8.1, 8.3, 8.4): the shared stride SIZEOFLANDVERT must
// equal the real on-disk vertex size. DXVertex.h already asserts sizeof(DXCompressedLandVertex)==20
// and the field offsets; this assert closes the loop so the stride that the IPC server skip-math,
// the client vertex-buffer build, and the stream-source stride all rely on can never drift from the
// struct the generator actually writes in LandMesh::Save below.
static_assert(static_cast<size_t>(SIZEOFLANDVERT) == sizeof(DXCompressedLandVertex),
              "SIZEOFLANDVERT (distantshader.h) must equal sizeof(DXCompressedLandVertex) (DXVertex.h)");

// Maximum ROAM subdivision depth the tessellator supports. The Mega Detail tier uses depth 12
// (leaf size 8192/2^(12/2) = 128 world units == the 65x65 source-grid spacing); every existing
// tier uses depth 10. The entry point clamps the caller-supplied tree_depth to this ceiling so the
// variance-node pool invariant cannot be violated. (Introduced by task 2.1; task 2.2 adds the
// static_assert tying RoamVarianceNode::pool capacity to this value.)
static const size_t kMaxRoamTreeDepth = 12;
static const size_t kSourceGridQuads = 64;
static const size_t kSourceGridVertices = kSourceGridQuads + 1;
static const float kSourceGridSpacing = 128.0f;

// Pre-sized capacity of the ROAM variance-node pool (RoamVarianceNode::pool, defined below). The
// pool is allocated once at this size and is NEVER resized at runtime: RoamVarianceNode::Create()
// hands out raw &pool[i] pointers that the variance tree stores as left_child/right_child, so a
// reallocation would dangle every node pointer mid-build. The pool must therefore be pre-sized to
// cover the worst case up front rather than growing on demand.
//
// Worst-case usage is one root's complete variance tree. RoamLandPatch::Tessellate builds the tree
// for the first root, tessellates, calls RoamVarianceNode::ResetPool(), then repeats for the second
// root -- so peak live usage is a single root, not both. CalculateVariance allocates two children
// per non-leaf node down to `depth`, giving 2^(depth+1)-1 nodes for a full tree; Create() starts
// handing out at index 1 (pre-increment), so it needs one extra slot, i.e. the capacity must reach
// 2^(kMaxRoamTreeDepth+1) (8192 nodes at depth 12). The existing 32768 covers depth 12 with ~4x
// margin and is left pre-sized as-is.
static const size_t kVariancePoolSize = 32768;

// Compile-time guarantee that the pre-sized pool holds one full root variance tree at the deepest
// supported tier. If kMaxRoamTreeDepth is ever raised past what kVariancePoolSize covers, this
// fails the build instead of letting Create() throw the Pool_Full_Error at bake time (Req 5.1,
// 5.2). 2^(kMaxRoamTreeDepth+1) is a strict upper bound on one root's node count (2^(d+1)-1) and
// also folds in the +1 headroom slot Create()'s pre-increment requires.
static_assert(kVariancePoolSize >= (size_t(1) << (kMaxRoamTreeDepth + 1)),
              "RoamVarianceNode::pool must hold one full root variance tree (2^(kMaxRoamTreeDepth+1) nodes)");

// Encode one signed normal component [-1,1] to a UBYTE4N byte [0,255]. This mirrors the
// distant-statics vertex normal encoding (NifConverter.cpp: 255 * (n*0.5+0.5)) so the runtime FFP
// decode (b/255)*2-1 round-trips identically for land and statics. Rounds to nearest and clamps so
// any tiny float overshoot from renormalization cannot wrap the byte.
static inline unsigned char EncodeNormalByte(float c) {
    float u = (c * 0.5f + 0.5f) * 255.0f;
    if (u < 0.0f) { u = 0.0f; }
    if (u > 255.0f) { u = 255.0f; }
    return (unsigned char)(u + 0.5f);
}

struct LargeTriangle {
    unsigned int v1; /*!< The index of the first vertex. */
    unsigned int v2; /*!< The index of the second vertex. */
    unsigned int v3; /*!< The index of the third vertex. */

    LargeTriangle() {}

    LargeTriangle(unsigned int v1, unsigned int v2, unsigned int v3) {
        this->v1 = v1;
        this->v2 = v2;
        this->v3 = v3;
    }

    void Set(unsigned int v1, unsigned int v2, unsigned int v3) {
        this->v1 = v1;
        this->v2 = v2;
        this->v3 = v3;
    }

    unsigned int& operator[](int n) {
        switch (n) {
        case 0:
            return v1;
            break;
        case 1:
            return v2;
            break;
        case 2:
            return v3;
            break;
        default:
            throw std::out_of_range("Index out of range for Triangle");
        };
    }
    unsigned int operator[](int n) const {
        switch (n) {
        case 0:
            return v1;
            break;
        case 1:
            return v2;
            break;
        case 2:
            return v3;
            break;
        default:
            throw std::out_of_range("Index out of range for Triangle");
        };
    }
};

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE value) : handle(value) {}
    ~ScopedFileHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }

    HANDLE Get() const { return handle; }
    bool IsValid() const { return handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle;
    ScopedFileHandle(const ScopedFileHandle&);
    ScopedFileHandle& operator=(const ScopedFileHandle&);
};

static bool SeekFile(HANDLE file, LONGLONG offset, DWORD origin) {
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    return SetFilePointerEx(file, distance, NULL, origin) != FALSE;
}

static bool ReadExact(HANDLE file, void* data, DWORD byte_count) {
    DWORD transferred = 0;
    if (ReadFile(file, data, byte_count, &transferred, NULL) == FALSE) {
        return false;
    }
    if (transferred != byte_count) {
        SetLastError(ERROR_HANDLE_EOF);
        return false;
    }
    return true;
}

static bool WriteExact(HANDLE file, const void* data, DWORD byte_count) {
    DWORD transferred = 0;
    if (WriteFile(file, data, byte_count, &transferred, NULL) == FALSE) {
        return false;
    }
    if (transferred != byte_count) {
        SetLastError(ERROR_WRITE_FAULT);
        return false;
    }
    return true;
}

class LandMesh {
public:
    vector<Vector3> vertices;
    vector<LargeTriangle> triangles;
    vector<TexCoord> uvs;
    // Per-vertex surface normals, parallel to `vertices`. Filled by GenerateMesh alongside the UV
    // loop via HeightFieldSampler::SampleNormal (task 5.1). Save() encodes these into the on-disk
    // DXCompressedLandVertex::Normal field; when this is empty (e.g. before task 5.1 populates it)
    // Save falls back to a straight-up (0,0,1) normal so output is always well-defined.
    vector<Vector3> normals;
    float radius;
    Vector3 center;
    Vector3 min;
    Vector3 max;

    void CalcBounds(const Vector3& new_min, const Vector3& new_max) {

        min = new_min;
        max = new_max;

        // Average min/max positions to get center
        center = (min + max) / 2.0f;

        // Find the furthest point from the center to get the radius
        float radius_sqared = 0.0f;

        for (size_t i = 0; i < vertices.size(); ++i) {
            float x, y, z;
            x = vertices[i].x;
            y = vertices[i].y;
            z = vertices[i].z;

            float dist = (x-center.x)*(x-center.x) + (y-center.y)*(y-center.y) + (z-center.z)*(z-center.z);

            if (dist > radius_sqared) {
                radius_sqared = dist;
            }
        }

        radius = sqrt(radius_sqared);
    }

    static bool SaveMeshes(LPCSTR file_path, vector<LandMesh>& meshes) {
        ScopedFileHandle file(CreateFileA(file_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
        if (!file.IsValid()) {
            return false;
        }

        LARGE_INTEGER file_size;
        if (GetFileSizeEx(file.Get(), &file_size) == FALSE) {
            return false;
        }

        DWORD mesh_count = 0;
        if (file_size.QuadPart > 0) {
            if (file_size.QuadPart < static_cast<LONGLONG>(sizeof(mesh_count)) ||
                !SeekFile(file.Get(), 0, FILE_BEGIN) ||
                !ReadExact(file.Get(), &mesh_count, sizeof(mesh_count))) {
                return false;
            }
        } else if (!WriteExact(file.Get(), &mesh_count, sizeof(mesh_count))) {
            return false;
        }

        if (!SeekFile(file.Get(), 0, FILE_END)) {
            return false;
        }

        for (size_t i = 0; i < meshes.size(); ++i) {
            if (mesh_count == MAXDWORD) {
                SetLastError(ERROR_ARITHMETIC_OVERFLOW);
                return false;
            }
            if (!meshes[i].Save(file.Get())) {
                return false;
            }
            ++mesh_count;
        }

        return SeekFile(file.Get(), 0, FILE_BEGIN) &&
               WriteExact(file.Get(), &mesh_count, sizeof(mesh_count));
    }

    bool Save(HANDLE file) {
        if (vertices.size() > MAXDWORD || triangles.size() > MAXDWORD ||
            uvs.size() < vertices.size()) {
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }

        DWORD verts = static_cast<DWORD>(vertices.size());
        DWORD faces = static_cast<DWORD>(triangles.size());
        bool large = verts > 0xFFFF || faces > 0xFFFF;

        if (!WriteExact(file, &radius, sizeof(radius)) ||
            !WriteExact(file, &center, sizeof(center)) ||
            !WriteExact(file, &min, sizeof(min)) ||
            !WriteExact(file, &max, sizeof(max)) ||
            !WriteExact(file, &verts, sizeof(verts)) ||
            !WriteExact(file, &faces, sizeof(faces))) {
            return false;
        }

        vector<DXCompressedLandVertex> compVerts(verts);
        for (size_t i = 0; i < verts; ++i) {
            compVerts[i].Position = vertices[i];
            compVerts[i].texCoord[0] = (short)(uvs[i].u * 32768.0f);
            compVerts[i].texCoord[1] = (short)(uvs[i].v * 32768.0f);

            // Encode the per-vertex surface normal as UBYTE4N (xyz, w pad), the same encoding the
            // distant-statics vertex uses, so the runtime FFP decode is identical for both paths.
            Vector3 n(0.0f, 0.0f, 1.0f);
            if (i < normals.size()) {
                n = normals[i];
            }
            compVerts[i].Normal[0] = EncodeNormalByte(n.x);
            compVerts[i].Normal[1] = EncodeNormalByte(n.y);
            compVerts[i].Normal[2] = EncodeNormalByte(n.z);
            compVerts[i].Normal[3] = 0;
        }

        if (verts > 0 &&
            !WriteExact(file, &compVerts[0], static_cast<DWORD>(sizeof(DXCompressedLandVertex) * verts))) {
            return false;
        }

        size_t index_count = static_cast<size_t>(faces) * 3;
        if (large) {
            vector<unsigned int> indices(index_count);
            for (size_t i = 0; i < faces; ++i) {
                indices[i * 3 + 0] = triangles[i].v1;
                indices[i * 3 + 1] = triangles[i].v2;
                indices[i * 3 + 2] = triangles[i].v3;
            }
            return indices.empty() ||
                   WriteExact(file, &indices[0], static_cast<DWORD>(indices.size() * sizeof(indices[0])));
        }

        vector<unsigned short> indices(index_count);
        for (size_t i = 0; i < faces; ++i) {
            indices[i * 3 + 0] = static_cast<unsigned short>(triangles[i].v1);
            indices[i * 3 + 1] = static_cast<unsigned short>(triangles[i].v2);
            indices[i * 3 + 2] = static_cast<unsigned short>(triangles[i].v3);
        }
        return indices.empty() ||
               WriteExact(file, &indices[0], static_cast<DWORD>(indices.size() * sizeof(indices[0])));
    }
};

class HeightFieldSampler {
public:
    struct AtlasRegion {
        float minX, maxX, minY, maxY;
        float offsetX, offsetY;
        float scaleX, scaleY;
    };

    float minX, minY, maxX, maxY;
    float* data;
    // Parallel per-sample surface-normal field, same grid as `data` (the height field) with three
    // floats (x,y,z) per sample, indexed normal_data[(y*data_width + x)*3 + component]. Supplied by
    // the TessellateLandscapeAtlased entry point (task 2.1 plumbing); SampleNormal consumes it
    // (task 5.1). May be null only if a caller passes no normals, in which case SampleNormal must
    // not be called -- the generator always passes a parallel field alongside height_data.
    float* normal_data;
    size_t data_width, data_height;
    AtlasRegion* atlas_data;
    size_t atlas_count;

    HeightFieldSampler(float* d, float* nd, size_t dw, size_t dh, float* adata, size_t ac, float _minX, float _minY, float _maxX, float _maxY) :
         minX(_minX), minY(_minY), maxX(_maxX), maxY(_maxY), data(d), normal_data(nd), data_width(dw), data_height(dh),
        atlas_data(reinterpret_cast<AtlasRegion*>(adata)), atlas_count(ac) {}
    ~HeightFieldSampler() {}

    TexCoord SampleTexCoord(float x, float y) {
        for (size_t n = 0; n < atlas_count; ++n) {
            AtlasRegion* r = &atlas_data[n];

            if (x >= r->minX && x <= r->maxX && y >= r->minY && y <= r->maxY) {
                float tx = (x - r->minX + r->offsetX) / r->scaleX;
                float ty = (y - r->minY + r->offsetY) / r->scaleY;
                return TexCoord(tx, 1.0f - ty);
            }
        }
        return TexCoord(0, 0);
    }

    float SampleHeight(float x, float y) {
        // Figure which height values to sample.
        int low_x, high_x, low_y, high_y;

        float data_x = (x - minX) / 128.0f;
        float data_y = (y - minY) / 128.0f;

        low_x = (int)floor(data_x);
        high_x = (int)ceil(data_x);
        low_y = (int)floor(data_y);
        high_y = (int)ceil(data_y);

        // Bilinear interpolation
        float x_interp = data_x - (float)low_x;
        float y_interp = data_y - (float)low_y;

        // horizontal
        float bottom_val = GetHeightValue(low_x, low_y) * (1.0f - x_interp) + GetHeightValue(high_x, low_y) * x_interp;
        float top_val = GetHeightValue(low_x, high_y) * (1.0f - x_interp) + GetHeightValue(high_x, high_y) * x_interp;

        // vertical
        float result = top_val * (1.0f - y_interp) + bottom_val * y_interp;
        return result;
    }

    // Bilinearly sample the per-vertex surface normal at world (x, y), then renormalize. This uses
    // EXACTLY the same low/high grid indices and bilinear weights as SampleHeight above (same
    // (coord - min)/128 mapping, floor/ceil bracket, and interp weights), applied componentwise to
    // the parallel normal_data field via the edge-clamped GetNormalValue. Because ceil(integer) ==
    // integer with a zero weight, a vertex landing exactly on a source-grid sample returns that
    // sample's normalized normal; a vertex between samples (ROAM emits edge midpoints) returns the
    // normalized bilinear blend of the four surrounding samples, so the normal is defined
    // everywhere (Req 7.4 -- no undefined normals between samples). A degenerate (near-zero-length)
    // blend falls back to straight-up (0,0,1) rather than emitting a NaN (design Error Handling).
    Vector3 SampleNormal(float x, float y) {
        // Figure which normal samples to read (identical index math to SampleHeight).
        int low_x, high_x, low_y, high_y;

        float data_x = (x - minX) / 128.0f;
        float data_y = (y - minY) / 128.0f;

        low_x = (int)floor(data_x);
        high_x = (int)ceil(data_x);
        low_y = (int)floor(data_y);
        high_y = (int)ceil(data_y);

        // Bilinear interpolation weights (identical to SampleHeight).
        float x_interp = data_x - (float)low_x;
        float y_interp = data_y - (float)low_y;

        // horizontal lerp of the bottom and top rows, then vertical lerp -- componentwise via the
        // Vector3 scalar operators, mirroring SampleHeight's scalar expression.
        Vector3 bottom_val = GetNormalValue(low_x, low_y) * (1.0f - x_interp) + GetNormalValue(high_x, low_y) * x_interp;
        Vector3 top_val = GetNormalValue(low_x, high_y) * (1.0f - x_interp) + GetNormalValue(high_x, high_y) * x_interp;

        // vertical
        Vector3 n = top_val * (1.0f - y_interp) + bottom_val * y_interp;

        // Renormalize the blended normal to unit length; fall back to up on a degenerate blend.
        float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len > 1e-6f) {
            return Vector3(n.x / len, n.y / len, n.z / len);
        }
        return Vector3(0.0f, 0.0f, 1.0f);
    }

    float GetHeightValue(int x, int y) {
        if (x < 0) {
            x = 0;
        }
        if (x > data_width - 1) {
            x = data_width - 1;
        }
        if (y < 0) {
            y = 0;
        }
        if (y > data_height - 1) {
            y = data_height - 1;
        }

        return data[y * data_width + x];
    }

    // Read the raw (possibly non-unit) source normal at integer grid index (x, y), clamping to the
    // grid edge exactly like GetHeightValue does for the height field. The normal field is flat
    // xyz-per-sample (parallel to `data`), so the sample at (x, y) lives at base (y*width + x)*3.
    Vector3 GetNormalValue(int x, int y) {
        if (x < 0) {
            x = 0;
        }
        if (x > data_width - 1) {
            x = data_width - 1;
        }
        if (y < 0) {
            y = 0;
        }
        if (y > data_height - 1) {
            y = data_height - 1;
        }

        size_t base = (size_t(y) * data_width + size_t(x)) * 3;
        return Vector3(normal_data[base], normal_data[base + 1], normal_data[base + 2]);
    }
};

class SplitTriangle {
public:
    Vector3 left, right, top;
    SplitTriangle(const Vector3& left, const Vector3& right, const Vector3& top) {
        this->left = left;
        this->right = right;
        this->top = top;
    }
    ~SplitTriangle() {}

    Vector3 GetHypoCenter() const {
        return (right + left) / 2.0f;
    }

    SplitTriangle LeftSplit(const Vector3& new_vert) const {
        return SplitTriangle(right, top, new_vert);
    }

    SplitTriangle RightSplit(const Vector3& new_vert) const {
        return SplitTriangle(top, left, new_vert);
    }
};

class RoamVarianceNode {
public:
    float variance;
    RoamVarianceNode* left_child;
    RoamVarianceNode* right_child;

private:
    static size_t last_used_index;
    static vector<RoamVarianceNode> pool;


public:
    static RoamVarianceNode* Create() {
        // Make sure pool can hold this object
        if (last_used_index + 1 >= pool.size()) {
            // No more room RoamVarianceNode objects to hand out.
            throw std::runtime_error("RoamVarianceNode pool full.");
        }

        // increment the last used index and return the object at that position
        ++last_used_index;
        // Clear out any data in the object so it's as if it's a new object
        pool[last_used_index].ClearChildren();

        return &pool[last_used_index];
    }

    // This shouldn't be called directly, used Create instead to allocate with the pool
    RoamVarianceNode() : variance(0.0f), left_child(0), right_child(0) {}

    static void ResetPool() {
        // Move the index of the next node to be handed out back to the begining.
        // There should be no outstanding RoamVarianceNode pointers when this is called.
        last_used_index = 0;
    }

    void ClearChildren() {
        left_child = 0;
        right_child = 0;
    }

    ~RoamVarianceNode() {}

    void CalculateVariance(HeightFieldSampler* sampler, const SplitTriangle& tri, size_t depth) {
        // On the downward pass, calculate the variance as the difference in height between the
        // average of left and right, and the the real height value as given by the sampler
        Vector3 avg = tri.GetHypoCenter();
        float samp_height = sampler->SampleHeight(avg.x, avg.y);

        variance = abs(avg.z - samp_height);

        // Give extra weight to the split if it causes the vertex to switch from being above the water to being below the water or vice versa
        // Water level is zero
        if ((avg.z > 0.0f && samp_height < 0.0f) || (samp_height > 0.0f && avg.z < 0.0f)) {
            variance *= 4.0f;
        }

        // Give extra weight to the split if it the average vertex would be above the real height
        if (avg.z > samp_height) {
            variance *= 4.0f;
        }

        // If we have reached the maximum depth, free any children and start unwiding traversal
        if (depth == 0) {
            ClearChildren();
            return;
        }

        // If we have not reached the maximum depth, create children and call this function on them
        avg.z = samp_height;

        if (!right_child) {
            right_child = Create();
        }
        right_child->CalculateVariance(sampler, tri.RightSplit(avg), depth - 1);


        if (!left_child) {
            left_child = Create();
        }
        left_child->CalculateVariance(sampler, tri.LeftSplit(avg), depth - 1);

        // We want the variance of this node to represent the maximum variance of all children nodes, so choose the highest of
        // the variances as the new variance
        if (right_child->variance > variance) {
            variance = right_child->variance;
        }
        if (left_child->variance > variance) {
            variance = left_child->variance;
        }
    }
};

size_t RoamVarianceNode::last_used_index = 0;
// Pre-sized to kVariancePoolSize and never resized at runtime (see kVariancePoolSize above):
// Create() returns raw &pool[i] pointers stored as left_child/right_child, so a realloc would
// dangle them. The static_assert above guarantees this capacity covers one full root variance
// tree at kMaxRoamTreeDepth, so Create()'s throw stays an unreachable guard in normal operation.
vector<RoamVarianceNode> RoamVarianceNode::pool(kVariancePoolSize);

class RoamTriangleNode;

class RenderTriangle {
public:
    SplitTriangle st;
    unsigned int left_index, right_index, top_index;
    RoamTriangleNode* rt;

    RenderTriangle(const SplitTriangle& s_tri, RoamTriangleNode* r_tri) : st(s_tri), rt(r_tri) {
        left_index = 0xFFFFFFFF;
        right_index = 0xFFFFFFFF;
        top_index = 0xFFFFFFFF;
    }
    ~RenderTriangle() {}
};

class RoamTriangleNode {
public:
    RoamTriangleNode* left_child;
    RoamTriangleNode* right_child;
    RoamTriangleNode* base_neighbor;
    RoamTriangleNode* left_neighbor;
    RoamTriangleNode* right_neighbor;

    RoamTriangleNode() : left_child(0), right_child(0), base_neighbor(0), left_neighbor(0), right_neighbor(0) {}
    ~RoamTriangleNode() {
        if (left_child) {
            delete left_child;
        }
        if (right_child) {
            delete right_child;
        }
    }

    void Split() {
        // Check whether this node has already been split
        if (left_child || right_child) {
            // This node has already been split
            return;
        }

        // Check if the hypotonuse of this triangle is on an edge (has no base neighbor)
        if (!base_neighbor) {
            // This is on an edge, so split this triangle only
            EdgeSplit();
            return;
        }

        // Check if this triangle and its base neighbor form a diamond (they are eachother's base neighbor)
        if (base_neighbor->base_neighbor == this) {
            // split this triangle and its neighbor
            DiamondSplit();
            return;
        }

        // These triangles don't form a diamond, so call split on the base neighbor before splitting this triangle
        base_neighbor->Split();

        // Split the triangle and its neighbor
        DiamondSplit();
    }

private:
    void EdgeSplit() {
        assert(left_child == 0 && right_child == 0);

        // Create children
        left_child = new RoamTriangleNode();
        right_child = new RoamTriangleNode();

        // Set neighbor linkage
        left_child->base_neighbor = left_neighbor;
        left_child->left_neighbor = right_child;
        left_child->right_neighbor = 0;

        RelinkNeighbor(left_child->left_neighbor, this, left_child);
        RelinkNeighbor(left_child->base_neighbor, this, left_child);

        right_child->base_neighbor = right_neighbor;
        right_child->left_neighbor = 0;
        right_child->right_neighbor = left_child;

        RelinkNeighbor(right_child->left_neighbor, this, right_child);
        RelinkNeighbor(right_child->base_neighbor, this, right_child);

        // Clear neighbors of this object since it now just represents a node, not a real triangle
        // Don't clear base_neighbor since it should either already be null or cleared by the DiamondSplit function
        right_neighbor = 0;
        left_neighbor = 0;
    }

    void RelinkNeighbor(RoamTriangleNode* neighbor, RoamTriangleNode* old_link, RoamTriangleNode* new_link) {
        if (neighbor) {
            if (neighbor->base_neighbor == old_link) {
                neighbor->base_neighbor = new_link;
            } else if (neighbor->left_neighbor == old_link) {
                neighbor->left_neighbor = new_link;
            } else if (neighbor->right_neighbor == old_link) {
                neighbor->right_neighbor = new_link;
            }
        }
    }

    void DiamondSplit() {
        assert(base_neighbor != 0 && base_neighbor->base_neighbor == this);

        // Start with an edge split for both triangles
        EdgeSplit();
        base_neighbor->EdgeSplit();

        // Now set left and right neighbor links which are set to zero by the basic edge split
        right_child->left_neighbor = base_neighbor->left_child;
        left_child->right_neighbor = base_neighbor->right_child;

        base_neighbor->right_child->left_neighbor = left_child;
        base_neighbor->left_child->right_neighbor = right_child;

        // Clear base_neighbors of these objects since they now just represents a node, not a real triangle
        base_neighbor->base_neighbor = 0;
        base_neighbor = 0;

    }
public:
    void Tessellate(RoamVarianceNode* v_tri, float max_variance) {
        // Determine if the variance of this triangle is enough that we want to split it
        if (v_tri->variance < max_variance) {
            // The variance is within tolerable levels, so end the recursion of this branch
            return;
        }

        // The variance is too high, so we need to split this triangle
        Split();

        // If the variance triangle has children, continue on down the tree recursivly
        if (!v_tri->left_child || !v_tri->right_child) {
            // The variance triangle has no children, so we're done
            return;
        }

        // The variance triangle has children so call this function on the newly created children of this roam triangle node
        left_child->Tessellate(v_tri->left_child, max_variance);
        right_child->Tessellate(v_tri->right_child, max_variance);
    }

    void GatherTriangles(HeightFieldSampler* sampler, const SplitTriangle& s_tri, vector<RenderTriangle>& triangles) {
        Vector3 hc = s_tri.GetHypoCenter();
        hc.z = sampler->SampleHeight(hc.x, hc.y);

        if (!left_child || !right_child) {
            // This node has no children, so add the triangle to the list and return
            triangles.push_back(RenderTriangle(s_tri, this));
            return;
        }

        // This node has children, so call this function on them
        left_child->GatherTriangles(sampler, s_tri.LeftSplit(hc), triangles);
        right_child->GatherTriangles(sampler, s_tri.RightSplit(hc), triangles);
    }
};

class CmpVec3 : public Vector3 {
public:
    CmpVec3(const Vector3& v) {
        x = v.x;
        y = v.y;
        z = v.z;
    }

    CmpVec3& operator=(const Vector3& rh) {
        x = rh.x;
        y = rh.y;
        z = rh.z;
        return *this;
    }

    Vector3 AsVec3() {
        return Vector3(x,y,z);
    }

    static bool FltEq(float lh, float rh) {
        return abs(lh - rh) < 0.0001f;
    }

    static bool FltLt(float lh, float rh) {
        return lh - 0.0001f < rh;
    }

    bool operator==(const CmpVec3& rh) const {
        return FltEq(x, rh.x) && FltEq(y, rh.y) && FltEq(z, rh.z);
    }

    friend bool operator<(const CmpVec3& lh, const CmpVec3& rh) {
        if (lh == rh) {
            return false;
        }

        if (FltEq(lh.x, rh.x)) {
            if (FltEq(lh.y, rh.y)) {
                return lh.z < rh.z;
            } else {
                return lh.y < rh.y;
            }
        }

        return lh.x < rh.x;
    }
};

class RoamLandPatch {
public:
    float top, left, bottom, right;
    RoamTriangleNode t1, t2;
    HeightFieldSampler* sampler;

    RoamLandPatch() {
        t1.base_neighbor = &t2;
        t2.base_neighbor = &t1;
    }
    RoamLandPatch(float t, float l, float b, float r, HeightFieldSampler* s) : top(t), left(l), bottom(b), right(r), sampler(s) {
        t1.base_neighbor = &t2;
        t2.base_neighbor = &t1;
    }

    RoamLandPatch(const RoamLandPatch& patch) {
        sampler = patch.sampler;
        top = patch.top;
        left = patch.left;
        bottom = patch.bottom;
        right = patch.right;

        t1.base_neighbor = &t2;
        t2.base_neighbor = &t1;
    }

    ~RoamLandPatch() {}

    Vector3 GetTopLeft() {
        return Vector3(left, top, sampler->SampleHeight(left,top));
    }
    Vector3 GetBottomLeft() {
        return Vector3(left, bottom, sampler->SampleHeight(left,bottom));
    }
    Vector3 GetTopRight() {
        return Vector3(right, top, sampler->SampleHeight(right,top));
    }
    Vector3 GetBottomRight() {
        return Vector3(right, bottom, sampler->SampleHeight(right,bottom));
    }

    void ConnectRightNeighbor(RoamLandPatch& neighbor) {
        t2.right_neighbor = &neighbor.t1;
        neighbor.t1.right_neighbor = &t2;
    }

    void ConnectLeftNeighbor(RoamLandPatch& neighbor) {
        t1.right_neighbor = &neighbor.t2;
        neighbor.t2.right_neighbor = &t1;
    }

    void ConnectTopNeighbor(RoamLandPatch& neighbor) {
        t2.left_neighbor = &neighbor.t1;
        neighbor.t1.left_neighbor = &t2;
    }

    void ConnectBottomNeighbor(RoamLandPatch& neighbor) {
        t1.left_neighbor = &neighbor.t2;
        neighbor.t2.left_neighbor = &t1;
    }

    void Tessellate(float max_variance, size_t tree_depth) {
        Vector3 top_left = GetTopLeft();
        Vector3 bottom_left = GetBottomLeft();
        Vector3 top_right = GetTopRight();
        Vector3 bottom_right = GetBottomRight();

        RoamVarianceNode* variance_root = RoamVarianceNode::Create();
        variance_root->CalculateVariance(sampler, SplitTriangle(bottom_right, top_left, bottom_left), tree_depth);
        t1.Tessellate(variance_root, max_variance);
        variance_root = 0;
        RoamVarianceNode::ResetPool();

        variance_root = RoamVarianceNode::Create();
        variance_root->CalculateVariance(sampler, SplitTriangle(top_left, bottom_right, top_right), tree_depth);
        t2.Tessellate(variance_root, max_variance);
        variance_root = 0;

        RoamVarianceNode::ResetPool();
    }

    // Mega Detail's depth-12 contract is the complete 65x65 source grid. Emit that grid directly
    // instead of materializing a persistent ROAM node tree for every cell in a large atlas region.
    LandMesh GenerateFullResolutionMesh(unsigned int cache_size) {
        LandMesh mesh;
        const size_t vertex_count = kSourceGridVertices * kSourceGridVertices;
        const size_t triangle_count = kSourceGridQuads * kSourceGridQuads * 2;
        mesh.vertices.reserve(vertex_count);
        mesh.triangles.reserve(triangle_count);

        Vector3 max = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        Vector3 min = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);

        for (size_t y = 0; y < kSourceGridVertices; ++y) {
            float world_y = bottom + (float)y * kSourceGridSpacing;
            for (size_t x = 0; x < kSourceGridVertices; ++x) {
                float world_x = left + (float)x * kSourceGridSpacing;
                Vector3 vertex(world_x, world_y, sampler->SampleHeight(world_x, world_y));
                mesh.vertices.push_back(vertex);

                if (vertex.x > max.x) { max.x = vertex.x; }
                if (vertex.y > max.y) { max.y = vertex.y; }
                if (vertex.z > max.z) { max.z = vertex.z; }
                if (vertex.x < min.x) { min.x = vertex.x; }
                if (vertex.y < min.y) { min.y = vertex.y; }
                if (vertex.z < min.z) { min.z = vertex.z; }
            }
        }

        for (size_t y = 0; y < kSourceGridQuads; ++y) {
            for (size_t x = 0; x < kSourceGridQuads; ++x) {
                unsigned int bottom_left = (unsigned int)(y * kSourceGridVertices + x);
                unsigned int bottom_right = bottom_left + 1;
                unsigned int top_left = bottom_left + (unsigned int)kSourceGridVertices;
                unsigned int top_right = top_left + 1;
                mesh.triangles.push_back(LargeTriangle(top_left, bottom_left, bottom_right));
                mesh.triangles.push_back(LargeTriangle(bottom_right, top_right, top_left));
            }
        }

        mesh.CalcBounds(min, max);

        unsigned int* iBuffer = (unsigned int*)&mesh.triangles[0];
        void* vBuffer = (void*)&mesh.vertices[0];
        unsigned int verts = (unsigned int)mesh.vertices.size();
        unsigned int faces = (unsigned int)mesh.triangles.size();
        unsigned int stride = 3 * sizeof(float);

        TootleResult result = TootleOptimizeVCache(iBuffer, faces, verts, cache_size, iBuffer, NULL, TOOTLE_VCACHE_AUTO);
        if (result != TOOTLE_OK) {
            SetLastError(ERROR_INVALID_DATA);
            return LandMesh();
        }

        result = TootleOptimizeVertexMemory(vBuffer, iBuffer, verts, faces, stride, vBuffer, iBuffer, NULL);
        if (result != TOOTLE_OK) {
            SetLastError(ERROR_INVALID_DATA);
            return LandMesh();
        }

        mesh.uvs.reserve(mesh.vertices.size());
        mesh.normals.reserve(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            mesh.uvs.push_back(sampler->SampleTexCoord(mesh.vertices[i].x, mesh.vertices[i].y));
            mesh.normals.push_back(sampler->SampleNormal(mesh.vertices[i].x, mesh.vertices[i].y));
        }

        return mesh;
    }

    LandMesh GenerateMesh(unsigned int cache_size) {
        LandMesh mesh;
        Vector3 top_left = GetTopLeft();
        Vector3 bottom_left = GetBottomLeft();
        Vector3 top_right = GetTopRight();
        Vector3 bottom_right = GetBottomRight();

        vector<RenderTriangle> render_triangles;

        t1.GatherTriangles(sampler, SplitTriangle(bottom_right, top_left, bottom_left), render_triangles);
        t2.GatherTriangles(sampler, SplitTriangle(top_left, bottom_right, top_right), render_triangles);

        // Walk through the triangles that were gathered, adding their vertices and indices to a map
        Vector3 max = Vector3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        Vector3 min = Vector3(FLT_MAX, FLT_MAX, FLT_MAX);

        map<CmpVec3, size_t> vert_map;
        size_t render_triangles_size = render_triangles.size();
        for (size_t i = 0; i < render_triangles_size; ++i) {
            size_t right_index, left_index, top_index;
            map<CmpVec3, size_t>::iterator it;

            it = vert_map.find(render_triangles[i].st.left);
            if (it == vert_map.end()) {
                // Add new index
                left_index = mesh.vertices.size();
                Vector3& v = render_triangles[i].st.left;
                vert_map[v] = left_index;
                mesh.vertices.push_back(v);

                if (v.x > max.x) { max.x = v.x; }
                if (v.y > max.y) { max.y = v.y; }
                if (v.z > max.z) { max.z = v.z; }

                if (v.x < min.x) { min.x = v.x; }
                if (v.y < min.y) { min.y = v.y; }
                if (v.z < min.z) { min.z = v.z; }
            } else {
                // Get index from existing vertex
                left_index = it->second;
            }

            it = vert_map.find(render_triangles[i].st.right);
            if (it == vert_map.end()) {
                // Add new index
                right_index = mesh.vertices.size();
                Vector3& v = render_triangles[i].st.right;
                vert_map[v] = right_index;
                mesh.vertices.push_back(v);

                if (v.x > max.x) { max.x = v.x; }
                if (v.y > max.y) { max.y = v.y; }
                if (v.z > max.z) { max.z = v.z; }

                if (v.x < min.x) { min.x = v.x; }
                if (v.y < min.y) { min.y = v.y; }
                if (v.z < min.z) { min.z = v.z; }
            } else {
                // Get index from existing vertex
                right_index = it->second;
            }

            it = vert_map.find(render_triangles[i].st.top);
            if (it == vert_map.end()) {
                // Add new index
                top_index = mesh.vertices.size();
                Vector3& v = render_triangles[i].st.top;
                vert_map[v] = top_index;
                mesh.vertices.push_back(v);

                if (v.x > max.x) { max.x = v.x; }
                if (v.y > max.y) { max.y = v.y; }
                if (v.z > max.z) { max.z = v.z; }

                if (v.x < min.x) { min.x = v.x; }
                if (v.y < min.y) { min.y = v.y; }
                if (v.z < min.z) { min.z = v.z; }
            } else {
                // Get index from existing vertex
                top_index = it->second;
            }

            mesh.triangles.push_back(LargeTriangle(right_index, top_index, left_index));
        }

        // Calculate mesh bounds
        mesh.CalcBounds(min, max);

        // Cache optimize triangle and vertex order
        unsigned int* iBuffer = (unsigned int*)&mesh.triangles[0];
        void* vBuffer = (void*)&mesh.vertices[0];
        unsigned int verts = (unsigned int)mesh.vertices.size();
        unsigned int faces = (unsigned int)mesh.triangles.size();
        unsigned int stride = 3 * sizeof(float);

        TootleResult result = TootleOptimizeVCache(iBuffer, faces, verts, cache_size, iBuffer, NULL, TOOTLE_VCACHE_AUTO);

        if (result != TOOTLE_OK) {
            return LandMesh();
        }

        result = TootleOptimizeVertexMemory(vBuffer, iBuffer, verts, faces, stride, vBuffer, iBuffer, NULL);

        if (result != TOOTLE_OK) {
            return LandMesh();
        }

        // Now that all the vertices have been found, figure out the UVs for them. The per-vertex
        // surface normal is sampled in the same loop so mesh.normals stays exactly parallel to
        // mesh.vertices (normals.size() == vertices.size()); LandMesh::Save then writes a real
        // per-vertex normal for every vertex instead of its (0,0,1) fallback (task 5.1; Req 7.2).
        // Tootle failures return an empty LandMesh before reaching here.
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            mesh.uvs.push_back(sampler->SampleTexCoord(mesh.vertices[i].x, mesh.vertices[i].y));
            mesh.normals.push_back(sampler->SampleNormal(mesh.vertices[i].x, mesh.vertices[i].y));
        }

        return mesh;
    }
};

// Append one complete source-grid mesh at a time. The file header is finalized after the region,
// preserving the existing multi-atlas append format without retaining cell meshes in memory.
static bool SaveFullResolutionPatches(LPCSTR file_path, HeightFieldSampler* sampler,
                                      const unsigned char* patch_validity,
                                      size_t patches_across, size_t patches_down,
                                      float minX, float minY, float patch_width, float patch_height) {
    ScopedFileHandle file(CreateFileA(file_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (!file.IsValid()) {
        return false;
    }

    LARGE_INTEGER file_size;
    if (GetFileSizeEx(file.Get(), &file_size) == FALSE) {
        return false;
    }

    DWORD mesh_count = 0;
    if (file_size.QuadPart > 0) {
        if (file_size.QuadPart < static_cast<LONGLONG>(sizeof(mesh_count)) ||
            !SeekFile(file.Get(), 0, FILE_BEGIN) ||
            !ReadExact(file.Get(), &mesh_count, sizeof(mesh_count))) {
            return false;
        }
    } else if (!WriteExact(file.Get(), &mesh_count, sizeof(mesh_count))) {
        return false;
    }

    if (!SeekFile(file.Get(), 0, FILE_END)) {
        return false;
    }

    for (size_t y = 0; y < patches_down; ++y) {
        for (size_t x = 0; x < patches_across; ++x) {
            size_t index = y * patches_across + x;
            if (patch_validity[index] == 0) {
                continue;
            }

            float left = minX + (float)x * patch_width;
            float bottom = minY + (float)y * patch_height;
            RoamLandPatch patch(bottom + patch_height, left, bottom, left + patch_width, sampler);
            SetLastError(ERROR_SUCCESS);
            LandMesh mesh = patch.GenerateFullResolutionMesh(16);
            if (mesh.vertices.empty()) {
                if (GetLastError() == ERROR_SUCCESS) {
                    SetLastError(ERROR_INVALID_DATA);
                }
                return false;
            }
            if (mesh_count == MAXDWORD) {
                SetLastError(ERROR_ARITHMETIC_OVERFLOW);
                return false;
            }
            if (!mesh.Save(file.Get())) {
                return false;
            }
            ++mesh_count;
        }
    }

    return SeekFile(file.Get(), 0, FILE_BEGIN) &&
           WriteExact(file.Get(), &mesh_count, sizeof(mesh_count));
}

extern "C" BOOL __stdcall TessellateLandscapeAtlased(char* file_path, float* height_data, float* normal_data, unsigned int data_width, unsigned int data_height, float* atlas_data, unsigned int atlas_count, float minX, float minY, float maxX, float maxY, float error_tolerance, unsigned int tree_depth, const unsigned char* patch_validity) {
    try {
        if (!file_path || !height_data || !normal_data || !patch_validity ||
            (atlas_count > 0 && !atlas_data) || data_width < 2 || data_height < 2 ||
            (data_width - 1) % kSourceGridQuads != 0 ||
            (data_height - 1) % kSourceGridQuads != 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

    // normal_data is a parallel xyz-per-sample field on the same grid as height_data, fed straight
    // into the HeightFieldSampler below so SampleNormal can sample it per emitted vertex (task 5.1).
    // patch_validity is one row-major ownership byte per cell-sized patch. Geometry shape still
    // tessellates across the full rectangular region, but only LAND-owned patches are serialized.

    // Clamp the caller-supplied ROAM tree depth to the maximum the variance pool is sized for.
    // The GUI passes 10 for every existing tier (byte-identical to the previous hard-coded value)
    // and 12 for the Mega Detail tier so a near-zero tolerance can reach the 65x65 source grid.
    if (tree_depth > kMaxRoamTreeDepth) {
        tree_depth = (unsigned int)kMaxRoamTreeDepth;
    }

    // Create sampler
    HeightFieldSampler sampler(height_data, normal_data, data_width, data_height, atlas_data, atlas_count, minX, minY, maxX, maxY);

    // Create patches. One patch == one Morrowind exterior cell (8192 units) so each emitted mesh
    // maps to exactly one cell, which is what the per-cell composite Texture_Binder requires
    // (Architecture B). Previously patches were 32768 (4x4 cells) which produced coarse meshes that
    // could not be addressed per cell. The heightfield is sampled every 128 units, so a cell spans
    // 8192/128 = 64 samples. The ROAM tree depth is now supplied per-tier via tree_depth (default
    // 10 keeps the leaf-triangle world size identical to the old 32768/depth-14: 32768/2^7 ==
    // 8192/2^5 == 256 units); the Mega tier passes 12.
    //
    // Patch count is derived from the CELL SPAN, not ceil(data_width / 64). Task 4.1 made the
    // heightmap builder sample the grid INCLUSIVELY -- data_width = data_height = cells*64 + 1, so
    // the shared seam sample at source index cells*64 (the column/row that adjacent cells agree on)
    // is present in-bounds (Req 6.1, 6.2). Each patch spans 64 samples (8192 world units), and patch
    // i covers samples [i*64 .. i*64+64] inclusive, sharing the seam sample with patch i+1. The cell
    // count per edge is therefore (data_width - 1) / 64: with the inclusive width that is exactly
    // `cells`, yielding one patch per cell and one mesh per cell (Req 10.4). Using ceil(data_width /
    // 64) on the inclusive width would round (cells*64+1)/64 up to cells+1 and spawn a one-sample
    // sliver patch past the seam, breaking the one-mesh-per-cell mapping the composite path relies
    // on. Because the grid is inclusive, the rightmost/topmost patch's far edge (world index
    // cells*64) now lands on real seam data instead of the clamped edge GetHeightValue/
    // GetNormalValue would otherwise return.
    const float patch_width = 8192.0f, patch_height = 8192.0f;
    size_t patches_across = (size_t)(data_width - 1) / kSourceGridQuads;
    size_t patches_down = (size_t)(data_height - 1) / kSourceGridQuads;

    // Depth 12 is Mega Detail's exact 65x65 source-grid contract. A direct regular grid has the
    // same maximum leaf size and seamless shared edges without retaining a ROAM forest and every
    // completed mesh for the entire atlas region.
    if (tree_depth == kMaxRoamTreeDepth) {
        return SaveFullResolutionPatches(file_path, &sampler, patch_validity,
                                         patches_across, patches_down, minX, minY,
                                         patch_width, patch_height) ? TRUE : FALSE;
    }

    vector<RoamLandPatch> patches;

    Vector3 corner(minX, minY, 0.0f);

    // Fill in patch data
    patches.resize(patches_across * patches_down);

    for (size_t y = 0; y < patches_down; ++y) {
        for (size_t x = 0; x < patches_across; ++x) {
            size_t i = y * patches_across + x;

            patches[i].sampler = &sampler;
            patches[i].left = corner.x;
            patches[i].right = corner.x + patch_width;
            patches[i].bottom = corner.y;
            patches[i].top = corner.y + patch_height;

            // Move the corner right for the next patch
            corner.x += patch_width;
        }
        // Move the corner up and back to the left edge for the next patch
        corner.y += patch_height;
        corner.x = minX;
    }

    // Connect neighboring patches
    for (size_t y = 0; y < patches_down; ++y) {
        for (size_t x = 0; x < patches_across; ++x) {
            size_t index = y * patches_across + x;
            size_t rn_index = y * patches_across + x + 1;
            size_t tn_index = (y+1) * patches_across + x;

            if (x != patches_across - 1) {
                patches[index].t2.left_neighbor = &patches[rn_index].t1;
                patches[rn_index].t1.left_neighbor = &patches[index].t2;
            }

            if (y != patches_down - 1) {
                patches[index].t2.right_neighbor = &patches[tn_index].t1;
                patches[tn_index].t1.right_neighbor = &patches[index].t2;
            }
        }
    }

    // Tessellate patches
    for (size_t i = 0; i < patches.size(); ++i) {
        patches[i].Tessellate(error_tolerance, tree_depth);
    }

    // Generate Meshes
    vector<LandMesh> meshes;

    for (size_t i = 0; i < patches.size(); ++i) {
        if (patch_validity[i] == 0) {
            continue;
        }

        LandMesh mesh = patches[i].GenerateMesh(16);

        if (mesh.vertices.size() > 0) {
            meshes.push_back(mesh);
        }
    }

    // Count verts and tris
    size_t verts = 0;
    size_t tris = 0;
    for (size_t i = 0; i < meshes.size(); ++i) {
        verts += meshes[i].vertices.size();
        tris += meshes[i].triangles.size();
    }

    // Save the Meshes
    return LandMesh::SaveMeshes(file_path, meshes) ? TRUE : FALSE;
    } catch (const std::bad_alloc&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    } catch (const std::exception&) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    } catch (...) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
}
