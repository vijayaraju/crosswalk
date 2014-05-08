// Copyright (c) 2013 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/xwalk_browser_main_parts_android.h"

#include "base/android/path_utils.h"
#include "base/base_paths_android.h"
#include "base/files/file_path.h"
#include "base/file_util.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "base/threading/sequenced_worker_pool.h"
#include "cc/base/switches.h"
#include "content/public/browser/android/compositor.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/cookie_store_factory.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "grit/net_resources.h"
#include "net/android/network_change_notifier_factory_android.h"
#include "net/base/network_change_notifier.h"
#include "net/base/net_module.h"
#include "net/base/net_util.h"
#include "net/cookies/cookie_monster.h"
#include "net/cookies/cookie_store.h"
#include "ui/base/layout.h"
#include "ui/base/l10n/l10n_util_android.h"
#include "ui/base/resource/resource_bundle.h"
#include "xwalk/extensions/browser/xwalk_extension_service.h"
#include "xwalk/extensions/common/xwalk_extension.h"
#include "xwalk/extensions/common/xwalk_extension_switches.h"
#include "xwalk/runtime/browser/android/cookie_manager.h"
#include "xwalk/runtime/browser/runtime_context.h"
#include "xwalk/runtime/browser/xwalk_runner.h"
#include "xwalk/runtime/common/xwalk_runtime_features.h"

namespace {

base::StringPiece PlatformResourceProvider(int key) {
  if (key == IDR_DIR_HEADER_HTML) {
    base::StringPiece html_data =
        ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
            IDR_DIR_HEADER_HTML);
    return html_data;
  }
  return base::StringPiece();
}

}  // namespace

namespace xwalk {

using content::BrowserThread;
using extensions::XWalkExtension;

XWalkBrowserMainPartsAndroid::XWalkBrowserMainPartsAndroid(
    const content::MainFunctionParams& parameters)
    : XWalkBrowserMainParts(parameters) {
}

XWalkBrowserMainPartsAndroid::~XWalkBrowserMainPartsAndroid() {
}

void XWalkBrowserMainPartsAndroid::PreEarlyInitialization() {
  net::NetworkChangeNotifier::SetFactory(
      new net::NetworkChangeNotifierFactoryAndroid());

  CommandLine::ForCurrentProcess()->AppendSwitch(
      cc::switches::kCompositeToMailbox);

  // Initialize the Compositor.
  content::Compositor::Initialize();

  XWalkBrowserMainParts::PreEarlyInitialization();
}

void XWalkBrowserMainPartsAndroid::PreMainMessageLoopStart() {
  CommandLine* command_line = CommandLine::ForCurrentProcess();
  // Disable ExtensionProcess for Android.
  // External extensions will run in the BrowserProcess (in process mode).
  command_line->AppendSwitch(switches::kXWalkDisableExtensionProcess);

  // Only force to enable WebGL for Android for IA platforms because
  // we've tested the WebGL conformance test. For other platforms, just
  // follow up the behavior defined by Chromium upstream.
#if defined(ARCH_CPU_X86)
  command_line->AppendSwitch(switches::kIgnoreGpuBlacklist);
#endif

  // Disable HW encoding acceleration for WebRTC on Android.
  // FIXME: Remove this switch for Android when Android OS is removed from
  // GPU accelerated_video_decode blacklist or we stop ignoring the GPU
  // blacklist.
  command_line->AppendSwitch(switches::kDisableWebRtcHWEncoding);

  // WebAudio is disabled on android x86 platform, and only enabled on android
  // ARM platform by default, we must enable it explicitly on x86 platform.
  // TODO(liyin): Remove enable webaudio switch when it is enabled by default.
#if defined(ARCH_CPU_X86)
  if (!command_line->HasSwitch(switches::kEnableWebAudio))
    command_line->AppendSwitch(switches::kEnableWebAudio);
#endif

  // For fullscreen video playback, the ContentVideoView is still buggy, so
  // we switch back to ContentVideoViewLegacy for temp.
  // TODO(shouqun): Remove this flag when ContentVideoView is ready.
  command_line->AppendSwitch(
      switches::kDisableOverlayFullscreenVideoSubtitle);

  XWalkBrowserMainParts::PreMainMessageLoopStart();

  startup_url_ = GURL();
}

void XWalkBrowserMainPartsAndroid::PostMainMessageLoopStart() {
  base::MessageLoopForUI::current()->Start();
}

void XWalkBrowserMainPartsAndroid::PreMainMessageLoopRun() {
  net::NetModule::SetResourceProvider(PlatformResourceProvider);
  if (parameters_.ui_task) {
    parameters_.ui_task->Run();
    delete parameters_.ui_task;
    run_default_message_loop_ = false;
  }

  xwalk_runner_->PreMainMessageLoopRun();

  extension_service_ = xwalk_runner_->extension_service();

  // Prepare the cookie store.
  base::FilePath user_data_dir;
  if (!PathService::Get(base::DIR_ANDROID_APP_DATA, &user_data_dir)) {
    NOTREACHED() << "Failed to get app data directory for Crosswalk";
  }

  base::FilePath cookie_store_path = user_data_dir.Append(
      FILE_PATH_LITERAL("Cookies"));
  scoped_refptr<base::SequencedTaskRunner> background_task_runner =
      BrowserThread::GetBlockingPool()->GetSequencedTaskRunner(
          BrowserThread::GetBlockingPool()->GetSequenceToken());

  content::CookieStoreConfig cookie_config(
      cookie_store_path,
      content::CookieStoreConfig::RESTORED_SESSION_COOKIES,
      NULL, NULL);
  cookie_config.client_task_runner =
      BrowserThread::GetMessageLoopProxyForThread(BrowserThread::IO);
  cookie_config.background_task_runner = background_task_runner;
  cookie_store_ = content::CreateCookieStore(cookie_config);
  cookie_store_->GetCookieMonster()->SetPersistSessionCookies(true);
  SetCookieMonsterOnNetworkStackInit(cookie_store_->GetCookieMonster());
}

void XWalkBrowserMainPartsAndroid::PostMainMessageLoopRun() {
  XWalkBrowserMainParts::PostMainMessageLoopRun();

  base::MessageLoopForUI::current()->Start();
}

void XWalkBrowserMainPartsAndroid::CreateInternalExtensionsForExtensionThread(
    content::RenderProcessHost* host,
    extensions::XWalkExtensionVector* extensions) {
  // On Android part, the ownership of each extension object will be transferred
  // to XWalkExtensionServer after this method is called. It is a rule enforced
  // by extension system that XWalkExtensionServer must own the extension
  // objects and extension instances.
  extensions::XWalkExtensionVector::const_iterator it = extensions_.begin();
  for (; it != extensions_.end(); ++it)
    extensions->push_back(*it);
}

void XWalkBrowserMainPartsAndroid::RegisterExtension(
    scoped_ptr<XWalkExtension> extension) {
  // Since the creation of extension object is driven by Java side, and each
  // Java extension is backed by a native extension object. However, the Java
  // object may be destroyed by Android lifecycle management without destroying
  // the native side object. We keep the reference to native extension object
  // to make sure we can reuse the native object if Java extension is re-created
  // on resuming.
  extensions_.push_back(extension.release());
}

XWalkExtension* XWalkBrowserMainPartsAndroid::LookupExtension(
    const std::string& name) {
  extensions::XWalkExtensionVector::const_iterator it = extensions_.begin();
  for (; it != extensions_.end(); ++it) {
    XWalkExtension* extension = *it;
    if (name == extension->name()) return extension;
  }

  return NULL;
}

}  // namespace xwalk
