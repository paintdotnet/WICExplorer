﻿//----------------------------------------------------------------------------------------
// THIS CODE AND INFORMATION IS PROVIDED "AS-IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//----------------------------------------------------------------------------------------
#include "pch.h"

#include "Element.h"
#include "Stopwatch.h"
#include "PropVariant.h"
#include "MetadataTranslator.h"
#include "resource.h"

class CProgressiveBitmapSource final : public IWICBitmapSource
{
public:
    CProgressiveBitmapSource(IWICBitmapSource * source, int level) :
        m_level(level),
        m_source(source)
    {
        m_source->AddRef();
        m_source->QueryInterface(IID_PPV_ARGS(&m_prog));
        m_prog->GetLevelCount(&m_max);
        m_max--;
    }

    ~CProgressiveBitmapSource()
    {
        m_source->Release();
        m_prog->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID riid,
            void **ppvObject) override
    {
        if (riid == IID_IUnknown)
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }

        if (riid == IID_IWICBitmapSource)
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        m_ref++;
        return m_ref;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        m_ref--;
        if (!m_ref)
        {
            delete this;
            return 0;
        }

        return m_ref;
    }

    STDMETHOD(GetSize)(
        UINT *puiWidth,
        UINT *puiHeight
        ) override
    {
        return m_source->GetSize(puiWidth, puiHeight);
    }

    STDMETHOD( GetPixelFormat)(
        WICPixelFormatGUID *pPixelFormat
        ) override
    {
        return m_source->GetPixelFormat(pPixelFormat);
    }

    STDMETHOD( GetResolution)(
        double *pDpiX,
        double *pDpiY
        ) override
    {
        return m_source->GetResolution(pDpiX, pDpiY);
    }

    STDMETHOD( CopyPalette)(
        IWICPalette *pIPalette
        ) override
    {
        return m_source->CopyPalette(pIPalette);
    }

    STDMETHOD( CopyPixels)(
        const WICRect *prc,
        const UINT cbStride,
        const UINT cbBufferSize,
        BYTE *pbBuffer
        ) override
    {
        HRESULT result;

        IFC(m_prog->SetCurrentLevel(static_cast<UINT>(m_level)));
        IFC(m_source->CopyPixels(prc, cbStride, cbBufferSize, pbBuffer));
        IFC(m_prog->SetCurrentLevel(static_cast<UINT>(m_max)));

        return result;
    }


private:
    int m_level{};
    UINT m_max{};
    IWICBitmapSource * m_source{};
    IWICProgressiveLevelControl * m_prog{};
    int m_ref{};
};


IWICImagingFactoryPtr g_imagingFactory;

CInfoElement::CInfoElement(LPCWSTR name)
{
    m_name = name;
    CElementManager::RegisterElement(this);
}

CInfoElement::~CInfoElement()
{
    RemoveChildren();
}

void CInfoElement::SetParent(CInfoElement *element)
{
    if(m_parent != element)
    {
        if(m_parent)
        {
            m_parent->AddChild(this);
        }
        else
        {
            Unlink();
        }
    }
}

//Adds element after this object
void CInfoElement::AddSibling(CInfoElement *element)
{
    element->Unlink();
    element->m_prevSibling = this;
    element->m_nextSibling = m_nextSibling;
    m_nextSibling = element;
    element->m_parent = m_parent;
}

void CInfoElement::AddChild(CInfoElement *element)
{
    CInfoElement *firstChild = FirstChild();
    if(!firstChild)
    {
        element->Unlink();
        m_firstChild = element;
        element->m_parent = this;
    }
    else
    {
        CInfoElement *prev = firstChild;
        while(firstChild)
        {
            prev = firstChild;
            firstChild = firstChild->NextSibling();
        }
        prev->AddSibling(element);
    }
}

void CInfoElement::RemoveChildren()
{
    //First unlink the children from the tree, then delete them
    while(m_firstChild)
    {
        RemoveChild(m_firstChild);
    }
}

void CInfoElement::Unlink()
{
    if(m_parent && m_parent->m_firstChild == this)
    {
        m_parent->m_firstChild = m_nextSibling;
    }
    if(m_nextSibling)
    {
        m_nextSibling->m_prevSibling = m_prevSibling;
    }
    if(m_prevSibling)
    {
        m_prevSibling->m_nextSibling = m_nextSibling;
    }
    m_nextSibling = nullptr;
    m_prevSibling = nullptr;
    m_parent = nullptr;
}

void CInfoElement::RemoveChild(CInfoElement *child)
{
    if(child->m_parent != this)
    {
        return;
    }
    child->Unlink();
    delete child;
}

HRESULT CElementManager::OpenFile(LPCWSTR filename, ICodeGenerator &codeGen, CInfoElement *&decElem)
{
    HRESULT result = E_UNEXPECTED;

    ATLASSERT(g_imagingFactory);

    if (g_imagingFactory)
    {
        // Refresh the list of codecs
        IEnumUnknownPtr enumUnknown;
        IFC(g_imagingFactory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateRefresh, &enumUnknown));

        // Begin monitoring how long this takes
        CStopwatch creationTimer;
        creationTimer.Start();

        decElem = nullptr;
        IFC(CreateDecoderAndChildElements(filename, codeGen, decElem));

        codeGen.EndVariableScope();

        // Remember how long this took
        if (decElem)
        {
            auto* realDecElem = dynamic_cast<CBitmapDecoderElement*>(decElem);
            if (nullptr != realDecElem)
            {
                realDecElem->SetCreationTime(creationTimer.GetTimeMS());

                CString code;
                codeGen.GenerateCode(code);
                realDecElem->SetCreationCode(code);
            }
        }
    }

    return result;
}

void CBitmapDecoderElement::Unload()
{
    if(m_decoder)
    {
        m_decoder->Release();
        m_decoder = nullptr;
    }

    RemoveChildren();
    m_loaded = FALSE;
}

HRESULT CBitmapDecoderElement::Load(ICodeGenerator &codeGen)
{
    HRESULT result = S_OK;

    Unload();
    codeGen.BeginVariableScope(L"IWICBitmapDecoder*", L"decoder", L"NULL");
    codeGen.CallFunction(L"imagingFactory->CreateDecoderFromFilename(\"%s\", NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)", m_filename.GetString());
    IFC(g_imagingFactory->CreateDecoderFromFilename(m_filename, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &m_decoder));

    // For each of the frames, create an element
    UINT frameCount = 0;

    codeGen.CallFunction(L"decoder->GetFrameCount(&frameCount)");
    IFC(m_decoder->GetFrameCount(&frameCount));

    // Even if one frame fails, we keep trying the others. However, we still
    // want to remember the failure so that we can return it from this function.
    HRESULT lastFailResult = result;

    if (frameCount == 0)
    {
        lastFailResult = E_FAIL;
    }

    for (UINT i = 0; i < frameCount; i++)
    {
        codeGen.BeginVariableScope(L"IWICBitmapFrameDecode*", L"frameDecode", L"NULL");
        IWICBitmapFrameDecodePtr frameDecode = NULL;

        codeGen.CallFunction(L"decoder->GetFrame(%d, &frameDecode)", i);
        HRESULT frameResult = m_decoder->GetFrame(i, &frameDecode);

        if (SUCCEEDED(frameResult))
        {
            frameResult = CElementManager::CreateFrameAndChildElements(this, i, frameDecode, codeGen);
        }

        if (FAILED(frameResult))
        {
            lastFailResult = frameResult;
        }

        codeGen.EndVariableScope();
    }

    // Add the Thumbnail if it exists
    IWICBitmapSourcePtr thumb;

    codeGen.CallFunction(L"decoder->GetThumbnail(&thumb)");
    result = m_decoder->GetThumbnail(&thumb);

    if (SUCCEEDED(result))
    {
        CInfoElement *thumbElem = new CBitmapSourceElement(L"Thumbnail", thumb);
        CElementManager::AddChildToElement(this, thumbElem);
    }
    else
    {
        result = S_OK;
    }

    // Add the Preview if it exists
    IWICBitmapSourcePtr preview;

    codeGen.CallFunction(L"decoder->GetPreview(&preview)");
    result = m_decoder->GetPreview(&preview);

    if (SUCCEEDED(result))
    {
        CInfoElement *prevElem = new CBitmapSourceElement(L"Preview", preview);
        CElementManager::AddChildToElement(this, prevElem);
    }
    else
    {
        result = S_OK;
    }

    // For each of the MetadataReaders attached to the decoder, create an element
    IWICMetadataBlockReaderPtr blockReader;

    result = m_decoder->QueryInterface(IID_PPV_ARGS(&blockReader));

    if (SUCCEEDED(result))
    {
        IFC(CElementManager::CreateMetadataElementsFromBlock(this, blockReader, codeGen));
    }
    else
    {
        result = S_OK;
    }

    codeGen.EndVariableScope();

    m_loaded = FirstChild() != nullptr;

    return (FAILED(lastFailResult)) ? lastFailResult : result;
}

HRESULT CElementManager::CreateDecoderAndChildElements(LPCWSTR filename, ICodeGenerator &codeGen, CInfoElement *&decElem)
{
    decElem = new CBitmapDecoderElement(filename);
    const HRESULT result = (static_cast<CBitmapDecoderElement *>(decElem)->Load(codeGen));
    if(!static_cast<CBitmapDecoderElement *>(decElem)->IsLoaded())
    {
        root.RemoveChild(decElem);
        decElem = nullptr;
    }

    return result;
}

HRESULT CElementManager::CreateFrameAndChildElements(CInfoElement *parent, UINT index, IWICBitmapFrameDecodePtr frameDecode, ICodeGenerator &codeGen)
{
    // Add the frame itself
    CInfoElement *frameElem = new CBitmapFrameDecodeElement(index, frameDecode);

    AddChildToElement(parent, frameElem);

    // Add the Thumbnail if it exists
    IWICBitmapSourcePtr thumb;

    codeGen.CallFunction(L"frameDecode->GetThumbnail(&thumb)");
    HRESULT result = frameDecode->GetThumbnail(&thumb);

    if (SUCCEEDED(result))
    {
        CInfoElement *thumbElem = new CBitmapSourceElement(L"Thumbnail", thumb);
        AddChildToElement(frameElem, thumbElem);
    }
    else
    {
        result = S_OK;
    }

    IWICProgressiveLevelControlPtr prog;
    result = frameDecode->QueryInterface(IID_PPV_ARGS(&prog));

    if (SUCCEEDED(result))
    {
        UINT count = 0;
        result = prog->GetLevelCount(&count);
        if (count > 1)
        {
            for (int c = 0; c < (int)count; c++)
            {
                AddChildToElement(frameElem, new CBitmapSourceElement(L"Level",
                                              new CProgressiveBitmapSource(frameDecode, c)));
            }
        }
    }

    // For each of the MetadataReaders attached to the frame, create an element
    codeGen.BeginVariableScope(L"IWICMetadataBlockReader*", L"blockReader", L"NULL");
    IWICMetadataBlockReaderPtr blockReader;

    codeGen.CallFunction(L"frameDecode->QueryInterface(IID_IWICMetadataBlockReader, (void**)&blockReader)");
    result = frameDecode->QueryInterface(IID_PPV_ARGS(&blockReader));

    if (SUCCEEDED(result))
    {
        IFC(CreateMetadataElementsFromBlock(frameElem, blockReader, codeGen));
    }
    else
    {
        result = S_OK;
    }

    codeGen.EndVariableScope();

    return result;
}

HRESULT CElementManager::CreateMetadataElementsFromBlock(CInfoElement *parent, IWICMetadataBlockReaderPtr blockReader, ICodeGenerator &codeGen)
{
    HRESULT result;

    UINT blockCount = 0;

    codeGen.CallFunction(L"blockReader->GetCount(&count)");
    IFC(blockReader->GetCount(&blockCount));

    for (UINT i = 0; i < blockCount; i++)
    {
        codeGen.BeginVariableScope(L"IWICMetadataReader*", L"reader", L"NULL");
        IWICMetadataReaderPtr reader;

        codeGen.CallFunction(L"blockReader->GetReaderByIndex(%d, &reader)", i);
        IFC(blockReader->GetReaderByIndex(i, &reader));

        IFC(CreateMetadataElements(parent, i, reader, codeGen));

        codeGen.EndVariableScope();
    }

    return result;
}

HRESULT CElementManager::CreateMetadataElements(CInfoElement *parent, UINT childIdx, IWICMetadataReaderPtr reader, ICodeGenerator &codeGen)
{
    HRESULT result = S_OK;

    // Add this reader
    CInfoElement *readerElem = new CMetadataReaderElement(parent, childIdx, reader);

    AddChildToElement(parent, readerElem);

    // Search for any embedded readers
    UINT numValues = 0;

    codeGen.CallFunction(L"reader->GetCount(&count)");
    IFC(reader->GetCount(&numValues));

    for (UINT i = 0; i < numValues; i++)
    {
        PROPVARIANT id, value;

        PropVariantInit(&id);
        PropVariantInit(&value);

        codeGen.CallFunction(L"reader->GetValueByIndex(%d, NULL, &id, &value)", i);
        IFC(reader->GetValueByIndex(i, NULL, &id, &value));

        if (VT_UNKNOWN == value.vt)
        {
            // Attempt to QI for a BlockReader
            codeGen.BeginVariableScope(L"IWICMetadataReader*", L"embReader", L"NULL");
            IWICMetadataReaderPtr embReader;

            codeGen.CallFunction(L"value.punkVal->QueryInterface(IID_IWICMetadataReader, (void**)&embReader)");
            result = value.punkVal->QueryInterface(IID_PPV_ARGS(&embReader));

            if (SUCCEEDED(result))
            {
                IFC(CreateMetadataElements(readerElem, i, embReader, codeGen));
            }
            else
            {
                result = S_OK;
            }

            codeGen.EndVariableScope();
        }
        else if ((VT_VECTOR | VT_UNKNOWN) == value.vt)
        {

        }

        PropVariantClear(&id);
        PropVariantClear(&value);
    }

    return result;
}

void CElementManager::RegisterElement(CInfoElement *element)
{
    if(!element->Parent() && !root.IsChild(element) && element != &root)
    {
        root.AddChild(element);
    }
}

void CElementManager::ClearAllElements()
{
    root.RemoveChildren();
}

void CElementManager::AddSiblingToElement(CInfoElement *element, CInfoElement *sib)
{
    ATLASSERT(NULL != element);

    if (nullptr != element)
    {
        // Find the end of the sibling chain
        CInfoElement *curr = element;
        while (nullptr != curr->NextSibling())
        {
            curr = curr->NextSibling();
        }

        // Add
        curr->AddSibling(sib);
    }
}

void CElementManager::AddChildToElement(CInfoElement *element, CInfoElement *child)
{
    ATLASSERT(NULL != element);

    if (nullptr != element)
    {
        element->AddChild(child);
    }
}

CInfoElement *CElementManager::GetRootElement()
{
    return &root;
}

HRESULT CElementManager::SaveElementAsImage(CInfoElement &element, REFGUID containerFormat, WICPixelFormatGUID &format, LPCWSTR filename, ICodeGenerator &codeGen)
{
    HRESULT result = S_OK;

    CImageTransencoder te;

    IFC(te.Begin(containerFormat, filename, codeGen));
    te.m_format = format;

    IFC(element.SaveAsImage(te, codeGen));
    format = te.m_format;

    IFC(te.End());

    return result;
}

CInfoElement CElementManager::root(L"");

//----------------------------------------------------------------------------------------
// COMPONENT INFO ELEMENT
//----------------------------------------------------------------------------------------

HRESULT CComponentInfoElement::OutputMetadataHandlerInfo(IOutputDevice &output, IWICMetadataHandlerInfoPtr metaInfo)
{
    HRESULT result = S_OK;

    CString str;
    WCHAR guidString[64];
    GUID guid{};

    output.BeginKeyValues(L"MetadataHandlerInfo");

    IFC(metaInfo->GetMetadataFormat(&guid));
    StringFromGUID2(guid, guidString, 64);
    output.AddKeyValue(L"MetadataFormat", guidString);

    READ_WIC_STRING(metaInfo->GetDeviceManufacturer, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"DeviceManufacturer", str);
    }

    READ_WIC_STRING(metaInfo->GetDeviceModels, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"DeviceModels", str);
    }

    output.EndKeyValues();

    IFC(OutputComponentInfo(output, metaInfo));

    return result;
}

HRESULT CComponentInfoElement::OutputDecoderInfo(IOutputDevice &output, IWICBitmapDecoderInfoPtr decoderInfo)
{
    HRESULT result = S_OK;

    IFC(OutputCodecInfo(output, decoderInfo));

    return result;
}

HRESULT CComponentInfoElement::OutputCodecInfo(IOutputDevice &output, IWICBitmapCodecInfoPtr codecInfo)
{
    HRESULT result = S_OK;

    BOOL b;
    CString str;
    WCHAR guidString[64];
    GUID guid{};

    output.BeginKeyValues(L"CodecInfo");

    IFC(codecInfo->GetContainerFormat(&guid));
    StringFromGUID2(guid, guidString, 64);
    output.AddKeyValue(L"ContainerFormat", guidString);

    READ_WIC_STRING(codecInfo->GetColorManagementVersion, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"ColorManagementVersion", str);
    }

    READ_WIC_STRING(codecInfo->GetDeviceManufacturer, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"DeviceManufacturer", str);
    }

    READ_WIC_STRING(codecInfo->GetDeviceModels, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"DeviceModels", str);
    }

    READ_WIC_STRING(codecInfo->GetMimeTypes, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"MimeTypes", str);
    }

    IFC(codecInfo->DoesSupportAnimation(&b));
    output.AddKeyValue(L"DoesSupportAnimation", b ? L"True" : L"False");

    IFC(codecInfo->DoesSupportChromakey(&b));
    output.AddKeyValue(L"DoesSupportChromakey", b ? L"True" : L"False");

    IFC(codecInfo->DoesSupportLossless(&b));
    output.AddKeyValue(L"DoesSupportLossless", b ? L"True" : L"False");

    IFC(codecInfo->DoesSupportMultiframe(&b));
    output.AddKeyValue(L"DoesSupportMultiframe", b ? L"True" : L"False");

    output.EndKeyValues();

    IFC(OutputComponentInfo(output, codecInfo));

    return result;
}

HRESULT CComponentInfoElement::OutputComponentInfo(IOutputDevice &output, IWICComponentInfoPtr compInfo)
{
    HRESULT result = S_OK;

    CString str;
    WCHAR guidString[64];
    GUID guid{};

    output.BeginKeyValues(L"ComponentInfo");

    IFC(compInfo->GetCLSID(&guid));
    StringFromGUID2(guid, guidString, 64);
    output.AddKeyValue(L"ClassID", guidString);

    READ_WIC_STRING(compInfo->GetAuthor, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"Author", str);
    }

    IFC(compInfo->GetVendorGUID(&guid));
    StringFromGUID2(guid, guidString, 64);
    output.AddKeyValue(L"VendorGUID", guidString);

    READ_WIC_STRING(compInfo->GetVersion, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"Version", str);
    }

    READ_WIC_STRING(compInfo->GetSpecVersion, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"SpecVersion", str);
    }

    READ_WIC_STRING(compInfo->GetFriendlyName, str);
    if (SUCCEEDED(result))
    {
        output.AddKeyValue(L"FriendlyName", str);
    }

    output.EndKeyValues();

    return result;
}


//----------------------------------------------------------------------------------------
// BITMAP DECODER ELEMENT
//----------------------------------------------------------------------------------------

void CBitmapDecoderElement::FillContextMenu(HMENU context)
{
    CComponentInfoElement::FillContextMenu(context);

    MENUITEMINFO itemInfo{};
    itemInfo.cbSize = sizeof(MENUITEMINFO);
    itemInfo.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING;
    itemInfo.fType = MFT_STRING;
    itemInfo.fState = MFS_ENABLED;

    if(m_loaded)
    {
        itemInfo.wID = ID_FILE_SAVE;
        itemInfo.dwTypeData = const_cast<LPWSTR>(L"Save As Image...");
        InsertMenuItem(context, GetMenuItemCount(context), TRUE, &itemInfo);

        itemInfo.wID = ID_FILE_UNLOAD;
        itemInfo.dwTypeData = const_cast<LPWSTR>(L"Unload");
    }
    else
    {
        itemInfo.wID = ID_FILE_LOAD;
        itemInfo.dwTypeData = const_cast<LPWSTR>(L"Load");
    }
    InsertMenuItem(context, GetMenuItemCount(context), TRUE, &itemInfo);

    itemInfo.wID = ID_FILE_CLOSE;
    itemInfo.dwTypeData = const_cast<LPWSTR>(L"Close");
    InsertMenuItem(context, GetMenuItemCount(context), TRUE, &itemInfo);
}

HRESULT CBitmapDecoderElement::SaveAsImage(CImageTransencoder &trans, ICodeGenerator &codeGen)
{
    HRESULT result = S_OK;
    if(!m_loaded)
    {
        return E_FAIL;
    }

    // Find the frame children and output them
    CInfoElement *child = FirstChild();
    while (nullptr != child)
    {
        CBitmapFrameDecodeElement *frameDecodeElement = dynamic_cast<CBitmapFrameDecodeElement*>(child);
        if (nullptr != frameDecodeElement)
        {
            IFC(frameDecodeElement->SaveAsImage(trans, codeGen));
        }

        child = child->NextSibling();
    }

    // Output Thumbnail
    codeGen.BeginVariableScope(L"IWICBitmapSource*", L"thumb", L"NULL");
    IWICBitmapSourcePtr thumb = NULL;

    codeGen.CallFunction(L"decoder->GetThumbnail(&thumb)");
    m_decoder->GetThumbnail(&thumb);

    if (NULL != thumb)
    {
        IFC(trans.SetThumbnail(thumb));
    }

    codeGen.EndVariableScope();

    // Output Preview
    codeGen.BeginVariableScope(L"IWICBitmapSource*", L"preview", L"NULL");
    IWICBitmapSourcePtr preview = NULL;

    codeGen.CallFunction(L"decoder->GetPreview(&preview)");
    m_decoder->GetPreview(&preview);

    if (NULL != preview)
    {
        IFC(trans.SetPreview(preview));
    }

    codeGen.EndVariableScope();

    // TODO: Output ColorContext

    // TODO: Output Metadata

    return result;
}

HRESULT CBitmapDecoderElement::OutputView(IOutputDevice &output, const InfoElementViewContext& context)
{
    HRESULT result = S_OK;

    if(!m_loaded)
    {
        int oldSize = output.SetFontSize(20);
        output.AddText(L"File not loaded");
        output.SetFontSize(oldSize);
        return result;
    }

    ATLASSERT(NULL != m_decoder);

    if (NULL != m_decoder)
    {
        output.BeginKeyValues(L"");

        output.AddKeyValue(L"Filename", m_filename);

        // Get the number of frames
        UINT numFrames = 0;
        m_decoder->GetFrameCount(&numFrames);
        CString value;
        value.Format(L"%u", numFrames);
        output.AddKeyValue(L"FrameCount", value);

        // Display the decode time
        value.Format(L"%u ms", m_creationTime);
        output.AddKeyValue(L"CreationTime", value);

        output.EndKeyValues();

        // Also show the children
        CInfoElement *child = FirstChild();
        while (nullptr != child)
        {
            output.BeginSection(child->Name());

            child->OutputView(output, context);
            child = child->NextSibling();

            output.EndSection();
        }

        // Show the code
        if (m_creationCode.GetLength() > 0)
        {
            output.BeginSection(L"Creation Code");
            output.AddVerbatimText(m_creationCode);
            output.EndSection();
        }
    }
    else
    {
    }

    return result;
}

HRESULT CBitmapDecoderElement::OutputInfo(IOutputDevice &output)
{
    HRESULT result = S_OK;

    if(m_loaded)
    {
        IWICBitmapDecoderInfoPtr decoderInfo;
        IFC(m_decoder->GetDecoderInfo(&decoderInfo));
        IFC(OutputDecoderInfo(output, decoderInfo));
    }

    return result;
}

void CBitmapDecoderElement::SetCreationTime(DWORD ms)
{
    m_creationTime = ms;
}

void CBitmapDecoderElement::SetCreationCode(LPCWSTR code)
{
    m_creationCode = code;
}


//----------------------------------------------------------------------------------------
// BITMAP SOURCE ELEMENT
//----------------------------------------------------------------------------------------

void CBitmapSourceElement::FillContextMenu(const HMENU context)
{
    CInfoElement::FillContextMenu(context);

    // Add the Save as Bitmap string if this element supports it
    MENUITEMINFO itemInfo{};
    itemInfo.cbSize = sizeof(MENUITEMINFO);
    itemInfo.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING;
    itemInfo.fType = MFT_STRING;
    itemInfo.fState = MFS_ENABLED;
    itemInfo.wID = ID_FILE_SAVE;
    itemInfo.dwTypeData = const_cast<LPWSTR>(L"Save As Image...");

    InsertMenuItem(context, GetMenuItemCount(context), TRUE, &itemInfo);
}

HRESULT CBitmapSourceElement::SaveAsImage(CImageTransencoder &trans, ICodeGenerator & /*codeGen*/)
{
    HRESULT result = S_OK;

    IFC(trans.AddFrame(m_source));

    return result;
}

HRESULT GetPixelFormatName(WCHAR *dest, UINT chars, WICPixelFormatGUID guid)
{
    HRESULT result;
    if(guid == GUID_WICPixelFormatDontCare)
    {
        wcscpy_s(dest, chars, L"Don't Care");
        return S_OK;
    }
    IWICComponentInfoPtr info;
    IFC(g_imagingFactory->CreateComponentInfo(guid, &info));
    IFC(info->GetFriendlyName(chars, dest, &chars));
    return S_OK;
}

HRESULT CBitmapSourceElement::OutputView(IOutputDevice &output, const InfoElementViewContext& context)
{
    HRESULT result;

    ATLASSERT(NULL != m_source);

    IFC(CInfoElement::OutputView(output,context));
    if (NULL != m_source)
    {
        // First, some info
        UINT width = 0, height = 0;
        IFC(m_source->GetSize(&width, &height));

        double dpiX = 0, dpiY = 0;
        IFC(m_source->GetResolution(&dpiX, &dpiY));

        WICPixelFormatGUID pixelFormat{};
        IFC(m_source->GetPixelFormat(&pixelFormat));

        output.BeginKeyValues(L"");

        WCHAR v[128];

        StringCchPrintfW(v, ARRAYSIZE(v), L"%u", width);
        output.AddKeyValue(L"Width", v);
        StringCchPrintfW(v, ARRAYSIZE(v), L"%u", height);
        output.AddKeyValue(L"Height", v);
        StringCchPrintfW(v, ARRAYSIZE(v), L"%g", dpiX);
        output.AddKeyValue(L"DpiX", v);
        StringCchPrintfW(v, ARRAYSIZE(v), L"%g", dpiY);
        output.AddKeyValue(L"DpiY", v);
        if(FAILED(GetPixelFormatName(v, ARRAYSIZE(v), pixelFormat)))
        {
            wcscpy_s(v, ARRAYSIZE(v), L"Unknown ");
        }
        wcscat_s(v, ARRAYSIZE(v), L" ");
        size_t len = wcslen(v);
        StringFromGUID2(pixelFormat, v + len, int(ARRAYSIZE(v) - len));
        output.AddKeyValue(L"Format", v);

        // Now, the bitmap itself
        CStopwatch renderTimer;
        renderTimer.Start();

        HGLOBAL hGlobal = nullptr;
        HGLOBAL hAlpha = nullptr;
        IWICBitmapSourcePtr source;

        if (m_colorTransform == NULL)
        {
            IWICBitmapFrameDecodePtr frame;
            IWICColorContextPtr colorContextSrc;
            IWICColorContextPtr colorContextDst;
            IWICColorTransformPtr colorTransform;
            WCHAR wzFilename[_MAX_PATH+1];
            UINT cActual = 0;

            result = m_source->QueryInterface(IID_PPV_ARGS(&frame));

            if (SUCCEEDED(result))
            {
                IWICColorContext **ppiContextSrc = &colorContextSrc;
                result = g_imagingFactory->CreateColorContext(ppiContextSrc);

                if (SUCCEEDED(result))
                {
                    result = frame->GetColorContexts(1, ppiContextSrc, &cActual);
                    CString value;
                    value.Format(L"%u", cActual);
                    output.AddKeyValue(L"Total ColorContexts", value);
                }

                if (SUCCEEDED(result) && cActual > 0)
                {
                    result = g_imagingFactory->CreateColorContext(&colorContextDst);

                    if (SUCCEEDED(result))
                    {
                        DWORD cbFilename = sizeof(wzFilename);

                        if (GetColorDirectoryW(nullptr, wzFilename, &cbFilename))
                        {
                            result = StringCchCatW(wzFilename,
                                                   sizeof(wzFilename)/sizeof(wzFilename[0]),
                                                   L"\\sRGB Color Space Profile.icm");
                        }
                        else
                        {
                            result = E_UNEXPECTED;
                        }

                        if (SUCCEEDED(result))
                        {
                            result = colorContextDst->InitializeFromFilename(wzFilename);
                        }
                    }

                    if (SUCCEEDED(result))
                    {
                        result = g_imagingFactory->CreateColorTransformer(&colorTransform);
                    }

                    if (SUCCEEDED(result))
                    {
                        result = colorTransform->Initialize(m_source,
                                                            colorContextSrc,
                                                            colorContextDst,
                                                            GUID_WICPixelFormat32bppBGRA);
                        if (SUCCEEDED(result))
                        {
                            m_colorTransform = colorTransform;
                            output.AddKeyValue(L"Output ColorContext", wzFilename);
                        }
                    }
                }

            }

        }

        if (m_colorTransform)
        {
            source = m_colorTransform;
        }
        else
        {
            source = m_source;
        }

        result = CreateDibFromBitmapSource(source, hGlobal, context.bIsAlphaEnable ? &hAlpha : nullptr);

        const DWORD renderTime = renderTimer.GetTimeMS();

        // Note how long it took to render
        StringCchPrintfW(v, 64, L"%u ms", renderTime);
        output.AddKeyValue(L"Time", v);

        output.EndKeyValues();

        // Output the bitmap
        if (SUCCEEDED(result))
        {
            output.AddText(L"RGB:\n");
            output.AddDib(hGlobal);
            if (hAlpha != nullptr)
            {
                output.AddText(L"Alpha:\n");
                output.AddDib(hAlpha);
            }
        }
        else
        {
            CString msg;
            CString err;

            GetHresultString(result, err);

            msg.Format(L"Failed to convert IWICBitmapSource to HBITMAP: %s", (LPCWSTR)err);
            COLORREF oldColor = output.SetTextColor(RGB(255, 0, 0));
            output.AddText(msg);
            output.SetTextColor(oldColor);
        }
    }
    else
    {
    }

    return result;
}

HRESULT CBitmapSourceElement::OutputInfo(IOutputDevice & /*output*/)
{
    return S_OK;
}

// Just hardcoded for now
bool HasAlpha (REFWICPixelFormatGUID pGuid)
{
    return IsEqualGUID(pGuid, GUID_WICPixelFormat32bppBGRA)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat32bppPBGRA)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat64bppRGBA)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat64bppPRGBA)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat128bppRGBAFloat)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat128bppPRGBAFloat)
        || IsEqualGUID(pGuid, GUID_WICPixelFormat128bppRGBAFixedPoint);
}

HRESULT CBitmapSourceElement::CreateDibFromBitmapSource(IWICBitmapSourcePtr source,
    HGLOBAL &hGlobal, HGLOBAL* phAlpha)
{
    HRESULT result = S_OK;
    if (NULL == source)
    {
        return E_INVALIDARG;
    }

    UINT width = 0, height = 0;
    UINT stride = 0;

    WICPixelFormatGUID pFormatGuid;
    IFC (source->GetPixelFormat(&pFormatGuid));

    const bool bAlphaEnabled = (phAlpha != nullptr) && HasAlpha (pFormatGuid);

    // Create a format converter
    IWICFormatConverterPtr formatConverter;
    IFC(g_imagingFactory->CreateFormatConverter(&formatConverter));

    // Init the format converter to output Bgra32
    IFC(formatConverter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom));

    // Create a FlipRotator because windows requires the bitmap to be bottom-up
    IWICBitmapFlipRotatorPtr flipper;
    IFC(g_imagingFactory->CreateBitmapFlipRotator(&flipper));

    // Init the format converter to output Bgra32
    IFC(flipper->Initialize(formatConverter, WICBitmapTransformFlipVertical));

    // Get the size
    IFC(flipper->GetSize(&width, &height));
    stride = width * 4;

    // Force the stride to be a multiple of sizeof(DWORD)
    stride = ((stride + sizeof(DWORD) - 1) / sizeof(DWORD)) * sizeof(DWORD);

    const SIZE_T dibSize = sizeof(BITMAPINFOHEADER) + stride*height;

    // Allocate the DIB bytes
    hGlobal = GlobalAlloc(GMEM_MOVEABLE, dibSize);
    ATLASSERT(hGlobal);

    if (nullptr == hGlobal)
    {
        return E_OUTOFMEMORY;
    }

    BYTE *dibBytes = static_cast<BYTE*>(GlobalLock(hGlobal));
    ATLASSERT(dibBytes);

    if (nullptr == dibBytes)
    {
        GlobalFree(hGlobal);
        return E_OUTOFMEMORY;
    }

    auto* bmih = reinterpret_cast<BITMAPINFOHEADER*>(dibBytes);
    BYTE *dibPixels = dibBytes + sizeof(BITMAPINFOHEADER);

    // Set the header
    ZeroMemory(bmih, sizeof(BITMAPINFOHEADER));
    bmih->biSize = sizeof(BITMAPINFOHEADER);
    bmih->biPlanes = 1;
    bmih->biBitCount = 32;
    bmih->biCompression = BI_RGB;
    bmih->biWidth = width;
    bmih->biHeight = height;
    bmih->biSizeImage = stride*height;

    // Copy the pixels
    WICRect rct;
    rct.X = 0;
    rct.Y = 0;
    rct.Width = width;
    rct.Height = height;

    result = flipper->CopyPixels(&rct, stride, stride*height, dibPixels);

    if (bAlphaEnabled)
    {
        *phAlpha = GlobalAlloc(GMEM_MOVEABLE, dibSize);
        ATLASSERT(NULL != *phAlpha);
        if (nullptr == hGlobal)
        {
        return E_OUTOFMEMORY;
        }

        BYTE *dibPixels2 = dibBytes + sizeof(BITMAPINFOHEADER);

        BYTE *dibAlphaBytes = static_cast<BYTE*>(GlobalLock(*phAlpha));

        ATLASSERT(dibAlphaBytes);
        if (nullptr == dibAlphaBytes)
        {
            GlobalFree(phAlpha);
            *phAlpha = nullptr;
            return E_OUTOFMEMORY;
        }

        BYTE *dibAlphaPixels = dibAlphaBytes + sizeof(BITMAPINFOHEADER);
        auto* bmih2 = reinterpret_cast<BITMAPINFOHEADER*>(dibAlphaBytes);

        // Set the header
        ZeroMemory(bmih2, sizeof(BITMAPINFOHEADER));
        bmih2->biSize = sizeof(BITMAPINFOHEADER);
        bmih2->biPlanes = 1;
        bmih2->biBitCount = 32;
        bmih2->biCompression = BI_RGB;
        bmih2->biWidth = width;
        bmih2->biHeight = height;
        bmih2->biSizeImage = stride*height;

        // Fill the dibpixels with alpha values:
        for (unsigned y = 0; y < height; y++)
        {
            for (unsigned x = 0; x < width; x++)
            {
                dibAlphaPixels[x*4+0] = dibPixels2[x*4+3];
                dibAlphaPixels[x*4+1] = dibPixels2[x*4+3];
                dibAlphaPixels[x*4+2] = dibPixels2[x*4+3];
                dibAlphaPixels[x*4+3] = dibPixels2[x*4+3];
            }
            dibAlphaPixels += stride;
            dibPixels2 += stride;
        }
        GlobalUnlock (*phAlpha);
        if (FAILED(result))
        {
            GlobalFree(*phAlpha);
        }
    }

    // Unlock the buffer
    GlobalUnlock(hGlobal);

    if (FAILED(result))
    {
        GlobalFree(hGlobal);
    }

    return result;
}


//----------------------------------------------------------------------------------------
// BITMAP FRAME DECODE ELEMENT
//----------------------------------------------------------------------------------------

void CBitmapFrameDecodeElement::FillContextMenu(HMENU context)
{
    CBitmapSourceElement::FillContextMenu(context);

    MENUITEMINFO itemInfo = { 0 };
    itemInfo.cbSize = sizeof(MENUITEMINFO);
    itemInfo.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_STRING;
    itemInfo.fType = MFT_STRING;
    itemInfo.fState = MFS_ENABLED;

    itemInfo.wID = ID_FIND_METADATA;
    itemInfo.dwTypeData = const_cast<LPWSTR>(L"Find metadata by Query Language");
    InsertMenuItem(context, GetMenuItemCount(context), TRUE, &itemInfo);
}

HRESULT CBitmapFrameDecodeElement::SaveAsImage(CImageTransencoder &trans, ICodeGenerator & /*codeGen*/)
{
    HRESULT result = S_OK;

    IFC(trans.AddFrame(m_frameDecode));

    return result;
}

HRESULT CBitmapFrameDecodeElement::OutputView(IOutputDevice &output, const InfoElementViewContext& context)
{
    HRESULT result = S_OK;

    ATLASSERT(NULL != m_frameDecode);

    if (NULL != m_frameDecode)
    {
        IFC(CBitmapSourceElement::OutputView(output, context));
    }

    return result;
}

HRESULT CBitmapFrameDecodeElement::OutputInfo(IOutputDevice & /*output*/)
{
    return S_OK;
}


//----------------------------------------------------------------------------------------
// METADATA READER ELEMENT
//----------------------------------------------------------------------------------------

CMetadataReaderElement::CMetadataReaderElement(CInfoElement *parent, UINT idx, IWICMetadataReaderPtr reader)
    : CComponentInfoElement(L"")
    , m_reader(reader)
{
    if (FAILED(SetNiceName(parent, idx)))
    {
        m_name = L"MetadataReader";
    }
}

HRESULT CMetadataReaderElement::SetNiceName(CInfoElement *parent, UINT idx)
{
    HRESULT result = S_OK;

    // Try to get a better name
    if (m_reader)
    {
        // First, get our FriendlyName
        CString t;
        IWICMetadataHandlerInfoPtr info;
        result = m_reader->GetMetadataHandlerInfo(&info);
        if (SUCCEEDED(result))
        {
            READ_WIC_STRING(info->GetFriendlyName, t);
        }

        // Next, try to get the name that our parent gave us. We can do this
        // only if our parent is a CMetadataReaderElement
        CString pn;
        auto* mre = dynamic_cast<CMetadataReaderElement*>(parent);
        if (mre)
        {
            PROPVARIANT id;
            PropVariantInit(&id);

            result = mre->m_reader->GetValueByIndex(idx, nullptr, &id, nullptr);
            if (SUCCEEDED(result))
            {
                IFC(TranslateValueID(&id, 0, pn));
                TrimQuotesFromName(pn);
            }

            PropVariantClear(&id);
        }

        // Merge them into a name
        if (pn.GetLength() > 0)
        {
            m_name = pn + L" (" + t + L")";
        }
        else
        {
            m_name = t;
        }
    }
    else
    {
        result = E_INVALIDARG;
    }

    return result;
}

HRESULT CMetadataReaderElement::TrimQuotesFromName(CString &out)
{
    out.TrimLeft(L'\"');
    out.TrimRight(L'\"');

    return S_OK;
}

HRESULT CMetadataReaderElement::OutputView(IOutputDevice &output, const InfoElementViewContext& context)
{
    HRESULT result = S_OK;

    ATLASSERT(NULL != m_reader);

    IFC(CComponentInfoElement::OutputView(output, context));

    if (NULL != m_reader)
    {
        output.BeginKeyValues(L"Metadata Values");

        UINT numValues = 0;
        IFC(m_reader->GetCount(&numValues));

        for (UINT i = 0; i < numValues; i++)
        {
            PROPVARIANT id, schema, value;

            PropVariantInit(&id);
            PropVariantInit(&schema);
            PropVariantInit(&value);

            IFC(m_reader->GetValueByIndex(i, &schema, &id, &value));

            CString k;
            CString v;

            IFC(TranslateValueID(&id, PVTSOPTION_IncludeType, k));
            IFC(PropVariantToString(&value, PVTSOPTION_IncludeType, v));

            if (schema.vt != VT_EMPTY)
            {
                CString s;
                IFC(PropVariantToString(&schema, PVTSOPTION_IncludeType, s));
                output.AddKeyValue(k + L" [" + s + L"]", v);
            }
            else
            {
                output.AddKeyValue(k, v);
            }

            PropVariantClear(&id);
            PropVariantClear(&schema);
            PropVariantClear(&value);
        }

        output.EndKeyValues();
    }

    return result;
}

HRESULT CMetadataReaderElement::OutputInfo(IOutputDevice &output)
{
    HRESULT result = S_OK;

    ATLASSERT(m_reader);

    if (m_reader)
    {
        IWICMetadataHandlerInfoPtr handlerInfo;

        IFC(m_reader->GetMetadataHandlerInfo(&handlerInfo));

        IFC(OutputMetadataHandlerInfo(output, handlerInfo));
    }

    return result;
}

HRESULT CMetadataReaderElement::TranslateValueID(PROPVARIANT *pv, unsigned options, CString &out)
{
    HRESULT result = S_OK;

    // Get our format
    GUID metadataFormat{};
    IWICMetadataHandlerInfoPtr handlerInfo;
    IFC(m_reader->GetMetadataHandlerInfo(&handlerInfo));
    IFC(handlerInfo->GetMetadataFormat(&metadataFormat));

    // Try using the translator
    result = CMetadataTranslator::Inst().Translate(metadataFormat, pv, out);

    // If that failed, use the string converter
    if (FAILED(result))
    {
        result = PropVariantToString(pv, options, out);
    }

    return result;
}

