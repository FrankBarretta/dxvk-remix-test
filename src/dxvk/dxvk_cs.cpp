#include "dxvk_cs.h"
#include "dxvk_scoped_annotation.h"

#include <dbghelp.h>
#include <mutex>
#include <typeinfo>

#include "../d3d11/d3d11_trace.h"

// NV-DXVK start: notify user and kill process on exception in CS thread to avoid silent hangs
#include "rtx_render/rtx_env.h"
// NV-DXVK end

#include "../tracy/TracyC.h"

#pragma comment(lib, "dbghelp.lib")

namespace dxvk {

  namespace {
    bool IsDx11CsTraceEnabled() {
      static const bool enabled = env::getEnvVar("DXVK_D3D11_CS_TRACE") == "1";
      return enabled;
    }

    std::mutex g_csSymbolMutex;

    bool EnsureSymbolHandlerInitialized() {
      static bool initialized = false;
      static bool success = false;

      if (!initialized) {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        success = SymInitialize(process, nullptr, TRUE) == TRUE;
        initialized = true;
      }

      return success;
    }

    void FormatCallsite(char* dst, size_t dstSize, const void* callsite) {
      if (dstSize == 0) {
        return;
      }

      if (callsite == nullptr) {
        std::snprintf(dst, dstSize, "callsite=null");
        return;
      }

      DWORD64 address = reinterpret_cast<DWORD64>(callsite);
      HMODULE module = nullptr;
      char moduleName[MAX_PATH] = "unknown";

      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(callsite), &module) && module != nullptr) {
        GetModuleFileNameA(module, moduleName, MAX_PATH);
      }

      DWORD64 displacement = 0;
      DWORD lineDisplacement = 0;
      char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
      auto symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = MAX_SYM_NAME;

      IMAGEHLP_LINE64 lineInfo = {};
      lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

      std::lock_guard<std::mutex> lock(g_csSymbolMutex);

      if (!EnsureSymbolHandlerInitialized()) {
        std::snprintf(dst, dstSize, "callsite=%p module=%s", callsite, moduleName);
        return;
      }

      const bool hasSymbol = SymFromAddr(GetCurrentProcess(), address, &displacement, symbol) == TRUE;
      const bool hasLine = SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &lineInfo) == TRUE;

      if (hasSymbol && hasLine) {
        std::snprintf(dst,
          dstSize,
          "callsite=%p symbol=%s+0x%llx file=%s:%lu",
          callsite,
          symbol->Name,
          static_cast<unsigned long long>(displacement),
          lineInfo.FileName,
          lineInfo.LineNumber);
      } else if (hasSymbol) {
        std::snprintf(dst,
          dstSize,
          "callsite=%p symbol=%s+0x%llx module=%s",
          callsite,
          symbol->Name,
          static_cast<unsigned long long>(displacement),
          moduleName);
      } else {
        std::snprintf(dst, dstSize, "callsite=%p module=%s", callsite, moduleName);
      }
    }

    const char* GetCsCmdTypeName(const DxvkCsCmd* cmd) {
      if (cmd == nullptr) {
        return "null";
      }

      return typeid(*cmd).name();
    }

    const void* GetCsCmdVtable(const DxvkCsCmd* cmd) {
      if (cmd == nullptr) {
        return nullptr;
      }

      return *reinterpret_cast<const void* const*>(cmd);
    }

    void TraceCsChunkState(const char* phase, const DxvkCsChunk* chunk, const DxvkCsCmd* cmd, const DxvkCsCmd* next, uint32_t index) {
      if (!IsDx11CsTraceEnabled()) {
        return;
      }

      char callsite[512];
      FormatCallsite(callsite, sizeof(callsite), cmd != nullptr ? cmd->debugCallsite() : nullptr);

      char message[1024];
      std::snprintf(message,
        sizeof(message),
        "DxvkCsChunk::executeAll %s chunk=%p cmd=%p next=%p index=%u type=%s vtbl=%p %s",
        phase,
        chunk,
        cmd,
        next,
        index,
        GetCsCmdTypeName(cmd),
        GetCsCmdVtable(cmd),
        callsite);
      D3D11EarlyTrace(message);
    }

  }
  
  DxvkCsChunk::DxvkCsChunk() {
    
  }
  
  
  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }
  
  
  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(DxvkContext* ctx) {
    ScopedCpuProfileZone();
    auto cmd = m_head;
    uint32_t cmdIndex = 0;

    TraceCsChunkState("begin", this, m_head, m_tail, 0u);
    
    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      m_commandOffset = 0;
      
      while (cmd != nullptr) {
        TraceCsChunkState("before-next", this, cmd, nullptr, cmdIndex);
        auto next = cmd->next();
        TraceCsChunkState("before-exec", this, cmd, next, cmdIndex);
        cmd->exec(ctx);
        TraceCsChunkState("after-exec", this, cmd, next, cmdIndex);
        cmd->~DxvkCsCmd();
        TraceCsChunkState("after-dtor", this, cmd, next, cmdIndex);
        cmd = next;
        cmdIndex += 1;
      }

      m_head = nullptr;
      m_tail = nullptr;
    } else {
      while (cmd != nullptr) {
        TraceCsChunkState("before-next", this, cmd, nullptr, cmdIndex);
        auto next = cmd->next();
        TraceCsChunkState("before-exec", this, cmd, next, cmdIndex);
        cmd->exec(ctx);
        TraceCsChunkState("after-exec", this, cmd, next, cmdIndex);
        cmd = next;
        cmdIndex += 1;
      }
    }

    TraceCsChunkState("complete", this, m_head, m_tail, cmdIndex);
  }
  
  
  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }
    
    m_head = nullptr;
    m_tail = nullptr;

    m_commandOffset = 0;
  }
  
  
  DxvkCsChunkPool::DxvkCsChunkPool() {
    
  }
  
  
  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (DxvkCsChunk* chunk : m_chunks)
      delete chunk;
  }
  
  
  DxvkCsChunk* DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    ScopedCpuProfileZone();
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<sync::Spinlock> lock(m_mutex);
      
      if (m_chunks.size() != 0) {
        chunk = m_chunks.back();
        m_chunks.pop_back();
      }
    }
    
    if (!chunk)
      chunk = new DxvkCsChunk();
    
    chunk->init(flags);
    return chunk;
  }
  
  
  void DxvkCsChunkPool::freeChunk(DxvkCsChunk* chunk) {
    ScopedCpuProfileZone();
    chunk->reset();
    
    std::lock_guard<sync::Spinlock> lock(m_mutex);
    m_chunks.push_back(chunk);
  }
  
  
  DxvkCsThread::DxvkCsThread(
    const Rc<DxvkDevice>&   device,
    const Rc<DxvkContext>&  context)
  : m_device(device), m_context(context),
    m_thread([this] { threadFunc(); }) {
    
  }
  
  
  DxvkCsThread::~DxvkCsThread() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_condOnAdd.notify_one();
    m_thread.join();
  }
  
  
  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    ScopedCpuProfileZone();

    uint64_t seq;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      seq = ++m_chunksDispatched;
      m_chunksQueued.push(std::move(chunk));
    }
    
    m_condOnAdd.notify_one();
    return seq;
  }
  
  
  void DxvkCsThread::synchronize(uint64_t seq) {
    ScopedCpuProfileZone();

    // Avoid locking if we know the sync is a no-op, may
    // reduce overhead if this is being called frequently
    if (seq > m_chunksExecuted.load(std::memory_order_acquire)) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (seq == SynchronizeAll)
        seq = m_chunksDispatched.load();

      auto t0 = dxvk::high_resolution_clock::now();
      m_condOnSync.wait(lock, [this, seq] {
        return m_chunksExecuted.load() >= seq;
      });
      auto t1 = dxvk::high_resolution_clock::now();
      auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

      m_device->addStatCtr(DxvkStatCounter::CsSyncCount, 1);
      m_device->addStatCtr(DxvkStatCounter::CsSyncTicks, ticks.count());
    }
  }
  
  
  void DxvkCsThread::threadFunc() {
    ScopedCpuProfileZone();

    env::setThreadName("dxvk-cs");

    DxvkCsChunkRef chunk;

    try {
      while (!m_stopped.load()) {
        { 
          ScopedCpuProfileZoneN("waiting for work");
          std::unique_lock<dxvk::mutex> lock(m_mutex);
          if (chunk) {
            m_chunksExecuted++;
            m_condOnSync.notify_one();
            
            chunk = DxvkCsChunkRef();
          }
          
          if (m_chunksQueued.size() == 0) {
            m_condOnAdd.wait(lock, [this] {
              return (m_chunksQueued.size() != 0)
                  || (m_stopped.load());
            });
          }
          
          if (m_chunksQueued.size() != 0) {
            chunk = std::move(m_chunksQueued.front());
            m_chunksQueued.pop();
          }
        }
        
        if (chunk) {
          m_context->addStatCtr(DxvkStatCounter::CsChunkCount, 1);
          chunk->executeAll(m_context.ptr());
        }
      }
    } catch (const DxvkError& e) {
      Logger::err("Exception on CS thread!");
      Logger::err(e.message());

      // NV-DXVK start: notify user and kill process on exception in CS thread to avoid silent hangs
      char buf[2048];
      snprintf(buf, sizeof(buf), "Exception on CS thread: %s. The game will exit now.", e.message().c_str());
      messageBox(buf, "RTX Remix", MB_OK);
      exit(1);
      // NV-DXVK end
    }
  }
  
}