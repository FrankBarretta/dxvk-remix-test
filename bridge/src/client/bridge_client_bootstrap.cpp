/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "bridge_client_bootstrap.h"

#include "version.h"

#include "client_options.h"
#include "config/config.h"
#include "config/global_options.h"
#include "log/log.h"

#include "util_bridgecommand.h"
#include "util_common.h"
#include "util_devicecommand.h"
#include "util_filesys.h"
#include "util_modulecommand.h"
#include "util_process.h"
#include "util_seh.h"
#include "util_semaphore.h"

#include <chrono>
#include <mutex>
#include <sstream>
#include <string>

using namespace bridge_util;

bridge_util::Guid gUniqueIdentifier;
bool gbBridgeRunning = true;

namespace bridge::client {

  namespace {

    bool s_isAttached = false;
    bool s_attachAttempted = false;
    HMODULE s_bootstrapModule = nullptr;
    Process* s_server = nullptr;
    NamedSemaphore* s_present = nullptr;
    std::mutex s_attachMutex;
    std::mutex s_serverStartMutex;
    std::string s_remixFolder;
    std::chrono::steady_clock::time_point s_timeStart;

    void printRecentCommandHistory() {
      Logger::info("Most recent Device Queue commands sent from Client");
      DeviceBridge::Command::print_writer_data_sent();
      Logger::info("Most recent Device Queue commands received by Server");
      DeviceBridge::Command::print_writer_data_received();
      Logger::info("Most recent Module Queue commands sent from Client");
      ModuleBridge::Command::print_writer_data_sent();
      Logger::info("Most recent Module Queue commands received by Server");
      ModuleBridge::Command::print_writer_data_received();
    }

    void setupExceptionHandler() {
      if (ClientOptions::getSetExceptionHandler()) {
        ExceptionHandler::get().init();
      }
    }

    bool initRemixFolder(HMODULE module) {
      if (!s_remixFolder.empty()) {
        return true;
      }

      DWORD len;
      std::string tmp(MAX_PATH, '\0');

      do {
        len = GetModuleFileNameA(module, tmp.data(), tmp.length());

        if (len > 0 && len < tmp.length()) {
          break;
        }

        tmp.resize(tmp.length() * 2);
      } while (true);

      while (len) {
        if (tmp[len] == '\\' || tmp[len] == '/') {
          tmp.resize(len + 1);
          break;
        }
        --len;
      }

      s_remixFolder = tmp;
      return true;
    }

    bool initFileSys(const HMODULE module) {
      const auto moduleFilePath = getModuleFilePath(module);
      const bool needExecutablePath = moduleFilePath.extension().compare(".exe") != 0;
      fspath executablePath = "";

      if (needExecutablePath) {
        auto executablePathVec = createPathVec();

        if (GetModuleFileName(nullptr, executablePathVec.data(), executablePathVec.size()) == 0) {
          Logger::err("Failed to find executable path!");
          return false;
        }

        executablePath = fspath(executablePathVec.data());
      }

      const auto exeDir = executablePath.parent_path();
      dxvk::util::RtxFileSys::init(exeDir.string());
      return true;
    }

    void onServerExited(Process const*) {
      gbBridgeRunning = false;

      const auto timeServerEnd = std::chrono::high_resolution_clock::now();
      std::stringstream uptimeStream;
      uptimeStream << "[Uptime] Server (estimated): ";
      uptimeStream << std::chrono::duration_cast<std::chrono::seconds>(timeServerEnd - s_timeStart).count();
      uptimeStream << "s";
      Logger::info(uptimeStream.str());
    }

    void initServer(const char* apiName) {
      std::lock_guard<std::mutex> guard(s_serverStartMutex);

      if (s_server != nullptr) {
        return;
      }

      Logger::info("Launching server with GUID " + gUniqueIdentifier.toString());

      std::stringstream commandStream;
      commandStream << s_remixFolder;
      commandStream << ".trex/NvRemixBridge.exe";
      commandStream << " " << gUniqueIdentifier.toString();
      commandStream << " " << BRIDGE_VERSION;
      commandStream << " -bridge-api " << apiName;
      commandStream << " " << std::string(GetCommandLineA());

      s_server = new Process(commandStream.str().c_str(), onServerExited);

      if (ClientOptions::getEnableDpiAwareness()) {
        static HINSTANCE shcoreDll = ::LoadLibraryA("shcore.dll");

        if (shcoreDll != nullptr) {
          typedef HRESULT(WINAPI* PFN_SetProcessDpiAwareness)(int);

          if (const auto setProcessDpiAwarenessFn = reinterpret_cast<PFN_SetProcessDpiAwareness>(::GetProcAddress(shcoreDll, "SetProcessDpiAwareness"))) {
            constexpr int kProcessPerMonitorDpiAware = 2;
            setProcessDpiAwarenessFn(kProcessPerMonitorDpiAware);
          } else {
            SetProcessDPIAware();
          }
        }
      }

      Logger::info("Sending SYN command, waiting for ACK from server...");
      ClientMessage{ Commands::Bridge_Syn, reinterpret_cast<uintptr_t>(s_server->GetCurrentProcessHandle()) };

      const auto waitForAckResult = DeviceBridge::waitForCommand(Commands::Bridge_Ack, GlobalOptions::getStartupTimeout());
      switch (waitForAckResult) {
        case Result::Timeout:
          Logger::err("Timeout. Connection not established to server.");
          gbBridgeRunning = false;
          return;

        case Result::Failure:
          Logger::err("Failed to connect to server.");
          gbBridgeRunning = false;
          return;

        default:
          break;
      }

      DeviceBridge::pop_front();
      Logger::info("Ack received! Handshake completed! Telling server to continue waiting for commands...");
      ClientMessage{ Commands::Bridge_Continue };
    }

  }

  void setBootstrapModule(HMODULE module) {
    s_bootstrapModule = module;
  }

  bool ensureBridgeAttached(const char* apiName) {
    std::lock_guard<std::mutex> guard(s_attachMutex);

    if (s_isAttached) {
      return true;
    }

    if (s_attachAttempted) {
      return false;
    }

    s_attachAttempted = true;
    gbBridgeRunning = true;
    s_timeStart = std::chrono::steady_clock::now();

    HMODULE module = s_bootstrapModule;
    if (module == nullptr) {
      if (!GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, nullptr, &module)) {
        return false;
      }
    }

    Config::init(Config::App::Client, module);
    Config::setOption("bridge.api", apiName);
    GlobalOptions::init();

    if (!initFileSys(module)) {
      Logger::err("Failed to initialize rtx filesystem!");
    }

    Logger::init();

    if (!initRemixFolder(module)) {
      Logger::err("Fatal: Unable to initialize Remix folder...");
      return false;
    }

    setupExceptionHandler();

    Logger::info("==================\nNVIDIA RTX Remix Bridge Client Bootstrap\n==================");
    Logger::info(std::string("Version: ") + std::string(BRIDGE_VERSION));
    Logger::info(bridge_util::format_string("Bootstrapping %s x86 bridge path", apiName));

    initModuleBridge();
    initDeviceBridge();

    s_present = new NamedSemaphore("Present", 0, GlobalOptions::getPresentSemaphoreMaxFrames());

    initServer(apiName);

    if (!gbBridgeRunning) {
      return false;
    }

    s_isAttached = true;
    return true;
  }

  void detachBridge() {
    std::lock_guard<std::mutex> guard(s_attachMutex);

    if (!s_isAttached) {
      return;
    }

    Logger::info("About to unload bridge client bootstrap.");

    if (s_server != nullptr) {
      Logger::info("Sending Terminate command to server...");
      s_server->UnregisterExitCallback();
      ClientMessage{ Commands::Bridge_Terminate };

      const auto result = DeviceBridge::waitForCommandAndDiscard(Commands::Bridge_Ack, GlobalOptions::getCommandTimeout());
      if (RESULT_SUCCESS(result)) {
        Logger::info("Server notified that it has cleanly terminated. Cleaning up.");
      } else {
        Logger::err("Timeout waiting for clean server termination. Moving ahead anyway.");
      }

      delete s_server;
      s_server = nullptr;
    }

    printRecentCommandHistory();

    delete s_present;
    s_present = nullptr;

    s_isAttached = false;
    gbBridgeRunning = false;
  }

}