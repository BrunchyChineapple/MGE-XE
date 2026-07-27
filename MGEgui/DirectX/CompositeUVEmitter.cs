using System;
using System.Collections.Generic;
using MGEgui.DistantLand;

namespace MGEgui.DirectX {

    // A single per-cell texture coordinate, both components in [0,1]. This is what gets stored in the
    // Composite_Set's per-cell UV blob (uvVertCount * (float u, float v)); CompositeSetWriter (task 3.1)
    // serializes these straight out, so they are kept as float to match the on-disk layout.
    struct CompositeUV {
        public float u, v;

        public CompositeUV(float u, float v) {
            this.u = u;
            this.v = v;
        }

        public override string ToString() {
            return "(" + u + ", " + v + ")";
        }
    }

    // A terrain mesh vertex's world-space XY position. Height (Z) is irrelevant to the per-cell UV
    // mapping, so only X/Y are carried. World units; one exterior cell spans CompositeUVEmitter.CellSize.
    struct WorldVertex {
        public double x, y;

        public WorldVertex(double x, double y) {
            this.x = x;
            this.y = y;
        }
    }

    // Per-cell UV + chunk-association record: one chunk of the distant-land mesh, all of whose vertices
    // belong to exactly one cell, hence exactly one Per_Cell_Composite (Req 2.2). The chunkId is the
    // distant-land chunk this cell maps to and is 1:1 with the cell/composite. CompositeSetWriter
    // (task 3.1) serializes cell.cellX/cellY, chunkId, uvs.Length (uvVertCount), then the UV blob into
    // composite.index. This record holds per-cell [0,1] UVs only; it never touches the world mesh's
    // existing SHORT2N atlas UVs (Req 2.3).
    struct PerCellUV {
        public CellId cell;        // which cell / composite these UVs address
        public int chunkId;        // distant-land chunk this cell maps to (1:1 with the composite)
        public CompositeUV[] uvs;  // per-vertex [0,1] UVs; count == uvVertCount in the directory entry

        public int VertexCount {
            get { return uvs == null ? 0 : uvs.Length; }
        }
    }

    // CompositeUVEmitter: the per-cell UV + chunking half of the Composite_Baker. The existing
    // tessellator (TessellateLandscapeAtlased / the native GenerateWorldMesh path) bakes the world
    // mesh's SHORT2N province-atlas UVs (decoded tc/32767); this class does NOT touch those. It adds a
    // parallel, additive emission that:
    //
    //   * Produces a per-cell chunking of the distant-land mesh so each chunk's vertices belong to
    //     exactly one cell, hence exactly one composite (Req 2.2). A world vertex (worldX, worldY)
    //     belongs to cell (floor(worldX / CellSize), floor(worldY / CellSize)); this is the same
    //     per-cell partition the generator already iterates for the atlas bake.
    //   * Emits clamped per-cell [0,1] UVs for those vertices (Req 2.1):
    //         u = clamp((worldX - cellMinX) / CellSize, 0, 1)
    //         v = 1 - clamp((worldY - cellMinY) / CellSize, 0, 1)
    //     The V axis is flipped to match the top-left texture origin the bake writes into. Clamping
    //     keeps an exact far-edge vertex on u=1 or v=0 instead of wrapping it to the opposite edge.
    //   * Stores the result only in the additive Composite_Set (as PerCellUV records); the world mesh
    //     and its atlas UVs are left byte-for-byte unchanged so the Single_Atlas_Path stays usable from
    //     the same generation (Req 2.3).
    //
    // The math here is pure and device-independent (no SlimDX / D3D9).
    class CompositeUVEmitter {

        // World-unit extent of one exterior cell edge (8192 units), matching LAND cell spacing and the
        // CELL_SIZE constant in composite_blend_model. Also the UV mapping's denominator.
        public const int CellSize = 8192;

        // Default per-edge vertex sampling used by the canonical per-cell grid helper. A cell's
        // heightmap is 65x65 vertices / 64 subcells; the half-open grid samples the 64 vertices the
        // cell owns (the 65th seam vertex belongs to the neighbouring cell, matching floor ownership).
        public const int DefaultCellSubdivisions = 64;

        private static double Clamp01(double value) {
            return Math.Max(0.0, Math.Min(1.0, value));
        }

        // The cell that owns a world vertex: (floor(worldX / CellSize), floor(worldY / CellSize)).
        // This is the chunking key - every vertex resolves to exactly one cell, so the partition is
        // clean and total (Req 2.2 / Property 5).
        public static CellId CellOf(double worldX, double worldY) {
            int cellX = (int)Math.Floor(worldX / CellSize);
            int cellY = (int)Math.Floor(worldY / CellSize);
            return new CellId(cellX, cellY);
        }

        // Per-cell [0,1] UV for one world vertex, relative to the given cell's minimum corner.
        // Clamping preserves the inclusive far boundary and absorbs small coordinate drift.
        public static CompositeUV CellUV(double worldX, double worldY, CellId cell) {
            double cellMinX = (double)cell.cellX * CellSize;
            double cellMinY = (double)cell.cellY * CellSize;
            float u = (float)Clamp01((worldX - cellMinX) / CellSize);
            float v = (float)(1.0 - Clamp01((worldY - cellMinY) / CellSize));
            return new CompositeUV(u, v);
        }

        // Emit [0,1] UVs for one cell's terrain vertices (world positions), tagged with chunkId. The
        // vertices are mapped against this cell's own minimum corner, so callers are expected to pass
        // vertices that belong to this cell (use ChunkAndEmit to derive ownership automatically). The
        // returned PerCellUV references exactly this one composite (Req 2.1, 2.2).
        public PerCellUV EmitCellUVs(LAND cell, int chunkId, IEnumerable<WorldVertex> worldVerts) {
            if (cell == null) {
                throw new ArgumentNullException("cell");
            }
            return EmitCellUVs(new CellId(cell.xpos, cell.ypos), chunkId, worldVerts);
        }

        // CellId overload of EmitCellUVs (above).
        public PerCellUV EmitCellUVs(CellId cell, int chunkId, IEnumerable<WorldVertex> worldVerts) {
            if (worldVerts == null) {
                throw new ArgumentNullException("worldVerts");
            }

            var uvs = new List<CompositeUV>();
            foreach (WorldVertex wv in worldVerts) {
                uvs.Add(CellUV(wv.x, wv.y, cell));
            }

            return new PerCellUV {
                cell = cell,
                chunkId = chunkId,
                uvs = uvs.ToArray()
            };
        }

        // Produce a per-cell chunking from a flat set of distant-land mesh world vertices: partition the
        // vertices by their owning cell (CellOf), assign each occupied cell a dense, deterministic
        // chunkId, and emit that cell's [0,1] UVs. Every vertex lands in exactly one chunk and every
        // chunk references exactly one composite (Req 2.2 / Property 5). Chunk ordering is row-major by
        // (cellY, cellX) so the chunking is reproducible across bakes and mirrors the y-outer/x-inner
        // iteration the atlas bake and GenerateWorldMesh already use.
        public List<PerCellUV> ChunkAndEmit(IEnumerable<WorldVertex> worldVerts) {
            if (worldVerts == null) {
                throw new ArgumentNullException("worldVerts");
            }

            // Group vertices by owning cell. Pack (cellX, cellY) into a long key for fast, exact lookup.
            var byCell = new Dictionary<long, List<WorldVertex>>();
            foreach (WorldVertex wv in worldVerts) {
                CellId cell = CellOf(wv.x, wv.y);
                long key = PackKey(cell.cellX, cell.cellY);
                List<WorldVertex> bucket;
                if (!byCell.TryGetValue(key, out bucket)) {
                    bucket = new List<WorldVertex>();
                    byCell[key] = bucket;
                }
                bucket.Add(wv);
            }

            // Deterministic row-major chunk order: sort occupied cells by (cellY, cellX).
            var cells = new List<CellId>();
            foreach (long key in byCell.Keys) {
                cells.Add(UnpackKey(key));
            }
            cells.Sort(CompareRowMajor);

            var result = new List<PerCellUV>(cells.Count);
            int chunkId = 0;
            foreach (CellId cell in cells) {
                List<WorldVertex> verts = byCell[PackKey(cell.cellX, cell.cellY)];
                result.Add(EmitCellUVs(cell, chunkId, verts));
                chunkId++;
            }
            return result;
        }

        // Canonical per-cell grid of world-space vertices for a cell, sampling the cell's own half-open
        // span [cellMin, cellMin + CellSize). Vertex k (0..subdivisions-1) sits at
        // cellMin + k * (CellSize / subdivisions), so all emitted UVs stay in [0,1) and the shared seam
        // (the next integer cell boundary) belongs to the neighbouring cell - consistent with the
        // floor-based chunking and clamped UV formula. This is a convenience for callers that want a
        // dense per-cell UV set at the heightmap resolution without owning the world-position layout;
        // it does not read or alter the world mesh.
        public static List<WorldVertex> CellGridWorldVertices(CellId cell, int subdivisions) {
            if (subdivisions <= 0) {
                subdivisions = DefaultCellSubdivisions;
            }

            double cellMinX = (double)cell.cellX * CellSize;
            double cellMinY = (double)cell.cellY * CellSize;
            double step = (double)CellSize / subdivisions;

            var verts = new List<WorldVertex>(subdivisions * subdivisions);
            for (int j = 0; j < subdivisions; j++) {
                double worldY = cellMinY + j * step;
                for (int i = 0; i < subdivisions; i++) {
                    double worldX = cellMinX + i * step;
                    verts.Add(new WorldVertex(worldX, worldY));
                }
            }
            return verts;
        }

        // Emit per-cell UVs directly over the canonical half-open grid. Building the coordinates in
        // place avoids the intermediate WorldVertex list and List<CompositeUV> for every cell.
        public PerCellUV EmitCellGrid(CellId cell, int chunkId, int subdivisions) {
            if (subdivisions <= 0) {
                subdivisions = DefaultCellSubdivisions;
            }

            var uvs = new CompositeUV[subdivisions * subdivisions];
            int index = 0;
            for (int j = 0; j < subdivisions; j++) {
                float v = (float)(1.0 - (double)j / subdivisions);
                for (int i = 0; i < subdivisions; i++) {
                    float u = (float)((double)i / subdivisions);
                    uvs[index++] = new CompositeUV(u, v);
                }
            }

            return new PerCellUV {
                cell = cell,
                chunkId = chunkId,
                uvs = uvs
            };
        }

        // LAND overload of EmitCellGrid (above).
        public PerCellUV EmitCellGrid(LAND cell, int chunkId, int subdivisions) {
            if (cell == null) {
                throw new ArgumentNullException("cell");
            }
            return EmitCellGrid(new CellId(cell.xpos, cell.ypos), chunkId, subdivisions);
        }

        private static long PackKey(int cellX, int cellY) {
            return ((long)(uint)cellX << 32) | (uint)cellY;
        }

        private static CellId UnpackKey(long key) {
            int cellX = (int)(key >> 32);
            int cellY = (int)(key & 0xFFFFFFFFL);
            return new CellId(cellX, cellY);
        }

        private static int CompareRowMajor(CellId a, CellId b) {
            if (a.cellY != b.cellY) {
                return a.cellY.CompareTo(b.cellY);
            }
            return a.cellX.CompareTo(b.cellX);
        }
    }
}
