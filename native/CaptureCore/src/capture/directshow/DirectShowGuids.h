#pragma once

// DirectShow ships its interface declarations (ICreateDevEnum, IGraphBuilder,
// IBaseFilter, ICaptureGraphBuilder2, ...) in the Windows SDK's strmif.h/control.h,
// but the GUID *values* for these well-known CLSIDs/media types, and the Sample
// Grabber's interfaces entirely, used to live in uuids.h/qedit.h — both removed from
// the SDK years ago. Redeclaring them locally (values copied from the last SDK that
// shipped them, unchanged since Windows 2000) is the standard workaround; ffmpeg's own
// libavdevice/dshow.c does the same thing for the same reason.
//
// Deliberately NOT linking strmiids.lib to get the CLSID/media-type values instead:
// since every symbol needed here is defined below, doing so would risk duplicate-
// symbol link errors against whichever of these strmiids.lib itself still provides.

#include <strmif.h>

namespace capturecore {

extern const CLSID CLSID_DShow_SystemDeviceEnum;
extern const CLSID CLSID_DShow_VideoInputDeviceCategory;
extern const CLSID CLSID_DShow_FilterGraph;
extern const CLSID CLSID_DShow_CaptureGraphBuilder2;
extern const CLSID CLSID_DShow_SampleGrabber;
extern const CLSID CLSID_DShow_NullRenderer;

extern const GUID MEDIATYPE_DShow_Video;
extern const GUID FORMAT_DShow_VideoInfo;
extern const GUID PIN_CATEGORY_DShow_Capture;
extern const GUID MEDIASUBTYPE_DShow_RGB24;
extern const GUID MEDIASUBTYPE_DShow_RGB32;
extern const GUID MEDIASUBTYPE_DShow_YUY2;
extern const GUID MEDIASUBTYPE_DShow_UYVY;

} // namespace capturecore

// The Sample Grabber's interfaces (qedit.h, also removed from the SDK). Declared at
// global scope, like every other COM interface, so __uuidof()/IID_PPV_ARGS work.
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};
