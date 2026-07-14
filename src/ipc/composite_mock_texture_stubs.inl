// composite_mock_texture_stubs.inl
//
// Inert IDirect3DTexture9 vtable fillers for the task 8.7 MockTexture (see
// composite_upload_test.cpp). AUTO-GENERATED from the DirectX SDK (June 2010) d3d9.h
// pure-virtual list (patches/rtxdll/gen_mock_stubs.py) and committed as a checked-in
// fragment so the test is self-contained.
//
// The methods with real test behavior -- QueryInterface, AddRef, Release, GetType,
// GetDevice, GetLevelCount, GetLevelDesc, LockRect, UnlockRect -- are hand-written in
// composite_upload_test.cpp and are intentionally absent here. This fragment is
// #included inside the MockTexture class body.
    HRESULT STDMETHODCALLTYPE SetPrivateData(const GUID &,const void *,DWORD,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(const GUID &,void *,DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(const GUID &) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override { return; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
    HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE) override { return E_NOTIMPL; }
    D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override { return D3DTEXF_NONE; }
    void STDMETHODCALLTYPE GenerateMipSubLevels() override { return; }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT,IDirect3DSurface9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *) override { return E_NOTIMPL; }
