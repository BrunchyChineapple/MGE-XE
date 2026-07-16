// composite_upload_test.cpp
//
// Task 8.7 - Integration test for client upload (Requirement 4.3).
//
// Req 4.3: "WHEN the IPC_Client receives a cell's compressed Per_Cell_Composite bytes,
// THE IPC_Client SHALL upload the texture to the D3D9 device that feeds Remix."
//
// This test EXERCISES THE REAL UPLOAD PATH. It compiles and links the production
// mge/compositecache.cpp and drives ResidentCompositeCache::admit() / lookup() with
// representative streamed cell blobs (CompositeChunkMsg + a matching DXT1 + full-mip-chain
// payload), exactly as the client reconcile loop (task 8.5) feeds it after the
// Composite_Streamer delivers a newly-visible cell's bytes. Nothing here is re-implemented:
// the staging-texture creation, the LockRect/UpdateTexture upload, the DXT1 sizing, and the
// byte-budget gate are all the real cache code.
//
// It uploads against TWO D3D9 devices, each verifying a different half of "correct DXT1
// size/format":
//
//   PART A - REAL headless D3D9 device (the strongest form; option a).
//     A real device is created (HAL, falling back to REF then NULLREF). The real cache
//     uploads 1024/256/64-edge composites to it and the resident texture is introspected
//     via the real driver's GetLevelDesc()/GetLevelCount(): the upload produced a texture
//     in D3DFMT_DXT1, at the streamed edge dimensions, with the full DXT1 mip chain. This
//     is the actual D3D9 upload path Remix consumes, run end to end.
//
//   PART B - MOCK IDirect3DDevice9 (option b; verifies what a real DEFAULT-pool texture
//     cannot expose). A DEFAULT-pool texture's bytes cannot be read back on a real device
//     (D3DXSaveTextureToFileInMemory returns D3DERR_INVALIDCALL on POOL_DEFAULT - verified
//     empirically during this task), so the per-mip *byte copy* is verified against a mock
//     device instead. The mock's IDirect3DTexture9::LockRect hands the cache a staging
//     buffer with an INFLATED pitch (pitch > the tight DXT1 block-row stride); if the real
//     cache honors the locked pitch (it does: it copies block-row by block-row at
//     dst + row*Pitch), de-pitching the captured bytes reproduces the source blob exactly.
//     The mock also records every CreateTexture call so the test asserts the cache created
//     a SYSTEMMEM staging texture then a DEFAULT-pool resident texture, both D3DFMT_DXT1,
//     both at the streamed edge, both with the full mip-level count, and that UpdateTexture
//     was issued staging -> default. Finally it drives the real cache's two non-upload exits
//     through the mock: a CreateTexture failure (admit -> false, Req 6.4) and a budget
//     rejection (admit -> false, not uploaded, Req 7.3).
//
// "Test passes" == process returns 0 and prints ALL_UPLOAD_CHECKS_PASS.
//
// Build (x86 client config; the cache is IPC-client / d3d8.dll, 32-bit, ptr32<T> == T*):
//   see patches/rtxdll/build_upload_test.bat. The build links the production
//   mge/compositecache.cpp and a no-op LOG::logline (the cache's only external symbol on
//   its error paths), so no other production object is dragged in.

#include <windows.h>

#include <d3d9.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mge/compositecache.h"   // the REAL ResidentCompositeCache under test
#include "ipc/bridge.h"           // CompositeChunkMsg (also pulled in by compositecache.h)

// ---------------------------------------------------------------------------
// Link stub: compositecache.cpp logs to LOG::logline only on its error paths. We link the
// real cache but not the whole logging subsystem, so provide a no-op definition that
// resolves the symbol. It is never required to do anything for the test to be meaningful.
// ---------------------------------------------------------------------------
namespace LOG {
    std::size_t logline(const char* /*fmt*/, ...) { return 0; }
}

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  PASS  %s\n", what);
    } else {
        std::printf("  FAIL  %s\n", what);
        ++g_failures;
    }
}

// ---- DXT1 sizing, recomputed independently of the cache so the test is self-checking. ----
constexpr std::uint32_t kDXT1BlockBytes = 8;

std::uint32_t dxt1LevelBytes(std::uint32_t w, std::uint32_t h) {
    const std::uint32_t bw = std::max<std::uint32_t>(1, (w + 3) / 4);
    const std::uint32_t bh = std::max<std::uint32_t>(1, (h + 3) / 4);
    return bw * bh * kDXT1BlockBytes;
}

std::uint32_t mipLevelCount(std::uint32_t edge) {
    std::uint32_t levels = 1;
    while (edge > 1) { edge >>= 1; ++levels; }
    return levels;
}

std::uint32_t dxt1FullChainBytes(std::uint32_t edge) {
    std::uint32_t total = 0, w = edge, h = edge;
    for (;;) {
        total += dxt1LevelBytes(w, h);
        if (w == 1 && h == 1) break;
        w = std::max<std::uint32_t>(1, w >> 1);
        h = std::max<std::uint32_t>(1, h >> 1);
    }
    return total;
}

// Build a deterministic, non-trivial DXT1 + full-mip-chain blob for a square edge. The
// content is a position-dependent pattern so a mis-stride (pitch ignored) copy cannot
// accidentally still compare equal.
std::vector<std::uint8_t> buildBlob(std::uint32_t edge, std::uint8_t salt) {
    std::vector<std::uint8_t> v(dxt1FullChainBytes(edge));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = static_cast<std::uint8_t>((i * 131u + salt * 17u + 7u) & 0xFF);
    }
    return v;
}

CompositeChunkMsg makeMsg(std::int32_t cellX, std::int32_t cellY, std::uint32_t edge) {
    CompositeChunkMsg m = {};
    m.cellX = cellX;
    m.cellY = cellY;
    m.edgeTexels = edge;
    m.byteLength = dxt1FullChainBytes(edge);
    m.status = CompositeCellStatus::Found;
    return m;
}

// ===========================================================================
// PART A - real headless D3D9 device
// ===========================================================================

IDirect3DDevice9* createHeadlessDevice(IDirect3D9* d3d, HWND hwnd, const char** outType) {
    const D3DDEVTYPE types[] = { D3DDEVTYPE_HAL, D3DDEVTYPE_REF, D3DDEVTYPE_NULLREF };
    const char* names[] = { "HAL", "REF", "NULLREF" };
    for (int i = 0; i < 3; ++i) {
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth = 4;
        pp.BackBufferHeight = 4;
        pp.hDeviceWindow = hwnd;
        IDirect3DDevice9* dev = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, types[i], hwnd,
                                       D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                       &pp, &dev);
        if (hr == D3D_OK && dev) {
            *outType = names[i];
            return dev;
        }
    }
    return nullptr;
}

void runRealDeviceTests() {
    std::printf("--- Part A: real headless D3D9 device (option a, strongest) ---\n");

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        std::printf("  SKIP  Direct3DCreate9 returned null (no D3D9 runtime here); "
                    "Part B (mock) still verifies the upload path.\n");
        return;
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "composite_upload_test", WS_OVERLAPPED,
                                0, 0, 8, 8, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    const char* devType = "?";
    IDirect3DDevice9* dev = createHeadlessDevice(d3d, hwnd, &devType);
    if (!dev) {
        std::printf("  SKIP  no headless device could be created (HAL/REF/NULLREF all failed); "
                    "Part B (mock) still verifies the upload path.\n");
        if (hwnd) DestroyWindow(hwnd);
        d3d->Release();
        return;
    }
    std::printf("  using a real %s device\n", devType);

    {
        ResidentCompositeCache cache;
        cache.init(dev);
        cache.setBudgetMB(64);   // default Texture_Memory_Budget (Req 7.4)

        // 1-3 representative streamed cell blobs: one realistic 1024 (699064 bytes), plus a
        // mid 256 and a small 64 to keep the run quick.
        const std::uint32_t edges[] = { 1024, 256, 64 };
        std::uint32_t expectedResidentBytes = 0;
        int admitted = 0;

        for (int i = 0; i < 3; ++i) {
            const std::uint32_t edge = edges[i];
            const std::vector<std::uint8_t> blob = buildBlob(edge, static_cast<std::uint8_t>(i + 1));
            const CompositeChunkMsg msg = makeMsg(/*cellX*/ static_cast<int>(edge), /*cellY*/ i, edge);

            const bool ok = cache.admit(msg, blob.data());
            char b[128];
            std::snprintf(b, sizeof(b), "admit() uploaded edge=%u full-mip blob (%u bytes)",
                          edge, msg.byteLength);
            check(ok, b);
            if (!ok) continue;
            ++admitted;
            expectedResidentBytes += ResidentCompositeCache::compositeBytes(edge);

            IDirect3DTexture9* tex = cache.lookup(CellId{ static_cast<int>(edge), i });
            std::snprintf(b, sizeof(b), "lookup() returns a resident texture for edge=%u", edge);
            check(tex != nullptr, b);
            if (!tex) continue;

            // Introspect the REAL resident texture: DXT1 format, edge dims, full mip chain.
            const DWORD levels = tex->GetLevelCount();
            std::snprintf(b, sizeof(b), "edge=%u resident texture has full DXT1 mip chain (%lu == %u)",
                          edge, (unsigned long)levels, mipLevelCount(edge));
            check(levels == mipLevelCount(edge), b);

            D3DSURFACE_DESC desc = {};
            const HRESULT hr = tex->GetLevelDesc(0, &desc);
            check(hr == D3D_OK, "GetLevelDesc(0) on the resident texture succeeds");
            if (hr == D3D_OK) {
                std::snprintf(b, sizeof(b), "edge=%u resident texture format is D3DFMT_DXT1", edge);
                check(desc.Format == D3DFMT_DXT1, b);
                std::snprintf(b, sizeof(b), "edge=%u resident texture is %ux%u (base level)",
                              edge, desc.Width, desc.Height);
                check(desc.Width == edge && desc.Height == edge, b);
            }
        }

        check(admitted == 3, "all three representative cells uploaded to the real device");
        check(cache.residentCount() == static_cast<std::uint32_t>(admitted),
              "residentCount() equals the number of uploaded cells");
        check(cache.residentBytes() == expectedResidentBytes,
              "residentBytes() equals the summed DXT1+mip budget cost of the uploaded cells");

        // Eviction releases the real D3D textures (resident set becomes subset of visible).
        VisibleCellSet emptyVisible;
        cache.evictNotVisible(emptyVisible);
        check(cache.residentCount() == 0, "evictNotVisible({}) releases every resident texture");
        check(cache.residentBytes() == 0, "residentBytes() returns to 0 after full eviction");

        // cache destructs here (releaseAll) before the device is released below.
    }

    dev->Release();
    if (hwnd) DestroyWindow(hwnd);
    d3d->Release();
}

// ===========================================================================
// PART B - mock IDirect3DDevice9 / IDirect3DTexture9
// ===========================================================================

// A recording mock texture. LockRect hands back a staging buffer with an INFLATED pitch so
// the test can prove the cache honors the locked pitch when copying each DXT1 block-row.
// UnlockRect de-pitches the captured rows into a tight, contiguous per-level store, which
// UpdateTexture then copies into the resident (DEFAULT) texture for read-back comparison.
class MockTexture : public IDirect3DTexture9 {
public:
    MockTexture(IDirect3DDevice9* dev, std::uint32_t w, std::uint32_t h, std::uint32_t levels,
                D3DFORMAT fmt, D3DPOOL pool)
        : device_(dev), width_(w), height_(h), levels_(levels), format_(fmt), pool_(pool),
          refs_(1) {
        levelData_.resize(levels_);
    }

    // --- COM lifetime (real behavior) ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (ppv) *ppv = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // --- queries the test/cache may use (real behavior) ---
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override {
        if (ppDevice) *ppDevice = device_;
        return S_OK;
    }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return levels_; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) override {
        if (!desc || level >= levels_) return D3DERR_INVALIDCALL;
        desc->Format = format_;
        desc->Type = D3DRTYPE_SURFACE;
        desc->Usage = 0;
        desc->Pool = pool_;
        desc->MultiSampleType = D3DMULTISAMPLE_NONE;
        desc->MultiSampleQuality = 0;
        desc->Width = std::max<std::uint32_t>(1, width_ >> level);
        desc->Height = std::max<std::uint32_t>(1, height_ >> level);
        return S_OK;
    }

    // Tight (contiguous) DXT1 block-row stride and block-row count for a level.
    void levelGeom(std::uint32_t level, std::uint32_t& rowBytes, std::uint32_t& blocksH) const {
        const std::uint32_t w = std::max<std::uint32_t>(1, width_ >> level);
        const std::uint32_t h = std::max<std::uint32_t>(1, height_ >> level);
        const std::uint32_t bw = std::max<std::uint32_t>(1, (w + 3) / 4);
        blocksH = std::max<std::uint32_t>(1, (h + 3) / 4);
        rowBytes = bw * kDXT1BlockBytes;
    }

    // LockRect with an INFLATED pitch (tight stride + padding) so honoring the pitch matters.
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* pLockedRect,
                                       const RECT*, DWORD) override {
        if (!pLockedRect || level >= levels_) return D3DERR_INVALIDCALL;
        std::uint32_t rowBytes = 0, blocksH = 0;
        levelGeom(level, rowBytes, blocksH);
        const std::uint32_t pitch = rowBytes + kPitchPadding;  // strictly > rowBytes
        if (pitch > maxPitchSeen) maxPitchSeen = pitch;
        if (pitch > rowBytes) sawInflatedPitch = true;
        lockScratch_.assign(static_cast<std::size_t>(pitch) * blocksH, 0xCD);  // 0xCD = untouched
        lockedLevel_ = level;
        lockedPitch_ = pitch;
        pLockedRect->Pitch = static_cast<INT>(pitch);
        pLockedRect->pBits = lockScratch_.data();
        return S_OK;
    }

    // UnlockRect de-pitches: copy each block-row's first rowBytes (stride = the locked pitch)
    // into a tight contiguous per-level store. If the cache wrote at dst + row*pitch (honoring
    // pitch), this reproduces the source; if it had written at dst + row*rowBytes (ignoring
    // pitch), the de-pitch would read shifted/garbled bytes and the later compare would fail.
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override {
        if (level != lockedLevel_) return D3DERR_INVALIDCALL;
        std::uint32_t rowBytes = 0, blocksH = 0;
        levelGeom(level, rowBytes, blocksH);
        std::vector<std::uint8_t>& dst = levelData_[level];
        dst.resize(static_cast<std::size_t>(rowBytes) * blocksH);
        for (std::uint32_t r = 0; r < blocksH; ++r) {
            std::memcpy(dst.data() + static_cast<std::size_t>(r) * rowBytes,
                        lockScratch_.data() + static_cast<std::size_t>(r) * lockedPitch_,
                        rowBytes);
        }
        lockedLevel_ = 0xFFFFFFFFu;
        return S_OK;
    }

#include "composite_mock_texture_stubs.inl"

    // Concatenate the de-pitched per-level bytes (mip0..mipN) into one contiguous blob.
    std::vector<std::uint8_t> concatLevels() const {
        std::vector<std::uint8_t> out;
        for (const auto& lv : levelData_) out.insert(out.end(), lv.begin(), lv.end());
        return out;
    }

    // Copy another mock texture's per-level stores into this one (UpdateTexture src->dst).
    void copyFrom(const MockTexture& src) { levelData_ = src.levelData_; }

    std::uint32_t width_, height_, levels_;
    D3DFORMAT format_;
    D3DPOOL pool_;
    std::vector<std::vector<std::uint8_t>> levelData_;  // tight, de-pitched, per level

    // pitch-honoring evidence (shared across instances via static so the test can read it).
    static constexpr std::uint32_t kPitchPadding = 32;  // makes pitch != tight rowBytes
    static std::uint32_t maxPitchSeen;
    static bool sawInflatedPitch;

private:
    IDirect3DDevice9* device_;
    ULONG refs_;
    std::vector<std::uint8_t> lockScratch_;
    std::uint32_t lockedLevel_ = 0xFFFFFFFFu;
    std::uint32_t lockedPitch_ = 0;
};

std::uint32_t MockTexture::maxPitchSeen = 0;
bool MockTexture::sawInflatedPitch = false;

// Recording mock device. CreateTexture logs each call and returns a MockTexture;
// UpdateTexture copies the staging texture's bytes into the resident texture.
class MockDevice : public IDirect3DDevice9 {
public:
    struct CreateRec {
        std::uint32_t width, height, levels;
        D3DFORMAT format;
        D3DPOOL pool;
    };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        if (ppv) *ppv = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD /*Usage*/,
                                            D3DFORMAT Format, D3DPOOL Pool,
                                            IDirect3DTexture9** ppTexture, HANDLE* /*pSharedHandle*/) override {
        createLog.push_back(CreateRec{ Width, Height, Levels, Format, Pool });
        if (failDefaultCreate && Pool == D3DPOOL_DEFAULT) {
            if (ppTexture) *ppTexture = nullptr;
            return D3DERR_INVALIDCALL;   // drives the cache's upload-failure exit (Req 6.4)
        }
        if (!ppTexture) return D3DERR_INVALIDCALL;
        // The cache always passes an explicit level count for DXT1 composites.
        const std::uint32_t levels = (Levels == 0) ? mipLevelCount(Width) : Levels;
        *ppTexture = new MockTexture(this, Width, Height, levels, Format, Pool);
        if (Pool == D3DPOOL_DEFAULT) lastDefault = static_cast<MockTexture*>(*ppTexture);
        return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* src,
                                            IDirect3DBaseTexture9* dst) override {
        ++updateCount;
        MockTexture* s = static_cast<MockTexture*>(static_cast<IDirect3DTexture9*>(src));
        MockTexture* d = static_cast<MockTexture*>(static_cast<IDirect3DTexture9*>(dst));
        if (!s || !d) return D3DERR_INVALIDCALL;
        d->copyFrom(*s);
        return D3D_OK;
    }

#include "composite_mock_device_stubs.inl"

    void reset() {
        createLog.clear();
        updateCount = 0;
        lastDefault = nullptr;
    }

    std::vector<CreateRec> createLog;
    int updateCount = 0;
    bool failDefaultCreate = false;
    MockTexture* lastDefault = nullptr;
};

void runMockDeviceTests() {
    std::printf("--- Part B: mock IDirect3DDevice9 (option b, per-mip pitch-honoring copy) ---\n");

    MockDevice dev;
    ResidentCompositeCache cache;
    cache.init(&dev);
    cache.setBudgetMB(64);

    MockTexture::maxPitchSeen = 0;
    MockTexture::sawInflatedPitch = false;

    const std::uint32_t edges[] = { 256, 128, 64 };
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t edge = edges[i];
        dev.reset();

        const std::vector<std::uint8_t> blob = buildBlob(edge, static_cast<std::uint8_t>(i + 10));
        const CompositeChunkMsg msg = makeMsg(/*cellX*/ -static_cast<int>(edge), /*cellY*/ i, edge);

        const bool ok = cache.admit(msg, blob.data());
        char b[160];
        std::snprintf(b, sizeof(b), "admit() succeeded against the mock for edge=%u", edge);
        check(ok, b);
        if (!ok) continue;

        // The cache created a SYSTEMMEM staging texture, then a DEFAULT resident texture.
        check(dev.createLog.size() == 2,
              "admit() issued exactly two CreateTexture calls (staging + resident)");
        if (dev.createLog.size() == 2) {
            const MockDevice::CreateRec& staging = dev.createLog[0];
            const MockDevice::CreateRec& resident = dev.createLog[1];
            const std::uint32_t levels = mipLevelCount(edge);

            check(staging.pool == D3DPOOL_SYSTEMMEM, "first CreateTexture is the SYSTEMMEM staging texture");
            check(staging.format == D3DFMT_DXT1, "staging texture is created as D3DFMT_DXT1");
            check(staging.width == edge && staging.height == edge,
                  "staging texture is created at the streamed edge dimensions");
            check(staging.levels == levels, "staging texture is created with the full DXT1 mip-level count");

            check(resident.pool == D3DPOOL_DEFAULT, "second CreateTexture is the DEFAULT-pool resident texture");
            check(resident.format == D3DFMT_DXT1, "resident texture is created as D3DFMT_DXT1");
            check(resident.width == edge && resident.height == edge,
                  "resident texture is created at the streamed edge dimensions");
            check(resident.levels == levels, "resident texture is created with the full DXT1 mip-level count");
        }
        check(dev.updateCount == 1, "admit() issued UpdateTexture(staging -> resident) exactly once");

        // Read back the resident texture's de-pitched bytes and compare to the source blob.
        // Equality here only holds if the cache honored the inflated LockRect pitch for every
        // mip level (Req 4.3: the streamed bytes are uploaded faithfully).
        IDirect3DTexture9* residentTex = cache.lookup(CellId{ -static_cast<int>(edge), i });
        std::snprintf(b, sizeof(b), "lookup() returns the resident texture for edge=%u", edge);
        check(residentTex != nullptr, b);
        if (residentTex) {
            MockTexture* mt = static_cast<MockTexture*>(residentTex);
            const std::vector<std::uint8_t> got = mt->concatLevels();
            std::snprintf(b, sizeof(b),
                          "edge=%u uploaded bytes equal the streamed blob across all mips "
                          "(pitch-honored, %zu bytes)", edge, got.size());
            check(got.size() == blob.size() && std::memcmp(got.data(), blob.data(), blob.size()) == 0, b);
        }
    }

    check(MockTexture::sawInflatedPitch,
          "the mock handed the cache an inflated LockRect pitch (the copy actually had to honor it)");

    // ---- Upload-failure exit through the REAL cache: CreateTexture(DEFAULT) fails (Req 6.4).
    {
        dev.reset();
        dev.failDefaultCreate = true;
        const std::uint32_t edge = 128;
        const std::vector<std::uint8_t> blob = buildBlob(edge, 99);
        const CompositeChunkMsg msg = makeMsg(7777, 7777, edge);
        const std::uint32_t bytesBefore = cache.residentBytes();
        const bool ok = cache.admit(msg, blob.data());
        check(!ok, "admit() returns false when the resident CreateTexture fails (Req 6.4)");
        check(cache.lookup(CellId{ 7777, 7777 }) == nullptr,
              "a failed upload leaves the cell non-resident (binder falls back to the atlas)");
        check(cache.residentBytes() == bytesBefore,
              "a failed upload does not change resident byte accounting");
        dev.failDefaultCreate = false;
    }

    // ---- Budget-rejection exit through the REAL cache: cell would exceed budget (Req 7.3).
    {
        ResidentCompositeCache tiny;
        tiny.init(&dev);
        tiny.setBudgetMB(0);   // 0 MiB budget => any non-empty cell is rejected before upload
        dev.reset();
        const std::uint32_t edge = 64;
        const std::vector<std::uint8_t> blob = buildBlob(edge, 5);
        const CompositeChunkMsg msg = makeMsg(8888, 8888, edge);
        const bool ok = tiny.admit(msg, blob.data());
        check(!ok, "admit() returns false when the cell would exceed the budget (Req 7.3)");
        check(dev.createLog.empty(),
              "a budget-rejected cell is NOT uploaded (no CreateTexture issued)");
        check(tiny.lookup(CellId{ 8888, 8888 }) == nullptr,
              "a budget-rejected cell is not resident (binder falls back to the atlas)");
        tiny.releaseAll();
    }

    cache.releaseAll();
}

}  // namespace

int main() {
    std::printf("=== Composite client upload integration test (task 8.7, Req 4.3) ===\n");
    runRealDeviceTests();
    runMockDeviceTests();

    if (g_failures == 0) {
        std::printf("ALL_UPLOAD_CHECKS_PASS\n");
        return 0;
    }
    std::printf("UPLOAD_CHECKS_FAILED count=%d\n", g_failures);
    return 1;
}
