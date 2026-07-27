using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

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
    //     per-cell UV blob: concatenated (float u, float v) pairs; each entry's uvOffset is the byte
    //         offset into this blob, which begins immediately after the directory.
    //   composite.data:
    //     concatenated per-cell DXT1 blobs (each includes its mip chain); a directory entry's
    //     dataOffset/dataLength slice that cell's bytes out of this file.
    //
    // The byte layout mirrors src\mge\dlcomposite.h and tests\composite_format_model.py. UV payloads
    // are spooled to disk as they are enumerated so a large world does not retain every cell's 64x64
    // UV array in the 32-bit generator while composites are baked.
    class CompositeSetWriter {

        public const uint CompositeFormatVersion = 1;
        public const int DefaultEdgeTexels = 1024;
        public const uint FlagContainsPlaceholders = 0x1;
        public const string CompositeIndexFileName = "composite.index";
        public const string CompositeDataFileName = "composite.data";

        private static readonly byte[] Magic = {
            (byte)'M', (byte)'G', (byte)'E', (byte)'D', (byte)'L', (byte)'C', (byte)'M', (byte)'P'
        };

        private const int UvPairBytes = 8;
        private static readonly byte[] EmptyData = new byte[0];

        public string IndexPath { get; private set; }
        public string DataPath { get; private set; }
        public int CellCount { get; private set; }

        public void Write(string distantLandDir, IEnumerable<CompositeBakeResult> bakes, IEnumerable<PerCellUV> uvs) {
            Write(distantLandDir, bakes, uvs, DefaultEdgeTexels);
        }

        // Writes the composite payload in one pass. UV arrays are consumed into a temporary spool first;
        // only their cell metadata stays resident while the much larger DXT1 stream is generated.
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
            CellCount = 0;

            using (var uvSpool = new UvSpool(uvs)) {
                var directory = new List<DirEntry>();
                uint flags = 0;
                ulong dataOffset = 0;
                uint nextUvOffset = 0;

                using (var dataFs = new FileStream(DataPath, FileMode.Create, FileAccess.Write)) {
                    foreach (CompositeBakeResult bake in bakes) {
                        byte[] dxt1 = bake.dxt1Bytes ?? EmptyData;
                        dataFs.Write(dxt1, 0, dxt1.Length);

                        UvEntry matched;
                        int chunkId = 0;
                        uint uvSpoolOffset = 0;
                        uint uvVertCount = 0;
                        if (uvSpool.Entries.TryGetValue(PackKey(bake.cell.cellX, bake.cell.cellY), out matched)) {
                            chunkId = matched.chunkId;
                            uvSpoolOffset = matched.spoolOffset;
                            uvVertCount = matched.uvVertCount;
                        }

                        uint uvOffset = nextUvOffset;
                        nextUvOffset = checked(nextUvOffset + checked(uvVertCount * (uint)UvPairBytes));

                        int edge = bake.edgeTexels > 0 ? bake.edgeTexels : defaultEdgeTexels;
                        directory.Add(new DirEntry {
                            cellX = bake.cell.cellX,
                            cellY = bake.cell.cellY,
                            edgeTexels = (uint)edge,
                            dataOffset = dataOffset,
                            dataLength = (uint)dxt1.Length,
                            chunkId = (uint)chunkId,
                            uvOffset = uvOffset,
                            uvVertCount = uvVertCount,
                            uvSpoolOffset = uvSpoolOffset,
                            isPlaceholder = bake.isPlaceholder
                        });

                        if (bake.isPlaceholder) {
                            flags |= FlagContainsPlaceholders;
                        }
                        dataOffset += (ulong)dxt1.Length;
                    }
                }

                CellCount = directory.Count;
                using (var idxFs = new FileStream(IndexPath, FileMode.Create, FileAccess.Write))
                using (var bw = new BinaryWriter(idxFs)) {
                    bw.Write(Magic);
                    bw.Write(CompositeFormatVersion);
                    bw.Write((uint)directory.Count);
                    bw.Write((uint)defaultEdgeTexels);
                    bw.Write(flags);

                    foreach (DirEntry entry in directory) {
                        bw.Write(entry.cellX);
                        bw.Write(entry.cellY);
                        bw.Write(entry.edgeTexels);
                        bw.Write(entry.dataOffset);
                        bw.Write(entry.dataLength);
                        bw.Write(entry.chunkId);
                        bw.Write(entry.uvOffset);
                        bw.Write(entry.uvVertCount);
                        bw.Write((byte)(entry.isPlaceholder ? 1 : 0));
                        bw.Write((byte)0);
                        bw.Write((byte)0);
                        bw.Write((byte)0);
                    }

                    bw.Flush();
                    uvSpool.CopyTo(idxFs, directory);
                }
            }
        }

        private static long PackKey(int cellX, int cellY) {
            return ((long)(uint)cellX << 32) | (uint)cellY;
        }

        private struct UvEntry {
            public int chunkId;
            public uint spoolOffset;
            public uint uvVertCount;
        }

        private sealed class UvSpool : IDisposable {
            public readonly Dictionary<long, UvEntry> Entries = new Dictionary<long, UvEntry>();
            private readonly string spoolPath;
            private FileStream spoolStream;

            public UvSpool(IEnumerable<PerCellUV> uvs) {
                spoolPath = Path.Combine(Path.GetTempPath(), "mge_composite_uv_" + Guid.NewGuid().ToString("N") + ".tmp");
                try {
                    spoolStream = new FileStream(spoolPath, FileMode.CreateNew, FileAccess.ReadWrite,
                        FileShare.None, 4096, FileOptions.DeleteOnClose);
                    using (var bw = new BinaryWriter(spoolStream, Encoding.UTF8, true)) {
                        if (uvs != null) {
                            foreach (PerCellUV uv in uvs) {
                                long key = PackKey(uv.cell.cellX, uv.cell.cellY);
                                if (Entries.ContainsKey(key)) {
                                    continue;
                                }

                                CompositeUV[] cellUvs = uv.uvs;
                                int count = cellUvs == null ? 0 : cellUvs.Length;
                                if (spoolStream.Position > UInt32.MaxValue) {
                                    throw new InvalidDataException("Composite UV payload exceeds the format's 32-bit offset range.");
                                }

                                Entries.Add(key, new UvEntry {
                                    chunkId = uv.chunkId,
                                    spoolOffset = (uint)spoolStream.Position,
                                    uvVertCount = (uint)count
                                });

                                for (int i = 0; i < count; i++) {
                                    bw.Write(cellUvs[i].u);
                                    bw.Write(cellUvs[i].v);
                                }

                                if (spoolStream.Position > UInt32.MaxValue) {
                                    throw new InvalidDataException("Composite UV payload exceeds the format's size limit.");
                                }
                            }
                        }
                    }
                } catch {
                    Dispose();
                    throw;
                }
            }

            public void CopyTo(Stream destination, IList<DirEntry> directory) {
                var buffer = new byte[64 * 1024];
                foreach (DirEntry entry in directory) {
                    long remaining = checked((long)entry.uvVertCount * UvPairBytes);
                    if (remaining == 0) {
                        continue;
                    }

                    spoolStream.Position = entry.uvSpoolOffset;
                    while (remaining > 0) {
                        int requested = (int)Math.Min(buffer.Length, remaining);
                        int read = spoolStream.Read(buffer, 0, requested);
                        if (read == 0) {
                            throw new EndOfStreamException("Composite UV spool ended before the directory payload was complete.");
                        }
                        destination.Write(buffer, 0, read);
                        remaining -= read;
                    }
                }
            }

            public void Dispose() {
                try {
                    if (spoolStream != null) {
                        spoolStream.Dispose();
                        spoolStream = null;
                    }
                } finally {
                    try {
                        if (File.Exists(spoolPath)) {
                            File.Delete(spoolPath);
                        }
                    } catch {
                        // DeleteOnClose normally owns cleanup; this is only a best-effort fallback.
                    }
                }
            }
        }

        private struct DirEntry {
            public int cellX, cellY;
            public uint edgeTexels;
            public ulong dataOffset;
            public uint dataLength;
            public uint chunkId;
            public uint uvOffset;
            public uint uvVertCount;
            public uint uvSpoolOffset;
            public bool isPlaceholder;
        }
    }
}
