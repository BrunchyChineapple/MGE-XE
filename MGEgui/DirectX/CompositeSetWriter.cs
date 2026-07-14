using System;
using System.Collections.Generic;
using System.IO;

namespace MGEgui.DirectX {

    // CompositeSetWriter: serializes a baked Composite_Set to disk as the additive file pair
    // composite.index + composite.data under Data Files\distantland\, alongside the unchanged
    // world / world.dds / world_n.dds outputs (Req 1.6, 3.1). It pairs each cell's CompositeBakeResult
    // (from CompositeBaker, task 2.5) with its PerCellUV record (from CompositeUVEmitter, task 2.6) by
    // CellId, then writes:
    //
    //   composite.index:
    //     CompositeSetHeader (24 bytes): magic "MGEDLCMP" (raw 8 bytes, no terminator),
    //         uint32 formatVersion, uint32 cellCount, uint32 defaultEdgeTexels, uint32 flags
    //     CompositeDirEntry[cellCount] (40 bytes each): int32 cellX, cellY; uint32 edgeTexels;
    //         uint64 dataOffset; uint32 dataLength, chunkId, uvOffset, uvVertCount;
    //         uint8 isPlaceholder; uint8[3] reserved
    //     per-cell UV blob: concatenated (float u, float v) pairs in directory order; each entry's
    //         uvOffset is the byte offset into THIS blob (which begins right after the directory),
    //         and uvVertCount is the pair count.
    //   composite.data:
    //     concatenated per-cell DXT1 blobs (each includes its mip chain); a directory entry's
    //     dataOffset/dataLength slice that cell's bytes out of this file.
    //
    // The byte layout is the exact mirror of the shared C++ header src\mge\dlcomposite.h
    // (CompositeSetHeader/CompositeDirEntry/CompositeUV) and the authoritative Python oracle
    // tests\composite_format_model.py (encode/decode/classify). All integers are written
    // little-endian via BinaryWriter (always little-endian in .NET), matching the on-disk format the
    // 32-bit client and 64-bit server both read. The magic is written as raw bytes - never a
    // length-prefixed BinaryWriter string - so it lands as exactly the 8 expected ASCII bytes.
    //
    // This class only writes the additive Composite_Set; it never reads or rewrites world /
    // world.dds / world_n.dds (Req 3.1), so the Single_Atlas_Path stays usable from the same run.
    class CompositeSetWriter {

        // Composite-set format version (mirrors COMPOSITE_FORMAT_VERSION in dlcomposite.h). Independent
        // of Statics.DistantLandVersion (the distant-statics marker) so the two formats evolve apart.
        public const uint CompositeFormatVersion = 1;

        // Default per-cell baked resolution stamped into the header when the caller does not override it
        // (Req 1.5 / 9.3). Matches CompositeBaker.DefaultEdgeTexels and DEFAULT_EDGE_TEXELS in the model.
        public const int DefaultEdgeTexels = 1024;

        // CompositeSetHeader::flags bit0 - the set contains one or more placeholder composites
        // (COMPOSITE_FLAG_HAS_PLACEHOLDERS in dlcomposite.h, FLAG_CONTAINS_PLACEHOLDERS in the model).
        public const uint FlagContainsPlaceholders = 0x1;

        // On-disk file names written under the distantland directory.
        public const string CompositeIndexFileName = "composite.index";
        public const string CompositeDataFileName = "composite.data";

        // CompositeSetHeader::magic - exactly 8 bytes, stored without a null terminator. Written as raw
        // bytes so no length prefix or terminator sneaks in.
        private static readonly byte[] Magic = {
            (byte)'M', (byte)'G', (byte)'E', (byte)'D', (byte)'L', (byte)'C', (byte)'M', (byte)'P'
        };

        private const int Dxt1MipPairBytes = 8; // one UV pair = 2 floats = 8 bytes

        // Resolved absolute paths of the last write (handy for the generator's reporting/telemetry).
        public string IndexPath { get; private set; }
        public string DataPath { get; private set; }

        // Writes the Composite_Set into distantLandDir using the default header resolution (1024).
        public void Write(string distantLandDir, IEnumerable<CompositeBakeResult> bakes, IEnumerable<PerCellUV> uvs) {
            Write(distantLandDir, bakes, uvs, DefaultEdgeTexels);
        }

        // Writes composite.index + composite.data into distantLandDir. Each bake is paired with its
        // PerCellUV by CellId to source the chunkId and the [0,1] UV pairs; bakes with no matching UV
        // record still get a dense directory entry (uvVertCount 0, chunkId 0) so the directory stays
        // one-entry-per-cell. The header's flags placeholder bit is set when any bake isPlaceholder.
        // Existing world / world.dds / world_n.dds files are never touched (Req 3.1).
        public void Write(string distantLandDir, IEnumerable<CompositeBakeResult> bakes, IEnumerable<PerCellUV> uvs, int defaultEdgeTexels) {
            if (distantLandDir == null) {
                throw new ArgumentNullException("distantLandDir");
            }
            if (bakes == null) {
                throw new ArgumentNullException("bakes");
            }
            if (defaultEdgeTexels <= 0) {
                defaultEdgeTexels = DefaultEdgeTexels;
            }

            Directory.CreateDirectory(distantLandDir);
            IndexPath = Path.Combine(distantLandDir, CompositeIndexFileName);
            DataPath = Path.Combine(distantLandDir, CompositeDataFileName);

            // Index per-cell UVs by packed cell coordinate for O(1) pairing with each bake. First record
            // for a given cell wins, keeping the pairing deterministic if a cell appears twice.
            var uvByCell = new Dictionary<long, PerCellUV>();
            if (uvs != null) {
                foreach (PerCellUV uv in uvs) {
                    long key = PackKey(uv.cell.cellX, uv.cell.cellY);
                    if (!uvByCell.ContainsKey(key)) {
                        uvByCell[key] = uv;
                    }
                }
            }

            // Pass 1: stream composite.data and build the directory + UV blob in memory. The DXT1 blobs
            // can total hundreds of MB, so they go straight to disk; the directory and UV blob are small.
            var directory = new List<DirEntry>();
            uint flags = 0;
            ulong dataOffset = 0;
            uint uvOffset = 0;

            using (var dataFs = new FileStream(DataPath, FileMode.Create, FileAccess.Write)) {
                foreach (CompositeBakeResult bake in bakes) {
                    byte[] dxt1 = bake.dxt1Bytes ?? new byte[0];
                    dataFs.Write(dxt1, 0, dxt1.Length);

                    CompositeUV[] cellUvs;
                    int chunkId;
                    PerCellUV matched;
                    if (uvByCell.TryGetValue(PackKey(bake.cell.cellX, bake.cell.cellY), out matched)) {
                        cellUvs = matched.uvs ?? new CompositeUV[0];
                        chunkId = matched.chunkId;
                    } else {
                        cellUvs = new CompositeUV[0];
                        chunkId = 0;
                    }

                    int edge = bake.edgeTexels > 0 ? bake.edgeTexels : defaultEdgeTexels;

                    directory.Add(new DirEntry {
                        cellX = bake.cell.cellX,
                        cellY = bake.cell.cellY,
                        edgeTexels = (uint)edge,
                        dataOffset = dataOffset,
                        dataLength = (uint)dxt1.Length,
                        chunkId = (uint)chunkId,
                        uvOffset = uvOffset,
                        uvVertCount = (uint)cellUvs.Length,
                        isPlaceholder = bake.isPlaceholder,
                        uvs = cellUvs
                    });

                    if (bake.isPlaceholder) {
                        flags |= FlagContainsPlaceholders;
                    }
                    dataOffset += (ulong)dxt1.Length;
                    uvOffset += (uint)(cellUvs.Length * Dxt1MipPairBytes);
                }
            }

            // Pass 2: write composite.index = header + directory + per-cell UV blob.
            using (var idxFs = new FileStream(IndexPath, FileMode.Create, FileAccess.Write))
            using (var bw = new BinaryWriter(idxFs)) {
                // CompositeSetHeader (24 bytes).
                bw.Write(Magic);                          // char magic[8], raw 8 bytes
                bw.Write(CompositeFormatVersion);         // uint32 formatVersion
                bw.Write((uint)directory.Count);          // uint32 cellCount
                bw.Write((uint)defaultEdgeTexels);        // uint32 defaultEdgeTexels
                bw.Write(flags);                          // uint32 flags

                // CompositeDirEntry[cellCount] (40 bytes each).
                foreach (DirEntry e in directory) {
                    bw.Write(e.cellX);                    // int32 cellX
                    bw.Write(e.cellY);                    // int32 cellY
                    bw.Write(e.edgeTexels);               // uint32 edgeTexels
                    bw.Write(e.dataOffset);               // uint64 dataOffset
                    bw.Write(e.dataLength);               // uint32 dataLength
                    bw.Write(e.chunkId);                  // uint32 chunkId
                    bw.Write(e.uvOffset);                 // uint32 uvOffset (into the UV blob below)
                    bw.Write(e.uvVertCount);              // uint32 uvVertCount
                    bw.Write((byte)(e.isPlaceholder ? 1 : 0)); // uint8 isPlaceholder
                    bw.Write((byte)0);                    // uint8 reserved[0]
                    bw.Write((byte)0);                    // uint8 reserved[1]
                    bw.Write((byte)0);                    // uint8 reserved[2]
                }

                // Per-cell UV blob: (float u, float v) pairs in directory order. uvOffset/uvVertCount in
                // each directory entry index into this region, which begins right here after the directory.
                foreach (DirEntry e in directory) {
                    for (int i = 0; i < e.uvs.Length; i++) {
                        bw.Write(e.uvs[i].u);             // float u
                        bw.Write(e.uvs[i].v);             // float v
                    }
                }
            }
        }

        private static long PackKey(int cellX, int cellY) {
            return ((long)(uint)cellX << 32) | (uint)cellY;
        }

        // Computed directory record plus the cell's UV pairs, carried between the two write passes.
        private struct DirEntry {
            public int cellX, cellY;
            public uint edgeTexels;
            public ulong dataOffset;
            public uint dataLength;
            public uint chunkId;
            public uint uvOffset;
            public uint uvVertCount;
            public bool isPlaceholder;
            public CompositeUV[] uvs;
        }
    }
}
