#pragma once

#include "d3d11_device_child.h"

#include "../d3d10/d3d10_input_layout.h"

#include <string>

namespace dxvk {
  
  class D3D11Device;
  
  // NV-DXVK start: Preserve original D3D11 semantic names alongside Vulkan
  // vertex attributes so the RTX capture path can resolve attributes by
  // semantic instead of guessing from vertex format.
  struct D3D11SemanticInfo {
    std::string name;
    uint32_t index = 0;
  };
  // NV-DXVK end
  
  class D3D11InputLayout : public D3D11DeviceChild<ID3D11InputLayout> {
    
  public:
    
    D3D11InputLayout(
            D3D11Device*          pDevice,
            uint32_t              numAttributes,
      const DxvkVertexAttribute*  pAttributes,
            uint32_t              numBindings,
      const DxvkVertexBinding*    pBindings,
      const D3D11SemanticInfo*    pSemantics = nullptr,
            uint32_t              numSemantics = 0);
    
    ~D3D11InputLayout();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                riid,
            void**                ppvObject) final;
    
    void BindToContext(
      const Rc<DxvkContext>&      ctx);
    
    bool Compare(
      const D3D11InputLayout*     pOther) const;

    const std::vector<DxvkVertexAttribute>& GetAttributes() const {
      return m_attributes;
    }

    const std::vector<DxvkVertexBinding>& GetBindings() const {
      return m_bindings;
    }

    // NV-DXVK start: Expose semantic info for RTX attribute resolution
    const std::vector<D3D11SemanticInfo>& GetSemantics() const {
      return m_semantics;
    }
    // NV-DXVK end
    
    D3D10InputLayout* GetD3D10Iface() {
      return &m_d3d10;
    }
    
  private:
    
    std::vector<DxvkVertexAttribute> m_attributes;
    std::vector<DxvkVertexBinding>   m_bindings;
    // NV-DXVK start: Semantic name storage
    std::vector<D3D11SemanticInfo>   m_semantics;
    // NV-DXVK end

    D3D10InputLayout m_d3d10;
    
  };
  
}
