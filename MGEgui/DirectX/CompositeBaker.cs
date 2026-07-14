using System;
using SlimDX;
using SlimDX.Direct3D9;
using MGEgui.DistantLand;

namespace MGEgui.DirectX {

    // Worldspace cell coordinate for a per-cell composite. Mirrors LAND.xpos/ypos.
    struct CellId {
        public int cellX, cellY;

        public CellId(int x, int y) {
            cellX = x;
            cellY = y;
        }

        public override string ToString() {
            return "(" + cellX + ", " + cellY + ")";
        }
    }

    // One entry produced per exterior distant-land cell.
    struct CompositeBakeResult {
        public CellId cell;          // worldspace cell coordinate
        public byte[] dxt1Bytes;     // compressed composite incl. mip chain (raw DXT1, tightly packed)
        public int edgeTexels;       // baked resolution (default 1024)
        public bool isPlaceholder;   // true if the cell failed compositing and got a placeholder (Req 1.7)
    }

    interface ICompositeBaker {
        // Reuses CalcWeights + CellTexBlend.fx; renders one cell to an Res x Res render target.
        CompositeBakeResult BakeCell(LAND cell, int edgeTexels);
    }

    // Composite_Baker: bakes one dedicated DXT1 composite texture per exterior cell, reusing the
    // existing CellTexCreator/TextureBank pipeline (CalcWeights one-hot VTEX + separable 5-tap blur,
    // additive CellTexBlend.fx splat passes). Where the province-atlas bake (WorldTexCreator) renders
    // every cell into a sub-rectangle of one big sheet, this renders each cell into its own
    // Res x Res render target, generates mips, and compresses to DXT1 in memory.
    //
    // This stage is purely additive: it produces a CompositeBakeResult in memory. Writing the
    // Composite_Set to disk is handled separately (CompositeSetWriter), as is per-cell UV emission
    // (CompositeUVEmitter); this class does neither.
    class CompositeBaker : ICompositeBaker, IDisposable {

        // Default per-cell composite resolution (Req 1.5 / 9.3).
        public const int DefaultEdgeTexels = 1024;

        private const int Dxt1BlockBytes = 8; // DXT1: 8 bytes per 4x4 block

        // Routes per-cell failure messages. When null, falls back to the generator log file
        // (Statics.fn_dlLog), matching the existing saveWarnings/atlas_log append pattern.
        private readonly Action<string> log;

        // Reused blend pipeline (one-hot weights + 5-tap blur + CellTexBlend.fx splat).
        private CellTexCreator ctc;

        // Per-cell render-target + compression resources (mirrors WorldTexCreator, but per cell).
        private Texture renderTargetTex;
        private Surface renderTarget;
        private Texture uncompressedTex;
        private Texture compressedTex;

        // Resolution the current resources are sized for; resources are (re)built lazily on change.
        private int resourceEdgeTexels;

        // The D3D9 step currently in progress, for precise per-cell failure logging. Set throughout
        // BakeCell (and its compress sub-steps) so LogFailure names the exact call that threw.
        private string bakeStage;

        // Per-baker scratch .dds path for the D3DX texture serializer (Texture.ToFile). Reused and
        // overwritten per cell; deleted in Dispose. See ReadCompressedBytes for why the on-disk D3DX
        // path is used instead of the in-memory / lock-readback variants.
        private readonly string scratchDdsPath;

        private bool disposed;

        public CompositeBaker() : this(DefaultEdgeTexels, null) {
        }

        public CompositeBaker(int edgeTexels) : this(edgeTexels, null) {
        }

        public CompositeBaker(int edgeTexels, Action<string> logger) {
            log = logger;
            // Unique per-baker scratch file in the user temp dir so concurrent/repeat runs never collide
            // and nothing is written under the live game install.
            scratchDdsPath = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                "mge_composite_bake_" + Guid.NewGuid().ToString("N") + ".dds");
            EnsureResources(edgeTexels > 0 ? edgeTexels : DefaultEdgeTexels);
        }

        // Bakes a single cell to its own DXT1 composite. On any compositing failure the cell is logged,
        // a placeholder composite (flagged isPlaceholder) is produced at the configured resolution so
        // the cell index stays dense, and baking can continue with the next cell (Req 1.7).
        public CompositeBakeResult BakeCell(LAND cell, int edgeTexels) {
            if (cell == null) {
                throw new ArgumentNullException("cell");
            }
            if (edgeTexels <= 0) {
                edgeTexels = DefaultEdgeTexels;
            }

            var cellId = new CellId(cell.xpos, cell.ypos);

            // bakeStage names the D3D9 step in progress so a per-cell failure logs exactly which call
            // threw, rather than a single opaque "INVALIDCALL" for the whole pipeline.
            bakeStage = "init";
            try {
                bakeStage = "EnsureResources";
                EnsureResources(edgeTexels);

                // Build texture banks (groups of four LTEX) and per-vertex one-hot weights blurred by
                // the separable 5-tap kernel {0.04, 0.16, 0.6, 0.16, 0.04} (Req 1.2, 1.3).
                bakeStage = "SetCell";
                ctc.SetCell(cell);

                // Render the whole cell into the per-cell Res x Res target (full target: pos 0,0 scale 1,1),
                // accumulating the banks with the additive CellTexBlend.fx passes (Req 1.3).
                bakeStage = "BeginRenderTarget";
                BeginRenderTarget();
                bakeStage = "Render";
                ctc.Begin();
                // applyVertexColor=false: emit pure, lighting-free albedo. Morrowind's baked per-vertex
                // terrain lighting (CellTexBlend.fx pass P2) is skipped because RTX Remix relights the
                // distant-land geometry itself; baking lighting into the albedo double-lights it and
                // washes the distant land out relative to the (Remix-relit) near terrain.
                ctc.Render(0.0f, 0.0f, 1.0f, 1.0f, false);
                ctc.End();

                // Generate mips and compress to DXT1, extracting the raw mip-chain bytes (Req 1.4).
                byte[] dxt1 = CompressToDxt1Bytes(true);

                return new CompositeBakeResult {
                    cell = cellId,
                    dxt1Bytes = dxt1,
                    edgeTexels = edgeTexels,
                    isPlaceholder = false
                };
            } catch (Exception ex) {
                LogFailure(cellId, bakeStage, ex);
                return MakePlaceholder(cellId, edgeTexels);
            }
        }

        // Builds a valid, device-independent placeholder composite (solid neutral gray) at the configured
        // resolution, with a full DXT1 mip chain matching a real bake's byte layout (Req 1.7).
        public CompositeBakeResult MakePlaceholder(CellId cell, int edgeTexels) {
            if (edgeTexels <= 0) {
                edgeTexels = DefaultEdgeTexels;
            }

            return new CompositeBakeResult {
                cell = cell,
                dxt1Bytes = CreatePlaceholderDxt1(edgeTexels),
                edgeTexels = edgeTexels,
                isPlaceholder = true
            };
        }

        // (Re)creates the render-target and compression textures plus the CellTexCreator when the
        // requested resolution differs from what is currently allocated.
        private void EnsureResources(int edgeTexels) {
            if (ctc != null && resourceEdgeTexels == edgeTexels) {
                return;
            }

            DisposeResources();

            resourceEdgeTexels = edgeTexels;
            ctc = new CellTexCreator(edgeTexels);

            renderTargetTex = new Texture(DXMain.device, edgeTexels, edgeTexels, 0, Usage.RenderTarget, Format.X8R8G8B8, Pool.Default);
            uncompressedTex = new Texture(DXMain.device, edgeTexels, edgeTexels, 0, Usage.None, Format.X8R8G8B8, Pool.SystemMemory);
            compressedTex = new Texture(DXMain.device, edgeTexels, edgeTexels, 0, Usage.None, Format.Dxt1, Pool.SystemMemory);
            renderTarget = renderTargetTex.GetSurfaceLevel(0);
        }

        // Bind the per-cell render target and clear it to black, ready for additive accumulation.
        // Always sets the target unconditionally: the previous "GetRenderTarget(0) + Dispose() if it
        // isn't ours" optimization aliased our own render-target surface from the second cell onward
        // (GetRenderTarget returns the same wrapper we left bound), and disposing that wrapper destroyed
        // our render target, so every cell after the first failed at Clear with D3DERR_INVALIDCALL.
        private void BeginRenderTarget() {
            DXMain.device.SetRenderTarget(0, renderTarget);
            DXMain.device.Clear(ClearFlags.Target, 0, 0.0f, 0);
        }

        // Copy the render target into the uncompressed system texture, generate mips, compress each mip
        // into the DXT1 texture, and read the tightly-packed DXT1 mip-chain bytes back out. Mirrors
        // WorldTexCreator.FinishCompressed exactly (same FromSurface/FilterTexture/compress loop, same
        // Texture.ToFile D3DX serialize) except that it returns the DXT1 bytes instead of leaving them
        // in a .dds file. Each step sets bakeStage so a failure pinpoints the exact call.
        private byte[] CompressToDxt1Bytes(bool isSRGB) {
            bakeStage = "Compress.FromSurface";
            Surface tmp = uncompressedTex.GetSurfaceLevel(0);
            Surface.FromSurface(tmp, renderTarget, Filter.None, 0);
            tmp.Dispose();

            // Generate mips
            bakeStage = "Compress.FilterTexture";
            Filter filter = Filter.Triangle | (isSRGB ? Filter.Srgb : 0);
            uncompressedTex.FilterTexture(0, filter);

            // Compress mips
            bakeStage = "Compress.CompressMips";
            for (int i = 0; i < compressedTex.LevelCount; i++) {
                Surface dest = compressedTex.GetSurfaceLevel(i);
                Surface src = uncompressedTex.GetSurfaceLevel(i);
                Surface.FromSurface(dest, src, Filter.None, 0);
                src.Dispose();
                dest.Dispose();
            }

            bakeStage = "Compress.ReadBack";
            return ReadCompressedBytes(compressedTex);
        }

        // Serializes a DXT1 texture (full mip chain) to a .dds via the exact D3DX path the atlas bake
        // uses (Texture.ToFile -> WorldTexCreator.FinishCompressed), then reads the file back and strips
        // the DDS header, returning just the concatenated tightly-packed DXT1 mip blocks. This is the
        // layout CompositeSetWriter / dlcomposite.h / composite_format_model expect for composite.data.
        //
        // Why on-disk D3DX instead of an in-memory read: under the Remix d3d9 runtime both the
        // hand-rolled LockRectangle readback and the in-memory Texture.ToStream serializer returned
        // D3DERR_INVALIDCALL, while Texture.ToFile is proven on this device every run (it writes
        // world.dds). The scratch file lives in the user temp dir, never under the game install.
        private byte[] ReadCompressedBytes(Texture tex) {
            Texture.ToFile(tex, scratchDdsPath, ImageFileFormat.Dds);
            byte[] dds = System.IO.File.ReadAllBytes(scratchDdsPath);
            int headerBytes = DdsHeaderLength(dds);
            int payloadLen = dds.Length - headerBytes;
            var payload = new byte[payloadLen];
            Buffer.BlockCopy(dds, headerBytes, payload, 0, payloadLen);
            return payload;
        }

        // Length of a DDS file header: 4-byte "DDS " magic + 124-byte DDS_HEADER, plus the optional
        // 20-byte DX10 extension when the pixel format's fourCC is "DX10". DXT1 output uses the classic
        // header (no DX10 block), so this resolves to 128 bytes; the fourCC check keeps it correct if a
        // future format goes through the DX10 path.
        private const int DdsMagicAndHeaderBytes = 128; // "DDS " (4) + DDS_HEADER (124)
        private const int DdsDx10ExtensionBytes = 20;    // DDS_HEADER_DXT10

        private static int DdsHeaderLength(byte[] dds) {
            // dwFourCC sits at file offset 84 (magic 4 + ddspf offset 72 within DDS_HEADER + 8).
            const int fourCCOffset = 84;
            if (dds.Length >= fourCCOffset + 4 &&
                dds[fourCCOffset] == (byte)'D' && dds[fourCCOffset + 1] == (byte)'X' &&
                dds[fourCCOffset + 2] == (byte)'1' && dds[fourCCOffset + 3] == (byte)'0') {
                return DdsMagicAndHeaderBytes + DdsDx10ExtensionBytes;
            }
            return DdsMagicAndHeaderBytes;
        }

        // Synthesizes a solid neutral-gray DXT1 mip chain of the same byte layout a real bake produces.
        // Device-independent so it succeeds even when the device render path is what failed.
        private static byte[] CreatePlaceholderDxt1(int edgeTexels) {
            ushort gray565 = PackRgb565(128, 128, 128);
            byte lo = (byte)(gray565 & 0xFF);
            byte hi = (byte)((gray565 >> 8) & 0xFF);

            // DXT1 block: color0 == color1 (so opaque), all 2-bit indices select color0 -> flat color.
            byte[] block = new byte[Dxt1BlockBytes] { lo, hi, lo, hi, 0, 0, 0, 0 };

            using (var ms = new System.IO.MemoryStream()) {
                int w = edgeTexels, h = edgeTexels;
                while (true) {
                    int blocksWide = Math.Max(1, (w + 3) / 4);
                    int blocksHigh = Math.Max(1, (h + 3) / 4);
                    int blocks = blocksWide * blocksHigh;
                    for (int i = 0; i < blocks; i++) {
                        ms.Write(block, 0, Dxt1BlockBytes);
                    }
                    if (w == 1 && h == 1) {
                        break;
                    }
                    w = Math.Max(1, w >> 1);
                    h = Math.Max(1, h >> 1);
                }
                return ms.ToArray();
            }
        }

        private static ushort PackRgb565(int r, int g, int b) {
            return (ushort)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }

        private void LogFailure(CellId cell, string stage, Exception ex) {
            string message = "Composite bake failed for cell " + cell + " [stage " + stage + "]: " + ex.Message + " (placeholder emitted)";
            if (log != null) {
                log(message);
            } else {
                try {
                    System.IO.File.AppendAllText(Statics.fn_dlLog, message + "\r\n");
                } catch {
                }
            }
        }

        private void DisposeResources() {
            if (renderTarget != null) {
                renderTarget.Dispose();
                renderTarget = null;
            }
            if (renderTargetTex != null) {
                renderTargetTex.Dispose();
                renderTargetTex = null;
            }
            if (uncompressedTex != null) {
                uncompressedTex.Dispose();
                uncompressedTex = null;
            }
            if (compressedTex != null) {
                compressedTex.Dispose();
                compressedTex = null;
            }
            if (ctc != null) {
                ctc.Dispose();
                ctc = null;
            }
        }

        public void Dispose() {
            if (disposed) {
                return;
            }
            disposed = true;
            DisposeResources();
            try {
                if (scratchDdsPath != null && System.IO.File.Exists(scratchDdsPath)) {
                    System.IO.File.Delete(scratchDdsPath);
                }
            } catch {
            }
        }
    }
}
