// composite_mock_device_stubs.inl
//
// Inert IDirect3DDevice9 vtable fillers for the task 8.7 MockDevice (see
// composite_upload_test.cpp). AUTO-GENERATED from the DirectX SDK (June 2010) d3d9.h
// pure-virtual list (patches/rtxdll/gen_mock_stubs.py) and committed as a checked-in
// fragment so the test is self-contained and does not depend on the generator.
//
// Every IDirect3DDevice9 method the upload path does NOT call is stubbed to a harmless
// default here, so MockDevice is a complete, instantiable concrete class. The five
// methods with real test behavior -- QueryInterface, AddRef, Release, CreateTexture,
// UpdateTexture -- are hand-written in composite_upload_test.cpp and are intentionally
// absent from this file. This fragment is #included inside the MockDevice class body.
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0; }
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT,D3DDISPLAYMODE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT,UINT,IDirect3DSurface9 *) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetCursorPosition(int,int,DWORD) override { return; }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *,IDirect3DSwapChain9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT,IDirect3DSwapChain9 **) override { return E_NOTIMPL; }
    UINT STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 0; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Present(const RECT *,const RECT *,HWND,const RGNDATA *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT,UINT,D3DBACKBUFFER_TYPE,IDirect3DSurface9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT,D3DRASTER_STATUS *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return E_NOTIMPL; }
    void STDMETHODCALLTYPE SetGammaRamp(UINT,DWORD,const D3DGAMMARAMP *) override { return; }
    void STDMETHODCALLTYPE GetGammaRamp(UINT,D3DGAMMARAMP *) override { return; }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DVolumeTexture9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DCubeTexture9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT,DWORD,DWORD,D3DPOOL,IDirect3DVertexBuffer9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DIndexBuffer9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT,UINT,D3DFORMAT,D3DMULTISAMPLE_TYPE,DWORD,BOOL,IDirect3DSurface9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT,UINT,D3DFORMAT,D3DMULTISAMPLE_TYPE,DWORD,BOOL,IDirect3DSurface9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *,const RECT *,IDirect3DSurface9 *,const POINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *,IDirect3DSurface9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT,IDirect3DSurface9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *,const RECT *,IDirect3DSurface9 *,const RECT *,D3DTEXTUREFILTERTYPE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *,const RECT *,D3DCOLOR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT,UINT,D3DFORMAT,D3DPOOL,IDirect3DSurface9 **,HANDLE *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD,IDirect3DSurface9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD,IDirect3DSurface9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginScene() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD,const D3DRECT *,DWORD,D3DCOLOR,float,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE,const D3DMATRIX *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE,D3DMATRIX *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE,const D3DMATRIX *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD,const D3DLIGHT9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD,D3DLIGHT9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD,BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD,BOOL *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD,const float *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD,float *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE,DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE,IDirect3DStateBlock9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD,IDirect3DBaseTexture9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD,IDirect3DBaseTexture9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD,D3DTEXTURESTAGESTATETYPE,DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD,D3DTEXTURESTAGESTATETYPE,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD,D3DSAMPLERSTATETYPE,DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD,D3DSAMPLERSTATETYPE,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT,const PALETTEENTRY *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT,PALETTEENTRY *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetScissorRect(const RECT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL) override { return E_NOTIMPL; }
    BOOL STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return FALSE; }
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float) override { return E_NOTIMPL; }
    float STDMETHODCALLTYPE GetNPatchMode() override { return 0.0f; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE,UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,INT,UINT,UINT,UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE,UINT,const void *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE,UINT,UINT,UINT,const void *,D3DFORMAT,const void *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT,UINT,UINT,IDirect3DVertexBuffer9 *,IDirect3DVertexDeclaration9 *,DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9 *,IDirect3DVertexDeclaration9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD *,IDirect3DVertexShader9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT,const float *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT,float *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT,const int *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT,int *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT,const BOOL *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT,BOOL *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT,IDirect3DVertexBuffer9 *,UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT,IDirect3DVertexBuffer9 **,UINT *,UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT,UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD *,IDirect3DPixelShader9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT,const float *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT,float *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT,const int *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT,int *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT,const BOOL *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT,BOOL *,UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT,const float *,const D3DRECTPATCH_INFO *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT,const float *,const D3DTRIPATCH_INFO *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE,IDirect3DQuery9 **) override { return E_NOTIMPL; }
