// SPDX-FileCopyrightText: 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

#include "d3d11_texture.h"
#include "common/assert.h"
#include "common/log.h"
#include "common/string_util.h"
#include "d3d11_device.h"
#include <array>
Log_SetChannel(D3D11);

static constexpr std::array<DXGI_FORMAT, static_cast<u32>(GPUTexture::Format::Count)> s_dxgi_mapping = {
  {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B5G6R5_UNORM,
   DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_D16_UNORM}};

D3D11Texture::D3D11Texture() = default;

D3D11Texture::D3D11Texture(ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> srv,
                           ComPtr<ID3D11View> rtv)
  : m_texture(std::move(texture)), m_srv(std::move(srv)), m_rtv_dsv(std::move(rtv))
{
  const D3D11_TEXTURE2D_DESC desc = GetDesc();
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.ArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_format = LookupBaseFormat(desc.Format);
  m_dynamic = (desc.Usage == D3D11_USAGE_DYNAMIC);
}

D3D11Texture::~D3D11Texture()
{
  Destroy();
}

DXGI_FORMAT D3D11Texture::GetDXGIFormat(Format format)
{
  return s_dxgi_mapping[static_cast<u8>(format)];
}

GPUTexture::Format D3D11Texture::LookupBaseFormat(DXGI_FORMAT dformat)
{
  for (u32 i = 0; i < static_cast<u32>(s_dxgi_mapping.size()); i++)
  {
    if (s_dxgi_mapping[i] == dformat)
      return static_cast<Format>(i);
  }
  return GPUTexture::Format::Unknown;
}

D3D11_TEXTURE2D_DESC D3D11Texture::GetDesc() const
{
  D3D11_TEXTURE2D_DESC desc;
  m_texture->GetDesc(&desc);
  return desc;
}

bool D3D11Texture::IsValid() const
{
  return static_cast<bool>(m_texture);
}

bool D3D11Texture::Update(u32 x, u32 y, u32 width, u32 height, const void* data, u32 pitch, u32 layer /*= 0*/,
                          u32 level /*= 0*/)
{
  if (m_dynamic)
  {
    void* map;
    u32 map_stride;
    if (!Map(&map, &map_stride, x, y, width, height, layer, level))
      return false;

    StringUtil::StrideMemCpy(map, map_stride, data, pitch, GetPixelSize() * width, height);
    Unmap();
    return true;
  }

  const CD3D11_BOX box(static_cast<LONG>(x), static_cast<LONG>(y), 0, static_cast<LONG>(x + width),
                       static_cast<LONG>(y + height), 1);
  const u32 srnum = D3D11CalcSubresource(level, layer, m_levels);

  D3D11Device::GetD3DContext()->UpdateSubresource(m_texture.Get(), srnum, &box, data, pitch, 0);
  return true;
}

bool D3D11Texture::Map(void** map, u32* map_stride, u32 x, u32 y, u32 width, u32 height, u32 layer /*= 0*/,
                       u32 level /*= 0*/)
{
  if (!m_dynamic || (x + width) > m_width || (y + height) > m_height || layer > m_layers || level > m_levels)
    return false;

  const bool discard = (width == m_width && height == m_height);
  const u32 srnum = D3D11CalcSubresource(level, layer, m_levels);
  D3D11_MAPPED_SUBRESOURCE sr;
  HRESULT hr = D3D11Device::GetD3DContext()->Map(m_texture.Get(), srnum,
                                                 discard ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE, 0, &sr);
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Map pixels texture failed: %08X", hr);
    return false;
  }

  *map = static_cast<u8*>(sr.pData) + (y * sr.RowPitch) + (x * GetPixelSize());
  *map_stride = sr.RowPitch;
  m_mapped_subresource = srnum;
  return true;
}

void D3D11Texture::Unmap()
{
  D3D11Device::GetD3DContext()->Unmap(m_texture.Get(), m_mapped_subresource);
  m_mapped_subresource = 0;
}

bool D3D11Texture::Create(ID3D11Device* device, u32 width, u32 height, u32 layers, u32 levels, u32 samples, Type type,
                          Format format, const void* initial_data /* = nullptr */, u32 initial_data_stride /* = 0 */,
                          bool dynamic /* = false */)
{
  if (width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
      layers > D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION || (layers > 1 && samples > 1))
  {
    Log_ErrorPrintf("Texture bounds (%ux%ux%u, %u mips, %u samples) are too large", width, height, layers, levels,
                    samples);
    return false;
  }

  u32 bind_flags = 0;
  switch (type)
  {
    case Type::RenderTarget:
      bind_flags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::DepthStencil:
      bind_flags = D3D11_BIND_DEPTH_STENCIL; // | D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::Texture:
      bind_flags = D3D11_BIND_SHADER_RESOURCE;
      break;
    case Type::RWTexture:
      bind_flags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
      break;
    default:
      break;
  }

  CD3D11_TEXTURE2D_DESC desc(GetDXGIFormat(format), width, height, layers, levels, bind_flags,
                             dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, dynamic ? D3D11_CPU_ACCESS_WRITE : 0,
                             samples, 0, 0);

  D3D11_SUBRESOURCE_DATA srd;
  srd.pSysMem = initial_data;
  srd.SysMemPitch = initial_data_stride;
  srd.SysMemSlicePitch = initial_data_stride * height;

  ComPtr<ID3D11Texture2D> texture;
  const HRESULT tex_hr = device->CreateTexture2D(&desc, initial_data ? &srd : nullptr, texture.GetAddressOf());
  if (FAILED(tex_hr))
  {
    Log_ErrorPrintf(
      "Create texture failed: 0x%08X (%ux%u levels:%u samples:%u format:%u bind_flags:%X initial_data:%p)", tex_hr,
      width, height, levels, samples, static_cast<unsigned>(format), bind_flags, initial_data);
    return false;
  }

  ComPtr<ID3D11ShaderResourceView> srv;
  if (bind_flags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ?
        D3D11_SRV_DIMENSION_TEXTURE2DMS :
        (desc.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D);
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, desc.Format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for texture failed: 0x%08X", hr);
      return false;
    }
  }

  ComPtr<ID3D11View> rtv_dsv;
  if (bind_flags & D3D11_BIND_RENDER_TARGET)
  {
    const D3D11_RTV_DIMENSION rtv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(rtv_dimension, desc.Format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, rtv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create RTV for texture failed: 0x%08X", hr);
      return false;
    }

    rtv_dsv = std::move(rtv);
  }
  else if (bind_flags & D3D11_BIND_DEPTH_STENCIL)
  {
    const D3D11_DSV_DIMENSION dsv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    const CD3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc(dsv_dimension, desc.Format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11DepthStencilView> dsv;
    const HRESULT hr = device->CreateDepthStencilView(texture.Get(), &dsv_desc, dsv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create DSV for texture failed: 0x%08X", hr);
      return false;
    }

    rtv_dsv = std::move(dsv);
  }

  m_texture = std::move(texture);
  m_srv = std::move(srv);
  m_rtv_dsv = std::move(rtv_dsv);
  m_width = static_cast<u16>(width);
  m_height = static_cast<u16>(height);
  m_layers = static_cast<u8>(layers);
  m_levels = static_cast<u8>(levels);
  m_samples = static_cast<u8>(samples);
  m_format = format;
  m_dynamic = dynamic;
  return true;
}

bool D3D11Texture::Adopt(ID3D11Device* device, ComPtr<ID3D11Texture2D> texture)
{
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  ComPtr<ID3D11ShaderResourceView> srv;
  if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
  {
    const D3D11_SRV_DIMENSION srv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(srv_dimension, desc.Format, 0, desc.MipLevels, 0, desc.ArraySize);
    const HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, srv.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create SRV for adopted texture failed: 0x%08X", hr);
      return false;
    }
  }

  ComPtr<ID3D11View> rtv_dsv;
  if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
  {
    const D3D11_RTV_DIMENSION rtv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    const CD3D11_RENDER_TARGET_VIEW_DESC rtv_desc(rtv_dimension, desc.Format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11RenderTargetView> rtv;
    const HRESULT hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, rtv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create RTV for adopted texture failed: 0x%08X", hr);
      return false;
    }

    rtv_dsv = std::move(rtv);
  }
  else if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
  {
    const D3D11_DSV_DIMENSION dsv_dimension =
      (desc.SampleDesc.Count > 1) ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    const CD3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc(dsv_dimension, desc.Format, 0, 0, desc.ArraySize);
    ComPtr<ID3D11DepthStencilView> dsv;
    const HRESULT hr = device->CreateDepthStencilView(texture.Get(), &dsv_desc, dsv.GetAddressOf());
    if (FAILED(hr))
    {
      Log_ErrorPrintf("Create DSV for adopted texture failed: 0x%08X", hr);
      return false;
    }

    rtv_dsv = std::move(dsv);
  }

  m_texture = std::move(texture);
  m_srv = std::move(srv);
  m_rtv_dsv = std::move(rtv_dsv);
  m_width = static_cast<u16>(desc.Width);
  m_height = static_cast<u16>(desc.Height);
  m_layers = static_cast<u8>(desc.ArraySize);
  m_levels = static_cast<u8>(desc.MipLevels);
  m_samples = static_cast<u8>(desc.SampleDesc.Count);
  m_dynamic = (desc.Usage == D3D11_USAGE_DYNAMIC);
  return true;
}

void D3D11Texture::Destroy()
{
  m_rtv_dsv.Reset();
  m_srv.Reset();
  m_texture.Reset();
  m_dynamic = false;
  ClearBaseProperties();
}
